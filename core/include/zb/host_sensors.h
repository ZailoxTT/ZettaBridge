#pragma once

#include <cstdint>
#include <mutex>
#include <unordered_set>

#include "zb/guest_thread.h"
#include "zb/library_runtime.h"

namespace zb {

// ASensor_* host-call dispatcher: the 32 entry points core/include/zb/sensor_hostcalls.h names.
//
// The guest needs every one of them to exist, because one unresolved import stops the library
// from loading at all: Unity stops on ASensorEventQueue_disableSensor, and the next guest would
// stop on a different name. What they report is an inventory with no sensors in it, which is a
// state a real device can be in and which every caller already has to handle.
//
// Nothing is invented: no sensor is described, no event is ever produced, and enabling a sensor
// fails. Forwarding the real sensors of the device is separate work; until then a guest that
// asks is told there is nothing, rather than being fed made-up motion.
class HostSensors {
public:
    explicit HostSensors(LibraryRuntime& runtime) : runtime_(runtime) {}
    HostSensors(const HostSensors&) = delete;
    HostSensors& operator=(const HostSensors&) = delete;

    // Serves ZB_SENSOR_HC_* indices; returns false for any other index.
    bool handle_host_call(std::uint32_t index, GuestThread& thread);

private:
    std::uint32_t create_queue();
    bool destroy_queue(std::uint32_t handle);
    bool live_queue(std::uint32_t handle) const;

    LibraryRuntime& runtime_;
    mutable std::mutex mutex_;
    std::unordered_set<std::uint32_t> queues_;
    std::uint32_t next_queue_ = 1;
};

}  // namespace zb
