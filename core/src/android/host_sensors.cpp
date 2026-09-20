#include "zb/host_sensors.h"

#include <cerrno>
#include <cstring>

#include "zb/guest_memory.h"
#include "zb/sensor_hostcalls.h"

namespace zb {

namespace {

// The manager is a single object on a real device too, so one fixed handle is enough. It must be
// nonzero: a guest that gets nothing back from ASensorManager_getInstance usually gives up.
constexpr std::uint32_t kManagerHandle = 1;

// What a call that cannot be served returns. The NDK sensor functions report failure as a
// negative errno, and this is the one a real device gives for a sensor it does not have.
constexpr std::int32_t kUnavailable = -EINVAL;

}  // namespace

std::uint32_t HostSensors::create_queue() {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::uint32_t handle = next_queue_++;
    queues_.insert(handle);
    return handle;
}

bool HostSensors::destroy_queue(std::uint32_t handle) {
    std::lock_guard<std::mutex> lock(mutex_);
    return queues_.erase(handle) != 0;
}

bool HostSensors::live_queue(std::uint32_t handle) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return queues_.count(handle) != 0;
}

bool HostSensors::handle_host_call(std::uint32_t index, GuestThread& thread) {
    if (index < ZB_SENSOR_HC_FIRST || index > ZB_SENSOR_HC_LAST) return false;
    auto& regs = thread.regs();
    switch (index) {
    case ZB_SENSOR_HC_ASensorManager_getInstance:
    case ZB_SENSOR_HC_ASensorManager_getInstanceForPackage:
        regs[0] = kManagerHandle;
        return true;

    case ZB_SENSOR_HC_ASensorManager_getSensorList: {
        // ASensorList is one pointer the caller receives; an empty inventory writes a null list
        // and reports zero entries. A guest that cannot be written to still gets the count.
        if (std::uint8_t* list = runtime_.memory().host_ptr(regs[1], 4, kPageRead | kPageWrite)) {
            std::uint32_t empty = 0;
            std::memcpy(list, &empty, sizeof empty);
        }
        regs[0] = 0;
        return true;
    }
    case ZB_SENSOR_HC_ASensorManager_getDynamicSensorList: {
        if (std::uint8_t* list = runtime_.memory().host_ptr(regs[1], 4, kPageRead | kPageWrite)) {
            std::uint32_t empty = 0;
            std::memcpy(list, &empty, sizeof empty);
        }
        regs[0] = 0;
        return true;
    }
    case ZB_SENSOR_HC_ASensorManager_getDefaultSensor:
    case ZB_SENSOR_HC_ASensorManager_getDefaultSensorEx:
        regs[0] = 0;  // no sensor of that type
        return true;

    case ZB_SENSOR_HC_ASensorManager_createEventQueue:
        // The queue exists and is valid; it simply never has an event in it.
        regs[0] = create_queue();
        return true;
    case ZB_SENSOR_HC_ASensorManager_destroyEventQueue:
        regs[0] = static_cast<std::uint32_t>(destroy_queue(regs[1]) ? 0 : kUnavailable);
        return true;

    case ZB_SENSOR_HC_ASensorEventQueue_hasEvents:
        // 0 means "no events waiting"; a negative value would mean the queue is broken.
        regs[0] = static_cast<std::uint32_t>(live_queue(regs[0]) ? 0 : kUnavailable);
        return true;
    case ZB_SENSOR_HC_ASensorEventQueue_getEvents:
        regs[0] = static_cast<std::uint32_t>(live_queue(regs[0]) ? 0 : kUnavailable);
        return true;

    case ZB_SENSOR_HC_ASensorEventQueue_registerSensor:
    case ZB_SENSOR_HC_ASensorEventQueue_enableSensor:
    case ZB_SENSOR_HC_ASensorEventQueue_disableSensor:
    case ZB_SENSOR_HC_ASensorEventQueue_setEventRate:
    case ZB_SENSOR_HC_ASensorEventQueue_requestAdditionalInfoEvents:
    case ZB_SENSOR_HC_ASensorManager_createSharedMemoryDirectChannel:
    case ZB_SENSOR_HC_ASensorManager_createHardwareBufferDirectChannel:
    case ZB_SENSOR_HC_ASensorManager_configureDirectReport:
        // There is no sensor to enable, so these fail the way they do for an unknown sensor.
        regs[0] = static_cast<std::uint32_t>(kUnavailable);
        return true;
    case ZB_SENSOR_HC_ASensorManager_destroyDirectChannel:
        regs[0] = 0;  // void
        return true;

    // The accessors take a sensor, and no sensor was ever handed out, so they answer for one that
    // does not exist rather than dereferencing whatever the guest passed.
    case ZB_SENSOR_HC_ASensor_getName:
    case ZB_SENSOR_HC_ASensor_getVendor:
    case ZB_SENSOR_HC_ASensor_getStringType:
        regs[0] = 0;  // const char*: null, as for a sensor that is not there
        return true;
    case ZB_SENSOR_HC_ASensor_getType:
        regs[0] = 0;  // ASENSOR_TYPE_INVALID
        return true;
    case ZB_SENSOR_HC_ASensor_getResolution:
        regs[0] = 0;  // 0.0f in AAPCS softfp
        return true;
    case ZB_SENSOR_HC_ASensor_getMinDelay:
    case ZB_SENSOR_HC_ASensor_getFifoMaxEventCount:
    case ZB_SENSOR_HC_ASensor_getFifoReservedEventCount:
    case ZB_SENSOR_HC_ASensor_getReportingMode:
    case ZB_SENSOR_HC_ASensor_getHighestDirectReportRateLevel:
        regs[0] = 0;
        return true;
    case ZB_SENSOR_HC_ASensor_getHandle:
        regs[0] = static_cast<std::uint32_t>(-1);
        return true;
    case ZB_SENSOR_HC_ASensor_isWakeUpSensor:
    case ZB_SENSOR_HC_ASensor_isDirectChannelTypeSupported:
        regs[0] = 0;  // false
        return true;

    default:
        // Every index in the range is listed above; a new generated name lands here.
        regs[0] = static_cast<std::uint32_t>(kUnavailable);
        return true;
    }
}

}  // namespace zb
