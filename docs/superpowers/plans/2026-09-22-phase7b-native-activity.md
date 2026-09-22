# Phase 7 part 2: NativeActivity, input and configuration implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development
> (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use
> checkbox (`- [ ]`) syntax for tracking.

**Goal:** A guest whose activity is a `NativeActivity` runs: the framework finds
`ANativeActivity_onCreate`, the guest gets a 32-bit activity, a window, lifecycle events, input
and configuration, and draws.

**Architecture:** `libzbproxy.so` gains the one export the framework looks for and reports it to
the launcher, which hands it to `HostNativeActivity` in `core/`. That unit owns the guest
`ANativeActivity` struct, the guest callback table, and the translation of every framework
callback into a guest call on a borrowed carrier. Input and configuration are generated stubs
over host calls (`HostInput`, `HostConfiguration`) behind abstract backends, with the real ones
in `core/android/`, exactly as EGL and windows are.

**Tech stack:** C++20, CMake+Ninja, Python generators, NDK r29 arm32 assembly stubs, ctest.

**Spec:** `docs/superpowers/specs/2026-09-22-native-activity-design.md`.

## Global constraints

- Host-call indices are append-only: sensors end the range at 375, everything new starts at 376.
- The guest never receives a host pointer: activity, window, queue, event and configuration all
  cross as 32-bit handles or as guest memory we wrote.
- Guest callbacks run only through the borrow path; never call guest code from a framework
  callback without a carrier.
- Never hold a unit's mutex across a guest call.
- Memory the guest allocated is freed by the guest allocator, and host memory by the host one.
- Every task ends in its own local commit with the whole suite green; do not push.
- Every task updates `AGENTS.md` with what it established and what it ruled out.

---

## File structure

Created:
- `core/include/zb/native_activity_hostcalls.h` — hand-written indices, as with sensors.
- `core/include/zb/host_native_activity.h`, `core/src/android/host_native_activity.cpp`.
- `core/include/zb/input_backend.h`, `core/include/zb/host_input.h`,
  `core/src/android/host_input.cpp`.
- `core/include/zb/configuration_backend.h`, `core/include/zb/host_configuration.h`,
  `core/src/android/host_configuration.cpp`.
- `core/android/input_driver_backend.{h,cpp}`, `core/android/configuration_driver_backend.{h,cpp}`
  — the real NDK-backed implementations.
- `guest/testlib/zbnativeprobe.c` — guest probe exporting `ANativeActivity_onCreate`.
- `tests/host/mock_input.h`, `tests/host/mock_configuration.h`.
- `tests/host/native_activity_test.cpp`, `tests/host/input_queue_test.cpp`,
  `tests/host/configuration_test.cpp`, `tests/host/native_activity_probe_test.cpp`.

Changed:
- `core/android/zbproxy.c` — the `ANativeActivity_onCreate` export.
- `android/launcher/.../ZBridge.java` and the launcher's native glue — the new entry point.
- `tools/gen_stubs.py` — input and configuration stub lists, appended last.
- `core/src/jni/proxy_runtime.cpp` — chain the new units.
- `core/include/zb/host_looper.h`, `core/src/android/host_looper.cpp` — the real-looper mode.
- `core/include/zb/runtime_report.h`, `core/src/runtime_report.cpp` — the report section.

---

## Task 1: the proxy export and the launcher entry point

- [ ] `core/android/zbproxy.c` exports `ANativeActivity_onCreate(ANativeActivity*, void*, size_t)`.
      It calls `static int com.zettabridge.core.ZBridge.onNativeActivityCreated(long activity,
      long savedState, long savedStateSize, String proxyPath)` and, when that returns nonzero,
      leaves the activity as the bridge configured it.
- [ ] A failure path that a user can see: no bridge, a throwing bridge or an unknown proxy path
      logs and leaves the callbacks untouched, so the framework reports a normal failure rather
      than crashing in a half-built activity.
- [ ] `tools/check_zbproxy.py` still passes: the proxy links against nothing of ours.
- [ ] `ZBridge.onNativeActivityCreated` forwards to the runtime; the launcher records the call in
      the runtime report even when nothing else works yet.

**Verify:** `tools/check_zbproxy.py`; a host unit test that loads the proxy on this machine and
checks the export exists and calls the bridge method with the arguments it was given.

## Task 2: the guest activity struct and callbacks

- [ ] `HostNativeActivity` writes the 40-byte guest `ANativeActivity` (NDK field order) and a
      zeroed 64-byte callback block into guest memory.
- [ ] Guest `JavaVM`/`JNIEnv` come from the JNI bridge; `clazz` is a global reference held for the
      activity's lifetime; the three paths are copied into guest memory; `assetManager` is the
      asset handle.
- [ ] The guest `ANativeActivity_onCreate` is called on a borrowed carrier, and the callback block
      is read back afterwards.
- [ ] Every framework callback is translated: the 13 plain ones forward, `onContentRectChanged`
      writes an `ARect` into guest memory, and `onSaveInstanceState` copies the guest block into
      host memory and frees the guest block through the guest allocator.
- [ ] A null guest callback is skipped, not called through zero.
- [ ] Window callbacks use part 1's window handle table; the handle is released when the framework
      destroys the window.

**Verify:** `native_activity_test` against a mock: layout and contents field by field, each
callback forwarded once with its arguments, the save-state copy and free, a null callback skipped,
and an unknown activity handle rejected.

## Task 3: input queues and events

- [ ] `tools/gen_stubs.py` appends the `AInputQueue_*` and `AInputEvent`/`AKeyEvent`/
      `AMotionEvent` names from the NDK headers; indices start at 376.
- [ ] `InputBackend` (abstract) and `HostInput`: queue and event handle tables, an event valid
      only between `getEvent` and `finishEvent`, `preDispatchEvent` forwarded as is.
- [ ] An accessor on a stale handle is rejected and recorded; nothing is dereferenced.
- [ ] A queue destroyed with events outstanding drops them and records how many.
- [ ] `core/android/input_driver_backend` implements the backend over the real NDK.

**Verify:** `input_queue_test` with a mock queue: handle lifetime, every accessor shape, a stale
handle rejected, outstanding events at destruction, and guest pointer bounds on the out
parameters.

## Task 4: the real looper mode

- [ ] A guest thread that attaches an input queue acquires a real Android looper on its own host
      thread; `HostLooper` gains this third mode next to the guest-only and borrower modes.
- [ ] `ALooper_pollOnce` on such a thread polls the real looper and our own registrations
      together, with the timeout the guest asked for and no busy waiting.
- [ ] `AInputQueue_attachLooper`/`detachLooper` reach the real looper; a queue attached on a
      borrower keeps part 1's behaviour.

**Verify:** `host_looper_test` gains the third mode: a guest thread with a real looper serves both
its own fd registrations and a queue, wake and timeout still behave, and the two existing modes
are unchanged.

## Task 5: configuration

- [ ] `tools/gen_stubs.py` appends the `AConfiguration_*` names; `ConfigurationBackend` and
      `HostConfiguration` with a handle per configuration.
- [ ] `AConfiguration_fromAssetManager` takes the asset-manager handle; `AConfiguration_new`/
      `delete` own the host object.
- [ ] `core/android/configuration_driver_backend` implements it over the real NDK.

**Verify:** `configuration_test` against a mock: every generated shape, handle lifetime, and a
bad handle rejected.

## Task 6: the guest probe, end to end on this machine

- [ ] `guest/testlib/zbnativeprobe.c` exports `ANativeActivity_onCreate`, fills the callback
      table, records every callback it receives with its arguments, allocates a save-state block,
      and reads back the fields of its activity.
- [ ] `native_activity_probe_test` drives the whole lifecycle against the real guest probe through
      the library runtime with mock backends and checks the recording.

**Verify:** the probe test fails when any one of Task 2's translations is disabled, and passes
with them. `tools/run_guest_tests.sh` and the whole ctest suite stay green.

## Task 7: report and launcher wiring

- [ ] A `native-activity` report section: the export found, callbacks installed, lifecycle events
      in order, window and queue handles, events delivered and finished, first rejected accessor.
- [ ] The launcher routes a plugin activity that descends from `NativeActivity` to a stub that is
      itself a `NativeActivity`, so the framework path runs; `tools/scan_apk.py`'s chain walk is
      the rule for deciding that.
- [ ] `PluginRecord` remembers that a plugin is a native-activity plugin, so the library screen
      can say so.

**Verify:** `runtime_report_test` covers the new section; the launcher builds; the bundle check
passes.

## Task 8: device bring-up

- [ ] Build the arm64 core, the bundle and the APK; import Thomas Was Alone.
- [ ] Acceptance: it reaches its first frame and takes touch input on the OnePlus 13.
- [ ] Record the run in `docs/phase7b-acceptance.md`: what worked, what was ruled out, and the
      exact report lines that prove it.
- [ ] Lane Racer as the second case; differences go into `AGENTS.md`, not into special cases in
      the code.

**Verify:** the acceptance document and the report from the device.
