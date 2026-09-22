#include "zb/host_input.h"

#include <cstring>

#include "zb/guest_memory.h"
#include "zb/input_hostcalls.h"
#include "zb/log.h"
#include "zb/runtime_report.h"

namespace zb {

namespace {

inline void* as_pointer(std::uint64_t value) {
    return reinterpret_cast<void*>(static_cast<std::uintptr_t>(value));
}

inline std::uint64_t from_pointer(const void* value) {
    return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(value));
}

}  // namespace

std::uint32_t HostInput::add_queue(void* queue) {
    if (queue == nullptr) return 0;
    return queues_.add(from_pointer(queue));
}

std::size_t HostInput::remove_queue(std::uint32_t handle) {
    const std::optional<std::uint64_t> value = queues_.remove(handle);
    if (!value) return 0;
    std::size_t outstanding = 0;
    std::vector<std::uint32_t> orphans;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& [event, owner] : live_events_) {
            if (owner == handle) orphans.push_back(event);
        }
        for (std::uint32_t event : orphans) live_events_.erase(event);
        outstanding = orphans.size();
    }
    for (std::uint32_t event : orphans) events_.remove(event);
    if (outstanding != 0) {
        log("input queue destroyed with %zu events the guest never finished", outstanding);
        runtime_report().note_jni_detail("input-events-dropped", std::to_string(outstanding), true);
    }
    return outstanding;
}

void* HostInput::queue_for(std::uint32_t handle) const {
    const std::optional<std::uint64_t> value = queues_.get(handle);
    return value && *value != 0 ? as_pointer(*value) : nullptr;
}

const void* HostInput::live_event(std::uint32_t handle) const {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (live_events_.count(handle) == 0) return nullptr;
    }
    const std::optional<std::uint64_t> value = events_.get(handle);
    return value && *value != 0 ? as_pointer(*value) : nullptr;
}

void* HostInput::live_queue(std::uint32_t handle) const {
    return queue_for(handle);
}

bool HostInput::reject(const char* function, std::uint32_t handle) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++rejections_;
        if (first_rejection_.empty()) {
            first_rejection_ = std::string(function) + " handle " + std::to_string(handle);
            runtime_report().note_jni_detail("input-rejected", first_rejection_, true);
        }
    }
    log("input call %s rejected: handle %u is not live", function, handle);
    return true;
}

bool HostInput::handle_host_call(std::uint32_t index, GuestThread& thread) {
    if (index < ZB_INPUT_HC_FIRST || index > ZB_INPUT_HC_LAST) return false;
    auto& regs = thread.regs();
    switch (index) {
    case ZB_INPUT_HC_AInputQueue_hasEvents: {
        void* queue = live_queue(regs[0]);
        if (queue == nullptr) { regs[0] = 0; return reject("AInputQueue_hasEvents", regs[0]); }
        regs[0] = static_cast<std::uint32_t>(backend_.queue_has_events(queue));
        return true;
    }
    case ZB_INPUT_HC_AInputQueue_getEvent: {
        void* queue = live_queue(regs[0]);
        if (queue == nullptr) { regs[0] = 0; return reject("AInputQueue_getEvent", regs[0]); }
        // The guest gets the event as a handle written through its own pointer, which must be
        // writable before the backend is asked for an event we would then have to drop.
        std::uint8_t* out = runtime_.memory().host_ptr(regs[1], 4, kPageRead | kPageWrite);
        if (out == nullptr) {
            regs[0] = static_cast<std::uint32_t>(-1);
            return reject("AInputQueue_getEvent output", regs[1]);
        }
        std::int32_t status = 0;
        void* event = backend_.queue_get_event(queue, status);
        std::uint32_t handle = 0;
        if (event != nullptr) {
            handle = events_.add(from_pointer(event));
            std::lock_guard<std::mutex> lock(mutex_);
            live_events_[handle] = regs[0];
        }
        std::memcpy(out, &handle, sizeof handle);
        regs[0] = static_cast<std::uint32_t>(status);
        return true;
    }
    case ZB_INPUT_HC_AInputQueue_preDispatchEvent: {
        void* queue = live_queue(regs[0]);
        const void* event = live_event(regs[1]);
        if (queue == nullptr) { regs[0] = 0; return reject("AInputQueue_preDispatchEvent", regs[0]); }
        if (event == nullptr) { regs[0] = 0; return reject("AInputQueue_preDispatchEvent event", regs[1]); }
        const std::int32_t handled = backend_.queue_pre_dispatch(queue, const_cast<void*>(event));
        // A pre-dispatched event belongs to the framework now (the soft keyboard path): the guest
        // must not finish it, so its handle ends here.
        if (handled != 0) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                live_events_.erase(regs[1]);
            }
            events_.remove(regs[1]);
        }
        regs[0] = static_cast<std::uint32_t>(handled);
        return true;
    }
    case ZB_INPUT_HC_AInputQueue_finishEvent: {
        void* queue = live_queue(regs[0]);
        const void* event = live_event(regs[1]);
        if (queue == nullptr) { regs[0] = 0; return reject("AInputQueue_finishEvent", regs[0]); }
        if (event == nullptr) { regs[0] = 0; return reject("AInputQueue_finishEvent event", regs[1]); }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            live_events_.erase(regs[1]);
        }
        events_.remove(regs[1]);
        backend_.queue_finish_event(queue, const_cast<void*>(event), static_cast<std::int32_t>(regs[2]));
        regs[0] = 0;
        return true;
    }
    case ZB_INPUT_HC_AInputEvent_release: {
        const void* event = live_event(regs[0]);
        if (event == nullptr) { regs[0] = 0; return reject("AInputEvent_release", regs[0]); }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            live_events_.erase(regs[0]);
        }
        events_.remove(regs[0]);
        regs[0] = 0;
        return true;
    }
    case ZB_INPUT_HC_AInputQueue_attachLooper: {
        void* queue = live_queue(regs[0]);
        if (queue == nullptr) { regs[0] = 0; return reject("AInputQueue_attachLooper", regs[0]); }
        // regs[1] is the guest's looper, regs[3] and regs[4] its callback and data. A host
        // AInputQueue has no descriptor we could poll, so this thread must first have a real
        // Android looper; the queue then goes to it and the guest's own poll picks it up.
        const bool ready = real_looper_ ? real_looper_(thread) : false;
        if (!ready) {
            runtime_report().note_jni_detail("input-attach", "no real looper on this thread", true);
            log("AInputQueue_attachLooper: this thread has no real looper, so the queue will not "
                "deliver");
        }
        backend_.queue_attach_looper(queue, static_cast<std::int32_t>(regs[2]));
        regs[0] = 0;
        return true;
    }
    case ZB_INPUT_HC_AInputQueue_detachLooper: {
        void* queue = live_queue(regs[0]);
        if (queue == nullptr) { regs[0] = 0; return reject("AInputQueue_detachLooper", regs[0]); }
        backend_.queue_detach_looper(queue);
        regs[0] = 0;
        return true;
    }
    // The JNI conversions hand a Java object to or from native code. A guest that asks for one
    // would receive a host jobject, which it cannot use; it is told there is none, which is what
    // a device without that API level answers.
    case ZB_INPUT_HC_AInputEvent_toJava:
    case ZB_INPUT_HC_AInputQueue_fromJava:
    case ZB_INPUT_HC_AKeyEvent_fromJava:
    case ZB_INPUT_HC_AMotionEvent_fromJava:
        runtime_report().note_jni_detail("input-unsupported", "JNI event conversion", false);
        regs[0] = 0;
        return true;

#include "gen/input_dispatch.inc"

    default:
        return false;
    }
}

}  // namespace zb
