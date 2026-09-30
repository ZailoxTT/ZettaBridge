#pragma once

#include <condition_variable>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "zb/android_looper_backend.h"
#include "zb/asset_backend.h"
#include "zb/egl_backend.h"
#include "zb/gl_backend.h"
#include "zb/host_assets.h"
#include "zb/host_egl.h"
#include "zb/host_gl.h"
#include "zb/host_looper.h"
#include "zb/host_configuration.h"
#include "zb/host_native_activity.h"
#include "zb/native_activity_platform.h"
#include "zb/host_input.h"
#include "zb/host_sensors.h"
#include "zb/host_native_window.h"
#include "zb/host_platform_compat.h"
#include "zb/jni_backend.h"
#include "zb/jni_loader.h"
#include "zb/library_runtime.h"
#include "zb/native_window_backend.h"

namespace zb {

class HostJni;

// Fixed on-disk layout of the launcher (plan 4d, decision 3), all under one files directory:
//   <files>/zb/sysroot/system/bin/linker        arm32 system files
//   <files>/zb/guest/zbhost                     library-mode service executable
//   <files>/zb/guest/lib/libzbcompat.so         zbhost's mandatory preload
//   <files>/zb/guest/lib/libzbjni.so            guest JNIEnv, preloaded by zbhost
//   <files>/plugins/<package>/lib/lib<name>.so    fixed arm32 copy of a plugin library
//   <files>/plugins/<package>/proxy/lib<name>.so  libzbproxy.so copy ART loads instead
struct ProxyLocation {
    std::string proxy;          // canonical proxy path
    std::string files_dir;      // <files>
    std::string plugin_root;    // <files>/plugins/<package>
    std::string package;
    std::string library;        // lib<name>.so
    std::string guest_library;  // <plugin_root>/lib/lib<name>.so
};

// realpath(3). Android's dladdr reports the linker's real path, so every path is compared in this form.
bool canonical_path(const std::string& path, std::string& out, std::string& error);

// Splits a canonical proxy path. Rejects relative, non-canonical and misplaced paths.
bool parse_proxy_path(const std::string& canonical_proxy, ProxyLocation& out, std::string& error);

struct GuestRuntimeLayout {
    std::string sysroot;
    std::string zbhost;
    std::string guest_lib_dir;
};

// Checks that the runtime files exist under <files>/zb.
bool resolve_guest_runtime_layout(const std::string& files_dir, GuestRuntimeLayout& out, std::string& error);

// zbhost <target_sdk> libzbjni.so with a guest-only environment: LD_LIBRARY_PATH is the runtime
// guest libraries, then the plugin's library directory. Nothing is taken from the host environment.
LibraryRuntimeOptions guest_runtime_options(const GuestRuntimeLayout& layout, const std::string& plugin_root,
                                            std::uint32_t target_sdk);

// The operations ProxyRuntime drives. GuestJniEngine is the real one.
class ProxyLoadEngine {
public:
    virtual ~ProxyLoadEngine() = default;
    // Retains the plugin class loader for class lookups; called before anything is loaded.
    virtual bool bind_class_loader(JniBackend::Env env, JniBackend::Ref loader, std::string& error) = 0;
    // Called at most once per engine.
    virtual bool start(const LibraryRuntimeOptions& options, std::string& error) = 0;
    // Loads, binds and runs JNI_OnLoad on the calling thread. A failed load leaves no Java
    // exception pending.
    virtual JniLoadReport load(JniBackend::Env env, const std::string& guest_library) = 0;
    // The framework created a NativeActivity whose library is `guest_library`. Builds the guest
    // activity, calls the guest ANativeActivity_onCreate and puts our callbacks into the
    // framework's table. Default: nothing, for an engine with no platform behind it.
    virtual bool create_native_activity(JniBackend::Env env, std::uint64_t activity, const void* saved_state,
                                        std::size_t saved_state_size, std::uint32_t guest_library,
                                        std::string& error) {
        (void)env;
        (void)activity;
        (void)saved_state;
        (void)saved_state_size;
        (void)guest_library;
        error = "this build has no NativeActivity support";
        return false;
    }

    // Redirects guest file operations under one directory to another. Default: nothing, for an
    // engine with no guest runtime behind it.
    virtual void add_path_alias(const std::string& guest_prefix, const std::string& host_prefix) {
        (void)guest_prefix;
        (void)host_prefix;
    }
};

struct ProxyLoadResult {
    bool ok = false;
    std::int32_t jni_version = 0;  // JNI_VERSION_1_2, _1_4 or _1_6 when ok
    std::string error;             // full, user-visible message when !ok
};

// Process-lifetime state machine behind ZBridge.onProxyLoaded.
//
//   activate_plugin  binds exactly one plugin (root, targetSdk, class loader) per process. The same
//                    plugin may activate again; any other plugin is rejected (plan 4d, decision 7).
//   on_proxy_loaded  canonicalizes the proxy path, starts the guest runtime once, loads the matching
//                    guest library, and memoizes the outcome by canonical proxy path. Concurrent
//                    loads of different proxies run in parallel; a proxy requested while it loads
//                    waits for that result.
//
// Outcomes are final. ART remembers a failed library by path and never runs JNI_OnLoad for it
// again, a failed start is never retried (guest threads cannot be torn down), and a successful
// load is never repeated. Recovering from any failure needs a new :guest process.
class ProxyRuntime {
public:
    explicit ProxyRuntime(ProxyLoadEngine& engine);
    ProxyRuntime(const ProxyRuntime&) = delete;
    ProxyRuntime& operator=(const ProxyRuntime&) = delete;

    bool activate_plugin(JniBackend::Env env, const std::string& plugin_root, std::uint32_t target_sdk,
                         JniBackend::Ref class_loader, std::string& error);
    ProxyLoadResult on_proxy_loaded(JniBackend::Env env, const std::string& proxy_path);

    // The stored failure of a proxy path (canonical or as passed), or nullopt.
    // The framework created a NativeActivity whose library is this proxy. Builds the guest
    // activity and calls the guest ANativeActivity_onCreate; false means the reason is in the
    // report and the framework's activity was left untouched.
    bool on_native_activity_created(JniBackend::Env env, std::uint64_t activity, std::uint64_t saved_state,
                                    std::uint64_t saved_state_size, const std::string& proxy_path);

    std::optional<std::string> load_error(const std::string& proxy_path) const;
    // The most recent failure of any proxy, or nullopt.
    std::optional<std::string> last_load_error() const;

private:
    enum class LoadState { Loading, Loaded, Failed };
    struct Entry {
        LoadState state = LoadState::Loading;
        std::thread::id owner;
        std::int32_t jni_version = 0;
        std::uint32_t guest_handle = 0;  // the guest library, for ANativeActivity_onCreate
        std::string error;
    };
    enum class StartState { NotStarted, Starting, Started, Failed };

    ProxyLoadResult fail_locked(const std::string& key, const std::string& library, std::string detail);

    ProxyLoadEngine& engine_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    bool active_ = false;
    std::string plugin_root_;
    std::uint32_t target_sdk_ = 0;
    LibraryRuntimeOptions options_;
    StartState start_state_ = StartState::NotStarted;
    std::string start_error_;
    std::map<std::string, Entry> entries_;  // by canonical proxy path, or the raw path if unresolvable
    std::string last_error_;
};

// The real engine: one LibraryRuntime, HostJni (chained into the runtime before start) and
// JniLoader over a JniBackend. Process-lifetime: its members are never destroyed, and the
// destructor aborts like the LibraryRuntime and HostJni it owns.
class GuestJniEngine : public ProxyLoadEngine {
public:
    // gl_backend is optional: nullptr (the host build) chains only HostJni. Android supplies the
    // real driver backend (and an EGL-current probe) so GLES host calls reach the driver too.
    // asset_backend, egl_backend and window_backend are likewise optional: nullptr leaves the
    // corresponding host-call range unimplemented. looper_backend, when given, lets guest loopers
    // prepared on a host thread attach to that thread's real Android looper; nullptr keeps every
    // thread on HostLooper's own poll set.
    explicit GuestJniEngine(JniBackend& backend, GlBackend* gl_backend = nullptr,
                            HostGl::EglContextProbe egl_context_probe = {},
                            AssetBackend* asset_backend = nullptr, EglBackend* egl_backend = nullptr,
                            NativeWindowBackend* window_backend = nullptr,
                            AndroidLooperBackend* looper_backend = nullptr,
                            InputBackend* input_backend = nullptr,
                            ConfigurationBackend* configuration_backend = nullptr);
    ~GuestJniEngine() override;
    GuestJniEngine(const GuestJniEngine&) = delete;
    GuestJniEngine& operator=(const GuestJniEngine&) = delete;

    // Accepts any loader: a plain JniBackend has no class loader to bind.
    bool bind_class_loader(JniBackend::Env env, JniBackend::Ref loader, std::string& error) override;
    bool start(const LibraryRuntimeOptions& options, std::string& error) override;
    JniLoadReport load(JniBackend::Env env, const std::string& guest_library) override;
    void add_path_alias(const std::string& guest_prefix, const std::string& host_prefix) override;
    bool create_native_activity(JniBackend::Env env, std::uint64_t activity, const void* saved_state,
                                std::size_t saved_state_size, std::uint32_t guest_library,
                                std::string& error) override;

    LibraryRuntime& runtime() { return *runtime_; }
    HostJni& host_jni() { return *host_jni_; }
    // nullptr unless a gl_backend was passed to the constructor.
    HostGl* host_gl() { return host_gl_; }
    // nullptr unless an asset_backend was passed to the constructor.
    HostAssets* host_assets() { return host_assets_; }
    // nullptr unless an egl_backend was passed to the constructor.
    HostEgl* host_egl() { return host_egl_; }
    // nullptr unless a window_backend was passed to the constructor.
    HostNativeWindow* host_native_window() { return host_windows_; }
    HostLooper* host_looper() { return host_looper_; }
    HostSensors* host_sensors() { return host_sensors_; }
    HostInput* host_input() { return host_input_; }
    HostConfiguration* host_configuration() { return host_configuration_; }
    HostNativeActivity* host_native_activity() { return host_native_activity_; }

    // The Android side is installed after construction: the glue needs the units this engine
    // builds, and those exist only once the engine does.
    void set_native_activity_platform(NativeActivityPlatform* platform) {
        native_activity_platform_ = platform;
    }
    HostPlatformCompat* host_platform_compat() { return host_compat_; }

protected:
    // Clears a Java exception left pending by a failed load and describes it for the error
    // message; empty when none was pending.
    virtual std::string take_pending_exception(JniBackend::Env env);

    JniBackend& backend_;

private:
    LibraryRuntime* runtime_;
    HostJni* host_jni_;
    HostGl* host_gl_ = nullptr;
    HostAssets* host_assets_ = nullptr;
    HostEgl* host_egl_ = nullptr;
    HostNativeWindow* host_windows_ = nullptr;
    HostLooper* host_looper_ = nullptr;
    HostSensors* host_sensors_ = nullptr;
    HostInput* host_input_ = nullptr;
    HostConfiguration* host_configuration_ = nullptr;
    HostNativeActivity* host_native_activity_ = nullptr;
    NativeActivityPlatform* native_activity_platform_ = nullptr;
    HostPlatformCompat* host_compat_ = nullptr;
    JniLoader* loader_;
};

}  // namespace zb
