#pragma once

#include <cstdint>
#include <mutex>
#include <unordered_map>

#include "zb/guest_thread.h"
#include "zb/jni_handles.h"
#include "zb/library_runtime.h"
#include "zb/native_window_backend.h"

namespace zb {

class HostJni;

// ANativeWindow_* host-call dispatcher (Phase 7a Task 5): the eight functions
// core/include/zb/window_hostcalls.h names. Chained into LibraryRuntime::set_host_call_handler
// alongside HostGl, HostAssets and HostJni, the same way GuestJniEngine wires them
// (core/src/jni/proxy_runtime.cpp). ANativeWindow* never crosses into the guest as a host
// pointer: it is a 32-bit handle into the table owned here. An unknown handle returns -1 from
// the query functions and never reaches the backend.
class HostNativeWindow {
public:
    HostNativeWindow(LibraryRuntime& runtime, NativeWindowBackend& backend, HostJni& host_jni)
        : runtime_(runtime), backend_(backend), host_jni_(host_jni) {}
    HostNativeWindow(const HostNativeWindow&) = delete;
    HostNativeWindow& operator=(const HostNativeWindow&) = delete;

    // Serves ZB_WINDOW_HC_* indices (160-167); returns false for any other index.
    bool handle_host_call(std::uint32_t index, GuestThread& thread);

    // A guest handle for a window the platform handed us directly (the one the framework gives a
    // NativeActivity), so the guest callback can receive it like any other window.
    std::uint32_t handle_for_window(const void* window);

    // The WindowResolver seam HostEgl::eglCreateWindowSurface uses to turn a guest window handle
    // into a host ANativeWindow*; nullptr for an unknown handle.
    const void* value_for(std::uint32_t handle) const;

private:
    const void* require_window(std::uint32_t handle) const;

    LibraryRuntime& runtime_;
    NativeWindowBackend& backend_;
    HostJni& host_jni_;
    GlobalHandles windows_{HandleKind::Global};
    // The host jobject Ref (android.view.Surface) each window handle was created from, so
    // ANativeWindow_toSurface can hand back a guest jobject for the same Java object without a
    // backend call. Keyed by window handle.
    mutable std::mutex surfaces_mutex_;
    std::unordered_map<std::uint32_t, std::uint64_t> surfaces_;
    // How many references the guest holds on each window handle: one from fromSurface, one more
    // per acquire. The handle dies with the last release, not the first: a guest that acquires a
    // window and releases one of its references still owns it, and EGL must keep accepting it.
    std::unordered_map<std::uint32_t, std::uint32_t> references_;
};

}  // namespace zb
