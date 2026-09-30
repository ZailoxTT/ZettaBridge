#pragma once

#include <cstdint>
#include <string>

namespace zb {

// The parts of a NativeActivity that only the Android side knows: what is inside the framework's
// own ANativeActivity, and how to put our host callbacks into it. Abstract so that the portable
// runtime stays free of <android/native_activity.h> and keeps its host tests.
class NativeActivityPlatform {
public:
    virtual ~NativeActivityPlatform() = default;

    struct Facts {
        std::string internal_data_path;
        std::string external_data_path;
        std::string obb_path;
        std::int32_t sdk_version = 0;
        std::uint64_t activity_object = 0;  // the Java activity, as a host JNI reference
        std::uint64_t asset_manager = 0;    // the host AAssetManager the framework owns
    };

    // Reads the framework's structure. False when the pointer is not one we can use.
    virtual bool read(std::uint64_t activity, Facts& facts) = 0;

    // Puts our host callbacks into the framework's table, so its lifecycle events reach the guest
    // activity. Called once, right after the guest's ANativeActivity_onCreate returned.
    virtual void attach(std::uint64_t activity, std::uint32_t guest_activity) = 0;
};

}  // namespace zb
