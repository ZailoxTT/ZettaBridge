// Hang watchdog: per-thread last-activity slots (lock-free, allocation-free) plus a background
// sampler that turns "same activity, same counter, two samples running" into a report note. No
// guest semantics change here; a stuck thread is only ever described, never touched.
#include "zb/hang_watchdog.h"

#include <dirent.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#include "zb/host_jni.h"
#include "zb/runtime_report.h"
#include "zb/syscalls.h"

namespace zb {

namespace {

// Fixed-size, never-grows table of per-thread activity slots. A guest thread claims a slot the
// first time it is seen and keeps it (tids are not reused across a run in a way that matters for
// diagnostics); a slot is identified by `tid` with 0 meaning free.
constexpr std::size_t kMaxSlots = 256;

struct Slot {
    std::atomic<bool> in_progress{false};
    std::atomic<std::int32_t> tid{0};
    std::atomic<std::uint8_t> kind{static_cast<std::uint8_t>(ThreadActivityKind::kNone)};
    std::atomic<std::uint32_t> id{0};
    std::atomic<std::uint64_t> counter{0};
    std::atomic<std::uint32_t> pc{0};
    std::atomic<std::uint32_t> lr{0};
};

Slot g_slots[kMaxSlots];
std::atomic<std::int32_t> g_render_thread{0};

}  // namespace

void record_render_thread(std::int32_t guest_tid) {
    g_render_thread.store(guest_tid, std::memory_order_relaxed);
}

void record_thread_activity(std::int32_t tid, ThreadActivityKind kind, std::uint32_t id,
                            std::uint32_t pc, std::uint32_t lr) {
    if (tid == 0) return;
    // This runs on every guest syscall and host call, so the common path must not scan the table:
    // each thread remembers its own slot after claiming it once.
    static thread_local Slot* mine = nullptr;
    static thread_local std::int32_t mine_tid = 0;
    const auto record = [&](Slot& slot) {
        slot.kind.store(static_cast<std::uint8_t>(kind), std::memory_order_relaxed);
        slot.id.store(id, std::memory_order_relaxed);
        slot.pc.store(pc, std::memory_order_relaxed);
        slot.lr.store(lr, std::memory_order_relaxed);
        slot.in_progress.store(true, std::memory_order_relaxed);
        slot.counter.fetch_add(1, std::memory_order_release);
    };
    if (mine != nullptr && mine_tid == tid) {
        record(*mine);
        return;
    }
    // Find an existing slot for this tid, or claim the first free one. A relaxed CAS race
    // between two threads claiming the same free slot for different tids is possible but
    // vanishingly rare and self-heals: the loser just tries the next slot next call.
    Slot* free_slot = nullptr;
    for (Slot& slot : g_slots) {
        const std::int32_t current = slot.tid.load(std::memory_order_relaxed);
        if (current == tid) {
            record(slot);
            mine = &slot;
            mine_tid = tid;
            return;
        }
        if (current == 0 && free_slot == nullptr) free_slot = &slot;
    }
    if (free_slot == nullptr) return;  // table full; diagnostics only, never blocks the guest
    std::int32_t expected = 0;
    if (!free_slot->tid.compare_exchange_strong(expected, tid, std::memory_order_relaxed)) {
        // Lost the race; the winner's slot is not this tid's, try once more on the next call.
        return;
    }
    record(*free_slot);
    mine = free_slot;
    mine_tid = tid;
}

void record_thread_activity_done(std::int32_t tid) {
    for (Slot& slot : g_slots) {
        if (slot.tid.load(std::memory_order_relaxed) == tid) {
            slot.in_progress.store(false, std::memory_order_relaxed);
            return;
        }
    }
}

std::vector<ThreadActivitySample> snapshot_thread_activity() {
    std::vector<ThreadActivitySample> out;
    for (Slot& slot : g_slots) {
        const std::int32_t tid = slot.tid.load(std::memory_order_relaxed);
        if (tid == 0) continue;
        ThreadActivitySample sample;
        sample.tid = tid;
        sample.counter = slot.counter.load(std::memory_order_acquire);
        sample.kind = static_cast<ThreadActivityKind>(slot.kind.load(std::memory_order_relaxed));
        sample.id = slot.id.load(std::memory_order_relaxed);
        sample.in_progress = slot.in_progress.load(std::memory_order_relaxed);
        sample.pc = slot.pc.load(std::memory_order_relaxed);
        sample.lr = slot.lr.load(std::memory_order_relaxed);
        out.push_back(sample);
    }
    return out;
}

// Ticks of CPU this thread has burned, from /proc/self/task/<tid>/stat, or 0 when unreadable.
// A thread whose activity never changes but whose CPU keeps climbing is spinning in translated
// code; one whose CPU stands still is blocked.
// What the kernel says about a thread: "gone" when it no longer exists, otherwise its scheduler
// state (R running, S sleeping, D uninterruptible), the CPU ticks it burned, and the kernel
// function it is waiting in. Zero CPU alone cannot tell an idle thread from a dead one.
std::string thread_kernel_state(std::int32_t tid) {
    char path[64];
    std::snprintf(path, sizeof path, "/proc/self/task/%d/stat", tid);
    std::FILE* file = std::fopen(path, "re");
    if (file == nullptr) return "gone";
    char line[1024];
    const char* read = std::fgets(line, sizeof line, file);
    std::fclose(file);
    if (read == nullptr) return "gone";
    const char* cursor = std::strrchr(line, ')');
    if (cursor == nullptr) return "?";
    char state = '?';
    unsigned long long utime = 0, stime = 0;
    // Fields after "comm": state, ppid, pgrp, session, tty, tpgid, flags, min_flt, cmin_flt,
    // maj_flt, cmaj_flt, utime, stime.
    if (std::sscanf(cursor + 2, "%c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu", &state, &utime,
                    &stime) != 3) {
        return "?";
    }
    char waiting[64] = {};
    std::snprintf(path, sizeof path, "/proc/self/task/%d/wchan", tid);
    std::FILE* wchan = std::fopen(path, "re");
    if (wchan != nullptr) {
        if (std::fgets(waiting, sizeof waiting, wchan) == nullptr) waiting[0] = '\0';
        std::fclose(wchan);
    }
    char text[128];
    std::snprintf(text, sizeof text, "%c cpu=%llu%s%s", state, utime + stime,
                  waiting[0] != '\0' ? " in=" : "", waiting);
    return text;
}

// Every thread of this process with its name, scheduler state and wait channel, whether or not it
// ever ran guest code. Engine threads carry telling names (Flutter uses "1.ui", "1.raster",
// "1.io"), so this says which part of the guest is missing or waiting, and shows ART's own threads
// next to ours.
// Threads of the runtime itself (binder, ART daemons) crowd out the engine's own, and one report
// line holds only so much, so they are counted rather than listed and the rest goes out in chunks.
bool is_platform_thread(const char* name) {
    static const char* const kNoise[] = {"binder:", "Jit thread pool", "Profile Saver", "ADB-JDWP",
                                         "Signal Catcher", "perfetto", "HeapTaskDaemon",
                                         "ReferenceQueueD", "FinalizerDaemon", "FinalizerWatchd",
                                         "hwuiTask", "RenderThread", "queued-work-loo", "Timer-",
                                         "GPU completion", "SurfaceSyncGrou"};
    for (const char* prefix : kNoise) {
        if (std::strncmp(name, prefix, std::strlen(prefix)) == 0) return true;
    }
    return false;
}

void write_threads_census() {
    DIR* dir = ::opendir("/proc/self/task");
    if (dir == nullptr) {
        runtime_report().note_watch_detail("threads", "(unavailable)");
        return;
    }
    std::string chunk;
    unsigned chunk_index = 0;
    unsigned platform = 0;
    unsigned listed = 0;
    while (const dirent* entry = ::readdir(dir)) {
        if (entry->d_name[0] == '.') continue;
        const std::int32_t tid = std::atoi(entry->d_name);
        if (tid == 0) continue;
        char path[64];
        std::snprintf(path, sizeof path, "/proc/self/task/%d/comm", tid);
        char name[64] = {};
        if (std::FILE* comm = std::fopen(path, "re")) {
            if (std::fgets(name, sizeof name, comm) != nullptr) {
                char* newline = std::strchr(name, '\n');
                if (newline != nullptr) *newline = '\0';
            }
            std::fclose(comm);
        }
        if (is_platform_thread(name)) {
            ++platform;
            continue;
        }
        if (listed >= 48) break;
        ++listed;
        chunk += (chunk.empty() ? "" : " | ") + std::to_string(tid) + ":" + name + " " +
                 thread_kernel_state(tid);
        if (chunk.size() > 420) {
            runtime_report().note_watch_detail("threads-" + std::to_string(++chunk_index), chunk);
            chunk.clear();
        }
    }
    ::closedir(dir);
    if (!chunk.empty()) runtime_report().note_watch_detail("threads-" + std::to_string(++chunk_index), chunk);
    runtime_report().note_watch_detail("threads-platform", std::to_string(platform) + " runtime threads hidden");
}

std::string describe_thread_activity(const ThreadActivitySample& sample) {
    char text[64];
    switch (sample.kind) {
    case ThreadActivityKind::kSyscall:
        std::snprintf(text, sizeof text, "sys:%s%s", syscall_name(sample.id),
                      sample.in_progress ? "(inside)" : "");
        return text;
    case ThreadActivityKind::kHostCall:
        std::snprintf(text, sizeof text, "host:0x%x%s", sample.id, sample.in_progress ? "(inside)" : "");
        return text;
    case ThreadActivityKind::kNone:
    default:
        return "(none)";
    }
}

HangWatchdog::HangWatchdog(SnapshotFn snapshot) : snapshot_(std::move(snapshot)) {}

bool HangWatchdog::sample(Clock::time_point now) {
    const std::vector<ThreadActivitySample> current = snapshot_();

    std::vector<State> next;
    next.reserve(current.size());
    for (const ThreadActivitySample& sample : current) {
        State* prior = nullptr;
        for (State& state : previous_) {
            if (state.tid == sample.tid) {
                prior = &state;
                break;
            }
        }
        State state;
        state.tid = sample.tid;
        state.kind = sample.kind;
        state.id = sample.id;
        state.counter = sample.counter;
        state.in_progress = sample.in_progress;
        state.pc = sample.pc;
        state.lr = sample.lr;
        if (prior != nullptr && prior->kind == sample.kind && prior->id == sample.id &&
            prior->counter == sample.counter && prior->in_progress == sample.in_progress) {
            state.since = prior->since;
            state.unchanged_samples = prior->unchanged_samples + 1;
        } else {
            state.since = now;
            state.unchanged_samples = 0;
        }
        next.push_back(state);
    }
    previous_ = std::move(next);

    const auto render_tid = g_render_thread.load(std::memory_order_relaxed);
    for (const State& state : previous_) {
        if (state.tid != render_tid) continue;
        ThreadActivitySample activity{state.tid, state.kind, state.id, state.counter, state.in_progress};
        const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(now - state.since).count();
        char detail[192];
        std::snprintf(detail, sizeof detail, "guest-tid=%d %s x%llu unchanged=%llds pc=0x%08x lr=0x%08x",
                      state.tid, describe_thread_activity(activity).c_str(),
                      static_cast<unsigned long long>(state.counter), static_cast<long long>(seconds),
                      state.pc, state.lr);
        runtime_report().note_watch_detail("render", detail);
        break;
    }

    std::vector<const State*> stuck;
    for (const State& state : previous_) {
        if (state.unchanged_samples >= kStuckThreshold) stuck.push_back(&state);
    }
    if (stuck.empty()) return false;
    std::stable_sort(stuck.begin(), stuck.end(), [](const State* a, const State* b) {
        if (a->in_progress != b->in_progress) return a->in_progress;
        return a->since > b->since;
    });

    std::string value;
    std::size_t shown = 0;
    for (const State* state : stuck) {
        if (shown >= kMaxThreadsShown) break;
        if (shown > 0) value += " | ";
        ThreadActivitySample sample;
        sample.tid = state->tid;
        sample.kind = state->kind;
        sample.id = state->id;
        sample.counter = state->counter;
        sample.in_progress = state->in_progress;
        const auto seconds =
            std::chrono::duration_cast<std::chrono::seconds>(now - state->since).count();
        value += std::to_string(state->tid) + "=" + describe_thread_activity(sample) +
                 " x" + std::to_string(state->counter) + " stuck=" + std::to_string(seconds) +
                 " " + thread_kernel_state(state->tid);
        if (state->pc != 0) {
            char location[40];
            std::snprintf(location, sizeof location, " pc=0x%08x lr=0x%08x", state->pc, state->lr);
            value += location;
        }
        ++shown;
    }
    value += " | recent-jni: " + jni_recent_calls();
    // Once, with the first report: the whole thread census is long and does not change much.
    static bool census_written = false;
    if (!census_written) {
        census_written = true;
        write_threads_census();
    }

    const std::string key = notes_written_ < kMaxNotes ? std::to_string(++notes_written_) : "latest";
    runtime_report().note_watch_detail(key, value);
    return true;
}

void HangWatchdog::run_forever() {
    for (;;) {
        std::this_thread::sleep_for(kInterval);
        sample(Clock::now());
    }
}

void start_hang_watchdog() {
    static std::atomic<bool> started{false};
    bool expected = false;
    if (!started.compare_exchange_strong(expected, true, std::memory_order_relaxed)) return;
    std::thread([]() {
        HangWatchdog watchdog;
        watchdog.run_forever();
    }).detach();
}

}  // namespace zb
