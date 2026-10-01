#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

#include <dynarmic/interface/A32/a32.h>
#include <dynarmic/interface/A32/config.h>

#include "zb/guest_abi.h"
#include "zb/guest_memory.h"
#include "zb/native_call.h"

namespace Dynarmic {
class ExclusiveMonitor;
}

namespace zb {

class Cp15;

// svc immediate used to return from a host->guest call.
inline constexpr std::uint32_t kHostReturnSwi = 0x5AFFFF;
inline constexpr std::uint32_t kHostReturnAddress = 0xFFFF0F00;
// The host signal that breaks a blocking host syscall so a guest signal can be delivered. It is
// never shown to the guest and never forwarded: its handler does nothing, and returning from it
// leaves the interrupted syscall with EINTR. ZB_INTERRUPT_SIGNAL overrides the default.
int host_interrupt_signal();

// Translated-code cache of one JIT. Carrier JITs only run thread start-up and parking.
inline constexpr std::size_t kDefaultCodeCacheSize = 32 * 1024 * 1024;
inline constexpr std::size_t kCarrierCodeCacheSize = 2 * 1024 * 1024;

struct GuestResult {
    std::uint32_t r0 = 0;
    std::uint32_t r1 = 0;
};

enum class StopKind { None, Svc, MemoryFault, Exception, Interrupted };

struct Stop {
    StopKind kind = StopKind::None;
    std::uint32_t swi = 0;
    std::uint32_t fault_addr = 0;
    bool fault_write = false;
    Dynarmic::A32::Exception exception{};
    // PC after the stop. For Svc it is the instruction after the svc.
    std::uint32_t pc = 0;
};

using GuestStopHandler = std::function<bool(const Stop&)>;

// One guest CPU context on one host thread. Dynarmic callbacks never do host work: they
// record a Stop and halt the JIT; the caller of run() handles it and calls run() again.
class GuestThread final : public Dynarmic::A32::UserCallbacks {
public:
    // precise_faults: a memory fault stops at the faulting instruction with all guest registers
    // committed (needed by guests whose SIGSEGV handlers resume, e.g. Mono). It disables
    // Dynarmic's GetSetElimination, which costs roughly 2x on integer-heavy code.
    GuestThread(GuestMemory& mem, Dynarmic::ExclusiveMonitor* monitor, std::size_t processor_id,
                bool precise_faults = false, std::size_t code_cache_size = kDefaultCodeCacheSize);
    ~GuestThread() override;

    std::array<std::uint32_t, 16>& regs();
    std::array<std::uint32_t, 64>& ext_regs();
    std::uint32_t cpsr() const;
    void set_cpsr(std::uint32_t value);
    std::uint32_t fpscr() const;
    void set_fpscr(std::uint32_t value);
    std::uint32_t tls() const { return tpidruro_; }
    void set_tls(std::uint32_t value) { tpidruro_ = value; }
    std::size_t processor_id() const { return processor_id_; }

    // Runs until a callback stops the JIT, or until post_signal() interrupts it (Interrupted).
    Stop run();
    // Turns a translator failure into a stop the callers already handle; see the definition.
    Stop translator_gave_up(const char* reason);
    // Runs one nested guest function. The stopped CPU state is restored on every return path.
    // The handler runs outside Dynarmic and returns true to resume or false to fail the call.
    std::optional<GuestResult> call(std::uint32_t target, const GuestCall& args,
                                    const GuestStopHandler& handle_stop);
    // Drop translated code for [addr, addr + len). Safe to call from other host threads.
    void invalidate(std::uint32_t addr, std::uint32_t len);

    // Queues a signal for this thread, interrupts its JIT, wakes park() and interrupts a host
    // syscall this thread blocks in. Async-signal-safe.
    void post_signal(const g::siginfo32& info);
    // Takes the lowest-numbered pending signal not in `blocked`; false if there is none.
    bool take_signal(std::uint64_t blocked, g::siginfo32& out);
    bool has_pending_signals(std::uint64_t blocked) const { return (pending_signals_.load() & ~blocked) != 0; }
    std::uint64_t pending_signals() const { return pending_signals_.load(); }

    // Marks the thread as blocked in a host syscall made on the guest's behalf. post_signal()
    // then breaks that syscall with the reserved host signal, so a guest signal reaches a thread
    // asleep in a futex. Without it the thread sleeps until the futex is woken, and a guest that
    // stops its threads with a signal (the Boehm collector of IL2CPP does) never proceeds.
    void begin_blocking();
    void end_blocking();
    bool blocking() const { return blocking_.load() != 0; }
    // Breaks this thread's blocking host syscall, if it is in one. Async-signal-safe.
    void interrupt();

    // Parking for a thread that waits inside a host call (library runtime service and carriers).
    // wake() and post_signal() change the token; park(token) sleeps only while the token is
    // unchanged, so a waiter that reads the token before checking its condition loses no wakeup.
    // park() may return spuriously (host signal, EINTR).
    std::uint32_t park_token() const { return park_word_.load(); }
    void park(std::uint32_t token);
    // Async-signal-safe.
    void wake();

    // Emulated per-thread kernel state.
    std::uint64_t sigmask = 0;
    // A syscall that ended in EINTR and can be restarted. The kernel restarts such a call by
    // itself when the handler that interrupted it was installed with SA_RESTART, and a guest that
    // trusts that does not retry: Unity treats an interrupted semaphore wait as a failure, and
    // mono and FMOD signal often enough that it happens within seconds.
    bool syscall_restartable = false;
    std::uint32_t restart_syscall = 0;
    std::uint32_t restart_pc = 0;                 // the svc instruction itself
    std::array<std::uint32_t, 8> restart_regs{};  // r0-r7 as the call was made
    // The mask a signal frame must record instead of `sigmask`, set while sigsuspend runs with a
    // temporary mask: the handler runs under the temporary mask and sigreturn restores this one.
    std::optional<std::uint64_t> saved_sigmask;
    g::stack32 altstack{0, 2 /* SS_DISABLE */, 0};
    std::uint32_t clear_child_tid = 0;
    int exit_status = 0;
    // Number of host-to-guest calls active on this thread (call() frames).
    int call_depth = 0;
    // Code cache size for threads this thread clones; 0 selects kDefaultCodeCacheSize.
    std::size_t child_code_cache_size = 0;
    // Host tid of the host thread running this guest thread.
    std::int32_t tid = 0;

    std::uint8_t MemoryRead8(std::uint32_t vaddr) override;
    std::uint16_t MemoryRead16(std::uint32_t vaddr) override;
    std::uint32_t MemoryRead32(std::uint32_t vaddr) override;
    std::uint64_t MemoryRead64(std::uint32_t vaddr) override;
    void MemoryWrite8(std::uint32_t vaddr, std::uint8_t value) override;
    void MemoryWrite16(std::uint32_t vaddr, std::uint16_t value) override;
    void MemoryWrite32(std::uint32_t vaddr, std::uint32_t value) override;
    void MemoryWrite64(std::uint32_t vaddr, std::uint64_t value) override;
    bool MemoryWriteExclusive8(std::uint32_t vaddr, std::uint8_t value, std::uint8_t expected) override;
    bool MemoryWriteExclusive16(std::uint32_t vaddr, std::uint16_t value, std::uint16_t expected) override;
    bool MemoryWriteExclusive32(std::uint32_t vaddr, std::uint32_t value, std::uint32_t expected) override;
    bool MemoryWriteExclusive64(std::uint32_t vaddr, std::uint64_t value, std::uint64_t expected) override;
    std::optional<std::uint32_t> MemoryReadCode(std::uint32_t vaddr) override;
    void InterpreterFallback(std::uint32_t pc, std::size_t num_instructions) override;
    void CallSVC(std::uint32_t swi) override;
    void ExceptionRaised(std::uint32_t pc, Dynarmic::A32::Exception exception) override;
    void AddTicks(std::uint64_t) override {}
    std::uint64_t GetTicksRemaining() override { return 0; }

private:
    bool check_access(std::uint32_t vaddr, std::uint32_t len, std::uint8_t need, bool write);
    void halt();

    GuestMemory& mem_;
    std::size_t processor_id_;
    std::uint32_t tpidruro_ = 0;
    std::uint32_t tpidrurw_ = 0;
    std::shared_ptr<Cp15> cp15_;
    std::unique_ptr<Dynarmic::A32::Jit> jit_;
    Stop pending_;
    std::atomic<std::uint64_t> pending_signals_{0};
    std::array<g::siginfo32, 65> pending_info_{};
    // futex word; std::atomic<std::uint32_t> has the layout of std::uint32_t.
    std::atomic<std::uint32_t> park_word_{0};
    std::atomic<std::uint32_t> blocking_{0};
};

}  // namespace zb
