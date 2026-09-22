#pragma once

#include <android/asset_manager.h>
#include <android/configuration.h>
#include <dlfcn.h>

#include "zb/configuration_backend.h"

namespace zb {

// ConfigurationBackend over the real NDK. The generated half is one cast and one call per
// accessor; the object's life, the asset manager and the two-character fields are here because
// they cross the guest boundary as memory rather than as values.
class ConfigurationDriverBackend final : public ConfigurationBackend {
public:
    void* create() override { return AConfiguration_new(); }

    void destroy(void* config) override { AConfiguration_delete(static_cast<AConfiguration*>(config)); }

    void from_asset_manager(void* config, void* asset_manager) override {
        AConfiguration_fromAssetManager(static_cast<AConfiguration*>(config),
                                        static_cast<AAssetManager*>(asset_manager));
    }

    void get_language(void* config, char* out) override {
        AConfiguration_getLanguage(static_cast<AConfiguration*>(config), out);
    }

    void get_country(void* config, char* out) override {
        AConfiguration_getCountry(static_cast<AConfiguration*>(config), out);
    }

    void set_language(void* config, const char* language) override {
        AConfiguration_setLanguage(static_cast<AConfiguration*>(config), language);
    }

    void set_country(void* config, const char* country) override {
        AConfiguration_setCountry(static_cast<AConfiguration*>(config), country);
    }

#include "gen/configuration_driver.inc"
};

}  // namespace zb
