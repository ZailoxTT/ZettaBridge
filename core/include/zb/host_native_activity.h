#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

#include "zb/guest_thread.h"
#include "zb/jni_backend.h"
#include "zb/library_runtime.h"
#include "zb/native_activity_abi.h"
#include "zb/native_call.h"

namespace zb {

// The NativeActivity bridge: one guest activity per host activity the framework creates.
//
// The framework owns a host ANativeActivity and calls the library's ANativeActivity_onCreate.
// This unit builds the 32-bit activity the guest expects, calls the guest's own
// ANativeActivity_onCreate with it, reads back the callbacks the guest installed, and puts its
// own host callbacks in the framework's table so every later lifecycle event is translated.
//
// Nothing of the host crosses into the guest: the guest activity holds the guest JavaVM and
// JNIEnv, a 32-bit JNI handle for the activity object, guest copies of the three paths, and the
// asset-manager handle. The guest's `instance` field is the guest's own and is never read here.
class HostNativeActivity {
public:
    // Calls a guest function as native code called from Java, which is what a lifecycle callback
    // is: the JNI bridge's call_native, bound by the owner. Returns the guest r0, or nullopt when
    // the call could not be made.
    using GuestCaller = std::function<std::optional<std::uint32_t>(std::uint32_t function, const GuestCall& args)>;
    // Allocates `size` bytes of guest memory (the guest allocator), or nullopt.
    using GuestAllocator = std::function<std::optional<std::uint32_t>(std::size_t size)>;
    // Frees guest memory the guest allocator gave out. Host memory is never passed here.
    using GuestDeallocator = std::function<void(std::uint32_t address)>;

    HostNativeActivity(LibraryRuntime& runtime, GuestCaller caller, GuestAllocator allocate,
                       GuestDeallocator deallocate)
        : runtime_(runtime), call_(std::move(caller)), allocate_(std::move(allocate)),
          deallocate_(std::move(deallocate)) {}
    HostNativeActivity(const HostNativeActivity&) = delete;
    HostNativeActivity& operator=(const HostNativeActivity&) = delete;

    // What the guest activity is built from. The caller resolves these; this unit does no JNI and
    // no loading of its own, which is what keeps it testable without a device.
    struct Description {
        std::uint32_t guest_on_create = 0;   // the guest's ANativeActivity_onCreate
        std::uint32_t guest_vm = 0;          // guest JavaVM*
        std::uint32_t guest_env = 0;         // guest JNIEnv*
        std::uint32_t activity_handle = 0;   // 32-bit JNI handle of the activity object
        std::uint32_t asset_manager = 0;     // AAssetManager handle, or 0
        std::int32_t sdk_version = 0;
        std::string internal_data_path;
        std::string external_data_path;
        std::string obb_path;
    };

    // Builds the guest activity and calls the guest ANativeActivity_onCreate. Returns the guest
    // address of the activity, or nullopt with `error` set. `saved_state` is host memory the
    // framework owns; its bytes are copied into guest memory for the call and freed afterwards.
    std::optional<std::uint32_t> create(const Description& description, const void* saved_state,
                                        std::size_t saved_state_size, std::string& error);

    // Filling the framework's own callback table with host functions is the Android glue's job
    // (core/android): those are real host pointers into the framework's structure, and this unit
    // stays free of them so it can be tested here. The glue calls deliver() from each one.

    // The guest callbacks of one guest activity, by slot; 0 means the guest wants no such event.
    std::uint32_t guest_callback(std::uint32_t guest_activity, GuestCallback slot) const;

    // Delivers one framework event to the guest. `argument` is the second argument the NDK
    // callback takes (a focus flag, a window or queue handle, 0 for the plain ones).
    void deliver(std::uint32_t guest_activity, GuestCallback slot, std::uint32_t argument);

    // onContentRectChanged: writes the rectangle into guest memory, then delivers it.
    void deliver_content_rect(std::uint32_t guest_activity, std::int32_t left, std::int32_t top,
                              std::int32_t right, std::int32_t bottom);

    // onSaveInstanceState: calls the guest, copies the block the guest allocated into host memory
    // allocated with malloc (which is what the framework frees) and frees the guest block through
    // the guest allocator. Returns nullptr with *out_size 0 when the guest saves nothing.
    void* save_instance_state(std::uint32_t guest_activity, std::size_t* out_size);

    // Drops the state of one guest activity. Its guest memory is freed; the guest has been told
    // onDestroy before this.
    void destroy(std::uint32_t guest_activity);

private:
    struct Activity {
        std::uint32_t callbacks_table = 0;
        std::uint32_t saved_state = 0;   // guest copy handed to onCreate, freed with the activity
        std::uint32_t internal_path = 0;
        std::uint32_t external_path = 0;
        std::uint32_t obb_path = 0;
    };

    std::optional<std::uint32_t> copy_string(const std::string& text);
    std::optional<std::uint32_t> copy_bytes(const void* data, std::size_t size);

    LibraryRuntime& runtime_;
    GuestCaller call_;
    GuestAllocator allocate_;
    GuestDeallocator deallocate_;
    mutable std::mutex mutex_;
    std::unordered_map<std::uint32_t, Activity> activities_;
};

}  // namespace zb
