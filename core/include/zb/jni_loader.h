#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_set>

#include "zb/elf_symbols.h"
#include "zb/jni_backend.h"

namespace zb {

class HostJni;

struct JniLoadReport {
    bool ok = false;
    std::uint32_t guest_handle = 0;
    std::int32_t jni_version = 0x00010006;
    std::size_t bound_methods = 0;
    // Exports whose declaring class is not in the plugin at all.
    std::size_t skipped_classes = 0;
    // Exports whose Java side could not be inspected (an unresolvable type in the signature, or in
    // the class the short-form lookup had to enumerate). Each one is logged once.
    std::size_t skipped_exports = 0;
    std::string error;
};

// Loads one arm32 JNI library, binds its statically named Java_* exports through HostJni
// thunks, then invokes its guest JNI_OnLoad on the same calling-thread carrier.
class JniLoader {
public:
    JniLoader(HostJni& host_jni, JniBackend& backend) : host_jni_(host_jni), backend_(backend) {}

    JniLoadReport load(JniBackend::Env env, const std::string& path, std::uint32_t guest_flags);

private:
    bool bind_exports(JniBackend::Env env, const ElfSymbolReport& symbols, std::uint32_t guest_handle,
                      JniLoadReport& report, bool& has_onload);
    void bind_guest_loaded_libraries(JniBackend::Env env, JniLoadReport& report);
    void log_missing_class_once(const std::string& name);
    void log_unresolvable_once(const std::string& symbol, const std::string& name);
    void log_unmatched_once(const std::string& symbol);

    HostJni& host_jni_;
    JniBackend& backend_;
    std::mutex missing_mutex_;
    std::unordered_set<std::string> missing_classes_;
    std::unordered_set<std::string> unresolvable_exports_;
    std::unordered_set<std::string> swept_libraries_;
};

}  // namespace zb
