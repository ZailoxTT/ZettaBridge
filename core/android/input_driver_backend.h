#pragma once

#include <android/input.h>
#include <dlfcn.h>
#include <android/looper.h>

#include "zb/input_backend.h"

namespace zb {

// The real input backend of the device: the NDK's own entry points. The generated half is one
// cast and one call per accessor; the queue lifecycle is here because a queue is attached to the
// real Android looper of the calling thread, which is the only thing that can poll it.
class InputDriverBackend final : public InputBackend {
public:
    std::int32_t queue_has_events(void* queue) override {
        return AInputQueue_hasEvents(static_cast<AInputQueue*>(queue));
    }

    void* queue_get_event(void* queue, std::int32_t& status) override {
        AInputEvent* event = nullptr;
        status = AInputQueue_getEvent(static_cast<AInputQueue*>(queue), &event);
        return status < 0 ? nullptr : event;
    }

    std::int32_t queue_pre_dispatch(void* queue, void* event) override {
        return AInputQueue_preDispatchEvent(static_cast<AInputQueue*>(queue), static_cast<AInputEvent*>(event));
    }

    void queue_finish_event(void* queue, void* event, std::int32_t handled) override {
        AInputQueue_finishEvent(static_cast<AInputQueue*>(queue), static_cast<AInputEvent*>(event), handled);
    }

    // The queue goes to this thread's own looper: a host AInputQueue has no descriptor we could
    // poll ourselves. The thread must already have one (HostLooper's real-looper mode).
    void queue_attach_looper(void* queue, std::int32_t ident, void* data, std::uint64_t target) override {
        ALooper* looper = target != 0 ? reinterpret_cast<ALooper*>(static_cast<std::uintptr_t>(target))
                                      : ALooper_forThread();
        if (looper == nullptr) return;
        // No callback: the guest polls for the ident itself, which is what the NDK's own glue
        // does. `data` is the guest's value and comes back out of ALooper_pollOnce unchanged.
        AInputQueue_attachLooper(static_cast<AInputQueue*>(queue), looper, ident, nullptr, data);
    }

    void queue_detach_looper(void* queue) override {
        AInputQueue_detachLooper(static_cast<AInputQueue*>(queue));
    }

#include "gen/input_driver.inc"
};

}  // namespace zb
