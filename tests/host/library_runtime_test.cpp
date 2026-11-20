// Library-mode runtime: guest RTLD flags, dlopen/dlsym/dlerror on the service thread and on a
// carrier, every return type, host-call chaining, misuse guards, preload failure, retirement of
// the process signal target, signals to parked threads, carrier identity and state inheritance,
// tgkill redirection, concurrent borrowers under bionic contention, and carrier cleanup.
// Usage: library_runtime_test <sysroot> <zbhost> <libzbcallprobe.so>
//        library_runtime_test --signal-target-retirement
#include <signal.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <functional>
#include <future>
#include <string>
#include <thread>
#include <vector>

#include <dynarmic/interface/exclusive_monitor.h>

#include "check.h"
#include "zb/library_runtime.h"
#include "zb/native_call.h"

namespace {

// The wrapper executes in a signal handler: only always-lock-free atomics are allowed.
static_assert(std::atomic<bool>::is_always_lock_free);
std::atomic<bool> hold_signal_reader{false};
std::atomic<bool> signal_reader_entered{false};
std::atomic<bool> release_signal_reader{false};
// Acquire-order stress: a forwarding reader must never use a target whose retirement completed.
std::atomic<bool> stress_active{false};
std::atomic<bool> stress_target_retired{true};
std::atomic<bool> stress_violation{false};
std::atomic<std::uint64_t> stress_posts{0};
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);

}  // namespace

extern "C" void real_post_signal(zb::GuestThread*, const zb::g::siginfo32&)
    asm("__real__ZN2zb11GuestThread11post_signalERKNS_1g9siginfo32E");
extern "C" void wrapped_post_signal(zb::GuestThread*, const zb::g::siginfo32&)
    asm("__wrap__ZN2zb11GuestThread11post_signalERKNS_1g9siginfo32E");

extern "C" void wrapped_post_signal(zb::GuestThread* thread, const zb::g::siginfo32& info) {
    if (hold_signal_reader.load() && info.si_signo == SIGALRM) {
        signal_reader_entered.store(true);
        while (!release_signal_reader.load()) {
        }
    }
    if (stress_active.load() && info.si_signo == SIGUSR1) {
        if (stress_target_retired.load()) stress_violation.store(true);
        stress_posts.fetch_add(1);
    }
    real_post_signal(thread, info);
}

namespace {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
using GuestInvoke = std::function<std::optional<zb::GuestResult>(std::uint32_t function, const zb::GuestCall& args)>;

void check_borrower_syscall_interrupt() {
    zb::GuestMemory memory;
    Dynarmic::ExclusiveMonitor monitor(1);
    zb::GuestThread borrower(memory, &monitor, 0);
    // The guest's identity belongs to the carrier, but its syscall runs on the borrower.
    borrower.tid = static_cast<std::int32_t>(::syscall(SYS_gettid));
    const int wake_signal = zb::host_interrupt_signal();
    sigset_t wake_set, original_mask;
    sigemptyset(&wake_set);
    sigaddset(&wake_set, wake_signal);
    CHECK(pthread_sigmask(SIG_BLOCK, &wake_set, &original_mask) == 0);

    std::promise<void> blocking;
    std::thread owner([&] {
        borrower.begin_blocking();
        blocking.set_value();
        const timespec timeout{1, 0};
        const int received = sigtimedwait(&wake_set, nullptr, &timeout);
        borrower.end_blocking();
        CHECK(received == wake_signal);
    });
    blocking.get_future().wait();
    zb::g::siginfo32 info{};
    info.si_signo = SIGUSR1;
    borrower.post_signal(info);
    owner.join();

    sigset_t pending;
    CHECK(sigpending(&pending) == 0);
    CHECK(sigismember(&pending, wake_signal) == 0);
    CHECK(pthread_sigmask(SIG_SETMASK, &original_mask, nullptr) == 0);
    CHECK(borrower.take_signal(0, info) && info.si_signo == SIGUSR1);
}

void check_signal_target_retirement() {
    zb::GuestMemory memory;
    Dynarmic::ExclusiveMonitor monitor(1);
    zb::GuestThread target(memory, &monitor, 0);
    zb::Process::install_host_signal_forwarding();
    zb::Process::set_process_signal_target(&target);
    hold_signal_reader.store(true);
    std::thread sender([] { CHECK(raise(SIGALRM) == 0); });
    const auto deadline = Clock::now() + 2s;
    while (!signal_reader_entered.load()) {
        CHECK(Clock::now() < deadline);
        std::this_thread::yield();
    }

    // The handler has acquired target but has not used it yet. Retirement must wait.
    std::promise<void> started;
    auto retiring = std::async(std::launch::async, [&] {
        started.set_value();
        zb::Process::clear_process_signal_target(&target);
    });
    started.get_future().wait();
    CHECK(retiring.wait_for(100ms) == std::future_status::timeout);
    release_signal_reader.store(true);
    sender.join();
    CHECK(retiring.wait_for(2s) == std::future_status::ready);
    retiring.get();
    hold_signal_reader.store(false);
    zb::g::siginfo32 info;
    CHECK(target.take_signal(0, info) && info.si_signo == SIGALRM);
    // After quiescence, new handlers cannot acquire the retired target.
    CHECK(raise(SIGALRM) == 0);
    CHECK(!target.take_signal(0, info));
}

// The held reader above is already counted when it blocks inside post_signal, so it cannot tell
// whether a handler registers before or after loading the target. Here readers on several threads
// race a publish/retire loop. stress_target_retired is cleared before each publication and set only
// after clear_process_signal_target() returns, so with correct ordering no post_signal can observe
// it set. A handler that loads the target before registering can run its post_signal after the
// retirement already finished; the loop stops at the first such violation.
void check_signal_target_acquire_order() {
    zb::GuestMemory memory;
    Dynarmic::ExclusiveMonitor monitor(1);
    zb::GuestThread target(memory, &monitor, 0);
    zb::Process::install_host_signal_forwarding();
    stress_violation.store(false);
    stress_posts.store(0);
    stress_target_retired.store(true);
    stress_active.store(true);
    const unsigned cpus = std::thread::hardware_concurrency();
    const unsigned readers = cpus > 2 ? (cpus - 1 < 4 ? cpus - 1 : 4) : 2;
    std::vector<std::thread> senders;
    std::atomic<bool> stop{false};
    for (unsigned i = 0; i < readers; ++i) {
        senders.emplace_back([&] {
            while (!stop.load()) CHECK(raise(SIGUSR1) == 0);
        });
    }
    const auto deadline = Clock::now() + 1s;
    while (!stress_violation.load() && Clock::now() < deadline) {
        for (int i = 0; i < 1000; ++i) {
            stress_target_retired.store(false);
            zb::Process::set_process_signal_target(&target);
            zb::Process::clear_process_signal_target(&target);
            stress_target_retired.store(true);
        }
    }
    stop.store(true);
    for (auto& sender : senders) sender.join();
    stress_active.store(false);
    std::fprintf(stderr, "signal target stress: %u readers, %llu posts\n", readers,
                 static_cast<unsigned long long>(stress_posts.load()));
    CHECK(!stress_violation.load());
    CHECK(stress_posts.load() > 0);
}

std::uint32_t fbits(float value) {
    std::uint32_t bits;
    std::memcpy(&bits, &value, sizeof bits);
    return bits;
}

std::uint64_t dbits(double value) {
    std::uint64_t bits;
    std::memcpy(&bits, &value, sizeof bits);
    return bits;
}

double elapsed_ms(Clock::time_point from, Clock::time_point to) {
    return std::chrono::duration<double, std::milli>(to - from).count();
}

zb::GuestCall args1(std::uint32_t r0) {
    zb::GuestCall call;
    call.regs = {r0, 0, 0, 0};
    return call;
}

std::uint32_t read32(zb::LibraryRuntime& runtime, std::uint32_t address) {
    const std::uint8_t* source = runtime.memory().host_ptr(address, 4, zb::kPageRead);
    CHECK(source != nullptr);
    std::uint32_t value;
    std::memcpy(&value, source, sizeof value);
    return value;
}

// Polls a guest word without running guest code on this host thread.
bool wait_nonzero(zb::LibraryRuntime& runtime, std::uint32_t address, std::chrono::milliseconds timeout) {
    const auto deadline = Clock::now() + timeout;
    while (read32(runtime, address) == 0) {
        if (Clock::now() > deadline) return false;
        std::this_thread::sleep_for(5ms);
    }
    return true;
}

// Released carriers exit through bionic asynchronously.
bool wait_thread_count(zb::LibraryRuntime& runtime, std::size_t expected, std::chrono::milliseconds timeout) {
    const auto deadline = Clock::now() + timeout;
    while (runtime.guest_thread_count() != expected) {
        if (Clock::now() > deadline) return false;
        std::this_thread::sleep_for(5ms);
    }
    return true;
}

zb::LibraryRuntimeOptions runtime_options(char** argv) {
    zb::LibraryRuntimeOptions options;
    options.sysroot = argv[1];
    options.zbhost = argv[2];
    options.target_sdk = 16;
    options.guest_environment = {"LD_LIBRARY_PATH=" + std::filesystem::path(argv[3]).parent_path().string()};
    return options;
}

// start() reports a failed preload; runs in a child because a runtime is process-lifetime.
void check_preload_failure(char** argv) {
    std::fflush(stdout);
    const pid_t child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        {
            zb::LibraryRuntime runtime;
            zb::LibraryRuntimeOptions options = runtime_options(argv);
            options.preload = "libzb-does-not-exist.so";
            std::string error;
            if (runtime.start(options, error)) std::_Exit(10);
            std::fprintf(stderr, "expected start error: %s\n", error.c_str());
            CHECK(error.find("status 4") != std::string::npos);
        }
        // Ordinary failed-start destruction must retire the process signal target.
        CHECK(raise(SIGALRM) == 0);
        std::_Exit(0);
    }
    int status = 0;
    CHECK(waitpid(child, &status, 0) == child);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

// Every JNI return type through store_native_result.
void check_returns(const GuestInvoke& invoke, const std::function<std::uint32_t(const char*)>& symbol) {
    struct ReturnCase {
        const char* name;
        char type;
        std::uint64_t x0;
        std::uint64_t d0;
    };
    const ReturnCase returns[] = {
        {"zb_return_z", 'Z', 1, 0},
        {"zb_return_b", 'B', static_cast<std::uint64_t>(-2), 0},
        {"zb_return_c", 'C', 0x1234, 0},
        {"zb_return_s", 'S', static_cast<std::uint64_t>(-3), 0},
        {"zb_return_i", 'I', 42, 0},
        {"zb_return_j", 'J', 0x1122334455667788ull, 0},
        {"zb_return_f", 'F', 0, fbits(3.5f)},
        {"zb_return_d", 'D', 0, dbits(-1.25)},
        {"zb_return_l", 'L', 0x7000012345ull, 0},
    };
    CHECK(invoke(symbol("zb_return_v"), zb::GuestCall{}));
    for (const ReturnCase& expected : returns) {
        const auto result = invoke(symbol(expected.name), zb::GuestCall{});
        CHECK(result);
        zb::NativeRegs regs{};
        zb::store_native_result(expected.type, result->r0, result->r1, regs,
                                [](std::uint32_t handle) { return 0x7000000000ull | handle; });
        if (expected.type == 'F' || expected.type == 'D') {
            CHECK(regs.d[0] == expected.d0);
        } else {
            CHECK(regs.x[0] == expected.x0);
        }
    }
}

// softfp argument layout of a mixed JNI signature.
void check_mixed_arguments(const GuestInvoke& invoke, std::uint32_t mix) {
    std::uint64_t host_stack[1] = {0x99};
    zb::NativeRegs host{};
    host.x[1] = 0x77;
    host.x[2] = 1;
    host.x[3] = static_cast<std::uint64_t>(-2);
    host.x[4] = 0x1234;
    host.x[5] = static_cast<std::uint64_t>(-3);
    host.x[6] = 4;
    host.x[7] = 0x1122334455667788ull;
    host.d[0] = fbits(1.5f);
    host.d[1] = dbits(-2.25);
    host.stack = host_stack;
    const auto to_handle = [](std::uint64_t reference) {
        return reference == 0 ? 0u : static_cast<std::uint32_t>(reference + 0x1000);
    };
    const zb::GuestCall mixed = zb::marshal_native_args("IZBCSIJFDL", host, 0xE000, to_handle);
    const auto result = invoke(mix, mixed);
    CHECK(result && result->r0 == 42);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::string(argv[1]) == "--signal-target-retirement") {
        check_signal_target_retirement();
        check_signal_target_acquire_order();
        return 0;
    }
    CHECK(argc == 4);
    const std::string probe = argv[3];
    const std::string libdir = std::filesystem::path(probe).parent_path();
    check_borrower_syscall_interrupt();
    check_preload_failure(argv);
    check_signal_target_retirement();
    check_signal_target_acquire_order();

    CHECK(ZB_GUEST_RTLD_NOW == 0u && ZB_GUEST_RTLD_LAZY == 1u && ZB_GUEST_RTLD_GLOBAL == 2u);
    CHECK(ZB_GUEST_RTLD_NOLOAD == 4u && ZB_GUEST_RTLD_NODELETE == 0x1000u && ZB_GUEST_RTLD_DEFAULT == 0xFFFFFFFFu);

    zb::LibraryRuntime runtime;
    std::atomic<std::uint32_t> chained_function{0};
    std::atomic<int> chained_calls{0};
    std::atomic<bool> chained_on_service{true};
    runtime.set_host_call_handler([&](std::uint32_t index, zb::GuestThread& thread) {
        if (index != 0xFD00) return false;
        ++chained_calls;
        std::string misuse;
        CHECK(!runtime.borrow(misuse));
        CHECK(misuse.find("call_on_current") != std::string::npos);
        if (chained_on_service) {
            misuse.clear();
            CHECK(runtime.load_library(argv[3], ZB_GUEST_RTLD_NOW, misuse) == 0);
            CHECK(misuse.find("service thread") != std::string::npos);
        }
        const auto nested = runtime.call_on_current(chained_function, zb::GuestCall{});
        CHECK(nested && nested->r0 == 42);
        thread.regs()[0] += nested->r0;
        return true;
    });

    std::string error;
    CHECK(runtime.start(runtime_options(argv), error));
    CHECK(runtime.guest_thread_count() == 1);
    CHECK(runtime.service_api().version == ZB_SERVICE_PROTOCOL_VERSION);
    CHECK(!runtime.call_on_current(runtime.service_api().malloc_fn, zb::GuestCall{}));

    // Loader on the service thread.
    CHECK(runtime.load_library(libdir + "/does-not-exist.so", ZB_GUEST_RTLD_NOW, error) == 0);
    CHECK(error.find("does-not-exist.so") != std::string::npos);
    error.clear();
    CHECK(runtime.load_library(probe, ZB_GUEST_RTLD_NOW | ZB_GUEST_RTLD_NOLOAD, error) == 0);
    error.clear();
    const std::uint32_t library = runtime.load_library(probe, ZB_GUEST_RTLD_NOW | ZB_GUEST_RTLD_GLOBAL, error);
    CHECK(library != 0 && error.empty());
    CHECK(runtime.load_library(probe, ZB_GUEST_RTLD_NOW | ZB_GUEST_RTLD_NOLOAD, error) == library);
    CHECK(runtime.find_symbol(library, "does_not_exist", error) == 0);
    CHECK(error.find("does_not_exist") != std::string::npos);

    const auto symbol = [&](const char* name) {
        std::string symbol_error;
        const std::uint32_t address = runtime.find_symbol(library, name, symbol_error);
        CHECK(address != 0 && symbol_error.empty());
        return address;
    };
    error.clear();
    CHECK(runtime.find_symbol(ZB_GUEST_RTLD_DEFAULT, "zb_return_i", error) == symbol("zb_return_i"));

    const GuestInvoke on_service = [&](std::uint32_t function, const zb::GuestCall& args) {
        return runtime.call_on_service(function, args);
    };
    check_returns(on_service, symbol);
    check_mixed_arguments(on_service, symbol("zb_probe_mix"));

    // Host calls outside the runtime range reach the chained handler, which may nest a call.
    chained_function = symbol("zb_return_i");
    const std::uint32_t host_call = symbol("zb_probe_host_call");
    const auto chained = runtime.call_on_service(host_call, args1(100));
    CHECK(chained && chained->r0 == 142 && chained_calls == 1);

    // A process-directed SIGALRM reaches the service thread while it is parked in READY.
    const std::uint32_t alarm_count = symbol("zb_alarm_count");
    const std::uint32_t alarm_tid = symbol("zb_alarm_tid");
    const std::uint32_t arm_alarm = symbol("zb_probe_arm_alarm");
    const std::uint32_t tid_function = symbol("zb_probe_tid");
    const std::uint32_t tls_function = symbol("zb_probe_tls");
    const auto service_tid = runtime.call_on_service(tid_function, zb::GuestCall{});
    const auto service_tls = runtime.call_on_service(tls_function, zb::GuestCall{});
    CHECK(service_tid && service_tid->r0 != 0 && service_tls && service_tls->r0 != 0);
    const auto armed = runtime.call_on_service(arm_alarm, args1(20000));
    CHECK(armed && armed->r0 == 0);
    CHECK(wait_nonzero(runtime, alarm_count, 2000ms));
    CHECK(read32(runtime, alarm_tid) == service_tid->r0);
    const auto after_alarm = runtime.call_on_service(symbol("zb_return_i"), zb::GuestCall{});
    CHECK(after_alarm && after_alarm->r0 == 42);

    // First borrow: latency, identity, loader, returns and arguments on the carrier.
    const std::uint32_t return_i = symbol("zb_return_i");
    const auto borrow_start = Clock::now();
    auto first = runtime.borrow(error);
    CHECK(first && error.empty());
    const auto borrowed = Clock::now();
    const auto cold = first->call(return_i, zb::GuestCall{});
    const auto cold_done = Clock::now();
    const auto warm = first->call(return_i, zb::GuestCall{});
    const auto warm_done = Clock::now();
    CHECK(cold && cold->r0 == 42 && warm && warm->r0 == 42);
    std::printf("carrier latency: borrow %.2f ms, first call %.2f ms, warm call %.3f ms\n",
                elapsed_ms(borrow_start, borrowed), elapsed_ms(borrowed, cold_done), elapsed_ms(cold_done, warm_done));
    CHECK(runtime.guest_thread_count() == 2);

    const auto tid = first->call(tid_function, zb::GuestCall{});
    const auto cached_tid = first->call(symbol("zb_probe_cached_tid"), zb::GuestCall{});
    const auto tls = first->call(tls_function, zb::GuestCall{});
    CHECK(tid && cached_tid && tls);
    CHECK(static_cast<std::int32_t>(tid->r0) == first->guest_tid() && cached_tid->r0 == tid->r0);
    CHECK(tls->r0 == first->guest_tls() && tls->r0 != service_tls->r0 && tid->r0 != service_tid->r0);

    error.clear();
    CHECK(first->load_library(libdir + "/does-not-exist.so", ZB_GUEST_RTLD_NOW, error) == 0);
    CHECK(error.find("does-not-exist.so") != std::string::npos);
    error.clear();
    CHECK(first->load_library(probe, ZB_GUEST_RTLD_NOW | ZB_GUEST_RTLD_NOLOAD, error) == library && error.empty());
    CHECK(first->find_symbol(library, "zb_return_i", error) == return_i && error.empty());
    CHECK(first->find_symbol(library, "does_not_exist", error) == 0);
    CHECK(error.find("does_not_exist") != std::string::npos);

    const GuestInvoke on_first = [&](std::uint32_t function, const zb::GuestCall& args) {
        return first->call(function, args);
    };
    check_returns(on_first, symbol);
    check_mixed_arguments(on_first, symbol("zb_probe_mix"));

    chained_on_service = false;
    const auto chained_on_carrier = first->call(host_call, args1(200));
    CHECK(chained_on_carrier && chained_on_carrier->r0 == 242 && chained_calls == 2);

    // tgkill to the borrowed carrier's tid is delivered on the borrower during the call.
    const auto installed = first->call(symbol("zb_probe_install_usr1"), zb::GuestCall{});
    CHECK(installed && installed->r0 == 0);
    const auto killed = first->call(symbol("zb_probe_tgkill_self"), args1(SIGUSR1));
    CHECK(killed && killed->r0 == 0);
    CHECK(read32(runtime, symbol("zb_usr1_count")) == 1);
    CHECK(read32(runtime, symbol("zb_usr1_tid")) == tid->r0);
    CHECK(read32(runtime, symbol("zb_usr1_tls")) == tls->r0);

    // setitimer from a borrower: SIGALRM is process-directed and reaches the parked service.
    const auto armed_on_carrier = first->call(arm_alarm, args1(20000));
    CHECK(armed_on_carrier && armed_on_carrier->r0 == 0);
    CHECK(wait_nonzero(runtime, alarm_count, 2000ms));
    CHECK(read32(runtime, alarm_tid) == service_tid->r0);

    // A carrier spawned from the service thread inherits its signal mask (clone), and the
    // borrower inherits the carrier's mask and bionic's alternate signal stack.
    const std::uint32_t block_signal = symbol("zb_probe_block_signal");
    const std::uint32_t signal_blocked = symbol("zb_probe_signal_blocked");
    zb::GuestCall block_usr2;
    block_usr2.regs = {static_cast<std::uint32_t>(SIGUSR2), 1, 0, 0};
    CHECK(runtime.call_on_service(block_signal, block_usr2));
    auto inherited = runtime.borrow(error);
    CHECK(inherited);
    const auto blocked = inherited->call(signal_blocked, args1(SIGUSR2));
    const auto altstack = inherited->call(symbol("zb_probe_altstack_sp"), zb::GuestCall{});
    CHECK(blocked && blocked->r0 == 1);
    CHECK(altstack && altstack->r0 != 0);
    block_usr2.regs[1] = 0;
    CHECK(runtime.call_on_service(block_signal, block_usr2));
    const auto service_blocked = runtime.call_on_service(signal_blocked, args1(SIGUSR2));
    CHECK(service_blocked && service_blocked->r0 == 0);
    CHECK(runtime.guest_thread_count() == 3);

    // Released carriers leave PARK and exit.
    inherited.reset();
    first.reset();
    CHECK(wait_thread_count(runtime, 1, 5000ms));

    // Two host threads borrow at once and run bionic code concurrently.
    const std::uint32_t overlap = symbol("zb_probe_overlap");
    const std::uint32_t contention = symbol("zb_probe_contention");
    constexpr std::uint32_t kIterations = 20000;
    std::barrier rendezvous(2);
    std::array<std::uint32_t, 2> worker_tls{};
    std::array<std::uint32_t, 2> worker_tids{};
    std::array<std::uint32_t, 2> tickets{};
    const auto worker = [&](std::size_t index) {
        std::string borrow_error;
        auto carrier = runtime.borrow(borrow_error);
        CHECK(carrier && borrow_error.empty());
        const auto worker_tid = carrier->call(tid_function, zb::GuestCall{});
        const auto own_tls = carrier->call(tls_function, zb::GuestCall{});
        CHECK(worker_tid && static_cast<std::int32_t>(worker_tid->r0) == carrier->guest_tid());
        CHECK(own_tls && own_tls->r0 == carrier->guest_tls());
        worker_tids[index] = worker_tid->r0;
        worker_tls[index] = own_tls->r0;
        rendezvous.arrive_and_wait();
        const auto ticket = carrier->call(overlap, zb::GuestCall{});
        CHECK(ticket && ticket->r0 != 0xFFFFFFFFu);
        tickets[index] = ticket->r0;
        const auto contended = carrier->call(contention, args1(kIterations));
        CHECK(contended && contended->r0 == 0);
        const auto tid_after = carrier->call(tid_function, zb::GuestCall{});
        CHECK(tid_after && tid_after->r0 == worker_tids[index]);
    };
    std::thread first_worker(worker, 0);
    std::thread second_worker(worker, 1);
    first_worker.join();
    second_worker.join();
    CHECK(worker_tls[0] != 0 && worker_tls[1] != 0 && worker_tls[0] != worker_tls[1]);
    CHECK(worker_tids[0] != worker_tids[1]);
    CHECK(tickets[0] != tickets[1]);
    CHECK(read32(runtime, symbol("zb_contention_total")) == 2 * kIterations);
    CHECK(wait_thread_count(runtime, 1, 5000ms));

    // The carrier path stays reusable.
    auto later = runtime.borrow(error);
    CHECK(later);
    const auto later_result = later->call(return_i, zb::GuestCall{});
    CHECK(later_result && later_result->r0 == 42);
    later.reset();
    CHECK(wait_thread_count(runtime, 1, 5000ms));

    std::puts("library_runtime_test PASS");
    std::fflush(stdout);
    std::_Exit(0);
}
