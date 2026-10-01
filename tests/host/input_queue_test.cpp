// Input queues and events: handles live exactly as long as the event does, accessors marshal by
// shape, a stale handle is rejected instead of followed, and a destroyed queue takes its
// outstanding events with it.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>

#include <dynarmic/interface/exclusive_monitor.h>
#include <sys/mman.h>

#include "check.h"
#include "mock_input.h"
#include "zb/host_input.h"
#include "zb/input_hostcalls.h"
#include "zb/library_runtime.h"
#include "zb/runtime_report.h"

namespace {

constexpr std::uint32_t kData = 0x20000;

std::uint32_t call_input(zb::HostInput& input, zb::GuestThread& thread, std::uint32_t index,
                         std::initializer_list<std::uint32_t> args) {
    auto& regs = thread.regs();
    regs[0] = regs[1] = regs[2] = regs[3] = 0;
    unsigned position = 0;
    for (std::uint32_t value : args) regs[position++] = value;
    CHECK(input.handle_host_call(index, thread));
    return regs[0];
}

}  // namespace

int main() {
    zb::runtime_report().clear();
    zb::LibraryRuntime runtime;
    CHECK(runtime.memory().map_anon(kData, 0x1000, PROT_READ | PROT_WRITE));
    MockInput backend;
    zb::HostInput input(runtime, backend,
                        [](zb::GuestThread&, std::uint32_t, std::uint64_t& real) {
                            real = 0x5100;  // the real looper the guest's handle resolves to
                            return true;
                        });
    Dynarmic::ExclusiveMonitor monitor(1);
    zb::GuestThread thread(runtime.memory(), &monitor, 0, false, zb::kCarrierCodeCacheSize);

    // Indices outside the input range belong to another unit.
    CHECK(!input.handle_host_call(zb::ZB_INPUT_HC_FIRST - 1, thread));
    CHECK(!input.handle_host_call(zb::ZB_INPUT_HC_LAST + 1, thread));

    const std::uint32_t queue = input.add_queue(backend.queue());
    CHECK(queue != 0 && input.queue_for(queue) == backend.queue());
    CHECK(input.add_queue(nullptr) == 0);

    // An empty queue reports no events, and getEvent says so without handing out a handle.
    CHECK(call_input(input, thread, zb::ZB_INPUT_HC_AInputQueue_hasEvents, {queue}) == 0);
    std::memset(runtime.memory().base() + kData, 0xAB, 4);
    CHECK(call_input(input, thread, zb::ZB_INPUT_HC_AInputQueue_getEvent, {queue, kData}) ==
          static_cast<std::uint32_t>(-1));
    std::uint32_t handle = 0;
    std::memcpy(&handle, runtime.memory().base() + kData, sizeof handle);
    CHECK(handle == 0);

    // A queued event arrives as a handle written through the guest's own pointer.
    backend.queue_event({2 /* motion */, 11, 2.5f, 0x0000'0007'0000'0009LL});
    CHECK(call_input(input, thread, zb::ZB_INPUT_HC_AInputQueue_hasEvents, {queue}) == 1);
    CHECK(call_input(input, thread, zb::ZB_INPUT_HC_AInputQueue_getEvent, {queue, kData}) == 0);
    std::memcpy(&handle, runtime.memory().base() + kData, sizeof handle);
    CHECK(handle != 0);
    CHECK(zb::runtime_report().text().find(
              "jni-input-queue: get=1 pre-dispatched=0 finish=0 release=0 outstanding=1") !=
          std::string::npos);

    // Accessors: a word result, a float in a core register (AAPCS softfp), and a 64-bit pair.
    CHECK(call_input(input, thread, zb::ZB_INPUT_HC_AInputEvent_getType, {handle}) == 2);
    CHECK(call_input(input, thread, zb::ZB_INPUT_HC_AInputEvent_getDeviceId, {handle}) == 11);
    const std::uint32_t bits = call_input(input, thread, zb::ZB_INPUT_HC_AMotionEvent_getX, {handle, 1});
    float x = 0;
    std::memcpy(&x, &bits, sizeof x);
    CHECK(x == 3.5f);
    call_input(input, thread, zb::ZB_INPUT_HC_AMotionEvent_getEventTime, {handle});
    CHECK(thread.regs()[0] == 9 && thread.regs()[1] == 7);

    // An event the guest never received is rejected and never reaches the backend.
    const std::size_t before = backend.calls.size();
    CHECK(call_input(input, thread, zb::ZB_INPUT_HC_AInputEvent_getType, {handle + 1000}) == 0);
    CHECK(backend.calls.size() == before);

    // A guest output pointer that is not writable fails the call before an event is taken.
    CHECK(call_input(input, thread, zb::ZB_INPUT_HC_AInputQueue_getEvent, {queue, 0xDEAD0000}) ==
          static_cast<std::uint32_t>(-1));

    // Finishing an event ends its handle; using it afterwards is rejected.
    CHECK(call_input(input, thread, zb::ZB_INPUT_HC_AInputQueue_finishEvent, {queue, handle, 1}) == 0);
    CHECK(backend.finished.size() == 1 && backend.last_handled == 1);
    CHECK(zb::runtime_report().text().find(
              "jni-input-queue: get=1 pre-dispatched=0 finish=1 release=0 outstanding=0") !=
          std::string::npos);
    CHECK(call_input(input, thread, zb::ZB_INPUT_HC_AInputEvent_getType, {handle}) == 0);
    CHECK(call_input(input, thread, zb::ZB_INPUT_HC_AInputQueue_finishEvent, {queue, handle, 0}) == 0);
    CHECK(backend.finished.size() == 1);  // the second finish never reached the backend

    // A pre-dispatched event belongs to the framework: its handle ends there, and the guest must
    // not finish it.
    backend.pre_dispatch_result = 1;
    backend.queue_event({});
    CHECK(call_input(input, thread, zb::ZB_INPUT_HC_AInputQueue_getEvent, {queue, kData}) == 0);
    std::memcpy(&handle, runtime.memory().base() + kData, sizeof handle);
    CHECK(call_input(input, thread, zb::ZB_INPUT_HC_AInputQueue_preDispatchEvent, {queue, handle}) == 1);
    CHECK(call_input(input, thread, zb::ZB_INPUT_HC_AInputEvent_getType, {handle}) == 0);
    backend.pre_dispatch_result = 0;

    // Attaching ties the queue to this thread's real looper; detaching undoes it.
    // The guest's own data word travels with the registration and comes back from the poll, so
    // the guest recognizes its source; nothing of the host crosses.
    CHECK(call_input(input, thread, zb::ZB_INPUT_HC_AInputQueue_attachLooper,
                     {queue, 0x1234, 42, 0, 0xCAFE}) == 0);
    CHECK(backend.attached && backend.attached_ident == 42);
    CHECK(backend.attached_data == reinterpret_cast<void*>(static_cast<std::uintptr_t>(0xCAFE)));
    CHECK(backend.attached_looper == 0x5100);
    // A queue holding events reports itself to the looper it was attached to: an event that came
    // through Java wakes the looper instead of marking a descriptor, and a guest waiting for its
    // ident would otherwise wait forever (Unity did, for the five seconds the framework allows).
    backend.queue_event({});
    const auto ready = input.ready_on(0x5100);
    CHECK(ready.has_value() && ready->ident == 42 && ready->data == 0xCAFE);
    CHECK(!input.ready_on(0x9999).has_value());

    CHECK(call_input(input, thread, zb::ZB_INPUT_HC_AInputQueue_detachLooper, {queue}) == 0);
    CHECK(!backend.attached);
    // Detached: it no longer belongs to that looper.
    CHECK(!input.ready_on(0x5100).has_value());

    // A destroyed queue takes the events the guest never finished, and says how many.
    backend.queue_event({});
    backend.queue_event({});
    CHECK(call_input(input, thread, zb::ZB_INPUT_HC_AInputQueue_getEvent, {queue, kData}) == 0);
    std::memcpy(&handle, runtime.memory().base() + kData, sizeof handle);
    CHECK(call_input(input, thread, zb::ZB_INPUT_HC_AInputQueue_getEvent, {queue, kData}) == 0);
    CHECK(input.remove_queue(queue) == 2);
    CHECK(input.queue_for(queue) == nullptr);
    CHECK(call_input(input, thread, zb::ZB_INPUT_HC_AInputEvent_getType, {handle}) == 0);
    CHECK(call_input(input, thread, zb::ZB_INPUT_HC_AInputQueue_hasEvents, {queue}) == 0);
    CHECK(input.remove_queue(queue) == 0);

    // The JNI conversions answer "no object" rather than handing the guest a host reference.
    CHECK(call_input(input, thread, zb::ZB_INPUT_HC_AInputQueue_fromJava, {0, 0}) == 0);

    std::puts("input_queue_test PASS");
    return 0;
}
