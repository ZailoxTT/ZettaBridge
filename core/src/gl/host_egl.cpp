#include "zb/host_egl.h"

#include <sys/syscall.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "zb/egl_hostcalls.h"
#include "zb/guest_memory.h"
#include "zb/library_protocol.h"
#include "zb/log.h"
#include "zb/runtime_report.h"

namespace zb {

namespace {

// EGL errors are per-thread and sticky until eglGetError reads them. A call rejected here never
// reaches the driver, so its error is recorded on this side and shadows the driver's.
thread_local EGLint t_pending_error = kEglSuccess;

// The object class of handle argument `position` of `index`. Every EGL function that takes a
// display takes it first; the rest are listed by name. Manual cases pass their class explicitly
// through Call::handle_of, so only the generated cases need to be covered here.
EglObject argument_kind(std::uint32_t index, unsigned position) {
    if (position == 0) return EglObject::Display;
    if (index == ZB_EGL_HC_eglDestroyContext) return EglObject::Context;
    if (index == ZB_EGL_HC_eglMakeCurrent && position == 3) return EglObject::Context;
    return EglObject::Surface;
}

EglObject result_kind(std::uint32_t index) {
    if (index == ZB_EGL_HC_eglGetDisplay || index == ZB_EGL_HC_eglGetPlatformDisplay) {
        return EglObject::Display;
    }
    return EglObject::Surface;
}

const char* kind_name(EglObject kind) {
    switch (kind) {
    case EglObject::Display: return "display";
    case EglObject::Config: return "config";
    case EglObject::Context: return "context";
    case EglObject::Surface: return "surface";
    }
    return "object";
}

}  // namespace

EGLint egl_object_error(EglObject kind) {
    switch (kind) {
    case EglObject::Display: return kEglBadDisplay;
    case EglObject::Config: return kEglBadParameter;  // EGL_BAD_CONFIG
    case EglObject::Context: return kEglBadContext;
    case EglObject::Surface: return kEglBadSurface;
    }
    return kEglBadParameter;
}

HostEgl::Call::Call(HostEgl& host, GuestThread& thread, std::uint32_t index)
    : host_(host), thread_(thread), index_(index),
      regs_{thread.regs()[0], thread.regs()[1], thread.regs()[2], thread.regs()[3]} {
    // Capture arguments before installing the default zero result.
    thread_.regs()[0] = 0;
    thread_.regs()[1] = 0;
}

std::uint32_t HostEgl::Call::arg(unsigned position) {
    if (position < 4) return regs_[position];
    const std::uint64_t address =
        static_cast<std::uint64_t>(thread_.regs()[13]) + 4u * (position - 4u);
    if (address > UINT32_MAX) {
        fail(kEglBadParameter, "argument address overflowed the guest stack");
        return 0;
    }
    const std::uint8_t* source =
        host_.runtime().memory().host_ptr(static_cast<std::uint32_t>(address), 4, kPageRead);
    if (source == nullptr) {
        fail(kEglBadParameter, "argument is not on a readable guest stack");
        return 0;
    }
    std::uint32_t value;
    std::memcpy(&value, source, sizeof value);
    return value;
}

const void* HostEgl::Call::handle_of(unsigned position, EglObject kind) {
    const std::uint32_t handle = arg(position);
    if (!valid_) return nullptr;
    if (handle == 0) return nullptr;  // EGL_NO_DISPLAY / EGL_NO_CONTEXT / EGL_NO_SURFACE
    const std::optional<const void*> value = host_.lookup(handle, kind);
    if (!value) {
        char reason[64];
        std::snprintf(reason, sizeof reason, "unknown %s handle", kind_name(kind));
        fail(egl_object_error(kind), reason);
        return nullptr;
    }
    return *value;
}

const void* HostEgl::Call::resolve_handle(unsigned position) {
    return handle_of(position, argument_kind(index_, position));
}

void HostEgl::Call::set_handle(const void* value) { set_handle(value, result_kind(index_)); }

void HostEgl::Call::set_handle(const void* value, EglObject kind) {
    thread_.regs()[0] = host_.handle_for(value, kind);
}

void HostEgl::Call::fail(EGLint error, const char* reason) {
    if (!valid_) return;
    valid_ = false;
    host_.reject(*this, error, reason);
}

void HostEgl::reject(Call& call, EGLint error, const char* reason) {
    const char* name = "?";
    if (call.index() >= kEglHostCallFirst && call.index() <= kEglHostCallLast) {
        name = kEglHostCalls[call.index() - kEglHostCallFirst].name;
    }
    log("EGL %s rejected: %s", name, reason);
    t_pending_error = error;
    runtime_report().note_egl_error(name, static_cast<std::uint32_t>(error));
}

std::uint32_t HostEgl::handle_for(const void* value, EglObject kind) {
    if (value == nullptr) return 0;
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = by_value_.find(value);
    if (found != by_value_.end()) return found->second;
    const std::uint32_t handle = objects_.add(reinterpret_cast<std::uint64_t>(value));
    if (handle == 0) return 0;
    by_value_.emplace(value, handle);
    kinds_.emplace(handle, kind);
    return handle;
}

const void* HostEgl::value_for(std::uint32_t handle) const {
    if (handle == 0) return nullptr;
    std::lock_guard<std::mutex> lock(mutex_);
    const std::optional<std::uint64_t> value = objects_.get(handle);
    if (!value) return nullptr;
    return reinterpret_cast<const void*>(static_cast<std::uintptr_t>(*value));
}

std::optional<const void*> HostEgl::lookup(std::uint32_t handle, EglObject kind) const {
    if (handle == 0) return static_cast<const void*>(nullptr);
    std::lock_guard<std::mutex> lock(mutex_);
    const std::optional<std::uint64_t> value = objects_.get(handle);
    if (!value || *value == 0) return std::nullopt;
    const auto known = kinds_.find(handle);
    if (known == kinds_.end() || known->second != kind) return std::nullopt;
    return reinterpret_cast<const void*>(static_cast<std::uintptr_t>(*value));
}

std::uint32_t HostEgl::stub_address(const std::string& name) {
    if (stubs_) return stubs_(name);
    // Any guest library already loaded, which covers gl* in libGLESv2.so as well as egl*: Skia and
    // Impeller resolve every GL entry point through eglGetProcAddress, so an EGL-only lookup left
    // them without a single GL function.
    std::string error;
    const std::uint32_t global = runtime_.find_symbol(ZB_GUEST_RTLD_DEFAULT, name, error);
    if (global != 0) return global;
    for (const char* library : {"libEGL.so", "libGLESv2.so"}) {
        std::uint32_t& handle = std::strcmp(library, "libEGL.so") == 0 ? libegl_ : libgles_;
        if (handle == 0) {
            std::string load_error;
            handle = runtime_.load_library(library, ZB_GUEST_RTLD_NOW, load_error);
            if (handle == 0) {
                log("eglGetProcAddress: guest %s is not loadable: %s", library, load_error.c_str());
                continue;
            }
        }
        const std::uint32_t address = runtime_.find_symbol(handle, name, error);
        if (address != 0) return address;
    }
    return 0;
}

std::optional<std::uint32_t> HostEgl::allocate_guest(std::size_t size) {
    if (size == 0 || size > UINT32_MAX) return std::nullopt;
    if (allocator_) return allocator_(size);
    GuestCall args;
    args.regs = {static_cast<std::uint32_t>(size), 0, 0, 0};
    const auto result = runtime_.call_on_current(runtime_.service_api().malloc_fn, args);
    if (!result || result->r0 == 0) return std::nullopt;
    return result->r0;
}

#include "gen/egl_dispatch.inc"

namespace {

// The last EGL calls with their results. A guest that dies inside its own code usually died
// because an EGL call answered zero, and there is no logcat on the device.
constexpr std::size_t kRecentEglCalls = 24;
struct RecentEgl {
    std::atomic<std::uint32_t> index{0};
    std::atomic<std::uint32_t> result{0};
};
RecentEgl g_recent_egl[kRecentEglCalls];
std::atomic<std::uint64_t> g_recent_egl_next{0};

}  // namespace

std::string egl_recent_calls() {
    const std::uint64_t next = g_recent_egl_next.load(std::memory_order_relaxed);
    if (next == 0) return "(none)";
    const std::uint64_t first = next > kRecentEglCalls ? next - kRecentEglCalls : 0;
    std::string out;
    for (std::uint64_t i = first; i < next; ++i) {
        const std::uint32_t index = g_recent_egl[i % kRecentEglCalls].index.load(std::memory_order_relaxed);
        const std::uint32_t result = g_recent_egl[i % kRecentEglCalls].result.load(std::memory_order_relaxed);
        const std::uint32_t row = index - kEglHostCallFirst;
        const char* name = row < kEglHostCalls.size() ? kEglHostCalls[row].name : "?";
        char text[96];
        std::snprintf(text, sizeof text, "%s%s=0x%x", out.empty() ? "" : " ", name, result);
        out += text;
    }
    return out;
}

bool HostEgl::handle_host_call(std::uint32_t index, GuestThread& thread) {
    if (index < kEglHostCallFirst || index > kEglHostCallLast) return false;
    // A rejection never reaches the driver, so its error is served from here and takes
    // precedence over the driver's own sticky error.
    if (index == ZB_EGL_HC_eglGetError && t_pending_error != kEglSuccess) {
        thread.regs()[0] = static_cast<std::uint32_t>(t_pending_error);
        thread.regs()[1] = 0;
        t_pending_error = kEglSuccess;
        return true;
    }
    if (index == ZB_EGL_HC_eglMakeCurrent) {
        runtime_report().note_egl_current(static_cast<std::uint64_t>(::syscall(SYS_gettid)));
    }
    Call call(*this, thread, index);
    if (dispatch(call)) {
        const std::uint64_t slot = g_recent_egl_next.fetch_add(1, std::memory_order_relaxed) % kRecentEglCalls;
        g_recent_egl[slot].index.store(index, std::memory_order_relaxed);
        g_recent_egl[slot].result.store(thread.regs()[0], std::memory_order_relaxed);
        if (index == ZB_EGL_HC_eglCreateWindowSurface || index == ZB_EGL_HC_eglDestroySurface ||
            index == ZB_EGL_HC_eglMakeCurrent) {
            char detail[64];
            std::snprintf(detail, sizeof detail, "tid=%ld result=0x%08x",
                          ::syscall(SYS_gettid), thread.regs()[0]);
            runtime_report().note_egl_object(kEglHostCalls[index - kEglHostCallFirst].name, detail);
        }
        return true;
    }
    log("EGL host call index %u has no generated handler", index);
    return true;
}

}  // namespace zb
