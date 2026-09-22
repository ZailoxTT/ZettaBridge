// The NativeActivity path end to end against a real guest library: the guest's own
// ANativeActivity_onCreate runs under the translator, reads the activity we built, installs its
// callbacks, and every lifecycle event we deliver arrives with the right arguments.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

#include "check.h"
#include "zb/host_native_activity.h"
#include "zb/library_protocol.h"
#include "zb/library_runtime.h"

namespace {

// The guest state structure of guest/testlib/zbnativeprobe.c, field by field.
struct ProbeState {
    std::uint32_t on_create_calls;
    std::uint32_t activity;
    std::uint32_t saved_state_size;
    char saved_state[16];
    std::uint32_t vm;
    std::uint32_t env;
    std::uint32_t clazz;
    std::int32_t sdk_version;
    std::uint32_t asset_manager;
    char internal_path[64];
    char obb_path[64];
    std::uint32_t events[16];
    std::uint32_t last_argument;
    std::int32_t rect[4];
    std::uint32_t save_block;
};

const ProbeState& probe_state(zb::LibraryRuntime& runtime, std::uint32_t address) {
    const std::uint8_t* memory = runtime.memory().host_ptr(address, sizeof(ProbeState), zb::kPageRead);
    CHECK(memory != nullptr);
    return *reinterpret_cast<const ProbeState*>(memory);
}

}  // namespace

int main(int argc, char** argv) {
    CHECK(argc == 4);
    zb::LibraryRuntime runtime;
    zb::LibraryRuntimeOptions options;
    options.sysroot = argv[1];
    options.zbhost = argv[2];
    options.target_sdk = 16;
    options.guest_environment = {"LD_LIBRARY_PATH=" + std::filesystem::path(argv[3]).parent_path().string()};
    std::string error;
    CHECK(runtime.start(options, error));

    const std::uint32_t library = runtime.load_library(argv[3], ZB_GUEST_RTLD_NOW, error);
    CHECK(library != 0);
    const std::uint32_t on_create = runtime.find_symbol(library, "ANativeActivity_onCreate", error);
    const std::uint32_t state_of = runtime.find_symbol(library, "zbnativeprobe_state", error);
    CHECK(on_create != 0 && state_of != 0);

    // Guest memory for the activity comes from the guest allocator, as it does in the launcher.
    const auto allocate = [&](std::size_t size) -> std::optional<std::uint32_t> {
        zb::GuestCall call;
        call.regs = {static_cast<std::uint32_t>(size), 0, 0, 0};
        const auto result = runtime.call_on_service(runtime.service_api().malloc_fn, call);
        if (!result || result->r0 == 0) return std::nullopt;
        return result->r0;
    };
    const auto deallocate = [&](std::uint32_t address) {
        zb::GuestCall call;
        call.regs = {address, 0, 0, 0};
        runtime.call_on_service(runtime.service_api().free_fn, call);
    };
    zb::HostNativeActivity activity(
        runtime,
        [&](std::uint32_t function, const zb::GuestCall& args) -> std::optional<std::uint32_t> {
            const auto result = runtime.call_on_service(function, args);
            if (!result) return std::nullopt;
            return result->r0;
        },
        allocate, deallocate);

    zb::HostNativeActivity::Description description;
    description.guest_on_create = on_create;
    description.guest_vm = 0xAAAA0001;
    description.guest_env = 0xAAAA0002;
    description.activity_handle = 0x2A;
    description.asset_manager = 0x3B;
    description.sdk_version = 22;
    description.internal_data_path = "/data/user/0/com.example/files";
    description.external_data_path = "/sdcard/Android/data/com.example/files";
    description.obb_path = "/sdcard/Android/obb/com.example";

    const char saved[] = "restore-me";
    const auto guest = activity.create(description, saved, sizeof saved, error);
    CHECK(guest.has_value());

    const std::uint32_t state_address = [&] {
        const auto result = runtime.call_on_service(state_of, zb::GuestCall{});
        CHECK(result.has_value());
        return result->r0;
    }();
    CHECK(state_address != 0);

    // The guest read the activity we built.
    {
        const ProbeState& state = probe_state(runtime, state_address);
        CHECK(state.on_create_calls == 1);
        CHECK(state.activity == *guest);
        CHECK(state.vm == 0xAAAA0001 && state.env == 0xAAAA0002);
        CHECK(state.clazz == 0x2A && state.asset_manager == 0x3B);
        CHECK(state.sdk_version == 22);
        CHECK(std::string(state.internal_path) == description.internal_data_path);
        CHECK(std::string(state.obb_path) == description.obb_path);
        CHECK(state.saved_state_size == sizeof saved);
        CHECK(std::string(state.saved_state) == "restore-me");
    }

    // Every event the guest asked for arrives, with the NDK's second argument.
    activity.deliver(*guest, zb::GuestCallback::OnStart, 0);
    activity.deliver(*guest, zb::GuestCallback::OnResume, 0);
    activity.deliver(*guest, zb::GuestCallback::OnWindowFocusChanged, 1);
    activity.deliver(*guest, zb::GuestCallback::OnNativeWindowCreated, 0x5150);
    {
        const ProbeState& state = probe_state(runtime, state_address);
        CHECK(state.events[0] == 1 && state.events[1] == 1);
        CHECK(state.events[6] == 1 && state.events[7] == 1);
        CHECK(state.last_argument == 0x5150);
    }

    // A slot the guest left empty is never called through zero: the process would be gone.
    activity.deliver(*guest, zb::GuestCallback::OnNativeWindowResized, 0x5150);
    CHECK(probe_state(runtime, state_address).events[8] == 0);

    activity.deliver(*guest, zb::GuestCallback::OnInputQueueCreated, 0x77);
    CHECK(probe_state(runtime, state_address).events[11] == 1);
    CHECK(probe_state(runtime, state_address).last_argument == 0x77);

    activity.deliver_content_rect(*guest, 10, 20, 30, 40);
    {
        const ProbeState& state = probe_state(runtime, state_address);
        CHECK(state.events[13] == 1);
        CHECK(state.rect[0] == 10 && state.rect[1] == 20 && state.rect[2] == 30 && state.rect[3] == 40);
    }

    // The guest allocates its save block with its own allocator; we copy it to the host heap and
    // hand the guest block back. The bytes must survive the crossing intact.
    std::size_t size = 0;
    void* copy = activity.save_instance_state(*guest, &size);
    CHECK(copy != nullptr && size == 5);
    CHECK(std::memcmp(copy, "state", 5) == 0);
    CHECK(probe_state(runtime, state_address).save_block != 0);
    std::free(copy);

    activity.deliver(*guest, zb::GuestCallback::OnPause, 0);
    activity.deliver(*guest, zb::GuestCallback::OnStop, 0);
    activity.deliver(*guest, zb::GuestCallback::OnDestroy, 0);
    {
        const ProbeState& state = probe_state(runtime, state_address);
        CHECK(state.events[3] == 1 && state.events[4] == 1 && state.events[5] == 1);
    }
    activity.destroy(*guest);
    CHECK(activity.guest_callback(*guest, zb::GuestCallback::OnStart) == 0);

    std::puts("native_activity_probe_test PASS");
    std::fflush(stdout);
    // The runtime is process-lifetime: leave without unwinding it.
    std::_Exit(0);
}
