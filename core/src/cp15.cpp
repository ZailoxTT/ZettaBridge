#include <atomic>
#include "zb/cp15.h"

namespace zb {

using Dynarmic::A32::CoprocReg;

namespace {

// The pre-ARMv7 way to write a memory barrier: a CP15 write rather than the DMB/DSB/ISB
// instructions. Mono still emits it (libmono.so does, in its own memory barrier), and old
// compilers emitted it for anything targeting ARMv6. The other CP15 operations are privileged
// and would fault on real hardware, so only these three are served.
bool is_barrier(bool two, unsigned opc1, CoprocReg CRn, CoprocReg CRm, unsigned opc2) {
    if (two || opc1 != 0) return false;
    if (CRn != CoprocReg::C7) return false;
    if (CRm == CoprocReg::C10 && (opc2 == 4 || opc2 == 5)) return true;  // DSB, DMB
    return CRm == CoprocReg::C5 && opc2 == 4;                            // ISB
}

std::uint64_t run_barrier(void*, std::uint32_t, std::uint32_t) {
    std::atomic_thread_fence(std::memory_order_seq_cst);
    return 0;
}

bool is_thread_id_reg(bool two, unsigned opc1, CoprocReg CRn, CoprocReg CRm) {
    return !two && opc1 == 0 && CRn == CoprocReg::C13 && CRm == CoprocReg::C0;
}

}  // namespace

std::optional<Cp15::Callback> Cp15::CompileInternalOperation(bool, unsigned, CoprocReg, CoprocReg, CoprocReg, unsigned) {
    return std::nullopt;
}

Cp15::CallbackOrAccessOneWord Cp15::CompileSendOneWord(bool two, unsigned opc1, CoprocReg CRn, CoprocReg CRm, unsigned opc2) {
    if (is_thread_id_reg(two, opc1, CRn, CRm) && opc2 == 2) return tpidrurw_;
    if (is_barrier(two, opc1, CRn, CRm, opc2)) return Callback{&run_barrier, std::nullopt};
    return std::monostate{};
}

Cp15::CallbackOrAccessTwoWords Cp15::CompileSendTwoWords(bool, unsigned, CoprocReg) {
    return std::monostate{};
}

Cp15::CallbackOrAccessOneWord Cp15::CompileGetOneWord(bool two, unsigned opc1, CoprocReg CRn, CoprocReg CRm, unsigned opc2) {
    if (is_thread_id_reg(two, opc1, CRn, CRm)) {
        if (opc2 == 3) return tpidruro_;
        if (opc2 == 2) return tpidrurw_;
    }
    return std::monostate{};
}

Cp15::CallbackOrAccessTwoWords Cp15::CompileGetTwoWords(bool, unsigned, CoprocReg) {
    return std::monostate{};
}

std::optional<Cp15::Callback> Cp15::CompileLoadWords(bool, bool, CoprocReg, std::optional<std::uint8_t>) {
    return std::nullopt;
}

std::optional<Cp15::Callback> Cp15::CompileStoreWords(bool, bool, CoprocReg, std::optional<std::uint8_t>) {
    return std::nullopt;
}

}  // namespace zb
