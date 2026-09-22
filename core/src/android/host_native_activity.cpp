#include "zb/host_native_activity.h"

#include <cstdlib>
#include <cstring>

#include "zb/guest_memory.h"
#include "zb/log.h"
#include "zb/runtime_report.h"

namespace zb {

std::optional<std::uint32_t> HostNativeActivity::copy_bytes(const void* data, std::size_t size) {
    if (size == 0) return 0u;
    const std::optional<std::uint32_t> address = allocate_(size);
    if (!address) return std::nullopt;
    std::uint8_t* destination = runtime_.memory().host_ptr(*address, size, kPageRead | kPageWrite);
    if (destination == nullptr) {
        deallocate_(*address);
        return std::nullopt;
    }
    std::memcpy(destination, data, size);
    return address;
}

std::optional<std::uint32_t> HostNativeActivity::copy_string(const std::string& text) {
    // Always NUL-terminated, and an empty string is still a valid empty string, never null: the
    // NDK promises the guest a readable path.
    const std::optional<std::uint32_t> address = copy_bytes(text.c_str(), text.size() + 1);
    return address;
}

std::optional<std::uint32_t> HostNativeActivity::create(const Description& description, const void* saved_state,
                                                        std::size_t saved_state_size, std::string& error) {
    if (description.guest_on_create == 0) {
        error = "the guest library exports no ANativeActivity_onCreate";
        return std::nullopt;
    }
    const std::optional<std::uint32_t> activity = allocate_(kGuestActivitySize);
    const std::optional<std::uint32_t> callbacks = allocate_(kGuestCallbackTableSize);
    const std::optional<std::uint32_t> internal_path = copy_string(description.internal_data_path);
    const std::optional<std::uint32_t> external_path = copy_string(description.external_data_path);
    const std::optional<std::uint32_t> obb_path = copy_string(description.obb_path);
    const std::optional<std::uint32_t> state =
        saved_state_size != 0 ? copy_bytes(saved_state, saved_state_size) : std::optional<std::uint32_t>(0u);
    if (!activity || !callbacks || !internal_path || !external_path || !obb_path || !state) {
        error = "cannot allocate the guest activity";
        return std::nullopt;
    }

    std::uint8_t* fields = runtime_.memory().host_ptr(*activity, kGuestActivitySize, kPageRead | kPageWrite);
    std::uint8_t* table = runtime_.memory().host_ptr(*callbacks, kGuestCallbackTableSize, kPageRead | kPageWrite);
    if (fields == nullptr || table == nullptr) {
        error = "the guest allocator returned unreadable memory";
        return std::nullopt;
    }
    std::memset(table, 0, kGuestCallbackTableSize);
    const std::uint32_t words[] = {
        *callbacks,
        description.guest_vm,
        description.guest_env,
        description.activity_handle,
        *internal_path,
        *external_path,
        static_cast<std::uint32_t>(description.sdk_version),
        0,  // instance: the guest's own
        description.asset_manager,
        *obb_path,
    };
    static_assert(sizeof words == kGuestActivitySize, "guest ANativeActivity layout");
    std::memcpy(fields, words, sizeof words);

    {
        std::lock_guard<std::mutex> lock(mutex_);
        activities_[*activity] = Activity{*callbacks, *state, *internal_path, *external_path, *obb_path};
    }

    GuestCall call;
    call.regs = {*activity, *state, static_cast<std::uint32_t>(saved_state_size), 0};
    if (!call_(description.guest_on_create, call)) {
        std::lock_guard<std::mutex> lock(mutex_);
        activities_.erase(*activity);
        error = "the guest ANativeActivity_onCreate failed";
        return std::nullopt;
    }

    // What the guest installed decides which events are worth delivering, and is the first thing
    // to look at when a guest goes quiet.
    std::string installed;
    for (std::uint32_t slot = 0; slot < kGuestCallbackCount; ++slot) {
        std::uint32_t function = 0;
        std::memcpy(&function, table + slot * 4, sizeof function);
        if (function == 0) continue;
        if (!installed.empty()) installed += " ";
        installed += kGuestCallbackNames[slot];
    }
    runtime_report().note_jni_detail("native-activity-callbacks", installed.empty() ? "none" : installed, true);
    log("guest NativeActivity created at 0x%08x, callbacks: %s", *activity,
        installed.empty() ? "none" : installed.c_str());
    return activity;
}

std::uint32_t HostNativeActivity::guest_callback(std::uint32_t guest_activity, GuestCallback slot) const {
    std::uint32_t table = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto entry = activities_.find(guest_activity);
        if (entry == activities_.end()) return 0;
        table = entry->second.callbacks_table;
    }
    const std::uint8_t* pointers =
        runtime_.memory().host_ptr(table, kGuestCallbackTableSize, kPageRead);
    if (pointers == nullptr) return 0;
    std::uint32_t function = 0;
    std::memcpy(&function, pointers + static_cast<std::uint32_t>(slot) * 4, sizeof function);
    return function;
}

void HostNativeActivity::deliver(std::uint32_t guest_activity, GuestCallback slot, std::uint32_t argument) {
    const std::uint32_t function = guest_callback(guest_activity, slot);
    if (function == 0) return;  // the guest does not want this event
    GuestCall call;
    call.regs = {guest_activity, argument, 0, 0};
    if (!call_(function, call)) {
        log("guest %s failed", kGuestCallbackNames[static_cast<std::uint32_t>(slot)]);
        runtime_report().note_jni_detail("native-activity-failed",
                                         kGuestCallbackNames[static_cast<std::uint32_t>(slot)], true);
        return;
    }
    runtime_report().note_jni_detail("native-activity-last",
                                     kGuestCallbackNames[static_cast<std::uint32_t>(slot)], true);
}

void HostNativeActivity::deliver_content_rect(std::uint32_t guest_activity, std::int32_t left, std::int32_t top,
                                              std::int32_t right, std::int32_t bottom) {
    if (guest_callback(guest_activity, GuestCallback::OnContentRectChanged) == 0) return;
    const std::int32_t rect[4] = {left, top, right, bottom};
    const std::optional<std::uint32_t> address = copy_bytes(rect, sizeof rect);
    if (!address) {
        log("cannot allocate the guest content rectangle");
        return;
    }
    deliver(guest_activity, GuestCallback::OnContentRectChanged, *address);
    deallocate_(*address);
}

void* HostNativeActivity::save_instance_state(std::uint32_t guest_activity, std::size_t* out_size) {
    if (out_size != nullptr) *out_size = 0;
    const std::uint32_t function = guest_callback(guest_activity, GuestCallback::OnSaveInstanceState);
    if (function == 0) return nullptr;

    // The guest writes its size through a pointer, so it needs a guest word to write into.
    const std::optional<std::uint32_t> size_word = allocate_(4);
    if (!size_word) return nullptr;
    std::uint8_t* size_memory = runtime_.memory().host_ptr(*size_word, 4, kPageRead | kPageWrite);
    if (size_memory == nullptr) {
        deallocate_(*size_word);
        return nullptr;
    }
    std::memset(size_memory, 0, 4);

    GuestCall call;
    call.regs = {guest_activity, *size_word, 0, 0};
    const std::optional<std::uint32_t> returned = call_(function, call);
    if (!returned) {
        deallocate_(*size_word);
        log("guest onSaveInstanceState failed");
        return nullptr;
    }
    std::uint32_t size = 0;
    std::memcpy(&size, size_memory, sizeof size);
    const std::uint32_t block = *returned;
    deallocate_(*size_word);
    if (block == 0 || size == 0) return nullptr;

    const std::uint8_t* source = runtime_.memory().host_ptr(block, size, kPageRead);
    if (source == nullptr) {
        log("guest onSaveInstanceState returned %u bytes at an unreadable address 0x%08x", size, block);
        return nullptr;
    }
    // The framework frees this with free(), so it must come from the host heap; the guest block
    // goes back to the guest allocator. A block freed by the wrong heap corrupts both.
    void* copy = std::malloc(size);
    if (copy == nullptr) {
        deallocate_(block);
        return nullptr;
    }
    std::memcpy(copy, source, size);
    deallocate_(block);
    if (out_size != nullptr) *out_size = size;
    return copy;
}

void HostNativeActivity::destroy(std::uint32_t guest_activity) {
    Activity activity;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto entry = activities_.find(guest_activity);
        if (entry == activities_.end()) return;
        activity = entry->second;
        activities_.erase(entry);
    }
    for (std::uint32_t address : {activity.callbacks_table, activity.saved_state, activity.internal_path,
                                  activity.external_path, activity.obb_path, guest_activity}) {
        if (address != 0) deallocate_(address);
    }
}

}  // namespace zb
