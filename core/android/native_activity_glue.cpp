#include "native_activity_glue.h"

#include <mutex>
#include <unordered_map>

#include "zb/log.h"

namespace zb {

namespace {

struct Attachment {
    NativeActivityGlue* glue = nullptr;
    HostNativeActivity* activities = nullptr;
    HostNativeWindow* windows = nullptr;
    HostInput* input = nullptr;
    std::uint32_t guest_activity = 0;
    std::uint32_t window_handle = 0;
    std::uint32_t queue_handle = 0;
};

std::mutex g_mutex;
// One entry per activity the framework created. Never cleared before onDestroy, because the
// framework calls back with the same pointer until then.
std::unordered_map<const ANativeActivity*, Attachment> g_attachments;

// Copies the entry rather than handing out a reference: the callbacks run on the UI thread while
// another thread may still be attaching, and a guest call must never hold this lock.
bool attachment_of(const ANativeActivity* activity, Attachment& out) {
    std::lock_guard<std::mutex> lock(g_mutex);
    const auto entry = g_attachments.find(activity);
    if (entry == g_attachments.end()) return false;
    out = entry->second;
    return true;
}

void deliver(const ANativeActivity* activity, GuestCallback slot, std::uint32_t argument) {
    Attachment attachment;
    if (!attachment_of(activity, attachment)) return;
    attachment.activities->deliver(attachment.guest_activity, slot, argument);
}

}  // namespace

bool NativeActivityGlue::read(std::uint64_t activity, Facts& facts) {
    const auto* native = reinterpret_cast<const ANativeActivity*>(static_cast<std::uintptr_t>(activity));
    if (native == nullptr) return false;
    facts.internal_data_path = native->internalDataPath != nullptr ? native->internalDataPath : "";
    facts.external_data_path = native->externalDataPath != nullptr ? native->externalDataPath : "";
    facts.obb_path = native->obbPath != nullptr ? native->obbPath : "";
    facts.sdk_version = native->sdkVersion;
    facts.activity_object = reinterpret_cast<std::uintptr_t>(native->clazz);
    facts.asset_manager = reinterpret_cast<std::uintptr_t>(native->assetManager);
    return true;
}

void NativeActivityGlue::attach(std::uint64_t activity, std::uint32_t guest_activity) {
    auto* native = reinterpret_cast<ANativeActivity*>(static_cast<std::uintptr_t>(activity));
    if (native == nullptr || native->callbacks == nullptr) return;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_attachments[native] = Attachment{this, &activities_, windows_, input_, guest_activity, 0, 0};
    }
    ANativeActivityCallbacks& callbacks = *native->callbacks;
    callbacks.onStart = &NativeActivityGlue::on_start;
    callbacks.onResume = &NativeActivityGlue::on_resume;
    callbacks.onSaveInstanceState = &NativeActivityGlue::on_save_instance_state;
    callbacks.onPause = &NativeActivityGlue::on_pause;
    callbacks.onStop = &NativeActivityGlue::on_stop;
    callbacks.onDestroy = &NativeActivityGlue::on_destroy;
    callbacks.onWindowFocusChanged = &NativeActivityGlue::on_window_focus_changed;
    callbacks.onNativeWindowCreated = &NativeActivityGlue::on_window_created;
    callbacks.onNativeWindowResized = &NativeActivityGlue::on_window_resized;
    callbacks.onNativeWindowRedrawNeeded = &NativeActivityGlue::on_window_redraw_needed;
    callbacks.onNativeWindowDestroyed = &NativeActivityGlue::on_window_destroyed;
    callbacks.onInputQueueCreated = &NativeActivityGlue::on_input_queue_created;
    callbacks.onInputQueueDestroyed = &NativeActivityGlue::on_input_queue_destroyed;
    callbacks.onContentRectChanged = &NativeActivityGlue::on_content_rect_changed;
    callbacks.onConfigurationChanged = &NativeActivityGlue::on_configuration_changed;
    callbacks.onLowMemory = &NativeActivityGlue::on_low_memory;
    log("NativeActivity callbacks installed for guest activity 0x%08x", guest_activity);
}

void NativeActivityGlue::on_start(ANativeActivity* activity) { deliver(activity, GuestCallback::OnStart, 0); }
void NativeActivityGlue::on_resume(ANativeActivity* activity) { deliver(activity, GuestCallback::OnResume, 0); }
void NativeActivityGlue::on_pause(ANativeActivity* activity) { deliver(activity, GuestCallback::OnPause, 0); }
void NativeActivityGlue::on_stop(ANativeActivity* activity) { deliver(activity, GuestCallback::OnStop, 0); }
void NativeActivityGlue::on_low_memory(ANativeActivity* activity) { deliver(activity, GuestCallback::OnLowMemory, 0); }

void NativeActivityGlue::on_configuration_changed(ANativeActivity* activity) {
    deliver(activity, GuestCallback::OnConfigurationChanged, 0);
}

void NativeActivityGlue::on_destroy(ANativeActivity* activity) {
    Attachment attachment;
    if (!attachment_of(activity, attachment)) return;
    attachment.activities->deliver(attachment.guest_activity, GuestCallback::OnDestroy, 0);
    attachment.activities->destroy(attachment.guest_activity);
    std::lock_guard<std::mutex> lock(g_mutex);
    g_attachments.erase(activity);
}

void* NativeActivityGlue::on_save_instance_state(ANativeActivity* activity, size_t* out_size) {
    Attachment attachment;
    if (!attachment_of(activity, attachment)) {
        if (out_size != nullptr) *out_size = 0;
        return nullptr;
    }
    return attachment.activities->save_instance_state(attachment.guest_activity, out_size);
}

void NativeActivityGlue::on_window_focus_changed(ANativeActivity* activity, int has_focus) {
    deliver(activity, GuestCallback::OnWindowFocusChanged, static_cast<std::uint32_t>(has_focus));
}

void NativeActivityGlue::on_window_created(ANativeActivity* activity, ANativeWindow* window) {
    Attachment attachment;
    if (!attachment_of(activity, attachment) || attachment.windows == nullptr) return;
    const std::uint32_t handle = attachment.windows->handle_for_window(window);
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        const auto entry = g_attachments.find(activity);
        if (entry != g_attachments.end()) entry->second.window_handle = handle;
    }
    attachment.activities->deliver(attachment.guest_activity, GuestCallback::OnNativeWindowCreated, handle);
}

void NativeActivityGlue::on_window_resized(ANativeActivity* activity, ANativeWindow* window) {
    (void)window;
    Attachment attachment;
    if (!attachment_of(activity, attachment)) return;
    attachment.activities->deliver(attachment.guest_activity, GuestCallback::OnNativeWindowResized,
                                   attachment.window_handle);
}

void NativeActivityGlue::on_window_redraw_needed(ANativeActivity* activity, ANativeWindow* window) {
    (void)window;
    Attachment attachment;
    if (!attachment_of(activity, attachment)) return;
    attachment.activities->deliver(attachment.guest_activity, GuestCallback::OnNativeWindowRedrawNeeded,
                                   attachment.window_handle);
}

void NativeActivityGlue::on_window_destroyed(ANativeActivity* activity, ANativeWindow* window) {
    (void)window;
    Attachment attachment;
    if (!attachment_of(activity, attachment)) return;
    // The guest is told before the handle dies, so it can still query the window it is losing.
    attachment.activities->deliver(attachment.guest_activity, GuestCallback::OnNativeWindowDestroyed,
                                   attachment.window_handle);
    std::lock_guard<std::mutex> lock(g_mutex);
    const auto entry = g_attachments.find(activity);
    if (entry != g_attachments.end()) entry->second.window_handle = 0;
}

void NativeActivityGlue::on_input_queue_created(ANativeActivity* activity, AInputQueue* queue) {
    Attachment attachment;
    if (!attachment_of(activity, attachment) || attachment.input == nullptr) return;
    const std::uint32_t handle = attachment.input->add_queue(queue);
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        const auto entry = g_attachments.find(activity);
        if (entry != g_attachments.end()) entry->second.queue_handle = handle;
    }
    attachment.activities->deliver(attachment.guest_activity, GuestCallback::OnInputQueueCreated, handle);
}

void NativeActivityGlue::on_input_queue_destroyed(ANativeActivity* activity, AInputQueue* queue) {
    (void)queue;
    Attachment attachment;
    if (!attachment_of(activity, attachment) || attachment.input == nullptr) return;
    attachment.activities->deliver(attachment.guest_activity, GuestCallback::OnInputQueueDestroyed,
                                   attachment.queue_handle);
    attachment.input->remove_queue(attachment.queue_handle);
    std::lock_guard<std::mutex> lock(g_mutex);
    const auto entry = g_attachments.find(activity);
    if (entry != g_attachments.end()) entry->second.queue_handle = 0;
}

void NativeActivityGlue::on_content_rect_changed(ANativeActivity* activity, const ARect* rect) {
    Attachment attachment;
    if (!attachment_of(activity, attachment) || rect == nullptr) return;
    attachment.activities->deliver_content_rect(attachment.guest_activity, rect->left, rect->top, rect->right,
                                                rect->bottom);
}

}  // namespace zb
