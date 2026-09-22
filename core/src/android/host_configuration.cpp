#include "zb/host_configuration.h"

#include <cstring>

#include "zb/configuration_hostcalls.h"
#include "zb/guest_memory.h"
#include "zb/log.h"
#include "zb/runtime_report.h"

namespace zb {

namespace {

inline void* as_pointer(std::uint64_t value) {
    return reinterpret_cast<void*>(static_cast<std::uintptr_t>(value));
}

inline std::uint64_t from_pointer(const void* value) {
    return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(value));
}

}  // namespace

void* HostConfiguration::live_config(std::uint32_t handle) const {
    const std::optional<std::uint64_t> value = configs_.get(handle);
    return value && *value != 0 ? as_pointer(*value) : nullptr;
}

bool HostConfiguration::reject(const char* function, std::uint32_t handle) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (first_rejection_.empty()) {
            first_rejection_ = std::string(function) + " handle " + std::to_string(handle);
            runtime_report().note_jni_detail("configuration-rejected", first_rejection_, true);
        }
    }
    log("configuration call %s rejected: handle %u is not live", function, handle);
    return true;
}

bool HostConfiguration::handle_host_call(std::uint32_t index, GuestThread& thread) {
    if (index < ZB_CONFIG_HC_FIRST || index > ZB_CONFIG_HC_LAST) return false;
    auto& regs = thread.regs();
    switch (index) {
    case ZB_CONFIG_HC_AConfiguration_new: {
        void* config = backend_.create();
        regs[0] = config != nullptr ? configs_.add(from_pointer(config)) : 0;
        return true;
    }
    case ZB_CONFIG_HC_AConfiguration_delete: {
        const std::optional<std::uint64_t> value = configs_.remove(regs[0]);
        if (value && *value != 0) backend_.destroy(as_pointer(*value));
        regs[0] = 0;
        return true;
    }
    case ZB_CONFIG_HC_AConfiguration_fromAssetManager: {
        void* config = live_config(regs[0]);
        if (config == nullptr) { regs[0] = 0; return reject("AConfiguration_fromAssetManager", regs[0]); }
        void* manager = resolve_asset_manager_ ? resolve_asset_manager_(regs[1]) : nullptr;
        if (manager == nullptr) {
            runtime_report().note_jni_detail("configuration-asset-manager", "unknown handle", true);
            regs[0] = 0;
            return true;
        }
        backend_.from_asset_manager(config, manager);
        regs[0] = 0;
        return true;
    }
    case ZB_CONFIG_HC_AConfiguration_getLanguage:
    case ZB_CONFIG_HC_AConfiguration_getCountry: {
        const bool language = index == ZB_CONFIG_HC_AConfiguration_getLanguage;
        const char* function = language ? "AConfiguration_getLanguage" : "AConfiguration_getCountry";
        void* config = live_config(regs[0]);
        if (config == nullptr) { regs[0] = 0; return reject(function, regs[0]); }
        // The NDK writes exactly two characters and does not terminate them.
        std::uint8_t* out = runtime_.memory().host_ptr(regs[1], 2, kPageRead | kPageWrite);
        if (out == nullptr) { regs[0] = 0; return reject(function, regs[1]); }
        char value[2] = {0, 0};
        if (language) {
            backend_.get_language(config, value);
        } else {
            backend_.get_country(config, value);
        }
        std::memcpy(out, value, sizeof value);
        regs[0] = 0;
        return true;
    }
    case ZB_CONFIG_HC_AConfiguration_setLanguage:
    case ZB_CONFIG_HC_AConfiguration_setCountry: {
        const bool language = index == ZB_CONFIG_HC_AConfiguration_setLanguage;
        const char* function = language ? "AConfiguration_setLanguage" : "AConfiguration_setCountry";
        void* config = live_config(regs[0]);
        if (config == nullptr) { regs[0] = 0; return reject(function, regs[0]); }
        char value[3] = {0, 0, 0};
        if (regs[1] != 0) {
            const std::uint8_t* text = runtime_.memory().host_ptr(regs[1], 2, kPageRead);
            if (text == nullptr) { regs[0] = 0; return reject(function, regs[1]); }
            std::memcpy(value, text, 2);
        }
        if (language) {
            backend_.set_language(config, regs[1] != 0 ? value : nullptr);
        } else {
            backend_.set_country(config, regs[1] != 0 ? value : nullptr);
        }
        regs[0] = 0;
        return true;
    }

#include "gen/configuration_dispatch.inc"

    default:
        return false;
    }
}

}  // namespace zb
