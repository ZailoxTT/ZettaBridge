// AConfiguration_*: handles own the host object's life, the generated accessors marshal by shape,
// the two-character fields cross as guest memory, and a handle that is not live is rejected.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>

#include <dynarmic/interface/exclusive_monitor.h>
#include <sys/mman.h>

#include "check.h"
#include "mock_configuration.h"
#include "zb/configuration_hostcalls.h"
#include "zb/host_configuration.h"
#include "zb/library_runtime.h"

namespace {

constexpr std::uint32_t kData = 0x20000;

std::uint32_t call_config(zb::HostConfiguration& configuration, zb::GuestThread& thread, std::uint32_t index,
                          std::initializer_list<std::uint32_t> args) {
    auto& regs = thread.regs();
    regs[0] = regs[1] = regs[2] = regs[3] = 0;
    unsigned position = 0;
    for (std::uint32_t value : args) regs[position++] = value;
    CHECK(configuration.handle_host_call(index, thread));
    return regs[0];
}

}  // namespace

int main() {
    zb::LibraryRuntime runtime;
    CHECK(runtime.memory().map_anon(kData, 0x1000, PROT_READ | PROT_WRITE));
    MockConfiguration backend;
    void* asset_manager = &backend;  // any host pointer will do
    zb::HostConfiguration configuration(runtime, backend, [&](std::uint32_t handle) -> void* {
        return handle == 0x99 ? asset_manager : nullptr;
    });
    Dynarmic::ExclusiveMonitor monitor(1);
    zb::GuestThread thread(runtime.memory(), &monitor, 0, false, zb::kCarrierCodeCacheSize);

    CHECK(!configuration.handle_host_call(zb::ZB_CONFIG_HC_FIRST - 1, thread));
    CHECK(!configuration.handle_host_call(zb::ZB_CONFIG_HC_LAST + 1, thread));

    const std::uint32_t config = call_config(configuration, thread, zb::ZB_CONFIG_HC_AConfiguration_new, {});
    CHECK(config != 0 && backend.created == 1);
    CHECK(configuration.live_config(config) != nullptr);

    // Generated accessors: a value in, a value out, both through the handle.
    CHECK(call_config(configuration, thread, zb::ZB_CONFIG_HC_AConfiguration_getDensity, {config}) == 420);
    CHECK(call_config(configuration, thread, zb::ZB_CONFIG_HC_AConfiguration_getOrientation, {config}) == 1);
    call_config(configuration, thread, zb::ZB_CONFIG_HC_AConfiguration_setOrientation, {config, 2});
    CHECK(call_config(configuration, thread, zb::ZB_CONFIG_HC_AConfiguration_getOrientation, {config}) == 2);

    // Two handles in one call.
    const std::uint32_t copy = call_config(configuration, thread, zb::ZB_CONFIG_HC_AConfiguration_new, {});
    call_config(configuration, thread, zb::ZB_CONFIG_HC_AConfiguration_copy, {copy, config});
    CHECK(call_config(configuration, thread, zb::ZB_CONFIG_HC_AConfiguration_getOrientation, {copy}) == 2);

    // The NDK writes exactly two characters and terminates nothing.
    std::memset(runtime.memory().base() + kData, '#', 4);
    call_config(configuration, thread, zb::ZB_CONFIG_HC_AConfiguration_getLanguage, {config, kData});
    CHECK(std::memcmp(runtime.memory().base() + kData, "en##", 4) == 0);
    call_config(configuration, thread, zb::ZB_CONFIG_HC_AConfiguration_getCountry, {config, kData + 2});
    CHECK(std::memcmp(runtime.memory().base() + kData, "enGB", 4) == 0);

    std::memcpy(runtime.memory().base() + kData + 8, "de", 2);
    call_config(configuration, thread, zb::ZB_CONFIG_HC_AConfiguration_setLanguage, {config, kData + 8});
    CHECK(backend.last_set_language == "de");
    call_config(configuration, thread, zb::ZB_CONFIG_HC_AConfiguration_setLanguage, {config, 0});
    CHECK(backend.last_set_language == "(null)");

    // The asset manager comes from its own handle table, and an unknown one leaves the
    // configuration alone rather than reaching a pointer the guest chose.
    call_config(configuration, thread, zb::ZB_CONFIG_HC_AConfiguration_fromAssetManager, {config, 0x99});
    CHECK(backend.last_asset_manager == asset_manager);
    backend.last_asset_manager = nullptr;
    call_config(configuration, thread, zb::ZB_CONFIG_HC_AConfiguration_fromAssetManager, {config, 0x1234});
    CHECK(backend.last_asset_manager == nullptr);

    // An unreadable guest buffer fails the call, and nothing is written.
    call_config(configuration, thread, zb::ZB_CONFIG_HC_AConfiguration_getLanguage, {config, 0xDEAD0000});

    // Deleting ends the handle and the object; a second delete reaches neither.
    call_config(configuration, thread, zb::ZB_CONFIG_HC_AConfiguration_delete, {config});
    CHECK(backend.destroyed == 1 && configuration.live_config(config) == nullptr);
    call_config(configuration, thread, zb::ZB_CONFIG_HC_AConfiguration_delete, {config});
    CHECK(backend.destroyed == 1);
    CHECK(call_config(configuration, thread, zb::ZB_CONFIG_HC_AConfiguration_getDensity, {config}) == 0);

    call_config(configuration, thread, zb::ZB_CONFIG_HC_AConfiguration_delete, {copy});
    CHECK(backend.live.empty());

    std::puts("configuration_test PASS");
    return 0;
}
