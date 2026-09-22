#pragma once

#include <cstdint>

namespace zb {

// Host mirror of the NDK's <android/native_activity.h> types, and the 32-bit layout the guest
// sees. The NDK structures are mirrored rather than included so this unit compiles and is tested
// on a machine with no NDK, and so the guest layout is written down next to the host one instead
// of being implied by a compiler. Both are fixed public ABI; core/android asserts the host half
// against the real header when it builds for Android.

struct NdkNativeActivityCallbacks {
    void (*onStart)(void* activity);
    void (*onResume)(void* activity);
    void* (*onSaveInstanceState)(void* activity, std::size_t* out_size);
    void (*onPause)(void* activity);
    void (*onStop)(void* activity);
    void (*onDestroy)(void* activity);
    void (*onWindowFocusChanged)(void* activity, int has_focus);
    void (*onNativeWindowCreated)(void* activity, void* window);
    void (*onNativeWindowResized)(void* activity, void* window);
    void (*onNativeWindowRedrawNeeded)(void* activity, void* window);
    void (*onNativeWindowDestroyed)(void* activity, void* window);
    void (*onInputQueueCreated)(void* activity, void* queue);
    void (*onInputQueueDestroyed)(void* activity, void* queue);
    void (*onContentRectChanged)(void* activity, const void* rect);
    void (*onConfigurationChanged)(void* activity);
    void (*onLowMemory)(void* activity);
};

struct NdkNativeActivity {
    NdkNativeActivityCallbacks* callbacks;
    void* vm;
    void* env;
    void* clazz;
    const char* internalDataPath;
    const char* externalDataPath;
    std::int32_t sdkVersion;
    void* instance;
    void* assetManager;
    const char* obbPath;
};

// The guest structure: the same fields, every one of them a 32-bit word.
enum GuestActivityField : std::uint32_t {
    kGuestActivityCallbacks = 0,
    kGuestActivityVm = 4,
    kGuestActivityEnv = 8,
    kGuestActivityClazz = 12,
    kGuestActivityInternalDataPath = 16,
    kGuestActivityExternalDataPath = 20,
    kGuestActivitySdkVersion = 24,
    kGuestActivityInstance = 28,
    kGuestActivityAssetManager = 32,
    kGuestActivityObbPath = 36,
};
inline constexpr std::uint32_t kGuestActivitySize = 40;

// The guest callback table: 16 guest function pointers in NDK order. The guest fills the ones it
// wants during its ANativeActivity_onCreate and leaves the rest zero.
enum class GuestCallback : std::uint32_t {
    OnStart = 0,
    OnResume,
    OnSaveInstanceState,
    OnPause,
    OnStop,
    OnDestroy,
    OnWindowFocusChanged,
    OnNativeWindowCreated,
    OnNativeWindowResized,
    OnNativeWindowRedrawNeeded,
    OnNativeWindowDestroyed,
    OnInputQueueCreated,
    OnInputQueueDestroyed,
    OnContentRectChanged,
    OnConfigurationChanged,
    OnLowMemory,
};
inline constexpr std::uint32_t kGuestCallbackCount = 16;
inline constexpr std::uint32_t kGuestCallbackTableSize = kGuestCallbackCount * 4;

// The name every callback is reported under, indexed by GuestCallback.
inline constexpr const char* kGuestCallbackNames[kGuestCallbackCount] = {
    "onStart",
    "onResume",
    "onSaveInstanceState",
    "onPause",
    "onStop",
    "onDestroy",
    "onWindowFocusChanged",
    "onNativeWindowCreated",
    "onNativeWindowResized",
    "onNativeWindowRedrawNeeded",
    "onNativeWindowDestroyed",
    "onInputQueueCreated",
    "onInputQueueDestroyed",
    "onContentRectChanged",
    "onConfigurationChanged",
    "onLowMemory",
};

// ARect as the guest sees it: four 32-bit values, left, top, right, bottom.
inline constexpr std::uint32_t kGuestRectSize = 16;

}  // namespace zb
