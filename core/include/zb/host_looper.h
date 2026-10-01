#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

#include "zb/guest_thread.h"
#include "zb/native_call.h"

namespace zb {

class AndroidLooperBackend;
class LibraryRuntime;

// Bridges the callback-based Android looper ABI used by arm32 guests. Guest file descriptors
// are process file descriptors, but ALooper objects and callback addresses remain guest values.
//
// Two worlds, decided per calling thread:
// - a guest-created thread owns its looper here: addFd records the fd and ALooper_pollOnce polls
//   it and dispatches the guest callbacks. This is the original behaviour and never changes.
// - a borrower (a host thread that entered the guest on a leased carrier, i.e. a Java thread)
//   builds its message loop here but returns to Java and never polls again; its own Android
//   looper is what polls. Its fds are registered with that real looper through
//   AndroidLooperBackend, and the host callback re-enters the guest to run the guest callback.
class HostLooper {
public:
    // Runs a guest function on the calling host thread. The default asks LibraryRuntime for the
    // guest thread that host thread already runs; the Android build passes HostJni's version,
    // which also uses that host thread's cached carrier when it currently runs no guest code
    // (an Android looper callback arrives with the thread back in Java).
    using GuestInvoker = std::function<std::optional<GuestResult>(std::uint32_t function,
                                                                  const GuestCall& args)>;
    // True when this guest thread is a borrower. The default asks the LibraryRuntime.
    using BorrowerProbe = std::function<bool(const GuestThread& thread)>;
    // An input queue attached to this real looper that has events waiting: its ident and the
    // guest data it was registered with. A queue wakes its looper instead of marking a descriptor
    // when the event came through Java, so a poll that only reports the wake-up leaves the guest
    // waiting for an ident that never comes.
    struct InputReady {
        std::int32_t ident = 0;
        std::uint32_t data = 0;
    };
    using InputProbe = std::function<std::optional<InputReady>(std::uint64_t real_looper)>;
    void set_input_probe(InputProbe probe);

    // The guest tid that owns a guest looper handle, or 0. The attach records it: a queue
    // attached to a looper nobody polls never delivers, and which thread owns what is the first
    // thing to check when it does not.
    std::int32_t owner_of(std::uint32_t looper_handle);

    // backend nullptr (the host build, and any process without an Android looper) keeps every
    // thread on the guest path.
    explicit HostLooper(LibraryRuntime& runtime, AndroidLooperBackend* backend = nullptr,
                        GuestInvoker invoker = {}, BorrowerProbe borrower_probe = {});
    ~HostLooper();
    HostLooper(const HostLooper&) = delete;
    HostLooper& operator=(const HostLooper&) = delete;

    bool handle_host_call(std::uint32_t index, GuestThread& thread);

    // Gives a guest looper a real Android looper and moves its registrations onto it, so a host
    // AInputQueue attached to it can be polled at all. `looper_handle` is the looper the guest
    // named; 0 means the calling thread's own. The real looper comes back in `real`, because the
    // queue must be attached to that one and not to whichever thread happens to be calling: a
    // guest that attaches on one thread and polls on another would otherwise never see its input.
    //
    // A looper that belongs to another thread can only be used if that thread already acquired a
    // real one, because ALooper_prepare works on the calling thread alone.
    bool ensure_real_looper(GuestThread& thread, std::uint32_t looper_handle, std::uint64_t& real);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace zb
