#include "zb/host_looper.h"

#include <chrono>
#include <sys/eventfd.h>
#include <poll.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

#include "zb/android_looper_backend.h"
#include "zb/guest_memory.h"
#include "zb/guest_thread.h"
#include "zb/library_runtime.h"
#include "zb/log.h"
#include "zb/platform_compat_hostcalls.h"
#include "zb/runtime_report.h"

#include <atomic>
#include <algorithm>
#include <cstdio>
#include <map>
#include <string>
#include <utility>

namespace zb {

namespace {

constexpr int kAllowNonCallbacks = 1;
constexpr int kPollWake = -1;
constexpr int kPollCallback = -2;
constexpr int kPollTimeout = -3;
constexpr int kPollError = -4;
constexpr int kEventInput = 1;
constexpr int kEventOutput = 2;
constexpr int kEventError = 4;
constexpr int kEventHangup = 8;
constexpr int kEventInvalid = 16;

int looper_events(short events) {
    int result = 0;
    if ((events & POLLIN) != 0) result |= kEventInput;
    if ((events & POLLOUT) != 0) result |= kEventOutput;
    if ((events & POLLERR) != 0) result |= kEventError;
    if ((events & POLLHUP) != 0) result |= kEventHangup;
    if ((events & POLLNVAL) != 0) result |= kEventInvalid;
    return result;
}

short poll_events(int events) {
    short result = 0;
    if ((events & kEventInput) != 0) result |= POLLIN;
    if ((events & kEventOutput) != 0) result |= POLLOUT;
    return result;
}

const char* poll_result_name(int result) {
    switch (result) {
    case kPollWake: return "wake";
    case kPollCallback: return "callback";
    case kPollTimeout: return "timeout";
    case kPollError: return "error";
    default: return result >= 0 ? "ident" : "unknown";
    }
}

void signal_eventfd(int fd) {
    const std::uint64_t one = 1;
    ssize_t written;
    do {
        written = write(fd, &one, sizeof one);
    } while (written < 0 && errno == EINTR);
}

void drain_eventfd(int fd) {
    std::uint64_t value;
    ssize_t count;
    do {
        count = read(fd, &value, sizeof value);
    } while (count < 0 && errno == EINTR);
}

}  // namespace

struct HostLooper::Impl {
    struct Registration {
        int fd = -1;
        int ident = 0;
        int events = 0;
        std::uint32_t callback = 0;
        std::uint32_t data = 0;
        std::uint64_t serial = 0;
    };

    struct Looper {
        std::uint32_t handle = 0;
        int options = 0;
        std::uint32_t references = 1;
        int wake_fd = -1;  // guest-path wake eventfd; -1 on an attached looper
        // Attached: this looper lives on a host thread whose own Android looper does the polling.
        bool attached = false;
        // A guest-path looper that also acquired a real Android looper on its own host thread,
        // because an input queue was attached to it: a host AInputQueue has no descriptor we
        // could poll ourselves. Its registrations live on the real looper and its pollOnce is
        // the real one, but unlike a borrower this thread does the polling itself.
        bool real_backed = false;
        std::uint64_t real = 0;  // backend handle while attached or real-backed
        std::unordered_map<int, Registration> registrations;
    };

    // What the real Android looper hands back to us as the callback's `data`. Bindings are never
    // freed: the looper may still hold the pointer after we stopped using it, and one process
    // registers only a handful of descriptors.
    struct Binding {
        Impl* impl = nullptr;
        std::uint32_t handle = 0;
        int fd = -1;
        std::uint32_t callback = 0;
        std::uint32_t data = 0;
        std::uint64_t serial = 0;
        std::atomic<bool> active{true};
    };

    Impl(LibraryRuntime& runtime_, AndroidLooperBackend* backend_, GuestInvoker invoker_,
         BorrowerProbe borrower_probe_)
        : runtime(runtime_), backend(backend_), invoker(std::move(invoker_)),
          borrower_probe(std::move(borrower_probe_)) {}

    ~Impl() {
        for (const auto& [handle, looper] : loopers) {
            (void)handle;
            if (looper->wake_fd >= 0) close(looper->wake_fd);
        }
    }

    // Diagnostics only (Flutter callback-looper hunt): counters and per-thread "last" state
    // pushed into the runtime report. None of this changes looper semantics.
    std::atomic<std::uint64_t> report_loopers{0};
    std::atomic<std::uint64_t> report_fds_added{0};
    std::atomic<std::uint64_t> report_fds_removed{0};
    std::atomic<std::uint64_t> report_polls{0};
    std::atomic<std::uint64_t> report_polls_wake{0};
    std::atomic<std::uint64_t> report_polls_timeout{0};
    std::atomic<std::uint64_t> report_polls_callback{0};
    std::atomic<std::uint64_t> report_polls_error{0};
    std::atomic<std::uint64_t> report_polls_ident{0};
    std::atomic<std::uint64_t> report_callbacks{0};
    std::atomic<std::uint64_t> report_callbacks_unregistered{0};
    std::atomic<std::uint64_t> report_wakes{0};
    std::atomic<std::uint64_t> report_attached{0};
    std::atomic<std::uint64_t> report_host_callbacks{0};
    std::atomic<std::uint64_t> report_host_callbacks_guest{0};
    std::atomic<std::uint64_t> report_host_callbacks_failed{0};

    static constexpr std::size_t kMaxReportSlots = 4;
    std::mutex report_mutex;
    std::vector<std::int32_t> report_slot_tids;

    void report_loopers_line() {
        runtime_report().note_looper_detail(
            "loopers", std::to_string(report_loopers.load(std::memory_order_relaxed)), true);
    }

    void report_fds_line() {
        char buf[96];
        std::snprintf(buf, sizeof buf, "added=%llu removed=%llu",
                      static_cast<unsigned long long>(report_fds_added.load(std::memory_order_relaxed)),
                      static_cast<unsigned long long>(report_fds_removed.load(std::memory_order_relaxed)));
        runtime_report().note_looper_detail("fds", buf, true);
    }

    void report_polls_line() {
        char buf[192];
        std::snprintf(buf, sizeof buf,
                      "total=%llu wake=%llu timeout=%llu callback=%llu error=%llu ident=%llu",
                      static_cast<unsigned long long>(report_polls.load(std::memory_order_relaxed)),
                      static_cast<unsigned long long>(report_polls_wake.load(std::memory_order_relaxed)),
                      static_cast<unsigned long long>(report_polls_timeout.load(std::memory_order_relaxed)),
                      static_cast<unsigned long long>(report_polls_callback.load(std::memory_order_relaxed)),
                      static_cast<unsigned long long>(report_polls_error.load(std::memory_order_relaxed)),
                      static_cast<unsigned long long>(report_polls_ident.load(std::memory_order_relaxed)));
        runtime_report().note_looper_detail("polls", buf, true);
    }

    void report_callbacks_line() {
        char buf[128];
        std::snprintf(buf, sizeof buf, "dispatched=%llu unregistered=%llu",
                      static_cast<unsigned long long>(report_callbacks.load(std::memory_order_relaxed)),
                      static_cast<unsigned long long>(report_callbacks_unregistered.load(std::memory_order_relaxed)));
        runtime_report().note_looper_detail("callbacks", buf, true);
    }

    void report_wakes_line() {
        runtime_report().note_looper_detail(
            "wakes", std::to_string(report_wakes.load(std::memory_order_relaxed)), true);
    }

    // Loopers that live on a host thread and were handed to the real Android looper.
    void report_attached_line() {
        runtime_report().note_looper_detail(
            "attached", std::to_string(report_attached.load(std::memory_order_relaxed)), true);
    }

    // Callbacks the real Android looper delivered on a host thread, and how many of them got
    // into the guest. fired > guest means the guest could not be entered.
    void report_host_callbacks_line() {
        char buf[160];
        std::snprintf(buf, sizeof buf, "fired=%llu guest=%llu failed=%llu",
                      static_cast<unsigned long long>(report_host_callbacks.load(std::memory_order_relaxed)),
                      static_cast<unsigned long long>(report_host_callbacks_guest.load(std::memory_order_relaxed)),
                      static_cast<unsigned long long>(report_host_callbacks_failed.load(std::memory_order_relaxed)));
        runtime_report().note_looper_detail("host-callbacks", buf, true);
    }

    void report_last(const GuestThread& thread, std::size_t fd_count, int result) {
        std::size_t index;
        {
            std::lock_guard<std::mutex> lock(report_mutex);
            const auto it = std::find(report_slot_tids.begin(), report_slot_tids.end(), thread.tid);
            if (it != report_slot_tids.end()) {
                index = static_cast<std::size_t>(it - report_slot_tids.begin());
            } else if (report_slot_tids.size() < kMaxReportSlots) {
                index = report_slot_tids.size();
                report_slot_tids.push_back(thread.tid);
            } else {
                return;
            }
        }
        char buf[96];
        std::snprintf(buf, sizeof buf, "tid=%d fds=%zu result=%s", static_cast<int>(thread.tid),
                      fd_count, poll_result_name(result));
        runtime_report().note_looper_detail(
            "last-" + std::to_string(index), buf, true);
    }

    std::uint32_t argument(GuestThread& thread, unsigned position, bool& valid) const {
        if (position < 4) return thread.regs()[position];
        const std::uint64_t address = static_cast<std::uint64_t>(thread.regs()[13]) +
                                      4u * static_cast<std::uint64_t>(position - 4);
        if (address > UINT32_MAX) {
            valid = false;
            return 0;
        }
        const std::uint8_t* source = runtime.memory().host_ptr(
            static_cast<std::uint32_t>(address), sizeof(std::uint32_t), kPageRead);
        if (source == nullptr) {
            valid = false;
            return 0;
        }
        std::uint32_t value;
        std::memcpy(&value, source, sizeof value);
        return value;
    }

    bool write_guest(std::uint32_t address, std::uint32_t value) const {
        if (address == 0) return true;
        std::uint8_t* target = runtime.memory().host_ptr(address, sizeof value, kPageWrite);
        if (target == nullptr) return false;
        std::memcpy(target, &value, sizeof value);
        return true;
    }

    bool is_borrower(const GuestThread& thread) {
        return borrower_probe ? borrower_probe(thread) : runtime.is_borrower(thread);
    }

    std::optional<GuestResult> invoke(std::uint32_t function, const GuestCall& args) {
        if (invoker) return invoker(function, args);
        return runtime.call_on_current(function, args);
    }

    // Publishes a new binding for (handle, fd) and retires any previous one. Call under `mutex`.
    Binding* bind_locked(std::uint32_t handle, int fd, std::uint32_t callback, std::uint32_t data,
                         std::uint64_t serial) {
        retire_locked(handle, fd);
        auto owned = std::make_unique<Binding>();
        owned->impl = this;
        owned->handle = handle;
        owned->fd = fd;
        owned->callback = callback;
        owned->data = data;
        owned->serial = serial;
        Binding* raw = owned.get();
        binding_storage.push_back(std::move(owned));
        bindings.emplace(std::make_pair(handle, fd), raw);
        return raw;
    }

    // Marks the binding of (handle, fd) dead so a callback still in flight does nothing but
    // unregister itself. Call under `mutex`.
    void retire_locked(std::uint32_t handle, int fd) {
        const auto it = bindings.find(std::make_pair(handle, fd));
        if (it == bindings.end()) return;
        it->second->active.store(false, std::memory_order_relaxed);
        bindings.erase(it);
    }

    // Forgets an fd registered by that exact addFd call; a later re-registration is untouched.
    void drop_registration_locked(std::uint32_t handle, int fd, std::uint64_t serial) {
        const auto looper_it = loopers.find(handle);
        if (looper_it == loopers.end()) return;
        const auto current = looper_it->second->registrations.find(fd);
        if (current == looper_it->second->registrations.end() || current->second.serial != serial) return;
        looper_it->second->registrations.erase(current);
        retire_locked(handle, fd);
    }

    static int attached_callback(int fd, int events, void* data) {
        auto* binding = static_cast<Binding*>(data);
        return binding->impl->dispatch_attached(*binding, fd, events);
    }

    // Runs on the host thread that owns the real looper, from its own loop, with no guest code
    // running on it. Enters the guest and runs the guest callback with (fd, events, data), like
    // the guest path does from pollOnce. Returning 0 makes the real looper drop the fd.
    int dispatch_attached(Binding& binding, int fd, int events) {
        report_host_callbacks.fetch_add(1, std::memory_order_relaxed);
        if (!binding.active.load(std::memory_order_relaxed)) {
            report_host_callbacks_line();
            return 0;
        }
        GuestCall call;
        call.regs = {static_cast<std::uint32_t>(fd), static_cast<std::uint32_t>(events),
                     binding.data, 0};
        const auto result = invoke(binding.callback, call);
        if (!result) {
            report_host_callbacks_failed.fetch_add(1, std::memory_order_relaxed);
            static std::atomic<bool> logged{false};
            if (!logged.exchange(true)) {
                log("ALooper callback on fd %d could not enter the guest; unregistering it", fd);
            }
            {
                std::lock_guard<std::mutex> lock(mutex);
                drop_registration_locked(binding.handle, fd, binding.serial);
            }
            report_host_callbacks_line();
            return 0;  // never let a fd we cannot serve spin the host thread's loop
        }
        report_host_callbacks_guest.fetch_add(1, std::memory_order_relaxed);
        report_callbacks.fetch_add(1, std::memory_order_relaxed);
        const bool keep = result->r0 != 0;
        if (!keep) {
            report_callbacks_unregistered.fetch_add(1, std::memory_order_relaxed);
            std::lock_guard<std::mutex> lock(mutex);
            drop_registration_locked(binding.handle, fd, binding.serial);
        }
        report_callbacks_line();
        report_host_callbacks_line();
        return keep ? 1 : 0;
    }

    std::uint32_t prepare(GuestThread& thread, int options) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            const auto existing = thread_loopers.find(&thread);
            if (existing != thread_loopers.end()) return existing->second;
        }
        if ((options & ~kAllowNonCallbacks) != 0) return 0;

        // A borrower came from Java and goes back to Java: its own Android looper is what polls,
        // so hand the fds to that one instead of building a poll set nobody ever runs.
        std::uint64_t real = 0;
        if (backend != nullptr && is_borrower(thread)) real = backend->prepare(options);

        int wake_fd = -1;
        if (real == 0) {
            wake_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
            if (wake_fd < 0) return 0;
        }
        std::lock_guard<std::mutex> lock(mutex);
        auto looper = std::make_unique<Looper>();
        looper->handle = next_handle;
        looper->options = options;
        looper->wake_fd = wake_fd;
        looper->attached = real != 0;
        looper->real = real;
        const std::uint32_t handle = looper->handle;
        next_handle += 4;
        thread_loopers.emplace(&thread, handle);
        loopers.emplace(handle, std::move(looper));
        report_loopers.fetch_add(1, std::memory_order_relaxed);
        report_loopers_line();
        if (real != 0) {
            report_attached.fetch_add(1, std::memory_order_relaxed);
            report_attached_line();
        }
        return handle;
    }

    std::uint32_t for_thread(GuestThread& thread) {
        std::lock_guard<std::mutex> lock(mutex);
        const auto it = thread_loopers.find(&thread);
        return it == thread_loopers.end() ? 0 : it->second;
    }

    void acquire(std::uint32_t handle) {
        std::uint64_t real = 0;
        {
            std::lock_guard<std::mutex> lock(mutex);
            const auto it = loopers.find(handle);
            if (it == loopers.end()) return;
            if (it->second->references != std::numeric_limits<std::uint32_t>::max()) {
                ++it->second->references;
            }
            real = it->second->real;
        }
        if (real != 0) backend->acquire(real);
    }

    void release(std::uint32_t handle) {
        std::uint64_t real = 0;
        {
            std::lock_guard<std::mutex> lock(mutex);
            const auto it = loopers.find(handle);
            if (it == loopers.end()) return;
            if (it->second->references > 1) --it->second->references;
            real = it->second->real;
        }
        if (real != 0) backend->release(real);
    }

    int add_fd(std::uint32_t handle, int fd, int ident, int events,
               std::uint32_t callback, std::uint32_t data) {
        int wake_fd = -1;
        std::uint64_t real = 0;
        Binding* binding = nullptr;
        std::uint64_t serial = 0;
        {
            std::lock_guard<std::mutex> lock(mutex);
            const auto it = loopers.find(handle);
            if (it == loopers.end() || fd < 0 || events == 0 ||
                (events & ~(kEventInput | kEventOutput)) != 0) {
                return -1;
            }
            Looper& looper = *it->second;
            if (callback == 0 &&
                (ident < 0 || (looper.options & kAllowNonCallbacks) == 0)) {
                return -1;
            }
            serial = next_serial++;
            looper.registrations[fd] = Registration{fd, ident, events, callback, data, serial};
            if (looper.attached || looper.real_backed) {
                real = looper.real;
                binding = bind_locked(handle, fd, callback, data, serial);
            } else {
                wake_fd = looper.wake_fd;
            }
        }
        if (real != 0) {
            // The real looper needs a host callback even for a guest ident registration: only a
            // callback gets control on the host thread, and nothing here ever calls pollOnce.
            if (backend->add_fd(real, fd, ident, events, &Impl::attached_callback, binding) != 1) {
                std::lock_guard<std::mutex> lock(mutex);
                drop_registration_locked(handle, fd, serial);
                return -1;
            }
        } else {
            signal_eventfd(wake_fd);
        }
        report_fds_added.fetch_add(1, std::memory_order_relaxed);
        report_fds_line();
        return 1;
    }

    // The old ALooper_pollAll: the same wait, except that it never hands a callback result back
    // to the caller. It keeps polling until something the caller can act on happens, so a guest
    // loop that ignores ALOOPER_POLL_CALLBACK (Unity 4.5 does) still makes progress.
    int poll_all(GuestThread& thread, int timeout, std::uint32_t out_fd, std::uint32_t out_events,
                 std::uint32_t out_data) {
        using Clock = std::chrono::steady_clock;
        const Clock::time_point deadline =
            timeout > 0 ? Clock::now() + std::chrono::milliseconds(timeout) : Clock::time_point{};
        int remaining = timeout;
        for (;;) {
            const int result = poll_once(thread, remaining, out_fd, out_events, out_data);
            if (result != kPollCallback) return result;
            if (timeout == 0) return kPollTimeout;  // a poll that must not block ends here
            if (timeout > 0) {
                const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
                if (left.count() <= 0) return kPollTimeout;
                remaining = static_cast<int>(left.count());
            }
        }
    }

    // Gives this thread's looper a real Android looper and moves its registrations onto it. The
    // caller is AInputQueue_attachLooper: a host input queue can only be polled by a real looper,
    // and this thread is a real host thread, so it can have one.
    bool ensure_real_looper(GuestThread& thread) {
        if (backend == nullptr) return false;
        std::uint32_t handle = 0;
        int options = 0;
        std::vector<Registration> registrations;
        {
            std::lock_guard<std::mutex> lock(mutex);
            const auto thread_it = thread_loopers.find(&thread);
            if (thread_it == thread_loopers.end()) return false;
            const auto looper_it = loopers.find(thread_it->second);
            if (looper_it == loopers.end()) return false;
            Looper& looper = *looper_it->second;
            if (looper.attached || looper.real_backed) return true;  // already served by a real looper
            handle = thread_it->second;
            options = looper.options;
            for (const auto& [fd, registration] : looper.registrations) {
                (void)fd;
                registrations.push_back(registration);
            }
        }

        const std::uint64_t real = backend->prepare(options);
        if (real == 0) {
            log("cannot give this guest thread a real looper; an input queue attached to it will "
                "never deliver");
            return false;
        }
        for (const Registration& registration : registrations) {
            Binding* binding = nullptr;
            {
                std::lock_guard<std::mutex> lock(mutex);
                binding = bind_locked(handle, registration.fd, registration.callback, registration.data,
                                      registration.serial);
            }
            backend->add_fd(real, registration.fd, registration.ident, registration.events,
                            &Impl::attached_callback, binding);
        }
        {
            std::lock_guard<std::mutex> lock(mutex);
            const auto looper_it = loopers.find(handle);
            if (looper_it == loopers.end()) return false;
            looper_it->second->real = real;
            looper_it->second->real_backed = true;
        }
        // Anything already waiting in the old poll must come back and take the new path.
        signal_eventfd(wake_fd_of(handle));
        log("guest looper 0x%08x now runs on a real Android looper (%zu registrations moved)", handle,
            registrations.size());
        runtime_report().note_jni_detail("looper-real", "input queue attached", true);
        return true;
    }

    int wake_fd_of(std::uint32_t handle) {
        std::lock_guard<std::mutex> lock(mutex);
        const auto it = loopers.find(handle);
        return it != loopers.end() ? it->second->wake_fd : -1;
    }

    int remove_fd(std::uint32_t handle, int fd) {
        int wake_fd = -1;
        std::uint64_t real = 0;
        {
            std::lock_guard<std::mutex> lock(mutex);
            const auto it = loopers.find(handle);
            if (it == loopers.end()) return -1;
            Looper& looper = *it->second;
            if (looper.registrations.erase(fd) == 0) return 0;
            if (looper.attached || looper.real_backed) {
                real = looper.real;
                retire_locked(handle, fd);
            } else {
                wake_fd = looper.wake_fd;
            }
        }
        if (real != 0) {
            backend->remove_fd(real, fd);
        } else {
            signal_eventfd(wake_fd);
        }
        report_fds_removed.fetch_add(1, std::memory_order_relaxed);
        report_fds_line();
        return 1;
    }

    void wake(std::uint32_t handle) {
        int wake_fd = -1;
        std::uint64_t real = 0;
        {
            std::lock_guard<std::mutex> lock(mutex);
            const auto it = loopers.find(handle);
            if (it != loopers.end()) {
                wake_fd = it->second->wake_fd;
                real = it->second->real;
            }
        }
        if (real != 0) {
            backend->wake(real);
        } else if (wake_fd >= 0) {
            signal_eventfd(wake_fd);
        }
        report_wakes.fetch_add(1, std::memory_order_relaxed);
        report_wakes_line();
    }

    int poll_once(GuestThread& thread, int timeout, std::uint32_t out_fd,
                  std::uint32_t out_events, std::uint32_t out_data) {
        std::uint32_t looper_handle = 0;
        int wake_fd = -1;
        bool attached = false;
        bool real_backed = false;
        std::vector<Registration> registrations;
        {
            std::lock_guard<std::mutex> lock(mutex);
            const auto thread_it = thread_loopers.find(&thread);
            if (thread_it == thread_loopers.end()) return kPollError;
            const auto looper_it = loopers.find(thread_it->second);
            if (looper_it == loopers.end()) return kPollError;
            looper_handle = thread_it->second;
            wake_fd = looper_it->second->wake_fd;
            attached = looper_it->second->attached;
            real_backed = looper_it->second->real_backed;
            registrations.reserve(looper_it->second->registrations.size());
            for (const auto& [fd, registration] : looper_it->second->registrations) {
                (void)fd;
                registrations.push_back(registration);
            }
        }

        // Diagnostics only: every exit path below runs through `finish`, which counts the
        // poll and records this thread's "last" state in the runtime report.
        auto finish = [&](int result) {
            report_polls.fetch_add(1, std::memory_order_relaxed);
            switch (result) {
            case kPollWake: report_polls_wake.fetch_add(1, std::memory_order_relaxed); break;
            case kPollCallback: report_polls_callback.fetch_add(1, std::memory_order_relaxed); break;
            case kPollTimeout: report_polls_timeout.fetch_add(1, std::memory_order_relaxed); break;
            case kPollError: report_polls_error.fetch_add(1, std::memory_order_relaxed); break;
            default: if (result >= 0) report_polls_ident.fetch_add(1, std::memory_order_relaxed); break;
            }
            report_polls_line();
            report_last(thread, registrations.size(), result);
            return result;
        };

        if (real_backed) {
            // This thread owns a real Android looper, and every registration was handed to it,
            // so the real pollOnce is the poll: it blocks and dispatches, ours included. The out
            // parameters stay unserved here, because an input queue is a callback registration
            // and that is what this mode exists for.
            return finish(backend->poll_once(timeout));
        }

        if (attached) {
            // The guest is asking to run a loop the host thread's Java Looper.loop() already
            // runs. Blocking here would stall that loop, so report a wake and let the guest
            // return; its callbacks arrive through attached_callback instead.
            static std::atomic<bool> logged{false};
            if (!logged.exchange(true)) {
                log("ALooper_pollOnce on a looper owned by the host thread's Android looper: "
                    "returning WAKE without polling");
            }
            return finish(kPollWake);
        }

        std::vector<pollfd> poll_fds;
        poll_fds.reserve(registrations.size() + 1);
        poll_fds.push_back(pollfd{wake_fd, POLLIN, 0});
        for (const Registration& registration : registrations) {
            poll_fds.push_back(pollfd{registration.fd, poll_events(registration.events), 0});
        }

        int ready;
        do {
            ready = poll(poll_fds.data(), static_cast<nfds_t>(poll_fds.size()), timeout);
        } while (ready < 0 && errno == EINTR);
        if (ready < 0) return finish(kPollError);
        if (ready == 0) return finish(kPollTimeout);

        const bool woke = poll_fds[0].revents != 0;
        if (woke) drain_eventfd(wake_fd);
        bool callback_invoked = false;
        const Registration* non_callback = nullptr;
        int non_callback_events = 0;
        for (std::size_t i = 0; i < registrations.size(); ++i) {
            const int events = looper_events(poll_fds[i + 1].revents);
            if (events == 0) continue;
            const Registration& registration = registrations[i];
            if (registration.callback != 0) {
                GuestCall call;
                call.regs = {static_cast<std::uint32_t>(registration.fd),
                             static_cast<std::uint32_t>(events), registration.data, 0};
                const auto result = runtime.call_on_current(registration.callback, call);
                if (!result) return finish(kPollError);
                callback_invoked = true;
                report_callbacks.fetch_add(1, std::memory_order_relaxed);
                if (result->r0 == 0) {
                    report_callbacks_unregistered.fetch_add(1, std::memory_order_relaxed);
                    std::lock_guard<std::mutex> lock(mutex);
                    const auto looper_it = loopers.find(looper_handle);
                    if (looper_it != loopers.end()) {
                        auto current = looper_it->second->registrations.find(registration.fd);
                        if (current != looper_it->second->registrations.end() &&
                            current->second.serial == registration.serial) {
                            looper_it->second->registrations.erase(current);
                        }
                    }
                }
                report_callbacks_line();
                continue;
            }
            if (non_callback == nullptr) {
                non_callback = &registration;
                non_callback_events = events;
            }
        }
        if (callback_invoked) return finish(kPollCallback);
        if (non_callback != nullptr) {
            if (!write_guest(out_fd, static_cast<std::uint32_t>(non_callback->fd)) ||
                !write_guest(out_events, static_cast<std::uint32_t>(non_callback_events)) ||
                !write_guest(out_data, non_callback->data)) {
                return finish(kPollError);
            }
            return finish(non_callback->ident);
        }
        return finish(woke ? kPollWake : kPollError);
    }

    LibraryRuntime& runtime;
    AndroidLooperBackend* backend = nullptr;
    GuestInvoker invoker;
    BorrowerProbe borrower_probe;
    std::mutex mutex;
    std::unordered_map<const GuestThread*, std::uint32_t> thread_loopers;
    std::unordered_map<std::uint32_t, std::unique_ptr<Looper>> loopers;
    std::map<std::pair<std::uint32_t, int>, Binding*> bindings;
    std::vector<std::unique_ptr<Binding>> binding_storage;  // never shrinks; see Binding
    std::uint32_t next_handle = 0x7a000000u;
    std::uint64_t next_serial = 1;
};

HostLooper::HostLooper(LibraryRuntime& runtime, AndroidLooperBackend* backend, GuestInvoker invoker,
                       BorrowerProbe borrower_probe)
    : impl_(std::make_unique<Impl>(runtime, backend, std::move(invoker), std::move(borrower_probe))) {}
HostLooper::~HostLooper() = default;

bool HostLooper::ensure_real_looper(GuestThread& thread) {
    return impl_->ensure_real_looper(thread);
}

bool HostLooper::handle_host_call(std::uint32_t index, GuestThread& thread) {
    bool valid = true;
    const std::uint32_t r0 = impl_->argument(thread, 0, valid);
    const std::uint32_t r1 = impl_->argument(thread, 1, valid);
    const std::uint32_t r2 = impl_->argument(thread, 2, valid);
    const std::uint32_t r3 = impl_->argument(thread, 3, valid);
    auto finish = [&](std::int32_t result) {
        thread.regs()[0] = static_cast<std::uint32_t>(result);
        thread.regs()[1] = 0;
        return true;
    };

    switch (index) {
    case ZB_COMPAT_HC_ALooper_forThread:
        return finish(static_cast<std::int32_t>(impl_->for_thread(thread)));
    case ZB_COMPAT_HC_ALooper_prepare: {
        const std::uint32_t looper = impl_->prepare(thread, static_cast<int>(r0));
        // Which threads own a looper, whether or not they ever poll it: a looper prepared on a
        // thread that never polls means that thread's message loop is not being run at all.
        static std::mutex prepared_mutex;
        static std::string prepared;
        {
            std::lock_guard<std::mutex> lock(prepared_mutex);
            if (prepared.size() < 200) {
                char text[48];
                std::snprintf(text, sizeof text, "%s%d=0x%x", prepared.empty() ? "" : " ",
                              static_cast<int>(thread.tid), looper);
                prepared += text;
            }
            runtime_report().note_looper_detail("prepared", prepared, true);
        }
        return finish(static_cast<std::int32_t>(looper));
    }
    case ZB_COMPAT_HC_ALooper_acquire:
        impl_->acquire(r0);
        return finish(0);
    case ZB_COMPAT_HC_ALooper_release:
        impl_->release(r0);
        return finish(0);
    case ZB_COMPAT_HC_ALooper_addFd: {
        const std::uint32_t callback = impl_->argument(thread, 4, valid);
        const std::uint32_t data = impl_->argument(thread, 5, valid);
        if (!valid) return finish(-1);
        const int added = impl_->add_fd(r0, static_cast<std::int32_t>(r1),
                                        static_cast<std::int32_t>(r2), static_cast<int>(r3),
                                        callback, data);
        static std::atomic<unsigned> add_fd_calls{0};
        const unsigned seen = add_fd_calls.fetch_add(1) + 1;
        if (seen <= 4) {
            char text[128];
            std::snprintf(text, sizeof text, "tid=%d looper=0x%x fd=%d ident=%d events=0x%x result=%d",
                          static_cast<int>(thread.tid), r0, static_cast<int>(r1),
                          static_cast<int>(r2), r3, added);
            runtime_report().note_looper_detail("addfd-" + std::to_string(seen), text, false);
        }
        return finish(added);
    }
    case ZB_COMPAT_HC_ALooper_removeFd:
        return finish(impl_->remove_fd(r0, static_cast<std::int32_t>(r1)));
    case ZB_COMPAT_HC_ALooper_wake:
        impl_->wake(r0);
        return finish(0);
    case ZB_COMPAT_HC_ALooper_pollOnce:
        return finish(impl_->poll_once(thread, static_cast<std::int32_t>(r0), r1, r2, r3));
    case ZB_COMPAT_HC_ALooper_pollAll:
        return finish(impl_->poll_all(thread, static_cast<std::int32_t>(r0), r1, r2, r3));
    default:
        return false;
    }
}

}  // namespace zb
