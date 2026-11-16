#include "asset_driver_backend.h"

#include <android/asset_manager.h>
#include <android/asset_manager_jni.h>
#include <jni.h>

#include <cstdint>

namespace zb {

namespace {

JNIEnv* E(JniBackend::Env env) { return reinterpret_cast<JNIEnv*>(static_cast<std::uintptr_t>(env)); }
jobject O(JniBackend::Ref ref) { return reinterpret_cast<jobject>(static_cast<std::uintptr_t>(ref)); }
AAssetManager* M(std::uint64_t handle) {
    return reinterpret_cast<AAssetManager*>(static_cast<std::uintptr_t>(handle));
}
AAsset* A(std::uint64_t handle) { return reinterpret_cast<AAsset*>(static_cast<std::uintptr_t>(handle)); }

}  // namespace

std::uint64_t AndroidAssetBackend::manager_from_java(JniBackend::Env env, JniBackend::Ref java_manager) {
    if (env == 0 || java_manager == 0) return 0;
    AAssetManager* manager = AAssetManager_fromJava(E(env), O(java_manager));
    return manager == nullptr ? 0 : reinterpret_cast<std::uint64_t>(manager);
}

std::uint64_t AndroidAssetBackend::open(std::uint64_t manager, const std::string& filename, std::int32_t mode) {
    if (manager == 0) return 0;
    AAsset* asset = AAssetManager_open(M(manager), filename.c_str(), mode);
    return asset == nullptr ? 0 : reinterpret_cast<std::uint64_t>(asset);
}

std::int64_t AndroidAssetBackend::length(std::uint64_t asset) {
    if (asset == 0) return -1;
    return static_cast<std::int64_t>(AAsset_getLength64(A(asset)));
}

const void* AndroidAssetBackend::buffer(std::uint64_t asset) {
    if (asset == 0) return nullptr;
    return AAsset_getBuffer(A(asset));
}

std::int64_t AndroidAssetBackend::read(std::uint64_t asset, void* buffer, std::size_t count) {
    if (asset == 0) return -1;
    return static_cast<std::int64_t>(AAsset_read(A(asset), buffer, count));
}

void AndroidAssetBackend::close(std::uint64_t asset) {
    if (asset != 0) AAsset_close(A(asset));
}

AssetBackend::FileDescriptor AndroidAssetBackend::open_file_descriptor(std::uint64_t asset) {
    FileDescriptor result;
    if (asset == 0) return result;
    off_t start = 0;
    off_t length_out = 0;
    const int fd = AAsset_openFileDescriptor(A(asset), &start, &length_out);
    if (fd < 0) return result;
    result.fd = fd;
    result.start = static_cast<std::int64_t>(start);
    result.length = static_cast<std::int64_t>(length_out);
    return result;
}

}  // namespace zb
