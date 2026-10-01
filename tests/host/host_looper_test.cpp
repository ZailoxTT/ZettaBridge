#include <sys/eventfd.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

#include <dynarmic/interface/exclusive_monitor.h>

#include "check.h"
#include "mock_jvm.h"
#include "mock_looper.h"
#include "zb/android_looper_backend.h"
#include "zb/guest_memory.h"
#include "zb/guest_thread.h"
#include "zb/host_looper.h"
#include "zb/library_runtime.h"
#include "zb/library_protocol.h"
#include "zb/platform_compat_hostcalls.h"
#include "zb/proxy_runtime.h"
#include "zb/runtime_report.h"

namespace {

constexpr std::uint32_t kGuestPage = 0x10000;
constexpr std::uint32_t kGuestStack = kGuestPage + 0x800;

std::uint32_t call(zb::HostLooper& looper, zb::GuestThread& thread, std::uint32_t index,
                   std::uint32_t r0 = 0, std::uint32_t r1 = 0, std::uint32_t r2 = 0,
                   std::uint32_t r3 = 0) {
    thread.regs()[0] = r0;
    thread.regs()[1] = r1;
    thread.regs()[2] = r2;
    thread.regs()[3] = r3;
    thread.regs()[13] = kGuestStack;
    CHECK(looper.handle_host_call(index, thread));
    CHECK(thread.regs()[1] == 0);
    return thread.regs()[0];
}

void guest_u32(zb::GuestMemory& memory, std::uint32_t address, std::uint32_t value) {
    std::uint8_t* target = memory.host_ptr(address, sizeof value, zb::kPageWrite);
    CHECK(target != nullptr);
    std::memcpy(target, &value, sizeof value);
}

std::uint32_t guest_u32(zb::GuestMemory& memory, std::uint32_t address) {
    const std::uint8_t* source = memory.host_ptr(address, sizeof(std::uint32_t), zb::kPageRead);
    CHECK(source != nullptr);
    std::uint32_t value;
    std::memcpy(&value, source, sizeof value);
    return value;
}

std::uint32_t add_fd(zb::HostLooper& looper, zb::GuestThread& thread, zb::GuestMemory& memory,
                     std::uint32_t handle, int fd, int ident, int events,
                     std::uint32_t callback, std::uint32_t data) {
    guest_u32(memory, kGuestStack, callback);
    guest_u32(memory, kGuestStack + 4, data);
    return call(looper, thread, zb::ZB_COMPAT_HC_ALooper_addFd, handle,
                static_cast<std::uint32_t>(fd), static_cast<std::uint32_t>(ident),
                static_cast<std::uint32_t>(events));
}

}  // namespace

void run_unit() {
    zb::LibraryRuntime runtime;
    CHECK(runtime.memory().map_anon(kGuestPage, 0x1000, PROT_READ | PROT_WRITE));
    Dynarmic::ExclusiveMonitor monitor(2);
    zb::GuestThread first(runtime.memory(), &monitor, 0, false, zb::kCarrierCodeCacheSize);
    zb::GuestThread second(runtime.memory(), &monitor, 1, false, zb::kCarrierCodeCacheSize);
    zb::HostLooper looper(runtime);

    CHECK(!looper.handle_host_call(0, first));
    CHECK(call(looper, first, zb::ZB_COMPAT_HC_ALooper_forThread) == 0);
    const std::uint32_t first_handle =
        call(looper, first, zb::ZB_COMPAT_HC_ALooper_prepare, 1);
    CHECK(first_handle != 0);
    CHECK(call(looper, first, zb::ZB_COMPAT_HC_ALooper_forThread) == first_handle);
    CHECK(call(looper, first, zb::ZB_COMPAT_HC_ALooper_prepare) == first_handle);

    const std::uint32_t second_handle =
        call(looper, second, zb::ZB_COMPAT_HC_ALooper_prepare, 0);
    CHECK(second_handle != 0);
    CHECK(second_handle != first_handle);
    CHECK(call(looper, first, zb::ZB_COMPAT_HC_ALooper_acquire, first_handle) == 0);
    CHECK(call(looper, first, zb::ZB_COMPAT_HC_ALooper_release, first_handle) == 0);

    const int fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    CHECK(fd >= 0);
    CHECK(add_fd(looper, first, runtime.memory(), first_handle, fd, 42, 1, 0,
                 0x12345678) == 1);
    // Replacing the same fd must update the identifier and private data.
    CHECK(add_fd(looper, first, runtime.memory(), first_handle, fd, 43, 1, 0,
                 0x87654321) == 1);
    const std::uint64_t one = 1;
    CHECK(write(fd, &one, sizeof one) == static_cast<ssize_t>(sizeof one));

    constexpr std::uint32_t kOutFd = kGuestPage + 0x100;
    constexpr std::uint32_t kOutEvents = kGuestPage + 0x104;
    constexpr std::uint32_t kOutData = kGuestPage + 0x108;
    CHECK(static_cast<std::int32_t>(call(looper, first, zb::ZB_COMPAT_HC_ALooper_pollOnce,
                                         1000, kOutFd, kOutEvents, kOutData)) == 43);
    CHECK(guest_u32(runtime.memory(), kOutFd) == static_cast<std::uint32_t>(fd));
    CHECK(guest_u32(runtime.memory(), kOutEvents) == 1);
    CHECK(guest_u32(runtime.memory(), kOutData) == 0x87654321);
    CHECK(call(looper, first, zb::ZB_COMPAT_HC_ALooper_removeFd, first_handle,
               static_cast<std::uint32_t>(fd)) == 1);
    CHECK(call(looper, first, zb::ZB_COMPAT_HC_ALooper_removeFd, first_handle,
               static_cast<std::uint32_t>(fd)) == 0);

    // removeFd wakes a concurrent poll; consume that wake before checking a true timeout.
    CHECK(static_cast<std::int32_t>(call(looper, first, zb::ZB_COMPAT_HC_ALooper_pollOnce,
                                         0, 0, 0, 0)) == -1);
    CHECK(static_cast<std::int32_t>(call(looper, first, zb::ZB_COMPAT_HC_ALooper_pollOnce,
                                         0, 0, 0, 0)) == -3);
    CHECK(call(looper, first, zb::ZB_COMPAT_HC_ALooper_wake, first_handle) == 0);
    CHECK(static_cast<std::int32_t>(call(looper, first, zb::ZB_COMPAT_HC_ALooper_pollOnce,
                                         0, 0, 0, 0)) == -1);

    CHECK(add_fd(looper, first, runtime.memory(), 0xdeadbeef, fd, 1, 1, 0, 0) ==
          static_cast<std::uint32_t>(-1));
    CHECK(add_fd(looper, first, runtime.memory(), first_handle, -1, 1, 1, 0, 0) ==
          static_cast<std::uint32_t>(-1));
    CHECK(add_fd(looper, second, runtime.memory(), second_handle, fd, 1, 1, 0, 0) ==
          static_cast<std::uint32_t>(-1));
    CHECK(add_fd(looper, first, runtime.memory(), first_handle, fd, -1, 1, 0, 0) ==
          static_cast<std::uint32_t>(-1));

    CHECK(add_fd(looper, first, runtime.memory(), first_handle, fd, 44, 1, 0, 0) == 1);
    CHECK(static_cast<std::int32_t>(call(looper, first, zb::ZB_COMPAT_HC_ALooper_pollOnce,
                                         0, 0xdeadbeef, 0, 0)) == -4);
    CHECK(call(looper, first, zb::ZB_COMPAT_HC_ALooper_removeFd, first_handle,
               static_cast<std::uint32_t>(fd)) == 1);
    CHECK(close(fd) == 0);

    std::puts("host_looper_test PASS");
}

// A borrower is a host thread that entered the guest and returns to Java: its loopers belong to
// the real Android looper of that host thread, not to our poll set.
void run_borrower() {
    zb::LibraryRuntime runtime;
    CHECK(runtime.memory().map_anon(kGuestPage, 0x1000, PROT_READ | PROT_WRITE));
    Dynarmic::ExclusiveMonitor monitor(2);
    zb::GuestThread guest(runtime.memory(), &monitor, 0, false, zb::kCarrierCodeCacheSize);
    zb::GuestThread borrower(runtime.memory(), &monitor, 1, false, zb::kCarrierCodeCacheSize);

    MockAndroidLooper backend;
    struct Invocation {
        std::uint32_t function = 0;
        std::uint32_t fd = 0;
        std::uint32_t events = 0;
        std::uint32_t data = 0;
    };
    std::vector<Invocation> invocations;
    std::uint32_t guest_callback_result = 1;
    bool guest_reachable = true;
    const zb::HostLooper::GuestInvoker invoker =
        [&](std::uint32_t function, const zb::GuestCall& args) -> std::optional<zb::GuestResult> {
        invocations.push_back(Invocation{function, args.regs[0], args.regs[1], args.regs[2]});
        if (!guest_reachable) return std::nullopt;
        zb::GuestResult result;
        result.r0 = guest_callback_result;
        return result;
    };
    const zb::HostLooper::BorrowerProbe probe = [&](const zb::GuestThread& thread) {
        return &thread == &borrower;
    };
    zb::HostLooper looper(runtime, &backend, invoker, probe);

    const int fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    CHECK(fd >= 0);

    // A guest-created thread never touches the backend.
    const std::uint32_t guest_handle = call(looper, guest, zb::ZB_COMPAT_HC_ALooper_prepare, 1);
    CHECK(guest_handle != 0);
    CHECK(backend.prepares() == 0);
    CHECK(add_fd(looper, guest, runtime.memory(), guest_handle, fd, 9, 1, 0x1000, 0x11) == 1);
    CHECK(backend.registrations() == 0);

    // The borrower's looper is the real one.
    const std::uint32_t handle = call(looper, borrower, zb::ZB_COMPAT_HC_ALooper_prepare, 0);
    CHECK(handle != 0);
    CHECK(handle != guest_handle);
    CHECK(backend.prepares() == 1);
    CHECK(backend.last_options() == 0);
    const std::uint64_t real = backend.current();
    CHECK(real != 0);
    CHECK(call(looper, borrower, zb::ZB_COMPAT_HC_ALooper_forThread) == handle);

    CHECK(add_fd(looper, borrower, runtime.memory(), handle, fd, -1, 1, 0x2000, 0xfeed) == 1);
    CHECK(backend.registered(real, fd));
    CHECK(backend.registrations() == 1);

    // acquire/release and wake reach the real looper.
    CHECK(call(looper, borrower, zb::ZB_COMPAT_HC_ALooper_acquire, handle) == 0);
    CHECK(call(looper, borrower, zb::ZB_COMPAT_HC_ALooper_release, handle) == 0);
    CHECK(backend.acquires() == 1);
    CHECK(backend.releases() == 1);
    CHECK(call(looper, borrower, zb::ZB_COMPAT_HC_ALooper_wake, handle) == 0);
    CHECK(backend.wakes() == 1);

    // pollOnce is Java's loop to run: report a wake at once instead of blocking for a second.
    CHECK(static_cast<std::int32_t>(call(looper, borrower, zb::ZB_COMPAT_HC_ALooper_pollOnce,
                                         1000, 0, 0, 0)) == -1);
    CHECK(invocations.empty());

    // An input queue attached to a borrower's looper belongs to the application's own message
    // loop, which knows nothing of the ident the guest waits for. A poll asks the queues instead,
    // and answers with the ident and the guest's own data; without that the guest waits forever
    // and the framework declares the app unresponsive.
    looper.set_input_probe([&](std::uint64_t real_looper) -> std::optional<zb::HostLooper::InputReady> {
        if (real_looper != real) return std::nullopt;
        return zb::HostLooper::InputReady{7, 0xF00D};
    });
    guest_u32(runtime.memory(), kGuestPage + 0x60, 0);
    guest_u32(runtime.memory(), kGuestPage + 0x64, 0);
    CHECK(static_cast<std::int32_t>(call(looper, borrower, zb::ZB_COMPAT_HC_ALooper_pollOnce, 0, 0,
                                         kGuestPage + 0x60, kGuestPage + 0x64)) == 7);
    CHECK(guest_u32(runtime.memory(), kGuestPage + 0x60) == 1);       // ALOOPER_EVENT_INPUT
    CHECK(guest_u32(runtime.memory(), kGuestPage + 0x64) == 0xF00D);  // the guest's own data
    looper.set_input_probe({});
    CHECK(static_cast<std::int32_t>(call(looper, borrower, zb::ZB_COMPAT_HC_ALooper_pollOnce,
                                         1000, 0, 0, 0)) == -1);

    // A host callback delivered by the real looper runs the guest callback with (fd, events, data).
    CHECK(backend.deliver(real, fd, 1) == 1);
    CHECK(invocations.size() == 1);
    CHECK(invocations[0].function == 0x2000);
    CHECK(invocations[0].fd == static_cast<std::uint32_t>(fd));
    CHECK(invocations[0].events == 1);
    CHECK(invocations[0].data == 0xfeed);
    CHECK(backend.registered(real, fd));

    // A guest callback returning 0 unregisters the fd on both sides.
    guest_callback_result = 0;
    CHECK(backend.deliver(real, fd, 1) == 0);
    CHECK(invocations.size() == 2);
    CHECK(!backend.registered(real, fd));
    CHECK(call(looper, borrower, zb::ZB_COMPAT_HC_ALooper_removeFd, handle,
               static_cast<std::uint32_t>(fd)) == 0);

    // An unreachable guest unregisters too, instead of spinning the host thread's loop.
    guest_callback_result = 1;
    guest_reachable = false;
    CHECK(add_fd(looper, borrower, runtime.memory(), handle, fd, -1, 1, 0x2000, 0xfeed) == 1);
    CHECK(backend.deliver(real, fd, 1) == 0);
    CHECK(!backend.registered(real, fd));
    guest_reachable = true;

    // A backend that refuses the fd fails the guest call and leaves nothing behind.
    backend.fail_add_fd = true;
    CHECK(add_fd(looper, borrower, runtime.memory(), handle, fd, -1, 1, 0x2000, 0xfeed) ==
          static_cast<std::uint32_t>(-1));
    backend.fail_add_fd = false;
    CHECK(call(looper, borrower, zb::ZB_COMPAT_HC_ALooper_removeFd, handle,
               static_cast<std::uint32_t>(fd)) == 0);

    const std::string report = zb::runtime_report().text();
    CHECK(report.find("looper-attached: 1") != std::string::npos);
    CHECK(report.find("looper-host-callbacks: fired=3 guest=2 failed=1") != std::string::npos);
    CHECK(close(fd) == 0);

    std::puts("host_looper_test borrower PASS");
}

// The third mode: a guest thread that attaches an input queue. It is a real host thread, so it
// can own a real Android looper, and from then on its registrations live there and its pollOnce
// is the real one - unlike a borrower, which returns to Java and never polls.
void run_real_looper() {
    zb::LibraryRuntime runtime;
    CHECK(runtime.memory().map_anon(kGuestPage, 0x1000, PROT_READ | PROT_WRITE));
    Dynarmic::ExclusiveMonitor monitor(1);
    zb::GuestThread guest(runtime.memory(), &monitor, 0, false, zb::kCarrierCodeCacheSize);

    MockAndroidLooper backend;
    std::vector<std::uint32_t> invoked;
    const zb::HostLooper::GuestInvoker invoker =
        [&](std::uint32_t function, const zb::GuestCall& args) -> std::optional<zb::GuestResult> {
        (void)args;
        invoked.push_back(function);
        zb::GuestResult result;
        result.r0 = 1;
        return result;
    };
    // No thread is a borrower here.
    const zb::HostLooper::BorrowerProbe probe = [](const zb::GuestThread&) { return false; };
    zb::HostLooper looper(runtime, &backend, invoker, probe);

    const int fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    CHECK(fd >= 0);

    // Before the queue: an ordinary guest looper, polled by us, with the backend untouched.
    const std::uint32_t handle = call(looper, guest, zb::ZB_COMPAT_HC_ALooper_prepare, 0);
    CHECK(handle != 0);
    CHECK(backend.prepares() == 0);
    CHECK(add_fd(looper, guest, runtime.memory(), handle, fd, -1, 1, 0x3000, 0xabc) == 1);
    CHECK(backend.registrations() == 0);

    // Attaching an input queue promotes the thread, and what it already watched moves across.
    std::uint64_t promoted = 0;
    CHECK(looper.ensure_real_looper(guest, 0, promoted));
    CHECK(backend.prepares() == 1);
    const std::uint64_t real = backend.current();
    CHECK(real != 0 && backend.registered(real, fd));
    // Asking twice changes nothing: the thread already has its looper.
    CHECK(looper.ensure_real_looper(guest, 0, promoted));
    CHECK(backend.prepares() == 1);

    // From now on registrations go straight to the real looper.
    const int second = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    CHECK(second >= 0);
    CHECK(add_fd(looper, guest, runtime.memory(), handle, second, -1, 1, 0x4000, 0xdef) == 1);
    CHECK(backend.registered(real, second));

    // pollOnce is the real one: the timeout is passed through and a ready fd runs the guest
    // callback from inside that call, on this same thread.
    backend.poll_result = -3;  // ALOOPER_POLL_TIMEOUT
    CHECK(static_cast<std::int32_t>(call(looper, guest, zb::ZB_COMPAT_HC_ALooper_pollOnce, 250, 0, 0, 0)) == -3);
    CHECK(backend.polls() == 1 && backend.last_timeout() == 250);
    CHECK(invoked.empty());

    backend.make_ready(second);
    CHECK(static_cast<std::int32_t>(call(looper, guest, zb::ZB_COMPAT_HC_ALooper_pollOnce, -1, 0, 0, 0)) == -2);
    CHECK(backend.polls() == 2 && backend.last_timeout() == -1);
    CHECK(invoked.size() == 1 && invoked[0] == 0x4000);

    // What the ready registration carried must reach the guest: a source attached by ident is
    // recognized only by the data that comes back, and dropping it left Unity polling forever
    // while the framework waited for the touch it had already delivered.
    backend.ready_fd = 11;
    backend.ready_events = 1;
    backend.ready_data = reinterpret_cast<void*>(static_cast<std::uintptr_t>(0xBEEF));
    backend.poll_result = 3;  // an ident registration
    guest_u32(runtime.memory(), kGuestPage + 0x40, 0);
    guest_u32(runtime.memory(), kGuestPage + 0x44, 0);
    guest_u32(runtime.memory(), kGuestPage + 0x48, 0);
    CHECK(static_cast<std::int32_t>(call(looper, guest, zb::ZB_COMPAT_HC_ALooper_pollOnce, 0,
                                         kGuestPage + 0x40, kGuestPage + 0x44, kGuestPage + 0x48)) == 3);
    CHECK(guest_u32(runtime.memory(), kGuestPage + 0x40) == 11);
    CHECK(guest_u32(runtime.memory(), kGuestPage + 0x44) == 1);
    CHECK(guest_u32(runtime.memory(), kGuestPage + 0x48) == 0xBEEF);
    backend.poll_result = -3;

    // wake and removeFd reach the real looper too.
    CHECK(call(looper, guest, zb::ZB_COMPAT_HC_ALooper_wake, handle) == 0);
    CHECK(backend.wakes() == 1);
    CHECK(call(looper, guest, zb::ZB_COMPAT_HC_ALooper_removeFd, handle,
               static_cast<std::uint32_t>(second)) == 1);
    CHECK(!backend.registered(real, second));

    CHECK(close(fd) == 0 && close(second) == 0);
    std::puts("host_looper_test real-looper PASS");
}

void run_guest(int argc, char** argv) {
    CHECK(argc == 4);
    auto* vm = new zb::mock::MockJvm();
    auto* engine = new zb::GuestJniEngine(*vm);
    zb::LibraryRuntimeOptions options;
    options.sysroot = argv[1];
    options.zbhost = argv[2];
    options.target_sdk = 16;
    options.guest_environment = {std::string("LD_LIBRARY_PATH=") + argv[3]};
    options.preload = "libzbjni.so";
    std::string error;
    CHECK(engine->start(options, error));

    zb::LibraryRuntime& runtime = engine->runtime();
    const std::uint32_t library = runtime.load_library(
        std::string(argv[3]) + "/libzblooperprobe.so", ZB_GUEST_RTLD_NOW, error);
    if (library == 0) std::fprintf(stderr, "looper probe load failed: %s\n", error.c_str());
    CHECK(library != 0);
    const std::uint32_t probe = runtime.find_symbol(library, "zb_looper_probe", error);
    CHECK(probe != 0);

    // The probe calls ALooper_prepare, addFd/removeFd and pollOnce (twice with a registered
    // callback: one that keeps itself registered, one that unregisters). The runtime report
    // must observe all of that as "looper-*" diagnostics.
    const std::string before = zb::runtime_report().text();
    CHECK(before.find("looper-loopers:") == std::string::npos);

    const auto result = runtime.call_on_service(probe, zb::GuestCall{});
    CHECK(result.has_value());
    if (result->r0 != 0) std::fprintf(stderr, "looper probe failed at guest line %u\n", result->r0);
    CHECK(result->r0 == 0);

    const std::string after = zb::runtime_report().text();
    CHECK(after.find("looper-loopers: 1") != std::string::npos);
    CHECK(after.find("looper-fds: added=2 removed=1") != std::string::npos);
    CHECK(after.find("looper-callbacks: dispatched=2 unregistered=1") != std::string::npos);
    CHECK(after.find("looper-polls: total=2") != std::string::npos);
    CHECK(after.find("looper-last-0: ") != std::string::npos);
    CHECK(after.find("result=callback") != std::string::npos);

    std::puts("host_looper_test guest PASS");
    std::fflush(stdout);
    std::_Exit(0);
}

int main(int argc, char** argv) {
    if (argc == 1) {
        run_unit();
        run_borrower();
        run_real_looper();
        return 0;
    }
    run_guest(argc, argv);
}
