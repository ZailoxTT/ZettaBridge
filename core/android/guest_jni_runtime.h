#pragma once

#include <jni.h>

#include <cstdlib>
#include <string>

#include "asset_driver_backend.h"
#include "configuration_driver_backend.h"
#include "input_driver_backend.h"
#include "native_activity_glue.h"
#include "egl_driver_backend.h"
#include "gl_driver_backend.h"
#include "jni_env_backend.h"
#include "looper_driver_backend.h"
#include "native_window_driver_backend.h"
#include "zb/proxy_runtime.h"

namespace zb {

// The one guest JNI runtime of an app process (Android build only):
//   JniEnvBackend -> GuestJniEngine (LibraryRuntime + HostJni + JniLoader) -> ProxyRuntime.
// Created on first use and intentionally never destroyed: guest threads, bound native thunks and
// the retained plugin class loader live as long as the process.
class GuestJniRuntime {
public:
    static GuestJniRuntime& get(JNIEnv* env);
    // The runtime if get() already created it, else nullptr (error queries never create it).
    static GuestJniRuntime* peek();

    ProxyRuntime& proxies() { return proxies_; }

private:
    class Engine final : public GuestJniEngine {
    public:
        Engine(JniEnvBackend& backend, GlBackend& gl_backend, AssetBackend& asset_backend,
               EglBackend& egl_backend, NativeWindowBackend& window_backend,
               AndroidLooperBackend& looper_backend, InputBackend& input_backend,
               ConfigurationBackend& configuration_backend)
            : GuestJniEngine(backend, &gl_backend, gl_egl_context, &asset_backend,
                             &egl_backend, &window_backend, &looper_backend, &input_backend,
                             &configuration_backend),
              jni_backend_(backend) {
            // Opt-in only: the GL instrumentation reads back whole framebuffers and diffs pixels
            // per draw, which is far too expensive for a release run. Gated exactly like
            // ZB_PRECISE_FAULTS (see Process): ZB_GL_DIAGNOSTICS=1 turns it on.
            const char* diagnostics = std::getenv("ZB_GL_DIAGNOSTICS");
            if (diagnostics != nullptr && diagnostics[0] == '1') enable_gl_diagnostics();
        }
        bool bind_class_loader(JniBackend::Env env, JniBackend::Ref loader, std::string& error) override;

    protected:
        std::string take_pending_exception(JniBackend::Env env) override;

    private:
        JniEnvBackend& jni_backend_;
    };

    explicit GuestJniRuntime(JavaVM* vm);

    JniEnvBackend backend_;
    GlDriverBackend gl_backend_;
    AndroidAssetBackend asset_backend_;
    EglDriverBackend egl_backend_;
    AndroidNativeWindowBackend window_backend_;
    AndroidLooperDriverBackend looper_backend_;
    InputDriverBackend input_backend_;
    ConfigurationDriverBackend configuration_backend_;
    Engine engine_;
    // Built after the engine, because it hands the engine's own units to the framework's
    // callbacks; installed into the engine right after construction.
    NativeActivityGlue native_activity_glue_;
    ProxyRuntime proxies_;
};

}  // namespace zb
