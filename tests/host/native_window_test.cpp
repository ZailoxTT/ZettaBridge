// ANativeWindow_* host calls against the mock NativeWindowBackend (Phase 7a Task 5):
// fromSurface resolves a guest jobject handle through HostJni, geometry queries reach the
// backend, release drops the handle and an unknown handle is rejected without reaching the
// backend, and toSurface hands the original surface back out as a guest jobject.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>

#include <dynarmic/interface/exclusive_monitor.h>

#include "check.h"
#include "mock_jvm.h"
#include "mock_native_window.h"
#include "zb/host_jni.h"
#include "zb/host_native_window.h"
#include "zb/library_runtime.h"
#include "zb/window_hostcalls.h"

namespace {

std::uint32_t call_window(zb::HostNativeWindow& windows, zb::GuestThread& thread, std::uint32_t index,
                          std::initializer_list<std::uint32_t> args) {
    auto& regs = thread.regs();
    regs[0] = regs[1] = regs[2] = regs[3] = 0;
    unsigned position = 0;
    for (std::uint32_t value : args) regs[position++] = value;
    CHECK(windows.handle_host_call(index, thread));
    return regs[0];
}

}  // namespace

int main() {
    // Process-lifetime graph like the other chain tests (gl_chain_test.cpp, asset_chain_test.cpp):
    // HostJni's destructor aborts, so these are never deleted; the test ends with std::_Exit.
    zb::LibraryRuntime runtime;
    auto* vm = new zb::mock::MockJvm();
    auto* host_jni = new zb::HostJni(runtime, *vm);
    auto* backend = new MockNativeWindow();
    auto* windows = new zb::HostNativeWindow(runtime, *backend, *host_jni);

    Dynarmic::ExclusiveMonitor monitor(1);
    zb::GuestThread thread(runtime.memory(), &monitor, 0, false, zb::kCarrierCodeCacheSize);

    // An index outside the window range is declined, like HostGl/HostAssets do.
    CHECK(!windows->handle_host_call(0xFC00, thread));

    vm->define_class("android/view/Surface");
    const zb::mock::MockJvm::ObjectId surface_object = vm->new_object("android/view/Surface");
    const std::uint32_t surface_handle = host_jni->new_local_handle(surface_object);
    CHECK(surface_handle != 0);

    // ANativeWindow_fromSurface resolves the guest jobject handle through HostJni and hands
    // back a window handle; geometry comes from the backend.
    const std::uint32_t window =
        call_window(*windows, thread, zb::ZB_WINDOW_HC_ANativeWindow_fromSurface, {0, surface_handle});
    CHECK(window != 0);
    CHECK(backend->last_surface != nullptr);

    CHECK(call_window(*windows, thread, zb::ZB_WINDOW_HC_ANativeWindow_getWidth, {window}) == 1080);
    CHECK(call_window(*windows, thread, zb::ZB_WINDOW_HC_ANativeWindow_getHeight, {window}) == 2376);
    CHECK(call_window(*windows, thread, zb::ZB_WINDOW_HC_ANativeWindow_getFormat, {window}) == 1);

    call_window(*windows, thread, zb::ZB_WINDOW_HC_ANativeWindow_acquire, {window});
    CHECK(backend->acquired() == 1);

    CHECK(call_window(*windows, thread, zb::ZB_WINDOW_HC_ANativeWindow_setBuffersGeometry, {window, 640, 480, 2}) ==
          0);
    CHECK(backend->last_geometry_width == 640);
    CHECK(backend->last_geometry_height == 480);
    CHECK(backend->last_geometry_format == 2);

    // ANativeWindow_toSurface returns a guest jobject handle for the same surface object.
    const std::uint32_t surface_back =
        call_window(*windows, thread, zb::ZB_WINDOW_HC_ANativeWindow_toSurface, {0, window});
    CHECK(surface_back != 0);
    CHECK(host_jni->resolve_ref(surface_back, "check") == surface_object);

    // The guest holds two references here (fromSurface and acquire), so this release is not the
    // last one and the window stays usable. EGL rejected a window surface on a handle dropped
    // this early, which is a black screen with no error anywhere.
    call_window(*windows, thread, zb::ZB_WINDOW_HC_ANativeWindow_release, {window});
    CHECK(backend->released() == 1);
    CHECK(call_window(*windows, thread, zb::ZB_WINDOW_HC_ANativeWindow_getWidth, {window}) == 1080);
    CHECK(windows->value_for(window) != nullptr);

    // The last release drops the handle; every further use is rejected and never reaches the
    // backend.
    call_window(*windows, thread, zb::ZB_WINDOW_HC_ANativeWindow_release, {window});
    CHECK(backend->released() == 2);
    CHECK(windows->value_for(window) == nullptr);

    CHECK(call_window(*windows, thread, zb::ZB_WINDOW_HC_ANativeWindow_getWidth, {window}) ==
          static_cast<std::uint32_t>(-1));
    CHECK(call_window(*windows, thread, zb::ZB_WINDOW_HC_ANativeWindow_getHeight, {window}) ==
          static_cast<std::uint32_t>(-1));
    CHECK(call_window(*windows, thread, zb::ZB_WINDOW_HC_ANativeWindow_toSurface, {0, window}) == 0);

    // A release of a handle that is already gone changes nothing.
    call_window(*windows, thread, zb::ZB_WINDOW_HC_ANativeWindow_release, {window});
    CHECK(backend->released() == 2);

    // An unknown handle also never reaches the backend.
    CHECK(call_window(*windows, thread, zb::ZB_WINDOW_HC_ANativeWindow_getFormat, {0xdeadbeef}) ==
          static_cast<std::uint32_t>(-1));

    std::puts("native_window_test PASS");
    std::fflush(stdout);
    std::_Exit(0);
}
