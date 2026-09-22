# Phase 7 part 2: NativeActivity, input and configuration

Status: draft for approval, 2026-09-22. Builds on `2026-09-17-native-surface-design.md`
(EGL, `ANativeWindow`, `ALooper`) and `2026-09-14-jni-bridge-design.md` (the guest `JNIEnv`,
the proxy library and host-to-guest calls).

## Goal

Run a guest whose activity **is** a `NativeActivity`: the Java side creates no views and
does nothing but hand the native code a window, an input queue and lifecycle events.

## Why this is the gate for Unity

Two 32-bit Unity titles were scanned with `tools/scan_apk.py`, which reads the class hierarchy
out of the dex:

```
Lane Racer      com.prime31.UnityPlayerNativeActivity
                  -> com.unity3d.player.UnityPlayerNativeActivity -> android.app.NativeActivity
Thomas Was Alone  com.neatplug.u3d.plugins.common.NPUnityPlayerActivity
                  -> com.unity3d.player.UnityPlayerActivity
                  -> com.unity3d.player.UnityPlayerNativeActivity -> android.app.NativeActivity
```

Both reach `android.app.NativeActivity`, and in the second case through a class named
`UnityPlayerActivity`, which sounds like an ordinary Java activity and is not. Unity of that
era always lands here, so no amount of searching for a "plain" 32-bit Unity game replaces this
work.

The observed failure is `UnsatisfiedLinkError: ANativeActivity_onCreate`. The framework's
`NativeActivity.onCreate` loads the library named by the `android.app.lib_name` meta-data, then
calls its own native `loadNativeCode`, which `dlopen`s **the path the class loader returned** and
looks up `ANativeActivity_onCreate` in it. That path is our arm64 proxy, which does not export it.

## Scope

In:
- `ANativeActivity_onCreate` in the proxy, a guest `ANativeActivity` and the 16 callbacks;
- the four `ANativeActivity_*` functions a guest calls (`finish`, `setWindowFlags`,
  `showSoftInput`, `hideSoftInput`);
- `AInputQueue_*` and the `AInputEvent`/`AKeyEvent`/`AMotionEvent` accessors;
- `AConfiguration_*`;
- a real host looper for a guest thread that attaches an input queue (see Threading);
- a `native-activity` section in the runtime report.

Out (with reasons):
- **`android_native_app_glue`.** It is source shipped inside the guest and already translated;
  nothing to do.
- **`ANativeWindow_lock`.** Unchanged from part 1: GL guests never use it.
- **`AChoreographer`, `APerformanceHint`, `ASurfaceControl`.** Later APIs; a guest of this era
  does not call them, and the first one that does will name itself in the report.
- **Text input beyond `showSoftInput`/`hideSoftInput`.** Soft-keyboard text arrives as key
  events through the same queue.

## Architecture

```
guest activity class (plugin dex, real ART)   extends android.app.NativeActivity
  |  framework loadNativeCode(proxy path, "ANativeActivity_onCreate")
  v
libzbproxy.so (arm64)   ANativeActivity_onCreate(host ANativeActivity*, void* state, size_t)
  |  ZBridge.onNativeActivityCreated  (Java, launcher)  -> HostNativeActivity
  v
HostNativeActivity (core/)
  |   builds the guest ANativeActivity struct in guest memory
  |   calls the guest ANativeActivity_onCreate on a borrowed carrier
  |   installs its own host callbacks into the host struct
  v
host callback (UI thread)  ->  borrow a carrier  ->  guest callback
```

The guest never sees a host pointer. Everything it receives is either guest memory we wrote or a
32-bit handle from the tables part 1 already uses.

### The guest `ANativeActivity`

Written into guest memory once, 40 bytes, in NDK field order: `callbacks`, `vm`, `env`, `clazz`,
`internalDataPath`, `externalDataPath`, `sdkVersion`, `instance`, `assetManager`, `obbPath`.

- `vm` and `env` are the guest `JavaVM` and `JNIEnv` the JNI bridge already synthesizes;
- `clazz` is a 32-bit JNI handle for the activity object, held as a global reference for the
  lifetime of the activity;
- the three paths are copied into guest memory as NUL-terminated strings;
- `assetManager` is the `AAssetManager` handle of part 1's asset unit;
- `instance` belongs to the guest and is never read by us;
- `callbacks` points at a 64-byte block of 16 guest function pointers, zeroed before the call and
  read back after it. A guest that leaves one null simply does not want that event.

### Callbacks

The 16 callbacks are called in NDK order, each with the guest activity pointer. Two need more
than forwarding:

- **`onSaveInstanceState`** returns a guest pointer and a size the guest allocated with its own
  `malloc`. The framework expects host memory it will `free`. We copy the bytes into host memory
  allocated with `malloc` and free the guest block through the guest allocator, because a guest
  block freed by the host heap corrupts both.
- **`onContentRectChanged`** takes a pointer to an `ARect` of four `int32_t`, which is written
  into guest memory per call.

`onNativeWindowCreated` and the other window callbacks receive the window handle from part 1's
handle table, created here and released when the framework says the window is gone.
`onInputQueueCreated` receives an input-queue handle from a table of the same kind.

### Input

`AInputQueue_*` and the event accessors are generated stubs over host calls, as GLES is. Events
are host objects; the guest sees a 32-bit event handle, valid between `getEvent` and
`finishEvent`. An accessor called on a handle that is no longer live is rejected and recorded,
never dereferenced.

`AInputQueue_getEvent` returns an event the guest must pass to `finishEvent`; a queue destroyed
with events outstanding drops them and records it. `preDispatchEvent` is forwarded as is: it is
what makes the soft keyboard work.

### Configuration

`AConfiguration_*` is ~30 small functions over a host `AConfiguration`. The guest gets a handle
from `AConfiguration_new`, and `AConfiguration_fromAssetManager` takes our asset-manager handle.
All of them are mechanical, so they are generated.

## Threading

The framework's callbacks arrive on the process main thread, which is a Java thread. It enters
the guest by borrowing a carrier, exactly as JNI calls already do, so the guest callback runs
with a guest stack and TLS.

The one new problem is the input queue. `AInputQueue_attachLooper` hands the queue to a looper,
and a host `AInputQueue` has no file descriptor we can poll ourselves, so our own looper cannot
serve it. But every guest thread **is** a real host thread, so when a guest thread attaches an
input queue we prepare a **real** Android looper on that host thread and attach the queue to it.
`ALooper_pollOnce` on such a thread then polls the real looper and our own registrations
together. A borrower already runs on a thread with a real looper and keeps the behaviour part 1
gave it.

This makes `HostLooper` a three-mode object: a guest thread with only our registrations (part 1),
a borrower on the real Android looper (part 1), and a guest thread that acquired a real looper
of its own (new). The third mode is the only one that can be polled for both kinds of source.

## Report

A `native-activity` section: whether `ANativeActivity_onCreate` was found and called, which
callbacks the guest installed, the lifecycle events delivered in order, window and input-queue
handles created and destroyed, the count of events delivered and finished, and the first
rejected accessor with its handle. A guest that goes quiet must be diagnosable from the phone,
because there is no logcat to rely on.

## Testing

Host, no device:
- `native_activity_test`: the guest struct's layout and contents, the callback table read back,
  each callback forwarded once with its arguments, `onSaveInstanceState` copied and the guest
  block freed, a null callback skipped;
- `input_queue_test`: mock queue and events, handle lifetime, an accessor on a finished event
  rejected, events outstanding at destruction;
- `configuration_test`: mock configuration, every generated shape;
- `host_looper_test` gains the third mode: a guest thread with a real looper serves both its own
  fd registrations and the queue.

Guest, on this machine: `zbnativeprobe.so` exports `ANativeActivity_onCreate`, fills the callback
table, and records what it receives; a host test drives the whole lifecycle against it and checks
the recording. This proves the struct, the callbacks and the marshaling end to end without a
device.

Device acceptance: **Thomas Was Alone reaches its first frame and takes touch input** on the
OnePlus 13. It is the simpler of the two scanned games: one plugin activity, Mono rather than
IL2CPP, three libraries. Lane Racer follows as the second case.

## Risks

- **The framework's `NativeActivity` may touch more of the activity than we model.** It calls
  back into Java on the same object; the guest class is the plugin's own, loaded by the plugin
  class loader, so this should hold, but the first device run will say.
- **Mono, not IL2CPP.** Mono generates code at run time and may use `mprotect`, signals and
  `mremap` in ways the IL2CPP path did not; `mremap` and the signal work are already in.
- **Input latency.** Every event crosses the bridge twice. Measure before optimizing; batching
  is possible but must not change ordering.
- **`onSaveInstanceState` allocation ownership** is the kind of detail that produces a heap
  corruption weeks later. The host test covers exactly this path.
- **Two activities, one process.** The guest runtime is process-lifetime, so a plugin that
  starts a second `NativeActivity` reuses the same guest state. The report must show it, and the
  design must not assume a single activity instance is the only one that ever exists.
