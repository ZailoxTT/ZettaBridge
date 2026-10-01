#include <sys/syscall.h>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "gl/gl_diagnostics.h"
#include "zb/egl_hostcalls.h"
#include "zb/host_egl.h"
#include "zb/log.h"
#include "zb/runtime_report.h"

namespace zb {

namespace {

constexpr std::uint64_t kMaxAttribs = 512;      // 255 pairs plus EGL_NONE
constexpr std::uint64_t kMaxGuestString = 4096;
constexpr std::size_t kMaxDriverString = 64u << 20;

// Local to the runtime-report annotations below; not part of the generated protocol.
constexpr EGLint kEglConfigIdAttrib = 0x3028;
constexpr EGLint kEglContextClientVersionAttrib = 0x3098;
constexpr EGLint kEglWidthAttrib = 0x3057;
constexpr EGLint kEglHeightAttrib = 0x3056;

void* as_pointer(const void* value) { return const_cast<void*>(value); }

// Reads a guest EGL attribute list: 32-bit words up to and including EGL_NONE. A null list
// stays empty and is passed to the driver as nullptr. T is EGLint for the 1.4 entry points and
// EGLAttrib for the 1.5 ones: the guest's EGLAttrib is 32 bits, the host's is 64, so the list is
// always copied and widened rather than aliased.
template <typename T>
bool read_attribs(HostEgl& host, HostEgl::Call& call, unsigned position, std::vector<T>& out) {
    const std::uint32_t address = call.arg(position);
    if (!call.valid()) return false;
    if (address == 0) return true;
    for (std::uint64_t i = 0; i < kMaxAttribs; ++i) {
        const std::uint64_t at = static_cast<std::uint64_t>(address) + 4u * i;
        if (at + 4 > kGuestSpaceSize) break;
        const std::uint8_t* word = host.runtime().memory().host_ptr(
            static_cast<std::uint32_t>(at), 4, kPageRead);
        if (word == nullptr) {
            call.fail(kEglBadParameter, "attribute list is outside readable guest memory");
            return false;
        }
        std::int32_t value;
        std::memcpy(&value, word, sizeof value);
        out.push_back(static_cast<T>(value));
        if (value == kEglNone) return true;
    }
    call.fail(kEglBadParameter, "attribute list is unterminated");
    return false;
}

bool read_guest_string(HostEgl& host, HostEgl::Call& call, unsigned position, std::string& out) {
    const std::uint32_t address = call.arg(position);
    if (!call.valid()) return false;
    if (address == 0) {
        call.fail(kEglBadParameter, "string pointer is null");
        return false;
    }
    std::uint64_t cursor = address;
    while (cursor < kGuestSpaceSize && out.size() <= kMaxGuestString) {
        const std::uint64_t chunk = kPageSize - (cursor & kPageMask);
        const std::uint8_t* bytes = host.runtime().memory().host_ptr(
            static_cast<std::uint32_t>(cursor), chunk, kPageRead);
        if (bytes == nullptr) break;
        const auto* end = static_cast<const std::uint8_t*>(std::memchr(bytes, 0, chunk));
        if (end != nullptr) {
            out.append(reinterpret_cast<const char*>(bytes),
                       static_cast<std::size_t>(end - bytes));
            return true;
        }
        out.append(reinterpret_cast<const char*>(bytes), static_cast<std::size_t>(chunk));
        cursor += chunk;
    }
    call.fail(kEglBadParameter, "string is unreadable or too long");
    return false;
}

// eglGetConfigAttrib / eglQuerySurface / eglQueryContext all shape the same: one object handle
// and one EGLint out-parameter written straight into guest memory. The object class picks the
// backend entry point.
bool query_attribute(HostEgl& host, HostEgl::Call& call, EglObject kind) {
    EGLDisplay dpy = as_pointer(call.handle_of(0, EglObject::Display));
    if (!call.valid()) return true;
    void* object = as_pointer(call.handle_of(1, kind));
    if (!call.valid()) return true;
    const EGLint attribute = call.scalar<EGLint>(2);
    EGLint* value = call.pointer<EGLint>(3, 1u, kPageRead | kPageWrite);
    if (!call.valid()) return true;
    if (value == nullptr) {
        call.fail(kEglBadParameter, "attribute output pointer is null");
        return true;
    }
    EGLBoolean result = 0;
    switch (kind) {
    case EglObject::Config:
        result = host.backend().eglGetConfigAttrib(dpy, object, attribute, value);
        break;
    case EglObject::Surface:
        result = host.backend().eglQuerySurface(dpy, object, attribute, value);
        // Records the surface size the first time the guest asks for it (Task 7): free
        // visibility into what got created, at the cost of no extra backend calls.
        if (result != 0 && (attribute == kEglWidthAttrib || attribute == kEglHeightAttrib)) {
            char detail[32];
            std::snprintf(detail, sizeof detail, "%s=%d",
                          attribute == kEglWidthAttrib ? "width" : "height", *value);
            runtime_report().note_egl_object(
                attribute == kEglWidthAttrib ? "surface-width" : "surface-height", detail);
        }
        break;
    case EglObject::Context:
        result = host.backend().eglQueryContext(dpy, object, attribute, value);
        break;
    case EglObject::Display:
        break;
    }
    call.set_result(result);
    return true;
}

// Shared by eglChooseConfig and eglGetConfigs: the guest array holds 32-bit handles, never host
// EGLConfig pointers, so the driver fills a host vector that is then mapped handle by handle.
struct ConfigOutcome {
    EGLint size = -1;
    EGLint count = -1;
    EGLBoolean ok = 0;
};

bool serve_configs(HostEgl& host, HostEgl::Call& call, EGLDisplay dpy,
                   const std::vector<EGLint>* attribs, unsigned configs_position,
                   ConfigOutcome* outcome = nullptr) {
    const EGLint config_size = call.scalar<EGLint>(configs_position + 1);
    if (outcome != nullptr) outcome->size = config_size;
    if (!call.valid()) return true;
    if (config_size < 0) {
        call.fail(kEglBadParameter, "config array size is negative");
        return true;
    }
    std::uint32_t* guest_configs = call.pointer<std::uint32_t>(
        configs_position, static_cast<std::uint64_t>(config_size), kPageRead | kPageWrite);
    EGLint* num_config = call.pointer<EGLint>(configs_position + 2, 1u, kPageRead | kPageWrite);
    if (!call.valid()) return true;
    if (num_config == nullptr) {
        call.fail(kEglBadParameter, "num_config is null");
        return true;
    }
    std::vector<EGLConfig> found(guest_configs == nullptr ? 0
                                                          : static_cast<std::size_t>(config_size));
    EGLint count = 0;
    const EGLBoolean ok =
        attribs == nullptr
            ? host.backend().eglGetConfigs(dpy, found.empty() ? nullptr : found.data(),
                                           config_size, &count)
            : host.backend().eglChooseConfig(dpy, attribs->empty() ? nullptr : attribs->data(),
                                             found.empty() ? nullptr : found.data(), config_size,
                                             &count);
    if (outcome != nullptr) {
        outcome->count = count;
        outcome->ok = ok;
    }
    if (ok != 0 && guest_configs != nullptr) {
        const std::size_t written =
            std::min<std::size_t>(found.size(), count < 0 ? 0 : static_cast<std::size_t>(count));
        for (std::size_t i = 0; i < written; ++i) {
            guest_configs[i] = host.handle_for(found[i], EglObject::Config);
        }
    }
    *num_config = count;
    call.set_result(ok);
    return true;
}

// EGL 1.5 sync, image and native-pixmap objects are host pointers that the generated protocol
// passes as plain 32-bit scalars, so they cannot cross safely. Nothing in the guest surface path
// uses them; they are rejected rather than truncated.
bool unsupported(HostEgl::Call& call, const char* what) {
    call.fail(kEglBadParameter, what);
    return true;
}

}  // namespace

bool zbegl_manual_eglChooseConfig(HostEgl& host, HostEgl::Call& call) {
    EGLDisplay dpy = as_pointer(call.handle_of(0, EglObject::Display));
    if (!call.valid()) return true;
    std::vector<EGLint> attribs;
    if (!read_attribs(host, call, 1, attribs)) return true;
    ConfigOutcome outcome;
    const bool served = serve_configs(host, call, dpy, &attribs, 2, &outcome);
    // Flutter chooses one configuration per context (onscreen, then offscreen) and does not check
    // the second for failure, so record what each request asked for and how many it got.
    static std::atomic<unsigned> chosen{0};
    const unsigned seen = chosen.fetch_add(1) + 1;
    if (seen <= 4) {
        std::string text;
        char word[24];
        for (std::size_t i = 0; i + 1 < attribs.size() && i < 24; i += 2) {
            std::snprintf(word, sizeof word, "%s0x%x=%d", text.empty() ? "" : " ", attribs[i], attribs[i + 1]);
            text += word;
        }
        std::snprintf(word, sizeof word, " -> ok=%d configs=%d/%d", outcome.ok, outcome.count,
                      outcome.size);
        runtime_report().note_egl_object("choose-" + std::to_string(seen), text + word);
    }
    return served;
}

bool zbegl_manual_eglGetConfigs(HostEgl& host, HostEgl::Call& call) {
    EGLDisplay dpy = as_pointer(call.handle_of(0, EglObject::Display));
    if (!call.valid()) return true;
    return serve_configs(host, call, dpy, nullptr, 1);
}

bool zbegl_manual_eglGetConfigAttrib(HostEgl& host, HostEgl::Call& call) {
    return query_attribute(host, call, EglObject::Config);
}

bool zbegl_manual_eglQuerySurface(HostEgl& host, HostEgl::Call& call) {
    return query_attribute(host, call, EglObject::Surface);
}

bool zbegl_manual_eglQueryContext(HostEgl& host, HostEgl::Call& call) {
    return query_attribute(host, call, EglObject::Context);
}

bool zbegl_manual_eglQueryString(HostEgl& host, HostEgl::Call& call) {
    const std::uint32_t display_handle = call.arg(0);
    EGLDisplay dpy = as_pointer(call.handle_of(0, EglObject::Display));
    const EGLint name = call.scalar<EGLint>(1);
    if (!call.valid()) return true;
    // One guest copy per (display, name): the driver string is constant, and the guest keeps the
    // pointer. HostEgl is process-lifetime, so the cache is too.
    static std::mutex mutex;
    static std::map<std::pair<std::uint32_t, EGLint>, std::uint32_t> cache;
    {
        std::lock_guard<std::mutex> lock(mutex);
        const auto found = cache.find({display_handle, name});
        if (found != cache.end()) {
            call.set_result(found->second);
            return true;
        }
    }
    const char* source = host.backend().eglQueryString(dpy, name);
    if (source == nullptr) return true;
    const std::size_t length = strnlen(source, kMaxDriverString);
    if (length == kMaxDriverString) {
        call.fail(kEglBadParameter, "driver string exceeds 64 MiB");
        return true;
    }
    const auto address = host.allocate_guest(length + 1);
    if (!address) {
        call.fail(kEglBadAlloc, "guest allocation for the driver string failed");
        return true;
    }
    std::uint8_t* destination =
        host.runtime().memory().host_ptr(*address, length + 1, kPageRead | kPageWrite);
    if (destination == nullptr) {
        call.fail(kEglBadAlloc, "guest allocator returned an unreadable buffer");
        return true;
    }
    std::memcpy(destination, source, length + 1);
    {
        std::lock_guard<std::mutex> lock(mutex);
        cache[{display_handle, name}] = *address;
    }
    call.set_result(*address);
    return true;
}

bool zbegl_manual_eglGetProcAddress(HostEgl& host, HostEgl::Call& call) {
    std::string name;
    if (!read_guest_string(host, call, 0, name)) return true;
    // Only our own gl*/egl* stubs may be handed out, never an arbitrary guest symbol. The guest
    // gets the address of a trap stub, never a host function pointer.
    const bool ours = name.rfind("egl", 0) == 0 || name.rfind("gl", 0) == 0;
    std::uint32_t address = ours ? host.stub_address(name) : 0;
    // A name of ours that has no stub still gets a callable address: one that does nothing and
    // returns zero. An engine that stores the result and calls it without checking - Unity does -
    // otherwise jumps to address zero and dies far from here.
    const bool missing = ours && address == 0;
    if (missing) address = host.stub_address("zbUnsupportedEntryPoint");
    call.set_result(address);
    // A miss is what an engine sees as "this GL function does not exist", so name the first few:
    // they are the exact list a later GLES 3 generator pass has to cover.
    if (missing) {
        // The first 8 misses name what the engine probed early; the last 8 are kept in a ring
        // because the miss that kills a guest is usually late (Impeller stores a proc table and
        // calls through it without a null check, long after the first probe).
        constexpr std::size_t kRecentMisses = 8;
        static std::mutex miss_mutex;
        static std::string recent[kRecentMisses];
        static unsigned missed = 0;
        std::lock_guard<std::mutex> lock(miss_mutex);
        const unsigned seen = ++missed;
        if (seen <= 8) runtime_report().note_egl_object("procaddress-miss-" + std::to_string(seen), name);
        runtime_report().note_egl_object("procaddress-misses", std::to_string(seen));
        recent[(seen - 1) % kRecentMisses] = name;
        const unsigned kept = seen < kRecentMisses ? seen : static_cast<unsigned>(kRecentMisses);
        for (unsigned i = 0; i < kept; ++i) {
            runtime_report().note_egl_object(
                "procaddress-last-" + std::to_string(i + 1),
                recent[(seen - kept + i) % kRecentMisses]);
        }
    }
    return true;
}

bool zbegl_manual_eglCreateContext(HostEgl& host, HostEgl::Call& call) {
    EGLDisplay dpy = as_pointer(call.handle_of(0, EglObject::Display));
    if (!call.valid()) return true;
    EGLConfig config = as_pointer(call.handle_of(1, EglObject::Config));
    if (!call.valid()) return true;
    EGLContext share = as_pointer(call.handle_of(2, EglObject::Context));
    if (!call.valid()) return true;
    std::vector<EGLint> attribs;
    if (!read_attribs(host, call, 3, attribs)) return true;
    EGLContext context = host.backend().eglCreateContext(
        dpy, config, share, attribs.empty() ? nullptr : attribs.data());
    if (context != nullptr) {
        EGLint config_id = -1;
        host.backend().eglGetConfigAttrib(dpy, config, kEglConfigIdAttrib, &config_id);
        EGLint client_version = 1;
        for (std::size_t i = 0; i + 1 < attribs.size(); i += 2) {
            if (attribs[i] == kEglContextClientVersionAttrib) client_version = attribs[i + 1];
        }
        // Flutter creates a resource context that must share objects with the render context;
        // textures uploaded on one are invisible to the other unless share is carried across.
        char detail[128];
        std::snprintf(detail, sizeof detail, "config=%d client-version=%d share-handle=0x%x share=%p tid=%ld",
                      config_id, client_version, call.arg(2), static_cast<const void*>(share),
                      static_cast<long>(::syscall(SYS_gettid)));
        static std::atomic<unsigned> contexts{0};
        runtime_report().note_egl_object("context-" + std::to_string(contexts.fetch_add(1) + 1), detail);
    }
    call.set_handle(context, EglObject::Context);
    return true;
}

bool zbegl_manual_eglCreateWindowSurface(HostEgl& host, HostEgl::Call& call) {
    EGLDisplay dpy = as_pointer(call.handle_of(0, EglObject::Display));
    if (!call.valid()) return true;
    EGLConfig config = as_pointer(call.handle_of(1, EglObject::Config));
    if (!call.valid()) return true;
    // The window is an ANativeWindow handle from HostNativeWindow (Task 5), resolved through the
    // WindowResolver seam HostEgl was built with.
    const std::uint32_t window_handle = call.arg(2);
    if (!call.valid()) return true;
    void* window = as_pointer(host.window_for(window_handle));
    if (window == nullptr) {
        call.fail(kEglBadNativeWindow, "unknown native window handle");
        return true;
    }
    std::vector<EGLint> attribs;
    if (!read_attribs(host, call, 3, attribs)) return true;
    EGLSurface surface = host.backend().eglCreateWindowSurface(
        dpy, config, window, attribs.empty() ? nullptr : attribs.data());
    // Size is not queried here: eglQuerySurface is a separate driver call the guest may not make
    // for a while, and this note must not add a call the guest did not ask for.
    if (surface != nullptr) runtime_report().note_egl_object("surface", "created");
    call.set_handle(surface, EglObject::Surface);
    return true;
}

bool zbegl_manual_eglCreatePlatformWindowSurface(HostEgl& host, HostEgl::Call& call) {
    EGLDisplay dpy = as_pointer(call.handle_of(0, EglObject::Display));
    if (!call.valid()) return true;
    EGLConfig config = as_pointer(call.handle_of(1, EglObject::Config));
    if (!call.valid()) return true;
    const std::uint32_t window_handle = call.arg(2);
    if (!call.valid()) return true;
    void* window = as_pointer(host.window_for(window_handle));
    if (window == nullptr) {
        call.fail(kEglBadNativeWindow, "unknown native window handle");
        return true;
    }
    std::vector<EGLAttrib> attribs;
    if (!read_attribs(host, call, 3, attribs)) return true;
    call.set_handle(host.backend().eglCreatePlatformWindowSurface(
                        dpy, config, window, attribs.empty() ? nullptr : attribs.data()),
                    EglObject::Surface);
    return true;
}

bool zbegl_manual_eglCreatePbufferSurface(HostEgl& host, HostEgl::Call& call) {
    EGLDisplay dpy = as_pointer(call.handle_of(0, EglObject::Display));
    if (!call.valid()) return true;
    EGLConfig config = as_pointer(call.handle_of(1, EglObject::Config));
    if (!call.valid()) return true;
    std::vector<EGLint> attribs;
    if (!read_attribs(host, call, 2, attribs)) return true;
    call.set_handle(host.backend().eglCreatePbufferSurface(dpy, config,
                                                           attribs.empty() ? nullptr
                                                                           : attribs.data()),
                    EglObject::Surface);
    return true;
}

bool zbegl_manual_eglGetPlatformDisplay(HostEgl& host, HostEgl::Call& call) {
    const EGLenum platform = call.scalar<EGLenum>(0);
    const std::uint32_t native_display = call.arg(1);
    if (!call.valid()) return true;
    if (native_display != 0) {
        // EGL_DEFAULT_DISPLAY is the only native display a guest can name: anything else would
        // be a host pointer truncated to 32 bits.
        return unsupported(call, "only EGL_DEFAULT_DISPLAY is supported");
    }
    std::vector<EGLAttrib> attribs;
    if (!read_attribs(host, call, 2, attribs)) return true;
    call.set_handle(host.backend().eglGetPlatformDisplay(platform, nullptr,
                                                         attribs.empty() ? nullptr
                                                                         : attribs.data()),
                    EglObject::Display);
    return true;
}

bool zbegl_manual_eglGetCurrentDisplay(HostEgl& host, HostEgl::Call& call) {
    call.set_handle(host.backend().eglGetCurrentDisplay(), EglObject::Display);
    return true;
}

bool zbegl_manual_eglGetCurrentContext(HostEgl& host, HostEgl::Call& call) {
    call.set_handle(host.backend().eglGetCurrentContext(), EglObject::Context);
    return true;
}

bool zbegl_manual_eglGetCurrentSurface(HostEgl& host, HostEgl::Call& call) {
    const EGLint readdraw = call.scalar<EGLint>(0);
    if (!call.valid()) return true;
    call.set_handle(host.backend().eglGetCurrentSurface(readdraw), EglObject::Surface);
    return true;
}

bool zbegl_manual_eglSwapBuffers(HostEgl& host, HostEgl::Call& call) {
    EGLDisplay dpy = as_pointer(call.handle_of(0, EglObject::Display));
    if (!call.valid()) return true;
    EGLSurface surface = as_pointer(call.handle_of(1, EglObject::Surface));
    if (!call.valid()) return true;
    runtime_report().note_egl_swap();
    if (gl_diagnostics_enabled()) gl_diagnose_swap();
    call.set_result(host.backend().eglSwapBuffers(dpy, surface));
    return true;
}

bool zbegl_manual_eglCreateImage(HostEgl&, HostEgl::Call& call) {
    return unsupported(call, "EGLImage objects cannot cross to a 32-bit guest");
}

bool zbegl_manual_eglCreateSync(HostEgl&, HostEgl::Call& call) {
    return unsupported(call, "EGLSync objects cannot cross to a 32-bit guest");
}

bool zbegl_manual_eglCreatePbufferFromClientBuffer(HostEgl&, HostEgl::Call& call) {
    return unsupported(call, "EGLClientBuffer objects cannot cross to a 32-bit guest");
}

bool zbegl_manual_eglCreatePixmapSurface(HostEgl&, HostEgl::Call& call) {
    return unsupported(call, "native pixmaps are not supported");
}

bool zbegl_manual_eglCreatePlatformPixmapSurface(HostEgl&, HostEgl::Call& call) {
    return unsupported(call, "native pixmaps are not supported");
}

}  // namespace zb
