#!/usr/bin/env python3
"""Says whether an APK is a candidate for ZettaBridge, and which entry point it uses.

    tools/scan_apk.py game.apk [more.apk ...]
    tools/scan_apk.py ~/apks            # a directory is scanned recursively

The two questions that decide everything:
  1. Does it carry 32-bit code and no 64-bit code? A build with arm64-v8a runs natively and is
     not our problem; one with no native code at all is plain Java and also not our problem.
  2. Does its launcher activity descend from NativeActivity? Those need the NativeActivity work
     (part 2); everything else goes through an ordinary Java activity and can be tried today.
"""
import os
import re
import subprocess
import sys
import struct
import zipfile

struct_error = struct.error

def find_aapt():
    """The system aapt first: the SDK copies are x86 binaries that do not run on this machine."""
    from shutil import which
    found = which("aapt")
    if found:
        return found
    import glob
    for candidate in sorted(glob.glob(os.path.expanduser("~/android-sdk/build-tools/*/aapt")), reverse=True):
        if os.access(candidate, os.X_OK):
            return candidate
    return "aapt"


AAPT = find_aapt()

NATIVE_ACTIVITY = "Landroid/app/NativeActivity;"


def read_uleb128(data, offset):
    result = 0
    shift = 0
    while True:
        byte = data[offset]
        offset += 1
        result |= (byte & 0x7F) << shift
        if byte < 0x80:
            return result, offset
        shift += 7


def dex_superclasses(data):
    """{class descriptor: superclass descriptor} of one classes.dex.

    The names in the manifest are not enough to tell a NativeActivity apart: Unity 4 ships a
    UnityPlayerActivity that extends UnityPlayerNativeActivity, which extends NativeActivity, and
    plugins add activities of their own on top. Only the chain in the dex answers it.
    """
    if data[:4] != b"dex\n":
        return {}
    import struct
    (string_ids_size, string_ids_off, type_ids_size, type_ids_off) = struct.unpack_from("<4I", data, 56)
    class_defs_size, class_defs_off = struct.unpack_from("<2I", data, 96)

    string_offsets = struct.unpack_from("<%dI" % string_ids_size, data, string_ids_off)
    strings = []
    for offset in string_offsets:
        length, start = read_uleb128(data, offset)
        end = data.index(b"\0", start)
        strings.append(data[start:end].decode("utf-8", "replace"))
    type_strings = struct.unpack_from("<%dI" % type_ids_size, data, type_ids_off)

    supers = {}
    for i in range(class_defs_size):
        class_idx, _flags, super_idx = struct.unpack_from("<3I", data, class_defs_off + i * 32)
        if super_idx == 0xFFFFFFFF:
            continue
        supers[strings[type_strings[class_idx]]] = strings[type_strings[super_idx]]
    return supers


def descends_from_native_activity(apk, names, activity):
    """(verdict, chain): whether the launcher activity is a NativeActivity, and how it got there."""
    if not activity:
        return None, []
    supers = {}
    for name in names:
        if name.startswith("classes") and name.endswith(".dex"):
            try:
                supers.update(dex_superclasses(apk.read(name)))
            except (KeyError, ValueError, IndexError, struct_error):
                continue
    current = "L%s;" % activity.replace(".", "/")
    chain = []
    seen = set()
    while current and current not in seen:
        seen.add(current)
        chain.append(current[1:-1].replace("/", "."))
        if current == NATIVE_ACTIVITY:
            return True, chain
        current = supers.get(current)
    # The chain ended in a framework class the APK does not carry: not a NativeActivity.
    return False, chain

ENGINE_MARKERS = (
    ("libil2cpp.so", "Unity (IL2CPP)"),
    ("libmono.so", "Unity (Mono)"),
    ("libunity.so", "Unity"),
    ("libflutter.so", "Flutter"),
    ("liblime.so", "OpenFL/lime"),
    ("libandengine.so", "AndEngine"),
    ("libcocos2d", "cocos2d-x"),
    ("libgodot", "Godot"),
    ("libUE4", "Unreal"),
    ("libmain.so", "native (unknown engine)"),
)


def badging(path):
    try:
        out = subprocess.run([AAPT, "dump", "badging", path], capture_output=True, text=True, check=False).stdout
    except OSError:
        return {}
    info = {}
    for key, pattern in (("package", r"package: name='([^']+)'"),
                         ("label", r"application-label:'([^']*)'"),
                         ("sdk", r"sdkVersion:'(\d+)'"),
                         ("target", r"targetSdkVersion:'(\d+)'"),
                         ("activity", r"launchable-activity: name='([^']+)'")):
        m = re.search(pattern, out)
        if m:
            info[key] = m.group(1)
    return info


def scan(path):
    try:
        with zipfile.ZipFile(path) as apk:
            names = apk.namelist()
    except (zipfile.BadZipFile, OSError) as e:
        return "%s: not readable (%s)" % (path, e)

    abis = sorted({n.split("/")[1] for n in names if n.startswith("lib/") and n.count("/") >= 2})
    libs = [n.rsplit("/", 1)[-1] for n in names if n.startswith("lib/") and n.endswith(".so")]
    engine = next((label for marker, label in ENGINE_MARKERS if any(marker in lib for lib in libs)), "")

    info = badging(path)
    activity = info.get("activity", "")
    with zipfile.ZipFile(path) as apk:
        native_activity, chain = descends_from_native_activity(apk, names, activity)

    if not abis:
        verdict = "no native code: runs as an ordinary app, nothing to translate"
    elif any(abi.startswith("arm64") for abi in abis):
        verdict = "has arm64: the phone runs it by itself"
    elif not any(abi.startswith("arm") for abi in abis):
        verdict = "no ARM code (%s): x86 guests are a later idea" % ",".join(abis)
    elif native_activity:
        verdict = "CANDIDATE, needs NativeActivity (part 2)\n  chain %s" % " -> ".join(chain)
    elif native_activity is None:
        verdict = "CANDIDATE, but the launcher activity is unknown: check the manifest by hand"
    else:
        verdict = "CANDIDATE, try it now: %s" % activity

    return "%s\n  package %s  label %s  minSdk %s  targetSdk %s\n  abis %s  libs %d  engine %s\n  %s" % (
        os.path.basename(path), info.get("package", "?"), info.get("label", "?"), info.get("sdk", "?"),
        info.get("target", "?"), ",".join(abis) or "none", len(libs), engine or "unknown", verdict)


def main(arguments):
    if not arguments:
        sys.exit(__doc__)
    paths = []
    for argument in arguments:
        if os.path.isdir(argument):
            for root, _, files in os.walk(argument):
                paths += [os.path.join(root, f) for f in sorted(files) if f.endswith(".apk")]
        else:
            paths.append(argument)
    for path in paths:
        print(scan(path))
        print()


if __name__ == "__main__":
    main(sys.argv[1:])
