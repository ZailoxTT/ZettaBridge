#pragma once

#include <cstdint>
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
    HostInput(LibraryRuntime& runtime, InputBackend& backend) : runtime_(runtime), backend_(backend) {}
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
    // Records the rejection and answers the call with zero. Always returns true: the call was
    // served, and the guest sees what a real device returns for an event it no longer owns.
    bool reject(const char* function, std::uint32_t handle);

    LibraryRuntime& runtime_;
    InputBackend& backend_;
    GlobalHandles queues_{HandleKind::Global};
    GlobalHandles events_{HandleKind::Global};
    mutable std::mutex mutex_;
    // Every live event handle and the queue it came from, so a destroyed queue takes its
    // outstanding events with it instead of leaving handles that point at freed memory.
    std::unordered_map<std::uint32_t, std::uint32_t> live_events_;
    std::string first_rejection_;
    std::size_t rejections_ = 0;
};

}  // namespace zb
