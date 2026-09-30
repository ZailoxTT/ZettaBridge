#!/usr/bin/env python3
"""Validates filenames and ELF architecture of the generated launcher bundle."""

from __future__ import annotations

import pathlib
import struct
import sys


SYSROOT = {
    "system/bin/linker",
    "system/lib/ld-android.so",
    "system/lib/libc++.so",
    "system/lib/libc.so",
    "system/lib/libdl_android.so",
    "system/lib/libdl.so",
    "system/lib/liblog.so",
    "system/lib/libm.so",
    "system/lib/libstdc++.so",
    "system/lib/libz.so",
}
RUNTIME = {
    *(f"assets/zb/sysroot/{name}" for name in SYSROOT),
    "assets/zb/guest/zbhost",
    "assets/zb/guest/lib/libzbcompat.so",
    "assets/zb/guest/lib/libzbjni.so",
    "assets/zb/guest/lib/libGLESv2.so",
    "assets/zb/guest/lib/libGLESv1_CM.so",
    "assets/zb/guest/lib/libEGL.so",
    "assets/zb/guest/lib/libandroid.so",
    "assets/zb/guest/lib/libjnigraphics.so",
    "assets/zb/host/libzbproxy.so",
    "assets/zb-files.txt",
    "assets/zb-version.txt",
    "jniLibs/arm64-v8a/libzbridge.so",
}


def elf(path: pathlib.Path, elf_class: int, machine: int) -> None:
    data = path.read_bytes()[:20]
    if len(data) < 20 or data[:4] != b"\x7fELF":
        raise SystemExit(f"not an ELF file: {path}")
    if data[4] != elf_class or data[5] != 1:
        raise SystemExit(f"wrong ELF class/endianness: {path}")
    actual = struct.unpack_from("<H", data, 18)[0]
    if actual != machine:
        raise SystemExit(f"wrong ELF machine {actual}: {path}")


def main() -> None:
    if len(sys.argv) != 2:
        raise SystemExit("usage: check_launcher_bundle.py build/launcher")
    root = pathlib.Path(sys.argv[1])
    actual = {str(path.relative_to(root)) for path in root.rglob("*") if path.is_file()}
    if actual != RUNTIME:
        missing = sorted(RUNTIME - actual)
        extra = sorted(actual - RUNTIME)
        raise SystemExit(f"launcher bundle file mismatch; missing={missing}, extra={extra}")

    for relative in sorted(SYSROOT):
        elf(root / "assets/zb/sysroot" / relative, 1, 40)  # ELF32, EM_ARM
    for relative in [
        "assets/zb/guest/zbhost",
        "assets/zb/guest/lib/libzbcompat.so",
        "assets/zb/guest/lib/libzbjni.so",
        "assets/zb/guest/lib/libGLESv2.so",
        "assets/zb/guest/lib/libGLESv1_CM.so",
    "assets/zb/guest/lib/libGLESv1_CM.so",
        "assets/zb/guest/lib/libEGL.so",
        "assets/zb/guest/lib/libandroid.so",
        "assets/zb/guest/lib/libjnigraphics.so",
    ]:
        elf(root / relative, 1, 40)
    elf(root / "assets/zb/host/libzbproxy.so", 2, 183)  # ELF64, EM_AARCH64
    elf(root / "jniLibs/arm64-v8a/libzbridge.so", 2, 183)

    listed = (root / "assets/zb-files.txt").read_text(encoding="utf-8").splitlines()
    expected_list = sorted(path[len("assets/") :] for path in RUNTIME if path.startswith("assets/zb/"))
    if listed != expected_list:
        raise SystemExit("zb-files.txt does not match the runtime assets")
    if sum(path.stat().st_size for path in root.rglob("*") if path.is_file()) > 8 * 1024 * 1024:
        raise SystemExit("launcher bundle exceeds 8 MiB")
    print("launcher bundle PASS")


if __name__ == "__main__":
    main()
