#!/usr/bin/env python3
"""Structural check of the arm64 Android proxy library libzbproxy.so.

The proxy is copied once per arm32 guest library and loaded by ART, so it must stay a
tiny standalone library:
  - ELF64 little-endian AArch64 shared object;
  - the only exported defined dynamic symbols are the functions JNI_OnLoad and
    ANativeActivity_onCreate, the two entry points Android looks up in a plugin library;
  - DT_NEEDED names only allowed system libraries (never libzbridge.so or libc++_shared.so);
  - the ZBridge class, method name and descriptor strings it calls are present.

Usage: check_zbproxy.py [--skip-missing] <libzbproxy.so>
Exit status: 0 ok, 1 check failed, 2 usage, 77 file missing with --skip-missing.
"""

import struct
import sys

ALLOWED_NEEDED = {"libc.so", "libdl.so", "liblog.so"}
REQUIRED_STRINGS = [b"com/zettabridge/core/ZBridge", b"onProxyLoaded", b"(Ljava/lang/String;)I"]

EM_AARCH64 = 183
ET_DYN = 3
SHT_DYNSYM = 11
SHT_DYNAMIC = 6
DT_NULL = 0
DT_NEEDED = 1
DT_TEXTREL = 22
SHN_UNDEF = 0
STB_GLOBAL = 1
STB_WEAK = 2
STB_GNU_UNIQUE = 10
STT_FUNC = 2
STV_DEFAULT = 0
STV_PROTECTED = 3


class CheckError(Exception):
    pass


def c_string(blob, offset):
    if offset >= len(blob):
        raise CheckError("string offset 0x%x outside the string table" % offset)
    end = blob.find(b"\0", offset)
    if end < 0:
        raise CheckError("unterminated string at 0x%x" % offset)
    return blob[offset:end].decode("ascii", "replace")


def section_bytes(data, header):
    offset, size = header["offset"], header["size"]
    if offset + size > len(data):
        raise CheckError("section outside the file")
    return data[offset:offset + size]


def check(data):
    errors = []
    if len(data) < 64 or data[:4] != b"\x7fELF":
        raise CheckError("not an ELF file")
    if data[4] != 2 or data[5] != 1:
        raise CheckError("not ELF64 little-endian")
    (e_type, e_machine) = struct.unpack_from("<HH", data, 16)
    if e_machine != EM_AARCH64:
        errors.append("machine %d is not AArch64" % e_machine)
    if e_type != ET_DYN:
        errors.append("type %d is not a shared object" % e_type)
    (e_shoff,) = struct.unpack_from("<Q", data, 40)
    (e_shentsize, e_shnum) = struct.unpack_from("<HH", data, 58)
    if e_shoff == 0 or e_shnum == 0 or e_shentsize < 64 or e_shoff + e_shnum * e_shentsize > len(data):
        raise CheckError("missing or truncated section headers")

    sections = []
    for i in range(e_shnum):
        (name, sh_type, _flags, _addr, offset, size, link, _info, _align, entsize) = struct.unpack_from(
            "<IIQQQQIIQQ", data, e_shoff + i * e_shentsize)
        sections.append({"type": sh_type, "offset": offset, "size": size, "link": link, "entsize": entsize})

    def linked_strings(header):
        if header["link"] >= len(sections):
            raise CheckError("bad sh_link")
        return section_bytes(data, sections[header["link"]])

    dynamic = [s for s in sections if s["type"] == SHT_DYNAMIC]
    dynsym = [s for s in sections if s["type"] == SHT_DYNSYM]
    if len(dynamic) != 1 or len(dynsym) != 1:
        raise CheckError("expected one .dynamic and one .dynsym section")

    needed = []
    dyn_blob = section_bytes(data, dynamic[0])
    dyn_strings = linked_strings(dynamic[0])
    for pos in range(0, len(dyn_blob) - 15, 16):
        (tag, value) = struct.unpack_from("<qQ", dyn_blob, pos)
        if tag == DT_NULL:
            break
        if tag == DT_NEEDED:
            needed.append(c_string(dyn_strings, value))
        if tag == DT_TEXTREL:
            errors.append("has DT_TEXTREL")
    for name in needed:
        if name not in ALLOWED_NEEDED:
            errors.append("DT_NEEDED %s is not allowed" % name)

    exports = {}
    sym_blob = section_bytes(data, dynsym[0])
    sym_strings = linked_strings(dynsym[0])
    for pos in range(24, len(sym_blob) - 23, 24):
        (st_name, st_info, st_other, st_shndx, _value, _size) = struct.unpack_from("<IBBHQQ", sym_blob, pos)
        bind, sym_type, visibility = st_info >> 4, st_info & 0xF, st_other & 0x3
        if st_shndx == SHN_UNDEF or bind not in (STB_GLOBAL, STB_WEAK, STB_GNU_UNIQUE):
            continue
        if visibility not in (STV_DEFAULT, STV_PROTECTED):
            continue
        exports[c_string(sym_strings, st_name)] = sym_type
    expected = {"JNI_OnLoad", "ANativeActivity_onCreate"}
    if set(exports) != expected:
        errors.append("exports must be exactly %s, got: %s" %
                      (", ".join(sorted(expected)), ", ".join(sorted(exports))))
    else:
        for name in sorted(expected):
            if exports[name] != STT_FUNC:
                errors.append("%s is not a function" % name)

    for text in REQUIRED_STRINGS:
        if text + b"\0" not in data:
            errors.append("missing string %s" % text.decode("ascii"))

    return needed, sorted(exports), errors


def main(argv):
    args = argv[1:]
    skip_missing = False
    if args and args[0] == "--skip-missing":
        skip_missing = True
        args = args[1:]
    if len(args) != 1:
        sys.stderr.write(__doc__)
        return 2
    path = args[0]
    try:
        with open(path, "rb") as f:
            data = f.read()
    except FileNotFoundError:
        if skip_missing:
            print("check_zbproxy: %s not built, skipping" % path)
            return 77
        print("check_zbproxy: FAIL %s: file not found" % path)
        return 1
    try:
        needed, exports, errors = check(data)
    except CheckError as e:
        print("check_zbproxy: FAIL %s: %s" % (path, e))
        return 1
    if errors:
        for error in errors:
            print("check_zbproxy: FAIL %s: %s" % (path, error))
        return 1
    print("check_zbproxy: OK %s (NEEDED: %s; exports: %s)" % (path, " ".join(needed), " ".join(exports)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
