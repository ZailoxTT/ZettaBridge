#pragma once

#include <cstdint>

namespace zb {

// Host-call indices for the six AAsset* functions HostAssets serves (Phase 5 Task 7). Hand-written
// from tools/gen_stubs.py's android_asset_names() output (core/src/gen/hostcalls.inc), which is the
// generated source of truth: if gen_stubs.py's function list or ordering changes, update these to
// match. The comment in core/include/zb/host_gl.h ("Serves the GLES range 0-141 and leaves 142-160
// for HostAssets") documents the reserved range; only these six of the ~18 AAsset* stubs in that
// range are implemented so far.
inline constexpr std::uint32_t ZB_ASSET_HC_AAssetManager_fromJava = 145u;
inline constexpr std::uint32_t ZB_ASSET_HC_AAssetManager_open = 146u;
inline constexpr std::uint32_t ZB_ASSET_HC_AAsset_close = 148u;
inline constexpr std::uint32_t ZB_ASSET_HC_AAsset_getBuffer = 149u;
inline constexpr std::uint32_t ZB_ASSET_HC_AAsset_getLength = 150u;
inline constexpr std::uint32_t ZB_ASSET_HC_AAsset_getLength64 = 151u;
inline constexpr std::uint32_t ZB_ASSET_HC_AAsset_openFileDescriptor = 155u;
inline constexpr std::uint32_t ZB_ASSET_HC_AAsset_read = 157u;

}  // namespace zb
