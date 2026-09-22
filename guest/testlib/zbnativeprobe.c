// A guest NativeActivity: what the framework's path actually reaches. It exports
// ANativeActivity_onCreate, fills the callback table, and records everything it is given, so a
// host test can prove the whole translation end to end without a device.
//
// Deliberately written against the 32-bit layout directly rather than <android/native_activity.h>:
// this file is the guest's view, and it is what the layout in native_activity_abi.h is checked
// against.
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum { kCallbackCount = 16 };

struct zb_activity {
    uint32_t callbacks;
    uint32_t vm;
    uint32_t env;
    uint32_t clazz;
    uint32_t internal_data_path;
    uint32_t external_data_path;
    int32_t sdk_version;
    uint32_t instance;
    uint32_t asset_manager;
    uint32_t obb_path;
};

struct zb_state {
    uint32_t on_create_calls;
    uint32_t activity;
    uint32_t saved_state_size;
    char saved_state[16];
    uint32_t vm;
    uint32_t env;
    uint32_t clazz;
    int32_t sdk_version;
    uint32_t asset_manager;
    char internal_path[64];
    char obb_path[64];
    uint32_t events[kCallbackCount];
    uint32_t last_argument;
    int32_t rect[4];
    uint32_t save_block;
};

static struct zb_state state;

uint32_t zbnativeprobe_state(void) {
    return (uint32_t)(uintptr_t)&state;
}

#define ZB_PLAIN_CALLBACK(index, name)                                   \
    static void name(struct zb_activity* activity) {                     \
        (void)activity;                                                  \
        ++state.events[index];                                           \
    }

ZB_PLAIN_CALLBACK(0, on_start)
ZB_PLAIN_CALLBACK(1, on_resume)
ZB_PLAIN_CALLBACK(3, on_pause)
ZB_PLAIN_CALLBACK(4, on_stop)
ZB_PLAIN_CALLBACK(5, on_destroy)
ZB_PLAIN_CALLBACK(14, on_configuration_changed)
ZB_PLAIN_CALLBACK(15, on_low_memory)

static void* on_save_instance_state(struct zb_activity* activity, uint32_t* out_size) {
    (void)activity;
    ++state.events[2];
    char* block = malloc(5);
    if (block == NULL) {
        *out_size = 0;
        return NULL;
    }
    memcpy(block, "state", 5);
    *out_size = 5;
    state.save_block = (uint32_t)(uintptr_t)block;
    return block;
}

static void on_window_focus_changed(struct zb_activity* activity, int32_t has_focus) {
    (void)activity;
    ++state.events[6];
    state.last_argument = (uint32_t)has_focus;
}

static void on_window_created(struct zb_activity* activity, uint32_t window) {
    (void)activity;
    ++state.events[7];
    state.last_argument = window;
}

static void on_input_queue_created(struct zb_activity* activity, uint32_t queue) {
    (void)activity;
    ++state.events[11];
    state.last_argument = queue;
}

static void on_content_rect_changed(struct zb_activity* activity, const int32_t* rect) {
    (void)activity;
    ++state.events[13];
    memcpy(state.rect, rect, sizeof state.rect);
}

static void copy_string(char* out, size_t size, uint32_t address) {
    const char* text = (const char*)(uintptr_t)address;
    if (text == NULL) {
        out[0] = 0;
        return;
    }
    strncpy(out, text, size - 1);
    out[size - 1] = 0;
}

void ANativeActivity_onCreate(struct zb_activity* activity, void* saved_state, uint32_t saved_state_size) {
    ++state.on_create_calls;
    state.activity = (uint32_t)(uintptr_t)activity;
    state.vm = activity->vm;
    state.env = activity->env;
    state.clazz = activity->clazz;
    state.sdk_version = activity->sdk_version;
    state.asset_manager = activity->asset_manager;
    copy_string(state.internal_path, sizeof state.internal_path, activity->internal_data_path);
    copy_string(state.obb_path, sizeof state.obb_path, activity->obb_path);

    state.saved_state_size = saved_state_size;
    if (saved_state != NULL && saved_state_size != 0) {
        const uint32_t take = saved_state_size < sizeof state.saved_state ? saved_state_size
                                                                         : (uint32_t)sizeof state.saved_state;
        memcpy(state.saved_state, saved_state, take);
    }

    // The guest fills the table it wants; every slot it leaves alone must never be called.
    uint32_t* callbacks = (uint32_t*)(uintptr_t)activity->callbacks;
    callbacks[0] = (uint32_t)(uintptr_t)&on_start;
    callbacks[1] = (uint32_t)(uintptr_t)&on_resume;
    callbacks[2] = (uint32_t)(uintptr_t)&on_save_instance_state;
    callbacks[3] = (uint32_t)(uintptr_t)&on_pause;
    callbacks[4] = (uint32_t)(uintptr_t)&on_stop;
    callbacks[5] = (uint32_t)(uintptr_t)&on_destroy;
    callbacks[6] = (uint32_t)(uintptr_t)&on_window_focus_changed;
    callbacks[7] = (uint32_t)(uintptr_t)&on_window_created;
    callbacks[11] = (uint32_t)(uintptr_t)&on_input_queue_created;
    callbacks[13] = (uint32_t)(uintptr_t)&on_content_rect_changed;
    callbacks[14] = (uint32_t)(uintptr_t)&on_configuration_changed;
    callbacks[15] = (uint32_t)(uintptr_t)&on_low_memory;
    // 8, 9, 10 and 12 stay zero on purpose.
}
