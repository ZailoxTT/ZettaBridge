#pragma once

#include "zb/android_looper_backend.h"

namespace zb {

// AndroidLooperBackend over the real NDK looper (<android/looper.h>). Every call runs on the
// calling host thread, which for us is always a Java thread that entered the guest: its ALooper
// is the one Looper.loop() polls, so a descriptor registered here is really watched.
class AndroidLooperDriverBackend final : public AndroidLooperBackend {
public:
    std::uint64_t prepare(int options) override;
    std::uint64_t for_thread() override;
    void acquire(std::uint64_t looper) override;
    void release(std::uint64_t looper) override;
    int add_fd(std::uint64_t looper, int fd, int ident, int events, Callback callback,
               void* data) override;
    int remove_fd(std::uint64_t looper, int fd) override;
    void wake(std::uint64_t looper) override;
    int poll_once(int timeout_millis, int* out_fd, int* out_events, void** out_data) override;
};

}  // namespace zb
