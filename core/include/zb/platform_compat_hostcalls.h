#pragma once

#include <cstdint>

namespace zb {

// Append-only compatibility exports from tools/gen_stubs.py. These deliberately live after the
// Phase 7a EGL range (168-211), so established GLES, asset, window and EGL indices never move.
inline constexpr std::uint32_t ZB_COMPAT_HC_ANativeWindow_lock = 212u;
inline constexpr std::uint32_t ZB_COMPAT_HC_ANativeWindow_unlockAndPost = 213u;
inline constexpr std::uint32_t ZB_COMPAT_HC_eglCreateImageKHR = 214u;
inline constexpr std::uint32_t ZB_COMPAT_HC_eglDestroyImageKHR = 215u;
inline constexpr std::uint32_t ZB_COMPAT_HC_glEGLImageTargetTexture2DOES = 216u;
inline constexpr std::uint32_t ZB_COMPAT_HC_AndroidBitmap_getInfo = 217u;
inline constexpr std::uint32_t ZB_COMPAT_HC_AndroidBitmap_lockPixels = 218u;
inline constexpr std::uint32_t ZB_COMPAT_HC_AndroidBitmap_unlockPixels = 219u;
inline constexpr std::uint32_t ZB_COMPAT_HC_ALooper_acquire = 220u;
inline constexpr std::uint32_t ZB_COMPAT_HC_ALooper_addFd = 221u;
inline constexpr std::uint32_t ZB_COMPAT_HC_ALooper_forThread = 222u;
inline constexpr std::uint32_t ZB_COMPAT_HC_ALooper_pollOnce = 223u;
inline constexpr std::uint32_t ZB_COMPAT_HC_ALooper_prepare = 224u;
inline constexpr std::uint32_t ZB_COMPAT_HC_ALooper_release = 225u;
inline constexpr std::uint32_t ZB_COMPAT_HC_ALooper_removeFd = 226u;
inline constexpr std::uint32_t ZB_COMPAT_HC_ALooper_wake = 227u;
// Appended after the input and configuration ranges: ALooper_pollAll is gone from the NDK headers
// but Unity 4.5 still imports it, and it was added once a real guest asked for it.
inline constexpr std::uint32_t ZB_COMPAT_HC_ALooper_pollAll = 491u;

}  // namespace zb
