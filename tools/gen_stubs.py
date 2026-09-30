#!/usr/bin/env python3
"""Generate arm32 guest stub libraries whose functions trap into the host.

Every exported function is two ARM-mode instructions:
    svc #(0x5A0000 | index)
    bx  lr
The svc immediate identifies the host call; arguments stay in r0-r3 and on the guest
stack exactly as the caller placed them (AAPCS softfp), so the host handler can read
them. Indices 0xFB00-0xFCFF are reserved for the JNI bridge
(core/include/zb/jni_protocol.h), 0xFE00-0xFEFF for the library runtime
(core/include/zb/library_protocol.h) and 0xFFFF for returning from host->guest calls, so
generated indices must stay below 0xFB00.

Outputs (committed):
  guest/stubs/gen/<lib>.S        one assembly file per stub library
  core/src/gen/hostcalls.inc     {index, "lib", "name"} rows for the host dispatcher
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
NDK = os.environ.get("NDK", os.path.expanduser("~/android-ndk-r29"))
NDK_HOST = os.environ.get("NDK_HOST", "linux-arm64")
INCLUDE = os.path.join(NDK, "toolchains", "llvm", "prebuilt", NDK_HOST, "sysroot", "usr", "include")

HOST_CALL_BASE = 0x5A0000
HOST_RETURN_INDEX = 0xFFFF
# ZB_JNI_SLOT_STUB_FIRST in core/include/zb/jni_protocol.h: the first index above the stubs.
RESERVED_HOST_CALL_FIRST = 0xFB00


def gles2_names():
    text = open(os.path.join(INCLUDE, "GLES2", "gl2.h")).read()
    return sorted(set(re.findall(r"GL_APICALL\s+[^;]*?GL_APIENTRY\s+(gl\w+)\s*\(", text)))


def gles3_names():
    """The GLES 3.0 entry points that GLES 2.0 does not already export.

    Android's real libGLESv2.so exports both, so these go into the same guest stub library.
    They are registered as a second libGLESv2 entry at the end of LIBRARIES, which appends to
    that library's assembly file without renumbering any established host-call index.
    """
    text = open(os.path.join(INCLUDE, "GLES3", "gl3.h")).read()
    names = set(re.findall(r"GL_APICALL\s+[^;]*?GL_APIENTRY\s+(gl\w+)\s*\(", text))
    return sorted(names - set(gles2_names()))


# Curated GLES extension entry points, the ones guests resolve through eglGetProcAddress and
# then call without checking the result for NULL. gen_gles.py takes the signatures from
# gl.xml and checks every name below against that extension's <require> block, so this list
# holds names only, never prototypes.
#
# APPEND ONLY, at the end: adding an extension is one line here, and appending keeps every
# established host-call index where it is.
GLES_EXTENSIONS = [
    ("GL_EXT_multisampled_render_to_texture",
     ["glRenderbufferStorageMultisampleEXT", "glFramebufferTexture2DMultisampleEXT"]),
    ("GL_EXT_discard_framebuffer", ["glDiscardFramebufferEXT"]),
    ("GL_OES_vertex_array_object",
     ["glBindVertexArrayOES", "glDeleteVertexArraysOES", "glGenVertexArraysOES",
      "glIsVertexArrayOES"]),
    ("GL_OES_mapbuffer", ["glMapBufferOES", "glUnmapBufferOES", "glGetBufferPointervOES"]),
    ("GL_EXT_texture_storage", ["glTexStorage2DEXT", "glTexStorage3DEXT"]),
]


def gles_ext_names():
    """Every name in GLES_EXTENSIONS, in declaration order."""
    names = [name for _, extension_names in GLES_EXTENSIONS for name in extension_names]
    if len(names) != len(set(names)):
        sys.exit("GLES_EXTENSIONS lists a name twice")
    return names


def android_asset_names():
    names = set()
    decl = re.compile(r"^[A-Za-z_][\w \*]*\b(AAsset\w*)\(")
    for header in ("asset_manager.h", "asset_manager_jni.h"):
        for line in open(os.path.join(INCLUDE, "android", header)):
            m = decl.match(line)
            if m:
                names.add(m.group(1))
    return sorted(names)


# Appended after the AAsset* names so the hand-written indices in
# core/include/zb/asset_hostcalls.h (145-157) keep pointing at the same functions.
ANATIVE_WINDOW = [
    "ANativeWindow_acquire",
    "ANativeWindow_fromSurface",
    "ANativeWindow_getFormat",
    "ANativeWindow_getHeight",
    "ANativeWindow_getWidth",
    "ANativeWindow_release",
    "ANativeWindow_setBuffersGeometry",
    "ANativeWindow_toSurface",
]


def android_names():
    return android_asset_names() + ANATIVE_WINDOW


def egl_names():
    text = open(os.path.join(INCLUDE, "EGL", "egl.h")).read()
    return sorted(set(re.findall(r"EGLAPI\s+[^;]*?EGLAPIENTRY\s+(egl\w+)\s*\(", text)))


def android_compat_names():
    return [
        "ANativeWindow_lock",
        "ANativeWindow_unlockAndPost",
    ]


def egl_compat_names():
    return [
        "eglCreateImageKHR",
        "eglDestroyImageKHR",
    ]


def gles2_compat_names():
    return ["glEGLImageTargetTexture2DOES"]


def jnigraphics_names():
    return [
        "AndroidBitmap_getInfo",
        "AndroidBitmap_lockPixels",
        "AndroidBitmap_unlockPixels",
    ]


def looper_compat_names():
    return [
        "ALooper_acquire",
        "ALooper_addFd",
        "ALooper_forThread",
        "ALooper_pollOnce",
        "ALooper_prepare",
        "ALooper_release",
        "ALooper_removeFd",
        "ALooper_wake",
    ]


def sensor_names():
    """Every ASensor* entry point of the NDK header, in name order.

    A guest that imports one missing name does not load at all, so the list comes from the header
    rather than from what one game happens to call: Unity stops on
    ASensorEventQueue_disableSensor, and the next guest would stop on a different one.
    """
    names = set()
    decl = re.compile(r"^[A-Za-z_][\w \*]*\b(ASensor\w*)\(")
    for line in open(os.path.join(INCLUDE, "android", "sensor.h")):
        m = decl.match(line)
        if m:
            names.add(m.group(1))
    return sorted(names)


def input_names():
    """Every AInputQueue/AInputEvent/AKeyEvent/AMotionEvent entry point of the NDK header.

    From the header, not from what one game imports: a guest library with one unresolved name
    does not load at all, and the NativeActivity guests differ in which accessors they use.
    """
    names = set()
    text = open(os.path.join(INCLUDE, "android", "input.h")).read()
    for match in re.finditer(
            r"^[A-Za-z_][A-Za-z0-9_ \*]*?\b((?:AInputQueue|AInputEvent|AKeyEvent|AMotionEvent)\w*)\s*\(",
            text, re.M):
        names.add(match.group(1))
    return sorted(names)


def gles1_names():
    """Every GLES 1.1 entry point of the NDK header.

    Unity 4.5 links libGLESv1_CM.so even when it renders through GLES 2, and a library that is
    not there stops the whole guest from loading. The stubs exist so the load succeeds; a guest
    that really calls one lands in the report as an unimplemented host call, by name.
    """
    text = open(os.path.join(INCLUDE, "GLES", "gl.h")).read()
    return sorted(set(re.findall(r"^GL_API\w*\s+\w[\w \*]*\bGL_APIENTRY\s+(gl\w+)\s*\(", text, re.M)))


def looper_pollall_names():
    """ALooper_pollAll, appended late because it turned up in a real guest after the rest.

    Removed from the NDK headers long ago but still imported by Unity 4.5's libmain.so, and one
    unresolved import stops the whole library from loading.
    """
    return ["ALooper_pollAll"]


def configuration_names():
    """Every AConfiguration_* entry point of the NDK header."""
    names = set()
    text = open(os.path.join(INCLUDE, "android", "configuration.h")).read()
    for match in re.finditer(r"^[A-Za-z_][A-Za-z0-9_ \*]*?\b(AConfiguration\w*)\s*\(", text, re.M):
        names.add(match.group(1))
    return sorted(names)


LIBRARIES = [
    ("libGLESv2", gles2_names),
    ("libandroid", android_names),
    ("libEGL", egl_names),
    # Compatibility exports are append-only. Repeating an existing library appends symbols to
    # its assembly file without shifting any established host-call index.
    ("libandroid", android_compat_names),
    ("libEGL", egl_compat_names),
    ("libGLESv2", gles2_compat_names),
    ("libjnigraphics", jnigraphics_names),
    ("libandroid", looper_compat_names),
    # GLES 3.0. Appended last on purpose: the GLES 2.0 indices (0-141), the AAsset* ones
    # (142-159), ANativeWindow_* (160-167) and EGL (168-211) are hand-referenced elsewhere
    # (core/include/zb/asset_hostcalls.h, window_hostcalls.h, egl_hostcalls.h) and must not move.
    ("libGLESv2", gles3_names),
    # GLES extension entry points, after GLES 3.0 for the same reason.
    ("libGLESv2", gles_ext_names),
    # Sensors, appended last (core/include/zb/sensor_hostcalls.h holds the indices by hand).
    ("libandroid", sensor_names),
    # Input: NativeActivity guests (phase 7b). Indices in core/include/zb/input_hostcalls.h,
    # which tools/gen_input.py generates from this order.
    ("libandroid", input_names),
    # Configuration, after input for the same append-only reason.
    ("libandroid", configuration_names),
    # ALooper_pollAll, last: Unity 4.5 imports it and nothing else here does.
    ("libandroid", looper_pollall_names),
    # GLES 1.1: linked by Unity 4.5, so the library has to exist for the guest to load at all.
    ("libGLESv1_CM", gles1_names),
]


def main():
    out_asm = os.path.join(ROOT, "guest", "stubs", "gen")
    out_host = os.path.join(ROOT, "core", "src", "gen")
    os.makedirs(out_asm, exist_ok=True)
    os.makedirs(out_host, exist_ok=True)

    index = 0
    rows = []
    assemblies = {}
    # A name that two guest libraries both export is one host call, not two. GLES 1 and GLES 2
    # share most of their entry points, and the guest linker binds a caller to whichever library
    # comes first: giving the GLES 1 copy its own index would send a call that GLES 2 already
    # serves into an empty stub instead. Unity did exactly that - its texture and state calls
    # landed in libGLESv1_CM.so and did nothing.
    assigned = {}
    for lib, source in LIBRARIES:
        names = source()
        if not names:
            sys.exit("no functions found for " + lib)
        lines = assemblies.setdefault(
            lib,
            [
                "@ Generated by tools/gen_stubs.py. Do not edit.",
                ".syntax unified",
                ".arm",
                ".text",
                "",
            ],
        )
        for name in names:
            shared = assigned.get(name)
            if shared is None and index >= RESERVED_HOST_CALL_FIRST:
                sys.exit("too many host calls: index 0x%x reaches the reserved range" % index)
            entry = shared if shared is not None else index
            lines += [
                ".global %s" % name,
                ".type %s, %%function" % name,
                ".p2align 2",
                "%s:" % name,
                "    svc #0x%x" % (HOST_CALL_BASE | entry),
                "    bx lr",
                ".size %s, . - %s" % (name, name),
                "",
            ]
            if shared is not None:
                continue  # the same host call, reached through a second library
            rows.append((index, lib + ".so", name))
            assigned[name] = index
            index += 1
        print("%s: %d functions" % (lib, len(names)))

    for lib, lines in assemblies.items():
        with open(os.path.join(out_asm, lib + ".S"), "w") as f:
            f.write("\n".join(lines))

    with open(os.path.join(out_host, "hostcalls.inc"), "w") as f:
        f.write("// Generated by tools/gen_stubs.py. Do not edit.\n")
        for i, lib, name in rows:
            f.write('{%d, "%s", "%s"},\n' % (i, lib, name))
    print("host calls: %d" % index)


if __name__ == "__main__":
    main()
