#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace zb {

// Hang diagnostics: a guest that stops making progress (gray screen, no crash) leaves nothing
// in the crash report, because nothing crashed. This records, per guest thread, the last thing
// it did on its way out of translated code, and a background thread periodically checks whether
// that has changed.

// The two places a guest thread leaves translated code.
enum class ThreadActivityKind : std::uint8_t {
    kNone = 0,
    kSyscall = 1,
    kHostCall = 2,
};

// Records that guest thread `tid` just performed a syscall (`id` = syscall number) or a host
// call (`id` = host-call index). Lock-free, allocation-free, safe from any thread: called on
// every syscall and every host call, so it must stay cheap.
void record_thread_activity(std::int32_t tid, ThreadActivityKind kind, std::uint32_t id,
                            std::uint32_t pc = 0, std::uint32_t lr = 0);

// The host call recorded last for this thread has returned. A thread whose last recorded event is
// still in progress is blocked inside our own code, which reads very differently from a guest
// that simply stopped calling out.
void record_thread_activity_done(std::int32_t tid);

struct ThreadActivitySample {
    std::int32_t tid = 0;
    ThreadActivityKind kind = ThreadActivityKind::kNone;
    std::uint32_t id = 0;
    std::uint64_t counter = 0;
    bool in_progress = false;  // still inside the recorded host call or syscall
    std::uint32_t pc = 0;
    std::uint32_t lr = 0;
};

// A snapshot of every guest thread slot touched so far.
std::vector<ThreadActivitySample> snapshot_thread_activity();

// "sys:openat" or "host:0x2a3"; "(none)" for a slot that was never recorded.
std::string describe_thread_activity(const ThreadActivitySample& sample);

// Periodically samples snapshot_thread_activity() and records a report note when one or more
// threads have made no progress since the previous two samples. Never aborts or kills the
// process; this is diagnostics only.
class HangWatchdog {
public:
    using Clock = std::chrono::steady_clock;
    using SnapshotFn = std::function<std::vector<ThreadActivitySample>()>;

    static constexpr std::chrono::seconds kInterval{3};
    static constexpr int kStuckThreshold = 2;      // consecutive unchanged samples
    static constexpr std::size_t kMaxNotes = 6;    // fixed history before updating the latest line
    static constexpr std::size_t kMaxThreadsShown = 5;

    explicit HangWatchdog(SnapshotFn snapshot = snapshot_thread_activity);

    // One sampling pass at `now`. Returns true if it wrote a new watch-<n> report note.
    bool sample(Clock::time_point now);

    // Runs sample() every kInterval, forever, on the calling thread.
    [[noreturn]] void run_forever();

private:
    struct State {
        std::int32_t tid = 0;
        ThreadActivityKind kind = ThreadActivityKind::kNone;
        std::uint32_t id = 0;
        std::uint64_t counter = 0;
        Clock::time_point since{};  // when this (kind, id, counter) was first observed
        int unchanged_samples = 0;
        bool in_progress = false;
        std::uint32_t pc = 0;
        std::uint32_t lr = 0;
    };

    SnapshotFn snapshot_;
    std::vector<State> previous_;
    std::size_t notes_written_ = 0;
};

// Starts the watchdog on a detached background thread, once per process. GuestJniEngine calls
// this when the runtime starts; the runtime is process-lifetime, so there is no shutdown path.
void start_hang_watchdog();

}  // namespace zb
