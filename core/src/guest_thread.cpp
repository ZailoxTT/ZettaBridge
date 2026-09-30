#include "zb/guest_thread.h"

#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <bit>
#include <climits>
#include <cstring>

#include <dynarmic/interface/exclusive_monitor.h>
#include <dynarmic/interface/halt_reason.h>

#include "zb/cp15.h"
#include <cstdio>

#include "zb/log.h"
#include "zb/runtime_report.h"

namespace zb {

namespace {

constexpr Dynarmic::HaltReason kStopHalt = Dynarmic::HaltReason::UserDefined1;
constexpr Dynarmic::HaltReason kInterruptHalt = Dynarmic::HaltReason::UserDefined2;
// With check_halt_on_memory_access, code emitted after each memory access tests this bit and
// returns with PC set to the accessing instruction, so faults are precise.
constexpr Dynarmic::HaltReason kMemoryAbortHalt = Dynarmic::HaltReason::MemoryAbort;

constexpr std::uint32_t kCpsrThumb = 0x20;
constexpr std::uint32_t kCpsrEndian = 0x200;
constexpr std::uint32_t kCpsrItMask = 0x0600FC00;

template <typename T>
T load(const std::uint8_t* p) {
    T v;
    std::memcpy(&v, p, sizeof v);
    return v;
}

template <typename T>
void store(std::uint8_t* p, T v) {
    std::memcpy(p, &v, sizeof v);
}

}  // namespace

GuestThread::GuestThread(GuestMemory& mem, Dynarmic::ExclusiveMonitor* monitor, std::size_t processor_id,
                         bool precise_faults, std::size_t code_cache_size)
    : mem_(mem), processor_id_(processor_id) {
    cp15_ = std::make_shared<Cp15>(&tpidruro_, &tpidrurw_);

    Dynarmic::A32::UserConfig cfg;
    cfg.callbacks = this;
    cfg.global_monitor = monitor;
    cfg.processor_id = processor_id;
    cfg.arch_version = Dynarmic::A32::ArchVersion::v8;
    cfg.fastmem_pointer = static_cast<std::uintptr_t>(reinterpret_cast<std::uintptr_t>(mem.base()));
    cfg.coprocessors[15] = cp15_;
    cfg.define_unpredictable_behaviour = true;
    cfg.enable_cycle_counting = false;
    cfg.check_halt_on_memory_access = precise_faults;
    cfg.code_cache_size = code_cache_size;
    jit_ = std::make_unique<Dynarmic::A32::Jit>(cfg);
}

GuestThread::~GuestThread() = default;

std::array<std::uint32_t, 16>& GuestThread::regs() {
    return jit_->Regs();
}

std::array<std::uint32_t, 64>& GuestThread::ext_regs() {
    return jit_->ExtRegs();
}

std::uint32_t GuestThread::cpsr() const {
    return jit_->Cpsr();
}

std::uint32_t GuestThread::fpscr() const {
    return jit_->Fpscr();
}

void GuestThread::set_fpscr(std::uint32_t value) {
    jit_->SetFpscr(value);
}

void GuestThread::set_cpsr(std::uint32_t value) {
    jit_->SetCpsr(value);
}

// The translator throws for an instruction its backend cannot emit, and that throw crosses every
// entry into translated code: the run loop, a Java-to-guest native call, a nested guest call. It
// is caught here, at the one place they all share, and turned into the same stop a guest fault
// produces, so the caller unwinds normally and the report names the program counter instead of
// the process dying with "terminating".
Stop GuestThread::translator_gave_up(const char* reason) {
    const std::uint32_t pc = regs()[15];
    char detail[128];
    std::snprintf(detail, sizeof detail, "pc 0x%08x: %s", pc, reason != nullptr ? reason : "unknown");
    log("the translator gave up at %s", detail);
    runtime_report().note_jni_detail("translator-gave-up", detail, true);
    Stop stop;
    stop.kind = StopKind::Exception;
    stop.exception = Dynarmic::A32::Exception::UndefinedInstruction;
    stop.pc = pc;
    return stop;
}

Stop GuestThread::run() {
    pending_ = Stop{};
    for (;;) {
        Dynarmic::HaltReason reason{};
        try {
            reason = jit_->Run();
        } catch (const std::exception& failure) {
            return translator_gave_up(failure.what());
        } catch (...) {
            return translator_gave_up(nullptr);
        }
        jit_->ClearHalt(kStopHalt | kMemoryAbortHalt);
        if (pending_.kind == StopKind::None && Dynarmic::Has(reason, kInterruptHalt)) {
            jit_->ClearHalt(kInterruptHalt);
            pending_.kind = StopKind::Interrupted;
            break;
        }
        // Another thread invalidated translated code while we were running; the JIT applies
        // the invalidation at the start of the next Run().
        if (pending_.kind == StopKind::None && Dynarmic::Has(reason, Dynarmic::HaltReason::CacheInvalidation)) continue;
        break;
    }
    Stop s = pending_;
    if (s.kind != StopKind::Exception) s.pc = jit_->Regs()[15];
    return s;
}

std::optional<GuestResult> GuestThread::call(std::uint32_t target, const GuestCall& args,
                                             const GuestStopHandler& handle_stop) {
    const auto saved_regs = regs();
    const auto saved_ext = ext_regs();
    const std::uint32_t saved_cpsr = cpsr();
    const std::uint32_t saved_fpscr = fpscr();

    const auto restore = [&] {
        regs() = saved_regs;
        ext_regs() = saved_ext;
        set_cpsr(saved_cpsr);
        set_fpscr(saved_fpscr);
    };

    const std::uint64_t bytes = static_cast<std::uint64_t>(args.stack.size()) * 4;
    if (bytes > saved_regs[13]) return std::nullopt;
    const std::uint32_t call_sp = static_cast<std::uint32_t>((saved_regs[13] - bytes) & ~7u);
    std::uint8_t* stack = mem_.host_ptr(call_sp, bytes, kPageWrite);
    if (bytes != 0 && stack == nullptr) return std::nullopt;
    if (bytes != 0) std::memcpy(stack, args.stack.data(), static_cast<std::size_t>(bytes));

    regs()[0] = args.regs[0];
    regs()[1] = args.regs[1];
    regs()[2] = args.regs[2];
    regs()[3] = args.regs[3];
    regs()[13] = call_sp;
    regs()[14] = kHostReturnAddress;
    regs()[15] = target & ~1u;
    // A fresh call starts outside any IT block with little-endian data; only T follows the target.
    set_cpsr((saved_cpsr & ~(kCpsrThumb | kCpsrItMask | kCpsrEndian)) | ((target & 1u) ? kCpsrThumb : 0));

    ++call_depth;
    std::optional<GuestResult> result;
    for (;;) {
        const Stop stop = run();
        if (stop.kind == StopKind::Svc && stop.swi == kHostReturnSwi) {
            if (regs()[13] == call_sp) {
                result = GuestResult{regs()[0], regs()[1]};
                break;
            }
            // Not this frame's return: the handler treats the svc as an illegal instruction.
            log("host-to-guest return with sp 0x%08x, frame sp 0x%08x: a longjmp or unwind crossed the "
                "host-to-guest call frame",
                regs()[13], call_sp);
        }
        if (!handle_stop(stop)) break;
    }
    --call_depth;
    restore();
    return result;
}

void GuestThread::invalidate(std::uint32_t addr, std::uint32_t len) {
    jit_->InvalidateCacheRange(addr, len);
}

void GuestThread::begin_blocking() {
    blocking_.store(1);
}

void GuestThread::end_blocking() {
    blocking_.store(0);
}

void GuestThread::interrupt() {
    // seq_cst: the poster sets the pending bit before reading this flag, and the waiter sets the
    // flag before it checks for pending signals, so at least one of the two sees the other.
    if (blocking_.load() == 0 || tid == 0) return;
    ::syscall(SYS_tgkill, ::getpid(), tid, host_interrupt_signal());
}

void GuestThread::post_signal(const g::siginfo32& info) {
    const int sig = info.si_signo;
    if (sig < 1 || sig > 64) return;
    pending_info_[static_cast<std::size_t>(sig)] = info;
    pending_signals_.fetch_or(1ULL << (sig - 1));
    jit_->HaltExecution(kInterruptHalt);
    wake();
    interrupt();
}

void GuestThread::park(std::uint32_t token) {
    ::syscall(SYS_futex, &park_word_, FUTEX_WAIT_PRIVATE, token, nullptr, nullptr, 0);
}

void GuestThread::wake() {
    park_word_.fetch_add(1);
    ::syscall(SYS_futex, &park_word_, FUTEX_WAKE_PRIVATE, INT_MAX, nullptr, nullptr, 0);
}

bool GuestThread::take_signal(std::uint64_t blocked, g::siginfo32& out) {
    std::uint64_t pending = pending_signals_.load();
    for (;;) {
        const std::uint64_t deliverable = pending & ~blocked;
        if (deliverable == 0) return false;
        const int bit = std::countr_zero(deliverable);
        if (pending_signals_.compare_exchange_weak(pending, pending & ~(1ULL << bit))) {
            out = pending_info_[static_cast<std::size_t>(bit + 1)];
            return true;
        }
    }
}

void GuestThread::halt() {
    jit_->HaltExecution(kStopHalt);
}

bool GuestThread::check_access(std::uint32_t vaddr, std::uint32_t len, std::uint8_t need, bool write) {
    if (mem_.accessible(vaddr, len, need)) return true;
    if (pending_.kind == StopKind::None) {
        pending_.kind = StopKind::MemoryFault;
        pending_.fault_addr = vaddr;
        pending_.fault_write = write;
    }
    jit_->HaltExecution(kMemoryAbortHalt);
    return false;
}

std::uint8_t GuestThread::MemoryRead8(std::uint32_t vaddr) {
    return check_access(vaddr, 1, kPageRead, false) ? mem_.base()[vaddr] : 0;
}

std::uint16_t GuestThread::MemoryRead16(std::uint32_t vaddr) {
    return check_access(vaddr, 2, kPageRead, false) ? load<std::uint16_t>(mem_.base() + vaddr) : 0;
}

std::uint32_t GuestThread::MemoryRead32(std::uint32_t vaddr) {
    return check_access(vaddr, 4, kPageRead, false) ? load<std::uint32_t>(mem_.base() + vaddr) : 0;
}

std::uint64_t GuestThread::MemoryRead64(std::uint32_t vaddr) {
    return check_access(vaddr, 8, kPageRead, false) ? load<std::uint64_t>(mem_.base() + vaddr) : 0;
}

void GuestThread::MemoryWrite8(std::uint32_t vaddr, std::uint8_t value) {
    if (check_access(vaddr, 1, kPageWrite, true)) mem_.base()[vaddr] = value;
}

void GuestThread::MemoryWrite16(std::uint32_t vaddr, std::uint16_t value) {
    if (check_access(vaddr, 2, kPageWrite, true)) store(mem_.base() + vaddr, value);
}

void GuestThread::MemoryWrite32(std::uint32_t vaddr, std::uint32_t value) {
    if (check_access(vaddr, 4, kPageWrite, true)) store(mem_.base() + vaddr, value);
}

void GuestThread::MemoryWrite64(std::uint32_t vaddr, std::uint64_t value) {
    if (check_access(vaddr, 8, kPageWrite, true)) store(mem_.base() + vaddr, value);
}

// The ExclusiveMonitor serializes these calls; compare-then-write is atomic under its lock.
bool GuestThread::MemoryWriteExclusive8(std::uint32_t vaddr, std::uint8_t value, std::uint8_t expected) {
    if (!check_access(vaddr, 1, kPageRead | kPageWrite, true) || mem_.base()[vaddr] != expected) return false;
    mem_.base()[vaddr] = value;
    return true;
}

bool GuestThread::MemoryWriteExclusive16(std::uint32_t vaddr, std::uint16_t value, std::uint16_t expected) {
    if (!check_access(vaddr, 2, kPageRead | kPageWrite, true) || load<std::uint16_t>(mem_.base() + vaddr) != expected) return false;
    store(mem_.base() + vaddr, value);
    return true;
}

bool GuestThread::MemoryWriteExclusive32(std::uint32_t vaddr, std::uint32_t value, std::uint32_t expected) {
    if (!check_access(vaddr, 4, kPageRead | kPageWrite, true) || load<std::uint32_t>(mem_.base() + vaddr) != expected) return false;
    store(mem_.base() + vaddr, value);
    return true;
}

bool GuestThread::MemoryWriteExclusive64(std::uint32_t vaddr, std::uint64_t value, std::uint64_t expected) {
    if (!check_access(vaddr, 8, kPageRead | kPageWrite, true) || load<std::uint64_t>(mem_.base() + vaddr) != expected) return false;
    store(mem_.base() + vaddr, value);
    return true;
}

std::optional<std::uint32_t> GuestThread::MemoryReadCode(std::uint32_t vaddr) {
    if (!mem_.accessible(vaddr, 4, kPageExec)) return std::nullopt;
    return load<std::uint32_t>(mem_.base() + vaddr);
}

void GuestThread::InterpreterFallback(std::uint32_t pc, std::size_t num_instructions) {
    log("interpreter fallback requested at pc 0x%08x (%zu instructions)", pc, num_instructions);
    if (pending_.kind == StopKind::None) {
        pending_.kind = StopKind::Exception;
        pending_.exception = Dynarmic::A32::Exception::UndefinedInstruction;
        pending_.pc = pc;
    }
    halt();
}

void GuestThread::CallSVC(std::uint32_t swi) {
    if (pending_.kind == StopKind::None) {
        pending_.kind = StopKind::Svc;
        pending_.swi = swi;
    }
    halt();
}

void GuestThread::ExceptionRaised(std::uint32_t pc, Dynarmic::A32::Exception exception) {
    if (pending_.kind == StopKind::None) {
        pending_.kind = StopKind::Exception;
        pending_.exception = exception;
        pending_.pc = pc;
    }
    halt();
}

}  // namespace zb
