// HostJni core: API registration, per-thread JNI state, handle and id resolution, guest memory
// access, native calls, and the host-call switch.
#include "host_jni_internal.h"

#include <atomic>
#include <string>

#include <array>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "zb/log.h"
#include "zb/process.h"
#include "zb/runtime_report.h"

namespace zb {

namespace {

struct JniHostCallName {
    std::uint32_t index;
    const char* name;
};

constexpr JniHostCallName kJniHostCallNames[] = {
#include "gen/jni_hostcalls.inc"
};

// Longest guest string read by a host call (class names, signatures, UTF text).
constexpr std::uint64_t kMaxGuestString = 64u << 20;

thread_local JniThread t_thread;

class ThreadEnvScope {
public:
    ThreadEnvScope(JniThread& state, JniBackend::Env env, std::string& error)
        : state_(state), previous_(state.env) {
        if (env == 0) {
            error = "JNI loader received a null JNIEnv";
            return;
        }
        if (previous_ != 0 && previous_ != env) {
            error = "JNI loader received a JNIEnv from another call";
            return;
        }
        state_.env = env;
        active_ = true;
    }

    ~ThreadEnvScope() {
        if (active_) state_.env = previous_;
    }

    bool active() const { return active_; }

private:
    JniThread& state_;
    JniBackend::Env previous_;
    bool active_ = false;
};

std::string loader_error(HostJni::Impl& jni, JniThread& state) {
    const auto result = jni.invoke(state, jni.runtime.service_api().dlerror_fn, GuestCall{});
    if (!result || result->r0 == 0) return "guest dlerror returned no message";
    std::string text;
    for (std::uint64_t i = 0; i < ZB_SERVICE_SCRATCH_SIZE; ++i) {
        const std::uint64_t address = static_cast<std::uint64_t>(result->r0) + i;
        if (address >= kGuestSpaceSize) return "guest dlerror returned an unreadable string";
        const std::uint8_t* byte =
            jni.runtime.memory().host_ptr(static_cast<std::uint32_t>(address), 1, kPageRead);
        if (byte == nullptr) return "guest dlerror returned an unreadable string";
        if (*byte == 0) return text;
        text.push_back(static_cast<char>(*byte));
    }
    return "guest dlerror string is not terminated";
}

std::uint32_t loader_operation(HostJni::Impl& jni, JniBackend::Env env, bool symbol,
                               std::uint32_t value, const std::string& text, std::string& error) {
    error.clear();
    if (!jni.ready.load()) {
        error = "JNI bridge is not ready";
        return 0;
    }
    if (text.find('\0') != std::string::npos || text.size() >= ZB_SERVICE_SCRATCH_SIZE) {
        error = "guest loader string does not fit the bounded buffer";
        return 0;
    }
    JniThread& state = jni.thread();
    ThreadEnvScope env_scope(state, env, error);
    if (!env_scope.active()) return 0;

    GuestCall allocate;
    allocate.regs = {static_cast<std::uint32_t>(text.size() + 1), 0, 0, 0};
    const auto allocated = jni.invoke(state, jni.runtime.service_api().malloc_fn, allocate);
    if (!allocated || allocated->r0 == 0) {
        error = "guest malloc for the loader string failed";
        return 0;
    }
    const std::uint32_t scratch = allocated->r0;
    std::uint8_t* destination = jni.runtime.memory().host_ptr(scratch, text.size() + 1, kPageWrite);
    if (destination == nullptr) {
        error = "guest loader string buffer is not writable";
    } else {
        std::memcpy(destination, text.c_str(), text.size() + 1);
    }

    std::optional<GuestResult> result;
    if (destination != nullptr) {
        GuestCall call;
        call.regs = symbol ? std::array<std::uint32_t, 4>{value, scratch, 0, 0}
                           : std::array<std::uint32_t, 4>{scratch, value, 0, 0};
        const std::uint32_t function = symbol ? jni.runtime.service_api().dlsym_fn
                                              : jni.runtime.service_api().dlopen_fn;
        result = jni.invoke(state, function, call);
        if (!result) {
            error = symbol ? "guest dlsym call failed" : "guest dlopen call failed";
        } else if (result->r0 == 0) {
            error = loader_error(jni, state);
        }
    }

    GuestCall release;
    release.regs = {scratch, 0, 0, 0};
    (void)jni.invoke(state, jni.runtime.service_api().free_fn, release);
    return result ? result->r0 : 0;
}

}  // namespace

const char* jni_host_call_name(std::uint32_t index) {
    for (const auto& entry : kJniHostCallNames) {
        if (entry.index == index) return entry.name;
    }
    return "?";
}

JniThread::~JniThread() {
    // A Java thread that ran guest natives frees its guest JNIEnv on its carrier before the lease
    // ends. Guest threads free theirs in DetachCurrentThread.
    if (owner != nullptr && carrier && guest_env != 0) {
        GuestCall args;
        args.regs = {guest_env, 0, 0, 0};
        (void)carrier->call(owner->api.free_env_fn, args);
        guest_env = 0;
    }
}

std::uint32_t JniCall::arg(unsigned position) const {
    if (position < 4) return regs_[position];
    const std::uint32_t address = thread_.regs()[13] + 4 * (position - 4);
    const std::uint8_t* word = jni_.runtime.memory().host_ptr(address, 4, kPageRead);
    if (word == nullptr) {
        jni_.fatal(state_.env, "JNI %s: argument %u is not on a readable guest stack", jni_host_call_name(index_),
                   position);
    }
    std::uint32_t value;
    std::memcpy(&value, word, sizeof value);
    return value;
}

JniBackend::Env JniCall::env() {
    if (state_.env == 0) {
        log("JNI %s called on a thread without a JNIEnv", jni_host_call_name(index_));
        std::abort();
    }
    return state_.env;
}

JniThread& HostJni::Impl::thread() {
    JniThread& state = t_thread;
    state.owner = this;
    return state;
}

void HostJni::Impl::fatal(JniBackend::Env env, const char* fmt, ...) {
    char text[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    log("JNI fatal error: %s", text);
    if (env != 0) backend.fatal_error(env, text);
    std::abort();
}

JniBackend::Ref HostJni::Impl::resolve(JniThread& state, std::uint32_t handle, const char* function) {
    if (handle == 0) return 0;
    std::optional<std::uint64_t> ref;
    switch (handle & 3u) {
    case static_cast<std::uint32_t>(HandleKind::Local):
        ref = state.locals.get(handle);
        break;
    case static_cast<std::uint32_t>(HandleKind::Global):
        ref = globals.get(handle);
        break;
    case static_cast<std::uint32_t>(HandleKind::WeakGlobal):
        ref = weaks.get(handle);
        break;
    default:
        break;
    }
    if (!ref) fatal(state.env, "%s: invalid JNI reference 0x%08x", function, handle);
    return *ref;
}

std::uint32_t HostJni::Impl::intern_method(JniBackend::Id id, const std::string& shorty) {
    const std::uint32_t guest = methods.intern(id);
    if (guest == 0) return 0;
    std::lock_guard<std::mutex> lock(shorty_mutex);
    if (shorties.size() < guest) shorties.resize(guest);
    shorties[guest - 1] = shorty;
    return guest;
}

std::optional<std::string> HostJni::Impl::method_shorty(std::uint32_t id) {
    std::lock_guard<std::mutex> lock(shorty_mutex);
    if (id == 0 || id > shorties.size() || shorties[id - 1].empty()) return std::nullopt;
    return shorties[id - 1];
}

JniBackend::Id HostJni::Impl::method_id(JniBackend::Env env, std::uint32_t id, const char* function) {
    const std::optional<std::uint64_t> host = methods.get(id);
    if (id == 0 || !host) fatal(env, "%s: invalid jmethodID 0x%08x", function, id);
    return *host;
}

JniBackend::Id HostJni::Impl::field_id(JniBackend::Env env, std::uint32_t id, const char* function) {
    const std::optional<std::uint64_t> host = fields.get(id);
    if (id == 0 || !host) fatal(env, "%s: invalid jfieldID 0x%08x", function, id);
    return *host;
}

std::string HostJni::Impl::read_string(JniBackend::Env env, std::uint32_t address, const char* function) {
    std::string text;
    std::uint64_t cursor = address;
    for (;;) {
        // Read up to the end of the current page, which is mapped as a whole or not at all.
        const std::uint64_t chunk = kPageSize - (cursor & kPageMask);
        if (cursor >= kGuestSpaceSize || text.size() > kMaxGuestString) {
            fatal(env, "%s: unterminated guest string at 0x%08x", function, address);
        }
        const std::uint8_t* bytes =
            runtime.memory().host_ptr(static_cast<std::uint32_t>(cursor), chunk, kPageRead);
        if (bytes == nullptr) fatal(env, "%s: unreadable guest string at 0x%08x", function, address);
        const auto* end = static_cast<const std::uint8_t*>(std::memchr(bytes, 0, chunk));
        if (end != nullptr) {
            text.append(reinterpret_cast<const char*>(bytes), static_cast<std::size_t>(end - bytes));
            return text;
        }
        text.append(reinterpret_cast<const char*>(bytes), static_cast<std::size_t>(chunk));
        cursor += chunk;
    }
}

const std::uint8_t* HostJni::Impl::readable(JniBackend::Env env, std::uint32_t address, std::uint64_t size,
                                            const char* function) {
    const std::uint8_t* bytes = runtime.memory().host_ptr(address, size, kPageRead);
    if (bytes == nullptr || static_cast<std::uint64_t>(address) + size > kGuestSpaceSize) {
        fatal(env, "%s: unreadable guest buffer 0x%08x (%llu bytes)", function, address,
              static_cast<unsigned long long>(size));
    }
    return bytes;
}

std::uint8_t* HostJni::Impl::writable(JniBackend::Env env, std::uint32_t address, std::uint64_t size,
                                      const char* function) {
    std::uint8_t* bytes = runtime.memory().host_ptr(address, size, kPageWrite);
    if (bytes == nullptr || static_cast<std::uint64_t>(address) + size > kGuestSpaceSize) {
        fatal(env, "%s: unwritable guest buffer 0x%08x (%llu bytes)", function, address,
              static_cast<unsigned long long>(size));
    }
    return bytes;
}

void HostJni::Impl::write_shorty(JniBackend::Env env, std::uint32_t address, const std::string& shorty,
                                 const char* function) {
    if (shorty.size() + 1 > ZB_JNI_SHORTY_SIZE) fatal(env, "%s: shorty longer than 256 letters", function);
    std::memcpy(writable(env, address, shorty.size() + 1, function), shorty.c_str(), shorty.size() + 1);
}

std::optional<GuestResult> HostJni::Impl::invoke(JniThread& state, std::uint32_t function, const GuestCall& args) {
    if (Process::current_thread() != nullptr) return runtime.call_on_current(function, args);
    if (!state.carrier) {
        std::string error;
        state.carrier = runtime.borrow(error);
        if (!state.carrier) {
            log("JNI: cannot borrow a guest carrier: %s", error.c_str());
            return std::nullopt;
        }
    }
    return state.carrier->call(function, args);
}

bool HostJni::Impl::ensure_guest_env(JniThread& state) {
    if (state.guest_env != 0) return true;
    const std::optional<GuestResult> env = invoke(state, api.new_env_fn, GuestCall{});
    if (!env || env->r0 == 0) {
        log("JNI: guest JNIEnv allocation failed");
        return false;
    }
    state.guest_env = env->r0;
    return true;
}

std::vector<std::string> HostJni::mapped_file_paths() const {
    return impl_->runtime.mapped_file_paths();
}

HostJni::HostJni(LibraryRuntime& runtime, JniBackend& backend, std::size_t slot_capacity)
    : impl_(std::make_unique<Impl>(runtime, backend, slot_capacity)) {
    impl_->owner = this;
    install_native_dispatcher(impl_.get());
}

HostJni::~HostJni() {
    log("HostJni destroyed; the JNI bridge is process-lifetime");
    std::abort();
}

std::optional<GuestResult> HostJni::call_on_host_thread(std::uint32_t function, const GuestCall& args) {
    return impl_->invoke(t_thread, function, args);
}

bool HostJni::ready() const {
    return impl_->ready.load();
}

std::uint32_t HostJni::guest_java_vm() const {
    return impl_->api.java_vm;
}

JniBackend::Env HostJni::current_env() {
    return impl_->thread().env;
}

JniBackend::Ref HostJni::resolve_ref(std::uint32_t handle, const char* function) {
    return impl_->resolve(impl_->thread(), handle, function);
}

std::uint32_t HostJni::new_global_handle(JniBackend::Ref ref) {
    if (ref == 0) return 0;
    return impl_->globals.add(ref);
}

std::uint32_t HostJni::new_local_handle(JniBackend::Ref ref) {
    if (ref == 0) return 0;
    return impl_->local(impl_->thread(), ref);
}

std::uint32_t HostJni::load_library_on_current(JniBackend::Env env, const std::string& path,
                                               std::uint32_t guest_flags, std::string& error) {
    return loader_operation(*impl_, env, false, guest_flags, path, error);
}

std::uint32_t HostJni::find_symbol_on_current(JniBackend::Env env, std::uint32_t handle,
                                              const std::string& name, std::string& error) {
    return loader_operation(*impl_, env, true, handle, name, error);
}

namespace {

// The last JNI host calls, for a crash report. A guest that dies inside its own code usually died
// because of what the previous call answered, and there is no logcat on the device.
constexpr std::size_t kRecentJniCalls = 24;
std::atomic<std::uint32_t> g_recent[kRecentJniCalls];
std::atomic<std::uint64_t> g_recent_next{0};

void note_recent(std::uint32_t index) {
    const std::uint64_t slot = g_recent_next.fetch_add(1, std::memory_order_relaxed);
    g_recent[slot % kRecentJniCalls].store(index, std::memory_order_relaxed);
}

}  // namespace

std::string jni_recent_calls() {
    const std::uint64_t next = g_recent_next.load(std::memory_order_relaxed);
    if (next == 0) return "(none)";
    const std::uint64_t first = next > kRecentJniCalls ? next - kRecentJniCalls : 0;
    std::string out;
    for (std::uint64_t i = first; i < next; ++i) {
        const std::uint32_t index = g_recent[i % kRecentJniCalls].load(std::memory_order_relaxed);
        if (!out.empty()) out += " ";
        out += jni_host_call_name(index);
    }
    return out;
}

bool HostJni::handle_host_call(std::uint32_t index, GuestThread& thread) {
    if (index < ZB_JNI_SLOT_STUB_FIRST || index > ZB_JNI_HOST_CALL_LAST) return false;
    note_recent(index);
    Impl& jni = *impl_;
    if (index == ZB_JNI_HC_Register) {
        zb_jni_guest_api api{};
        const std::uint8_t* source = jni.runtime.memory().host_ptr(thread.regs()[0], sizeof api, kPageRead);
        if (source != nullptr) std::memcpy(&api, source, sizeof api);
        const bool valid = source != nullptr && api.size == sizeof api && api.version == ZB_JNI_PROTOCOL_VERSION &&
                           api.new_env_fn != 0 && api.free_env_fn != 0 && api.java_vm != 0;
        if (valid && !jni.ready.load()) {
            jni.api = api;
            jni.ready.store(true);
        } else if (!valid) {
            log("libzbjni.so registered an invalid or mismatched API");
        }
        thread.regs()[0] = valid ? 1 : 0;
        return true;
    }
    if (!jni.ready.load()) {
        log("JNI host call %s before libzbjni.so registered", jni_host_call_name(index));
        std::abort();
    }
    JniCall call(jni, thread, jni.thread(), index);
    if (jni.serve_vm(call) || jni.serve_objects(call) || jni.serve_values(call) || jni.serve_data(call) ||
        jni.serve_natives(call)) {
        return true;
    }
    log("JNI host call 0x%x (%s) is not implemented", index, jni_host_call_name(index));
    std::abort();
}

std::optional<HostJni::NativeResult> HostJni::call_native(JniBackend::Env env, char return_type,
                                                          std::uint32_t function, const BuildCall& build,
                                                          NativeCallCounter* call_counter) {
    // The single funnel for every Java -> guest native invocation: one relaxed atomic add per
    // call, no lock, no string work. call_counter is null for calls that are not dispatching a
    // bound native method (the loader's own JNI_OnLoad call), which are not counted.
    if (call_counter != nullptr) call_counter->count.fetch_add(1, std::memory_order_relaxed);
    Impl& jni = *impl_;
    JniThread& state = jni.thread();
    const JniBackend::Env outer_env = state.env;
    if (outer_env != 0 && outer_env != env) {
        log("JNI native call with a JNIEnv that differs from this thread's JNIEnv");
        std::abort();
    }
    if (!jni.ready.load() || !jni.ensure_guest_env(state)) return std::nullopt;
    // The backend frame frees every host local created during the call in one step and keeps
    // the caller's (argument) references untouched.
    if (jni.backend.push_local_frame(env, 16) != 0) return std::nullopt;
    state.env = env;
    ++state.native_depth;
    const int outer_user_frames = state.user_frames;
    state.user_frames = 0;
    state.locals.push_frame();
    const std::size_t frames = state.locals.frame_count();

    const GuestCall args =
        build(state.guest_env, [&](std::uint64_t ref) { return state.locals.add(ref); });
    std::optional<GuestResult> guest = jni.invoke(state, function, args);

    // The one guest -> host sync point of the direct-buffer mirrors (host_jni_data.cpp): whatever
    // the guest wrote into a mirrored Java buffer becomes visible to Java here, when the native
    // method returns. A guest that writes to a mirror outside a Java -> guest native call (from a
    // guest thread of its own, say) is not seen by Java until the next native call returns, and a
    // later GetDirectBufferAddress for the same buffer overwrites those writes with Java's bytes.
    jni.flush_buffer_mirrors(env);

    std::optional<NativeResult> result;
    JniBackend::Ref kept = 0;
    if (guest) {
        result = NativeResult{*guest, 0};
        if (return_type == 'L') kept = jni.resolve(state, guest->r0, "native method result");
    }
    if (state.locals.frame_count() != frames || state.user_frames != 0) {
        log("JNI: a native method returned with %d unpopped PushLocalFrame frames", state.user_frames);
        while (state.user_frames > 0) {
            state.locals.pop_frame();
            kept = jni.backend.pop_local_frame(env, kept);
            --state.user_frames;
        }
    }
    state.locals.pop_frame();
    const JniBackend::Ref outer = jni.backend.pop_local_frame(env, kept);
    if (result && return_type == 'L') result->ref = outer;
    state.user_frames = outer_user_frames;
    --state.native_depth;
    state.env = outer_env;
    return result;
}

}  // namespace zb
