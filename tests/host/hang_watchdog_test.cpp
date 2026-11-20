// HangWatchdog: the per-thread activity table and the sampler's stuck-thread decision, driven
// with a fake snapshot source and an explicit clock so it needs no real guest thread or timing.
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "check.h"
#include "zb/hang_watchdog.h"
#include "zb/runtime_report.h"

namespace {

bool contains(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

}  // namespace

int main() {
    const auto calling_tid = zb::current_watch_tid();
    std::thread other([&] {
        const auto tid = zb::current_watch_tid();
        CHECK(tid > 0 && tid != calling_tid && tid == zb::current_watch_tid());
    });
    other.join();
    // record_thread_activity / snapshot_thread_activity: a fresh tid claims a slot, repeated
    // calls bump the counter and update the recorded activity in place.
    zb::record_thread_activity(9001, zb::ThreadActivityKind::kSyscall, 3);
    for (const auto& sample : zb::snapshot_thread_activity()) {
        if (sample.tid == 9001) CHECK(sample.in_progress);
    }
    zb::record_thread_activity(9001, zb::ThreadActivityKind::kSyscall, 3);
    zb::record_thread_activity(9001, zb::ThreadActivityKind::kHostCall, 0x2a);
    {
        const auto snapshot = zb::snapshot_thread_activity();
        bool found = false;
        for (const auto& sample : snapshot) {
            if (sample.tid != 9001) continue;
            found = true;
            CHECK(sample.kind == zb::ThreadActivityKind::kHostCall);
            CHECK(sample.id == 0x2a);
            CHECK(sample.counter == 3);
            // Recorded calls start as in progress; the marker says so until they finish.
            CHECK(zb::describe_thread_activity(sample) == "host:0x2a(inside)");
            zb::record_thread_activity_done(9001);
            auto after = zb::snapshot_thread_activity();
            for (const auto& done : after) {
                if (done.tid == 9001) CHECK(zb::describe_thread_activity(done) == "host:0x2a");
            }
        }
        CHECK(found);
    }

    // HangWatchdog::sample: a thread whose activity never changes across kStuckThreshold + 1
    // samples is reported as stuck; one that keeps moving never is.
    zb::runtime_report().clear();
    using Clock = zb::HangWatchdog::Clock;
    Clock::time_point now{};
    bool stuck_activity = true;  // toggled by the fake snapshot to simulate the moving thread

    zb::HangWatchdog watchdog([&]() {
        std::vector<zb::ThreadActivitySample> out;
        zb::ThreadActivitySample stuck;
        stuck.tid = 111;
        stuck.kind = zb::ThreadActivityKind::kSyscall;
        stuck.id = 7;
        stuck.counter = 42;  // never changes
        out.push_back(stuck);

        zb::ThreadActivitySample moving;
        moving.tid = 222;
        moving.kind = zb::ThreadActivityKind::kHostCall;
        moving.id = 5;
        static std::uint64_t moving_counter = 0;
        if (stuck_activity) ++moving_counter;
        moving.counter = moving_counter;
        out.push_back(moving);
        return out;
    });

    // Sample 1: establishes the baseline for both threads; nothing is stuck yet.
    CHECK(!watchdog.sample(now));
    now += std::chrono::seconds(3);
    // Sample 2: thread 111 unchanged once, thread 222 still moving; below the threshold.
    CHECK(!watchdog.sample(now));
    now += std::chrono::seconds(3);
    // Sample 3: thread 111 unchanged twice in a row -> stuck. Thread 222 keeps moving.
    CHECK(watchdog.sample(now));

    const std::string text = zb::runtime_report().text();
    CHECK(contains(text, "watch-1: 111=sys:"));
    CHECK(contains(text, "x42"));
    CHECK(contains(text, "stuck=6"));
    CHECK(!contains(text, "222=host:"));

    // Once a thread starts moving again it drops out of the stuck list; if nothing new is stuck,
    // no further note is written.
    zb::runtime_report().clear();
    zb::HangWatchdog watchdog2([&]() {
        std::vector<zb::ThreadActivitySample> out;
        zb::ThreadActivitySample sample;
        sample.tid = 333;
        sample.kind = zb::ThreadActivityKind::kSyscall;
        sample.id = 1;
        static std::uint64_t counter = 0;
        sample.counter = ++counter;  // always moving
        out.push_back(sample);
        return out;
    });
    Clock::time_point now2{};
    CHECK(!watchdog2.sample(now2));
    now2 += std::chrono::seconds(3);
    CHECK(!watchdog2.sample(now2));
    now2 += std::chrono::seconds(3);
    CHECK(!watchdog2.sample(now2));
    CHECK(zb::runtime_report().text().find("watch-1") == std::string::npos);

    for (int i = 1; i <= 6; ++i) {
        zb::runtime_report().note_watch_detail("threads-" + std::to_string(i), "thread census");
    }
    zb::runtime_report().note_watch_detail("threads-platform", "platform thread census");
    zb::HangWatchdog crowded([] {
        std::vector<zb::ThreadActivitySample> out;
        for (int tid = 100; tid < 110; ++tid) {
            out.push_back({tid, zb::ThreadActivityKind::kSyscall, 3, 1, false});
        }
        out.push_back({999, zb::ThreadActivityKind::kHostCall, 0x2a, 1, true, 0x1234, 0x5678});
        return out;
    });
    zb::record_render_thread(109);  // Outside the general report's five slots.
    for (std::size_t i = 0; i < zb::HangWatchdog::kMaxNotes + 3; ++i) {
        crowded.sample(now);
        now += zb::HangWatchdog::kInterval;
    }
    const auto latest = zb::runtime_report().text();
    CHECK(contains(latest, "watch-latest: 999=host:0x2a(inside)"));
    CHECK(contains(latest, "pc=0x00001234 lr=0x00005678"));
    CHECK(contains(latest, "watch-render: host-tid=109"));

    std::puts("hang_watchdog_test: ok");
    return 0;
}
