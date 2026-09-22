#!/usr/bin/env python3
"""Generate the input host-call protocol, backend and dispatcher from the NDK header.

Inputs: the NDK's android/input.h (signatures) and gen_stubs.input_names() (the names and their
host-call order). Outputs, all committed:

    core/include/zb/input_hostcalls.h   indices, one per entry point
    core/include/zb/input_backend.h     the abstract Java/NDK side
    core/src/gen/input_dispatch.inc     the dispatcher HostInput includes

Run with --check to fail when the committed files are stale (ctest: gen_input_check).

The accessors are mechanical: an event or queue handle, a few scalars, and a scalar result. They
are generated whole. The ones that are not - the JNI conversions, the queue lifecycle and
AInputQueue_getEvent, which writes through a guest pointer - are declared here and implemented by
hand in HostInput, for the same reasons the GLES generator leaves its own list to gl_manual.cpp.
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import gen_stubs  # noqa: E402

# The API level core/android is built against (ANDROID_PLATFORM in the CMake command). An entry
# point introduced later cannot be called directly: the NDK marks it unavailable. It is looked up
# at run time instead, which is right anyway - the device may be older than the entry point.
BUILD_API = 29
API_LEVELS = {"__ANDROID_API_S__": 31, "__ANDROID_API_S_V2__": 32, "__ANDROID_API_T__": 33,
              "__ANDROID_API_U__": 34, "__ANDROID_API_V__": 35}

HEADER = os.path.join(os.path.expanduser("~"), "android-ndk-r29", "toolchains", "llvm", "prebuilt",
                      "linux-arm64", "sysroot", "usr", "include", "android", "input.h")

# Hand-written in HostInput, with the reason.
MANUAL = {
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
}

# C type -> (host C++ type, how the dispatcher reads or writes it).
ARGUMENT_TYPES = {
    "const AInputEvent*": ("const void*", "event"),
    "AInputEvent*": ("const void*", "event"),
    "AInputQueue*": ("void*", "queue"),
    "int32_t": ("std::int32_t", "scalar"),
    "int": ("std::int32_t", "scalar"),
    "size_t": ("std::size_t", "scalar"),
}
RESULT_TYPES = {
    "void": ("void", "void"),
    "int32_t": ("std::int32_t", "word"),
    "int64_t": ("std::int64_t", "pair"),
    "float": ("float", "float"),
    "size_t": ("std::size_t", "word"),
}


def declarations():
    """{name: (result type, [(type, name), ...])} for every entry point of the header."""
    text = open(HEADER).read()
    text = re.sub(r"//[^\n]*", "", text)
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    pattern = re.compile(
        r"([A-Za-z_][A-Za-z0-9_ \*]*?)\b((?:AInputQueue|AInputEvent|AKeyEvent|AMotionEvent)\w*)\s*"
        r"\(([^;{()]*)\)\s*(?:__INTRODUCED_IN\(([^)]*)\))?\s*;", re.S)
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
        introduced = (match.group(4) or "").strip()
        if introduced.isdigit():
            level = int(introduced)
        elif introduced in API_LEVELS:
            level = API_LEVELS[introduced]
        elif introduced:
            level = 99  # an unknown marker is treated as late, which only costs a lookup
        else:
            level = 0
        found.setdefault(name, (result, parameters, level))
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


def classify(name, result, parameters):
    """(host result type, result kind, [(host type, kind, name)]) or None when unsupported."""
    if result not in RESULT_TYPES:
        return None
    host_result, result_kind = RESULT_TYPES[result]
    arguments = []
    for parameter_type, parameter_name in parameters:
        if parameter_type not in ARGUMENT_TYPES:
            return None
        host_type, kind = ARGUMENT_TYPES[parameter_type]
        arguments.append((host_type, kind, parameter_name))
    return host_result, result_kind, arguments


def generated_entries(declared, order):
    entries = []
    for name in order:
        if name in MANUAL:
            continue
        result, parameters, level = declared[name]
        classified = classify(name, result, parameters)
        if classified is None:
            sys.exit("input.h: %s has a shape this generator does not know (%s)" % (name, result))
        entries.append((name, classified, level))
    return entries


def hostcalls_header(order, index_of):
    lines = [
        "#pragma once",
        "",
        "#include <cstdint>",
        "",
        "namespace zb {",
        "",
        "// Generated by tools/gen_input.py. Do not edit.",
        "// Host-call indices of the input entry points, from the single append-only stub space",
        "// (tools/gen_stubs.py, core/src/gen/hostcalls.inc).",
    ]
    for name in order:
        lines.append("inline constexpr std::uint32_t ZB_INPUT_HC_%s = %du;" % (name, index_of[name]))
    lines += [
        "",
        "inline constexpr std::uint32_t ZB_INPUT_HC_FIRST = %du;" % min(index_of[n] for n in order),
        "inline constexpr std::uint32_t ZB_INPUT_HC_LAST = %du;" % max(index_of[n] for n in order),
        "",
        "}  // namespace zb",
        "",
    ]
    return "\n".join(lines)


def backend_header(entries):
    lines = [
        "#pragma once",
        "",
        "#include <cstddef>",
        "#include <cstdint>",
        "",
        "namespace zb {",
        "",
        "// Generated by tools/gen_input.py. Do not edit.",
        "//",
        "// The input side of the platform, abstract so the host tests can supply their own. Events",
        "// and queues are host objects; this interface speaks in host pointers, and turning those",
        "// into the 32-bit handles the guest sees is HostInput's job.",
        "class InputBackend {",
        "public:",
        "    virtual ~InputBackend() = default;",
        "",
        "    // Hand-written entry points: the queue lifecycle and the event handle's life.",
        "    virtual std::int32_t queue_has_events(void* queue) = 0;",
        "    // Returns the next event, or nullptr when there is none; status is the NDK's result.",
        "    virtual void* queue_get_event(void* queue, std::int32_t& status) = 0;",
        "    virtual std::int32_t queue_pre_dispatch(void* queue, void* event) = 0;",
        "    virtual void queue_finish_event(void* queue, void* event, std::int32_t handled) = 0;",
        "    // Attaches the queue to the real looper of the calling host thread, or detaches it.",
        "    // The looper is the thread's own: a host AInputQueue has no descriptor we could poll",
        "    // ourselves, so the queue is given to a real Android looper on that same thread.",
        "    virtual void queue_attach_looper(void* queue, std::int32_t ident) = 0;",
        "    virtual void queue_detach_looper(void* queue) = 0;",
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


def dispatch_include(entries):
    lines = [
        "// Generated by tools/gen_input.py. Do not edit.",
        "// The mechanical half of HostInput::handle_host_call: an event or queue handle, a few",
        "// scalars, a scalar result. A handle that is not live fails the call before the backend",
        "// sees it, and nothing here dereferences a value the guest chose.",
    ]
    for name, (host_result, result_kind, arguments), _level in entries:
        lines.append("case ZB_INPUT_HC_%s: {" % name)
        call_arguments = []
        for position, (host_type, kind, parameter_name) in enumerate(arguments):
            if kind == "event":
                lines.append("    const void* %s = live_event(regs[%d]);" % (parameter_name, position))
                lines.append("    if (%s == nullptr) { regs[0] = regs[1] = 0; return reject(\"%s\", regs[%d]); }" %
                             (parameter_name, name, position))
            elif kind == "queue":
                lines.append("    void* %s = live_queue(regs[%d]);" % (parameter_name, position))
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


def driver_include(entries):
    """The real NDK implementation of the generated accessors, for core/android."""
    lines = [
        "// Generated by tools/gen_input.py. Do not edit.",
        "// The accessors of InputDriverBackend: each one casts the host pointer back to the NDK",
        "// type and calls the real entry point. Included inside the class body.",
    ]
    for name, (host_result, _result_kind, arguments), level in entries:
        parameters = ", ".join("%s %s" % (host_type, parameter_name)
                               for host_type, _kind, parameter_name in arguments)
        call_arguments = []
        for host_type, kind, parameter_name in arguments:
            if kind == "event":
                call_arguments.append("static_cast<const AInputEvent*>(%s)" % parameter_name)
            elif kind == "queue":
                call_arguments.append("static_cast<AInputQueue*>(%s)" % parameter_name)
            else:
                call_arguments.append(parameter_name)
        # Qualified: inside the class the unqualified name is this very override, and the call
        # would recurse into itself instead of reaching the NDK.
        returns = "" if host_result == "void" else "return "
        if level <= BUILD_API:
            body = "::%s(%s)" % (name, ", ".join(call_arguments))
            lines.append("%s %s(%s) override { %s%s; }" % (host_result, name, parameters, returns, body))
            continue
        # Introduced after the level we build against: resolved once at run time, and answered
        # with a default on a device that does not have it.
        signature = "%s (*)(%s)" % (host_result, ", ".join(
            ("const AInputEvent*" if kind == "event" else "AInputQueue*" if kind == "queue" else host_type)
            for host_type, kind, _parameter_name in arguments))
        default = "" if host_result == "void" else " return %s{};" % host_result
        lines.append("%s %s(%s) override {  // Android %d" % (host_result, name, parameters, level))
        lines.append("    using Fn = %s;" % signature)
        lines.append("    static Fn fn = reinterpret_cast<Fn>(dlsym(RTLD_DEFAULT, \"%s\"));" % name)
        lines.append("    if (fn == nullptr) {%s }" % (default if default else " return;"))
        lines.append("    %s fn(%s);" % (returns, ", ".join(call_arguments)))
        lines.append("}")
    return "\n".join(lines) + "\n"


def generate():
    declared = declarations()
    order = gen_stubs.input_names()
    missing = [name for name in order if name not in declared]
    if missing:
        sys.exit("input.h: no declaration for " + ", ".join(missing))
    index_of = indices()
    entries = generated_entries(declared, order)
    return {
        "core/include/zb/input_hostcalls.h": hostcalls_header(order, index_of),
        "core/include/zb/input_backend.h": backend_header(entries),
        "core/src/gen/input_dispatch.inc": dispatch_include(entries),
        "core/src/gen/input_driver.inc": driver_include(entries),
    }


def main():
    check = sys.argv[1:] == ["--check"]
    if sys.argv[1:] and not check:
        sys.exit("usage: gen_input.py [--check]")
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
        sys.exit("stale generated files (run tools/gen_input.py):\n  " + "\n  ".join(stale))


if __name__ == "__main__":
    main()
