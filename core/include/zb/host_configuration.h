#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>

#include "zb/configuration_backend.h"
#include "zb/guest_thread.h"
#include "zb/jni_handles.h"
#include "zb/library_runtime.h"

namespace zb {

// AConfiguration_*: the device configuration a NativeActivity guest reads (orientation, density,
// screen size, locale). A configuration is a host object; the guest holds a 32-bit handle, and a
// handle that is not live is rejected rather than followed.
class HostConfiguration {
public:
    // Turns the guest's AAssetManager handle into the host pointer HostAssets holds. Without one
    // (a build with no assets) AConfiguration_fromAssetManager leaves the configuration alone,
    // which is what the NDK does for a null manager.
    using AssetManagerResolver = std::function<void*(std::uint32_t handle)>;

    HostConfiguration(LibraryRuntime& runtime, ConfigurationBackend& backend,
                      AssetManagerResolver resolve_asset_manager = {})
        : runtime_(runtime), backend_(backend), resolve_asset_manager_(std::move(resolve_asset_manager)) {}
    HostConfiguration(const HostConfiguration&) = delete;
    HostConfiguration& operator=(const HostConfiguration&) = delete;

    // Serves ZB_CONFIG_HC_* indices; returns false for any other index.
    bool handle_host_call(std::uint32_t index, GuestThread& thread);

    // The host configuration behind a handle, for the glue; nullptr when it is not live.
    void* live_config(std::uint32_t handle) const;

private:
    bool reject(const char* function, std::uint32_t handle);

    LibraryRuntime& runtime_;
    ConfigurationBackend& backend_;
    AssetManagerResolver resolve_asset_manager_;
    GlobalHandles configs_{HandleKind::Global};
    mutable std::mutex mutex_;
    std::string first_rejection_;
};

}  // namespace zb
