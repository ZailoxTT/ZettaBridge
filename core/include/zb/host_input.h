#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>

#include "zb/guest_thread.h"
#include "zb/input_backend.h"
#include "zb/jni_handles.h"
#include "zb/library_runtime.h"

namespace zb {

// AInputQueue_* and the input event accessors.
//
// Queues and events are host objects the framework owns; the guest sees 32-bit handles. An event
// handle is valid from AInputQueue_getEvent until AInputQueue_finishEvent, and an accessor on a
// handle that is not live is rejected and recorded rather than followed: a guest that keeps an
// event past its life would otherwise read framework memory that has been reused.
class HostInput {
public:
    // Called when the guest attaches a queue to its looper: the calling guest thread must end up
    // with a real Android looper, because a host AInputQueue has no descriptor we could poll.
    // Without one (the host build, or a platform that refuses) the attach is recorded and the
    // queue simply never delivers, which the report says.
    // (thread, the looper handle the guest named, out: the real looper to attach to).
    using RealLooperRequest = std::function<bool(GuestThread& thread, std::uint32_t looper, std::uint64_t& real)>;

    HostInput(LibraryRuntime& runtime, InputBackend& backend, RealLooperRequest real_looper = {})
        : runtime_(runtime), backend_(backend), real_looper_(std::move(real_looper)) {}
    HostInput(const HostInput&) = delete;
    HostInput& operator=(const HostInput&) = delete;

    // Serves ZB_INPUT_HC_* indices; returns false for any other index.
    bool handle_host_call(std::uint32_t index, GuestThread& thread);

    // The framework created or destroyed a queue (ANativeActivity's onInputQueueCreated and
    // onInputQueueDestroyed). The handle is what the guest callback receives.
    std::uint32_t add_queue(void* queue);
    // Drops the queue and every event still outstanding on it, and says how many those were.
    std::size_t remove_queue(std::uint32_t handle);

    // The host queue behind a handle, for the glue; nullptr when the handle is not live.
    void* queue_for(std::uint32_t handle) const;

private:
    const void* live_event(std::uint32_t handle) const;
    void* live_queue(std::uint32_t handle) const;
    void report_queue_state();
    // Records the rejection and answers the call with zero. Always returns true: the call was
    // served, and the guest sees what a real device returns for an event it no longer owns.
    bool reject(const char* function, std::uint32_t handle);

    LibraryRuntime& runtime_;
    InputBackend& backend_;
    RealLooperRequest real_looper_;
    GlobalHandles queues_{HandleKind::Global};
    GlobalHandles events_{HandleKind::Global};
    mutable std::mutex mutex_;
    // Every live event handle and the queue it came from, so a destroyed queue takes its
    // outstanding events with it instead of leaving handles that point at freed memory.
    std::unordered_map<std::uint32_t, std::uint32_t> live_events_;
    std::string first_rejection_;
    std::size_t rejections_ = 0;
    std::size_t events_gotten_ = 0;
    std::size_t events_pre_dispatched_ = 0;
    std::size_t events_finished_ = 0;
    std::size_t events_released_ = 0;
};

}  // namespace zb
