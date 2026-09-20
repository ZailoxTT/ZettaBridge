#include "zb/jni_loader.h"

#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "zb/elf_symbols.h"
#include "zb/host_jni.h"
#include "zb/jni_mangle.h"
#include "zb/library_protocol.h"
#include "zb/log.h"
#include "zb/runtime_report.h"

namespace zb {

namespace {

constexpr std::int32_t kJniVersion11 = 0x00010001;
constexpr std::int32_t kJniVersion12 = 0x00010002;
constexpr std::int32_t kJniVersion14 = 0x00010004;
constexpr std::int32_t kJniVersion16 = 0x00010006;

bool supported_version(std::int32_t version) {
    return version == kJniVersion11 || version == kJniVersion12 || version == kJniVersion14 ||
           version == kJniVersion16;
}

std::string base_name(const std::string& path) {
    const std::size_t slash = path.rfind('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

bool matches_arguments(const std::string& signature, const std::optional<std::string>& arguments) {
    if (!arguments) return true;
    const std::size_t close = signature.find(')');
    return close != std::string::npos && signature.compare(0, close + 1, *arguments) == 0;
}

class LocalClass {
public:
    LocalClass(JniBackend& backend, JniBackend::Env env, JniBackend::Ref ref)
        : backend_(backend), env_(env), ref_(ref) {}
    ~LocalClass() {
        if (ref_ != 0) backend_.delete_local_ref(env_, ref_);
    }
    LocalClass(const LocalClass&) = delete;
    LocalClass& operator=(const LocalClass&) = delete;

private:
    JniBackend& backend_;
    JniBackend::Env env_;
    JniBackend::Ref ref_;
};

}  // namespace

void JniLoader::log_missing_class_once(const std::string& name) {
    std::lock_guard<std::mutex> lock(missing_mutex_);
    if (missing_classes_.insert(name).second) {
        log("JNI loader: class %s is not present; skipping its native exports", name.c_str());
    }
}

void JniLoader::log_unresolvable_once(const std::string& symbol, const std::string& name) {
    std::lock_guard<std::mutex> lock(missing_mutex_);
    if (unresolvable_exports_.insert(symbol).second) {
        log("JNI loader: cannot resolve the declared natives of %s; skipping export %s", name.c_str(),
            symbol.c_str());
    }
}

void JniLoader::log_unmatched_once(const std::string& symbol) {
    std::lock_guard<std::mutex> lock(missing_mutex_);
    if (unresolvable_exports_.insert(symbol).second) {
        log("JNI loader: no declared native method matches %s; skipping it", symbol.c_str());
    }
}

// Binds the Java_* exports of one library that is already loaded in the guest. JNI_OnLoad is
// not called here: a library the guest opened by itself was never handed to ART, and running its
// JNI_OnLoad a second time is not ours to decide.
bool JniLoader::bind_exports(JniBackend::Env env, const ElfSymbolReport& symbols,
                             std::uint32_t guest_handle, JniLoadReport& report, bool& has_onload) {
    for (const std::string& symbol : symbols.exports) {
        if (symbol == "JNI_OnLoad") {
            has_onload = true;
            continue;
        }
        const std::optional<JniExport> decoded = decode_jni_export(symbol);
        if (!decoded) {
            report.error = "invalid JNI export name: " + symbol;
            return false;
        }

        JniBackend::Ref cls = 0;
        std::vector<DeclaredNativeMethod> methods;
        const NativeLookupStatus lookup =
            backend_.find_declared_natives(env, decoded->class_name.c_str(), decoded->method.c_str(),
                                           decoded->arguments ? decoded->arguments->c_str() : nullptr, cls,
                                           methods);
        if (lookup == NativeLookupStatus::MissingClass) {
            log_missing_class_once(decoded->class_name);
            ++report.skipped_classes;
            continue;
        }
        // A type this export names, or a type of a sibling method the lookup had to touch, is not
        // present. That costs this one export, never the rest of the library: a guest that bundles
        // unresolvable classes next to the ones it needs must still bind the ones it needs.
        if (lookup == NativeLookupStatus::Unresolvable) {
            log_unresolvable_once(symbol, decoded->class_name);
            ++report.skipped_exports;
            continue;
        }
        if (lookup != NativeLookupStatus::Found || cls == 0) {
            report.error = "failed to inspect declared natives for " + decoded->class_name + "." + decoded->method;
            return false;
        }
        LocalClass local_class(backend_, env, cls);
        std::erase_if(methods, [&](const DeclaredNativeMethod& method) {
            return !matches_arguments(method.signature, decoded->arguments);
        });
        // An export with no matching native method is ignored, as ART does: it resolves natives
        // lazily and never looks at unused exports. Shrunk (ProGuard) apps drop native methods
        // their code never calls while the library still exports them (Flappy Bird's
        // libandengine.so exports GLES20Fix.glDrawElements).
        if (methods.empty()) {
            log_unmatched_once(symbol);
            ++report.skipped_exports;
            continue;
        }

        std::string symbol_error;
        const std::uint32_t function =
            host_jni_.find_symbol_on_current(env, guest_handle, symbol, symbol_error);
        if (function == 0) {
            report.error = "guest dlsym failed for " + symbol + ": " + symbol_error;
            return false;
        }
        for (const DeclaredNativeMethod& method : methods) {
            if (host_jni_.register_native(env, cls, decoded->method.c_str(), method.signature.c_str(), function,
                                          method.is_static, decoded->class_name.c_str()) != 0) {
                report.error = "RegisterNatives failed for " + decoded->class_name + "." + decoded->method +
                               method.signature;
                return false;
            }
            ++report.bound_methods;
        }
    }

    return true;
}

// Binds the Java_* exports of every library the guest mapped by itself. A JNI shim whose
// JNI_OnLoad dlopens the real library is a common Unity plugin shape: ART resolves a native
// method against the library Java loaded, which is the shim, and the real library's exports were
// reachable from nowhere. Nothing here can fail the load ART asked for.
void JniLoader::bind_guest_loaded_libraries(JniBackend::Env env, JniLoadReport& report) {
    for (const std::string& path : host_jni_.mapped_file_paths()) {
        if (path.size() < 4 || path.compare(path.size() - 3, 3, ".so") != 0) continue;
        {
            std::lock_guard<std::mutex> lock(missing_mutex_);
            if (!swept_libraries_.insert(path).second) continue;
        }
        const ElfSymbolReport symbols = scan_elf32_jni_exports(path);
        if (symbols.status != ElfSymbolStatus::Ok) continue;
        const bool has_natives = std::any_of(symbols.exports.begin(), symbols.exports.end(),
                                             [](const std::string& name) { return name != "JNI_OnLoad"; });
        if (!has_natives) continue;

        std::string error;
        // RTLD_NOLOAD: this must find a library the guest already has, never load a new one.
        const std::uint32_t handle = host_jni_.load_library_on_current(
            env, path, ZB_GUEST_RTLD_NOLOAD | ZB_GUEST_RTLD_NOW, error);
        if (handle == 0) continue;

        JniLoadReport swept;
        swept.guest_handle = handle;
        bool has_onload = false;
        if (!bind_exports(env, symbols, handle, swept, has_onload)) {
            log("cannot bind the exports of %s, which the guest loaded itself: %s", path.c_str(),
                swept.error.c_str());
            continue;
        }
        report.bound_methods += swept.bound_methods;
        report.skipped_classes += swept.skipped_classes;
        report.skipped_exports += swept.skipped_exports;
        if (swept.bound_methods != 0) {
            log("bound %zu natives of %s, which the guest loaded itself", swept.bound_methods, path.c_str());
        }
    }
}

JniLoadReport JniLoader::load(JniBackend::Env env, const std::string& path,
                              std::uint32_t guest_flags) {
    JniLoadReport report;
    const ElfSymbolReport symbols = scan_elf32_jni_exports(path);
    if (symbols.status != ElfSymbolStatus::Ok) {
        report.error = "cannot scan JNI exports: " + symbols.message;
        return report;
    }

    report.guest_handle = host_jni_.load_library_on_current(env, path, guest_flags, report.error);
    if (report.guest_handle == 0) {
        report.error = "guest dlopen failed: " + report.error;
        return report;
    }

    {
        std::lock_guard<std::mutex> lock(missing_mutex_);
        swept_libraries_.insert(path);  // never bind the library ART asked for twice
    }
    bool has_onload = false;
    if (!bind_exports(env, symbols, report.guest_handle, report, has_onload)) return report;

    if (has_onload) {
        const std::string library = base_name(path);
        std::string symbol_error;
        const std::uint32_t onload =
            host_jni_.find_symbol_on_current(env, report.guest_handle, "JNI_OnLoad", symbol_error);
        if (onload == 0) {
            report.error = "guest dlsym failed for JNI_OnLoad: " + symbol_error;
            return report;
        }
        const auto result = host_jni_.call_native(
            env, 'I', onload, [&](std::uint32_t, const RefToHandle&) {
                GuestCall call;
                call.regs = {host_jni_.guest_java_vm(), 0, 0, 0};
                return call;
            });
        if (!result) {
            runtime_report().note_jni_onload(library, false, 0);
            report.error = "guest JNI_OnLoad call failed";
            return report;
        }
        if (backend_.exception_check(env)) {
            runtime_report().note_jni_onload(library, false, 0);
            report.error = "guest JNI_OnLoad left a pending Java exception";
            return report;
        }
        report.jni_version = static_cast<std::int32_t>(result->guest.r0);
        if (!supported_version(report.jni_version)) {
            runtime_report().note_jni_onload(library, false, report.jni_version);
            report.error = "guest JNI_OnLoad returned unsupported JNI version 0x";
            char suffix[9];
            std::snprintf(suffix, sizeof suffix, "%08x", static_cast<std::uint32_t>(report.jni_version));
            report.error += suffix;
            return report;
        }
        runtime_report().note_jni_onload(library, true, report.jni_version);
    }

    report.ok = true;
    bind_guest_loaded_libraries(env, report);
    return report;
}

}  // namespace zb
