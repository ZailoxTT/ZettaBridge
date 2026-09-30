#pragma once

#include <array>
#include <atomic>
#include <bitset>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "zb/guest_abi.h"
#include "zb/guest_memory.h"
#include "zb/guest_thread.h"

namespace Dynarmic {
class ExclusiveMonitor;
}

namespace zb {

// One guest process: its address space, emulated kernel state and threads.
class Process {
public:
    using HostCallHandler = std::function<bool(std::uint32_t index, GuestThread& thread)>;

    static constexpr std::uint32_t kStackTop = 0xFF000000;
    static constexpr std::uint32_t kStackSize = 8 * 1024 * 1024;
    static constexpr std::uint32_t kMmapLimit = 0xFE000000;
    // A PIE main executable is placed below this address.
    static constexpr std::uint32_t kExecutableLimit = 0x40000000;
    static constexpr std::size_t kMaxThreads = 256;

    Process();
    ~Process();
    Process(const Process&) = delete;
    Process& operator=(const Process&) = delete;

    // Host directory holding the arm32 Android system files (system/bin/linker, system/lib/...).
    void set_sysroot(std::string dir) { sysroot_ = std::move(dir); }
    // See GuestThread: precise memory faults at a speed cost. Defaults to $ZB_PRECISE_FAULTS.
    void set_precise_faults(bool enabled) { precise_faults_ = enabled; }

    // Loads an arm32 executable (and its PT_INTERP), builds its stack and runs it until the
    // process exits. Returns the guest exit status, or 128 + signal for a fatal guest fault.
    int run(const std::string& path, const std::vector<std::string>& argv, const std::vector<std::string>& envp);

    // Handles svc #(0x5A0000 | index) before the generated host-call table; returns true if it
    // handled the index. Set before run().
    void set_host_call_handler(HostCallHandler handler) { host_call_handler_ = std::move(handler); }
    // Runs a guest function on `thread`, which is stopped outside Dynarmic (typically inside a
    // host call), through the same stop dispatch as the thread's own loop. Returns nullopt when
    // the call cannot be laid out or the guest cannot continue:
    // - a fatal fault or signal, or exit_group, with other guest threads alive ends the host
    //   process; on the only guest thread it fails the call with exiting() set, and run()
    //   later returns the status;
    // - a thread exit (exit, pthread_exit) inside the call always ends the host process.
    std::optional<GuestResult> call_guest(GuestThread& thread, std::uint32_t target, const GuestCall& args);

    GuestMemory& memory() { return mem_; }
    // Process-wide exit (exit_group, fatal signal). Threads other than the caller are not
    // stopped here; callers with other live threads end the host process instead.
    void request_exit(int status);
    bool exiting() const { return exiting_; }
    int exit_status() const { return exit_status_; }

    // Starts a guest thread for clone(CLONE_VM | CLONE_THREAD ...). Returns the new tid or -errno.
    std::int32_t clone_thread(GuestThread& parent, std::uint32_t flags, std::uint32_t stack,
                              std::uint32_t parent_tid_addr, std::uint32_t tls, std::uint32_t child_tid_addr);
    // Real guest threads; borrowers are not counted.
    std::size_t thread_count() const;
    // True when this guest thread is a borrower created by create_borrower, i.e. a host thread
    // that entered the guest on a leased carrier, and false for a real guest pthread.
    bool is_borrower(const GuestThread& thread) const;
    // Borrowers first, so tkill/tgkill aimed at a borrowed carrier's tid reach the borrower.
    // The pointer is only safe to dereference while the thread cannot exit; to signal a thread
    // use post_signal_to.
    GuestThread* find_thread(std::int32_t tid);
    // Posts info to the guest thread with this tid (borrowers first) atomically with the lookup.
    // It holds threads_mutex_, which unregister_thread() and destroy_borrower() take to unlist a
    // thread before freeing it, so the target cannot be freed between lookup and post. False if
    // no thread has this tid.
    bool post_signal_to(std::int32_t tid, const g::siginfo32& info);

    // A JIT for the calling host thread that runs as `carrier`, a guest thread parked inside a
    // host call: its TLS, guest tid, a stack below its sp, its signal mask, alternate signal
    // stack and FPSCR. The carrier must stay parked until destroy_borrower. Costs one processor
    // id and a 32 MiB JIT; translated code starts cold. nullptr if no processor id is free.
    // A borrower is never the process signal target.
    std::unique_ptr<GuestThread> create_borrower(GuestThread& carrier);
    // Copies the signal mask and alternate stack back to the still-parked carrier, moves signals
    // still pending on the borrower to it, and frees the borrower. Call on the borrowing thread,
    // after it stopped naming the borrower as its current guest thread.
    void destroy_borrower(std::unique_ptr<GuestThread> borrower, GuestThread& carrier);

    // Signals (signals.cpp).
    static void set_current_thread(GuestThread* thread);
    // Guest thread running on the calling host thread, or nullptr.
    static GuestThread* current_thread();
    // Records the guest program counter, and the library it belongs to, when the translator ends
    // the process outright. Not static: it needs this process to resolve the address.
    void install_terminate_reporter();
    // Guest thread that takes host signals arriving on threads without guest code. A published
    // target must be cleared before another one is set.
    static void set_process_signal_target(GuestThread* thread);
    // Unpublishes `thread` if it is the target and waits until no signal handler still uses it;
    // afterwards the thread may be destroyed. Must not be called from a signal handler.
    static void clear_process_signal_target(GuestThread* thread);
    static void install_host_signal_forwarding();
    // Delivers pending, unblocked signals of the thread; false if one terminated the process.
    bool dispatch_pending_signals(GuestThread& thread);
    // Turns a memory fault or exception into a guest signal; false if the guest cannot handle it.
    bool deliver_fault(GuestThread& thread, const Stop& stop);
    // Builds the signal frame and redirects the thread to the handler. True if the signal was
    // handled or ignored; false if its action terminates the process.
    bool deliver_signal(GuestThread& thread, const g::siginfo32& info, bool forced);
    // rt_sigreturn / sigreturn: restores the context saved by deliver_signal.
    bool sigreturn(GuestThread& thread, bool rt);

    // Serializes guest address-space changes (mmap/munmap/mprotect/brk/madvise).
    std::mutex& mm_mutex() { return mm_mutex_; }
    // Serializes the guest signal disposition table.
    std::mutex& signal_mutex() { return signal_mutex_; }

    // Maps absolute guest paths of the Android system (/system, /apex, /vendor, ...) into the
    // sysroot, and /proc/self/exe to the guest executable. Other paths are returned unchanged.
    std::string translate_path(const char* guest_path) const;

    // Redirects one directory of guest file operations to another. The launcher gives ART an
    // arm64 proxy library per guest library, so paths that reach the guest from Java (the
    // application's native library directory, most of all) name the proxy directory; the arm32
    // files it stands for live elsewhere. Without this, a guest that opens a file next to "its
    // own" library finds the proxy directory, which holds no arm32 anything: IL2CPP reports that
    // it cannot load. Aliases win over the sysroot mappings, and the longest prefix wins.
    void add_path_alias(std::string guest_prefix, std::string host_prefix);

    // Distinct paths of the files the guest currently has mapped, newest first. A library the
    // guest loaded by itself (a JNI shim whose JNI_OnLoad dlopens the real library, which Unity
    // plugins do) appears here and nowhere else.
    std::vector<std::string> mapped_file_paths() const;
    // Host path of the guest executable, as reported by /proc/self/exe.
    const std::string& exe_path() const { return exe_path_; }
    // Opens a synthesized /proc file (/proc/self/maps, /proc/self/stat, /proc/cpuinfo) that
    // must describe the 32-bit guest rather than the host. Returns false if guest_path is not
    // one of them; otherwise sets result to a file descriptor or -errno (proc_files.cpp).
    bool open_synthetic_file(const char* guest_path, int flags, std::int32_t& result);
    // True if addr lies in the image of the guest dynamic linker.
    bool in_guest_linker(std::uint32_t addr) const { return addr >= linker_start_ && addr < linker_end_; }

    // Drops translated code for the range in every thread.
    void invalidate(std::uint32_t addr, std::uint32_t len);
    // True the first time `key` is seen; used to log each unsupported case once.
    bool first_time(std::uint64_t key);

    // File-backed guest ranges, used to name addresses in crash reports. `offset` is the file
    // offset at `start` (or the ELF virtual address for images placed by our own loader).
    // Callers changing mappings hold mm_mutex().
    void record_file_mapping(std::uint32_t start, std::uint32_t length, std::uint64_t offset, std::string path,
                             bool offset_is_vaddr = false);
    void forget_mappings(std::uint32_t start, std::uint64_t length);
    // "libc.so offset 0x1234" style description, or "?" if the address is not file-backed.
    std::string describe_address(std::uint32_t addr) const;

    // Executable segments of libraries marked DT_ZB_TEXTREL (see elf_fixups.h). They stay
    // writable inside the emulator so text relocations can be applied. forget_mappings drops them.
    void add_textrel_range(std::uint32_t start, std::uint32_t length);
    bool overlaps_textrel_range(std::uint32_t start, std::uint64_t length) const;

    std::uint32_t brk_start = 0;
    std::uint32_t brk_current = 0;
    std::uint32_t mmap_limit = kMmapLimit;
    // Emulated: a 64-bit-only host kernel refuses PER_LINUX32, and bionic aborts if that fails.
    std::uint32_t personality = 0;
    std::array<g::ksigaction32, 65> sigactions{};

private:
    struct FileMapping {
        std::uint32_t start;
        std::uint32_t length;
        std::uint64_t offset;
        std::string path;
        bool offset_is_vaddr;
    };

    int allocate_processor_id();
    void register_thread(GuestThread* thread);
    // Handles one stop; true if the thread may resume.
    bool dispatch_stop(GuestThread& thread, const Stop& stop);
    // Delivers pending unblocked signals after a stop; false if one ended the guest.
    bool after_stop(GuestThread& thread);
    // A fault or exception becomes a guest signal, or a crash report and process exit.
    bool fault_or_crash(GuestThread& thread, const Stop& stop);
    // Runs a guest thread until it exits. Process-wide exits with other live threads end the
    // host process from here.
    void thread_loop(GuestThread& thread);
    void thread_main(std::unique_ptr<GuestThread> thread);
    // Thread exit bookkeeping: CLONE_CHILD_CLEARTID, then unregister_thread().
    void finish_thread(GuestThread& thread);
    // Retires the signal target publication, exclusive monitor slot, registry entry and
    // processor id of a thread that runs no more guest code.
    void unregister_thread(GuestThread& thread);
    void wait_for_threads();
    [[noreturn]] void exit_host_process();
    void crash_report(const Stop& stop, GuestThread& thread) const;
    std::string build_maps() const;
    std::string build_stat() const;

    GuestMemory mem_;
    std::unique_ptr<Dynarmic::ExclusiveMonitor> monitor_;
    std::unique_ptr<GuestThread> main_;

    mutable std::mutex threads_mutex_;
    std::condition_variable threads_cv_;
    std::vector<GuestThread*> threads_;
    std::vector<GuestThread*> borrowers_;
    std::bitset<kMaxThreads> processor_ids_;

    std::mutex mm_mutex_;
    std::mutex signal_mutex_;
    std::mutex seen_mutex_;
    std::set<std::uint64_t> seen_;
    std::vector<FileMapping> file_mappings_;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> textrel_ranges_;
    mutable std::mutex path_aliases_mutex_;
    std::vector<std::pair<std::string, std::string>> path_aliases_;
    static Process* reporter_process;  // for the terminate handler, which has no other way in
    std::string sysroot_;
    std::string exe_path_;
    bool precise_faults_ = false;
    std::uint32_t initial_sp_ = 0;
    std::uint32_t linker_start_ = 0;
    std::uint32_t linker_end_ = 0;
    std::atomic<bool> exiting_{false};
    std::atomic<int> exit_status_{0};
    HostCallHandler host_call_handler_;
};

}  // namespace zb
