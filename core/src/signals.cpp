// Guest signal delivery: frames, sigreturn, faults turned into signals, and forwarding of
// asynchronous host signals to the guest thread they arrive on.
#include <sched.h>
#include <signal.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <cstdlib>
#include <cstring>

#include "zb/log.h"
#include "zb/process.h"
#include "zb/runtime_report.h"
#include "zb/syscalls.h"

namespace zb {

namespace {

// Kernel sigaction flags (identical on arm and arm64).
constexpr std::uint32_t kSaSiginfo = 0x00000004;
constexpr std::uint32_t kSaRestorer = 0x04000000;
constexpr std::uint32_t kSaOnstack = 0x08000000;
constexpr std::uint32_t kSaRestart = 0x10000000;
constexpr std::uint32_t kSaNodefer = 0x40000000;
constexpr std::uint32_t kSaResethand = 0x80000000;
constexpr std::uint32_t kSigDfl = 0;
constexpr std::uint32_t kSigIgn = 1;
constexpr std::int32_t kSsOnstack = 1;
constexpr std::int32_t kSsDisable = 2;

constexpr std::uint32_t kCpsrUserMode = 0x10;
constexpr std::uint32_t kCpsrThumb = 0x20;
constexpr std::uint32_t kCpsrEndian = 0x200;
constexpr std::uint32_t kCpsrItMask = 0x0600FC00;
// Bits user code may restore through sigreturn: NZCVQ, GE, IT, T.
constexpr std::uint32_t kCpsrUserRestorable = 0xF80F0000 | kCpsrItMask | kCpsrThumb;

// zbrun's own layout inside uc_regspace: magic, 64 extension-register words, fpscr.
constexpr std::uint32_t kVfpFrameMagic = 0x5A425646;
constexpr std::size_t kVfpWords = 64;

constexpr std::uint64_t kUnblockable = (1ULL << (SIGKILL - 1)) | (1ULL << (SIGSTOP - 1));

constexpr int kForwardedHostSignals[] = {SIGALRM, SIGPIPE, SIGUSR1, SIGUSR2, SIGCHLD,
                                         SIGWINCH, SIGURG, SIGVTALRM, SIGPROF, SIGIO};

thread_local GuestThread* t_current_thread = nullptr;
// Receives process-directed host signals that land on a thread running no guest code. Inside an
// app process the kernel may pick any ART thread for them (a guest thread may have the signal
// blocked on the host), so dropping them there loses SIGALRM from setitimer and the like.
std::atomic<GuestThread*> g_process_signal_target{nullptr};
// Forwarding handlers currently between acquiring g_process_signal_target and finishing their use
// of it. clear_process_signal_target() waits for this to reach zero before returning, so the
// caller may free the retired thread. Handlers use it, so it must never take a lock.
std::atomic<std::uint32_t> g_target_readers{0};
static_assert(std::atomic<GuestThread*>::is_always_lock_free);
static_assert(std::atomic<std::uint32_t>::is_always_lock_free);

std::uint64_t sig_bit(int sig) {
    return 1ULL << (sig - 1);
}

bool default_ignored(int sig) {
    return sig == SIGCHLD || sig == SIGCONT || sig == SIGURG || sig == SIGWINCH;
}

void forward_host_signal(int sig, siginfo_t* info, void*) {
    g::siginfo32 guest{};
    guest.si_signo = sig;
    guest.si_code = info->si_code;
    guest.fields[0] = static_cast<std::uint32_t>(info->si_pid);
    guest.fields[1] = static_cast<std::uint32_t>(info->si_uid);
    // This thread's own guest thread cannot be freed while a handler on this thread runs: its owner
    // clears t_current_thread in normal context on this same thread, after every handler returns.
    if (GuestThread* thread = t_current_thread) {
        thread->post_signal(guest);
        return;
    }
    // Register as a reader BEFORE loading the target. clear_process_signal_target() unpublishes the
    // pointer first and then waits for readers, so any handler that could have seen the old value
    // is already counted when it looks at the counter.
    g_target_readers.fetch_add(1, std::memory_order_seq_cst);
    if (GuestThread* thread = g_process_signal_target.load(std::memory_order_seq_cst)) thread->post_signal(guest);
    g_target_readers.fetch_sub(1, std::memory_order_seq_cst);
}

}  // namespace

void Process::set_current_thread(GuestThread* thread) {
    t_current_thread = thread;
}

GuestThread* Process::current_thread() {
    return t_current_thread;
}

void Process::set_process_signal_target(GuestThread* thread) {
    // A published target must be retired through clear_process_signal_target() before another one
    // replaces it; overwriting it here would skip the reader wait for the old thread.
    g_process_signal_target.store(thread, std::memory_order_seq_cst);
}

// Invariant: when this returns, no forwarding handler holds `thread` or can still acquire it, so
// the caller may destroy it. Handlers increment g_target_readers before loading the target; after
// the compare-exchange, every handler that loaded `thread` is counted until it has finished
// post_signal(), and later handlers load nullptr.
//
// The wait runs in normal context and never inside forward_host_signal. A forwarding handler that
// interrupts the waiting thread runs to completion (including its decrement) before the wait
// resumes, so the waiter never waits for itself and the forwarded signals need not be blocked. On
// the Process runner thread t_current_thread is still set here, so its own handlers do not even
// touch the counter.
void Process::clear_process_signal_target(GuestThread* thread) {
    GuestThread* expected = thread;
    if (!g_process_signal_target.compare_exchange_strong(expected, nullptr, std::memory_order_seq_cst)) return;
    // Readers only span one post_signal() call (a few atomic ops and a futex wake): spin briefly,
    // then yield, then sleep with a capped backoff so a descheduled reader does not burn a core.
    for (unsigned attempt = 0; g_target_readers.load(std::memory_order_seq_cst) != 0; ++attempt) {
        if (attempt < 64) continue;
        if (attempt < 1024) {
            sched_yield();
            continue;
        }
        const unsigned shift = attempt - 1024 < 10 ? attempt - 1024 : 10;
        const timespec pause{0, static_cast<long>(1000u << shift)};  // 1 us .. ~1 ms
        nanosleep(&pause, nullptr);
    }
}

int host_interrupt_signal() {
    static const int sig = [] {
        if (const char* name = std::getenv("ZB_INTERRUPT_SIGNAL")) {
            const int chosen = std::atoi(name);
            if (chosen >= SIGRTMIN && chosen <= SIGRTMAX) return chosen;
        }
        // The highest real-time signal: bionic and the ART runtime take theirs from the bottom of
        // the range (POSIX timers, debuggerd, the profiler).
        return SIGRTMAX;
    }();
    return sig;
}

void interrupt_handler(int) {}  // NOLINT: the empty body is the point

void Process::install_host_signal_forwarding() {
    struct sigaction sa;
    std::memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = forward_host_signal;
    sa.sa_flags = SA_SIGINFO;  // no SA_RESTART: blocked host syscalls return EINTR to the guest
    sigemptyset(&sa.sa_mask);
    for (int sig : kForwardedHostSignals) sigaction(sig, &sa, nullptr);

    struct sigaction interrupt;
    std::memset(&interrupt, 0, sizeof interrupt);
    interrupt.sa_handler = interrupt_handler;
    interrupt.sa_flags = 0;  // no SA_RESTART: the point is to end the syscall with EINTR
    sigemptyset(&interrupt.sa_mask);
    sigaction(host_interrupt_signal(), &interrupt, nullptr);
}

bool Process::dispatch_pending_signals(GuestThread& thread) {
    g::siginfo32 info;
    while (thread.take_signal(thread.sigmask, info)) {
        if (!deliver_signal(thread, info, false)) {
            log("guest terminated by signal %d", info.si_signo);
            request_exit(128 + info.si_signo);
            return false;
        }
    }
    return true;
}

bool Process::deliver_fault(GuestThread& thread, const Stop& stop) {
    g::siginfo32 info{};
    if (stop.kind == StopKind::MemoryFault) {
        info.si_signo = SIGSEGV;
        info.si_code = mem_.accessible(stop.fault_addr, 1, 0) ? SEGV_ACCERR : SEGV_MAPERR;
        info.fields[0] = stop.fault_addr;
    } else {
        const bool breakpoint = stop.exception == Dynarmic::A32::Exception::Breakpoint;
        info.si_signo = breakpoint ? SIGTRAP : SIGILL;
        info.si_code = breakpoint ? static_cast<std::int32_t>(TRAP_BRKPT) : static_cast<std::int32_t>(ILL_ILLOPC);
        info.fields[0] = stop.pc;
        thread.regs()[15] = stop.pc;
    }
    return deliver_signal(thread, info, true);
}

bool Process::deliver_signal(GuestThread& thread, const g::siginfo32& info, bool forced) {
    const int sig = info.si_signo;
    if (sig < 1 || sig > 64) return true;

    g::ksigaction32 act;
    {
        std::lock_guard<std::mutex> lock(signal_mutex_);
        act = sigactions[sig];
        const bool has_handler = act.handler > kSigIgn;
        // A fault the guest cannot handle (default, ignored or blocked) kills the process.
        if (forced && (!has_handler || (thread.sigmask & sig_bit(sig)) != 0)) return false;
        if (sig == SIGKILL || sig == SIGSTOP) return false;
        if (act.handler == kSigIgn) return true;
        if (act.handler == kSigDfl) return default_ignored(sig);
        if (act.flags & kSaResethand) {
            sigactions[sig].handler = kSigDfl;
            sigactions[sig].flags &= ~kSaSiginfo;
        }
    }

    auto& regs = thread.regs();
    // A syscall the guest was inside when this signal arrived. The kernel restarts it after the
    // handler when the handler asked for that, and a guest that trusts it does not retry: Unity
    // fails an interrupted semaphore wait outright. The restart is arranged before the frame is
    // built, so sigreturn resumes at the svc itself.
    if (thread.syscall_restartable) {
        const bool sa_restart = (act.flags & kSaRestart) != 0;
        if (sa_restart) {
            for (unsigned i = 0; i < 8; ++i) regs[i] = thread.restart_regs[i];
            regs[15] = thread.restart_pc;
        }
        const std::uint64_t guest_tid =
            thread.tid != 0 ? static_cast<std::uint64_t>(thread.tid)
                            : static_cast<std::uint64_t>(::syscall(SYS_gettid));
        runtime_report().note_signal_restart(thread.restart_syscall,
                                             syscall_name(thread.restart_syscall), guest_tid,
                                             sa_restart, sa_restart);
        thread.syscall_restartable = false;
    }
    const g::stack32& alt = thread.altstack;
    const bool alt_enabled = alt.ss_flags != kSsDisable && alt.ss_size != 0;
    const bool on_altstack = alt_enabled && regs[13] - alt.ss_sp < alt.ss_size;
    std::uint32_t frame_top = regs[13];
    if ((act.flags & kSaOnstack) && alt_enabled && !on_altstack) frame_top = alt.ss_sp + alt.ss_size;

    const bool rt = (act.flags & kSaSiginfo) != 0;
    const std::uint32_t frame_size = rt ? sizeof(g::rt_sigframe32) : sizeof(g::sigframe32);
    const std::uint32_t frame = (frame_top - frame_size) & ~7u;

    g::ucontext32 uc{};
    uc.uc_stack = alt;
    uc.uc_stack.ss_flags = alt_enabled ? (on_altstack ? kSsOnstack : 0) : kSsDisable;
    const std::uint64_t frame_mask = thread.saved_sigmask.value_or(thread.sigmask);
    thread.saved_sigmask.reset();
    uc.uc_mcontext.oldmask = static_cast<std::uint32_t>(frame_mask);
    for (int i = 0; i < 16; ++i) uc.uc_mcontext.regs[i] = regs[i];
    uc.uc_mcontext.cpsr = thread.cpsr();
    if (sig == SIGSEGV || sig == SIGBUS) uc.uc_mcontext.fault_address = info.fields[0];
    uc.uc_sigmask = frame_mask;
    uc.uc_regspace[0] = kVfpFrameMagic;
    std::memcpy(&uc.uc_regspace[1], thread.ext_regs().data(), kVfpWords * sizeof(std::uint32_t));
    uc.uc_regspace[1 + kVfpWords] = thread.fpscr();

    std::uint8_t* dst = mem_.host_ptr(frame, frame_size, kPageWrite);
    if (dst == nullptr) {
        log("cannot write the signal %d frame at 0x%08x", sig, frame);
        return false;
    }
    if (rt) {
        g::rt_sigframe32 f{};
        f.info = info;
        f.uc = uc;
        std::memcpy(dst, &f, sizeof f);
    } else {
        g::sigframe32 f{};
        f.uc = uc;
        std::memcpy(dst, &f, sizeof f);
    }

    std::uint64_t mask = thread.sigmask | (static_cast<std::uint64_t>(act.mask[1]) << 32) | act.mask[0];
    if (!(act.flags & kSaNodefer)) mask |= sig_bit(sig);
    thread.sigmask = mask & ~kUnblockable;

    regs[0] = static_cast<std::uint32_t>(sig);
    if (rt) {
        regs[1] = frame;
        regs[2] = frame + sizeof(g::siginfo32);
    }
    regs[13] = frame;
    regs[14] = (act.flags & kSaRestorer) ? act.restorer : 0;
    regs[15] = act.handler & ~1u;
    thread.set_cpsr((thread.cpsr() & ~(kCpsrThumb | kCpsrItMask | kCpsrEndian)) | ((act.handler & 1) ? kCpsrThumb : 0));
    return true;
}

bool Process::sigreturn(GuestThread& thread, bool rt) {
    auto& regs = thread.regs();
    const std::uint32_t uc_addr = regs[13] + (rt ? sizeof(g::siginfo32) : 0);
    const std::uint8_t* src = mem_.host_ptr(uc_addr, sizeof(g::ucontext32), kPageRead);
    if (src == nullptr) return false;
    g::ucontext32 uc;
    std::memcpy(&uc, src, sizeof uc);

    for (int i = 0; i < 16; ++i) regs[i] = uc.uc_mcontext.regs[i];
    thread.set_cpsr((uc.uc_mcontext.cpsr & kCpsrUserRestorable) | kCpsrUserMode);
    thread.sigmask = uc.uc_sigmask & ~kUnblockable;
    if (uc.uc_regspace[0] == kVfpFrameMagic) {
        std::memcpy(thread.ext_regs().data(), &uc.uc_regspace[1], kVfpWords * sizeof(std::uint32_t));
        thread.set_fpscr(uc.uc_regspace[1 + kVfpWords]);
    }
    if (uc.uc_stack.ss_flags == 0 || uc.uc_stack.ss_flags == kSsDisable) {
        thread.altstack = uc.uc_stack;
    }
    return true;
}

}  // namespace zb
