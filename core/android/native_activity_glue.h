#pragma once

#include <android/native_activity.h>

#include "zb/host_input.h"
#include "zb/host_native_activity.h"
#include "zb/host_native_window.h"
#include "zb/native_activity_platform.h"

namespace zb {

// The Android half of the NativeActivity bridge: it reads the framework's ANativeActivity and
// puts our host callbacks into its table, so every lifecycle event the framework delivers on the
// UI thread is translated into a guest call. The windows and input queues it receives are host
// objects, and cross to the guest as handles from the tables that already own them.
class NativeActivityGlue final : public NativeActivityPlatform {
public:
    NativeActivityGlue(HostNativeActivity& activities, HostNativeWindow* windows, HostInput* input)
        : activities_(activities), windows_(windows), input_(input) {}

    bool read(std::uint64_t activity, Facts& facts) override;
    void attach(std::uint64_t activity, std::uint32_t guest_activity) override;

private:
    // The framework calls these with its own ANativeActivity*; each one finds the guest activity
    // that belongs to it and delivers the event.
    static void on_start(ANativeActivity* activity);
    static void on_resume(ANativeActivity* activity);
    static void* on_save_instance_state(ANativeActivity* activity, size_t* out_size);
    static void on_pause(ANativeActivity* activity);
    static void on_stop(ANativeActivity* activity);
    static void on_destroy(ANativeActivity* activity);
    static void on_window_focus_changed(ANativeActivity* activity, int has_focus);
    static void on_window_created(ANativeActivity* activity, ANativeWindow* window);
    static void on_window_resized(ANativeActivity* activity, ANativeWindow* window);
    static void on_window_redraw_needed(ANativeActivity* activity, ANativeWindow* window);
    static void on_window_destroyed(ANativeActivity* activity, ANativeWindow* window);
    static void on_input_queue_created(ANativeActivity* activity, AInputQueue* queue);
    static void on_input_queue_destroyed(ANativeActivity* activity, AInputQueue* queue);
    static void on_content_rect_changed(ANativeActivity* activity, const ARect* rect);
    static void on_configuration_changed(ANativeActivity* activity);
    static void on_low_memory(ANativeActivity* activity);

    HostNativeActivity& activities_;
    HostNativeWindow* windows_;
    HostInput* input_;
};

}  // namespace zb
