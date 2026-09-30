#include "zb/host_native_window.h"

#include <cstdint>

#include "zb/host_jni.h"
#include "zb/window_hostcalls.h"

namespace zb {

namespace {

inline void* as_pointer(std::uint64_t value) {
    return reinterpret_cast<void*>(static_cast<std::uintptr_t>(value));
}

inline std::uint64_t from_pointer(const void* value) {
    return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(value));
}

}  // namespace

const void* HostNativeWindow::value_for(std::uint32_t handle) const {
    const std::optional<std::uint64_t> value = windows_.get(handle);
    if (!value || *value == 0) return nullptr;
    return as_pointer(*value);
}

const void* HostNativeWindow::require_window(std::uint32_t handle) const {
    return value_for(handle);
}

std::uint32_t HostNativeWindow::handle_for_window(const void* window) {
    if (window == nullptr) return 0;
    const std::uint32_t handle = windows_.add(from_pointer(window));
    std::lock_guard<std::mutex> lock(surfaces_mutex_);
    references_[handle] = 1;
    return handle;
}

bool HostNativeWindow::handle_host_call(std::uint32_t index, GuestThread& thread) {
    auto& regs = thread.regs();
    switch (index) {
    case ZB_WINDOW_HC_ANativeWindow_fromSurface: {
        // regs[0] is the guest JNIEnv*: ignored, we use the real host JNIEnv of this thread.
        const JniBackend::Ref surface = host_jni_.resolve_ref(regs[1], "ANativeWindow_fromSurface");
        void* window = nullptr;
        if (surface != 0) {
            void* env = as_pointer(static_cast<std::uint64_t>(host_jni_.current_env()));
            window = backend_.from_surface(env, as_pointer(surface));
        }
        std::uint32_t handle = 0;
        if (window != nullptr) {
            handle = windows_.add(from_pointer(window));
            std::lock_guard<std::mutex> lock(surfaces_mutex_);
            surfaces_[handle] = surface;
            references_[handle] = 1;
        }
        regs[0] = handle;
        return true;
    }
    case ZB_WINDOW_HC_ANativeWindow_acquire: {
        const void* window = require_window(regs[0]);
        if (window != nullptr) {
            backend_.acquire(const_cast<void*>(window));
            std::lock_guard<std::mutex> lock(surfaces_mutex_);
            ++references_[regs[0]];
        }
        regs[0] = 0;
        return true;
    }
    case ZB_WINDOW_HC_ANativeWindow_release: {
        bool last = false;
        {
            std::lock_guard<std::mutex> lock(surfaces_mutex_);
            const auto reference = references_.find(regs[0]);
            if (reference == references_.end()) {
                regs[0] = 0;  // not a live window handle
                return true;
            }
            last = --reference->second == 0;
            if (last) {
                references_.erase(reference);
                surfaces_.erase(regs[0]);
            }
        }
        // The backend is told about every release, as the guest issued it; only the handle
        // itself survives until the last one.
        const std::optional<std::uint64_t> value = last ? windows_.remove(regs[0]) : windows_.get(regs[0]);
        if (value && *value != 0) backend_.release(as_pointer(*value));
        regs[0] = 0;
        return true;
    }
    case ZB_WINDOW_HC_ANativeWindow_getWidth: {
        const void* window = require_window(regs[0]);
        const std::int32_t result =
            window != nullptr ? backend_.query(const_cast<void*>(window), NativeWindowBackend::Query::Width) : -1;
        regs[0] = static_cast<std::uint32_t>(result);
        return true;
    }
    case ZB_WINDOW_HC_ANativeWindow_getHeight: {
        const void* window = require_window(regs[0]);
        const std::int32_t result =
            window != nullptr ? backend_.query(const_cast<void*>(window), NativeWindowBackend::Query::Height) : -1;
        regs[0] = static_cast<std::uint32_t>(result);
        return true;
    }
    case ZB_WINDOW_HC_ANativeWindow_getFormat: {
        const void* window = require_window(regs[0]);
        const std::int32_t result =
            window != nullptr ? backend_.query(const_cast<void*>(window), NativeWindowBackend::Query::Format) : -1;
        regs[0] = static_cast<std::uint32_t>(result);
        return true;
    }
    case ZB_WINDOW_HC_ANativeWindow_setBuffersGeometry: {
        const void* window = require_window(regs[0]);
        std::int32_t result = -1;
        if (window != nullptr) {
            result = backend_.set_buffers_geometry(const_cast<void*>(window), static_cast<std::int32_t>(regs[1]),
                                                   static_cast<std::int32_t>(regs[2]),
                                                   static_cast<std::int32_t>(regs[3]));
        }
        regs[0] = static_cast<std::uint32_t>(result);
        return true;
    }
    case ZB_WINDOW_HC_ANativeWindow_toSurface: {
        // regs[0] is the guest JNIEnv*: ignored. regs[1] is the window handle.
        std::uint64_t surface = 0;
        {
            std::lock_guard<std::mutex> lock(surfaces_mutex_);
            auto it = surfaces_.find(regs[1]);
            if (it != surfaces_.end()) surface = it->second;
        }
        regs[0] = host_jni_.new_local_handle(surface);
        return true;
    }
    default:
        return false;
    }
}

}  // namespace zb
