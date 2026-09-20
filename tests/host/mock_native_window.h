#pragma once

#include <cstdint>
#include <unordered_map>

#include "zb/native_window_backend.h"

// In-memory NativeWindowBackend for host tests: fromSurface hands out fixed-geometry fake
// windows, one per distinct host surface value. References are counted as a real ANativeWindow
// counts them, so a window outlives a release that is not its last.
class MockNativeWindow final : public zb::NativeWindowBackend {
public:
    void* from_surface(void* env, void* surface) override {
        last_env = env;
        last_surface = surface;
        auto* window = new int(static_cast<int>(++next_window_));
        windows_[window] = 1;
        return window;
    }

    void acquire(void* window) override {
        const auto entry = windows_.find(window);
        if (entry == windows_.end()) return;
        ++entry->second;
        ++acquired_;
    }

    void release(void* window) override {
        const auto entry = windows_.find(window);
        if (entry == windows_.end()) return;
        ++released_;
        if (--entry->second != 0) return;
        windows_.erase(entry);
        delete static_cast<int*>(window);
    }

    std::int32_t query(void* window, Query which) override {
        if (!windows_.count(window)) return -1;
        switch (which) {
        case Query::Width:
            return width;
        case Query::Height:
            return height;
        case Query::Format:
            return format;
        }
        return -1;
    }

    std::int32_t set_buffers_geometry(void* window, std::int32_t w, std::int32_t h, std::int32_t f) override {
        if (!windows_.count(window)) return -1;
        last_geometry_width = w;
        last_geometry_height = h;
        last_geometry_format = f;
        return 0;
    }

    int released() const { return released_; }
    int acquired() const { return acquired_; }

    void* last_env = nullptr;
    void* last_surface = nullptr;
    std::int32_t width = 1080;
    std::int32_t height = 2376;
    std::int32_t format = 1;  // PIXEL_FORMAT_RGBA_8888
    std::int32_t last_geometry_width = 0;
    std::int32_t last_geometry_height = 0;
    std::int32_t last_geometry_format = 0;

private:
    std::unordered_map<void*, int> windows_;
    std::uint64_t next_window_ = 0;
    int released_ = 0;
    int acquired_ = 0;
};
