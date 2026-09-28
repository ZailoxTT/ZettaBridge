#!/usr/bin/env python3
"""Step 05 transactional APK converter. Does not install or remove packages."""

import argparse
import copy
import hashlib
import json
import os
from pathlib import Path
import re
import secrets
import shutil
import stat
import subprocess
import sys
import tempfile
import zipfile
import struct

import apk_preflight as pre
from axml_inject import inject, bootstrap_processes, bootstrap_class, MAX_BOOTSTRAP_PROCESSES
import check_zbproxy

ROOT = Path(__file__).resolve().parents[1]
BOOTSTRAP_CLASS = b"Lcom/zettabridge/bootstrap/BootstrapProvider;"
BRIDGE_CLASS = b"Lcom/zettabridge/core/ZBridge;"
SIGNATURE_FILE = re.compile(r"^META-INF/(?:[^/]+\.(?:RSA|DSA|EC|SF|MF)|SIG-[^/]+)$", re.I)
DEX_NAME = re.compile(r"classes(?:([2-9][0-9]*))?\.dex$")
LIB_NAME = re.compile(r"lib[A-Za-z0-9_+.\-]+\.so$")


# This workaround is pinned to the exact NFS Most Wanted 1.3.128 ARM32
# library that reproduced the garage callback fault. Review changed binaries.
NFS_PACKAGE = "com.ea.games.nfs13_row"
NFS_LIB_NAME = "libapp.so"
NFS_LIB_SHA256 = "a73dcc170552866af52bf2d4f5822606307fb42aed1ef0df2e469fdfa374ff43"
NFS_GUARDED_LIB_SHA256 = "6aab56c18280e7b11634f6fa0edb12c09be67a35ff9066017781692f3de3801f"
NFS_CALLBACK_OFFSET = 0x473A3C
NFS_CALLBACK_EXPECTED = bytes.fromhex("f0 4b 2d e9")
NFS_GUARD_CAVE_OFFSET = 0x90D8D8
NFS_GUARD_CODE = bytes.fromhex(
    "00 00 50 e3"  # cmp r0, #0
    "1e ff 2f 01"  # bxeq lr
    "00 20 90 e5"  # ldr r2, [r0]
    "01 08 52 e3"  # cmp r2, #0x10000
    "1e ff 2f 31"  # bxlo lr
    "f0 4b 2d e9"  # push {r4-r9, r11, lr}
    "18 b0 8d e2"  # add r11, sp, #24
    "00 40 a0 e1"  # mov r4, r0
    "00 00 90 e5"  # ldr r0, [r0]
    "00 10 a0 e3"  # mov r1, #0
    "30 10 80 e5"  # str r1, [r0, #0x30]
    "00 00 00 00"  # replaced with a branch to the original continuation
)


def patch_known_guest(package, name, data):
    """Guard the observed callback only in its exact known game library."""
    if package != NFS_PACKAGE or name != NFS_LIB_NAME:
        return data, None

    input_sha256 = digest(data)
    if input_sha256 == NFS_GUARDED_LIB_SHA256:
        return data, {
            "id": "nfs13-garage-low-pointer-callback-guard-v1",
            "library": name,
            "input_sha256": input_sha256,
            "output_sha256": input_sha256,
            "status": "already_applied",
            "callback_offset": NFS_CALLBACK_OFFSET,
            "guard_offset": NFS_GUARD_CAVE_OFFSET,
        }
    if input_sha256 != NFS_LIB_SHA256:
        raise pre.Invalid(
            "NFS garage callback guard is pinned to libapp.so SHA-256 "
            f"{NFS_LIB_SHA256}; found {input_sha256}. Review the new game "
            "binary before converting it with this fix."
        )
    if data[NFS_CALLBACK_OFFSET:NFS_CALLBACK_OFFSET + 4] != NFS_CALLBACK_EXPECTED:
        raise pre.Invalid("NFS callback prologue does not match the known library")
    cave_end = NFS_GUARD_CAVE_OFFSET + len(NFS_GUARD_CODE)
    if cave_end > len(data) or any(data[NFS_GUARD_CAVE_OFFSET:cave_end]):
        raise pre.Invalid("NFS callback guard code cave is not empty in the known library")

    # ARM B uses a signed PC-relative immediate; PC is instruction address + 8.
    return_offset = NFS_GUARD_CAVE_OFFSET + len(NFS_GUARD_CODE) - 4
    return_target = NFS_CALLBACK_OFFSET + 0x18
    branch_delta = return_target - (return_offset + 8)
    if branch_delta % 4:
        raise pre.Invalid("NFS callback continuation is not ARM-aligned")
    guard_branch = 0xEA000000 | ((branch_delta // 4) & 0x00FFFFFF)
    guard = NFS_GUARD_CODE[:-4] + struct.pack("<I", guard_branch)

    branch_delta = NFS_GUARD_CAVE_OFFSET - (NFS_CALLBACK_OFFSET + 8)
    if branch_delta % 4:
        raise pre.Invalid("NFS callback trampoline is not ARM-aligned")
    trampoline_branch = 0xEA000000 | ((branch_delta // 4) & 0x00FFFFFF)
    patched = bytearray(data)
    patched[NFS_CALLBACK_OFFSET:NFS_CALLBACK_OFFSET + 4] = struct.pack("<I", trampoline_branch)
    patched[NFS_GUARD_CAVE_OFFSET:cave_end] = guard
    patched = bytes(patched)
    output_sha256 = digest(patched)
    if output_sha256 != NFS_GUARDED_LIB_SHA256:
        raise pre.Invalid("NFS callback guard bytes failed their pinned output hash")
    return patched, {
        "id": "nfs13-garage-low-pointer-callback-guard-v1",
        "library": name,
        "input_sha256": input_sha256,
        "output_sha256": output_sha256,
        "status": "applied",
        "callback_offset": NFS_CALLBACK_OFFSET,
        "guard_offset": NFS_GUARD_CAVE_OFFSET,
        "invalid_object_threshold": 65536,
    }


def digest(data):
    return hashlib.sha256(data).hexdigest()


def file_hash(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def run(command, *, env=None, timeout=120):
    try:
        result = subprocess.run(command, capture_output=True, text=True, timeout=timeout, env=env)
    except (OSError, subprocess.TimeoutExpired) as error:
        raise pre.Invalid(f"tool failed: {Path(command[0]).name}: {error}") from error
    if result.returncode:
        raise pre.Invalid(f"{Path(command[0]).name} failed: {(result.stderr or result.stdout).strip()[:500]}")
    return result.stdout


def init_key(key_dir, keytool):
    if key_dir.is_symlink():
        raise pre.Invalid("key directory must not be a symlink")
    if key_dir.exists() and any(key_dir.iterdir()):
        raise pre.Invalid("key directory already contains files; import or use the existing key")
    key_dir.mkdir(mode=0o700, parents=True, exist_ok=True)
    os.chmod(key_dir, 0o700)
    password = secrets.token_urlsafe(48)
    secret = key_dir / "password"
    key = key_dir / "signing.p12"
    try:
        with secret.open("x", encoding="ascii") as out:
            out.write(password + "\n")
        os.chmod(secret, 0o600)
        env = os.environ.copy()
        env["ZB_KEY_PASS"] = password
        run([keytool, "-genkeypair", "-keystore", str(key), "-storetype", "PKCS12",
             "-alias", "zettabridge", "-keyalg", "RSA", "-keysize", "3072",
             "-validity", "10000", "-dname", "CN=ZettaBridge Personal Signing",
             "-storepass:env", "ZB_KEY_PASS", "-keypass:env", "ZB_KEY_PASS"],
            env=env, timeout=120)
        os.chmod(key, 0o600)
    except Exception:
        key.unlink(missing_ok=True)
        secret.unlink(missing_ok=True)
        raise
    return key


def key_env(key_dir):
    if key_dir.is_symlink():
        raise pre.Invalid("key directory must not be a symlink")
    key = key_dir / "signing.p12"
    secret = key_dir / "password"
    if not key.is_file() or not secret.is_file() or key.is_symlink() or secret.is_symlink():
        raise pre.Invalid("personal signing key missing; run init-key or import signing.p12 and password")
    if os.name != "nt" and (stat.S_IMODE(key.stat().st_mode) & 0o077 or
                             stat.S_IMODE(secret.stat().st_mode) & 0o077):
        raise pre.Invalid("signing key/password must be owner-only (chmod 600)")
    password = secret.read_text(encoding="ascii").strip()
    if len(password) < 16:
        raise pre.Invalid("signing password is too short")
    env = os.environ.copy()
    env["ZB_KEY_PASS"] = password
    return key, env


def stage_inputs(inputs, work):
    if len(inputs) == 1 and inputs[0].suffix.lower() == ".apks":
        z, infos = pre.guarded_zip(inputs[0])
        try:
            members = sorted(n for n in infos if n.endswith(".apk"))
            if not members or len(members) > 128:
                raise pre.Invalid("container has no APKs or too many APKs")
            staged = []
            for index, member in enumerate(members):
                target = work / f"input-{index}.apk"
                with z.open(member) as src, target.open("wb") as dst:
                    shutil.copyfileobj(src, dst, 65536)
                staged.append((target, str(inputs[0]) + "!" + member))
            return staged
        finally:
            z.close()
    if not inputs or len(inputs) > 128 or any(p.suffix.lower() != ".apk" for p in inputs):
        raise pre.Invalid("input must be APK(s) or one .apks container")
    if sum(p.stat().st_size for p in inputs) > pre.MAX_ARCHIVE:
        raise pre.Invalid("oversized input set")
    staged = []
    for index, path in enumerate(inputs):
        if path.is_symlink():
            raise pre.Invalid("symlink input is not allowed")
        target = work / f"input-{index}.apk"
        shutil.copyfile(path, target)
        staged.append((target, str(path)))
    return staged


def runtime_files(runtime_dir, bootstrap_apk, proxy_path, zbridge_path, readelf):
    if not runtime_dir.is_dir() or runtime_dir.is_symlink():
        raise pre.Invalid("runtime directory missing or symlinked")
    result = {}
    count = 0
    total = 0
    for path in sorted(runtime_dir.rglob("*")):
        if path.is_symlink():
            raise pre.Invalid("symlink runtime asset")
        if not path.is_file():
            continue
        rel = path.relative_to(runtime_dir).as_posix()
        pre.check_name("zb/" + rel)
        if rel.startswith("app/") or rel.startswith("host/"):
            continue
        if path.stat().st_size > pre.MAX_ENTRY:
            raise pre.Invalid("oversized runtime asset")
        count += 1
        total += path.stat().st_size
        if count > pre.MAX_ENTRIES or total > pre.MAX_TOTAL:
            raise pre.Invalid("runtime asset set exceeds limits")
        result["assets/zb/" + rel] = path.read_bytes()
    for needed in ("assets/zb/guest/zbhost", "assets/zb/guest/lib/libzbjni.so",
                   "assets/zb/sysroot/system/bin/linker"):
        if needed not in result:
            raise pre.Invalid(f"runtime asset missing: {needed}")
    for path, label in ((proxy_path, "proxy"), (zbridge_path, "bridge")):
        if not path.is_file() or path.is_symlink() or path.stat().st_size > pre.MAX_LIB:
            raise pre.Invalid(f"{label} library missing, symlinked or oversized")
        info = pre.elf_report(path.read_bytes(), readelf)
        if (info["class"], info["machine"]) != (64, 183):
            raise pre.Invalid(f"{label} library is not ARM64")
    require_installed_entry(zbridge_path, readelf)
    try:
        _, _, errors = check_zbproxy.check(proxy_path.read_bytes())
    except check_zbproxy.CheckError as error:
        raise pre.Invalid(f"invalid proxy: {error}") from error
    if errors:
        raise pre.Invalid("invalid proxy: " + "; ".join(errors))
    z, infos = pre.guarded_zip(bootstrap_apk, max_total=32 * 1024 * 1024)
    try:
        dex_names = sorted((name for name in infos if DEX_NAME.fullmatch(name)),
                           key=lambda name: int(DEX_NAME.fullmatch(name).group(1) or 1))
        dex = [pre.read_small(z, name, 16 * 1024 * 1024) for name in dex_names]
        combined = b"".join(dex)
        required = [BRIDGE_CLASS] + [("L" + bootstrap_class(i).replace(".", "/") + ";").encode()
                                     for i in range(MAX_BOOTSTRAP_PROCESSES)]
        if not dex or any(cls not in combined for cls in required):
            raise pre.Invalid("bootstrap APK lacks required classes")
    finally:
        z.close()
    return result, dex


def require_installed_entry(zbridge_path, readelf):
    """Reject an old ARM64 bridge that would crash the injected provider at startup."""
    symbol = "Java_com_zettabridge_core_ZBridge_activateInstalled"
    dynamic = run([readelf, "--dyn-syms", "--wide", str(zbridge_path)])
    if not any((fields := line.split()) and len(fields) >= 8
               and fields[4] == "GLOBAL" and fields[5] == "DEFAULT"
               and fields[-1] == symbol for line in dynamic.splitlines()):
        raise pre.Invalid("bridge lacks exported installed bootstrap entry: " + symbol)


def source_layout(report, staged):
    files = report["files"]
    if report["status"] != "analyzed":
        raise pre.Invalid("preflight rejects input: " + "; ".join(
            f["kind"] for f in report["findings"] if f["level"] == "unsupported"))
    if len(files) != len(staged):
        raise pre.Invalid("preflight file count mismatch")
    bases = [i for i, f in enumerate(files) if not f["manifest"]["split"]]
    if len(bases) != 1:
        raise pre.Invalid("no unique base APK")
    chosen = {}
    x86_names = set()
    for i, f in enumerate(files):
        for lib in f["libraries"]:
            name = Path(lib["path"]).name
            if not LIB_NAME.fullmatch(name):
                raise pre.Invalid("unsupported native library name")
            if lib["abi"] == "x86":
                x86_names.add(name)
                continue
            if lib["abi"] not in ("armeabi", "armeabi-v7a"):
                raise pre.Invalid("converter accepts ARM32 libraries and matching x86 variants only")
            if name == "libzbridge.so":
                raise pre.Invalid("guest library collides with ARM64 bridge name")
            rank = 1 if lib["abi"] == "armeabi-v7a" else 0
            entry = chosen.get(name)
            if entry and entry[0] == rank and (entry[1], entry[2]["path"]) != (i, lib["path"]):
                raise pre.Invalid(f"duplicate guest library at same ABI: {name}")
            if entry is None or rank > entry[0]:
                chosen[name] = (rank, i, lib)
    unmatched = sorted(x86_names - chosen.keys())
    if unmatched:
        raise pre.Invalid("x86 library has no ARM32 counterpart: " + ", ".join(unmatched))
    return bases[0], chosen


def add(zout, name, data, compress=zipfile.ZIP_DEFLATED):
    info = zipfile.ZipInfo(name)
    info.compress_type = compress
    info.external_attr = 0o100644 << 16
    zout.writestr(info, data)


def transform_one(src, dest, *, base, package, guest, selected_paths, runtime, dex, proxy, bridge):
    zin, infos = pre.guarded_zip(src)
    changes = []
    try:
        dex_numbers = [int(match.group(1) or 1) for name in infos
                       if (match := DEX_NAME.fullmatch(name))]
        if base and (not dex_numbers or len(dex_numbers) != len(set(dex_numbers))):
            raise pre.Invalid("base APK needs unambiguous DEX files")
        if base:
            for name in infos:
                if DEX_NAME.fullmatch(name):
                    data = zin.read(name)
                    if BRIDGE_CLASS in data or any(
                            ("L" + bootstrap_class(i).replace(".", "/") + ";").encode() in data
                            for i in range(MAX_BOOTSTRAP_PROCESSES)):
                        raise pre.Invalid("source DEX conflicts with bridge bootstrap classes")
        with zipfile.ZipFile(dest, "w", allowZip64=False) as zout:
            for name, info in infos.items():
                if info.is_dir() or SIGNATURE_FILE.match(name) or name.startswith("lib/") and name.endswith(".so"):
                    if name.startswith("lib/") and name.endswith(".so"):
                        if name in selected_paths:
                            changes.append({"kind": "move_guest_library", "from": name,
                                            "to": "assets/zb/app/lib/" + Path(name).name})
                        else:
                            changes.append({"kind": "remove_unselected_abi_variant", "path": name})
                    continue
                if base and (name.startswith("assets/zb/") or name.startswith("lib/arm64-v8a/") or
                             name in ("assets/zb-files.txt", "assets/zb-version.txt")):
                    raise pre.Invalid(f"source collides with generated content: {name}")
                data = zin.read(name)
                if base and name == "AndroidManifest.xml":
                    data, edits = inject(data, package)
                    changes.extend(edits)
                copied = copy.copy(info)
                copied.extra = b""
                copied.comment = b""
                zout.writestr(copied, data)
            if base:
                for offset, payload in enumerate(dex, 1):
                    new_dex = "classes%d.dex" % (max(dex_numbers) + offset)
                    add(zout, new_dex, payload)
                    changes.append({"kind": "add_bootstrap_dex", "path": new_dex,
                                    "sha256": digest(payload)})
                for name, data in sorted(runtime.items()):
                    add(zout, name, data)
                for name, data in sorted(guest.items()):
                    add(zout, "assets/zb/app/lib/" + name, data)
                names = sorted(x.removeprefix("assets/") for x in runtime)
                names += sorted("zb/app/lib/" + x for x in guest)
                version = digest(b"".join(hashlib.sha256(
                    (name + "\n").encode() + (runtime["assets/" + name]
                    if "assets/" + name in runtime else guest[name.split("/")[-1]])
                ).digest() for name in names))
                add(zout, "assets/zb-files.txt", ("\n".join(names) + "\n").encode("ascii"))
                add(zout, "assets/zb-version.txt", (version + "\n").encode("ascii"))
                add(zout, "lib/arm64-v8a/libzbridge.so", bridge)
                for name in sorted(guest):
                    add(zout, "lib/arm64-v8a/" + name, proxy)
                    changes.append({"kind": "add_arm64_proxy", "path": "lib/arm64-v8a/" + name,
                                    "guest": "assets/zb/app/lib/" + name})
    finally:
        zin.close()
    return changes


def verify_output(path, source_file, *, base, guest, proxy, bridge,
                  apksigner, readelf, signer_fp):
    result = pre.apk(path, apksigner, readelf)
    original = source_file["manifest"]
    m = result["manifest"]
    expected_manifest = copy.deepcopy(original)
    if base:
        expected_manifest["extract_native_libs"] = "true"
        with zipfile.ZipFile(source_file["file"]) as source:
            processes = bootstrap_processes(pre.parse_manifest(source.read("AndroidManifest.xml")))
        for i, process in enumerate(processes):
            expected_manifest["components"].insert(i, {
                "type": "provider", "name": bootstrap_class(i),
                "permission": None, "process": process,
                "authorities": original["package"] + ".zettabridge.bootstrap" +
                               ("." + str(i) if i else "")})
        expected_manifest["bootstrap_authority_collisions"] = [
            original["package"] + ".zettabridge.bootstrap" + ("." + str(i) if i else "")
            for i in range(len(processes))]
    if m != expected_manifest:
        raise pre.Invalid("output manifest differs outside recorded bootstrap edits")
    with zipfile.ZipFile(source_file["file"]) as source, zipfile.ZipFile(path) as output:
        original_tree = pre.parse_manifest(source.read("AndroidManifest.xml"))
        output_tree = pre.parse_manifest(output.read("AndroidManifest.xml"))
    if base:
        original_app = original_tree.find("application")
        output_app = output_tree.find("application")
        if original_app is None or output_app is None:
            raise pre.Invalid("output manifest lacks application")
        output_app.attrib.pop(pre.ANDROID + "extractNativeLibs", None)
        original_app.attrib.pop(pre.ANDROID + "extractNativeLibs", None)
        for provider in list(output_app):
            if provider.tag == "provider" and pre.attr(provider, "name") in {
                    bootstrap_class(i) for i in range(len(processes))}:
                output_app.remove(provider)
    if _manifest_tree(original_tree) != _manifest_tree(output_tree):
        raise pre.Invalid("output changed source manifest attributes, components or children")
    if result["signer"]["cert_sha256"] != [signer_fp]:
        raise pre.Invalid("output signer does not match personal key")
    libs = {x["path"]: x for x in result["libraries"]}
    expected = ({"lib/arm64-v8a/libzbridge.so"} |
                {"lib/arm64-v8a/" + n for n in guest}) if base else set()
    if set(libs) != expected or any((x["class"], x["machine"]) != (64, 183) for x in libs.values()):
        raise pre.Invalid("output contains unexpected or non-ARM64 host libraries")
    zin, infos = pre.guarded_zip(path)
    try:
        if base:
            expected_guest = {"assets/zb/app/lib/" + n for n in guest}
            if not expected_guest.issubset(infos):
                raise pre.Invalid("output missing guest libraries")
            if "assets/zb-files.txt" not in infos or "assets/zb-version.txt" not in infos:
                raise pre.Invalid("output missing bootstrap inventory")
            if zin.read("lib/arm64-v8a/libzbridge.so") != bridge:
                raise pre.Invalid("output bridge differs from validated build")
            for name, data in guest.items():
                if (zin.read("assets/zb/app/lib/" + name) != data or
                        zin.read("lib/arm64-v8a/" + name) != proxy):
                    raise pre.Invalid("output guest/proxy differs from validated build")
        source = Path(source_file["file"])
        if source.is_file():
            zsource, source_infos = pre.guarded_zip(source)
            try:
                for name, info in source_infos.items():
                    if (info.is_dir() or SIGNATURE_FILE.match(name) or
                            name.startswith("lib/") and name.endswith(".so") or
                            base and name == "AndroidManifest.xml"):
                        continue
                    if name not in infos or zin.read(name) != zsource.read(name):
                        raise pre.Invalid(f"output changed preserved {name}")
            finally:
                zsource.close()
    finally:
        zin.close()
    return result


def _manifest_tree(element):
    """Compare complete parsed manifests, including component filters and metadata."""
    return (element.tag, tuple(sorted(element.attrib.items())),
            tuple(_manifest_tree(child) for child in element))


def convert(args):
    output = args.output_dir.resolve()
    if output.exists() or output.is_symlink():
        raise pre.Invalid("output directory already exists")
    output.parent.mkdir(parents=True, exist_ok=True)
    key, env = key_env(args.key_dir)
    input_hashes = {str(p): file_hash(p) for p in args.inputs}
    stage = Path(tempfile.mkdtemp(prefix=".zb-convert-", dir=output.parent))
    os.chmod(stage, 0o700)
    published = False
    try:
        work = stage / "work"
        work.mkdir(mode=0o700)
        staged = stage_inputs(args.inputs, work)
        report = pre.analyze([x[0] for x in staged], args.apksigner, args.readelf)
        base_idx, chosen = source_layout(report, staged)
        if not chosen:
            raise pre.Invalid("Step 05 requires at least one ARM32 guest library")
        runtime, dex = runtime_files(args.runtime_dir, args.bootstrap_apk, args.proxy,
                                     args.zbridge, args.readelf)
        guest = {}
        mapping = []
        guest_patches = []
        for name, (rank, i, lib) in sorted(chosen.items()):
            with zipfile.ZipFile(staged[i][0]) as source:
                data = source.read(lib["path"])
            source_sha256 = digest(data)
            data, patch = patch_known_guest(report["package"], name, data)
            guest[name] = data
            mapping.append({"guest": name, "source": staged[i][1], "entry": lib["path"],
                            "sha256": source_sha256, "runtime_sha256": digest(data),
                            "abi": lib["abi"]})
            if patch:
                guest_patches.append(patch)
        if report["package"] == NFS_PACKAGE and not any(
                item["guest"] == NFS_LIB_NAME for item in mapping):
            raise pre.Invalid("NFS Most Wanted input has no selected ARM32 libapp.so")
        if sum(map(len, runtime.values())) + sum(map(len, guest.values())) > pre.MAX_TOTAL:
            raise pre.Invalid("runtime plus guest assets exceed 512 MiB")
        output_paths = []
        for i, ((src, label), source_file) in enumerate(zip(staged, report["files"])):
            filename = "base.apk" if i == base_idx else f"split-{i}.apk"
            unsigned = work / f"unsigned-{i}.apk"
            aligned = work / f"aligned-{i}.apk"
            signed = stage / filename
            edits = transform_one(src, unsigned, base=i == base_idx, package=report["package"],
                                  guest=guest,
                                  selected_paths={lib["path"] for _, source_i, lib in chosen.values()
                                                  if source_i == i}, runtime=runtime, dex=dex,
                                  proxy=args.proxy.read_bytes(), bridge=args.zbridge.read_bytes())
            if i == base_idx:
                edits.extend({"kind": "patch_guest_library", **patch}
                             for patch in guest_patches)
            run([args.zipalign, "-f", "4", str(unsigned), str(aligned)])
            run([args.apksigner, "sign", "--ks", str(key), "--ks-type", "PKCS12",
                 "--ks-key-alias", "zettabridge", "--ks-pass", "env:ZB_KEY_PASS",
                 "--out", str(signed), str(aligned)], env=env)
            run([args.zipalign, "-c", "4", str(signed)])
            signed_info = pre.signer(signed, args.apksigner)
            if len(signed_info["cert_sha256"]) != 1:
                raise pre.Invalid("output has multiple signing certificates")
            output_paths.append((signed, source_file, label, edits))
        signer_fp = pre.signer(output_paths[0][0], args.apksigner)["cert_sha256"][0]
        final = []
        for i, (path, source_file, label, edits) in enumerate(output_paths):
            result = verify_output(path, source_file, base=i == base_idx,
                                   guest=guest, proxy=args.proxy.read_bytes(),
                                   bridge=args.zbridge.read_bytes(),
                                   apksigner=args.apksigner, readelf=args.readelf,
                                   signer_fp=signer_fp)
            final.append({"input": label, "input_sha256": file_hash(staged[i][0]),
                          "output": path.name, "output_sha256": result["sha256"],
                          "split": source_file["manifest"]["split"], "changes": edits})
        if any(file_hash(p) != original for p, original in ((Path(k), v) for k, v in input_hashes.items())):
            raise pre.Invalid("input changed during conversion")
        record = {"schema": 1, "package": report["package"], "version_code": report["version_code"],
                  "source_signer_sha256": report["files"][base_idx]["signer"]["cert_sha256"],
                  "output_signer_sha256": signer_fp, "guest_mapping": mapping,
                  "guest_patches": guest_patches,
                  "runtime_assets_sha256": {name: digest(data) for name, data in sorted(runtime.items())},
                  "bootstrap_apk_sha256": file_hash(args.bootstrap_apk),
                  "proxy_sha256": file_hash(args.proxy), "zbridge_sha256": file_hash(args.zbridge),
                  "files": final,
                  "manifest_changes": [x for x in final[base_idx]["changes"]
                                       if x["kind"] in ("application_attribute", "provider")],
                  "resource_changes": [],
                  "preflight_findings": report["findings"]}
        (stage / "transformation.json").write_text(json.dumps(record, indent=2, sort_keys=True) + "\n",
                                                    encoding="utf-8")
        shutil.rmtree(work)
        os.replace(stage, output)
        published = True
        return record
    finally:
        if not published:
            shutil.rmtree(stage, ignore_errors=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("init-key", "convert"))
    parser.add_argument("inputs", nargs="*", type=Path)
    parser.add_argument("--output-dir", type=Path)
    parser.add_argument("--key-dir", type=Path, default=Path.home() / ".local/share/zettabridge/signing")
    parser.add_argument("--runtime-dir", type=Path, default=ROOT / "build/launcher/assets/zb")
    parser.add_argument("--bootstrap-apk", type=Path,
                        default=ROOT / "android/launcher/step05bootstrap/build/outputs/apk/debug/step05bootstrap-debug.apk")
    parser.add_argument("--proxy", type=Path, default=ROOT / "build/launcher/assets/zb/host/libzbproxy.so")
    parser.add_argument("--zbridge", type=Path, default=ROOT / "build/launcher/jniLibs/arm64-v8a/libzbridge.so")
    parser.add_argument("--apksigner", default=os.environ.get("APKSIGNER", "apksigner"))
    parser.add_argument("--zipalign", default=os.environ.get("ZIPALIGN", "zipalign"))
    parser.add_argument("--readelf", default=os.environ.get("READELF", "readelf"))
    parser.add_argument("--keytool", default="keytool")
    args = parser.parse_args()
    try:
        if args.command == "init-key":
            if args.inputs or args.output_dir:
                raise pre.Invalid("init-key takes no APKs or output directory")
            key = init_key(args.key_dir, args.keytool)
            print(json.dumps({"status": "key_created", "key": str(key)}))
        else:
            if not args.inputs or args.output_dir is None:
                raise pre.Invalid("convert needs input APK(s) and --output-dir")
            record = convert(args)
            print(json.dumps({"status": "converted", "output": str(args.output_dir),
                              "package": record["package"],
                              "signer_sha256": record["output_signer_sha256"]}, sort_keys=True))
        return 0
    except (pre.Invalid, OSError, KeyError, ValueError, zipfile.BadZipFile) as error:
        print(json.dumps({"status": "failed", "error": str(error)}), file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
