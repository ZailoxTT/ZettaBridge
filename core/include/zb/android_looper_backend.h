#pragma once

#include <cstdint>

namespace zb {

// Portable seam behind the real Android looper of a host thread (<android/looper.h>).
//
// HostLooper implements ALooper itself for guest-created threads: they call ALooper_pollOnce and
// the bridge dispatches their callbacks. A host thread that entered the guest on a borrowed
// carrier (a Java thread) is different: it builds its message loop inside the guest, returns to
// Java and never polls again, because on a real device its own Android looper - the one
// Looper.loop() runs - polls the descriptors. For those threads the fds must be registered with
// that real looper instead, which is what this interface does.
//
// Implementations: the real NDK looper (core/android/looper_driver_backend.*) and an in-memory
// mock for host tests (tests/host/mock_looper.h). All handles are opaque; 0 is invalid.
class AndroidLooperBackend {
public:
    virtual ~AndroidLooperBackend() = default;

    // ALooper_addFd's callback, invoked by the real looper on the host thread that owns it, from
    // that thread's own loop. Returning 0 unregisters the fd, any other value keeps it.
    using Callback = int (*)(int fd, int events, void* data);

    // ALooper_prepare on the calling host thread; options is ALOOPER_PREPARE_ALLOW_NON_CALLBACKS
    // or 0. Returns an opaque looper handle, or 0 when the calling thread cannot have one.
    virtual std::uint64_t prepare(int options) = 0;
    // ALooper_forThread: the real looper already prepared for the calling host thread, or 0.
    virtual std::uint64_t for_thread() = 0;
    // ALooper_acquire / ALooper_release on a handle obtained above.
    virtual void acquire(std::uint64_t looper) = 0;
    virtual void release(std::uint64_t looper) = 0;

    // ALooper_addFd with a host callback and opaque data. 1 on success, -1 on failure. Replacing
    // an fd already registered on the same looper is allowed and replaces callback and data.
    virtual int add_fd(std::uint64_t looper, int fd, int ident, int events, Callback callback,
                       void* data) = 0;
    // ALooper_removeFd: 1 removed, 0 not registered, -1 invalid looper.
    virtual int remove_fd(std::uint64_t looper, int fd) = 0;
    // ALooper_wake: makes that looper return from its poll, on whatever thread runs it.
    virtual void wake(std::uint64_t looper) = 0;

    // ALooper_pollOnce on the calling host thread's own looper: blocks up to timeout_millis and
    // dispatches whatever that looper holds, our registrations among them. Only a guest thread
    // that acquired a real looper of its own calls this; a borrower returns to Java and lets
    // Looper.loop() do the polling. Returns the NDK result.
    // out_fd, out_events and out_data receive what the registration that woke the poll carried,
    // the way ALooper_pollOnce fills them. A guest that registered by ident reads its own data
    // back from here; without them it cannot tell which source is ready.
    virtual int poll_once(int timeout_millis, int* out_fd, int* out_events, void** out_data) = 0;
};

}  // namespace zb
