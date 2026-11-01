#include "zb/host_assets.h"

#include <unistd.h>

#include <cstring>
#include <limits>
#include <optional>
#include <string>

#include "zb/asset_hostcalls.h"
#include "zb/guest_memory.h"
#include "zb/host_jni.h"
#include "zb/log.h"

namespace zb {

namespace {

constexpr std::uint64_t kMaxFilenameLength = 4096;

// Reads a NUL-terminated guest string, page by page like HostJni::Impl::read_string. ok is false
// for an unreadable address or a string longer than kMaxFilenameLength.
std::string read_guest_string(GuestMemory& memory, std::uint32_t address, bool& ok) {
    ok = false;
    if (address == 0) return {};
    std::string text;
    std::uint64_t cursor = address;
    while (cursor < kGuestSpaceSize && text.size() <= kMaxFilenameLength) {
        const std::uint64_t chunk = kPageSize - (cursor & kPageMask);
        const std::uint8_t* bytes = memory.host_ptr(static_cast<std::uint32_t>(cursor), chunk, kPageRead);
        if (bytes == nullptr) return {};
        const auto* end = static_cast<const std::uint8_t*>(std::memchr(bytes, 0, chunk));
        if (end != nullptr) {
            text.append(reinterpret_cast<const char*>(bytes), static_cast<std::size_t>(end - bytes));
            ok = true;
            return text;
        }
        text.append(reinterpret_cast<const char*>(bytes), static_cast<std::size_t>(chunk));
        cursor += chunk;
    }
    return {};
}

}  // namespace

std::uint64_t HostAssets::require_manager(std::uint32_t handle) const {
    return managers_.get(handle).value_or(0);
}

std::uint64_t HostAssets::require_asset(std::uint32_t handle) const {
    return assets_.get(handle).value_or(0);
}

std::uint32_t HostAssets::asset_buffer(std::uint64_t asset) {
    const auto cached = asset_buffers_.find(asset);
    if (cached != asset_buffers_.end()) return cached->second;

    const std::int64_t length = backend_.length(asset);
    const void* host = backend_.buffer(asset);
    if (host == nullptr || length <= 0) {
        log("AAsset_getBuffer: the asset has no buffer (length %lld)", static_cast<long long>(length));
        return 0;
    }
    const auto size = static_cast<std::uint64_t>(length);
    if (size > kMaxBufferedBytes || buffered_bytes_ + size > kMaxBufferedBytes) {
        log("AAsset_getBuffer: %llu bytes exceed the buffer budget", static_cast<unsigned long long>(size));
        return 0;
    }
    // Guest malloc runs guest code on this thread; HostAssets holds no lock here.
    GuestCall args;
    args.regs = {static_cast<std::uint32_t>(size), 0, 0, 0};
    const auto allocated = runtime_.call_on_current(runtime_.service_api().malloc_fn, args);
    if (!allocated || allocated->r0 == 0) {
        log("AAsset_getBuffer: the guest allocator refused %llu bytes",
            static_cast<unsigned long long>(size));
        return 0;
    }
    std::uint8_t* destination = runtime_.memory().host_ptr(allocated->r0, size, kPageRead | kPageWrite);
    if (destination == nullptr) {
        log("AAsset_getBuffer: the guest allocator returned an unusable buffer");
        return 0;
    }
    std::memcpy(destination, host, size);
    asset_buffers_.emplace(asset, allocated->r0);
    buffered_bytes_ += size;
    return allocated->r0;
}

bool HostAssets::handle_host_call(std::uint32_t index, GuestThread& thread) {
    auto& regs = thread.regs();
    switch (index) {
    case ZB_ASSET_HC_AAssetManager_fromJava: {
        // regs[0] is the guest JNIEnv*: ignored, we use the real host JNIEnv of this thread
        // (0 outside an active JNI transition; the backend decides what to do with that).
        const JniBackend::Env env = host_jni_.current_env();
        const JniBackend::Ref java_manager = host_jni_.resolve_ref(regs[1], "AAssetManager_fromJava");
        const std::uint64_t manager = backend_.manager_from_java(env, java_manager);
        regs[0] = manager == 0 ? 0 : managers_.add(manager);
        return true;
    }
    case ZB_ASSET_HC_AAssetManager_open: {
        const std::uint64_t manager = require_manager(regs[0]);
        bool ok = false;
        std::string filename;
        if (manager != 0) filename = read_guest_string(runtime_.memory(), regs[1], ok);
        std::uint64_t asset = 0;
        if (manager != 0 && ok) asset = backend_.open(manager, filename, static_cast<std::int32_t>(regs[2]));
        regs[0] = asset == 0 ? 0 : assets_.add(asset);
        return true;
    }
    case ZB_ASSET_HC_AAsset_close: {
        const std::optional<std::uint64_t> asset = assets_.remove(regs[0]);
        if (asset && *asset != 0) backend_.close(*asset);
        regs[0] = 0;
        return true;
    }
    case ZB_ASSET_HC_AAsset_getLength: {
        const std::uint64_t asset = require_asset(regs[0]);
        std::int64_t length = asset != 0 ? backend_.length(asset) : -1;
        if (length > std::numeric_limits<std::int32_t>::max()) {
            if (!logged_overflow_) {
                log("AAsset_getLength: asset length %lld exceeds INT32_MAX; returning -1",
                    static_cast<long long>(length));
                logged_overflow_ = true;
            }
            length = -1;
        }
        regs[0] = static_cast<std::uint32_t>(static_cast<std::int32_t>(length));
        return true;
    }
    case ZB_ASSET_HC_AAsset_getLength64: {
        // off64_t comes back in r0:r1 (AAPCS32 64-bit return, low word first).
        const std::uint64_t asset = require_asset(regs[0]);
        const std::int64_t length = asset != 0 ? backend_.length(asset) : -1;
        const auto bits = static_cast<std::uint64_t>(length);
        regs[0] = static_cast<std::uint32_t>(bits);
        regs[1] = static_cast<std::uint32_t>(bits >> 32);
        return true;
    }
    case ZB_ASSET_HC_AAsset_read: {
        const std::uint64_t asset = require_asset(regs[0]);
        const std::uint32_t buf_addr = regs[1];
        const std::uint32_t count = regs[2];
        std::int64_t result = -1;
        if (asset != 0) {
            if (count == 0) {
                result = 0;
            } else {
                std::uint8_t* host_buf = runtime_.memory().host_ptr(buf_addr, count, kPageWrite);
                if (host_buf != nullptr) result = backend_.read(asset, host_buf, count);
            }
        }
        regs[0] = static_cast<std::uint32_t>(static_cast<std::int32_t>(result));
        return true;
    }
    case ZB_ASSET_HC_AAsset_getBuffer: {
        const std::uint64_t asset = require_asset(regs[0]);
        regs[0] = asset != 0 ? asset_buffer(asset) : 0;
        return true;
    }
    case ZB_ASSET_HC_AAsset_openFileDescriptor: {
        const std::uint64_t asset = require_asset(regs[0]);
        std::int32_t fd = -1;
        if (asset != 0) {
            AssetBackend::FileDescriptor descriptor = backend_.open_file_descriptor(asset);
            const bool fits = descriptor.fd >= 0 && descriptor.start >= 0 &&
                              descriptor.start <= std::numeric_limits<std::int32_t>::max() &&
                              descriptor.length >= 0 &&
                              descriptor.length <= std::numeric_limits<std::int32_t>::max();
            if (fits) {
                std::uint8_t* out_start = runtime_.memory().host_ptr(regs[1], 4, kPageWrite);
                std::uint8_t* out_length = runtime_.memory().host_ptr(regs[2], 4, kPageWrite);
                if (out_start != nullptr && out_length != nullptr) {
                    const std::uint32_t start32 = static_cast<std::uint32_t>(descriptor.start);
                    const std::uint32_t length32 = static_cast<std::uint32_t>(descriptor.length);
                    std::memcpy(out_start, &start32, 4);
                    std::memcpy(out_length, &length32, 4);
                    fd = descriptor.fd;
                } else {
                    ::close(descriptor.fd);
                }
            } else if (descriptor.fd >= 0) {
                ::close(descriptor.fd);
            }
        }
        regs[0] = static_cast<std::uint32_t>(fd);
        return true;
    }
    default:
        return false;
    }
}

}  // namespace zb
