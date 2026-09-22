#pragma once

#include <cstring>
#include <set>
#include <string>

#include "zb/configuration_backend.h"

// In-memory ConfigurationBackend for host tests: a configuration is an object with the few fields
// the tests look at, and every created object is tracked so a leak or a double delete shows up.
class MockConfiguration final : public zb::ConfigurationBackend {
public:
    struct Config {
        std::int32_t orientation = 1;
        std::int32_t density = 420;
        char language[2] = {'e', 'n'};
        char country[2] = {'G', 'B'};
    };

    void* create() override {
        Config* config = new Config();
        live.insert(config);
        ++created;
        return config;
    }

    void destroy(void* config) override {
        ++destroyed;
        if (live.erase(config) != 0) delete static_cast<Config*>(config);
    }

    void from_asset_manager(void* config, void* asset_manager) override {
        (void)config;
        last_asset_manager = asset_manager;
    }

    void get_language(void* config, char* out) override { std::memcpy(out, at(config).language, 2); }
    void get_country(void* config, char* out) override { std::memcpy(out, at(config).country, 2); }
    void set_language(void* config, const char* language) override {
        last_set_language = language != nullptr ? std::string(language, 2) : std::string("(null)");
    }
    void set_country(void* config, const char* country) override {
        (void)config;
        last_set_country = country != nullptr ? std::string(country, 2) : std::string("(null)");
    }

    std::int32_t AConfiguration_getOrientation(void* config) override { return at(config).orientation; }
    std::int32_t AConfiguration_getDensity(void* config) override { return at(config).density; }
    void AConfiguration_setOrientation(void* config, std::int32_t value) override {
        at(config).orientation = value;
    }
    void AConfiguration_copy(void* dest, void* src) override { at(dest) = at(src); }

    Config& at(void* config) { return *static_cast<Config*>(config); }

    std::set<void*> live;
    int created = 0;
    int destroyed = 0;
    void* last_asset_manager = nullptr;
    std::string last_set_language;
    std::string last_set_country;
};
