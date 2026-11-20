// The JNI bridge end to end on this machine: libzbjni.so inside the library runtime, the flat JNI
// host calls against the mock JVM, and the guest probe library guest/testlib/zbjniprobe.c.
// Usage: jni_bridge_test <sysroot> <zbhost> <guest lib dir>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <barrier>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#include "check.h"
#include "mock_jvm.h"
#include "zb/host_jni.h"
#include "zb/library_runtime.h"
#include "zb/runtime_report.h"

namespace {

using namespace std::chrono_literals;
using zb::JValue;
using zb::mock::MockCall;
using zb::mock::MockJvm;

JValue value_i(std::int32_t v) {
    JValue value{};
    value.i = v;
    return value;
}

std::u16string to_u16(const std::string& ascii) {
    return std::u16string(ascii.begin(), ascii.end());
}

// zb/Probe and zb/ProbeChild: the Java side of the guest-called JNI probes.
void define_probe_model(MockJvm& vm) {
    vm.define_class("zb/Probe");
    vm.define_class("zb/ProbeChild", "zb/Probe");
    vm.define_class("java/lang/IllegalStateException", "java/lang/RuntimeException");
    struct Type {
        char letter;
        const char* descriptor;
        const char* field;
    };
    const Type types[] = {{'Z', "Z", "z"}, {'B', "B", "b"}, {'C', "C", "c"}, {'S', "S", "s"}, {'I', "I", "i"},
                          {'J', "J", "j"}, {'F', "F", "f"}, {'D', "D", "d"}, {'L', "Ljava/lang/String;", "l"}};
    for (const Type& type : types) {
        const std::string signature = std::string("(") + type.descriptor + ")" + type.descriptor;
        const std::string echo = std::string("echo") + type.letter;
        const char letter = type.letter;
        // step 1: zb/Probe (arg+1, !z, *2, same reference); step 2: zb/ProbeChild (arg+2, z, *4, null).
        const auto body = [letter](int step) {
            return [letter, step](MockCall& call) {
                JValue out = call.args[0];
                switch (letter) {
                case 'Z': out.z = step == 1 ? !out.z : out.z; break;
                case 'B': out.b = static_cast<std::int8_t>(out.b + step); break;
                case 'C': out.c = static_cast<std::uint16_t>(out.c + step); break;
                case 'S': out.s = static_cast<std::int16_t>(out.s + step); break;
                case 'I': out.i += step; break;
                case 'J': out.j += step; break;
                case 'F': out.f *= static_cast<float>(2 * step); break;
                case 'D': out.d *= 2.0 * step; break;
                default: out.l = step == 1 ? out.l : 0; break;
                }
                return out;
            };
        };
        vm.add_method("zb/Probe", echo, signature, false, body(1));
        vm.add_method("zb/ProbeChild", echo, signature, false, body(2));
        vm.add_method("zb/Probe", "s" + echo, signature, true, body(1));
        vm.add_field("zb/Probe", type.field, type.descriptor, false);
        vm.add_field("zb/Probe", std::string("s") + type.field, type.descriptor, true);
    }
    const auto bump = [](std::int32_t step) {
        return [step](MockCall& call) {
            const auto cls = call.vm.class_object("zb/Probe");
            call.vm.set_field_value(cls, "calls", value_i(call.vm.field_value(cls, "calls").i + step));
            return JValue{};
        };
    };
    vm.add_method("zb/Probe", "echoV", "()V", false, bump(1));
    vm.add_method("zb/ProbeChild", "echoV", "()V", false, bump(2));
    vm.add_method("zb/Probe", "sechoV", "()V", true, bump(4));
    vm.add_field("zb/Probe", "calls", "I", true);
    vm.add_method("zb/Probe", "mix", "(ZBCSIJFDLjava/lang/String;)I", true, [](MockCall& call) {
        const auto& a = call.args;
        const bool ok = a[0].z == 1 && a[1].b == -2 && a[2].c == 0x1234 && a[3].s == -3 && a[4].i == 4 &&
                        a[5].j == 0x1122334455667788LL && a[6].f == 1.5f && a[7].d == -2.25 &&
                        call.vm.string_value(a[8].l) == u"text";
        return value_i(ok ? 42 : 0);
    });
    vm.add_method("zb/Probe", "<init>", "(ILjava/lang/String;)V", false, [](MockCall& call) {
        call.vm.set_field_value(call.self, "i", call.args[0]);
        call.vm.set_field_value(call.self, "l", call.args[1]);
        return JValue{};
    });
    vm.add_method("zb/Probe", "fail", "()V", true, [](MockCall& call) {
        call.vm.throw_in(call.env, "java/lang/IllegalStateException", "boom");
        return JValue{};
    });
}

// Host AAPCS64 signatures of the registered natives, called through their thunks.
using AddFn = std::int32_t (*)(std::uint64_t, std::uint64_t, std::int32_t, float, float, std::int32_t, float, float);
using WideFn = void (*)(std::uint64_t, std::uint64_t, std::int64_t);
using NestFn = std::int32_t (*)(std::uint64_t, std::uint64_t, std::int32_t);
using EchoFn = std::uint64_t (*)(std::uint64_t, std::uint64_t, std::uint64_t);
using TidFn = std::int32_t (*)(std::uint64_t, std::uint64_t);

// zb/Natives: native methods bound by the guest, and callback(n) = nest(n) + 10 calling back in.
void define_native_model(MockJvm& vm) {
    vm.define_class("zb/Natives");
    vm.add_native("zb/Natives", "add", "(IFFIFF)I", true);
    vm.add_native("zb/Natives", "wide", "(J)V", true);
    vm.add_native("zb/Natives", "nest", "(I)I", true);
    vm.add_native("zb/Natives", "echo", "(Ljava/lang/String;)Ljava/lang/String;", false);
    vm.add_native("zb/Natives", "tid", "()I", true);
    vm.add_field("zb/Natives", "wide", "J", true);
    vm.add_method("zb/Natives", "callback", "(I)I", true, [](MockCall& call) {
        const auto nest = reinterpret_cast<NestFn>(call.vm.native_function("zb/Natives", "nest", "(I)I"));
        MockJvm::NativeFrame frame(call.vm, call.env);
        const std::int32_t result = nest(call.env, frame.local(call.self), call.args[0].i) + 10;
        CHECK(frame.close() == 1);  // the class argument
        return value_i(result);
    });
}

// Java methods that report the calling Java thread.
void define_thread_model(MockJvm& vm) {
    vm.add_method("zb/Probe", "threadName", "()Ljava/lang/String;", true, [](MockCall& call) {
        JValue out{};
        out.l = call.vm.new_string_object(to_u16(call.vm.thread_name(call.env)));
        return out;
    });
    vm.add_method("zb/Probe", "threadDaemon", "()Z", true, [](MockCall& call) {
        JValue out{};
        out.z = call.vm.thread_daemon(call.env) ? 1 : 0;
        return out;
    });
}

zb::LibraryRuntimeOptions options(char** argv) {
    zb::LibraryRuntimeOptions result;
    result.sysroot = argv[1];
    result.zbhost = argv[2];
    result.target_sdk = 16;
    result.guest_environment = {std::string("LD_LIBRARY_PATH=") + argv[3]};
    result.preload = "libzbjni.so";
    return result;
}

struct Bridge {
    MockJvm* vm;
    zb::LibraryRuntime* runtime;
    zb::HostJni* jni;
    std::uint32_t library;
};

// Process-lifetime objects: never destroyed (tests end with _Exit).
Bridge start_bridge(char** argv) {
    Bridge bridge{};
    bridge.vm = new MockJvm();
    define_probe_model(*bridge.vm);
    define_native_model(*bridge.vm);
    define_thread_model(*bridge.vm);
    bridge.runtime = new zb::LibraryRuntime();
    bridge.jni = new zb::HostJni(*bridge.runtime, *bridge.vm, 64);
    zb::HostJni* jni = bridge.jni;
    bridge.runtime->set_host_call_handler(
        [jni](std::uint32_t index, zb::GuestThread& thread) { return jni->handle_host_call(index, thread); });
    std::string error;
    CHECK(bridge.runtime->start(options(argv), error));
    CHECK(bridge.jni->ready() && bridge.jni->guest_java_vm() != 0);
    bridge.library = bridge.runtime->load_library(std::string(argv[3]) + "/libzbjniprobe.so", ZB_GUEST_RTLD_NOW, error);
    CHECK(bridge.library != 0);
    return bridge;
}

std::uint32_t symbol(Bridge& bridge, const char* name) {
    std::string error;
    const std::uint32_t address = bridge.runtime->find_symbol(bridge.library, name, error);
    if (address == 0) std::fprintf(stderr, "missing guest symbol %s: %s\n", name, error.c_str());
    CHECK(address != 0);
    return address;
}

// Calls a probe as a native method of the calling Java thread with one object argument (or null).
// Returns 0, or the probe's failing source line.
std::int32_t run_probe(Bridge& bridge, const char* name, MockJvm::ObjectId argument = 0) {
    const std::uint32_t function = symbol(bridge, name);
    const auto env = bridge.vm->thread_env();
    MockJvm::NativeFrame frame(*bridge.vm, env);
    const auto ref = argument != 0 ? frame.local(argument) : 0;
    auto& counter = zb::runtime_report().native_call_counter(name);
    const auto result =
        bridge.jni->call_native(env, 'I', function, [&](std::uint32_t guest_env, const zb::RefToHandle& to_handle) {
            CHECK(zb::runtime_report().text().find(std::string("native-pending: ") + name + "=1") !=
                  std::string::npos);
            zb::GuestCall call;
            call.regs = {guest_env, to_handle(ref), 0, 0};
            return call;
        }, &counter);
    CHECK(result);
    CHECK(zb::runtime_report().text().find("native-pending: (none)") != std::string::npos);
    const auto line = static_cast<std::int32_t>(result->guest.r0);
    if (line != 0) std::fprintf(stderr, "%s failed at zbjniprobe.c:%d\n", name, line);
    // The call's frame released everything the guest created; only the argument is left.
    CHECK(frame.close() == (argument != 0 ? 1u : 0u));
    return line;
}

// An invalid handle ends the process through the backend's FatalError.
void check_invalid_handle(char** argv) {
    std::fflush(stdout);
    const pid_t child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        Bridge bridge = start_bridge(argv);
        run_probe(bridge, "zbjniprobe_bad_handle");
        std::_Exit(10);
    }
    int status = 0;
    CHECK(waitpid(child, &status, 0) == child);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == zb::mock::kFatalExitStatus);
}

void check_buffer_overflow(char** argv) {
    std::fflush(stdout);
    const pid_t child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        Bridge bridge = start_bridge(argv);
        const auto huge = bridge.vm->new_sparse_primitive_array('J', INT32_MAX);
        std::_Exit(run_probe(bridge, "zbjniprobe_buffer_overflow", huge));
    }
    int status = 0;
    CHECK(waitpid(child, &status, 0) == child);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

void check_bad_direct_capacity(char** argv) {
    std::fflush(stdout);
    const pid_t child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        Bridge bridge = start_bridge(argv);
        run_probe(bridge, "zbjniprobe_bad_direct_capacity");
        std::_Exit(10);
    }
    int status = 0;
    CHECK(waitpid(child, &status, 0) == child);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == zb::mock::kFatalExitStatus);
}

void check_objects(Bridge& bridge) {
    MockJvm& vm = *bridge.vm;
    const auto held = vm.new_object("zb/Probe");
    CHECK(run_probe(bridge, "zbjniprobe_objects", held) == 0);
    CHECK(vm.live_global_refs() == 1 && vm.live_weak_refs() == 1);
    vm.collect(held);
    CHECK(run_probe(bridge, "zbjniprobe_objects_after_gc") == 0);
    CHECK(vm.live_global_refs() == 0 && vm.live_weak_refs() == 0);

    // An exception thrown by the guest stays pending for the Java caller.
    const auto env = vm.thread_env();
    CHECK(run_probe(bridge, "zbjniprobe_throw") == 0);
    const auto pending = vm.pending_exception(env);
    CHECK(pending != 0 && vm.class_name_of(pending) == "java/lang/IllegalArgumentException");
    CHECK(vm.string_value(pending) == u"from guest");
    vm.clear_pending_exception(env);
}

void check_values(Bridge& bridge) {
    MockJvm& vm = *bridge.vm;
    CHECK(run_probe(bridge, "zbjniprobe_calls", vm.new_string_object(u"text")) == 0);
    CHECK(run_probe(bridge, "zbjniprobe_fields", vm.new_string_object(u"field")) == 0);
    const auto probe = vm.class_object("zb/Probe");
    CHECK(vm.field_value(probe, "sz").z == 1 && vm.field_value(probe, "sc").c == 0xBEEF);
    CHECK(vm.field_value(probe, "sj").j == -0x0123456789abcdefLL && vm.field_value(probe, "sf").f == -0.375f);
    CHECK(vm.field_value(probe, "sd").d == 6.02214076e23 && vm.string_value(vm.field_value(probe, "sl").l) == u"field");

    const auto env = vm.thread_env();
    CHECK(run_probe(bridge, "zbjniprobe_exceptions") == 0);
    const auto pending = vm.pending_exception(env);
    CHECK(pending != 0 && vm.class_name_of(pending) == "java/lang/IllegalStateException");
    CHECK(vm.string_value(pending) == u"boom");
    vm.clear_pending_exception(env);
}

void check_data(Bridge& bridge) {
    MockJvm& vm = *bridge.vm;
    CHECK(run_probe(bridge, "zbjniprobe_strings") == 0);
    CHECK(run_probe(bridge, "zbjniprobe_arrays") == 0);
    // A direct buffer Java allocated: its memory is outside the guest space, so the bridge
    // mirrors it into guest memory. The probe checks the mirror is non-null and stable, reads
    // Java's bytes through it (mirror[0] = mirror[1] + mirror[2]) and writes 0x5A at the end.
    static unsigned char foreign[16];
    std::memset(foreign, 0, sizeof foreign);
    foreign[1] = 0x20;
    foreign[2] = 0x03;
    const auto buffer = vm.new_direct_buffer_object(foreign, sizeof foreign);
    CHECK(run_probe(bridge, "zbjniprobe_direct_buffers", buffer) == 0);
    // The guest's writes reached Java when the native call returned.
    CHECK(foreign[0] == 0x23 && foreign[15] == 0x5A);

    // The same buffer again: the mirror is reused and refreshed from Java first.
    foreign[1] = 0x11;
    foreign[2] = 0x11;
    foreign[15] = 0;
    CHECK(run_probe(bridge, "zbjniprobe_direct_buffers", buffer) == 0);
    CHECK(foreign[0] == 0x22 && foreign[15] == 0x5A);
}

bool wait_thread_count(zb::LibraryRuntime& runtime, std::size_t expected) {
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (runtime.guest_thread_count() != expected) {
        if (std::chrono::steady_clock::now() > deadline) return false;
        std::this_thread::sleep_for(5ms);
    }
    return true;
}

void check_natives(Bridge& bridge) {
    MockJvm& vm = *bridge.vm;
    CHECK(run_probe(bridge, "zbjniprobe_register") == 0);
    const auto add = reinterpret_cast<AddFn>(vm.native_function("zb/Natives", "add", "(IFFIFF)I"));
    const auto wide = reinterpret_cast<WideFn>(vm.native_function("zb/Natives", "wide", "(J)V"));
    const auto nest = reinterpret_cast<NestFn>(vm.native_function("zb/Natives", "nest", "(I)I"));
    const auto echo =
        reinterpret_cast<EchoFn>(vm.native_function("zb/Natives", "echo", "(Ljava/lang/String;)Ljava/lang/String;"));
    const auto tid = reinterpret_cast<TidFn>(vm.native_function("zb/Natives", "tid", "()I"));
    CHECK(add != nullptr && wide != nullptr && nest != nullptr && echo != nullptr && tid != nullptr);
    // Slots 0-3 are bound; the failed registration's slot 4 was released and reused by tid.
    CHECK(reinterpret_cast<void*>(add) == zb::native_thunk_address(0));
    CHECK(reinterpret_cast<void*>(tid) == zb::native_thunk_address(4));

    const auto natives = vm.class_object("zb/Natives");
    const auto java_env = vm.thread_env();
    {
        MockJvm::NativeFrame frame(vm, java_env);
        const auto cls = frame.local(natives);
        CHECK(add(java_env, cls, 1, 0.5f, 0.25f, 3, 1.5f, -2.0f) == 1 + 6 + 2 + 2 + 24 - 64);
        wide(java_env, cls, -0x0102030405060708LL);
        CHECK(vm.field_value(natives, "wide").j == -0x0102030405060708LL);
        CHECK(nest(java_env, cls, 3) == 1033);  // Java -> guest -> Java -> guest, three levels deep
        CHECK(tid(java_env, cls) > 0);
        CHECK(frame.close() == 1);
    }

    // Two Java threads at once, each on its own carrier, with nesting and a reference result.
    const std::size_t baseline = bridge.runtime->guest_thread_count();
    std::barrier rendezvous(2);
    std::array<std::int32_t, 2> tids{};
    constexpr int kIterations = 200;
    const auto java_thread = [&](int index) {
        const auto env = vm.thread_env();
        rendezvous.arrive_and_wait();
        for (int i = 0; i < kIterations; ++i) {
            MockJvm::NativeFrame frame(vm, env);
            const auto cls = frame.local(natives);
            CHECK(add(env, cls, index, 1.0f, 0.5f, i, 0.25f, 0.125f) == index + 2 * i + 4 + 4 + 4 + 4);
            CHECK(nest(env, cls, 2 + index) == 1000 + 11 * (2 + index));
            const auto self = frame.local(vm.new_object("zb/Natives"));
            const std::string text = "t" + std::to_string(index) + "-" + std::to_string(i);
            const auto argument = frame.local(vm.new_string_object(to_u16(text)));
            const auto echoed = frame.result(echo(env, self, argument));
            CHECK(vm.string_value(echoed) == to_u16(text + "!"));
            const std::int32_t current = tid(env, cls);
            CHECK(current > 0 && (tids[index] == 0 || tids[index] == current));
            tids[index] = current;
            CHECK(frame.close() == 3);  // cls, self, argument
            CHECK(vm.live_local_refs(env) == 0);
        }
    };
    std::thread first(java_thread, 0);
    std::thread second(java_thread, 1);
    first.join();
    second.join();
    CHECK(tids[0] != tids[1]);
    // Each Java thread leased one carrier; the leases ended with their threads.
    CHECK(wait_thread_count(*bridge.runtime, baseline));

    // The native-call census (runtime_report.h): HostJni::call_native counts every Java ->
    // guest dispatch through this method's thunk slot with one relaxed atomic add, keyed by the
    // name RegisterNatives bound it under ("add", the guest probe's RegisterNatives call has no
    // class name to prefix it with).
    zb::NativeCallCounter& add_counter = zb::runtime_report().native_call_counter("add");
    const std::uint64_t before = add_counter.count.load(std::memory_order_relaxed);
    {
        MockJvm::NativeFrame frame(vm, java_env);
        const auto cls = frame.local(natives);
        CHECK(add(java_env, cls, 1, 0.5f, 0.25f, 3, 1.5f, -2.0f) == 1 + 6 + 2 + 2 + 24 - 64);
        CHECK(frame.close() == 1);
    }
    CHECK(add_counter.count.load(std::memory_order_relaxed) == before + 1);
    CHECK(zb::runtime_report().text().find("native-calls:") != std::string::npos);
}

void check_vm(Bridge& bridge) {
    CHECK(run_probe(bridge, "zbjniprobe_vm") == 0);
    const std::size_t attached = bridge.vm->attached_threads();
    CHECK(run_probe(bridge, "zbjniprobe_attach") == 0);
    CHECK(bridge.vm->attached_threads() == attached);  // the guest worker attached and detached
}

}  // namespace

int main(int argc, char** argv) {
    CHECK(argc == 4);
    check_invalid_handle(argv);
    check_buffer_overflow(argv);
    check_bad_direct_capacity(argv);
    Bridge bridge = start_bridge(argv);
    check_objects(bridge);
    check_values(bridge);
    check_data(bridge);
    check_natives(bridge);
    check_vm(bridge);
    const auto errors = bridge.vm->errors();
    for (const auto& error : errors) std::fprintf(stderr, "mock error: %s\n", error.c_str());
    CHECK(errors.empty());
    std::puts("jni_bridge_test PASS");
    std::fflush(stdout);
    std::_Exit(0);
}
