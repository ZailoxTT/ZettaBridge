#pragma once

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include "zb/input_backend.h"

// In-memory InputBackend for host tests. Events are small objects the mock owns; accessors return
// values derived from the event so a test can tell one from another, and every call is recorded.
class MockInput final : public zb::InputBackend {
public:
    struct Event {
        std::int32_t type = 1;      // AINPUT_EVENT_TYPE_KEY
        std::int32_t device = 7;
        float x = 1.5f;
        std::int64_t event_time = 0x1234'5678'9abcLL;
    };

    void queue_event(Event event) { pending_.push_back(event); }
    void* queue() { return &queue_object_; }

    std::int32_t queue_has_events(void* queue) override {
        calls.push_back("hasEvents");
        return queue == &queue_object_ && !pending_.empty() ? 1 : 0;
    }

    void* queue_get_event(void* queue, std::int32_t& status) override {
        calls.push_back("getEvent");
        if (queue != &queue_object_ || pending_.empty()) {
            status = -1;  // AINPUT_EVENT_NONE-ish: the NDK returns a negative result
            return nullptr;
        }
        events_.push_back(pending_.front());
        pending_.pop_front();
        status = 0;
        return &events_.back();
    }

    std::int32_t queue_pre_dispatch(void* queue, void* event) override {
        calls.push_back("preDispatch");
        (void)queue;
        (void)event;
        return pre_dispatch_result;
    }

    void queue_finish_event(void* queue, void* event, std::int32_t handled) override {
        calls.push_back("finishEvent");
        (void)queue;
        finished.push_back(event);
        last_handled = handled;
    }

    void queue_attach_looper(void* queue, std::int32_t ident, void* data, std::uint64_t looper) override {
        attached_looper = looper;
        calls.push_back("attachLooper");
        (void)queue;
        attached_ident = ident;
        attached_data = data;
        attached = true;
    }

    void queue_detach_looper(void* queue) override {
        calls.push_back("detachLooper");
        (void)queue;
        attached = false;
    }

    std::int32_t AInputEvent_getType(const void* event) override { return at(event).type; }
    std::int32_t AInputEvent_getDeviceId(const void* event) override { return at(event).device; }
    float AMotionEvent_getX(const void* motion_event, std::size_t pointer_index) override {
        return at(motion_event).x + static_cast<float>(pointer_index);
    }
    std::int64_t AMotionEvent_getEventTime(const void* motion_event) override {
        return at(motion_event).event_time;
    }

    std::vector<std::string> calls;
    std::vector<void*> finished;
    std::int32_t pre_dispatch_result = 0;
    std::int32_t last_handled = -1;
    std::int32_t attached_ident = -1;
    void* attached_data = nullptr;
    std::uint64_t attached_looper = 0;
    bool attached = false;

private:
    const Event& at(const void* event) const { return *static_cast<const Event*>(event); }

    int queue_object_ = 0;
    std::deque<Event> pending_;
    std::deque<Event> events_;
};
