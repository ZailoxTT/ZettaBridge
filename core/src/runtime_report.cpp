#include "zb/runtime_report.h"

#include <algorithm>

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <utility>

#include "zb/log.h"

namespace zb {

namespace {

// One line per record: newlines and other control characters would break the "key: value"
// layout that makes the report diff-friendly.
std::string one_line(const std::string& text, std::size_t limit) {
    std::string out;
    out.reserve(text.size() < limit ? text.size() : limit);
    for (const char c : text) {
        if (out.size() >= limit) {
            out += "...";
            break;
        }
        out += (static_cast<unsigned char>(c) < 0x20 || c == 0x7F) ? ' ' : c;
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

std::string hex_version(std::int32_t version) {
    char text[11];
    std::snprintf(text, sizeof text, "0x%08x", static_cast<std::uint32_t>(version));
    return text;
}

void append_count(std::string& out, const char* key, std::uint64_t value) {
    out += key;
    out += ": ";
    out += std::to_string(value);
    out += '\n';
}

}  // namespace

std::shared_ptr<RuntimeReport::Observer> RuntimeReport::take_observer() const {
    return observer_;
}

void RuntimeReport::set_observer(Observer observer) {
    std::lock_guard<std::mutex> lock(mutex_);
    observer_ = observer ? std::make_shared<Observer>(std::move(observer)) : nullptr;
}

void RuntimeReport::note_plugin(const std::string& plugin_root, std::uint32_t target_sdk) {
    std::shared_ptr<Observer> observer;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        plugin_root_ = one_line(plugin_root, kMaxDetail);
        target_sdk_ = target_sdk;
        observer = take_observer();
    }
    if (observer) (*observer)(true);
}

void RuntimeReport::note_unimplemented_host_call(std::uint32_t index, const char* library, const char* function) {
    bool structural = false;
    std::shared_ptr<Observer> observer;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++host_call_total_;
        HostCall* found = nullptr;
        for (HostCall& entry : host_calls_) {
            if (entry.index == index) {
                found = &entry;
                break;
            }
        }
        if (found != nullptr) {
            ++found->count;
        } else {
            ++distinct_host_calls_;
            structural = true;
            if (host_calls_.size() < kMaxDistinctHostCalls) {
                host_calls_.push_back(HostCall{index, library != nullptr ? library : "?",
                                               function != nullptr ? function : "?", 1});
            }
        }
        observer = take_observer();
    }
    if (observer) (*observer)(structural);
}

void RuntimeReport::note_proxy_loaded(const std::string& library, std::int32_t jni_version) {
    std::shared_ptr<Observer> observer;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++proxy_load_total_;
        if (proxy_loads_.size() < kMaxLibraries) {
            proxy_loads_.push_back(Load{one_line(library, kMaxDetail), true, jni_version, {}});
        }
        observer = take_observer();
    }
    if (observer) (*observer)(true);
}

void RuntimeReport::note_proxy_failed(const std::string& library, const std::string& error) {
    std::shared_ptr<Observer> observer;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++proxy_failure_total_;
        if (proxy_loads_.size() < kMaxLibraries) {
            proxy_loads_.push_back(Load{one_line(library, kMaxDetail), false, 0, one_line(error, kMaxDetail)});
        }
        observer = take_observer();
    }
    if (observer) (*observer)(true);
}

void RuntimeReport::note_jni_onload(const std::string& library, bool ok, std::int32_t jni_version) {
    std::shared_ptr<Observer> observer;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++onload_total_;
        if (onloads_.size() < kMaxLibraries) {
            onloads_.push_back(Load{one_line(library, kMaxDetail), ok, jni_version, {}});
        }
        observer = take_observer();
    }
    if (observer) (*observer)(true);
}

void RuntimeReport::note_registered_native() {
    std::shared_ptr<Observer> observer;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++registered_natives_;
        observer = take_observer();
    }
    if (observer) (*observer)(false);
}

NativeCallCounter& RuntimeReport::native_call_counter(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::string label = one_line(name, 128);
    for (NativeCallEntry& entry : native_calls_) {
        if (entry.name == label) return *entry.counter;
    }
    if (native_calls_.size() >= kMaxNativeCalls) return native_calls_overflow_;
    native_calls_.push_back(NativeCallEntry{label, std::make_unique<NativeCallCounter>()});
    return *native_calls_.back().counter;
}

void RuntimeReport::note_guest_open(const std::string& path) {
    bool structural = false;
    std::shared_ptr<Observer> observer;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const std::string line = one_line(path, kMaxDetail);
        auto found = std::find(opened_paths_.begin(), opened_paths_.end(), line);
        if (found != opened_paths_.end()) opened_paths_.erase(found);
        else structural = true;
        opened_paths_.push_back(line);
        if (opened_paths_.size() > kMaxOpenedPaths) opened_paths_.erase(opened_paths_.begin());
        observer = take_observer();
    }
    if (observer) (*observer)(structural);
}

void RuntimeReport::note_guest_open_failed(const std::string& path, int error) {
    std::shared_ptr<Observer> observer;
    bool structural = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (failed_opens_.size() < kMaxFailedOpens) {
            failed_opens_.emplace_back(one_line(path, kMaxDetail), error);
            structural = true;
        }
        observer = take_observer();
    }
    if (observer) (*observer)(structural);
}

void RuntimeReport::note_guest_exit(const std::string& reason) {
    std::shared_ptr<Observer> observer;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!exit_reason_.empty()) return;
        exit_reason_ = one_line(reason, kMaxDetail);
        if (exit_reason_.empty()) exit_reason_ = "unknown";
        observer = take_observer();
    }
    if (observer) (*observer)(true);
}

void RuntimeReport::note_syscall_eintr(std::uint32_t nr, const char* name,
                                       std::uint64_t guest_tid, bool restartable) {
    std::shared_ptr<Observer> observer;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++signal_eintr_total_;
        ++(restartable ? signal_restartable_total_ : signal_nonrestartable_total_);
        signal_last_eintr_ = one_line(std::string(name != nullptr ? name : "?") + "(" +
                                          std::to_string(nr) + ") tid=" +
                                          std::to_string(guest_tid) + " restartable=" +
                                          (restartable ? "yes" : "no"),
                                      kMaxDetail);
        observer = take_observer();
    }
    if (observer) (*observer)(false);
}

void RuntimeReport::note_signal_restart(std::uint32_t nr, const char* name,
                                        std::uint64_t guest_tid, bool sa_restart, bool rewound) {
    std::shared_ptr<Observer> observer;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++(sa_restart ? signal_delivered_sa_restart_total_ : signal_delivered_no_restart_total_);
        if (rewound) ++signal_rewound_total_;
        signal_last_delivery_ = one_line(std::string(name != nullptr ? name : "?") + "(" +
                                             std::to_string(nr) + ") tid=" +
                                             std::to_string(guest_tid) + " sa-restart=" +
                                             (sa_restart ? "yes" : "no") + " rewound=" +
                                             (rewound ? "yes" : "no"),
                                         kMaxDetail);
        observer = take_observer();
    }
    if (observer) (*observer)(false);
}

void RuntimeReport::note_gl_call(const char* function, std::uint64_t host_tid) {
    bool structural = false;
    std::shared_ptr<Observer> observer;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++gl_call_total_;
        if (gl_call_total_ == 1) {
            gl_first_call_function_ = function != nullptr ? function : "?";
            gl_first_call_tid_ = host_tid;
            structural = true;
        }
        observer = take_observer();
    }
    if (observer) (*observer)(structural);
}

void RuntimeReport::note_gl_egl_context(bool current) {
    std::shared_ptr<Observer> observer;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (gl_egl_context_known_) return;
        gl_egl_context_known_ = true;
        gl_egl_context_current_ = current;
        observer = take_observer();
    }
    if (observer) (*observer)(true);
}

void RuntimeReport::note_gl_error(const char* function, std::uint32_t error) {
    std::shared_ptr<Observer> observer;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (gl_error_known_) return;
        gl_error_known_ = true;
        gl_error_function_ = function != nullptr ? function : "?";
        gl_error_value_ = error;
        observer = take_observer();
    }
    if (observer) (*observer)(true);
}

void RuntimeReport::note_gl_detail(const std::string& key, const std::string& value, bool overwrite) {
    bool structural = false;
    std::shared_ptr<Observer> observer;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const std::string line = one_line(value, 600);
        auto found = std::find_if(gl_details_.begin(), gl_details_.end(),
                                  [&](const auto& entry) { return entry.first == key; });
        if (found != gl_details_.end()) {
            if (!overwrite || found->second == line) return;
            found->second = line;
        } else {
            if (gl_details_.size() >= kMaxGlDetails) return;
            gl_details_.emplace_back(one_line(key, 64), line);
            structural = true;
        }
        observer = take_observer();
    }
    if (observer) (*observer)(structural);
}

void RuntimeReport::note_crash_detail(const std::string& key, const std::string& value) {
    bool structural = false;
    std::shared_ptr<Observer> observer;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const std::string line = one_line(value, 600);
        auto found = std::find_if(crash_details_.begin(), crash_details_.end(),
                                  [&](const auto& entry) { return entry.first == key; });
        if (found != crash_details_.end()) {
            if (found->second == line) return;
            found->second = line;
        } else {
            if (crash_details_.size() >= kMaxCrashDetails) return;
            crash_details_.emplace_back(one_line(key, 64), line);
            structural = true;
        }
        observer = take_observer();
    }
    if (observer) (*observer)(structural);
}

void RuntimeReport::note_jni_detail(const std::string& key, const std::string& value, bool overwrite) {
    bool structural = false;
    std::shared_ptr<Observer> observer;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const std::string line = one_line(value, 600);
        auto found = std::find_if(jni_details_.begin(), jni_details_.end(),
                                  [&](const auto& entry) { return entry.first == key; });
        if (found != jni_details_.end()) {
            if (!overwrite || found->second == line) return;
            found->second = line;
        } else {
            if (jni_details_.size() >= kMaxJniDetails) return;
            jni_details_.emplace_back(one_line(key, 64), line);
            structural = true;
        }
        observer = take_observer();
    }
    if (observer) (*observer)(structural);
}

void RuntimeReport::note_looper_detail(const std::string& key, const std::string& value, bool overwrite) {
    bool structural = false;
    std::shared_ptr<Observer> observer;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const std::string line = one_line(value, 600);
        auto found = std::find_if(looper_details_.begin(), looper_details_.end(),
                                  [&](const auto& entry) { return entry.first == key; });
        if (found != looper_details_.end()) {
            if (!overwrite || found->second == line) return;
            found->second = line;
        } else {
            if (looper_details_.size() >= kMaxLooperDetails) return;
            looper_details_.emplace_back(one_line(key, 64), line);
            structural = true;
        }
        observer = take_observer();
    }
    if (observer) (*observer)(structural);
}

void RuntimeReport::note_watch_detail(const std::string& key, const std::string& value) {
    bool structural = false;
    std::shared_ptr<Observer> observer;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const std::string line = one_line(value, 600);
        auto found = std::find_if(watch_details_.begin(), watch_details_.end(),
                                  [&](const auto& entry) { return entry.first == key; });
        if (found != watch_details_.end()) {
            if (found->second == line) return;
            found->second = line;
        } else {
            if (watch_details_.size() >= kMaxWatchDetails) return;
            watch_details_.emplace_back(one_line(key, 64), line);
            structural = true;
        }
        observer = take_observer();
    }
    if (observer) (*observer)(structural);
}

void RuntimeReport::note_egl_object(const std::string& key, const std::string& value) {
    bool structural = false;
    std::shared_ptr<Observer> observer;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const std::string line = one_line(value, 600);
        auto found = std::find_if(egl_objects_.begin(), egl_objects_.end(),
                                  [&](const auto& entry) { return entry.first == key; });
        if (found != egl_objects_.end()) {
            found->second = line;
        } else {
            if (egl_objects_.size() >= kMaxGlDetails) return;
            egl_objects_.emplace_back(one_line(key, 64), line);
            structural = true;
        }
        observer = take_observer();
    }
    if (observer) (*observer)(structural);
}

void RuntimeReport::note_egl_current(std::uint64_t host_tid) {
    egl_current_generation_.fetch_add(1, std::memory_order_relaxed);
    bool structural = false;
    std::shared_ptr<Observer> observer;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (std::find(egl_current_tids_.begin(), egl_current_tids_.end(), host_tid) == egl_current_tids_.end()) {
            if (egl_current_tids_.size() < kMaxEglThreads) egl_current_tids_.push_back(host_tid);
            structural = true;
        }
        observer = take_observer();
    }
    if (observer) (*observer)(structural);
}

void RuntimeReport::note_gl_thread(std::uint64_t host_tid) {
    bool structural = false;
    std::shared_ptr<Observer> observer;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (std::find(gl_thread_tids_.begin(), gl_thread_tids_.end(), host_tid) == gl_thread_tids_.end()) {
            if (gl_thread_tids_.size() < kMaxEglThreads) gl_thread_tids_.push_back(host_tid);
            structural = true;
        }
        observer = take_observer();
    }
    if (observer) (*observer)(structural);
}

void RuntimeReport::note_egl_swap() {
    std::shared_ptr<Observer> observer;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++egl_swap_total_;
        observer = take_observer();
    }
    if (observer) (*observer)(false);
}

void RuntimeReport::note_egl_error(const char* function, std::uint32_t error) {
    std::shared_ptr<Observer> observer;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (egl_error_known_) return;
        egl_error_known_ = true;
        egl_error_function_ = function != nullptr ? function : "?";
        egl_error_value_ = error;
        observer = take_observer();
    }
    if (observer) (*observer)(true);
}

std::uint64_t RuntimeReport::egl_current_generation() const {
    return egl_current_generation_.load(std::memory_order_relaxed);
}

std::size_t RuntimeReport::unimplemented_host_calls() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<std::size_t>(host_call_total_);
}

std::size_t RuntimeReport::proxy_loads() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return proxy_load_total_;
}

std::size_t RuntimeReport::jni_onload_calls() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return onload_total_;
}

std::size_t RuntimeReport::registered_natives() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<std::size_t>(registered_natives_);
}

std::string RuntimeReport::first_unimplemented_host_call() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (host_calls_.empty()) return {};
    return std::string(host_calls_.front().library) + " " + host_calls_.front().function;
}

std::uint64_t RuntimeReport::gl_calls() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return gl_call_total_;
}

std::string RuntimeReport::text() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string out = "zettabridge-runtime-report 1\n";
    out += "plugin: ";
    out += plugin_root_.empty() ? "(none)" : plugin_root_;
    if (!plugin_root_.empty()) out += " targetSdk " + std::to_string(target_sdk_);
    out += '\n';

    append_count(out, "proxy-loads", proxy_load_total_);
    append_count(out, "proxy-failures", proxy_failure_total_);
    for (const Load& load : proxy_loads_) {
        out += load.ok ? "proxy-loaded: " : "proxy-failed: ";
        out += load.library;
        if (load.ok) {
            out += " jni=" + hex_version(load.jni_version);
        } else {
            out += ' ';
            out += load.error;
        }
        out += '\n';
    }
    if (proxy_load_total_ + proxy_failure_total_ > proxy_loads_.size()) {
        append_count(out, "proxy-more", proxy_load_total_ + proxy_failure_total_ - proxy_loads_.size());
    }

    append_count(out, "jni-onload-calls", onload_total_);
    for (const Load& load : onloads_) {
        out += "jni-onload: ";
        out += load.library;
        out += load.ok ? " ok jni=" + hex_version(load.jni_version) : std::string(" failed");
        out += '\n';
    }
    if (onload_total_ > onloads_.size()) append_count(out, "jni-onload-more", onload_total_ - onloads_.size());

    append_count(out, "registered-natives", registered_natives_);
    append_count(out, "unimplemented-host-calls", host_call_total_);
    append_count(out, "unimplemented-distinct", distinct_host_calls_);
    out += "first-unimplemented: ";
    if (host_calls_.empty()) {
        out += "(none)";
    } else {
        out += host_calls_.front().library;
        out += ' ';
        out += host_calls_.front().function;
    }
    out += '\n';
    for (const HostCall& entry : host_calls_) {
        out += "unimplemented: ";
        out += entry.library;
        out += ' ';
        out += entry.function;
        out += " x" + std::to_string(entry.count);
        out += '\n';
    }
    if (distinct_host_calls_ > host_calls_.size()) {
        append_count(out, "unimplemented-more", distinct_host_calls_ - host_calls_.size());
    }

    out += "guest-exit: ";
    out += exit_reason_.empty() ? "(none)" : exit_reason_;
    out += '\n';

    out += "signal-restart: eintr=" + std::to_string(signal_eintr_total_) +
           " restartable=" + std::to_string(signal_restartable_total_) +
           " nonrestartable=" + std::to_string(signal_nonrestartable_total_) +
           " delivered-sa-restart=" + std::to_string(signal_delivered_sa_restart_total_) +
           " delivered-no-restart=" + std::to_string(signal_delivered_no_restart_total_) +
           " rewound=" + std::to_string(signal_rewound_total_) + '\n';
    if (!signal_last_eintr_.empty()) out += "signal-restart-last-eintr: " + signal_last_eintr_ + '\n';
    if (!signal_last_delivery_.empty()) {
        out += "signal-restart-last-delivery: " + signal_last_delivery_ + '\n';
    }

    {
        std::vector<std::pair<const std::string*, std::uint64_t>> counts;
        std::uint64_t total = 0;
        for (const NativeCallEntry& entry : native_calls_) {
            const std::uint64_t n = entry.counter->count.load(std::memory_order_relaxed);
            total += n;
            counts.emplace_back(&entry.name, n);
        }
        total += native_calls_overflow_.count.load(std::memory_order_relaxed);
        std::sort(counts.begin(), counts.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
        out += "native-calls:";
        std::size_t shown = 0;
        for (const auto& [name, count] : counts) {
            if (shown >= 8 || count == 0) break;
            out += " " + *name + "=" + std::to_string(count);
            ++shown;
        }
        if (shown == 0) out += " (none)";
        out += " total=" + std::to_string(total) + '\n';
    }

    if (opened_paths_.empty()) {
        out += "opened-1: (none)\n";
    } else {
        std::size_t n = 1;
        for (const std::string& path : opened_paths_) out += "opened-" + std::to_string(n++) + ": " + path + '\n';
    }
    {
        std::size_t n = 1;
        for (const auto& [path, error] : failed_opens_) {
            out += "open-failed-" + std::to_string(n++) + ": " + path + " errno=" + std::to_string(error) + '\n';
        }
    }

    append_count(out, "gl-calls", gl_call_total_);
    out += "gl-first-call: ";
    if (gl_call_total_ == 0) {
        out += "(none)";
    } else {
        out += gl_first_call_function_;
        out += " tid=" + std::to_string(gl_first_call_tid_);
    }
    out += '\n';
    out += "gl-egl-context-current: ";
    out += !gl_egl_context_known_ ? "(unknown)" : (gl_egl_context_current_ ? "yes" : "no");
    out += '\n';
    out += "gl-first-error: ";
    if (!gl_error_known_) {
        out += "(none)";
    } else {
        char hex[11];
        std::snprintf(hex, sizeof hex, "0x%04x", gl_error_value_);
        out += gl_error_function_ + " " + hex;
    }
    out += '\n';
    for (const auto& [key, value] : gl_details_) out += "gl-" + key + ": " + value + '\n';

    for (const auto& [key, value] : egl_objects_) out += "egl-" + key + ": " + value + '\n';
    append_count(out, "egl-swaps", egl_swap_total_);
    // A mismatch is a thread that issued a gl* call having never made an EGL context current on
    // itself. A raster thread and a resource thread each with their own (correct) context both
    // appear in egl_current_tids_, so this does not fire for that normal multi-context case.
    for (const std::uint64_t tid : gl_thread_tids_) {
        if (std::find(egl_current_tids_.begin(), egl_current_tids_.end(), tid) != egl_current_tids_.end()) continue;
        out += "egl-thread-mismatch: current=" +
               std::to_string(egl_current_tids_.empty() ? 0 : egl_current_tids_.back()) +
               " gl=" + std::to_string(tid) + '\n';
        break;
    }
    out += "egl-first-error: ";
    if (!egl_error_known_) {
        out += "(none)";
    } else {
        char hex[11];
        std::snprintf(hex, sizeof hex, "0x%04x", egl_error_value_);
        out += egl_error_function_ + " " + hex;
    }
    out += '\n';
    for (const auto& [key, value] : jni_details_) out += "jni-" + key + ": " + value + '\n';
    for (const auto& [key, value] : crash_details_) out += "crash-" + key + ": " + value + '\n';
    for (const auto& [key, value] : watch_details_) out += "watch-" + key + ": " + value + '\n';
    for (const auto& [key, value] : looper_details_) out += "looper-" + key + ": " + value + '\n';
    return out;
}

void RuntimeReport::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    observer_ = nullptr;
    plugin_root_.clear();
    target_sdk_ = 0;
    host_calls_.clear();
    distinct_host_calls_ = 0;
    host_call_total_ = 0;
    proxy_loads_.clear();
    proxy_load_total_ = 0;
    proxy_failure_total_ = 0;
    onloads_.clear();
    onload_total_ = 0;
    registered_natives_ = 0;
    exit_reason_.clear();
    signal_eintr_total_ = 0;
    signal_restartable_total_ = 0;
    signal_nonrestartable_total_ = 0;
    signal_delivered_sa_restart_total_ = 0;
    signal_delivered_no_restart_total_ = 0;
    signal_rewound_total_ = 0;
    signal_last_eintr_.clear();
    signal_last_delivery_.clear();
    native_calls_.clear();
    native_calls_overflow_.count.store(0, std::memory_order_relaxed);
    opened_paths_.clear();
    failed_opens_.clear();
    gl_call_total_ = 0;
    gl_first_call_function_.clear();
    gl_first_call_tid_ = 0;
    gl_egl_context_known_ = false;
    gl_egl_context_current_ = false;
    gl_error_known_ = false;
    gl_error_function_.clear();
    gl_error_value_ = 0;
    gl_details_.clear();
    egl_objects_.clear();
    egl_current_tids_.clear();
    egl_current_generation_.store(0, std::memory_order_relaxed);
    gl_thread_tids_.clear();
    egl_swap_total_ = 0;
    egl_error_known_ = false;
    egl_error_function_.clear();
    egl_error_value_ = 0;
    crash_details_.clear();
    jni_details_.clear();
    watch_details_.clear();
    looper_details_.clear();
}

RuntimeReport& runtime_report() {
    // Process-lifetime: notes arrive from guest threads that are never torn down, so this must
    // outlive every static destructor.
    static RuntimeReport* report = new RuntimeReport();
    return *report;
}

namespace {

// Rewrites one file from a RuntimeReport. Shared by the observer and owned by it.
class ReportWriter {
public:
    using Clock = std::chrono::steady_clock;

    ReportWriter(RuntimeReport& report, std::string path, std::chrono::milliseconds min_interval)
        : report_(report), path_(std::move(path)), temporary_(path_ + ".tmp"), min_interval_(min_interval) {}

    bool write_now() {
        const std::string text = report_.text();
        const int fd = ::open(temporary_.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (fd < 0) return false;
        std::size_t written = 0;
        while (written < text.size()) {
            const ssize_t n = ::write(fd, text.data() + written, text.size() - written);
            if (n <= 0) {
                if (errno == EINTR) continue;
                ::close(fd);
                ::unlink(temporary_.c_str());
                return false;
            }
            written += static_cast<std::size_t>(n);
        }
        ::fsync(fd);
        ::close(fd);
        if (::rename(temporary_.c_str(), path_.c_str()) != 0) {
            ::unlink(temporary_.c_str());
            return false;
        }
        return true;
    }

    void operator()(bool structural) {
        std::lock_guard<std::mutex> lock(mutex_);
        const Clock::time_point now = Clock::now();
        if (!structural && now - last_ < min_interval_) return;
        last_ = now;
        write_now();
    }

private:
    RuntimeReport& report_;
    std::string path_;
    std::string temporary_;
    std::chrono::milliseconds min_interval_;
    std::mutex mutex_;
    Clock::time_point last_ = Clock::time_point::min();
};

}  // namespace

bool write_runtime_report_to(RuntimeReport& report, const std::string& path,
                             std::chrono::milliseconds min_interval) {
    auto writer = std::make_shared<ReportWriter>(report, path, min_interval);
    if (!writer->write_now()) {
        log("cannot write the runtime report to %s: %s", path.c_str(), std::strerror(errno));
        return false;
    }
    report.set_observer([writer](bool structural) { (*writer)(structural); });
    return true;
}

}  // namespace zb
