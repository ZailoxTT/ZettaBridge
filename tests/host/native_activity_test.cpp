// The guest ANativeActivity: its layout and contents, the callbacks the guest installs, every
// lifecycle event forwarded once, the content rectangle written into guest memory, and the
// save-state block copied to the host heap and returned to the guest allocator.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <sys/mman.h>

#include "check.h"
#include "zb/host_native_activity.h"
#include "zb/library_runtime.h"

namespace {

constexpr std::uint32_t kHeap = 0x30000;
constexpr std::uint32_t kHeapSize = 0x4000;
constexpr std::uint32_t kOnCreate = 0x8000;
constexpr std::uint32_t kGuestCallbackBase = 0x9000;  // fake guest callback addresses

struct Call {
    std::uint32_t function;
    std::uint32_t arg0;
    std::uint32_t arg1;
    std::uint32_t arg2;
};

// A bump allocator standing in for the guest allocator, with freeing recorded rather than done:
// what matters is that every block goes back, and to the right heap.
struct Heap {
    std::uint32_t next = kHeap;
    std::vector<std::uint32_t> freed;

    std::uint32_t allocate(std::size_t size) {
        const std::uint32_t result = next;
        next += static_cast<std::uint32_t>((size + 7) & ~std::size_t{7});
        CHECK(next < kHeap + kHeapSize);
        return result;
    }
    void deallocate(std::uint32_t address) { freed.push_back(address); }
    bool was_freed(std::uint32_t address) const {
        for (std::uint32_t entry : freed) {
            if (entry == address) return true;
        }
        return false;
    }
};

std::uint32_t read_word(zb::LibraryRuntime& runtime, std::uint32_t address) {
    std::uint32_t value = 0;
    std::memcpy(&value, runtime.memory().base() + address, sizeof value);
    return value;
}

void write_word(zb::LibraryRuntime& runtime, std::uint32_t address, std::uint32_t value) {
    std::memcpy(runtime.memory().base() + address, &value, sizeof value);
}

const char* guest_string(zb::LibraryRuntime& runtime, std::uint32_t address) {
    return reinterpret_cast<const char*>(runtime.memory().base() + address);
}

}  // namespace

int main() {
    zb::LibraryRuntime runtime;
    CHECK(runtime.memory().map_anon(kHeap, kHeapSize, PROT_READ | PROT_WRITE));
    Heap heap;
    std::vector<Call> calls;
    // The fake guest: onCreate installs a callback in every slot but onConfigurationChanged, and
    // onSaveInstanceState writes a size and returns a block, as a real guest would.
    std::uint32_t save_block = 0;
    std::uint32_t save_size = 0;
    bool fail_next = false;

    zb::HostNativeActivity activity(
        runtime,
        [&](std::uint32_t function, const zb::GuestCall& args) -> std::optional<std::uint32_t> {
            calls.push_back({function, args.regs[0], args.regs[1], args.regs[2]});
            if (fail_next) return std::nullopt;
            if (function == kOnCreate) {
                const std::uint32_t table = read_word(runtime, args.regs[0] + zb::kGuestActivityCallbacks);
                for (std::uint32_t slot = 0; slot < zb::kGuestCallbackCount; ++slot) {
                    const bool skipped = slot == static_cast<std::uint32_t>(zb::GuestCallback::OnConfigurationChanged);
                    write_word(runtime, table + slot * 4, skipped ? 0 : kGuestCallbackBase + slot);
                }
                return 0u;
            }
            if (function == kGuestCallbackBase + static_cast<std::uint32_t>(zb::GuestCallback::OnSaveInstanceState)) {
                write_word(runtime, args.regs[1], save_size);
                return save_block;
            }
            return 0u;
        },
        [&](std::size_t size) -> std::optional<std::uint32_t> { return heap.allocate(size); },
        [&](std::uint32_t address) { heap.deallocate(address); });

    zb::HostNativeActivity::Description description;
    description.guest_on_create = kOnCreate;
    description.guest_vm = 0x1111;
    description.guest_env = 0x2222;
    description.activity_handle = 0x3333;
    description.asset_manager = 0x4444;
    description.sdk_version = 22;
    description.internal_data_path = "/data/user/0/com.example/files";
    description.external_data_path = "/sdcard/Android/data/com.example/files";
    description.obb_path = "/sdcard/Android/obb/com.example";

    // A library with no ANativeActivity_onCreate is refused before anything is allocated.
    {
        zb::HostNativeActivity::Description without = description;
        without.guest_on_create = 0;
        std::string error;
        CHECK(!activity.create(without, nullptr, 0, error));
        CHECK(error.find("ANativeActivity_onCreate") != std::string::npos);
        CHECK(calls.empty());
    }

    const char saved[] = "saved-state";
    std::string error;
    const auto guest = activity.create(description, saved, sizeof saved, error);
    CHECK(guest.has_value());
    CHECK(error.empty());

    // The guest activity holds guest values only, in NDK field order.
    const std::uint32_t table = read_word(runtime, *guest + zb::kGuestActivityCallbacks);
    CHECK(table != 0);
    CHECK(read_word(runtime, *guest + zb::kGuestActivityVm) == 0x1111);
    CHECK(read_word(runtime, *guest + zb::kGuestActivityEnv) == 0x2222);
    CHECK(read_word(runtime, *guest + zb::kGuestActivityClazz) == 0x3333);
    CHECK(read_word(runtime, *guest + zb::kGuestActivitySdkVersion) == 22);
    CHECK(read_word(runtime, *guest + zb::kGuestActivityInstance) == 0);
    CHECK(read_word(runtime, *guest + zb::kGuestActivityAssetManager) == 0x4444);
    CHECK(std::string(guest_string(runtime, read_word(runtime, *guest + zb::kGuestActivityInternalDataPath))) ==
          description.internal_data_path);
    CHECK(std::string(guest_string(runtime, read_word(runtime, *guest + zb::kGuestActivityExternalDataPath))) ==
          description.external_data_path);
    CHECK(std::string(guest_string(runtime, read_word(runtime, *guest + zb::kGuestActivityObbPath))) ==
          description.obb_path);

    // onCreate got the activity, the saved state copied into guest memory, and its size.
    CHECK(calls.size() == 1);
    CHECK(calls[0].function == kOnCreate && calls[0].arg0 == *guest);
    CHECK(calls[0].arg2 == sizeof saved);
    CHECK(std::string(guest_string(runtime, calls[0].arg1)) == "saved-state");

    // Callbacks were read back from guest memory.
    CHECK(activity.guest_callback(*guest, zb::GuestCallback::OnStart) ==
          kGuestCallbackBase + static_cast<std::uint32_t>(zb::GuestCallback::OnStart));
    CHECK(activity.guest_callback(*guest, zb::GuestCallback::OnConfigurationChanged) == 0);
    CHECK(activity.guest_callback(0xdead, zb::GuestCallback::OnStart) == 0);

    // Each event is forwarded once, with the activity and the NDK's second argument.
    calls.clear();
    activity.deliver(*guest, zb::GuestCallback::OnResume, 0);
    activity.deliver(*guest, zb::GuestCallback::OnWindowFocusChanged, 1);
    activity.deliver(*guest, zb::GuestCallback::OnNativeWindowCreated, 0x77);
    CHECK(calls.size() == 3);
    CHECK(calls[0].function == kGuestCallbackBase + static_cast<std::uint32_t>(zb::GuestCallback::OnResume));
    CHECK(calls[0].arg0 == *guest && calls[0].arg1 == 0);
    CHECK(calls[1].arg1 == 1);
    CHECK(calls[2].arg1 == 0x77);

    // A callback the guest did not install is not called through zero, and an unknown activity
    // reaches nothing at all.
    calls.clear();
    activity.deliver(*guest, zb::GuestCallback::OnConfigurationChanged, 0);
    activity.deliver(0xdead, zb::GuestCallback::OnStart, 0);
    CHECK(calls.empty());

    // The content rectangle is written into guest memory as four words and freed afterwards.
    calls.clear();
    activity.deliver_content_rect(*guest, 1, 2, 3, 4);
    CHECK(calls.size() == 1);
    const std::uint32_t rect = calls[0].arg1;
    CHECK(read_word(runtime, rect) == 1 && read_word(runtime, rect + 4) == 2);
    CHECK(read_word(runtime, rect + 8) == 3 && read_word(runtime, rect + 12) == 4);
    CHECK(heap.was_freed(rect));

    // onSaveInstanceState: the guest's block is copied to the host heap and given back to the
    // guest allocator, because a block freed by the wrong heap corrupts both.
    save_block = heap.allocate(8);
    save_size = 5;
    std::memcpy(runtime.memory().base() + save_block, "state", 5);
    std::size_t out_size = 0;
    void* copy = activity.save_instance_state(*guest, &out_size);
    CHECK(copy != nullptr && out_size == 5);
    CHECK(std::memcmp(copy, "state", 5) == 0);
    CHECK(copy < runtime.memory().base() || copy >= runtime.memory().base() + (std::uint64_t{1} << 32));
    CHECK(heap.was_freed(save_block));
    std::free(copy);

    // A guest that saves nothing returns no block and no size.
    save_block = 0;
    save_size = 0;
    out_size = 7;
    CHECK(activity.save_instance_state(*guest, &out_size) == nullptr);
    CHECK(out_size == 0);

    // A failing guest call is reported, not ignored silently, and never returns a block.
    fail_next = true;
    save_block = heap.allocate(8);
    save_size = 4;
    CHECK(activity.save_instance_state(*guest, &out_size) == nullptr);
    fail_next = false;

    // Destroying the activity returns its guest memory: the activity, its callback table, the
    // saved state and the three paths.
    const std::uint32_t internal_path = read_word(runtime, *guest + zb::kGuestActivityInternalDataPath);
    activity.destroy(*guest);
    CHECK(heap.was_freed(*guest) && heap.was_freed(table) && heap.was_freed(internal_path));
    CHECK(activity.guest_callback(*guest, zb::GuestCallback::OnStart) == 0);

    std::puts("native_activity_test PASS");
    return 0;
}
