#pragma once

#include <cstdint>
#include <map>
#include <vector>
#include <utility>

#include "zb/android_looper_backend.h"

// In-memory AndroidLooperBackend for host tests. prepare() hands out a fake real looper per
// calling "host thread" (tests drive one at a time), records the registered descriptors, and
// deliver() plays the part of Looper.loop(): it calls the host callback and applies its result,
// dropping the fd when the callback returns 0, exactly as ALooper does.
class MockAndroidLooper final : public zb::AndroidLooperBackend {
public:
    struct Entry {
        int ident = 0;
        int events = 0;
        Callback callback = nullptr;
        void* data = nullptr;
    };

    std::uint64_t prepare(int options) override {
        ++prepares_;
        last_options_ = options;
        if (current_ == 0) current_ = ++next_looper_;
        return current_;
    }

    std::uint64_t for_thread() override { return current_; }
    void acquire(std::uint64_t looper) override { if (looper == current_) ++acquires_; }
    void release(std::uint64_t looper) override { if (looper == current_) ++releases_; }

    int add_fd(std::uint64_t looper, int fd, int ident, int events, Callback callback,
               void* data) override {
        if (looper == 0 || looper != current_ || fd < 0 || callback == nullptr) return -1;
        if (fail_add_fd) return -1;
        fds_[std::make_pair(looper, fd)] = Entry{ident, events, callback, data};
        return 1;
    }

    // Stands in for the real ALooper_pollOnce: dispatches the fds the test marked ready, then
    // answers with what the NDK would. Only a thread that acquired a real looper polls this way.
    int poll_once(int timeout_millis, int* out_fd, int* out_events, void** out_data) override {
        ++polls_;
        last_timeout_ = timeout_millis;
        if (out_fd != nullptr) *out_fd = ready_fd;
        if (out_events != nullptr) *out_events = ready_events;
        if (out_data != nullptr) *out_data = ready_data;
        if (ready_.empty()) return poll_result;
        int result = -3;  // ALOOPER_POLL_TIMEOUT until something is dispatched
        for (int fd : ready_) {
            const auto entry = fds_.find(std::make_pair(current_, fd));
            if (entry == fds_.end()) continue;
            entry->second.callback(fd, 1 /* ALOOPER_EVENT_INPUT */, entry->second.data);
            result = -2;  // ALOOPER_POLL_CALLBACK
        }
        ready_.clear();
        return result;
    }

    void make_ready(int fd) { ready_.push_back(fd); }
    int polls() const { return polls_; }
    int last_timeout() const { return last_timeout_; }
    int poll_result = -3;
    // What a registration hands back from a poll, as ALooper_pollOnce fills its out parameters.
    int ready_fd = -1;
    int ready_events = 0;
    void* ready_data = nullptr;

    int remove_fd(std::uint64_t looper, int fd) override {
        if (looper != current_) return -1;
        return fds_.erase(std::make_pair(looper, fd)) == 0 ? 0 : 1;
    }

    void wake(std::uint64_t looper) override { if (looper == current_) ++wakes_; }

    // One callback delivery on the host thread, as the real looper would do it. Returns the
    // callback's value, or -1 when the fd is not registered.
    int deliver(std::uint64_t looper, int fd, int events) {
        const auto it = fds_.find(std::make_pair(looper, fd));
        if (it == fds_.end()) return -1;
        const Entry entry = it->second;
        const int keep = entry.callback(fd, events, entry.data);
        if (keep == 0) fds_.erase(std::make_pair(looper, fd));
        return keep;
    }

    bool registered(std::uint64_t looper, int fd) const {
        return fds_.count(std::make_pair(looper, fd)) != 0;
    }
    std::size_t registrations() const { return fds_.size(); }
    std::uint64_t current() const { return current_; }
    int prepares() const { return prepares_; }
    int last_options() const { return last_options_; }
    int wakes() const { return wakes_; }
    int acquires() const { return acquires_; }
    int releases() const { return releases_; }

    bool fail_add_fd = false;

private:
    std::map<std::pair<std::uint64_t, int>, Entry> fds_;
    std::uint64_t next_looper_ = 0x5100;
    std::uint64_t current_ = 0;
    int prepares_ = 0;
    int last_options_ = -1;
    int wakes_ = 0;
    int acquires_ = 0;
    int releases_ = 0;
    int polls_ = 0;
    int last_timeout_ = -99;
    std::vector<int> ready_;
};
