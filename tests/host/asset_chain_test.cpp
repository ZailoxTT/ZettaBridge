// AAsset* host calls against the mock AssetBackend (Phase 5 Task 7): open/getLength/read/close
// round trip, a missing file, an out-of-bounds buffer, a closed handle, 32-bit
// AAsset_openFileDescriptor outputs, and that GuestJniEngine chains HostAssets the way it chains
// HostGl and HostJni.
#include <sys/mman.h>

#include <cstdint>
#include <cstring>

#include <dynarmic/interface/exclusive_monitor.h>

#include "check.h"
#include "mock_assets.h"
#include "mock_jvm.h"
#include "zb/asset_hostcalls.h"
#include "zb/guest_memory.h"
#include "zb/host_assets.h"
#include "zb/proxy_runtime.h"

namespace {

std::uint32_t write_guest_string(zb::LibraryRuntime& runtime, std::uint32_t address, const std::string& text) {
    std::uint8_t* host = runtime.memory().host_ptr(address, text.size() + 1, zb::kPageWrite);
    CHECK(host != nullptr);
    std::memcpy(host, text.c_str(), text.size() + 1);
    return address;
}

}  // namespace

int main() {
    auto* vm = new zb::mock::MockJvm();
    auto* backend = new MockAssetBackend();
    backend->add_file("assets/hello.txt", "hello asset bridge");

    auto* engine = new zb::GuestJniEngine(*vm, /*gl_backend=*/nullptr, /*egl_context_probe=*/{}, backend);
    CHECK(engine->host_assets() != nullptr);

    zb::LibraryRuntime& runtime = engine->runtime();
    CHECK(runtime.memory().map_anon(0x10000, 0x2000, PROT_READ | PROT_WRITE));
    Dynarmic::ExclusiveMonitor monitor(1);
    zb::GuestThread thread(runtime.memory(), &monitor, 0, false, zb::kCarrierCodeCacheSize);
    thread.regs()[13] = 0x10000;
    zb::HostAssets& assets = *engine->host_assets();

    // A JNI-range index is not an asset call; HostAssets declines it, like HostGl does.
    CHECK(!assets.handle_host_call(0xFC00, thread));

    // AAssetManager_fromJava: guest jobject handle 0 (null) resolves without touching the handle
    // tables; the mock backend always succeeds.
    thread.regs()[0] = 0;  // guest JNIEnv* (ignored)
    thread.regs()[1] = 0;  // guest jobject handle
    CHECK(assets.handle_host_call(zb::ZB_ASSET_HC_AAssetManager_fromJava, thread));
    const std::uint32_t manager = thread.regs()[0];
    CHECK(manager != 0);

    // AAssetManager_open + AAsset_getLength + AAsset_read: round trip.
    const std::uint32_t filename_addr = write_guest_string(runtime, 0x11000, "assets/hello.txt");
    thread.regs()[0] = manager;
    thread.regs()[1] = filename_addr;
    thread.regs()[2] = 0;  // AASSET_MODE_UNKNOWN
    CHECK(assets.handle_host_call(zb::ZB_ASSET_HC_AAssetManager_open, thread));
    const std::uint32_t asset = thread.regs()[0];
    CHECK(asset != 0);

    thread.regs()[0] = asset;
    CHECK(assets.handle_host_call(zb::ZB_ASSET_HC_AAsset_getLength, thread));
    CHECK(thread.regs()[0] == std::strlen("hello asset bridge"));

    // AAsset_getLength64 returns off64_t in r0:r1. r1 is preset to garbage so a handler that only
    // writes r0 fails here.
    thread.regs()[0] = asset;
    thread.regs()[1] = 0xDEADBEEFu;
    CHECK(assets.handle_host_call(zb::ZB_ASSET_HC_AAsset_getLength64, thread));
    CHECK(thread.regs()[0] == std::strlen("hello asset bridge"));
    CHECK(thread.regs()[1] == 0);

    const std::uint32_t buffer_addr = 0x11100;
    thread.regs()[0] = asset;
    thread.regs()[1] = buffer_addr;
    thread.regs()[2] = 64;
    CHECK(assets.handle_host_call(zb::ZB_ASSET_HC_AAsset_read, thread));
    const std::int32_t bytes_read = static_cast<std::int32_t>(thread.regs()[0]);
    CHECK(bytes_read == static_cast<std::int32_t>(std::strlen("hello asset bridge")));
    const std::uint8_t* read_back = runtime.memory().host_ptr(buffer_addr, static_cast<std::size_t>(bytes_read), zb::kPageRead);
    CHECK(read_back != nullptr);
    CHECK(std::memcmp(read_back, "hello asset bridge", static_cast<std::size_t>(bytes_read)) == 0);

    // A missing file: AAssetManager_open returns 0.
    const std::uint32_t missing_addr = write_guest_string(runtime, 0x11200, "assets/missing.txt");
    thread.regs()[0] = manager;
    thread.regs()[1] = missing_addr;
    thread.regs()[2] = 0;
    CHECK(assets.handle_host_call(zb::ZB_ASSET_HC_AAssetManager_open, thread));
    CHECK(thread.regs()[0] == 0);

    // An out-of-bounds buffer pointer is rejected: AAsset_read returns -1, the mock backend is
    // never reached for that call (nothing is written, no crash).
    thread.regs()[0] = asset;
    thread.regs()[1] = 0xFFFF0000u;  // unmapped
    thread.regs()[2] = 16;
    CHECK(assets.handle_host_call(zb::ZB_ASSET_HC_AAsset_read, thread));
    CHECK(static_cast<std::int32_t>(thread.regs()[0]) == -1);

    // AAsset_openFileDescriptor: writes 32-bit start/length and returns the fd.
    const std::uint32_t out_start_addr = 0x11300;
    const std::uint32_t out_length_addr = 0x11304;
    thread.regs()[0] = asset;
    thread.regs()[1] = out_start_addr;
    thread.regs()[2] = out_length_addr;
    CHECK(assets.handle_host_call(zb::ZB_ASSET_HC_AAsset_openFileDescriptor, thread));
    CHECK(static_cast<std::int32_t>(thread.regs()[0]) == backend->fake_fd);
    std::uint32_t out_start = 0, out_length = 0;
    std::memcpy(&out_start, runtime.memory().host_ptr(out_start_addr, 4, zb::kPageRead), 4);
    std::memcpy(&out_length, runtime.memory().host_ptr(out_length_addr, 4, zb::kPageRead), 4);
    CHECK(out_start == static_cast<std::uint32_t>(backend->fake_fd_start));
    CHECK(out_length == static_cast<std::uint32_t>(backend->fake_fd_length));

    // AAsset_close, then every further operation on the closed handle is rejected.
    thread.regs()[0] = asset;
    CHECK(assets.handle_host_call(zb::ZB_ASSET_HC_AAsset_close, thread));

    thread.regs()[0] = asset;
    CHECK(assets.handle_host_call(zb::ZB_ASSET_HC_AAsset_getLength, thread));
    CHECK(static_cast<std::int32_t>(thread.regs()[0]) == -1);

    thread.regs()[0] = asset;
    CHECK(assets.handle_host_call(zb::ZB_ASSET_HC_AAsset_getLength64, thread));
    CHECK(thread.regs()[0] == 0xFFFFFFFFu && thread.regs()[1] == 0xFFFFFFFFu);

    thread.regs()[0] = asset;
    thread.regs()[1] = buffer_addr;
    thread.regs()[2] = 16;
    CHECK(assets.handle_host_call(zb::ZB_ASSET_HC_AAsset_read, thread));
    CHECK(static_cast<std::int32_t>(thread.regs()[0]) == -1);

    thread.regs()[0] = asset;
    CHECK(assets.handle_host_call(zb::ZB_ASSET_HC_AAsset_openFileDescriptor, thread));
    CHECK(static_cast<std::int32_t>(thread.regs()[0]) == -1);

    // AAsset_getBuffer needs guest memory from the guest allocator. Without a started guest
    // runtime it must answer NULL instead of handing over a host pointer or crashing; the real
    // copy is exercised on the device.
    thread.regs()[0] = asset;
    CHECK(assets.handle_host_call(zb::ZB_ASSET_HC_AAsset_getBuffer, thread));
    CHECK(thread.regs()[0] == 0);

    std::puts("asset_chain_test PASS");
    return 0;
}
