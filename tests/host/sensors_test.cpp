// ASensor_* host calls: every generated entry point answers, the inventory is empty, an event
// queue exists but never produces an event, and enabling a sensor fails instead of inventing one.
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

#include <dynarmic/interface/exclusive_monitor.h>
#include <sys/mman.h>

#include "check.h"
#include "zb/host_sensors.h"
#include "zb/library_runtime.h"
#include "zb/sensor_hostcalls.h"

namespace {

constexpr std::uint32_t kData = 0x20000;

std::uint32_t call_sensor(zb::HostSensors& sensors, zb::GuestThread& thread, std::uint32_t index,
                          std::initializer_list<std::uint32_t> args) {
    auto& regs = thread.regs();
    regs[0] = regs[1] = regs[2] = regs[3] = 0;
    unsigned position = 0;
    for (std::uint32_t value : args) regs[position++] = value;
    CHECK(sensors.handle_host_call(index, thread));
    return regs[0];
}

}  // namespace

int main() {
    zb::LibraryRuntime runtime;
    CHECK(runtime.memory().map_anon(kData, 0x1000, PROT_READ | PROT_WRITE));
    zb::HostSensors sensors(runtime);
    Dynarmic::ExclusiveMonitor monitor(1);
    zb::GuestThread thread(runtime.memory(), &monitor, 0, false, zb::kCarrierCodeCacheSize);

    // Indices outside the sensor range belong to another unit and must fall through.
    CHECK(!sensors.handle_host_call(zb::ZB_SENSOR_HC_FIRST - 1, thread));
    CHECK(!sensors.handle_host_call(zb::ZB_SENSOR_HC_LAST + 1, thread));

    // A manager exists: a guest that gets nothing back from getInstance gives up before it starts.
    const std::uint32_t manager = call_sensor(sensors, thread, zb::ZB_SENSOR_HC_ASensorManager_getInstance, {});
    CHECK(manager != 0);
    CHECK(call_sensor(sensors, thread, zb::ZB_SENSOR_HC_ASensorManager_getInstanceForPackage, {kData}) == manager);

    // The inventory is empty, and the list the caller passed is written as such.
    std::memset(runtime.memory().base() + kData, 0xAB, 4);
    CHECK(call_sensor(sensors, thread, zb::ZB_SENSOR_HC_ASensorManager_getSensorList, {manager, kData}) == 0);
    std::uint32_t list = 0;
    std::memcpy(&list, runtime.memory().base() + kData, sizeof list);
    CHECK(list == 0);
    CHECK(call_sensor(sensors, thread, zb::ZB_SENSOR_HC_ASensorManager_getDefaultSensor, {manager, 1}) == 0);
    CHECK(call_sensor(sensors, thread, zb::ZB_SENSOR_HC_ASensorManager_getDefaultSensorEx, {manager, 1, 0}) == 0);

    // An unwritable list address is not a reason to fail the call; the count still says zero.
    CHECK(call_sensor(sensors, thread, zb::ZB_SENSOR_HC_ASensorManager_getSensorList, {manager, 0xDEAD0000}) == 0);

    // A queue is real and distinct per creation, holds no events, and can be destroyed once.
    const std::uint32_t queue = call_sensor(sensors, thread, zb::ZB_SENSOR_HC_ASensorManager_createEventQueue,
                                            {manager, 0, 0, 0, 0});
    const std::uint32_t second = call_sensor(sensors, thread, zb::ZB_SENSOR_HC_ASensorManager_createEventQueue,
                                             {manager, 0, 0, 0, 0});
    CHECK(queue != 0 && second != 0 && queue != second);
    CHECK(call_sensor(sensors, thread, zb::ZB_SENSOR_HC_ASensorEventQueue_hasEvents, {queue}) == 0);
    CHECK(call_sensor(sensors, thread, zb::ZB_SENSOR_HC_ASensorEventQueue_getEvents, {queue, kData, 4}) == 0);
    CHECK(call_sensor(sensors, thread, zb::ZB_SENSOR_HC_ASensorManager_destroyEventQueue, {manager, queue}) == 0);
    const auto unavailable = static_cast<std::uint32_t>(-EINVAL);
    CHECK(call_sensor(sensors, thread, zb::ZB_SENSOR_HC_ASensorManager_destroyEventQueue, {manager, queue}) ==
          unavailable);
    CHECK(call_sensor(sensors, thread, zb::ZB_SENSOR_HC_ASensorEventQueue_hasEvents, {queue}) == unavailable);

    // Enabling a sensor fails rather than pretending: there is no sensor to enable.
    CHECK(call_sensor(sensors, thread, zb::ZB_SENSOR_HC_ASensorEventQueue_enableSensor, {second, 0}) == unavailable);
    CHECK(call_sensor(sensors, thread, zb::ZB_SENSOR_HC_ASensorEventQueue_disableSensor, {second, 0}) == unavailable);
    CHECK(call_sensor(sensors, thread, zb::ZB_SENSOR_HC_ASensorEventQueue_registerSensor, {second, 0, 0, 0}) ==
          unavailable);
    CHECK(call_sensor(sensors, thread, zb::ZB_SENSOR_HC_ASensorEventQueue_setEventRate, {second, 0, 1000}) ==
          unavailable);

    // The accessors answer for a sensor that is not there instead of following the guest pointer.
    CHECK(call_sensor(sensors, thread, zb::ZB_SENSOR_HC_ASensor_getName, {0xDEAD0000}) == 0);
    CHECK(call_sensor(sensors, thread, zb::ZB_SENSOR_HC_ASensor_getVendor, {0xDEAD0000}) == 0);
    CHECK(call_sensor(sensors, thread, zb::ZB_SENSOR_HC_ASensor_getType, {0xDEAD0000}) == 0);
    CHECK(call_sensor(sensors, thread, zb::ZB_SENSOR_HC_ASensor_getResolution, {0xDEAD0000}) == 0);
    CHECK(call_sensor(sensors, thread, zb::ZB_SENSOR_HC_ASensor_isWakeUpSensor, {0xDEAD0000}) == 0);
    CHECK(call_sensor(sensors, thread, zb::ZB_SENSOR_HC_ASensor_getHandle, {0xDEAD0000}) ==
          static_cast<std::uint32_t>(-1));

    // Every generated index answers: one unresolved import stops the guest library from loading.
    for (std::uint32_t index = zb::ZB_SENSOR_HC_FIRST; index <= zb::ZB_SENSOR_HC_LAST; ++index) {
        thread.regs()[0] = thread.regs()[1] = 0;
        CHECK(sensors.handle_host_call(index, thread));
    }

    std::printf("sensors_test ok\n");
    return 0;
}
