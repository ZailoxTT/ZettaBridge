#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "zb/guest_memory.h"
#include "zb/guest_thread.h"
#include "zb/library_protocol.h"
#include "zb/native_call.h"
#include "zb/process.h"

namespace zb {

struct LibraryRuntimeOptions {
    std::string zbhost;   // host path of the arm32 zbhost executable
    std::string sysroot;  // host directory with the arm32 system files
    std::uint32_t target_sdk = 0;
    std::vector<std::string> guest_environment;  // guest environment, e.g. LD_LIBRARY_PATH=...
    std::string preload;            // guest path dlopen'ed RTLD_GLOBAL before READY; empty: none
    std::chrono::milliseconds ready_timeout{10000};
};

// A guest process in library mode: zbhost runs on a service guest thread that owns the boot
// JIT and serves requests from inside its READY host call.
//
// The runtime is process-lifetime. Guest threads cannot be torn down, so after a successful
// start() the destructor logs and aborts; tests end with std::_Exit.
class LibraryRuntime {
public:
    class Carrier;

    LibraryRuntime();
    ~LibraryRuntime();
    LibraryRuntime(const LibraryRuntime&) = delete;
    LibraryRuntime& operator=(const LibraryRuntime&) = delete;

    // Receives every host call outside the runtime range 0xFE00-0xFEFF. Call before start().
    void set_host_call_handler(Process::HostCallHandler handler);
    bool start(const LibraryRuntimeOptions& options, std::string& error);

    // Guest dlopen/dlsym/dlerror on the service thread. Flags are ZB_GUEST_RTLD_* values.
    // Return 0 and set error (guest dlerror text) on failure.
    std::uint32_t load_library(const std::string& path, std::uint32_t guest_flags, std::string& error);
    std::uint32_t find_symbol(std::uint32_t handle, const std::string& name, std::string& error);
    std::optional<GuestResult> call_on_service(std::uint32_t function, const GuestCall& args);
    // Nested call on the guest thread the calling host thread already runs (for host-call
    // handlers). nullopt if the calling host thread runs no guest code.
    std::optional<GuestResult> call_on_current(std::uint32_t function, const GuestCall& args);

    // Leases a new carrier (a guest pthread parked in PARK) to the calling host thread.
    // Spawning is serialized through the service thread. Fails on a host thread that already
    // runs guest code (the service thread, a borrower, a guest pthread): use call_on_current.
    std::unique_ptr<Carrier> borrow(std::string& error);

    GuestMemory& memory();
    // Redirects guest file operations under one directory to another (Process::add_path_alias):
    // the launcher's arm64 proxy directory stands for the plugin's arm32 libraries.
    void add_path_alias(std::string guest_prefix, std::string host_prefix);
    // Paths of the files the guest has mapped (Process::mapped_file_paths).
    std::vector<std::string> mapped_file_paths() const;
    // Valid after a successful start().
    const zb_service_api& service_api() const;
    // Real guest pthreads (service, carriers, guest-created threads); borrowers are not counted.
    std::size_t guest_thread_count() const;
    // True when this guest thread is a borrower: a host thread that entered the guest on a leased
    // carrier. Such a thread came from Java and has its own real Android looper, so guest loopers
    // prepared on it cannot be driven by us. False for the service thread, a carrier and every
    // guest-created pthread.
    bool is_borrower(const GuestThread& thread) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// A lease of one carrier, bound to the host thread that borrowed it. Each lease costs two
// processor ids and two JITs (a 2 MiB carrier JIT and a 32 MiB borrower JIT), and the borrower
// translates guest code cold. Destroying the lease (on the same host thread) releases the
// carrier, which returns from PARK and exits through bionic.
class LibraryRuntime::Carrier {
public:
    ~Carrier();
    Carrier(const Carrier&) = delete;
    Carrier& operator=(const Carrier&) = delete;

    std::optional<GuestResult> call(std::uint32_t function, const GuestCall& args);
    // Guest dlopen/dlsym on this carrier, with dlerror read on the same guest thread.
    std::uint32_t load_library(const std::string& path, std::uint32_t guest_flags, std::string& error);
    std::uint32_t find_symbol(std::uint32_t handle, const std::string& name, std::string& error);
    std::int32_t guest_tid() const;
    std::uint32_t guest_tls() const;

private:
    friend class LibraryRuntime;
    struct State;
    explicit Carrier(std::unique_ptr<State> state);
    // Guest malloc'ed string buffer of this carrier, allocated on first use.
    bool ensure_scratch(std::string& error);

    std::unique_ptr<State> state_;
};

}  // namespace zb
