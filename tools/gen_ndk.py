#!/usr/bin/env python3
"""Generate host-call protocols, backends and dispatchers from plain NDK headers.

Two families so far, both of them shaped the same way: a handle, a few scalars, a scalar result.

    input          android/input.h          AInputQueue, AInputEvent, AKeyEvent, AMotionEvent
    configuration  android/configuration.h  AConfiguration

For each family the outputs are committed:

    core/include/zb/<family>_hostcalls.h   indices, one per entry point
    core/include/zb/<family>_backend.h     the abstract platform side
    core/src/gen/<family>_dispatch.inc     the dispatcher the host unit includes
    core/src/gen/<family>_driver.inc       the device implementation, one call each

Run with --check to fail when the committed files are stale (ctest: gen_ndk_check).

What is not mechanical stays hand-written in the host unit, for the same reasons the GLES
generator leaves its own list to gl_manual.cpp: the JNI conversions, anything that writes through
a guest pointer, and everything that owns an object's life.
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import gen_stubs  # noqa: E402

INCLUDE = os.path.join(os.path.expanduser("~"), "android-ndk-r29", "toolchains", "llvm", "prebuilt",
                       "linux-arm64", "sysroot", "usr", "include", "android")

# Objects the guest only ever sees as 32-bit handles, and the name of the lookup the host unit
# provides for each. A lookup that returns nullptr fails the call before the backend sees it.
HANDLES = {
    "const AInputEvent*": ("const void*", "live_event", "const AInputEvent*"),
    "AInputEvent*": ("const void*", "live_event", "const AInputEvent*"),
    "AInputQueue*": ("void*", "live_queue", "AInputQueue*"),
    "AConfiguration*": ("void*", "live_config", "AConfiguration*"),
}
SCALARS = {
    "int32_t": "std::int32_t",
    "int": "std::int32_t",
    "size_t": "std::size_t",
    "uint32_t": "std::uint32_t",
}
RESULT_TYPES = {
    "void": ("void", "void"),
    "int32_t": ("std::int32_t", "word"),
    "int64_t": ("std::int64_t", "pair"),
    "float": ("float", "float"),
    "size_t": ("std::size_t", "word"),
    "uint32_t": ("std::uint32_t", "word"),
}

FAMILIES = {
    "input": {
        "header": "input.h",
        "prefix": "ZB_INPUT_HC",
        "names": lambda: gen_stubs.input_names(),
        "backend_class": "InputBackend",
        "driver_class": "InputDriverBackend",
        "manual": {
            "AInputQueue_attachLooper": "ties the queue to a looper of this thread",
            "AInputQueue_detachLooper": "ties the queue to a looper of this thread",
            "AInputQueue_hasEvents": "queue handle only",
            "AInputQueue_getEvent": "writes the event handle through a guest pointer",
            "AInputQueue_preDispatchEvent": "queue and event handles",
            "AInputQueue_finishEvent": "ends an event handle's life",
            "AInputEvent_release": "ends an event handle's life",
            "AInputEvent_toJava": "JNI conversion",
            "AInputQueue_fromJava": "JNI conversion",
            "AKeyEvent_fromJava": "JNI conversion",
            "AMotionEvent_fromJava": "JNI conversion",
        },
        "manual_backend": [
            "    // Hand-written entry points: the queue lifecycle and the event handle's life.",
            "    virtual std::int32_t queue_has_events(void* queue) = 0;",
            "    // Returns the next event, or nullptr when there is none; status is the NDK's result.",
            "    virtual void* queue_get_event(void* queue, std::int32_t& status) = 0;",
            "    virtual std::int32_t queue_pre_dispatch(void* queue, void* event) = 0;",
            "    virtual void queue_finish_event(void* queue, void* event, std::int32_t handled) = 0;",
            "    // Attaches the queue to the real looper of the calling host thread, or detaches it.",
            "    // The looper is the thread's own: a host AInputQueue has no descriptor we could poll",
            "    // ourselves, so the queue is given to a real Android looper on that same thread.",
            "    // `data` is the guest's own value, round-tripped: the looper hands it back from",
            "    // poll, and the guest reads its own word again. No host pointer is involved.",
            "    // `looper` is the real looper the queue belongs to, as HostLooper resolved it; 0",
            "    // means the calling thread's own. A queue attached to the wrong looper is never",
            "    // polled, and the framework waits out its five seconds for an event nobody took.",
            "    virtual void queue_attach_looper(void* queue, std::int32_t ident, void* data,",
            "                                     std::uint64_t looper) = 0;",
            "    virtual void queue_detach_looper(void* queue) = 0;",
        ],
        "doc": ["// The input side of the platform, abstract so the host tests can supply their own. Events",
                "// and queues are host objects; this interface speaks in host pointers, and turning those",
                "// into the 32-bit handles the guest sees is HostInput's job."],
    },
    "configuration": {
        "header": "configuration.h",
        "prefix": "ZB_CONFIG_HC",
        "names": lambda: gen_stubs.configuration_names(),
        "backend_class": "ConfigurationBackend",
        "driver_class": "ConfigurationDriverBackend",
        "manual": {
            "AConfiguration_new": "owns the object's life",
            "AConfiguration_delete": "owns the object's life",
            "AConfiguration_fromAssetManager": "takes the asset-manager handle",
            "AConfiguration_getLanguage": "writes two characters into guest memory",
            "AConfiguration_getCountry": "writes two characters into guest memory",
            "AConfiguration_setLanguage": "reads a string from guest memory",
            "AConfiguration_setCountry": "reads a string from guest memory",
        },
        "manual_backend": [
            "    // Hand-written entry points: the object's life, the asset manager and the two",
            "    // two-character fields, which cross as guest memory rather than as values.",
            "    virtual void* create() = 0;",
            "    virtual void destroy(void* config) = 0;",
            "    virtual void from_asset_manager(void* config, void* asset_manager) = 0;",
            "    // Writes exactly two characters, unpadded, as the NDK does.",
            "    virtual void get_language(void* config, char* out) = 0;",
            "    virtual void get_country(void* config, char* out) = 0;",
            "    virtual void set_language(void* config, const char* language) = 0;",
            "    virtual void set_country(void* config, const char* country) = 0;",
        ],
        "doc": ["// The device configuration, abstract so the host tests can supply their own. A",
                "// configuration is a host object; the guest holds a 32-bit handle for it."],
    },
}

def declarations(header, names):
    """{name: (result, parameters, introduced level)} for the entry points of one header."""
    text = open(os.path.join(INCLUDE, header)).read()
    text = re.sub(r"//[^\n]*", "", text)
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    wanted = "|".join(sorted({name.split("_")[0] for name in names}))
    pattern = re.compile(
        r"([A-Za-z_][A-Za-z0-9_ \*]*?)\b((?:%s)\w*)\s*"
        r"\(([^;{()]*)\)\s*(?:__INTRODUCED_IN\(([^)]*)\))?\s*;" % wanted, re.S)
    found = {}
    for match in pattern.finditer(text):
        result = " ".join(match.group(1).split())
        name = match.group(2)
        raw = " ".join(match.group(3).split())
        parameters = []
        for part in raw.split(","):
            part = part.strip()
            if not part or part == "void":
                continue
            words = part.replace("*", "* ").split()
            parameter_name = words[-1]
            parameter_type = " ".join(words[:-1]).replace("* ", "*").strip()
            parameters.append((parameter_type, parameter_name))
        found.setdefault(name, (result, parameters, 0))
    return found


def indices():
    """{name: host-call index}, from the single append-only stub space."""
    index = 0
    result = {}
    for _library, source in gen_stubs.LIBRARIES:
        for name in source():
            result[name] = index
            index += 1
    return result


def classify(result, parameters):
    """(host result type, result kind, [(host type, kind, name)]) or None when unsupported."""
    if result not in RESULT_TYPES:
        return None
    host_result, result_kind = RESULT_TYPES[result]
    arguments = []
    for parameter_type, parameter_name in parameters:
        if parameter_type in HANDLES:
            host_type, lookup, _ndk_type = HANDLES[parameter_type]
            arguments.append((host_type, lookup, parameter_name))
        elif parameter_type in SCALARS:
            arguments.append((SCALARS[parameter_type], "scalar", parameter_name))
        else:
            return None
    return host_result, result_kind, arguments


def generated_entries(declared, order, manual):
    entries = []
    for name in order:
        if name in manual:
            continue
        result, parameters, level = declared[name]
        classified = classify(result, parameters)
        if classified is None:
            sys.exit("%s has a shape this generator does not know (%s)" % (name, result))
        entries.append((name, classified, level))
    return entries


def hostcalls_header(family, order, index_of):
    lines = [
        "#pragma once",
        "",
        "#include <cstdint>",
        "",
        "namespace zb {",
        "",
        "// Generated by tools/gen_input.py. Do not edit.",
        "// Host-call indices of these entry points, from the single append-only stub space",
        "// (tools/gen_stubs.py, core/src/gen/hostcalls.inc).",
    ]
    for name in order:
        lines.append("inline constexpr std::uint32_t %s_%s = %du;" % (family["prefix"], name, index_of[name]))
    lines += [
        "",
        "inline constexpr std::uint32_t %s_FIRST = %du;" % (family["prefix"], min(index_of[n] for n in order)),
        "inline constexpr std::uint32_t %s_LAST = %du;" % (family["prefix"], max(index_of[n] for n in order)),
        "",
        "}  // namespace zb",
        "",
    ]
    return "\n".join(lines)


def backend_header(family, entries):
    lines = [
        "#pragma once",
        "",
        "#include <cstddef>",
        "#include <cstdint>",
        "",
        "namespace zb {",
        "",
        "// Generated by tools/gen_ndk.py. Do not edit.",
        "//",
    ] + family["doc"] + [
        "class %s {" % family["backend_class"],
        "public:",
        "    virtual ~%s() = default;" % family["backend_class"],
        "",
    ] + family["manual_backend"] + [
        "",
        "    // Generated accessors.",
    ]
    for name, (host_result, _result_kind, arguments), _level in entries:
        # The names are commented out: a backend that does not override the accessor would
        # otherwise warn about every unused parameter.
        parameters = ", ".join("%s /* %s */" % (host_type, parameter_name)
                               for host_type, _kind, parameter_name in arguments)
        default = "" if host_result == "void" else " return %s{};" % host_result
        lines.append("    virtual %s %s(%s) {%s }" % (host_result, name, parameters, default))
    lines += [
        "};",
        "",
        "}  // namespace zb",
        "",
    ]
    return "\n".join(lines)


def dispatch_include(family, entries):
    lines = [
        "// Generated by tools/gen_ndk.py. Do not edit.",
        "// The mechanical half of the host unit's handle_host_call: a handle, a few",
        "// scalars, a scalar result. A handle that is not live fails the call before the backend",
        "// sees it, and nothing here dereferences a value the guest chose.",
    ]
    for name, (host_result, result_kind, arguments), _level in entries:
        lines.append("case %s_%s: {" % (family["prefix"], name))
        call_arguments = []
        for position, (host_type, kind, parameter_name) in enumerate(arguments):
            if kind != "scalar":
                lines.append("    %s %s = %s(regs[%d]);" % (host_type, parameter_name, kind, position))
                lines.append("    if (%s == nullptr) { regs[0] = regs[1] = 0; return reject(\"%s\", regs[%d]); }" %
                             (parameter_name, name, position))
            else:
                lines.append("    const %s %s = static_cast<%s>(regs[%d]);" %
                             (host_type, parameter_name, host_type, position))
            call_arguments.append(parameter_name)
        call = "backend_.%s(%s)" % (name, ", ".join(call_arguments))
        if result_kind == "void":
            lines.append("    %s;" % call)
            lines.append("    regs[0] = 0;")
        elif result_kind == "word":
            lines.append("    regs[0] = static_cast<std::uint32_t>(%s);" % call)
        elif result_kind == "pair":
            lines.append("    const std::uint64_t result = static_cast<std::uint64_t>(%s);" % call)
            lines.append("    regs[0] = static_cast<std::uint32_t>(result);")
            lines.append("    regs[1] = static_cast<std::uint32_t>(result >> 32);")
        elif result_kind == "float":
            lines.append("    // AAPCS softfp: a float result comes back in a core register.")
            lines.append("    const float result = %s;" % call)
            lines.append("    std::memcpy(&regs[0], &result, sizeof result);")
        lines.append("    return true;")
        lines.append("}")
    return "\n".join(lines) + "\n"


def driver_include(family, entries):
    """The real NDK implementation of the generated accessors, for core/android."""
    lines = [
        "// Generated by tools/gen_ndk.py. Do not edit.",
        "// The accessors of %s: each one casts the host pointer back to the NDK" % family["driver_class"],
        "// type and calls the real entry point. Included inside the class body.",
    ]
    for name, (host_result, _result_kind, arguments), level in entries:
        parameters = ", ".join("%s %s" % (host_type, parameter_name)
                               for host_type, _kind, parameter_name in arguments)
        call_arguments = []
        for host_type, kind, parameter_name in arguments:
            ndk_type = next((ndk for _host, lookup, ndk in HANDLES.values() if lookup == kind), None)
            if ndk_type is not None:
                call_arguments.append("static_cast<%s>(%s)" % (ndk_type, parameter_name))
            else:
                call_arguments.append(parameter_name)
        # Every entry point is resolved once at run time rather than linked: the NDK headers are
        # not a reliable guide to which API level a symbol appeared in (AConfiguration_setScreenRound
        # carries no marker yet exists only from 30), and a device older than the entry point must
        # get a default instead of failing to load the library at all.
        returns = "" if host_result == "void" else "return "
        signature = "%s (*)(%s)" % (host_result, ", ".join(
            next((ndk for _host, lookup, ndk in HANDLES.values() if lookup == kind), host_type)
            for host_type, kind, _parameter_name in arguments))
        default = "" if host_result == "void" else " return %s{};" % host_result
        lines.append("%s %s(%s) override {" % (host_result, name, parameters))
        lines.append("    using Fn = %s;" % signature)
        lines.append("    static Fn fn = reinterpret_cast<Fn>(dlsym(RTLD_DEFAULT, \"%s\"));" % name)
        lines.append("    if (fn == nullptr) {%s }" % (default if default else " return;"))
        lines.append("    %s fn(%s);" % (returns, ", ".join(call_arguments)))
        lines.append("}")
    return "\n".join(lines) + "\n"


def generate():
    index_of = indices()
    outputs = {}
    for family_name, family in FAMILIES.items():
        order = family["names"]()
        declared = declarations(family["header"], order)
        missing = [name for name in order if name not in declared]
        if missing:
            sys.exit("%s: no declaration for %s" % (family["header"], ", ".join(missing)))
        entries = generated_entries(declared, order, family["manual"])
        outputs["core/include/zb/%s_hostcalls.h" % family_name] = hostcalls_header(family, order, index_of)
        outputs["core/include/zb/%s_backend.h" % family_name] = backend_header(family, entries)
        outputs["core/src/gen/%s_dispatch.inc" % family_name] = dispatch_include(family, entries)
        outputs["core/src/gen/%s_driver.inc" % family_name] = driver_include(family, entries)
    return outputs


def main():
    check = sys.argv[1:] == ["--check"]
    if sys.argv[1:] and not check:
        sys.exit("usage: gen_ndk.py [--check]")
    stale = []
    for path, content in generate().items():
        full = os.path.join(ROOT, path)
        if check:
            try:
                with open(full) as source:
                    current = source.read()
            except OSError:
                current = None
            if current != content:
                stale.append(path)
        else:
            os.makedirs(os.path.dirname(full), exist_ok=True)
            with open(full, "w") as output:
                output.write(content)
            print("wrote " + path)
    if stale:
        sys.exit("stale generated files (run tools/gen_ndk.py):\n  " + "\n  ".join(stale))


if __name__ == "__main__":
    main()
