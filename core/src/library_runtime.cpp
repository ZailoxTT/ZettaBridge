#include "zb/library_runtime.h"

#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>

#include "zb/log.h"
#include "zb/runtime_report.h"

namespace zb {

namespace {

constexpr std::chrono::milliseconds kCarrierParkTimeout{10000};

struct Response {
    bool ok = false;
    std::optional<GuestResult> result;
    std::string error;
};

struct Command {
    enum class Kind { Load, Symbol, Call, SpawnCarrier };
    Kind kind = Kind::Call;
    std::uint32_t value = 0;
    std::uint32_t flags = 0;
    std::string text;
    GuestCall args;
    std::promise<Response> done;
};

using Invoke = std::function<std::optional<GuestResult>(std::uint32_t function, const GuestCall& args)>;

std::string exit_message(int status) {
    if (status == ZB_HOST_EXIT_PRELOAD) return "zbhost could not preload a library (status 4)";
    return "zbhost exited with status " + std::to_string(status);
}

// A carrier guest pthread inside its PARK host call. Guarded by Impl::mutex. Only a carrier
// that is inside PARK is listed as available, so a lease never starts while the carrier runs
// guest code; a leased carrier does not leave PARK until it is released.
struct ParkedCarrier {
    enum class State { Available, Leased, Released };
    GuestThread* thread = nullptr;
    State state = State::Available;
    bool listed = false;
};

}  // namespace

struct LibraryRuntime::Impl {
    Process process;
    Process::HostCallHandler chained;
    std::thread runner;
    mutable std::mutex mutex;
    std::condition_variable cv;
    std::condition_variable carrier_cv;
    std::deque<std::shared_ptr<Command>> commands;
    std::deque<std::shared_ptr<ParkedCarrier>> available;
    std::unordered_map<GuestThread*, std::shared_ptr<ParkedCarrier>> carriers;
    zb_service_api api{};
    GuestThread* service = nullptr;
    std::thread::id service_id;
    bool started = false;
    bool ready = false;
    bool finished = false;
    std::string startup_error;

    bool validate_api(std::uint32_t address, std::string& error) {
        const std::uint8_t* source = process.memory().host_ptr(address, sizeof api, kPageRead);
        if (source == nullptr) {
            error = "zbhost passed an unreadable service API";
            return false;
        }
        zb_service_api candidate;
        std::memcpy(&candidate, source, sizeof candidate);
        if (candidate.size != sizeof candidate || candidate.version != ZB_SERVICE_PROTOCOL_VERSION) {
            error = "zbhost service protocol mismatch";
            return false;
        }
        if (candidate.dlopen_fn == 0 || candidate.dlsym_fn == 0 || candidate.dlerror_fn == 0 ||
            candidate.spawn_carrier_fn == 0 || candidate.malloc_fn == 0 || candidate.free_fn == 0 ||
            candidate.scratch_size == 0 || candidate.scratch_size > ZB_SERVICE_SCRATCH_SIZE ||
            process.memory().host_ptr(candidate.scratch, candidate.scratch_size, kPageWrite) == nullptr) {
            error = "zbhost passed an invalid service API";
            return false;
        }
        api = candidate;
        return true;
    }

    bool copy_text(std::uint32_t buffer, std::uint32_t size, const std::string& text, std::string& error) {
        if (text.find('\0') != std::string::npos || text.size() + 1 > size) {
            error = "guest loader string does not fit its scratch buffer";
            return false;
        }
        std::uint8_t* destination = process.memory().host_ptr(buffer, text.size() + 1, kPageWrite);
        if (destination == nullptr) {
            error = "guest loader scratch buffer is not writable";
            return false;
        }
        std::memcpy(destination, text.c_str(), text.size() + 1);
        return true;
    }

    std::string read_text(std::uint32_t address) {
        if (address == 0) return "guest dlerror returned null";
        std::string text;
        for (std::uint32_t i = 0; i < ZB_SERVICE_SCRATCH_SIZE; ++i) {
            const std::uint8_t* byte = process.memory().host_ptr(address + i, 1, kPageRead);
            if (byte == nullptr) return "guest dlerror returned an unreadable string";
            if (*byte == 0) return text;
            text.push_back(static_cast<char>(*byte));
        }
        return "guest dlerror string is not terminated";
    }

    // dlerror is per guest thread: read it through the same invoke as the failed call.
    std::string last_dlerror(const Invoke& invoke) {
        const auto result = invoke(api.dlerror_fn, GuestCall{});
        return result ? read_text(result->r0) : "guest dlerror call failed";
    }

    std::uint32_t load(const Invoke& invoke, std::uint32_t buffer, std::uint32_t size, const std::string& path,
                       std::uint32_t guest_flags, std::string& error) {
        if (!copy_text(buffer, size, path, error)) return 0;
        GuestCall args;
        args.regs = {buffer, guest_flags, 0, 0};
        const auto result = invoke(api.dlopen_fn, args);
        if (!result) {
            error = "guest dlopen call failed";
            return 0;
        }
        if (result->r0 == 0) error = last_dlerror(invoke);
        return result->r0;
    }

    std::uint32_t symbol(const Invoke& invoke, std::uint32_t buffer, std::uint32_t size, std::uint32_t handle,
                         const std::string& name, std::string& error) {
        if (!copy_text(buffer, size, name, error)) return 0;
        GuestCall args;
        args.regs = {handle, buffer, 0, 0};
        const auto result = invoke(api.dlsym_fn, args);
        if (!result) {
            error = "guest dlsym call failed";
            return 0;
        }
        if (result->r0 == 0) error = last_dlerror(invoke);
        return result->r0;
    }

    Response execute(GuestThread& thread, const Command& command) {
        const Invoke invoke = [&](std::uint32_t function, const GuestCall& args) {
            return process.call_guest(thread, function, args);
        };
        Response response;
        switch (command.kind) {
        case Command::Kind::Load:
            response.result = GuestResult{
                load(invoke, api.scratch, api.scratch_size, command.text, command.flags, response.error), 0};
            response.ok = response.result->r0 != 0;
            break;
        case Command::Kind::Symbol:
            response.result = GuestResult{
                symbol(invoke, api.scratch, api.scratch_size, command.value, command.text, response.error), 0};
            response.ok = response.result->r0 != 0;
            break;
        case Command::Kind::Call:
            response.result = invoke(command.value, command.args);
            response.ok = response.result.has_value();
            if (!response.ok) response.error = "guest service call failed";
            break;
        case Command::Kind::SpawnCarrier:
            // The carrier JIT only runs bionic thread start-up, parking and exit.
            thread.child_code_cache_size = kCarrierCodeCacheSize;
            response.result = invoke(api.spawn_carrier_fn, GuestCall{});
            thread.child_code_cache_size = 0;
            if (!response.result) {
                response.error = "guest pthread_create call failed";
            } else if (response.result->r0 != 0) {
                response.error = "guest pthread_create returned " + std::to_string(response.result->r0);
            } else {
                response.ok = true;
            }
            break;
        }
        return response;
    }

    // Serves commands until a signal must be delivered (r0 = AGAIN) or the guest is exiting.
    void serve(GuestThread& thread) {
        for (;;) {
            const std::uint32_t token = thread.park_token();
            if (thread.has_pending_signals(thread.sigmask)) {
                thread.regs()[0] = ZB_SERVICE_AGAIN;
                return;
            }
            std::shared_ptr<Command> command;
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (!commands.empty()) {
                    command = commands.front();
                    commands.pop_front();
                }
            }
            if (!command) {
                thread.park(token);
                continue;
            }
            command->done.set_value(execute(thread, *command));
            if (process.exiting()) {
                thread.regs()[0] = 1;
                return;
            }
        }
    }

    bool handle_ready(GuestThread& thread) {
        {
            std::unique_lock<std::mutex> lock(mutex);
            if (!ready) {
                std::string error;
                if (!validate_api(thread.regs()[0], error)) {
                    startup_error = std::move(error);
                    lock.unlock();
                    cv.notify_all();
                    thread.regs()[0] = 1;
                    return true;
                }
                service = &thread;
                service_id = std::this_thread::get_id();
                ready = true;
                lock.unlock();
                cv.notify_all();
            }
        }
        serve(thread);
        return true;
    }

    // PARK: publish this carrier, deliver its signals while it is unleased, and return 0 once a
    // borrower has released it.
    bool park_carrier(GuestThread& thread) {
        std::shared_ptr<ParkedCarrier> record;
        {
            std::lock_guard<std::mutex> lock(mutex);
            std::shared_ptr<ParkedCarrier>& slot = carriers[&thread];
            if (!slot) {
                slot = std::make_shared<ParkedCarrier>();
                slot->thread = &thread;
            }
            record = slot;
        }
        bool detached = false;
        for (;;) {
            const std::uint32_t token = thread.park_token();
            bool published = false;
            bool leased = false;
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (record->state == ParkedCarrier::State::Released) {
                    carriers.erase(&thread);
                    break;
                }
                if (record->state == ParkedCarrier::State::Available) {
                    if (thread.has_pending_signals(thread.sigmask)) {
                        if (record->listed) {
                            std::erase(available, record);
                            record->listed = false;
                        }
                        thread.regs()[0] = ZB_SERVICE_AGAIN;
                        return true;
                    }
                    if (!record->listed) {
                        available.push_back(record);
                        record->listed = true;
                        published = true;
                    }
                } else {
                    leased = true;
                }
            }
            if (published) carrier_cv.notify_all();
            if (leased && !detached) {
                // Host signals landing on this host thread go to the process signal target
                // while the carrier's identity runs on the borrower.
                Process::set_current_thread(nullptr);
                detached = true;
            }
            thread.park(token);
        }
        if (detached) Process::set_current_thread(&thread);
        thread.regs()[0] = 0;
        return true;
    }

    bool handle_host_call(std::uint32_t index, GuestThread& thread) {
        if (index >= ZB_RUNTIME_HOST_CALL_FIRST && index <= ZB_RUNTIME_HOST_CALL_LAST) {
            if (index == ZB_SERVICE_READY_INDEX) return handle_ready(thread);
            if (index == ZB_CARRIER_PARK_INDEX) return park_carrier(thread);
            return false;
        }
        return chained && chained(index, thread);
    }

    Response submit(std::shared_ptr<Command> command) {
        std::future<Response> future = command->done.get_future();
        GuestThread* target = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (!ready || finished) return {false, std::nullopt, "guest library runtime is not running"};
            if (std::this_thread::get_id() == service_id) {
                return {false, std::nullopt, "guest service request made on the service thread; use call_on_current"};
            }
            commands.push_back(std::move(command));
            target = service;
        }
        target->wake();
        return future.get();
    }
};

struct LibraryRuntime::Carrier::State {
    Impl* impl = nullptr;
    std::shared_ptr<ParkedCarrier> parked;
    std::unique_ptr<GuestThread> borrower;
    std::thread::id owner;
    std::uint32_t scratch = 0;
};

LibraryRuntime::LibraryRuntime() : impl_(std::make_unique<Impl>()) {}

LibraryRuntime::~LibraryRuntime() {
    if (!impl_->runner.joinable()) return;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (impl_->ready || !impl_->finished) {
            log("LibraryRuntime destroyed after start; the runtime is process-lifetime");
            std::abort();
        }
    }
    impl_->runner.join();
}

void LibraryRuntime::set_host_call_handler(Process::HostCallHandler handler) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->started) {
        log("LibraryRuntime::set_host_call_handler called after start");
        std::abort();
    }
    impl_->chained = std::move(handler);
}

bool LibraryRuntime::start(const LibraryRuntimeOptions& options, std::string& error) {
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (impl_->started) {
            error = "guest library runtime was already started";
            return false;
        }
        impl_->started = true;
    }
    Impl* impl = impl_.get();
    impl->process.set_sysroot(options.sysroot);
    impl->process.set_host_call_handler(
        [impl](std::uint32_t index, GuestThread& thread) { return impl->handle_host_call(index, thread); });
    std::vector<std::string> argv = {options.zbhost, std::to_string(options.target_sdk)};
    if (!options.preload.empty()) argv.push_back(options.preload);
    impl->runner = std::thread([impl, argv, options] {
        const int status = impl->process.run(options.zbhost, argv, options.guest_environment);
        runtime_report().note_guest_exit(exit_message(status));
        std::deque<std::shared_ptr<Command>> pending;
        {
            std::lock_guard<std::mutex> lock(impl->mutex);
            impl->finished = true;
            if (!impl->ready && impl->startup_error.empty()) impl->startup_error = exit_message(status);
            pending.swap(impl->commands);
        }
        impl->cv.notify_all();
        impl->carrier_cv.notify_all();
        for (const auto& command : pending) command->done.set_value({false, std::nullopt, exit_message(status)});
    });

    std::unique_lock<std::mutex> lock(impl->mutex);
    const bool settled = impl->cv.wait_for(lock, options.ready_timeout, [impl] {
        return impl->ready || impl->finished || !impl->startup_error.empty();
    });
    if (impl->ready) return true;
    error = settled ? impl->startup_error
                    : "zbhost did not report ready within " + std::to_string(options.ready_timeout.count()) + " ms";
    return false;
}

std::uint32_t LibraryRuntime::load_library(const std::string& path, std::uint32_t guest_flags, std::string& error) {
    auto command = std::make_shared<Command>();
    command->kind = Command::Kind::Load;
    command->text = path;
    command->flags = guest_flags;
    Response response = impl_->submit(std::move(command));
    if (!response.ok) {
        error = std::move(response.error);
        return 0;
    }
    return response.result->r0;
}

std::uint32_t LibraryRuntime::find_symbol(std::uint32_t handle, const std::string& name, std::string& error) {
    auto command = std::make_shared<Command>();
    command->kind = Command::Kind::Symbol;
    command->value = handle;
    command->text = name;
    Response response = impl_->submit(std::move(command));
    if (!response.ok) {
        error = std::move(response.error);
        return 0;
    }
    return response.result->r0;
}

std::optional<GuestResult> LibraryRuntime::call_on_service(std::uint32_t function, const GuestCall& args) {
    auto command = std::make_shared<Command>();
    command->kind = Command::Kind::Call;
    command->value = function;
    command->args = args;
    Response response = impl_->submit(std::move(command));
    return response.ok ? response.result : std::nullopt;
}

std::optional<GuestResult> LibraryRuntime::call_on_current(std::uint32_t function, const GuestCall& args) {
    GuestThread* thread = Process::current_thread();
    if (thread == nullptr) return std::nullopt;
    return impl_->process.call_guest(*thread, function, args);
}

std::unique_ptr<LibraryRuntime::Carrier> LibraryRuntime::borrow(std::string& error) {
    if (Process::current_thread() != nullptr) {
        error = "the calling host thread already runs guest code; use call_on_current";
        return nullptr;
    }
    auto command = std::make_shared<Command>();
    command->kind = Command::Kind::SpawnCarrier;
    Response response = impl_->submit(std::move(command));
    if (!response.ok) {
        error = std::move(response.error);
        return nullptr;
    }

    std::shared_ptr<ParkedCarrier> record;
    {
        std::unique_lock<std::mutex> lock(impl_->mutex);
        impl_->carrier_cv.wait_for(lock, kCarrierParkTimeout,
                                   [&] { return !impl_->available.empty() || impl_->finished; });
        if (impl_->available.empty()) {
            error = impl_->finished ? "zbhost exited" : "guest carrier did not park within 10000 ms";
            return nullptr;
        }
        record = impl_->available.front();
        impl_->available.pop_front();
        record->listed = false;
        record->state = ParkedCarrier::State::Leased;
        record->thread->wake();
    }

    std::unique_ptr<GuestThread> borrower = impl_->process.create_borrower(*record->thread);
    if (!borrower) {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        record->state = ParkedCarrier::State::Released;
        record->thread->wake();
        error = "no Dynarmic processor id is available for a borrower";
        return nullptr;
    }
    auto state = std::make_unique<Carrier::State>();
    state->impl = impl_.get();
    state->parked = std::move(record);
    state->borrower = std::move(borrower);
    state->owner = std::this_thread::get_id();
    return std::unique_ptr<Carrier>(new Carrier(std::move(state)));
}

GuestMemory& LibraryRuntime::memory() {
    return impl_->process.memory();
}

std::vector<std::string> LibraryRuntime::mapped_file_paths() const {
    return impl_->process.mapped_file_paths();
}

void LibraryRuntime::add_path_alias(std::string guest_prefix, std::string host_prefix) {
    impl_->process.add_path_alias(std::move(guest_prefix), std::move(host_prefix));
}

const zb_service_api& LibraryRuntime::service_api() const {
    return impl_->api;
}

std::size_t LibraryRuntime::guest_thread_count() const {
    return impl_->process.thread_count();
}

bool LibraryRuntime::is_borrower(const GuestThread& thread) const {
    return impl_->process.is_borrower(thread);
}

LibraryRuntime::Carrier::Carrier(std::unique_ptr<State> state) : state_(std::move(state)) {}

LibraryRuntime::Carrier::~Carrier() {
    if (state_->owner != std::this_thread::get_id()) {
        log("carrier lease released on a different host thread");
        std::abort();
    }
    Impl* impl = state_->impl;
    if (state_->scratch != 0) {
        GuestCall args;
        args.regs = {state_->scratch, 0, 0, 0};
        (void)call(impl->api.free_fn, args);
    }
    GuestThread& carrier = *state_->parked->thread;
    impl->process.destroy_borrower(std::move(state_->borrower), carrier);
    // Wake under the lock: once the carrier sees Released it may exit and free its GuestThread.
    std::lock_guard<std::mutex> lock(impl->mutex);
    state_->parked->state = ParkedCarrier::State::Released;
    carrier.wake();
}

std::optional<GuestResult> LibraryRuntime::Carrier::call(std::uint32_t function, const GuestCall& args) {
    if (state_->owner != std::this_thread::get_id()) {
        log("carrier lease used on a different host thread");
        std::abort();
    }
    GuestThread* borrower = state_->borrower.get();
    GuestThread* previous = Process::current_thread();
    if (previous != nullptr && previous != borrower) {
        log("carrier lease used while the host thread runs other guest code");
        return std::nullopt;
    }
    Process::set_current_thread(borrower);
    auto result = state_->impl->process.call_guest(*borrower, function, args);
    Process::set_current_thread(previous);
    return result;
}

bool LibraryRuntime::Carrier::ensure_scratch(std::string& error) {
    if (state_->scratch != 0) return true;
    GuestCall args;
    args.regs = {ZB_SERVICE_SCRATCH_SIZE, 0, 0, 0};
    const auto result = call(state_->impl->api.malloc_fn, args);
    if (!result || result->r0 == 0) {
        error = "guest malloc for the carrier scratch buffer failed";
        return false;
    }
    state_->scratch = result->r0;
    return true;
}

std::uint32_t LibraryRuntime::Carrier::load_library(const std::string& path, std::uint32_t guest_flags,
                                                    std::string& error) {
    if (!ensure_scratch(error)) return 0;
    const Invoke invoke = [this](std::uint32_t function, const GuestCall& args) { return call(function, args); };
    return state_->impl->load(invoke, state_->scratch, ZB_SERVICE_SCRATCH_SIZE, path, guest_flags, error);
}

std::uint32_t LibraryRuntime::Carrier::find_symbol(std::uint32_t handle, const std::string& name,
                                                   std::string& error) {
    if (!ensure_scratch(error)) return 0;
    const Invoke invoke = [this](std::uint32_t function, const GuestCall& args) { return call(function, args); };
    return state_->impl->symbol(invoke, state_->scratch, ZB_SERVICE_SCRATCH_SIZE, handle, name, error);
}

std::int32_t LibraryRuntime::Carrier::guest_tid() const {
    return state_->borrower->tid;
}

std::uint32_t LibraryRuntime::Carrier::guest_tls() const {
    return state_->borrower->tls();
}

}  // namespace zb
