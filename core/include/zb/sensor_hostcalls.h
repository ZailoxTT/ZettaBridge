#pragma once

#include <cstdint>

namespace zb {

// Host-call indices for the ASensor* entry points HostSensors serves. Hand-written from the
// generated tools/gen_stubs.py order (core/src/gen/hostcalls.inc), which is the source of
// truth: the sensor range is appended last, after the GLES extension entry points, so nothing
// established moves. If the generated order changes, update these to match.
inline constexpr std::uint32_t ZB_SENSOR_HC_ASensorEventQueue_disableSensor = 344u;
inline constexpr std::uint32_t ZB_SENSOR_HC_ASensorEventQueue_enableSensor = 345u;
inline constexpr std::uint32_t ZB_SENSOR_HC_ASensorEventQueue_getEvents = 346u;
inline constexpr std::uint32_t ZB_SENSOR_HC_ASensorEventQueue_hasEvents = 347u;
inline constexpr std::uint32_t ZB_SENSOR_HC_ASensorEventQueue_registerSensor = 348u;
inline constexpr std::uint32_t ZB_SENSOR_HC_ASensorEventQueue_requestAdditionalInfoEvents = 349u;
inline constexpr std::uint32_t ZB_SENSOR_HC_ASensorEventQueue_setEventRate = 350u;
inline constexpr std::uint32_t ZB_SENSOR_HC_ASensorManager_configureDirectReport = 351u;
inline constexpr std::uint32_t ZB_SENSOR_HC_ASensorManager_createEventQueue = 352u;
inline constexpr std::uint32_t ZB_SENSOR_HC_ASensorManager_createHardwareBufferDirectChannel = 353u;
inline constexpr std::uint32_t ZB_SENSOR_HC_ASensorManager_createSharedMemoryDirectChannel = 354u;
inline constexpr std::uint32_t ZB_SENSOR_HC_ASensorManager_destroyDirectChannel = 355u;
inline constexpr std::uint32_t ZB_SENSOR_HC_ASensorManager_destroyEventQueue = 356u;
inline constexpr std::uint32_t ZB_SENSOR_HC_ASensorManager_getDefaultSensor = 357u;
inline constexpr std::uint32_t ZB_SENSOR_HC_ASensorManager_getDefaultSensorEx = 358u;
inline constexpr std::uint32_t ZB_SENSOR_HC_ASensorManager_getDynamicSensorList = 359u;
inline constexpr std::uint32_t ZB_SENSOR_HC_ASensorManager_getInstance = 360u;
inline constexpr std::uint32_t ZB_SENSOR_HC_ASensorManager_getInstanceForPackage = 361u;
inline constexpr std::uint32_t ZB_SENSOR_HC_ASensorManager_getSensorList = 362u;
inline constexpr std::uint32_t ZB_SENSOR_HC_ASensor_getFifoMaxEventCount = 363u;
inline constexpr std::uint32_t ZB_SENSOR_HC_ASensor_getFifoReservedEventCount = 364u;
inline constexpr std::uint32_t ZB_SENSOR_HC_ASensor_getHandle = 365u;
inline constexpr std::uint32_t ZB_SENSOR_HC_ASensor_getHighestDirectReportRateLevel = 366u;
inline constexpr std::uint32_t ZB_SENSOR_HC_ASensor_getMinDelay = 367u;
inline constexpr std::uint32_t ZB_SENSOR_HC_ASensor_getName = 368u;
inline constexpr std::uint32_t ZB_SENSOR_HC_ASensor_getReportingMode = 369u;
inline constexpr std::uint32_t ZB_SENSOR_HC_ASensor_getResolution = 370u;
inline constexpr std::uint32_t ZB_SENSOR_HC_ASensor_getStringType = 371u;
inline constexpr std::uint32_t ZB_SENSOR_HC_ASensor_getType = 372u;
inline constexpr std::uint32_t ZB_SENSOR_HC_ASensor_getVendor = 373u;
inline constexpr std::uint32_t ZB_SENSOR_HC_ASensor_isDirectChannelTypeSupported = 374u;
inline constexpr std::uint32_t ZB_SENSOR_HC_ASensor_isWakeUpSensor = 375u;

inline constexpr std::uint32_t ZB_SENSOR_HC_FIRST = 344u;
inline constexpr std::uint32_t ZB_SENSOR_HC_LAST = 375u;

}  // namespace zb
