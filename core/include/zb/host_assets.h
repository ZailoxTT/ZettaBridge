#pragma once

#include <cstdint>
#include <unordered_map>

#include "zb/asset_backend.h"
#include "zb/guest_thread.h"
#include "zb/jni_handles.h"
#include "zb/library_runtime.h"

namespace zb {

class HostJni;

// AAsset* host-call dispatcher (Phase 5 Task 7): the six functions liblime.so imports
// (core/include/zb/asset_hostcalls.h). Chained into LibraryRuntime::set_host_call_handler
// alongside HostGl and HostJni, the same way GuestJniEngine wires them
// (core/src/jni/proxy_runtime.cpp). AAssetManager* and AAsset* never cross into the guest as host
// pointers: they are 32-bit handles into handle tables owned here.
class HostAssets {
public:
    HostAssets(LibraryRuntime& runtime, AssetBackend& backend, HostJni& host_jni)
        : runtime_(runtime), backend_(backend), host_jni_(host_jni) {}
    HostAssets(const HostAssets&) = delete;
    HostAssets& operator=(const HostAssets&) = delete;

    // Serves ZB_ASSET_HC_* indices; returns false for any other index (including the other,
    // unimplemented AAsset* stubs in the 142-160 range).
    bool handle_host_call(std::uint32_t index, GuestThread& thread);

    // A guest handle for an AAssetManager the platform handed us directly (the one inside the
    // framework's ANativeActivity), so the guest activity can carry it like any other.
    std::uint32_t handle_for_manager(std::uint64_t manager) {
        return manager == 0 ? 0 : managers_.add(manager);
    }

    // The host AAssetManager behind a guest handle, or 0. HostConfiguration needs it for
    // AConfiguration_fromAssetManager, which takes the manager the guest already holds.
    std::uint64_t manager_for(std::uint32_t handle) const { return require_manager(handle); }

private:
    std::uint64_t require_manager(std::uint32_t handle) const;
    std::uint64_t require_asset(std::uint32_t handle) const;

    LibraryRuntime& runtime_;
    AssetBackend& backend_;
    HostJni& host_jni_;
    // AAsset_getBuffer copies, since the NDK buffer lives outside the guest address space. An
    // asset is read-only, so one copy per asset is enough and never needs writing back.
    std::uint32_t asset_buffer(std::uint64_t asset);

    GlobalHandles managers_{HandleKind::Global};
    GlobalHandles assets_{HandleKind::Global};
    bool logged_overflow_ = false;
    static constexpr std::uint64_t kMaxBufferedBytes = 64ull << 20;
    std::unordered_map<std::uint64_t, std::uint32_t> asset_buffers_;
    std::uint64_t buffered_bytes_ = 0;
};

}  // namespace zb
