#include "zb/syscalls.h"

#include <fcntl.h>
#include <linux/futex.h>
#include <sched.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <sys/utsname.h>
#include <sys/vfs.h>
#include <poll.h>
#include <sys/epoll.h>
#include <sys/file.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/sysinfo.h>
#include <sys/timerfd.h>
#include <sys/times.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <climits>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "gen/syscall_nrs_arm.h"
#include "zb/elf_fixups.h"
#include "zb/guest_abi.h"
#include "zb/guest_memory.h"
#include "zb/guest_thread.h"
#include "zb/hang_watchdog.h"
#include "zb/log.h"
#include "zb/process.h"
#include "zb/runtime_report.h"

namespace zb {

namespace {

struct SyscallName {
    std::uint32_t nr;
    const char* name;
};

constexpr SyscallName kSyscallNames[] = {
#include "gen/syscall_names_arm.inc"
};

constexpr int kPrSetVma = 0x53564d41;

// Keys for Process::first_time, kept apart from raw syscall numbers.
constexpr std::uint64_t kSeenIoctl = 1ULL << 33;
constexpr std::uint64_t kSeenFcntl = 1ULL << 34;
constexpr std::uint64_t kSeenPrctl = 1ULL << 35;
constexpr std::uint64_t kSeenFutex = 1ULL << 36;
constexpr std::uint64_t kSeenSignal = 1ULL << 37;

struct Ctx {
    Process& proc;
    GuestThread& thread;
    GuestMemory& mem;
    std::uint32_t a[6];
    bool stop = false;
    // Set by rt_sigreturn: r0 was restored from the signal frame and must not be overwritten.
    bool no_result = false;
};

// Marks the thread as asleep in a host syscall for as long as it lives, so a guest signal posted
// to this thread breaks that syscall instead of waiting for it to end on its own.
class BlockingSection {
public:
    explicit BlockingSection(GuestThread& thread) : thread_(thread) { thread_.begin_blocking(); }
    ~BlockingSection() { thread_.end_blocking(); }
    BlockingSection(const BlockingSection&) = delete;
    BlockingSection& operator=(const BlockingSection&) = delete;

private:
    GuestThread& thread_;
};

// True when the thread has a signal the guest can act on right now. A syscall interrupted for any
// other reason is restarted rather than handed an EINTR the guest never asked for.
constexpr std::uint64_t kUnblockableMask = (1ULL << (SIGKILL - 1)) | (1ULL << (SIGSTOP - 1));

bool has_deliverable_signal(GuestThread& thread) {
    return thread.has_pending_signals(thread.sigmask);
}

// ZB_STRACE=1 logs every guest syscall with its first four arguments and result.
bool trace_enabled() {
    static const bool enabled = std::getenv("ZB_STRACE") != nullptr;
    return enabled;
}

std::int32_t result_of(long host_ret) {
    return host_ret == -1 ? -errno : static_cast<std::int32_t>(host_ret);
}

template <typename T>
bool read_guest(GuestMemory& m, std::uint32_t addr, T& out) {
    const std::uint8_t* p = m.host_ptr(addr, sizeof(T), kPageRead);
    if (!p) return false;
    std::memcpy(&out, p, sizeof(T));
    return true;
}

template <typename T>
bool write_guest(GuestMemory& m, std::uint32_t addr, const T& value) {
    std::uint8_t* p = m.host_ptr(addr, sizeof(T), kPageWrite);
    if (!p) return false;
    std::memcpy(p, &value, sizeof(T));
    return true;
}

// Validates a NUL-terminated guest string page by page; nullptr if it is not readable.
const char* guest_cstr(GuestMemory& m, std::uint32_t addr) {
    if (addr == 0) return nullptr;
    std::uint64_t a = addr;
    while (a < kGuestSpaceSize) {
        if (!m.accessible(static_cast<std::uint32_t>(a), 1, kPageRead)) return nullptr;
        const std::uint64_t page_end = page_round_up(a + 1);
        for (; a < page_end; ++a) {
            if (m.base()[a] == 0) return reinterpret_cast<const char*>(m.base() + addr);
        }
    }
    return nullptr;
}

// Whether an opened path is worth recording in the runtime report: guest binaries and data
// files (import surface / asset-loading diagnostics), or anything under flutter_assets.
bool open_worth_watching(const char* path) {
    if (path == nullptr) return false;
    const std::string_view name(path);
    if (name.find("flutter_assets") != std::string_view::npos) return true;
    // Fonts decide whether a guest can draw text at all, so their opens and failures deserve the
    // same attention as libraries.
    if (name.find("/fonts") != std::string_view::npos) return true;
    auto ends_with = [&](std::string_view suffix) {
        return name.size() >= suffix.size() && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0;
    };
    return ends_with(".so") || ends_with(".dat") || ends_with(".bin") || ends_with(".ttf") ||
           ends_with(".otf") || ends_with(".ttc") || ends_with("fonts.xml");
}

// Records an openat() of a watched path in the runtime report. Only called after a matching
// path has already been opened successfully or failed, never on every syscall.
void note_watched_open(const char* path, std::int32_t result) {
    if (result >= 0) {
        runtime_report().note_guest_open(path);
    } else {
        runtime_report().note_guest_open_failed(path, -result);
    }
}

void fill_stat64(g::stat64& out, const struct stat& st) {
    std::memset(&out, 0, sizeof out);
    out.st_dev = st.st_dev;
    out.st_ino_trunc = static_cast<std::uint32_t>(st.st_ino);
    out.st_mode = st.st_mode;
    out.st_nlink = static_cast<std::uint32_t>(st.st_nlink);
    out.st_uid = st.st_uid;
    out.st_gid = st.st_gid;
    out.st_rdev = st.st_rdev;
    out.st_size = st.st_size;
    out.st_blksize = static_cast<std::uint32_t>(st.st_blksize);
    out.st_blocks = static_cast<std::uint64_t>(st.st_blocks);
    out.atime_sec = static_cast<std::uint32_t>(st.st_atim.tv_sec);
    out.atime_nsec = static_cast<std::uint32_t>(st.st_atim.tv_nsec);
    out.mtime_sec = static_cast<std::uint32_t>(st.st_mtim.tv_sec);
    out.mtime_nsec = static_cast<std::uint32_t>(st.st_mtim.tv_nsec);
    out.ctime_sec = static_cast<std::uint32_t>(st.st_ctim.tv_sec);
    out.ctime_nsec = static_cast<std::uint32_t>(st.st_ctim.tv_nsec);
    out.st_ino = st.st_ino;
}

bool read_timespec(GuestMemory& m, std::uint32_t addr, bool time64, timespec& out) {
    if (time64) {
        g::timespec64 t;
        if (!read_guest(m, addr, t)) return false;
        out.tv_sec = static_cast<time_t>(t.tv_sec);
        out.tv_nsec = static_cast<long>(t.tv_nsec);
    } else {
        g::timespec32 t;
        if (!read_guest(m, addr, t)) return false;
        out.tv_sec = t.tv_sec;
        out.tv_nsec = t.tv_nsec;
    }
    return true;
}

bool write_timespec(GuestMemory& m, std::uint32_t addr, bool time64, const timespec& ts) {
    if (time64) return write_guest(m, addr, g::timespec64{ts.tv_sec, ts.tv_nsec});
    return write_guest(m, addr, g::timespec32{static_cast<std::int32_t>(ts.tv_sec), static_cast<std::int32_t>(ts.tv_nsec)});
}

std::int32_t sys_read_write(Ctx& c, bool is_write) {
    const std::uint32_t len = c.a[2];
    std::uint8_t* buf = c.mem.host_ptr(c.a[1], len, is_write ? kPageRead : kPageWrite);
    if (len != 0 && buf == nullptr) return -EFAULT;
    const int fd = static_cast<int>(c.a[0]);
    return result_of(is_write ? ::write(fd, buf, len) : ::read(fd, buf, len));
}

std::int32_t sys_pread_pwrite(Ctx& c, bool is_write) {
    const std::uint32_t len = c.a[2];
    std::uint8_t* buf = c.mem.host_ptr(c.a[1], len, is_write ? kPageRead : kPageWrite);
    if (len != 0 && buf == nullptr) return -EFAULT;
    const off_t offset = static_cast<off_t>(static_cast<std::uint64_t>(c.a[4]) | (static_cast<std::uint64_t>(c.a[5]) << 32));
    const int fd = static_cast<int>(c.a[0]);
    return result_of(is_write ? ::pwrite(fd, buf, len, offset) : ::pread(fd, buf, len, offset));
}

std::int32_t sys_readv_writev(Ctx& c, bool is_write) {
    const std::uint32_t count = c.a[2];
    if (count > IOV_MAX) return -EINVAL;
    std::vector<iovec> iov(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        g::iovec32 v;
        if (!read_guest(c.mem, c.a[1] + i * sizeof(g::iovec32), v)) return -EFAULT;
        std::uint8_t* base = c.mem.host_ptr(v.iov_base, v.iov_len, is_write ? kPageRead : kPageWrite);
        if (v.iov_len != 0 && base == nullptr) return -EFAULT;
        iov[i] = {base, v.iov_len};
    }
    const int fd = static_cast<int>(c.a[0]);
    return result_of(is_write ? ::writev(fd, iov.data(), static_cast<int>(count)) : ::readv(fd, iov.data(), static_cast<int>(count)));
}

std::int32_t sys_brk(Ctx& c) {
    Process& p = c.proc;
    const std::uint32_t want = c.a[0];
    if (want < p.brk_start) return static_cast<std::int32_t>(p.brk_current);
    const std::uint64_t old_top = page_round_up(p.brk_current);
    const std::uint64_t new_top = page_round_up(want);
    if (new_top > old_top) {
        const auto start = static_cast<std::uint32_t>(old_top);
        if (new_top > p.mmap_limit || !c.mem.range_free(start, new_top - old_top) ||
            !c.mem.map_anon(start, new_top - old_top, PROT_READ | PROT_WRITE)) {
            return static_cast<std::int32_t>(p.brk_current);
        }
    } else if (new_top < old_top) {
        c.mem.unmap(static_cast<std::uint32_t>(new_top), old_top - new_top);
        p.invalidate(static_cast<std::uint32_t>(new_top), static_cast<std::uint32_t>(old_top - new_top));
    }
    p.brk_current = want;
    return static_cast<std::int32_t>(want);
}

std::int32_t sys_mmap2(Ctx& c) {
    const std::uint32_t addr = c.a[0];
    const std::uint32_t len = c.a[1];
    const int prot = static_cast<int>(c.a[2]);
    const int flags = static_cast<int>(c.a[3]);
    const int fd = static_cast<int>(c.a[4]);
    const std::uint64_t offset = static_cast<std::uint64_t>(c.a[5]) * kPageSize;

    if (len == 0) return -EINVAL;
    const std::uint64_t size = page_round_up(len);
    const bool fixed = (flags & MAP_FIXED) != 0;
    const bool noreplace = (flags & MAP_FIXED_NOREPLACE) != 0;

    std::uint32_t at = 0;
    if (fixed || noreplace) {
        if (addr & kPageMask) return -EINVAL;
        if (static_cast<std::uint64_t>(addr) + size > kGuestSpaceSize) return -ENOMEM;
        if (noreplace && !c.mem.range_free(addr, size)) return -EEXIST;
        at = addr;
    } else if (addr >= 0x10000 && (addr & kPageMask) == 0 && static_cast<std::uint64_t>(addr) + size <= c.proc.mmap_limit &&
               c.mem.range_free(addr, size)) {
        at = addr;
    } else {
        at = c.mem.find_free(size, c.proc.mmap_limit);
        if (at == 0) return -ENOMEM;
    }

    // Code of libraries marked DT_ZB_TEXTREL stays writable so the linker can relocate it.
    const bool textrel = !(flags & MAP_ANONYMOUS) && (prot & PROT_EXEC) && elf_has_textrel_marker(fd);
    const int effective_prot = textrel ? (prot | PROT_WRITE) : prot;

    errno = 0;
    const bool ok = (flags & MAP_ANONYMOUS) ? c.mem.map_anon(at, size, effective_prot)
                                            : c.mem.map_file(at, size, effective_prot, flags, fd, offset);
    if (!ok) return errno ? -errno : -ENOMEM;
    if (flags & MAP_ANONYMOUS) {
        c.proc.forget_mappings(at, size);
    } else {
        char link[64];
        char target[PATH_MAX];
        std::snprintf(link, sizeof link, "/proc/self/fd/%d", fd);
        const ssize_t n = ::readlink(link, target, sizeof target - 1);
        c.proc.record_file_mapping(at, static_cast<std::uint32_t>(size), offset,
                                   n > 0 ? std::string(target, static_cast<std::size_t>(n)) : std::string("fd"));
        if (textrel) c.proc.add_textrel_range(at, static_cast<std::uint32_t>(size));
    }
    c.proc.invalidate(at, static_cast<std::uint32_t>(size));
    return static_cast<std::int32_t>(at);
}

std::int32_t sys_munmap(Ctx& c) {
    const std::uint32_t addr = c.a[0];
    if ((addr & kPageMask) || c.a[1] == 0) return -EINVAL;
    const std::uint64_t size = page_round_up(c.a[1]);
    if (static_cast<std::uint64_t>(addr) + size > kGuestSpaceSize) return -EINVAL;
    if (!c.mem.unmap(addr, size)) return -EINVAL;
    c.proc.forget_mappings(addr, size);
    c.proc.invalidate(addr, static_cast<std::uint32_t>(size));
    return 0;
}

std::int32_t sys_mprotect(Ctx& c) {
    const std::uint32_t addr = c.a[0];
    if (addr & kPageMask) return -EINVAL;
    if (c.a[1] == 0) return 0;
    const std::uint64_t size = page_round_up(c.a[1]);
    if (!c.mem.accessible(addr, size, 0)) return -ENOMEM;
    int prot = static_cast<int>(c.a[2]);
    if ((prot & PROT_EXEC) && c.proc.overlaps_textrel_range(addr, size)) prot |= PROT_WRITE;
    if (!c.mem.protect(addr, size, prot)) return -EACCES;
    c.proc.invalidate(addr, static_cast<std::uint32_t>(size));
    return 0;
}

std::int32_t sys_madvise(Ctx& c) {
    const std::uint32_t addr = c.a[0];
    if (addr & kPageMask) return -EINVAL;
    if (c.a[1] == 0) return 0;
    const std::uint64_t size = page_round_up(c.a[1]);
    if (!c.mem.accessible(addr, size, 0)) return -ENOMEM;
    return result_of(::madvise(c.mem.base() + addr, size, static_cast<int>(c.a[2])));
}

std::int32_t sys_rt_sigaction(Ctx& c) {
    const std::uint32_t sig = c.a[0];
    if (sig < 1 || sig > 64 || c.a[3] != 8) return -EINVAL;
    g::ksigaction32 act{};
    const bool has_new = c.a[1] != 0;
    if (has_new) {
        if (sig == SIGKILL || sig == SIGSTOP) return -EINVAL;
        if (!read_guest(c.mem, c.a[1], act)) return -EFAULT;
        // The guest linker installs Android's debuggerd crash handlers, which fork and exec
        // crash_dump. zbrun has no crash_dump (and refuses fork), so those handlers would turn
        // every crash into exit(1). Keep the default action instead: the process then dies with
        // the signal and zbrun prints its own crash report. Application handlers are unaffected.
        if (act.handler > 1 && c.proc.in_guest_linker(act.handler & ~1u)) {
            if (trace_enabled()) log("not installing the guest linker's crash handler for signal %u", sig);
            act = g::ksigaction32{};
        }
    }
    if (c.a[2] != 0 && !write_guest(c.mem, c.a[2], c.proc.sigactions[sig])) return -EFAULT;
    if (has_new) c.proc.sigactions[sig] = act;
    return 0;
}

std::int32_t sys_rt_sigprocmask(Ctx& c) {
    if (c.a[3] != 8) return -EINVAL;
    const std::uint64_t old_mask = c.thread.sigmask;
    if (c.a[1] != 0) {
        std::uint64_t set;
        if (!read_guest(c.mem, c.a[1], set)) return -EFAULT;
        std::uint64_t mask = old_mask;
        switch (c.a[0]) {
        case SIG_BLOCK: mask |= set; break;
        case SIG_UNBLOCK: mask &= ~set; break;
        case SIG_SETMASK: mask = set; break;
        default: return -EINVAL;
        }
        mask &= ~kUnblockableMask;
        c.thread.sigmask = mask;
    }
    if (c.a[2] != 0 && !write_guest(c.mem, c.a[2], old_mask)) return -EFAULT;
    return 0;
}

std::int32_t sys_sigaltstack(Ctx& c) {
    g::stack32 ss{};
    const bool has_new = c.a[0] != 0;
    if (has_new && !read_guest(c.mem, c.a[0], ss)) return -EFAULT;
    if (c.a[1] != 0 && !write_guest(c.mem, c.a[1], c.thread.altstack)) return -EFAULT;
    if (has_new) c.thread.altstack = ss;
    return 0;
}

// kill / tkill / tgkill / rt_tgsigqueueinfo inside the guest process. The signal is queued on
// the target guest thread; Process delivers it at that thread's next stop.
std::int32_t sys_send_signal(Ctx& c, bool process_directed, std::int32_t tgid, std::int32_t tid, std::uint32_t sig,
                             const g::siginfo32* queued_info, std::int32_t code) {
    if (sig > 64) return -EINVAL;
    const auto pid = static_cast<std::int32_t>(::getpid());

    g::siginfo32 info{};
    if (queued_info != nullptr) {
        info = *queued_info;
    } else {
        info.si_code = code;
        info.fields[0] = static_cast<std::uint32_t>(pid);
        info.fields[1] = static_cast<std::uint32_t>(::getuid());
    }
    info.si_signo = static_cast<std::int32_t>(sig);

    if (process_directed) {
        if (tgid != pid && tgid != 0) {
            if (c.proc.first_time(kSeenSignal | sig)) log("signal %u to another process refused", sig);
            return -EPERM;
        }
        if (sig != 0) c.thread.post_signal(info);
        return 0;
    }
    if (tgid != -1 && tgid != pid) return -ESRCH;
    // Signal 0 only probes for existence and never dereferences the thread.
    if (sig == 0) return c.proc.find_thread(tid) != nullptr ? 0 : -ESRCH;
    // Lookup and post under one lock: the target may be exiting or a carrier lease releasing.
    return c.proc.post_signal_to(tid, info) ? 0 : -ESRCH;
}

// Waits with a temporary signal mask until this thread has a signal to act on. The guest sees the
// mask it asked for while its handler runs, and sigreturn restores the mask it had before.
std::int32_t sys_sigsuspend(Ctx& c, bool rt) {
    std::uint64_t mask = 0;
    if (rt) {
        // rt_sigsuspend(set, sigsetsize): the size is the second argument, not the third.
        if (c.a[1] != 8) return -EINVAL;
        if (!read_guest(c.mem, c.a[0], mask)) return -EFAULT;
    } else {
        // The legacy call takes the mask itself in the third argument, as one 32-bit set.
        mask = c.a[2];
    }
    const std::uint64_t saved = c.thread.sigmask;
    c.thread.sigmask = mask & ~kUnblockableMask;
    {
        BlockingSection blocking(c.thread);
        while (!has_deliverable_signal(c.thread)) {
            const std::uint32_t token = c.thread.park_token();
            if (has_deliverable_signal(c.thread)) break;
            c.thread.park(token);
        }
    }
    // The handler runs under the temporary mask; its frame records the one to return to.
    c.thread.saved_sigmask = saved;
    return -EINTR;
}

std::int32_t sys_clock_get(Ctx& c, bool res, bool time64) {
    timespec ts;
    const clockid_t clk = static_cast<clockid_t>(static_cast<std::int32_t>(c.a[0]));
    if ((res ? ::clock_getres(clk, &ts) : ::clock_gettime(clk, &ts)) != 0) return -errno;
    if (c.a[1] == 0) return 0;
    return write_timespec(c.mem, c.a[1], time64, ts) ? 0 : -EFAULT;
}

std::int32_t sys_gettimeofday(Ctx& c) {
    timeval tv;
    struct timezone tz;
    if (::gettimeofday(&tv, &tz) != 0) return -errno;
    if (c.a[0] != 0 &&
        !write_guest(c.mem, c.a[0], g::timeval32{static_cast<std::int32_t>(tv.tv_sec), static_cast<std::int32_t>(tv.tv_usec)})) {
        return -EFAULT;
    }
    if (c.a[1] != 0 && !write_guest(c.mem, c.a[1], tz)) return -EFAULT;
    return 0;
}

std::int32_t sys_clock_nanosleep(Ctx& c, bool time64) {
    timespec req;
    timespec rem{};
    if (!read_timespec(c.mem, c.a[2], time64, req)) return -EFAULT;
    const clockid_t clk = static_cast<clockid_t>(static_cast<std::int32_t>(c.a[0]));
    const int rc = ::clock_nanosleep(clk, static_cast<int>(c.a[1]), &req, &rem);
    if (rc == EINTR && c.a[3] != 0) write_timespec(c.mem, c.a[3], time64, rem);
    return -rc;
}

std::int32_t sys_nanosleep(Ctx& c) {
    timespec req;
    timespec rem{};
    if (!read_timespec(c.mem, c.a[0], false, req)) return -EFAULT;
    if (::nanosleep(&req, &rem) == 0) return 0;
    const int err = errno;
    if (err == EINTR && c.a[1] != 0) write_timespec(c.mem, c.a[1], false, rem);
    return -err;
}

std::int32_t sys_stat_common(Ctx& c, int rc, const struct stat& st, std::uint32_t out_addr) {
    if (rc != 0) return -errno;
    g::stat64 out;
    fill_stat64(out, st);
    return write_guest(c.mem, out_addr, out) ? 0 : -EFAULT;
}

std::int32_t sys_statfs_common(Ctx& c, int rc, const struct statfs& st, std::uint32_t out_addr) {
    if (rc != 0) return -errno;
    g::statfs64 out{};
    out.f_type = static_cast<std::uint32_t>(st.f_type);
    out.f_bsize = static_cast<std::uint32_t>(st.f_bsize);
    out.f_blocks = st.f_blocks;
    out.f_bfree = st.f_bfree;
    out.f_bavail = st.f_bavail;
    out.f_files = st.f_files;
    out.f_ffree = st.f_ffree;
    std::memcpy(out.f_fsid, &st.f_fsid, sizeof out.f_fsid);
    out.f_namelen = static_cast<std::uint32_t>(st.f_namelen);
    out.f_frsize = static_cast<std::uint32_t>(st.f_frsize);
    out.f_flags = static_cast<std::uint32_t>(st.f_flags);
    return write_guest(c.mem, out_addr, out) ? 0 : -EFAULT;
}

std::int32_t sys_fstatat64(Ctx& c) {
    const char* path = guest_cstr(c.mem, c.a[1]);
    if (!path) return -EFAULT;
    struct stat st;
    const int rc = ::fstatat(static_cast<int>(c.a[0]), c.proc.translate_path(path).c_str(), &st, static_cast<int>(c.a[3]));
    return sys_stat_common(c, rc, st, c.a[2]);
}

std::int32_t sys_statx(Ctx& c) {
    const char* path = guest_cstr(c.mem, c.a[1]);
    std::uint8_t* buf = c.mem.host_ptr(c.a[4], 256, kPageWrite);
    if (!path || !buf) return -EFAULT;
    const std::string host_path = c.proc.translate_path(path);
    return result_of(::syscall(SYS_statx, static_cast<int>(c.a[0]), host_path.c_str(), static_cast<int>(c.a[2]), c.a[3], buf));
}

std::int32_t sys_ioctl(Ctx& c) {
    const unsigned long request = c.a[1];
    std::uint32_t arg_size = 0;
    std::uint8_t need = kPageRead | kPageWrite;
    switch (request) {
    case TCGETS: arg_size = 36; break;
    case TIOCGWINSZ: arg_size = 8; break;
    case FIONREAD: arg_size = 4; break;
    case FIONBIO: arg_size = 4; need = kPageRead; break;
    default:
        if (c.proc.first_time(kSeenIoctl | request)) log("unsupported ioctl 0x%lx on fd %u", request, c.a[0]);
        return -ENOTTY;
    }
    std::uint8_t* arg = c.mem.host_ptr(c.a[2], arg_size, need);
    if (!arg) return -EFAULT;
    return result_of(::ioctl(static_cast<int>(c.a[0]), request, arg));
}

std::int32_t sys_fcntl64(Ctx& c) {
    const int cmd = static_cast<int>(c.a[1]);
    switch (cmd) {
    case F_DUPFD:
    case F_GETFD:
    case F_SETFD:
    case F_GETFL:
    case F_SETFL:
    case F_DUPFD_CLOEXEC:
        return result_of(::fcntl(static_cast<int>(c.a[0]), cmd, static_cast<long>(c.a[2])));
    default:
        if (c.proc.first_time(kSeenFcntl | static_cast<std::uint32_t>(cmd))) log("unsupported fcntl command %d", cmd);
        return -EINVAL;
    }
}

std::int32_t sys_prctl(Ctx& c) {
    const int option = static_cast<int>(c.a[0]);
    switch (option) {
    case kPrSetVma:
        return 0;
    case PR_GET_DUMPABLE:
    case PR_SET_DUMPABLE:
    case PR_SET_NO_NEW_PRIVS:
    case PR_GET_NO_NEW_PRIVS:
        return result_of(::prctl(option, static_cast<unsigned long>(c.a[1]), static_cast<unsigned long>(c.a[2]),
                                 static_cast<unsigned long>(c.a[3]), static_cast<unsigned long>(c.a[4])));
    case PR_SET_NAME:
    case PR_GET_NAME: {
        std::uint8_t* name = c.mem.host_ptr(c.a[1], 16, option == PR_SET_NAME ? kPageRead : kPageWrite);
        if (!name) return -EFAULT;
        return result_of(::prctl(option, name, 0, 0, 0));
    }
    default:
        if (c.proc.first_time(kSeenPrctl | static_cast<std::uint32_t>(option))) log("unsupported prctl option %d", option);
        return -EINVAL;
    }
}

std::int32_t sys_uname(Ctx& c) {
    utsname u;
    if (::uname(&u) != 0) return -errno;
    std::strncpy(u.machine, "armv8l", sizeof u.machine);
    return write_guest(c.mem, c.a[0], u) ? 0 : -EFAULT;
}

std::int32_t sys_futex(Ctx& c, bool time64) {
    const int op = static_cast<int>(c.a[1]);
    const int cmd = op & FUTEX_CMD_MASK;
    std::uint8_t* uaddr = c.mem.host_ptr(c.a[0], 4, kPageRead);
    if (!uaddr) return -EFAULT;
    switch (cmd) {
    case FUTEX_WAIT:
    case FUTEX_WAIT_BITSET: {
        timespec ts;
        timespec* tsp = nullptr;
        if (c.a[3] != 0) {
            if (!read_timespec(c.mem, c.a[3], time64, ts)) return -EFAULT;
            tsp = &ts;
        }
        BlockingSection blocking(c.thread);
        for (;;) {
            // Ordered against post_signal(): the flag above is set before this check, and a poster
            // sets the pending bit before it reads the flag, so a signal is never missed here.
            if (has_deliverable_signal(c.thread)) return -EINTR;
            const std::int32_t res = result_of(::syscall(SYS_futex, uaddr, op, c.a[2], tsp, nullptr, c.a[5]));
            if (res != -EINTR) return res;
            if (has_deliverable_signal(c.thread)) return -EINTR;
            // Interrupted by our own wakeup with nothing to deliver: wait again. A timed wait
            // restarts with its full timeout, as a restarted guest wait would.
        }
    }
    case FUTEX_WAKE:
    case FUTEX_WAKE_BITSET:
        return result_of(::syscall(SYS_futex, uaddr, op, c.a[2], nullptr, nullptr, c.a[5]));
    default:
        if (c.proc.first_time(kSeenFutex | static_cast<std::uint32_t>(cmd))) log("unsupported futex command %d", cmd);
        return -ENOSYS;
    }
}

std::int32_t sys_ugetrlimit(Ctx& c) {
    rlimit rl;
    if (::getrlimit(static_cast<int>(c.a[0]), &rl) != 0) return -errno;
    auto clamp = [](rlim_t v) -> std::uint32_t { return v > 0xFFFFFFFFu ? 0xFFFFFFFFu : static_cast<std::uint32_t>(v); };
    return write_guest(c.mem, c.a[1], g::rlimit32{clamp(rl.rlim_cur), clamp(rl.rlim_max)}) ? 0 : -EFAULT;
}

std::int32_t sys_prlimit64(Ctx& c) {
    std::uint8_t* new_limit = nullptr;
    std::uint8_t* old_limit = nullptr;
    if (c.a[2] != 0 && !(new_limit = c.mem.host_ptr(c.a[2], 16, kPageRead))) return -EFAULT;
    if (c.a[3] != 0 && !(old_limit = c.mem.host_ptr(c.a[3], 16, kPageWrite))) return -EFAULT;
    return result_of(::syscall(SYS_prlimit64, static_cast<pid_t>(c.a[0]), static_cast<int>(c.a[1]), new_limit, old_limit));
}

std::int32_t sys_llseek(Ctx& c) {
    const off_t offset = static_cast<off_t>((static_cast<std::uint64_t>(c.a[1]) << 32) | c.a[2]);
    const off_t r = ::lseek(static_cast<int>(c.a[0]), offset, static_cast<int>(c.a[4]));
    if (r == -1) return -errno;
    return write_guest(c.mem, c.a[3], static_cast<std::int64_t>(r)) ? 0 : -EFAULT;
}

std::int32_t sys_lseek(Ctx& c) {
    const off_t r = ::lseek(static_cast<int>(c.a[0]), static_cast<std::int32_t>(c.a[1]), static_cast<int>(c.a[2]));
    if (r == -1) return -errno;
    if (r > INT32_MAX) return -EOVERFLOW;
    return static_cast<std::int32_t>(r);
}

std::int32_t sys_pipe2(Ctx& c) {
    std::uint8_t* out = c.mem.host_ptr(c.a[0], 8, kPageWrite);
    if (!out) return -EFAULT;
    int fds[2];
    if (::pipe2(fds, static_cast<int>(c.a[1])) != 0) return -errno;
    std::memcpy(out, fds, sizeof fds);
    return 0;
}

std::int32_t sys_path_call(Ctx& c, std::uint32_t path_arg, long (*call)(Ctx&, const char*)) {
    const char* path = guest_cstr(c.mem, c.a[path_arg]);
    if (!path) return -EFAULT;
    const std::string host_path = c.proc.translate_path(path);
    return result_of(call(c, host_path.c_str()));
}


std::int32_t sys_two_paths(Ctx& c, std::uint32_t first, std::uint32_t second,
                           long (*call)(Ctx&, const char*, const char*)) {
    const char* a = guest_cstr(c.mem, c.a[first]);
    const char* b = guest_cstr(c.mem, c.a[second]);
    if (!a || !b) return -EFAULT;
    const std::string host_a = c.proc.translate_path(a);
    const std::string host_b = c.proc.translate_path(b);
    return result_of(call(c, host_a.c_str(), host_b.c_str()));
}

std::int32_t sys_poll(Ctx& c, bool ppoll, bool time64) {
    const std::uint32_t nfds = c.a[1];
    if (nfds > 65536) return -EINVAL;
    std::uint8_t* fds = c.mem.host_ptr(c.a[0], static_cast<std::uint64_t>(nfds) * sizeof(pollfd), kPageRead | kPageWrite);
    if (nfds != 0 && fds == nullptr) return -EFAULT;
    if (!ppoll) return result_of(::poll(reinterpret_cast<pollfd*>(fds), nfds, static_cast<int>(c.a[2])));
    timespec ts;
    timespec* tsp = nullptr;
    if (c.a[2] != 0) {
        if (!read_timespec(c.mem, c.a[2], time64, ts)) return -EFAULT;
        tsp = &ts;
    }
    // The guest signal mask is emulated; a host mask would affect the wrong signals.
    return result_of(::ppoll(reinterpret_cast<pollfd*>(fds), nfds, tsp, nullptr));
}

fd_set* guest_fd_set(GuestMemory& m, std::uint32_t addr, bool& fault) {
    if (addr == 0) return nullptr;
    std::uint8_t* p = m.host_ptr(addr, sizeof(fd_set), kPageRead | kPageWrite);
    if (p == nullptr) fault = true;
    return reinterpret_cast<fd_set*>(p);
}

// fd_set has the same 128-byte bit layout for 32-bit and 64-bit longs on little-endian.
std::int32_t sys_select(Ctx& c, bool pselect, bool time64) {
    const int nfds = static_cast<int>(c.a[0]);
    if (nfds < 0 || nfds > FD_SETSIZE) return -EINVAL;
    bool fault = false;
    fd_set* r = guest_fd_set(c.mem, c.a[1], fault);
    fd_set* w = guest_fd_set(c.mem, c.a[2], fault);
    fd_set* e = guest_fd_set(c.mem, c.a[3], fault);
    if (fault) return -EFAULT;
    if (!pselect) {
        timeval tv;
        timeval* tvp = nullptr;
        if (c.a[4] != 0) {
            g::timeval32 t;
            if (!read_guest(c.mem, c.a[4], t)) return -EFAULT;
            tv.tv_sec = t.tv_sec;
            tv.tv_usec = t.tv_usec;
            tvp = &tv;
        }
        const int res = ::select(nfds, r, w, e, tvp);
        if (res < 0) return -errno;
        if (tvp != nullptr) {
            write_guest(c.mem, c.a[4], g::timeval32{static_cast<std::int32_t>(tv.tv_sec), static_cast<std::int32_t>(tv.tv_usec)});
        }
        return res;
    }
    timespec ts;
    timespec* tsp = nullptr;
    if (c.a[4] != 0) {
        if (!read_timespec(c.mem, c.a[4], time64, ts)) return -EFAULT;
        tsp = &ts;
    }
    return result_of(::pselect(nfds, r, w, e, tsp, nullptr));
}

std::int32_t sys_sockaddr_in(Ctx& c, long host_nr) {
    std::uint8_t* addr = c.mem.host_ptr(c.a[1], c.a[2], kPageRead);
    if (c.a[2] != 0 && addr == nullptr) return -EFAULT;
    return result_of(::syscall(host_nr, static_cast<int>(c.a[0]), addr, c.a[2]));
}

// accept / accept4 / getsockname / getpeername: (fd, sockaddr*, socklen_t* [, flags]).
std::int32_t sys_sockaddr_out(Ctx& c, long host_nr, bool with_flags) {
    std::uint8_t* addr = nullptr;
    std::uint8_t* len = nullptr;
    if (c.a[2] != 0) {
        len = c.mem.host_ptr(c.a[2], 4, kPageRead | kPageWrite);
        if (len == nullptr) return -EFAULT;
        std::uint32_t capacity;
        std::memcpy(&capacity, len, 4);
        if (c.a[1] != 0) {
            addr = c.mem.host_ptr(c.a[1], capacity, kPageWrite);
            if (capacity != 0 && addr == nullptr) return -EFAULT;
        }
    }
    if (with_flags) return result_of(::syscall(host_nr, static_cast<int>(c.a[0]), addr, len, static_cast<int>(c.a[3])));
    return result_of(::syscall(host_nr, static_cast<int>(c.a[0]), addr, len));
}

std::int32_t sys_sendto(Ctx& c, std::uint32_t dest, std::uint32_t dest_len) {
    std::uint8_t* buf = c.mem.host_ptr(c.a[1], c.a[2], kPageRead);
    std::uint8_t* to = dest != 0 ? c.mem.host_ptr(dest, dest_len, kPageRead) : nullptr;
    if ((c.a[2] != 0 && buf == nullptr) || (dest != 0 && to == nullptr)) return -EFAULT;
    return result_of(::syscall(SYS_sendto, static_cast<int>(c.a[0]), buf, c.a[2], static_cast<int>(c.a[3]), to,
                               dest != 0 ? dest_len : 0));
}

std::int32_t sys_recvfrom(Ctx& c, std::uint32_t src, std::uint32_t src_len_addr) {
    std::uint8_t* buf = c.mem.host_ptr(c.a[1], c.a[2], kPageWrite);
    if (c.a[2] != 0 && buf == nullptr) return -EFAULT;
    std::uint8_t* from = nullptr;
    std::uint8_t* from_len = nullptr;
    if (src_len_addr != 0) {
        from_len = c.mem.host_ptr(src_len_addr, 4, kPageRead | kPageWrite);
        if (from_len == nullptr) return -EFAULT;
        std::uint32_t capacity;
        std::memcpy(&capacity, from_len, 4);
        if (src != 0) {
            from = c.mem.host_ptr(src, capacity, kPageWrite);
            if (capacity != 0 && from == nullptr) return -EFAULT;
        }
    }
    return result_of(::syscall(SYS_recvfrom, static_cast<int>(c.a[0]), buf, c.a[2], static_cast<int>(c.a[3]), from, from_len));
}

// SO_RCVTIMEO/SO_SNDTIMEO with the old option numbers take a native-width struct timeval.
constexpr int kSoRcvTimeoOld = 20;
constexpr int kSoSndTimeoOld = 21;

bool is_old_timeval_option(int level, int name) {
    return level == SOL_SOCKET && (name == kSoRcvTimeoOld || name == kSoSndTimeoOld);
}

std::int32_t sys_setsockopt(Ctx& c) {
    const int level = static_cast<int>(c.a[1]);
    const int name = static_cast<int>(c.a[2]);
    if (is_old_timeval_option(level, name) && c.a[4] >= sizeof(g::timeval32)) {
        g::timeval32 t;
        if (!read_guest(c.mem, c.a[3], t)) return -EFAULT;
        timeval tv{t.tv_sec, t.tv_usec};
        return result_of(::setsockopt(static_cast<int>(c.a[0]), level, name, &tv, sizeof tv));
    }
    std::uint8_t* value = c.mem.host_ptr(c.a[3], c.a[4], kPageRead);
    if (c.a[4] != 0 && value == nullptr) return -EFAULT;
    return result_of(::syscall(SYS_setsockopt, static_cast<int>(c.a[0]), level, name, value, c.a[4]));
}

std::int32_t sys_getsockopt(Ctx& c) {
    const int level = static_cast<int>(c.a[1]);
    const int name = static_cast<int>(c.a[2]);
    std::uint8_t* len = c.mem.host_ptr(c.a[4], 4, kPageRead | kPageWrite);
    if (len == nullptr) return -EFAULT;
    std::uint32_t capacity;
    std::memcpy(&capacity, len, 4);
    if (is_old_timeval_option(level, name)) {
        timeval tv{};
        socklen_t tv_len = sizeof tv;
        if (::getsockopt(static_cast<int>(c.a[0]), level, name, &tv, &tv_len) != 0) return -errno;
        if (capacity < sizeof(g::timeval32)) return -EINVAL;
        if (!write_guest(c.mem, c.a[3], g::timeval32{static_cast<std::int32_t>(tv.tv_sec), static_cast<std::int32_t>(tv.tv_usec)})) {
            return -EFAULT;
        }
        const std::uint32_t written = sizeof(g::timeval32);
        std::memcpy(len, &written, 4);
        return 0;
    }
    std::uint8_t* value = c.mem.host_ptr(c.a[3], capacity, kPageWrite);
    if (capacity != 0 && value == nullptr) return -EFAULT;
    return result_of(::syscall(SYS_getsockopt, static_cast<int>(c.a[0]), level, name, value, len));
}

struct HostMessage {
    msghdr msg{};
    std::vector<iovec> iov;
    std::vector<std::uint8_t> control;
};

std::int32_t build_host_message(Ctx& c, const g::msghdr32& gm, bool receiving, HostMessage& out) {
    if (gm.msg_iovlen > IOV_MAX) return -EMSGSIZE;
    out.iov.resize(gm.msg_iovlen);
    for (std::uint32_t i = 0; i < gm.msg_iovlen; ++i) {
        g::iovec32 v;
        if (!read_guest(c.mem, gm.msg_iov + i * sizeof(g::iovec32), v)) return -EFAULT;
        std::uint8_t* base = c.mem.host_ptr(v.iov_base, v.iov_len, receiving ? kPageWrite : kPageRead);
        if (v.iov_len != 0 && base == nullptr) return -EFAULT;
        out.iov[i] = {base, v.iov_len};
    }
    out.msg.msg_iov = out.iov.data();
    out.msg.msg_iovlen = gm.msg_iovlen;
    if (gm.msg_name != 0) {
        std::uint8_t* name = c.mem.host_ptr(gm.msg_name, gm.msg_namelen, receiving ? kPageWrite : kPageRead);
        if (gm.msg_namelen != 0 && name == nullptr) return -EFAULT;
        out.msg.msg_name = name;
        out.msg.msg_namelen = gm.msg_namelen;
    }
    out.msg.msg_flags = gm.msg_flags;
    return 0;
}

// Guest control messages (12-byte headers, 4-byte alignment) -> host (16-byte, 8-byte).
bool control_to_host(GuestMemory& m, std::uint32_t addr, std::uint32_t len, std::vector<std::uint8_t>& out) {
    const std::uint8_t* src = m.host_ptr(addr, len, kPageRead);
    if (src == nullptr) return false;
    std::uint32_t offset = 0;
    while (offset + sizeof(g::cmsghdr32) <= len) {
        g::cmsghdr32 header;
        std::memcpy(&header, src + offset, sizeof header);
        if (header.cmsg_len < sizeof header || offset + header.cmsg_len > len) break;
        const std::uint32_t data_len = header.cmsg_len - sizeof header;
        const std::size_t host_offset = out.size();
        out.resize(host_offset + CMSG_SPACE(data_len));
        auto* host = reinterpret_cast<cmsghdr*>(out.data() + host_offset);
        host->cmsg_len = CMSG_LEN(data_len);
        host->cmsg_level = header.cmsg_level;
        host->cmsg_type = header.cmsg_type;
        std::memcpy(CMSG_DATA(host), src + offset + sizeof header, data_len);
        offset += (header.cmsg_len + 3) & ~3u;
    }
    return true;
}

std::uint32_t control_to_guest(msghdr& host, std::uint8_t* dst, std::uint32_t capacity, int& flags) {
    std::uint32_t offset = 0;
    for (cmsghdr* h = CMSG_FIRSTHDR(&host); h != nullptr; h = CMSG_NXTHDR(&host, h)) {
        const std::size_t data_len = h->cmsg_len - CMSG_LEN(0);
        const auto needed = static_cast<std::uint32_t>(sizeof(g::cmsghdr32) + data_len);
        if (offset + needed > capacity) {
            flags |= MSG_CTRUNC;
            break;
        }
        const g::cmsghdr32 header{needed, h->cmsg_level, h->cmsg_type};
        std::memcpy(dst + offset, &header, sizeof header);
        std::memcpy(dst + offset + sizeof header, CMSG_DATA(h), data_len);
        offset = std::min(capacity, offset + ((needed + 3) & ~3u));
    }
    return offset;
}

std::int32_t sys_sendmsg(Ctx& c) {
    g::msghdr32 gm;
    if (!read_guest(c.mem, c.a[1], gm)) return -EFAULT;
    HostMessage hm;
    if (const std::int32_t err = build_host_message(c, gm, false, hm); err != 0) return err;
    if (gm.msg_controllen != 0) {
        if (!control_to_host(c.mem, gm.msg_control, gm.msg_controllen, hm.control)) return -EFAULT;
        hm.msg.msg_control = hm.control.data();
        hm.msg.msg_controllen = hm.control.size();
    }
    return result_of(::sendmsg(static_cast<int>(c.a[0]), &hm.msg, static_cast<int>(c.a[2])));
}

std::int32_t sys_recvmsg(Ctx& c) {
    g::msghdr32 gm;
    if (!read_guest(c.mem, c.a[1], gm) || c.mem.host_ptr(c.a[1], sizeof gm, kPageWrite) == nullptr) return -EFAULT;
    HostMessage hm;
    if (const std::int32_t err = build_host_message(c, gm, true, hm); err != 0) return err;
    std::uint8_t* guest_control = nullptr;
    if (gm.msg_controllen != 0) {
        guest_control = c.mem.host_ptr(gm.msg_control, gm.msg_controllen, kPageWrite);
        if (guest_control == nullptr) return -EFAULT;
        hm.control.resize(static_cast<std::size_t>(gm.msg_controllen) * 2 + 64);
        hm.msg.msg_control = hm.control.data();
        hm.msg.msg_controllen = hm.control.size();
    }
    const ssize_t n = ::recvmsg(static_cast<int>(c.a[0]), &hm.msg, static_cast<int>(c.a[2]));
    if (n < 0) return -errno;
    int flags = hm.msg.msg_flags;
    gm.msg_controllen = guest_control != nullptr ? control_to_guest(hm.msg, guest_control, gm.msg_controllen, flags) : 0;
    gm.msg_namelen = static_cast<std::uint32_t>(hm.msg.msg_namelen);
    gm.msg_flags = flags;
    write_guest(c.mem, c.a[1], gm);
    return static_cast<std::int32_t>(n);
}

std::int32_t sys_epoll_wait(Ctx& c) {
    const int max_events = static_cast<int>(c.a[2]);
    if (max_events <= 0 || max_events > 65536) return -EINVAL;
    std::uint8_t* events = c.mem.host_ptr(c.a[1], static_cast<std::uint64_t>(max_events) * sizeof(epoll_event), kPageWrite);
    if (events == nullptr) return -EFAULT;
    // epoll_pwait's guest signal mask is ignored: guest masks are emulated.
    return result_of(::epoll_pwait(static_cast<int>(c.a[0]), reinterpret_cast<epoll_event*>(events), max_events,
                                   static_cast<int>(c.a[3]), nullptr));
}

bool read_itimerval(GuestMemory& m, std::uint32_t addr, itimerval& out) {
    g::itimerval32 t;
    if (!read_guest(m, addr, t)) return false;
    out.it_interval = {t.it_interval.tv_sec, t.it_interval.tv_usec};
    out.it_value = {t.it_value.tv_sec, t.it_value.tv_usec};
    return true;
}

bool write_itimerval(GuestMemory& m, std::uint32_t addr, const itimerval& v) {
    const g::itimerval32 t{{static_cast<std::int32_t>(v.it_interval.tv_sec), static_cast<std::int32_t>(v.it_interval.tv_usec)},
                           {static_cast<std::int32_t>(v.it_value.tv_sec), static_cast<std::int32_t>(v.it_value.tv_usec)}};
    return write_guest(m, addr, t);
}

std::int32_t sys_setitimer(Ctx& c) {
    itimerval new_value{};
    itimerval old_value{};
    if (c.a[1] != 0 && !read_itimerval(c.mem, c.a[1], new_value)) return -EFAULT;
    if (::syscall(SYS_setitimer, static_cast<int>(c.a[0]), c.a[1] != 0 ? &new_value : nullptr, &old_value) != 0) return -errno;
    if (c.a[2] != 0 && !write_itimerval(c.mem, c.a[2], old_value)) return -EFAULT;
    return 0;
}

std::int32_t sys_getitimer(Ctx& c) {
    itimerval value{};
    if (::syscall(SYS_getitimer, static_cast<int>(c.a[0]), &value) != 0) return -errno;
    return write_itimerval(c.mem, c.a[1], value) ? 0 : -EFAULT;
}

void fill_rusage32(g::rusage32& out, const rusage& ru) {
    out.ru_utime = {static_cast<std::int32_t>(ru.ru_utime.tv_sec), static_cast<std::int32_t>(ru.ru_utime.tv_usec)};
    out.ru_stime = {static_cast<std::int32_t>(ru.ru_stime.tv_sec), static_cast<std::int32_t>(ru.ru_stime.tv_usec)};
    const long fields[14] = {ru.ru_maxrss, ru.ru_ixrss,  ru.ru_idrss,  ru.ru_isrss,  ru.ru_minflt,   ru.ru_majflt, ru.ru_nswap,
                             ru.ru_inblock, ru.ru_oublock, ru.ru_msgsnd, ru.ru_msgrcv, ru.ru_nsignals, ru.ru_nvcsw, ru.ru_nivcsw};
    for (int i = 0; i < 14; ++i) out.fields[i] = static_cast<std::int32_t>(fields[i]);
}

std::int32_t sys_sysinfo(Ctx& c) {
    struct sysinfo si;
    if (::sysinfo(&si) != 0) return -errno;
    const std::uint64_t unit = si.mem_unit != 0 ? si.mem_unit : 1;
    const std::uint64_t bytes[8] = {si.totalram * unit, si.freeram * unit,   si.sharedram * unit, si.bufferram * unit,
                                    si.totalswap * unit, si.freeswap * unit, si.totalhigh * unit, si.freehigh * unit};
    // Like the kernel's compat path: grow mem_unit until every value fits in 32 bits.
    std::uint64_t guest_unit = 1;
    for (std::uint64_t b : bytes) {
        while (b / guest_unit > 0xFFFFFFFFull) guest_unit <<= 1;
    }
    g::sysinfo32 out{};
    out.uptime = static_cast<std::int32_t>(si.uptime);
    for (int i = 0; i < 3; ++i) out.loads[i] = static_cast<std::uint32_t>(si.loads[i]);
    out.totalram = static_cast<std::uint32_t>(bytes[0] / guest_unit);
    out.freeram = static_cast<std::uint32_t>(bytes[1] / guest_unit);
    out.sharedram = static_cast<std::uint32_t>(bytes[2] / guest_unit);
    out.bufferram = static_cast<std::uint32_t>(bytes[3] / guest_unit);
    out.totalswap = static_cast<std::uint32_t>(bytes[4] / guest_unit);
    out.freeswap = static_cast<std::uint32_t>(bytes[5] / guest_unit);
    out.procs = si.procs;
    out.totalhigh = static_cast<std::uint32_t>(bytes[6] / guest_unit);
    out.freehigh = static_cast<std::uint32_t>(bytes[7] / guest_unit);
    out.mem_unit = static_cast<std::uint32_t>(guest_unit);
    return write_guest(c.mem, c.a[0], out) ? 0 : -EFAULT;
}

std::int32_t sys_utimensat(Ctx& c, bool time64) {
    std::string host_path;
    const char* path = nullptr;
    if (c.a[1] != 0) {
        const char* guest = guest_cstr(c.mem, c.a[1]);
        if (guest == nullptr) return -EFAULT;
        host_path = c.proc.translate_path(guest);
        path = host_path.c_str();
    }
    timespec times[2];
    timespec* tp = nullptr;
    if (c.a[2] != 0) {
        const std::uint32_t step = time64 ? sizeof(g::timespec64) : sizeof(g::timespec32);
        if (!read_timespec(c.mem, c.a[2], time64, times[0]) || !read_timespec(c.mem, c.a[2] + step, time64, times[1])) {
            return -EFAULT;
        }
        tp = times;
    }
    return result_of(::syscall(SYS_utimensat, static_cast<int>(c.a[0]), path, tp, static_cast<int>(c.a[3])));
}

bool read_itimerspec(GuestMemory& m, std::uint32_t addr, itimerspec& out) {
    g::itimerspec32 t;
    if (!read_guest(m, addr, t)) return false;
    out.it_interval = {t.it_interval.tv_sec, t.it_interval.tv_nsec};
    out.it_value = {t.it_value.tv_sec, t.it_value.tv_nsec};
    return true;
}

bool write_itimerspec(GuestMemory& m, std::uint32_t addr, const itimerspec& v) {
    const g::itimerspec32 t{{static_cast<std::int32_t>(v.it_interval.tv_sec), static_cast<std::int32_t>(v.it_interval.tv_nsec)},
                            {static_cast<std::int32_t>(v.it_value.tv_sec), static_cast<std::int32_t>(v.it_value.tv_nsec)}};
    return write_guest(m, addr, t);
}

std::uint64_t join64(std::uint32_t lo, std::uint32_t hi) {
    return static_cast<std::uint64_t>(lo) | (static_cast<std::uint64_t>(hi) << 32);
}

}  // namespace

const char* syscall_name(std::uint32_t nr) {
    for (const auto& entry : kSyscallNames) {
        if (entry.nr == nr) return entry.name;
    }
    return "?";
}

bool handle_syscall(Process& proc, GuestThread& thread) {
    auto& regs = thread.regs();
    Ctx c{proc, thread, proc.memory(), {regs[0], regs[1], regs[2], regs[3], regs[4], regs[5]}};
    const std::uint32_t nr = regs[7];
    std::int32_t res = -ENOSYS;
    const auto guest_tid = [&] {
        return thread.tid != 0 ? thread.tid : static_cast<std::int32_t>(::syscall(SYS_gettid));
    };
    record_thread_activity(guest_tid(), ThreadActivityKind::kSyscall, nr);
    // Marks the syscall finished on every exit path, so a thread sitting inside one (a blocking
    // futex, a poll) reads differently from a thread that merely stopped calling out.
    struct ActivityDone {
        std::int32_t tid;
        ~ActivityDone() { record_thread_activity_done(tid); }
    } activity_done{guest_tid()};

    switch (nr) {
    case NR_exit:
        // Ends only the calling thread; Process decides what that means for the process.
        if (trace_enabled()) log("exit(%d)", static_cast<int>(c.a[0]));
        thread.exit_status = static_cast<int>(c.a[0] & 0xff);
        return false;
    case NR_exit_group:
        if (trace_enabled()) log("exit_group(%d)", static_cast<int>(c.a[0]));
        if (proc.thread_count() > 1) std::_Exit(static_cast<int>(c.a[0] & 0xff));
        proc.request_exit(static_cast<int>(c.a[0] & 0xff));
        return false;
    case NR_clone: {
        constexpr std::uint32_t kRequired = 0x00000100 | 0x00010000;  // CLONE_VM | CLONE_THREAD
        if ((c.a[0] & kRequired) != kRequired) {
            if (proc.first_time(nr)) log("clone without CLONE_VM|CLONE_THREAD (fork) is refused");
            res = -EPERM;
            break;
        }
        res = proc.clone_thread(thread, c.a[0], c.a[1], c.a[2], c.a[3], c.a[4]);
        break;
    }

    case NR_read: res = sys_read_write(c, false); break;
    case NR_write: res = sys_read_write(c, true); break;
    case NR_pread64: res = sys_pread_pwrite(c, false); break;
    case NR_pwrite64: res = sys_pread_pwrite(c, true); break;
    case NR_readv: res = sys_readv_writev(c, false); break;
    case NR_writev: res = sys_readv_writev(c, true); break;
    case NR_close: res = result_of(::close(static_cast<int>(c.a[0]))); break;
    case NR_dup: res = result_of(::dup(static_cast<int>(c.a[0]))); break;
    case NR_dup3: res = result_of(::dup3(static_cast<int>(c.a[0]), static_cast<int>(c.a[1]), static_cast<int>(c.a[2]))); break;
    case NR_pipe2: res = sys_pipe2(c); break;
    case NR_lseek: res = sys_lseek(c); break;
    case NR__llseek: res = sys_llseek(c); break;
    case NR_fcntl64: res = sys_fcntl64(c); break;
    case NR_ioctl: res = sys_ioctl(c); break;

    case NR_openat: {
        const char* guest_path = guest_cstr(c.mem, c.a[1]);
        const bool watched = open_worth_watching(guest_path);
        if (guest_path != nullptr && proc.open_synthetic_file(guest_path, static_cast<int>(c.a[2]), res)) {
            if (watched) note_watched_open(guest_path, res);
            break;
        }
        res = sys_path_call(c, 1, [](Ctx& x, const char* p) -> long {
            return ::syscall(SYS_openat, static_cast<int>(x.a[0]), p, static_cast<int>(x.a[2]), static_cast<mode_t>(x.a[3]));
        });
        if (watched) note_watched_open(guest_path, res);
        break;
    }
    case NR_faccessat:
        res = sys_path_call(c, 1, [](Ctx& x, const char* p) -> long {
            return ::syscall(SYS_faccessat, static_cast<int>(x.a[0]), p, static_cast<int>(x.a[2]));
        });
        break;
    case NR_faccessat2:
        res = sys_path_call(c, 1, [](Ctx& x, const char* p) -> long {
            return ::syscall(SYS_faccessat2, static_cast<int>(x.a[0]), p, static_cast<int>(x.a[2]), static_cast<int>(x.a[3]));
        });
        break;
    case NR_unlinkat:
        res = sys_path_call(c, 1, [](Ctx& x, const char* p) -> long {
            return ::unlinkat(static_cast<int>(x.a[0]), p, static_cast<int>(x.a[2]));
        });
        break;
    case NR_mkdirat:
        res = sys_path_call(c, 1, [](Ctx& x, const char* p) -> long {
            return ::mkdirat(static_cast<int>(x.a[0]), p, static_cast<mode_t>(x.a[2]));
        });
        break;
    case NR_readlinkat: {
        const char* path = guest_cstr(c.mem, c.a[1]);
        std::uint8_t* buf = c.mem.host_ptr(c.a[2], c.a[3], kPageWrite);
        if (!path || (c.a[3] != 0 && !buf)) {
            res = -EFAULT;
            break;
        }
        if (std::strcmp(path, "/proc/self/exe") == 0) {
            const std::string& exe = proc.exe_path();
            const std::size_t n = std::min<std::size_t>(exe.size(), c.a[3]);
            std::memcpy(buf, exe.data(), n);
            res = static_cast<std::int32_t>(n);
            break;
        }
        const std::string host_path = proc.translate_path(path);
        res = result_of(::readlinkat(static_cast<int>(c.a[0]), host_path.c_str(), reinterpret_cast<char*>(buf), c.a[3]));
        break;
    }
    case NR_getcwd: {
        std::uint8_t* buf = c.mem.host_ptr(c.a[0], c.a[1], kPageWrite);
        res = (c.a[1] != 0 && !buf) ? -EFAULT : result_of(::syscall(SYS_getcwd, buf, c.a[1]));
        break;
    }
    case NR_fstat64: {
        struct stat st;
        const int rc = ::fstat(static_cast<int>(c.a[0]), &st);
        res = sys_stat_common(c, rc, st, c.a[1]);
        break;
    }
    case NR_fstatat64: res = sys_fstatat64(c); break;
    case NR_statx: res = sys_statx(c); break;
    case NR_fstatfs64: {
        if (c.a[1] != sizeof(g::statfs64) && c.a[1] != g::kStatfs64UserSize) {
            res = -EINVAL;
            break;
        }
        struct statfs st;
        const int rc = ::fstatfs(static_cast<int>(c.a[0]), &st);
        res = sys_statfs_common(c, rc, st, c.a[2]);
        break;
    }
    case NR_statfs64: {
        const char* path = guest_cstr(c.mem, c.a[0]);
        if (!path) {
            res = -EFAULT;
            break;
        }
        if (c.a[1] != sizeof(g::statfs64) && c.a[1] != g::kStatfs64UserSize) {
            res = -EINVAL;
            break;
        }
        struct statfs st;
        const int rc = ::statfs(proc.translate_path(path).c_str(), &st);
        res = sys_statfs_common(c, rc, st, c.a[2]);
        break;
    }

    case NR_brk: {
        std::lock_guard<std::mutex> lock(proc.mm_mutex());
        res = sys_brk(c);
        break;
    }
    case NR_mmap2: {
        std::lock_guard<std::mutex> lock(proc.mm_mutex());
        res = sys_mmap2(c);
        break;
    }
    case NR_munmap: {
        std::lock_guard<std::mutex> lock(proc.mm_mutex());
        res = sys_munmap(c);
        break;
    }
    case NR_mprotect: {
        std::lock_guard<std::mutex> lock(proc.mm_mutex());
        res = sys_mprotect(c);
        break;
    }
    case NR_madvise: {
        std::lock_guard<std::mutex> lock(proc.mm_mutex());
        res = sys_madvise(c);
        break;
    }
    case NR_mremap: res = -ENOMEM; break;

    case NR_ARM_set_tls:
        thread.set_tls(c.a[0]);
        res = 0;
        break;
    case NR_ARM_get_tls: res = static_cast<std::int32_t>(thread.tls()); break;
    case NR_ARM_cacheflush:
        if (c.a[1] > c.a[0]) proc.invalidate(c.a[0], c.a[1] - c.a[0]);
        res = 0;
        break;

    case NR_set_tid_address:
        thread.clear_child_tid = c.a[0];
        res = guest_tid();
        break;
    case NR_getpid: res = result_of(::getpid()); break;
    case NR_getppid: res = result_of(::getppid()); break;
    case NR_gettid: res = guest_tid(); break;
    case NR_getuid32: res = static_cast<std::int32_t>(::getuid()); break;
    case NR_geteuid32: res = static_cast<std::int32_t>(::geteuid()); break;
    case NR_getgid32: res = static_cast<std::int32_t>(::getgid()); break;
    case NR_getegid32: res = static_cast<std::int32_t>(::getegid()); break;

    case NR_rt_sigaction: {
        std::lock_guard<std::mutex> lock(proc.signal_mutex());
        res = sys_rt_sigaction(c);
        break;
    }
    case NR_rt_sigprocmask: res = sys_rt_sigprocmask(c); break;
    case NR_sigaltstack: res = sys_sigaltstack(c); break;
    case NR_kill:
        res = sys_send_signal(c, true, static_cast<std::int32_t>(c.a[0]), 0, c.a[1], nullptr, SI_USER);
        break;
    case NR_tkill:
        res = sys_send_signal(c, false, -1, static_cast<std::int32_t>(c.a[0]), c.a[1], nullptr, SI_TKILL);
        break;
    case NR_tgkill:
        res = sys_send_signal(c, false, static_cast<std::int32_t>(c.a[0]), static_cast<std::int32_t>(c.a[1]), c.a[2],
                              nullptr, SI_TKILL);
        break;
    case NR_rt_sigreturn:
    case NR_sigreturn:
        if (!proc.sigreturn(thread, nr == NR_rt_sigreturn)) {
            log("%s with an unreadable signal frame", syscall_name(nr));
            proc.request_exit(128 + SIGSEGV);
            return false;
        }
        c.no_result = true;
        break;

    case NR_clock_gettime: res = sys_clock_get(c, false, false); break;
    case NR_clock_getres: res = sys_clock_get(c, true, false); break;
    case NR_clock_gettime64: res = sys_clock_get(c, false, true); break;
    case NR_clock_getres_time64: res = sys_clock_get(c, true, true); break;
    case NR_gettimeofday: res = sys_gettimeofday(c); break;
    case NR_nanosleep: res = sys_nanosleep(c); break;
    case NR_clock_nanosleep: res = sys_clock_nanosleep(c, false); break;
    case NR_clock_nanosleep_time64: res = sys_clock_nanosleep(c, true); break;

    case NR_futex: res = sys_futex(c, false); break;
    case NR_futex_time64: res = sys_futex(c, true); break;
    case NR_sched_yield: res = result_of(::sched_yield()); break;
    case NR_sched_getaffinity: {
        std::uint8_t* mask = c.mem.host_ptr(c.a[2], c.a[1], kPageWrite);
        res = !mask ? -EFAULT : result_of(::syscall(SYS_sched_getaffinity, static_cast<pid_t>(c.a[0]), c.a[1], mask));
        break;
    }
    case NR_getrandom: {
        std::uint8_t* buf = c.mem.host_ptr(c.a[0], c.a[1], kPageWrite);
        res = (c.a[1] != 0 && !buf) ? -EFAULT : result_of(::syscall(SYS_getrandom, buf, c.a[1], c.a[2]));
        break;
    }
    case NR_prctl: res = sys_prctl(c); break;
    case NR_uname: res = sys_uname(c); break;
    case NR_ugetrlimit: res = sys_ugetrlimit(c); break;
    case NR_prlimit64: res = sys_prlimit64(c); break;

    case NR_personality: {
        const std::uint32_t previous = proc.personality;
        if (c.a[0] != 0xFFFFFFFFu) proc.personality = c.a[0];
        res = static_cast<std::int32_t>(previous);
        break;
    }
    case NR_sched_getscheduler: res = result_of(::sched_getscheduler(static_cast<pid_t>(c.a[0]))); break;
    case NR_socket:
        res = result_of(::syscall(SYS_socket, static_cast<int>(c.a[0]), static_cast<int>(c.a[1]), static_cast<int>(c.a[2])));
        break;
    case NR_connect: {
        std::uint8_t* addr = c.mem.host_ptr(c.a[1], c.a[2], kPageRead);
        res = (c.a[2] != 0 && !addr) ? -EFAULT
                                     : result_of(::syscall(SYS_connect, static_cast<int>(c.a[0]), addr, c.a[2]));
        break;
    }
    case NR_rt_tgsigqueueinfo: {
        g::siginfo32 queued{};
        if (c.a[3] != 0 && !read_guest(c.mem, c.a[3], queued)) {
            res = -EFAULT;
            break;
        }
        res = sys_send_signal(c, false, static_cast<std::int32_t>(c.a[0]), static_cast<std::int32_t>(c.a[1]), c.a[2],
                              c.a[3] != 0 ? &queued : nullptr, SI_QUEUE);
        break;
    }

    // ---- files and directories
    case NR_getdents64: {
        std::uint8_t* buf = c.mem.host_ptr(c.a[1], c.a[2], kPageWrite);
        res = (c.a[2] != 0 && !buf) ? -EFAULT : result_of(::syscall(SYS_getdents64, static_cast<int>(c.a[0]), buf, c.a[2]));
        break;
    }
    case NR_renameat:
        res = sys_two_paths(c, 1, 3, [](Ctx& x, const char* a, const char* b) -> long {
            return ::renameat(static_cast<int>(x.a[0]), a, static_cast<int>(x.a[2]), b);
        });
        break;
    case NR_renameat2:
        res = sys_two_paths(c, 1, 3, [](Ctx& x, const char* a, const char* b) -> long {
            return ::syscall(SYS_renameat2, static_cast<int>(x.a[0]), a, static_cast<int>(x.a[2]), b, x.a[4]);
        });
        break;
    case NR_linkat:
        res = sys_two_paths(c, 1, 3, [](Ctx& x, const char* a, const char* b) -> long {
            return ::linkat(static_cast<int>(x.a[0]), a, static_cast<int>(x.a[2]), b, static_cast<int>(x.a[4]));
        });
        break;
    case NR_symlinkat: {
        const char* target = guest_cstr(c.mem, c.a[0]);
        const char* link = guest_cstr(c.mem, c.a[2]);
        if (!target || !link) {
            res = -EFAULT;
            break;
        }
        res = result_of(::symlinkat(target, static_cast<int>(c.a[1]), proc.translate_path(link).c_str()));
        break;
    }
    case NR_fchmodat:
        res = sys_path_call(c, 1, [](Ctx& x, const char* p) -> long {
            return ::syscall(SYS_fchmodat, static_cast<int>(x.a[0]), p, static_cast<mode_t>(x.a[2]));
        });
        break;
    case NR_fchownat:
        res = sys_path_call(c, 1, [](Ctx& x, const char* p) -> long {
            return ::fchownat(static_cast<int>(x.a[0]), p, x.a[2], x.a[3], static_cast<int>(x.a[4]));
        });
        break;
    case NR_chdir:
        res = sys_path_call(c, 0, [](Ctx&, const char* p) -> long { return ::chdir(p); });
        break;
    case NR_truncate64:
        res = sys_path_call(c, 0, [](Ctx& x, const char* p) -> long {
            return ::truncate(p, static_cast<off_t>(join64(x.a[2], x.a[3])));
        });
        break;
    case NR_ftruncate64:
        res = result_of(::ftruncate(static_cast<int>(c.a[0]), static_cast<off_t>(join64(c.a[2], c.a[3]))));
        break;
    case NR_utimensat: res = sys_utimensat(c, false); break;
    case NR_utimensat_time64: res = sys_utimensat(c, true); break;
    case NR_fchmod: res = result_of(::fchmod(static_cast<int>(c.a[0]), static_cast<mode_t>(c.a[1]))); break;
    case NR_fchown32: res = result_of(::fchown(static_cast<int>(c.a[0]), c.a[1], c.a[2])); break;
    case NR_fsync: res = result_of(::fsync(static_cast<int>(c.a[0]))); break;
    case NR_fdatasync: res = result_of(::fdatasync(static_cast<int>(c.a[0]))); break;
    case NR_flock: res = result_of(::flock(static_cast<int>(c.a[0]), static_cast<int>(c.a[1]))); break;
    case NR_umask: res = static_cast<std::int32_t>(::umask(static_cast<mode_t>(c.a[0]))); break;
    case NR_fchdir: res = result_of(::fchdir(static_cast<int>(c.a[0]))); break;
    case NR_dup2: res = result_of(::dup2(static_cast<int>(c.a[0]), static_cast<int>(c.a[1]))); break;
    case NR_pipe: {
        std::uint8_t* out = c.mem.host_ptr(c.a[0], 8, kPageWrite);
        int fds[2];
        if (!out) {
            res = -EFAULT;
        } else if (::pipe(fds) != 0) {
            res = -errno;
        } else {
            std::memcpy(out, fds, sizeof fds);
            res = 0;
        }
        break;
    }
    case NR_memfd_create: {
        const char* name = guest_cstr(c.mem, c.a[0]);
        res = !name ? -EFAULT : result_of(::syscall(SYS_memfd_create, name, c.a[1]));
        break;
    }
    case NR_msync: {
        const std::uint64_t size = page_round_up(c.a[1]);
        std::lock_guard<std::mutex> lock(proc.mm_mutex());
        if (c.a[0] & kPageMask) {
            res = -EINVAL;
        } else if (!c.mem.accessible(c.a[0], size, 0)) {
            res = -ENOMEM;
        } else {
            res = result_of(::msync(c.mem.base() + c.a[0], size, static_cast<int>(c.a[2])));
        }
        break;
    }
    case NR_mlock:
    case NR_munlock:
        res = 0;
        break;

    // ---- waiting
    case NR_poll: res = sys_poll(c, false, false); break;
    case NR_ppoll: res = sys_poll(c, true, false); break;
    case NR_ppoll_time64: res = sys_poll(c, true, true); break;
    case NR__newselect: res = sys_select(c, false, false); break;
    case NR_pselect6: res = sys_select(c, true, false); break;
    case NR_pselect6_time64: res = sys_select(c, true, true); break;
    case NR_eventfd2: res = result_of(::syscall(SYS_eventfd2, c.a[0], static_cast<int>(c.a[1]))); break;
    case NR_epoll_create1: res = result_of(::epoll_create1(static_cast<int>(c.a[0]))); break;
    case NR_epoll_ctl: {
        std::uint8_t* event = c.a[3] != 0 ? c.mem.host_ptr(c.a[3], sizeof(epoll_event), kPageRead) : nullptr;
        if (c.a[3] != 0 && !event) {
            res = -EFAULT;
        } else {
            res = result_of(::epoll_ctl(static_cast<int>(c.a[0]), static_cast<int>(c.a[1]), static_cast<int>(c.a[2]),
                                        reinterpret_cast<epoll_event*>(event)));
        }
        break;
    }
    case NR_epoll_wait:
    case NR_epoll_pwait:
        res = sys_epoll_wait(c);
        break;
    case NR_timerfd_create: res = result_of(::timerfd_create(static_cast<int>(c.a[0]), static_cast<int>(c.a[1]))); break;
    case NR_timerfd_settime: {
        itimerspec new_value{};
        itimerspec old_value{};
        if (!read_itimerspec(c.mem, c.a[2], new_value)) {
            res = -EFAULT;
            break;
        }
        if (::timerfd_settime(static_cast<int>(c.a[0]), static_cast<int>(c.a[1]), &new_value, &old_value) != 0) {
            res = -errno;
            break;
        }
        res = (c.a[3] != 0 && !write_itimerspec(c.mem, c.a[3], old_value)) ? -EFAULT : 0;
        break;
    }
    case NR_timerfd_gettime: {
        itimerspec value{};
        if (::timerfd_gettime(static_cast<int>(c.a[0]), &value) != 0) {
            res = -errno;
            break;
        }
        res = write_itimerspec(c.mem, c.a[1], value) ? 0 : -EFAULT;
        break;
    }

    // ---- sockets
    case NR_bind: res = sys_sockaddr_in(c, SYS_bind); break;
    case NR_listen: res = result_of(::listen(static_cast<int>(c.a[0]), static_cast<int>(c.a[1]))); break;
    case NR_accept: res = sys_sockaddr_out(c, SYS_accept, false); break;
    case NR_accept4: res = sys_sockaddr_out(c, SYS_accept4, true); break;
    case NR_getsockname: res = sys_sockaddr_out(c, SYS_getsockname, false); break;
    case NR_getpeername: res = sys_sockaddr_out(c, SYS_getpeername, false); break;
    case NR_socketpair: {
        std::uint8_t* out = c.mem.host_ptr(c.a[3], 8, kPageWrite);
        int sv[2];
        if (!out) {
            res = -EFAULT;
        } else if (::socketpair(static_cast<int>(c.a[0]), static_cast<int>(c.a[1]), static_cast<int>(c.a[2]), sv) != 0) {
            res = -errno;
        } else {
            std::memcpy(out, sv, sizeof sv);
            res = 0;
        }
        break;
    }
    case NR_send: res = sys_sendto(c, 0, 0); break;
    case NR_sendto: res = sys_sendto(c, c.a[4], c.a[5]); break;
    case NR_recv: res = sys_recvfrom(c, 0, 0); break;
    case NR_recvfrom: res = sys_recvfrom(c, c.a[4], c.a[5]); break;
    case NR_shutdown: res = result_of(::shutdown(static_cast<int>(c.a[0]), static_cast<int>(c.a[1]))); break;
    case NR_setsockopt: res = sys_setsockopt(c); break;
    case NR_getsockopt: res = sys_getsockopt(c); break;
    case NR_sendmsg: res = sys_sendmsg(c); break;
    case NR_recvmsg: res = sys_recvmsg(c); break;

    // ---- time, resources, processes
    case NR_setitimer: res = sys_setitimer(c); break;
    case NR_getitimer: res = sys_getitimer(c); break;
    case NR_times: {
        tms t;
        const clock_t r = ::times(&t);
        if (r == static_cast<clock_t>(-1)) {
            res = -errno;
            break;
        }
        const g::tms32 out{static_cast<std::int32_t>(t.tms_utime), static_cast<std::int32_t>(t.tms_stime),
                           static_cast<std::int32_t>(t.tms_cutime), static_cast<std::int32_t>(t.tms_cstime)};
        res = (c.a[0] != 0 && !write_guest(c.mem, c.a[0], out)) ? -EFAULT : static_cast<std::int32_t>(r);
        break;
    }
    case NR_getrusage: {
        rusage ru;
        if (::getrusage(static_cast<int>(c.a[0]), &ru) != 0) {
            res = -errno;
            break;
        }
        g::rusage32 out{};
        fill_rusage32(out, ru);
        res = write_guest(c.mem, c.a[1], out) ? 0 : -EFAULT;
        break;
    }
    case NR_wait4: {
        int status = 0;
        rusage ru{};
        const pid_t r = ::wait4(static_cast<pid_t>(c.a[0]), &status, static_cast<int>(c.a[2]), &ru);
        if (r < 0) {
            res = -errno;
            break;
        }
        if (c.a[1] != 0 && !write_guest(c.mem, c.a[1], static_cast<std::int32_t>(status))) {
            res = -EFAULT;
            break;
        }
        if (c.a[3] != 0) {
            g::rusage32 out{};
            fill_rusage32(out, ru);
            write_guest(c.mem, c.a[3], out);
        }
        res = r;
        break;
    }
    case NR_sysinfo: res = sys_sysinfo(c); break;
    case NR_getpriority: res = result_of(::syscall(SYS_getpriority, static_cast<int>(c.a[0]), static_cast<int>(c.a[1]))); break;
    case NR_setpriority:
        res = result_of(::syscall(SYS_setpriority, static_cast<int>(c.a[0]), static_cast<int>(c.a[1]), static_cast<int>(c.a[2])));
        break;
    case NR_sched_setaffinity: {
        std::uint8_t* mask = c.mem.host_ptr(c.a[2], c.a[1], kPageRead);
        res = !mask ? -EFAULT : result_of(::syscall(SYS_sched_setaffinity, static_cast<pid_t>(c.a[0]), c.a[1], mask));
        break;
    }
    case NR_getpgid: res = result_of(::getpgid(static_cast<pid_t>(c.a[0]))); break;
    case NR_setsid: res = result_of(::setsid()); break;
    case NR_rt_sigsuspend: res = sys_sigsuspend(c, true); break;
    case NR_sigsuspend: res = sys_sigsuspend(c, false); break;
    case NR_rt_sigpending: {
        const std::uint64_t pending = thread.pending_signals();
        if (c.a[1] != 8) {
            res = -EINVAL;
        } else {
            res = write_guest(c.mem, c.a[0], pending) ? 0 : -EFAULT;
        }
        break;
    }

    default:
        if (proc.first_time(nr)) log("unimplemented syscall %s (%u)", syscall_name(nr), nr);
        res = -ENOSYS;
        break;
    }

    if (trace_enabled()) {
        log("%s(0x%x, 0x%x, 0x%x, 0x%x) = %d", syscall_name(nr), c.a[0], c.a[1], c.a[2], c.a[3], res);
    }
    if (c.stop) return false;
    if (!c.no_result) regs[0] = static_cast<std::uint32_t>(res);
    return true;
}

}  // namespace zb
