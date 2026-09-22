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
import zipfile

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

# Activities known to be NativeActivity or to descend from it.
NATIVE_ACTIVITIES = (
    "android.app.NativeActivity",
    "com.unity3d.player.UnityPlayerNativeActivity",
    "com.unity3d.player.UnityPlayerProxyActivity",
)

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
    native_activity = activity in NATIVE_ACTIVITIES

    if not abis:
        verdict = "no native code: runs as an ordinary app, nothing to translate"
    elif any(abi.startswith("arm64") for abi in abis):
        verdict = "has arm64: the phone runs it by itself"
    elif not any(abi.startswith("arm") for abi in abis):
        verdict = "no ARM code (%s): x86 guests are a later idea" % ",".join(abis)
    elif native_activity:
        verdict = "CANDIDATE, needs NativeActivity (part 2): %s" % activity
    else:
        verdict = "CANDIDATE, try it now: %s" % (activity or "activity unknown")

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
