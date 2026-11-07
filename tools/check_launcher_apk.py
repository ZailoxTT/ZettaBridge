#!/usr/bin/env python3
"""Audit an assembled ZettaBridge launcher APK for ARM32 GLES1 guest stub.

Usage: python3 tools/check_launcher_apk.py path/to/launcher.apk
"""
from __future__ import annotations
import pathlib
import struct
import sys
import zipfile

REQUIRED = {
    "assets/zb/guest/lib/libGLESv1_CM.so": (1, 40),
    "assets/zb/guest/lib/libGLESv2.so": (1, 40),
    "assets/zb/guest/lib/libEGL.so": (1, 40),
    "assets/zb/guest/lib/libzbjni.so": (1, 40),
    "lib/arm64-v8a/libzbridge.so": (2, 183),
}

def check(apk: pathlib.Path) -> None:
    with zipfile.ZipFile(apk) as z:
        files = set(z.namelist())
        for name, (elf_class, machine) in REQUIRED.items():
            if name not in files:
                raise ValueError(f"MISSING in APK: {name}")
            header = z.read(name)[:20]
            if len(header) < 20 or header[:4] != b"\x7fELF":
                raise ValueError(f"NOT ELF: {name}")
            if header[4] != elf_class or header[5] != 1 or struct.unpack_from("<H", header, 18)[0] != machine:
                raise ValueError(f"WRONG ABI: {name}")
        listing = "assets/zb-files.txt"
        if listing not in files:
            raise ValueError(f"MISSING in APK: {listing}")
        asset_names = set(z.read(listing).decode("utf-8").splitlines())
        if "zb/guest/lib/libGLESv1_CM.so" not in asset_names:
            raise ValueError("GLES1 absent from runtime extraction manifest zb-files.txt")
    print("APK GLES1 audit PASS — this verifies packaging, not game compatibility")

if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit("usage: check_launcher_apk.py <launcher.apk>")
    try:
        check(pathlib.Path(sys.argv[1]))
    except (OSError, ValueError, zipfile.BadZipFile, UnicodeDecodeError) as exc:
        raise SystemExit(str(exc)) from exc
