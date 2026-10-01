#include "zb/proxy_runtime.h"

#include <sys/stat.h>

#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <vector>

#include "zb/hang_watchdog.h"
#include "zb/host_jni.h"
#include "zb/library_protocol.h"
#include "zb/log.h"
#include "zb/runtime_report.h"

namespace zb {

namespace {

constexpr std::uint32_t kMaxTargetSdk = 10000;
constexpr const char* kRestartHint = "restart the :guest process to change plugins";

bool supported_art_version(std::int32_t version) {
    return version == 0x00010002 || version == 0x00010004 || version == 0x00010006;
}

std::vector<std::string_view> split(std::string_view path) {
    std::vector<std::string_view> parts;
    std::size_t start = 1;
    while (start <= path.size()) {
        std::size_t end = path.find('/', start);
        if (end == std::string_view::npos) end = path.size();
        parts.push_back(path.substr(start, end - start));
        start = end + 1;
    }
    return parts;
}

std::string join(const std::vector<std::string_view>& parts, std::size_t count) {
    std::string out;
    for (std::size_t i = 0; i < count; ++i) {
        out += '/';
        out += parts[i];
    }
    return out.empty() ? "/" : out;
}

bool is_regular_file(const std::string& path) {
    struct stat st;
    return ::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

bool is_directory(const std::string& path) {
    struct stat st;
    return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

std::string hex_version(std::int32_t version) {
    char text[11];
    std::snprintf(text, sizeof text, "0x%08x", static_cast<std::uint32_t>(version));
    return text;
}

std::string parent_of(const std::string& path) {
    const std::size_t slash = path.rfind('/');
    return slash == 0 || slash == std::string::npos ? "/" : path.substr(0, slash);
}

std::string base_name(const std::string& path) {
    const std::size_t slash = path.rfind('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

}  // namespace

bool canonical_path(const std::string& path, std::string& out, std::string& error) {
    char resolved[PATH_MAX];
    if (::realpath(path.c_str(), resolved) == nullptr) {
        error = "cannot resolve " + path + ": " + std::strerror(errno);
        return false;
    }
    out = resolved;
    return true;
}

bool parse_proxy_path(const std::string& canonical_proxy, ProxyLocation& out, std::string& error) {
    const auto fail = [&](const char* why) {
        error = "proxy path " + canonical_proxy + " is not <files>/plugins/<package>/proxy/lib<name>.so: " + why;
        return false;
    };
    if (canonical_proxy.empty() || canonical_proxy[0] != '/') return fail("not absolute");
    const std::vector<std::string_view> parts = split(canonical_proxy);
    for (const std::string_view part : parts) {
        if (part.empty() || part == "." || part == "..") return fail("not canonical");
    }
    // .../plugins/<package>/proxy/lib<name>.so
    if (parts.size() < 5) return fail("too short");
    const std::size_t n = parts.size();
    const std::string_view file = parts[n - 1];
    if (parts[n - 2] != "proxy") return fail("not in a proxy directory");
    if (parts[n - 4] != "plugins") return fail("not under a plugins directory");
    if (file.size() <= 6 || file.substr(0, 3) != "lib" || file.substr(file.size() - 3) != ".so") {
        return fail("file name is not lib<name>.so");
    }
    out.proxy = canonical_proxy;
    out.files_dir = join(parts, n - 4);
    out.plugin_root = join(parts, n - 2);
    out.package = std::string(parts[n - 3]);
    out.library = std::string(file);
    out.guest_library = out.plugin_root + "/lib/" + out.library;
    return true;
}

bool resolve_guest_runtime_layout(const std::string& files_dir, GuestRuntimeLayout& out, std::string& error) {
    const std::string runtime = files_dir + "/zb";
    GuestRuntimeLayout layout{runtime + "/sysroot", runtime + "/guest/zbhost", runtime + "/guest/lib"};
    for (const std::string& required : {layout.zbhost, layout.guest_lib_dir + "/libzbcompat.so",
                                        layout.guest_lib_dir + "/libzbjni.so",
                                        layout.sysroot + "/system/bin/linker"}) {
        if (!is_regular_file(required)) {
            error = "guest runtime file " + required + " is missing; the launcher runtime assets are not installed";
            return false;
        }
    }
    out = std::move(layout);
    return true;
}

LibraryRuntimeOptions guest_runtime_options(const GuestRuntimeLayout& layout, const std::string& plugin_root,
                                            std::uint32_t target_sdk) {
    LibraryRuntimeOptions options;
    options.zbhost = layout.zbhost;
    options.sysroot = layout.sysroot;
    options.target_sdk = target_sdk;
    options.preload = "libzbjni.so";
    options.guest_environment = {"LD_LIBRARY_PATH=" + layout.guest_lib_dir + ":" + plugin_root + "/lib"};
    return options;
}

ProxyRuntime::ProxyRuntime(ProxyLoadEngine& engine) : engine_(engine) {}

bool ProxyRuntime::activate_plugin(JniBackend::Env env, const std::string& plugin_root, std::uint32_t target_sdk,
                                   JniBackend::Ref class_loader, std::string& error) {
    if (target_sdk == 0 || target_sdk > kMaxTargetSdk) {
        error = "invalid plugin targetSdk " + std::to_string(target_sdk);
        return false;
    }
    if (class_loader == 0) {
        error = "the plugin class loader is null";
        return false;
    }
    std::string root;
    if (!canonical_path(plugin_root, root, error)) {
        error = "plugin root: " + error;
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (active_ && (root != plugin_root_ || target_sdk != target_sdk_)) {
        error = "this :guest process already runs plugin " + plugin_root_ + " (targetSdk " +
                std::to_string(target_sdk_) + "); cannot activate " + root + " (targetSdk " +
                std::to_string(target_sdk) + "): " + kRestartHint;
        return false;
    }
    LibraryRuntimeOptions options = options_;
    if (!active_) {
        const std::string plugins = parent_of(root);
        if (base_name(plugins) != "plugins") {
            error = "plugin root " + root + " is not <files>/plugins/<package>";
            return false;
        }
        if (!is_directory(root + "/lib")) {
            error = "plugin root " + root + " has no lib directory";
            return false;
        }
        GuestRuntimeLayout layout;
        if (!resolve_guest_runtime_layout(parent_of(plugins), layout, error)) return false;
        options = guest_runtime_options(layout, root, target_sdk);
    }
    std::string bind_error;
    if (!engine_.bind_class_loader(env, class_loader, bind_error)) {
        error = "cannot bind the plugin class loader: " + bind_error;
        return false;
    }
    if (!active_) {
        active_ = true;
        plugin_root_ = root;
        // Java hands native code the application's library directory, which for a plugin is the
        // arm64 proxy directory. The arm32 files it stands for live next to it, so a guest that
        // opens something beside "its own" library finds the real thing (IL2CPP does exactly
        // this and otherwise reports that it cannot load).
        engine_.add_path_alias(root + "/proxy/", root + "/lib/");
        target_sdk_ = target_sdk;
        options_ = std::move(options);
        log("guest JNI runtime: plugin %s activated (targetSdk %u)", root.c_str(), target_sdk);
        runtime_report().note_plugin(root, target_sdk);
    }
    return true;
}

ProxyLoadResult ProxyRuntime::fail_locked(const std::string& key, const std::string& library, std::string detail) {
    Entry& entry = entries_[key];
    entry.state = LoadState::Failed;
    entry.error = "ZettaBridge cannot load " + library + " (proxy " + key + "): " + detail;
    last_error_ = entry.error;
    log("%s", entry.error.c_str());
    runtime_report().note_proxy_failed(library, detail);
    cv_.notify_all();
    return {false, 0, entry.error};
}

ProxyLoadResult ProxyRuntime::on_proxy_loaded(JniBackend::Env env, const std::string& proxy_path) {
    std::string canonical, error;
    const bool resolved = canonical_path(proxy_path, canonical, error);
    const std::string key = resolved ? canonical : proxy_path;

    std::unique_lock<std::mutex> lock(mutex_);
    for (;;) {
        const auto it = entries_.find(key);
        if (it == entries_.end()) break;
        const Entry& entry = it->second;
        if (entry.state == LoadState::Loaded) return {true, entry.jni_version, {}};
        if (entry.state == LoadState::Failed) return {false, 0, entry.error};
        if (entry.owner == std::this_thread::get_id()) {
            return {false, 0, "ZettaBridge: recursive load of proxy " + key};
        }
        cv_.wait(lock);
    }
    if (!resolved) return fail_locked(key, base_name(proxy_path), error);
    entries_[key] = Entry{LoadState::Loading, std::this_thread::get_id(), 0, 0, {}};

    ProxyLocation location;
    if (!parse_proxy_path(canonical, location, error)) return fail_locked(key, base_name(key), error);
    if (!active_) {
        return fail_locked(key, location.library,
                           "no plugin is active in this process; ZBridge.activatePlugin must run before "
                           "plugin code loads libraries");
    }
    if (location.plugin_root != plugin_root_) {
        return fail_locked(key, location.library,
                           "the proxy belongs to plugin " + location.plugin_root + " but this :guest process runs " +
                               plugin_root_ + ": " + kRestartHint);
    }
    if (!is_regular_file(location.guest_library)) {
        return fail_locked(key, location.library,
                           "the arm32 library " + location.guest_library + " is missing; reimport the plugin");
    }

    cv_.wait(lock, [this] { return start_state_ != StartState::Starting; });
    if (start_state_ == StartState::NotStarted) {
        start_state_ = StartState::Starting;
        const LibraryRuntimeOptions options = options_;
        lock.unlock();
        std::string start_error;
        // Phase notes: between "the plugin is known" and "a library loaded" the report was blind,
        // and a guest process that never gets past zbhost looks identical to one that never ran.
        runtime_report().note_jni_detail("runtime-start", "starting " + options.zbhost, true);
        const bool started = engine_.start(options, start_error);
        runtime_report().note_jni_detail("runtime-start", started ? "ok" : "failed: " + start_error, true);
        lock.lock();
        start_state_ = started ? StartState::Started : StartState::Failed;
        start_error_ = started ? std::string() : std::move(start_error);
        if (started) log("guest JNI runtime started: %s %u %s", options.zbhost.c_str(), options.target_sdk,
                         options.preload.c_str());
        cv_.notify_all();
    }
    if (start_state_ == StartState::Failed) {
        return fail_locked(key, location.library, "the guest runtime failed to start: " + start_error_);
    }

    lock.unlock();
    runtime_report().note_jni_detail("loading", location.library, true);
    const JniLoadReport report = engine_.load(env, location.guest_library);
    lock.lock();
    if (!report.ok) return fail_locked(key, location.library, report.error);
    if (!supported_art_version(report.jni_version)) {
        return fail_locked(key, location.library,
                           "guest JNI_OnLoad returned unsupported JNI version " + hex_version(report.jni_version) +
                               " (ART accepts 1.2, 1.4 and 1.6)");
    }
    Entry& entry = entries_[key];
    entry.state = LoadState::Loaded;
    entry.jni_version = report.jni_version;
    entry.guest_handle = report.guest_handle;
    log("guest JNI library %s loaded: %zu natives bound, %zu classes skipped, JNI version %s",
        location.guest_library.c_str(), report.bound_methods, report.skipped_classes,
        hex_version(report.jni_version).c_str());
    runtime_report().note_proxy_loaded(location.library, report.jni_version);
    cv_.notify_all();
    return {true, report.jni_version, {}};
}

bool ProxyRuntime::on_native_activity_created(JniBackend::Env env, std::uint64_t activity,
                                              std::uint64_t saved_state, std::uint64_t saved_state_size,
                                              const std::string& proxy_path) {
    const std::string library = base_name(proxy_path);
    std::string error;
    std::uint32_t guest_library = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::string canonical;
        if (!canonical_path(proxy_path, canonical, error)) {
            canonical = proxy_path;
        }
        const auto entry = entries_.find(canonical);
        if (entry == entries_.end() || entry->second.state != LoadState::Loaded) {
            error = "the library of this activity was never loaded through its proxy";
        } else {
            guest_library = entry->second.guest_handle;
        }
    }
    if (guest_library != 0) {
        // Written before the call, not after: a guest ANativeActivity_onCreate that never returns
        // leaves no line at all otherwise, and "never called" and "called and hung" look the same.
        runtime_report().note_jni_detail("native-activity", library + ": calling the guest", true);
        if (engine_.create_native_activity(env, activity, reinterpret_cast<const void*>(
                                               static_cast<std::uintptr_t>(saved_state)),
                                           static_cast<std::size_t>(saved_state_size), guest_library, error)) {
            runtime_report().note_jni_detail("native-activity", library + ": running", true);
            log("guest NativeActivity of %s is running", library.c_str());
            return true;
        }
    }
    runtime_report().note_jni_detail("native-activity", library + ": " + error, true);
    log("ZBridge.onNativeActivityCreated(%s): %s", library.c_str(), error.c_str());
    return false;
}

std::optional<std::string> ProxyRuntime::load_error(const std::string& proxy_path) const {
    std::string canonical, error;
    const std::string key = canonical_path(proxy_path, canonical, error) ? canonical : proxy_path;
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = entries_.find(key);
    if (it == entries_.end()) it = entries_.find(proxy_path);
    if (it == entries_.end() || it->second.state != LoadState::Failed) return std::nullopt;
    return it->second.error;
}

std::optional<std::string> ProxyRuntime::last_load_error() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (last_error_.empty()) return std::nullopt;
    return last_error_;
}

GuestJniEngine::GuestJniEngine(JniBackend& backend, GlBackend* gl_backend, HostGl::EglContextProbe egl_context_probe,
                               AssetBackend* asset_backend, EglBackend* egl_backend, NativeWindowBackend* window_backend,
                               AndroidLooperBackend* looper_backend, InputBackend* input_backend,
                               ConfigurationBackend* configuration_backend)
    : backend_(backend),
      runtime_(new LibraryRuntime()),
      host_jni_(new HostJni(*runtime_, backend)),
      loader_(new JniLoader(*host_jni_, backend)) {
    // A looper callback arrives on a Java thread that currently runs no guest code, so entering
    // the guest goes through HostJni, which reuses that host thread's cached carrier.
    HostJni* jni = host_jni_;
    HostLooper::GuestInvoker looper_invoker = [jni](std::uint32_t function, const GuestCall& args) {
        return jni->call_on_host_thread(function, args);
    };
    host_looper_ = new HostLooper(*runtime_, looper_backend, std::move(looper_invoker));
    host_sensors_ = new HostSensors(*runtime_);
    {
        HostJni* jni_for_activity = host_jni_;
        LibraryRuntime* runtime = runtime_;
        host_native_activity_ = new HostNativeActivity(
            *runtime_,
            // A lifecycle callback is native code called from Java, so it goes through the same
            // transition as any bound native method: a local frame on this thread's carrier.
            //
            // The JNIEnv comes from the thread itself, not from HostJni's current transition:
            // these calls arrive straight from Java (ZBridge.onNativeActivityCreated) and from the
            // framework's own callbacks, where there is no transition in progress and
            // current_env() is nothing. Passing that to call_native crashed the process inside the
            // activity entry on the device.
            [jni_for_activity, java = &backend_](std::uint32_t function,
                                                const GuestCall& args) -> std::optional<std::uint32_t> {
                JniBackend::Env env = jni_for_activity->current_env();
                if (env == 0) env = java->attach_current_thread(false, "zb-native-activity", 0);
                if (env == 0) return std::nullopt;
                const auto result = jni_for_activity->call_native(
                    env, 'I', function,
                    [&args](std::uint32_t, const RefToHandle&) { return args; });
                if (!result) return std::nullopt;
                return result->guest.r0;
            },
            // The guest allocator, reached the same way as the callbacks. It must not be
            // call_on_current: this whole path arrives on the Java main thread, which runs no
            // guest code of its own, and the activity failed to be built at all for that reason
            // ("cannot allocate the guest activity" on the device).
            [jni_for_activity, runtime](std::size_t size) -> std::optional<std::uint32_t> {
                GuestCall call;
                call.regs = {static_cast<std::uint32_t>(size), 0, 0, 0};
                const auto result = jni_for_activity->call_on_host_thread(runtime->service_api().malloc_fn, call);
                if (!result || result->r0 == 0) return std::nullopt;
                return result->r0;
            },
            [jni_for_activity, runtime](std::uint32_t address) {
                GuestCall call;
                call.regs = {address, 0, 0, 0};
                jni_for_activity->call_on_host_thread(runtime->service_api().free_fn, call);
            });
    }
    if (configuration_backend != nullptr) {
        HostAssets* assets = host_assets_;
        host_configuration_ = new HostConfiguration(*runtime_, *configuration_backend,
                                                    [assets](std::uint32_t handle) -> void* {
                                                        if (assets == nullptr) return nullptr;
                                                        const std::uint64_t manager = assets->manager_for(handle);
                                                        return reinterpret_cast<void*>(static_cast<std::uintptr_t>(manager));
                                                    });
    }
    if (input_backend != nullptr) {
        HostLooper* looper = host_looper_;
        host_input_ = new HostInput(*runtime_, *input_backend,
                                    [looper](GuestThread& thread, std::uint32_t handle, std::uint64_t& real) {
                                        return looper->ensure_real_looper(thread, handle, real);
                                    },
                                    [looper](std::uint32_t handle) { return looper->owner_of(handle); });
        HostInput* input = host_input_;
        host_looper_->set_input_probe([input](std::uint64_t real) -> std::optional<HostLooper::InputReady> {
            const std::optional<HostInput::Ready> ready = input->ready_on(real);
            if (!ready) return std::nullopt;
            return HostLooper::InputReady{ready->ident, ready->data};
        });
    }
    host_compat_ = new HostPlatformCompat();
    if (gl_backend != nullptr) {
        host_gl_ = new HostGl(*runtime_, *gl_backend, HostGl::GuestAllocator{}, std::move(egl_context_probe));
    }
    if (asset_backend != nullptr) {
        host_assets_ = new HostAssets(*runtime_, *asset_backend, *host_jni_);
    }
    if (window_backend != nullptr) {
        host_windows_ = new HostNativeWindow(*runtime_, *window_backend, *host_jni_);
    }
    if (egl_backend != nullptr) {
        HostEgl::WindowResolver windows;
        HostNativeWindow* host_windows = host_windows_;
        if (host_windows != nullptr) {
            windows = [host_windows](std::uint32_t handle) { return host_windows->value_for(handle); };
        }
        host_egl_ = new HostEgl(*runtime_, *egl_backend, HostEgl::GuestAllocator{}, std::move(windows));
    }
    HostJni* host_jni = host_jni_;
    HostGl* host_gl = host_gl_;
    HostAssets* host_assets = host_assets_;
    HostNativeWindow* host_windows = host_windows_;
    HostEgl* host_egl = host_egl_;
    HostLooper* host_looper = host_looper_;
    HostSensors* host_sensors = host_sensors_;
    HostInput* host_input = host_input_;
    HostConfiguration* host_configuration = host_configuration_;
    HostPlatformCompat* host_compat = host_compat_;
    // Core ranges never overlap (GLES 0-141, assets 142-159, windows 160-167, EGL 168-211,
    // append-only platform compatibility 212-227, sensors 344-375, JNI 0xFB00+), so the chain
    // order is free;
    // GL stays first because it is by far the hotter path during rendering.
    runtime_->set_host_call_handler([host_jni, host_gl, host_assets, host_windows, host_egl,
                                     host_looper, host_sensors, host_input, host_configuration,
                                     host_compat](std::uint32_t index, GuestThread& thread) {
        if (host_gl != nullptr && host_gl->handle_host_call(index, thread)) return true;
        if (host_assets != nullptr && host_assets->handle_host_call(index, thread)) return true;
        if (host_windows != nullptr && host_windows->handle_host_call(index, thread)) return true;
        if (host_egl != nullptr && host_egl->handle_host_call(index, thread)) return true;
        if (host_looper->handle_host_call(index, thread)) return true;
        if (host_sensors->handle_host_call(index, thread)) return true;
        if (host_input != nullptr && host_input->handle_host_call(index, thread)) return true;
        if (host_configuration != nullptr && host_configuration->handle_host_call(index, thread)) return true;
        if (host_compat->handle_host_call(index, thread)) return true;
        return host_jni->handle_host_call(index, thread);
    });
}

GuestJniEngine::~GuestJniEngine() {
    log("GuestJniEngine destroyed; the guest JNI runtime is process-lifetime");
    std::abort();
}

bool GuestJniEngine::bind_class_loader(JniBackend::Env, JniBackend::Ref, std::string&) {
    return true;
}

bool GuestJniEngine::start(const LibraryRuntimeOptions& options, std::string& error) {
    if (!runtime_->start(options, error)) return false;
    if (!host_jni_->ready()) {
        error = "zbhost reported ready but libzbjni.so did not register the guest JNI API";
        return false;
    }
    // Process-lifetime, like the runtime itself: no shutdown path, started once.
    start_hang_watchdog();
    return true;
}

bool GuestJniEngine::create_native_activity(JniBackend::Env env, std::uint64_t activity, const void* saved_state,
                                            std::size_t saved_state_size, std::uint32_t guest_library,
                                            std::string& error) {
    if (host_native_activity_ == nullptr || native_activity_platform_ == nullptr) {
        error = "this build has no NativeActivity support";
        return false;
    }
    NativeActivityPlatform::Facts facts;
    if (!native_activity_platform_->read(activity, facts)) {
        error = "the framework activity could not be read";
        return false;
    }

    HostNativeActivity::Description description;
    description.guest_on_create = host_jni_->find_symbol_on_current(env, guest_library,
                                                                    "ANativeActivity_onCreate", error);
    if (description.guest_on_create == 0) {
        error = "the guest library exports no ANativeActivity_onCreate: " + error;
        return false;
    }
    description.guest_vm = host_jni_->guest_java_vm();
    // The activity object is held for the life of the activity: the guest keeps the jobject in
    // its own structure and calls Java through it long after this returns.
    description.activity_handle = host_jni_->new_global_handle(backend_.new_global_ref(env, facts.activity_object));
    description.asset_manager =
        host_assets_ != nullptr ? host_assets_->handle_for_manager(facts.asset_manager) : 0;
    description.sdk_version = facts.sdk_version;
    description.internal_data_path = facts.internal_data_path;
    description.external_data_path = facts.external_data_path;
    description.obb_path = facts.obb_path;

    const auto guest = host_native_activity_->create(description, saved_state, saved_state_size, error);
    if (!guest) return false;
    native_activity_platform_->attach(activity, *guest);
    return true;
}

void GuestJniEngine::add_path_alias(const std::string& guest_prefix, const std::string& host_prefix) {
    if (runtime_ != nullptr) runtime_->add_path_alias(guest_prefix, host_prefix);
}

JniLoadReport GuestJniEngine::load(JniBackend::Env env, const std::string& guest_library) {
    JniLoadReport report = loader_->load(env, guest_library, ZB_GUEST_RTLD_NOW);
    if (!report.ok) {
        const std::string pending = take_pending_exception(env);
        if (!pending.empty()) report.error += " (" + pending + ")";
    }
    return report;
}

std::string GuestJniEngine::take_pending_exception(JniBackend::Env env) {
    if (!backend_.exception_check(env)) return {};
    backend_.exception_clear(env);
    return "a pending Java exception was cleared";
}

}  // namespace zb
