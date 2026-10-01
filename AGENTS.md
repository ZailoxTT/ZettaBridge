# AGENTS.md: handoff for Codex (2026-09-14)

Read `CLAUDE.md` first. It holds the architecture, gotchas, build commands and working
conventions, and all of it applies to you. The user chats in Russian; repo files are
English and ASCII only.

## State

- **Phases 1-3 done.** T1-T6 pass: host tests, and the guest suite in Termux and inside
  an app process on the OnePlus 13. Branch `phase1-zbrun`.
- **Phase 0 launcher works on the phone** and is merged into `phase1-zbrun`
  (`android/launcher/`, phone copy `/sdcard/AndroidIDEProjects/ZettaBridge`).
- **Part 1 (JNI bridge) spec approved:**
  `docs/superpowers/specs/2026-09-14-jni-bridge-design.md`. Follow it; do not redesign
  without asking the user.
- **Phase 4a (JNI host units) done.** All 10 host tests and all 9 guest tests pass.
- **Phase 4b done.** All six tasks (nested call frame, stop dispatch, `zbhost`
  protocol, service-thread runtime, carriers, regression/docs) are committed and
  reviewed. Host tests are 15/15. Phase 4c followed.
- **Phase 4c done (2026-09-15).** Guest `JNIEnv`/`JavaVM`, host JNI calls against
  `JniBackend`, Java -> guest dispatch, `RegisterNatives`, attach/detach; committed directly
  to `phase1-zbrun` (see "Phase 4c done" below). Host tests are 18/18. Next is plan 4d.
- **Phase 4 is complete and accepted on the OnePlus 13.** See `docs/phase4-acceptance.md`.
- **Phase 5 Tasks 1-5 are done (2026-09-16).** Registry generation, scalar and pointer
  marshaling, COMPSIZE, pixels, uniforms, bounded names, `glGetString` and `glShaderSource` are
  implemented, as are client-side vertex arrays and both draw paths: all 142/142 GLES calls are
  functional. Host tests are 34/34. Next is Phase 5 Task 6, the arm32 guest probe and full bridge
  test.
- Work continues on local branch `codex/phase4d-launcher`, based on `phase1-zbrun`.
- Commit locally after every task and update this file. Push only with the user's agreement.

## Phase 4a completed (JNI host units)

Plan: `docs/superpowers/plans/2026-09-14-phase4a-jni-host-units.md`. Its 6 TDD tasks
were implemented in order, with the full host suite run before each local commit.

**Progress (2026-09-14):**

| Task | State | Commits |
|---|---|---|
| 1. `Java_*` name decoding | done, reviewed | `5eeada8`, `96250bc` (strict ART-canonical decoding after review; the plan's Task 1 code was synced) |
| 2. Signature -> shorty | done, reviewed | `cbfe969`, `7558c25` (strict class-name scan, 255-dimension limit; plan synced), `44362e8` (docs) |
| 3. AAPCS64 -> AAPCS32 marshaling | done, reviewed | `fe12334`, `69051d4` (strict invalid-shorty failure and additional ABI coverage) |
| 4. Handle tables | done | `07c2443` |
| 5. Thunk pool (`thunks.S`), dispatcher, slots | done | `1ffb543` |
| 6. Regression run + docs | done | `3d7d15e` |

All Phase 4a review notes were folded into the implementation, tests, plan, and spec.

## Phase 4b done

The executable TDD plan is
`docs/superpowers/plans/2026-09-14-phase4b-library-runtime.md`. All six tasks are
complete. The plan's "Review decisions" table (D1-D14) says where each review decision
lands; the specs were amended to match.

| Task | State | Commit |
|---|---|---|
| 1. Nested host-to-guest call frame | done, reviewed, review fixes | `d0708d8`, `a15b86c` |
| 2. Reusable Process stop dispatch | done, reviewed, review fixes | `fcaef77`, `a15b86c` |
| 3. Fixed `zbhost` service protocol | done, reviewed | `49608ec` |
| 4. Service-thread library runtime | done, reviewed, review fix | `d3b7119`; P1 fix `5e21137` |
| 5. Carrier leases and guest-tid routing | done | pre-fix `1c3d145`; carriers `75015d9`; review fix `54c6776` |
| 6. Regression, Android link, and docs | done | this commit |

`a15b86c` fixed: IT/E bits cleared on call entry, `call_depth`, thread exit inside a call
ends the host process, stray or wrong-`sp` return svc is SIGILL, shared `after_stop`,
code cache size parameter, guest tid in crash reports, `_Exit` in `check.h`, and the
extended `guest_call_test`.

Phase 4a hardening after review (thunk abort/CFI/exception barrier, handle serials and LIFO
reclaim) landed on this branch before Task 4.

The Task 1 review note (observable register mutation, handler-false after execution) is
done in `a15b86c`.

The Superpowers SDD scratch ledger and generated Task 1-6 briefs are under
`.superpowers/sdd/2026-09-14-phase4b-library-runtime/` in the Phase 4b worktree. The
directory is intentionally git-ignored.

The isolated worktree uses an ignored `sysroot` symlink. Its Dynarmic submodule has
the same five-file uncommitted baseline patch as the main checkout. That patch is
required: a clean recorded Dynarmic revision fails `fault_pc_test` and encounters an
unsupported `ldab` in the Android linker. Never stage the submodule pointer or modify
that patch as part of Phase 4b.

Last verified at `54c6776` (Task 6 regression):

```text
ctest --test-dir build/host                  15/15 PASS
tools/build_guest.sh                         PASS
tools/run_guest_tests.sh                     all 9 cases PASS (Orange Roulette APK
                                              symlinked into the worktree)
library_runtime_test, 20-run loop            0 failures
Android arm64 zbridge + zbrun                link OK
```

## After Phase 4b: plans 4c and 4d

Write each plan after 4b lands, using the real interfaces. The scope and the review
notes (per-method `RegisterNatives`, the `!` prefix, host-computed shorties, slot
release only for never-bound slots) are at the end of the 4a plan under "Following
plans". Use the same format as the 4a plan: TDD tasks with complete code.

- **4c:** generated guest `JNIEnv`, host JNI backend, and mock-JNI host test.
- **4d:** ART proxy loading, per-method registration, launcher integration, T7, and
  the Orange Roulette phone smoke test. This is the first end-to-end 32-bit launcher
  milestone.

## Practical notes

- **Build and test:**
  ```
  ninja -C build/host; ctest --test-dir build/host --output-on-failure; tools/build_guest.sh; tools/run_guest_tests.sh
  ```
- **Android core build:** see CLAUDE.md. Android binaries cannot run on this machine,
  so device tests go through the user.
- **Java compile check:** `javac` against `~/android-sdk/platforms/android-36/android.jar`.
  The launcher also needs a stub of `org.lsposed.hiddenapibypass.HiddenApiBypass`.
- **OxygenOS drops third-party app logs in logcat.** Report errors on screen, to the
  clipboard, or to a file (see `android/launcher/.../Diagnostics.java`).
- **Every app manifest must remove the `LogWireInitializer` provider** that AndroidIDE
  injects (see CLAUDE.md).
- **Commit messages:** plain English, imperative subject line.

## Phase 4b status (2026-09-14, Task 6 complete)

*(The 2026-09-15 "Claude limit reached mid-task" handoff that used to be here is
obsolete: the Task 4-5 work it described as a prototype is committed at `d3b7119`,
`5e21137`, `1c3d145`, `75015d9`, and `54c6776`, and Task 6 below closes out the plan.)*

- **Task 4:** `d3b7119`. Its P1 fix (retire the process signal target safely) is `5e21137`.
- **Task 5:** `1c3d145` (clear thread-local before freeing cloned threads) and `75015d9`
  (carriers). The review fix (tkill/tgkill post under the registry lock) is `54c6776`.
- **Task 6:** this commit (regression, Android link, docs).
- **Verification:** 15/15 host tests, a 20-run repeated `library_runtime_test` loop with
  0 failures, the guest suite passes (Orange Roulette's `or_dlopen_dynamic` included),
  and the Android `zbridge`/`zbrun` targets link.
- **Next:** plan 4c (guest `JNIEnv`). Then merge the `codex/*` branches into
  `phase1-zbrun`.
- **Deferred follow-up (pre-existing, minor).** `Process::unregister_thread` releases the
  processor id before the real thread's `GuestThread`/JIT is destroyed. Task 5 fixed this
  ordering for borrowers only.

## Launcher bug: plugin resources not found (fixed 2026-09-15, device retest pending)

- **repostzap.** `Resources$NotFoundException` for a string that exists in its APK. Its
  Compose code localizes through `createConfigurationContext`, which returned a context
  with the launcher's resources.
- **avtobuy (Flutter).** No asset loads (`assets/cities.bin`, icon font). The Flutter engine
  takes its AssetManager from `createPackageContext(getPackageName())`.
- **Fix.** `PluginContext` wraps every derived context (configuration, display, window,
  attribution, `createContext`, device-protected storage) and package contexts for its own
  package in a `PluginContext` with plugin resources.

## Phase 4c done

Record: `docs/superpowers/plans/2026-09-15-phase4c-guest-jnienv.md`. Prototyped and verified in
`.worktrees/proto-4c`, then committed along these boundaries (each commit builds and passes
the host suite):

| Task | Commit |
|---|---|
| 4c-1.1 JNI protocol and `tools/gen_jni.py` generated tables | `2fb99a9` |
| 4c-1.2 `JniBackend` interface and mock JVM | `e9d2731` |
| 4c-1.3 `HostJni` core, object host calls, guest `libzbjni.so`, `jni_bridge_test` | `81887b9` |
| 4c-1.4 `Call*Method` and field host calls | `dbc6c2e` |
| 4c-1.5 string, array, direct buffer host calls | `608a21d` |
| 4c-2.1 `NativeSlots::release` | `48a774f` |
| 4c-2.2 Java -> guest dispatcher and `RegisterNatives` | `6b607b2` |
| 4c-2.3 `GetEnv`, `AttachCurrentThread`, `DetachCurrentThread` | `c567611` |
| 4c-2.4 `JniEnvBackend` over the real `JNIEnv` (compile-only) | `43ec3e2` |

## HANDOFF 2026-09-15 (after Phase 4c)

- **Phase 4c is committed** on `phase1-zbrun` (`2fb99a9`..`30c811c`, 10 commits). Last verified
  results:
  - host tests 18/18;
  - guest tests 9/9, including Orange Roulette `or_dlopen_dynamic`;
  - `jni_bridge_test` passed 20 of 20 repeated runs;
  - `tools/gen_jni.py --check` passes;
  - the Android build links.
- **Code review of 4c:** a Sonnet review of `2141dc6..30c811c` was running when this note was
  written. If its findings are not recorded below, run a new review before building 4d on top.
  Focus on:
  - the dispatcher thread choice and `thread_local` cleanup at thread exit;
  - `JniCall` argument capture and guest pointer bounds;
  - integer overflow in buffer sizes;
  - `NativeSlots::release`;
  - JNI misuse in the real backend (`core/android/jni_env_backend.*`, compile-only).
- **Prototype worktree:** `.worktrees/proto-4c` is detached, and its contents are already
  committed. It can be removed.
- **Next: plan 4d (Android integration).** Inputs gathered:
  - The arm32 sysroot is 10 files / 4 MB. `zbhost`, `libzbjni.so` and `libzbcompat.so` are under
    50 KB. Bundle all of them in the launcher APK; no download step is needed.
  - Port `tools/fix_guest_lib.py` (120 lines) to C++ in `core`, so the launcher (through JNI),
    zbrun and the tests share it. It handles absolute `DT_NEEDED` -> basename and `DT_TEXTREL`
    -> the `DT_ZB_TEXTREL` marker.
  - Orange Roulette libs:
    - TEXTREL in `libApplicationMain`, `liblime`, `libopenal`;
    - absolute `DT_NEEDED` in `libApplicationMain`, `liblime`;
    - `libregexp`, `libstd`, `libzlib` need no fixups.
  - 4d scope (spec part 1, section 1):
    - `libzbproxy.so`;
    - `ZBridge.onProxyLoaded`;
    - binding `Java_*` exports through the real `RegisterNatives`;
    - guest `JNI_OnLoad`;
    - launcher class loader: delegate `com.zettabridge.core.*` to the launcher, and
      `findLibrary` returns proxies for armeabi libs;
    - extracting and fixing armeabi libs at import;
    - device test T7 and the Orange Roulette smoke test.
- **Open risks from 4c:**
  - carrier cost: 2 processor ids + ~34 MiB JIT per Java thread calling natives, so a pool may be
    needed;
  - `thread_local` destructor ordering against ART detach, untested on device;
  - buffers are always copied.

## HANDOFF addendum (Claude at 90% limit)

- The Sonnet review of Phase 4c may not finish before the limit. Codex: run your own code review of `2141dc6..30c811c` first, using the focus list above, and fix any Critical or Important findings before plan 4d.
- **The `.worktrees/proto-4c` working tree differs from the committed 4c code.** Diff stat vs `30c811c` over `core guest tests tools`:  24 files changed, 6480 deletions(-).
  - Treat `phase1-zbrun` as the source of truth.
  - Before deleting the worktree, check `git -C .worktrees/proto-4c diff 30c811c` for any fix that is not yet committed. Port such fixes on purpose.
- **Next after the review: write plan 4d** (inputs above). Use the lean style of the 4c record: task list, decisions, tests, acceptance. Do not reproduce full code per task. Implement it task by task with a commit per task.

## Phase 4c review result (Sonnet, reviewed committed 30c811c) - fix these first, then plan 4d

**Verdict:** With fixes. Everything else matches the amended spec, and all earlier review decisions hold.

1. **[Important] Reused native slot is published without synchronization.**
   - Where: `core/src/jni/native_thunks.cpp`.
   - `NativeSlots::allocate` writes `targets_[reused]` under the mutex but never touches `count_`, which is what lock-free `target()` relies on.
   - Fix: add a per-slot atomic ready/generation flag. `allocate` stores it after the write; `target()` loads it before reading.
2. **[Important] 32-bit size overflow in the guest buffer allocator.**
   - Where: `guest/zbjni/zbjni.c` `zbjni_buffer_new`.
   - `((size_t)length + 1) * size` wraps on arm32 for J/D arrays (length around 2^29). The host is then told a small allocation is several GB.
   - Fix: compute the size in 64-bit and return NULL if it does not fit.
3. **[Important] `NewDirectByteBuffer` capacity is not checked against `INT32_MAX`.**
   - Where: `core/src/jni/host_jni_data.cpp`, and `core/android/jni_env_backend.cpp` passes it through unchanged.
   - Fix: validate `[0, INT32_MAX]` in `host_jni_data.cpp` before calling the backend.
4. **[Minor] Leaked guest env.** `JniThread` destructor (`core/src/jni/host_jni.cpp`) never frees the guest env of a guest pthread that attached without detaching.
5. **[Minor] Misleading abort message.** `dispatch_native` (`core/src/jni/host_jni_natives.cpp`) says "has no target" when HostJni itself is missing.
6. **[Housekeeping] Delete the prototype worktree.** `.worktrees/proto-4c` is stale, superseded prototype code. Delete it with `git worktree remove --force .worktrees/proto-4c` (it has nothing uncommitted of value).

After fixing items 1-3 (with tests), run the full suite, `tools/gen_jni.py --check` and the Android link, then write plan 4d.

## Phase 4c review fixes done (2026-09-15)

Implemented on `codex/phase4c-review-fixes` with an observed RED test before each
production change:

| Finding | Commit | Result |
|---|---|---|
| Reused native-slot publication | `9647ea8` | per-slot release/acquire ready flag; released slots are hidden until republished |
| arm32 JNI buffer size overflow | `007f582` | 64-bit checked total; oversized copied arrays return `NULL` |
| direct-buffer capacity bound | `76e00be` | values outside `[0, INT32_MAX]` fail before backend dispatch |
| Recycled mock thread ids (found by stress verification) | `67285cb` | mock JNIEnv ownership uses a unique thread-lifetime token |

The stale `.worktrees/proto-4c` worktree was compared file-by-file with `30c811c`;
all project files matched, so it was removed as instructed.

Fresh verification after all fixes:

```text
tools/build_guest.sh                         PASS
ctest --test-dir build/host                  18/18 PASS
jni_bridge_test, 20-run loop                 20/20 PASS
tools/run_guest_tests.sh                     all 9 PASS, including Orange Roulette
tools/gen_jni.py --check                     PASS
Android arm64 zbridge + zbrun                link OK
```

Still open, non-blocking Phase 4c review minors:

- a guest pthread that attaches and exits without `DetachCurrentThread` leaks its guest env;
- `dispatch_native` says "has no target" when the process-wide `HostJni` pointer is missing.

Next: write the Phase 4d plan, then implement proxy loading, ART binding, T7 and the
Orange Roulette smoke launch.

## Phase 4d Task 3 done (2026-09-15)

- Task 3 (real ART discovery backend): `JniEnvBackend::find_declared_natives` loads through the
  retained plugin loader, enumerates `getDeclaredMethods`, and builds exact descriptors with
  `zb/jni_descriptor.h`, which is tested on the host by `jni_descriptor_test`.
  - Seam `zbjni_reflection_compile_test` links with `--no-undefined`; `zbridge` links in the
    Android build.
  - `ReflectionSmoke.java` compiles against android-36.
  - Real ART behavior is checked in Task 7.
- Task 4 (standalone arm64 proxy): `core/android/zbproxy.c` -> `libzbproxy.so` (5.8 KB, NEEDED
  liblog/libdl/libc, exports only `JNI_OnLoad`) calls `static int ZBridge.onProxyLoaded(String)`;
  0 means `JNI_VERSION_1_6`, 1.2/1.4/1.6 pass, anything else or an exception gives `JNI_ERR`.
  - `zbproxy_fake_jni_test` drives it on the host; `tools/check_zbproxy.py` runs after every
    Android link and as `zbproxy_structure_test` (skipped when not built).
  - ART's `JVM_NativeLoad` clears the pending exception, so Task 5 must record failure detail itself.

## Phase 4d Task 5 done (2026-09-15)

- Portable state machine `zb::ProxyRuntime` + real graph `zb::GuestJniEngine` (LibraryRuntime,
  HostJni chained before start, JniLoader) in `core/include/zb/proxy_runtime.h`; Android glue
  `core/android/guest_jni_runtime.*` (JniEnvBackend, never destroyed) and `zbridge_jni.cpp`.
- Java API (`com.zettabridge.core.ZBridge`, launcher copy): `activatePlugin(String pluginRoot, int
  targetSdk, ClassLoader)` throws IllegalStateException; `onProxyLoaded(String)`; `loadError(String)`
  and `lastLoadError()` return the stored message or null; `fixGuestLibrary(String)` returns
  `unchanged` / `changed: ...` / `skipped: ...` or throws IOException.
- Decisions:
  - Class loader comes from an explicit `activatePlugin` call before plugin code runs.
  - Memoization is keyed by the realpath of the proxy; successes and failures are both final.
  - One plugin per process; a failed load or start needs a new `:guest` process (ART never reruns
    `JNI_OnLoad` for a path).
  - zbhost runs as `<targetSdk> libzbjni.so` with only
    `LD_LIBRARY_PATH=<files>/zb/guest/lib:<files>/plugins/<pkg>/lib`.
- Layout: `<files>/zb/{sysroot,guest/zbhost,guest/lib}`, `<files>/plugins/<pkg>/{lib,proxy}`.
- Tests: `proxy_runtime_test` (fake engine, all eight cases) and `guest_jni_engine_test_{load,
  preload-failure}` (real guest over MockJvm). Host 27/27, guest 9/9, Android links.

## Repository (2026-09-15)

- Private GitHub repo: https://github.com/ZailoxTT/ZettaBridge (GPL-3.0, README.md plus
  README.ru.md).
- Local `phase1-zbrun` tracks `origin/main`.
- Do not push without the user's agreement.
- Never commit `cc`, `cod`, screenshots or APKs; they are in `.gitignore`.
- Before the repo goes public, check `CLAUDE.md`/`AGENTS.md` for anything private.

## HANDOFF 2026-09-15 late (Claude at 85% limit)

- **Task 5** (Android guest runtime) was being implemented by a Claude Opus agent.
  - If there is no `android: connect proxy loads to the guest JNI runtime` commit, inspect the uncommitted changes and the plan's Task 5 checkboxes before continuing.
  - Keep its risk list: ART clears the `JNI_OnLoad` exception, so preserve the detailed error yourself; a failed path cannot be retried; canonicalize paths; `set_class_loader` before loads; reject a second plugin.
- **Review of Tasks 3-4** (`fcbd609`, `70989eb`, `299dd7f`) was running.
  - If its findings are not recorded here, review again. Main open question: `getDeclaredMethods` resolves the types of every method, so one missing type fails a whole library load. Consider a fallback.
- **Direction changed (user decision):** 3D games and performance work are goals after 2D (see CLAUDE.md Non-goals). The user will provide the Portal (NVIDIA Shield) APK as a future 3D target; it may need Tegra-specific GLES extensions.
- **Remote:** `origin/main` = pushed `phase1-zbrun`. Push only with the user's agreement.

## NEXT (after Task 6, 2026-09-15)

- **Done:**
  - Task 5 (`f90638b`); the Java API for Task 6 is in `android/launcher/.../core/ZBridge.java` (`activatePlugin`, `onProxyLoaded`, `loadError`, `lastLoadError`, `fixGuestLibrary`);
  - Task 6 on local branch `codex/phase4d-launcher`: launcher arm32 import, runtime bundle,
    `PluginClassLoader`, activation before plugin code, proxy routing, and persistent diagnostics;
  - host tests 27/27 and guest tests 9/9.
- **Review of Tasks 3-4:** `docs/superpowers/reviews/2026-09-15-phase4d-tasks3-4-review.md` (`61c2361`). Both Important items are now fixed:
  1. `getDeclaredMethods` could fail a whole library -> `19bc37a` (long-form exports resolve only their own signature, short-form failures skip-and-log);
  2. executable host tests for `jni_env_backend.cpp` with a fake reflective `JNIEnv` -> `fc10efa`, expanded in `19bc37a`.
- **Task 5 review passed:**
  `docs/superpowers/reviews/2026-09-15-phase4d-task5-review.md`. No Critical or Important
  findings. Focused tests passed 3/3 and `proxy_runtime_test` passed 50/50 repeated runs.
- **Task 6 is complete.** Key details:
  - ABI priority is arm64-v8a, armeabi-v7a, then armeabi; old 32-bit imports require reimport;
  - staging import runs `fixGuestLibrary` before metadata/publication, removes stale native files,
    and preserves `plugins/<pkg>/data` on reimport;
  - `PluginClassLoader` delegates only `com.zettabridge.core.*`, returns real arm64 libraries or
    atomically-created `plugins/<pkg>/proxy/lib<name>.so` copies for arm32, and returns null for the
    normal system fallback when a library is absent;
  - `tools/make_launcher_bundle.sh` produces an ignored 6.9 MiB `build/launcher` tree from the exact
    10-file sysroot/runtime set. Gradle consumes only that generated assets/jniLibs tree;
  - runtime assets install atomically before plugin code and `ZBridge.activatePlugin` runs before
    providers or `Application`; a bridge load failure is shown through `Diagnostics` and the
    unusable `:guest` process exits after preserving the error.
- **Task 6 verification:** launcher contract test PASS; full Java compile against android-36 PASS;
  bundle filename/ELF/size validation PASS; Android `zbridge` and `zbproxy` link; host 27/27; guest
  9/9; `tools/gen_jni.py --check` PASS. No Gradle wrapper or system Gradle is available here, so an
  APK build was not run locally.
- **Any load failure needs a `:guest` process restart.**
- **Next:** Task 7, the real-ART T7 diagnostics app and OnePlus 13 device run. Then fix the two
  Tasks 3-4 review findings above before Task 8 (Orange Roulette smoke launch).

## Codex continuation (2026-09-15)

- Work continues on local branch `codex/phase4d-launcher`, based on `3085daa` from
  `phase1-zbrun`. Do not push without the user's agreement.
- Task 5 review and Task 6 are complete. Next is Phase 4d Task 7.
- Commit every completed task locally and update this file in the same task commit so a fresh
  Claude or Codex session can resume from the latest `NEXT` section.

## Phase 4d Task 7 done (2026-09-16)

- Task 7 is complete in this commit:
  - `guest/testlib/zbt7probe.c` and its `tools/build_guest.sh` target;
  - `android/t7/java/` real-ART model/runner and minimal plugin loader activity;
  - `android/t7/project/`, `tools/make_t7_bundle.sh`, and `docs/phase4-device-test.md`.
- `libzbt7probe.so` uses the safe portions of the existing JNI probe plus real-ART string, array,
  reference and direct-buffer checks. Its guest `JNI_OnLoad` performs `RegisterNatives`; Java then
  tests nested calls and two concurrent callers. Deliberate invalid-JNI cases remain host-only so
  CheckJNI cannot abort the diagnostics app.
- Fresh local verification with the Task 7 work:
  - `tools/make_t7_bundle.sh`: PASS, 7.0 MiB; Java compile and ELF/export checks pass;
  - Gradle 8.11.1 / AGP 8.7.3 `:app:assembleDebug`: PASS on the arm64 phone host;
    the ready APK is copied to
    `/sdcard/AndroidIDEProjects/ZettaBridge/ZBridgeT7-debug.apk` (6.0 MiB);
  - host suite 28/28; guest suite 9/9; JNI generator check PASS;
  - Android `zbridge`/`zbproxy` link and proxy structure check PASS.
- First device run reached the `strings` checkpoint and failed only at `zbt7probe.c:94`:
  current ART deliberately emits a supplementary code point as four-byte UTF-8 from
  `GetStringUTFRegion`, rather than the six-byte Modified UTF-8 form required by the JNI spec.
  The test expectation was corrected to ART behavior and the replacement APK (SHA-256
  `bc69597e7e35cf6ae56051b946c7af896a508a906cbe7196eb691fca0b592165`) was rebuilt; the next
  device run passed this checkpoint.
- Second device run passed strings and reached `RegisterNatives`, then `zb.Natives.add` was
  unresolved. Root cause: guest `FindClass("zb/Natives")` used ART's caller loader and registered
  the default app-loader copy, while T7 invoked the plugin-loader copy. `JniEnvBackend::find_class`
  now routes ordinary internal class names through the retained plugin loader. A new executable
  fake-JNI host test observed RED before the fix and PASS after it; it also made the Android-only
  backend compile and run on the host. Full verification is host 28/28, guest 9/9, generator PASS,
  Android link PASS and APK build PASS. Device rerun of APK SHA-256
  `de344ba365acf12eee9e740ef6fb1ce71de1bc5cc182c7093ea516af2677612d` passed T7 on the OnePlus 13.
- **Device acceptance:** `T7 PASS` on 2026-09-16. This covers the real ART backend, all JNI value
  types and call forms, refs, strings, arrays, direct buffers, exceptions, guest `JNI_OnLoad`,
  `RegisterNatives`, nested calls, JavaVM attach/detach and two concurrent Java callers.
- **NEXT:** the remaining Important Tasks 3-4 review item is now **done** (`19bc37a`; see "Phase 4d:
  the Tasks 3-4 review is closed" at the end of this file). Next is Task 8, the Orange Roulette
  Phase 4 smoke launch. Phase
  4 is complete when it reaches the first intentionally unimplemented GLES or `AAsset*` call with
  no JNI error; GLES passthrough and the first rendered frame are Phase 5.

## Phase 4d Task 8 launcher gate ready (2026-09-16)

- The first real AGP launcher build exposed `ClassLoader.getClassLoadingLock`, which exists in the
  desktop JDK used by the compile harness but not in Android's API. `PluginClassLoader` now
  synchronizes on itself, matching the T7 loader. `LauncherContractsTest` and the signed launcher
  APK build both pass.
- Ready device files:
  - launcher APK: `/sdcard/AndroidIDEProjects/ZettaBridge/ZettaBridge-launcher-debug.apk`, SHA-256
    `e6ec3a52a64dbfb556fcbf405c480c74a1b95b8fbe396914676f7fcb1c7977a3`;
  - untouched Orange Roulette APK: `/sdcard/AndroidIDEProjects/ZettaBridge/orange-roulette-1-0-0.apk`,
    SHA-256 `1fb252e27c06bc8f1a438a8bbed69f8feb75de4245a6105c04d4ca06982b3864`.
- **NEXT/device action:** install/update the launcher, import the Orange Roulette APK, launch it,
  and preserve the first on-screen/clipboard/file failure. A failed guest load requires force-stop
  of the launcher before retrying. (The `getDeclaredMethods` hardening this paragraph left open
  landed later as `19bc37a`.)

## Phase 4d Task 8 first device result (2026-09-16, not complete)

- The user installed `fc10efa`'s launcher APK, imported the untouched Orange Roulette APK, and
  launched it. There was no Java/JNI error or launcher diagnostic. The plugin showed a black
  screen, briefly changed half the screen to white while switching to landscape/render setup, then
  silently returned to the launcher.
- This is the first observed end-to-end arm32 launch through the production launcher and is
  consistent with reaching the generated GLES/asset stubs. It is not yet enough to check Task 8:
  OxygenOS hides third-party logcat output, so the run did not preserve the first host-call name or
  prove the exact six proxy loads, two guest `JNI_OnLoad`s and 20 registrations.
- `Process::dispatch_stop` currently logs the first unimplemented host call, writes `r0 = 0`, and
  continues. Therefore the silent exit can happen after several zero-returning GLES calls; it is
  not necessarily the first trap itself.
- **NEXT:** expose the first non-JNI generated host call through a process-safe persistent/on-screen
  diagnostic (or begin the Phase 5 host dispatcher with equivalent tracing), rerun Orange Roulette,
  and record the exact call plus load/registration counts. Then perform the final Phase 4 regression
  and documentation commit. Do not claim Phase 4 complete from the visual symptom alone.
  The diagnostic half of this is done: see "Phase 4d Task 8 runtime report" at the end of this file
  for the report, the device file and the exact phone steps. The device rerun is still open.
- The Tasks 3-4 review hardening is **done** (`19bc37a`), implemented exactly this way: JNI long
  names encode parameter types but not the return type, while `GetMethodID` needs the complete
  descriptor, so the backend uses
  `MethodType.fromMethodDescriptorString(arguments + "V", pluginLoader).parameterArray()` followed
  by `Class.getDeclaredMethod` and the found method's real return type; short-form reflection
  failures skip-and-log. No return type is ever guessed.

## Phase 4d Task 8 runtime report (2026-09-16)

The Task 8 device run now records what it did. OxygenOS drops third-party logcat output, so the
report is written to a file and read back from the launcher UI; nothing needs a shell or adb.

**What it records** (`core/include/zb/runtime_report.h`, `core/src/runtime_report.cpp`). One
process-wide `zb::RuntimeReport`, `zb::runtime_report()`, fed from the places that already see the
events and bounded so a hot guest loop cannot grow it:

| Fact | Recorded in |
|---|---|
| unimplemented (non-JNI) host calls: first one, total, first 16 distinct with counts | `Process::dispatch_stop` (`core/src/process.cpp`) |
| active plugin, proxy loads and failures | `ProxyRuntime` (`core/src/jni/proxy_runtime.cpp`) |
| guest `JNI_OnLoad` calls and their results | `JniLoader::load` (`core/src/jni/loader.cpp`) |
| registered natives (`Java_*` binding and guest `RegisterNatives`) | `HostJni::register_native` (`core/src/jni/host_jni_natives.cpp`) |
| how the guest ended | `Process::crash_report`, `Process::request_exit`, the `LibraryRuntime` runner |

Guest semantics are unchanged: an unimplemented host call is still logged once, still returns
`r0 = 0`, and the guest still continues.

**Report format.** One `key: value` line per fact, fixed order, ASCII, diff-friendly. A filled
example:

```text
zettabridge-runtime-report 1
plugin: /data/user/0/com.zettabridge.launcher/files/plugins/com.heyhouser.OrangeRoulette targetSdk 16
proxy-loads: 6
proxy-failures: 0
proxy-loaded: libstd.so jni=0x00010006
proxy-loaded: libregexp.so jni=0x00010006
proxy-loaded: libzlib.so jni=0x00010006
proxy-loaded: libopenal.so jni=0x00010006
proxy-loaded: liblime.so jni=0x00010006
proxy-loaded: libApplicationMain.so jni=0x00010006
jni-onload-calls: 2
jni-onload: liblime.so ok jni=0x00010006
jni-onload: libopenal.so ok jni=0x00010006
registered-natives: 21
unimplemented-host-calls: 42
unimplemented-distinct: 3
first-unimplemented: libGLESv2.so glCreateProgram
unimplemented: libGLESv2.so glCreateProgram x37
unimplemented: libGLESv2.so glCreateShader x4
unimplemented: libandroid.so AAssetManager_fromJava x1
guest-exit: guest SIGSEGV: read of 0x00000000, pc 0xf3a12345 in libApplicationMain.so offset 0x2345
```

`proxy-failed:`, `proxy-more:`, `jni-onload-more:` and `unimplemented-more:` lines appear only when
there is something to report. `(none)` marks a fact nothing was recorded for.

**Where it lands on the device.**
`/sdcard/Android/data/com.zettabridge.launcher/files/zb-runtime-report.txt`, next to the existing
`zb-errors.txt`. The `:guest` process starts persisting it in `ZbApplication.onCreate` before any
plugin code runs (`Diagnostics.startRuntimeReport` -> `ZBridge.setReportFile`). The file is
rewritten atomically (temp file, `fsync`, `rename`) on every structural change - a new distinct
host call, a load, a `JNI_OnLoad`, the exit reason - and at most once a second for counter-only
changes, so a `:guest` process that dies silently still leaves its last state on disk. The same
text is available in-process as `ZBridge.runtimeReport()`.

**How the user opens it.** Library screen -> long-press the app -> **Last run report**. The dialog
shows the text and copies it, plus the file path, to the clipboard.

### Exact phone steps for the next Task 8 run

1. Rebuild the launcher project (`android/launcher/`, AndroidIDE, NDK r29) so it picks up the new
   `libzbridge.so` and launcher Java, and install the resulting APK over the old one.
2. Open ZettaBridge, long-press Orange Roulette, and choose **Delete**, then import
   `orange-roulette-1-0-0.apk` again (a fresh import also force-stops the `:guest` process). If the
   app is already imported and was never launched since the update, importing again is enough.
3. Tap Orange Roulette and let it run until it returns to the launcher by itself, or wait about ten
   seconds if it stays on a black screen.
4. Back in the library screen, long-press Orange Roulette and choose **Last run report**. The report
   is now on the clipboard.
5. Paste that text back into the chat. If the entry says there is no report yet, send
   `/sdcard/Android/data/com.zettabridge.launcher/files/zb-errors.txt` instead.

The run answers Task 8 when the report shows six `proxy-loaded:` lines, the guest `JNI_OnLoad`
results, a non-zero `registered-natives:`, and a `first-unimplemented:` naming a generated GLES or
`AAsset*` function with no JNI failure before it.

## Phase 4d: the Tasks 3-4 review is closed (2026-09-16)

Both Important findings of `docs/superpowers/reviews/2026-09-15-phase4d-tasks3-4-review.md` are
fixed. Finding 2 (executable tests for the real ART backend) closed with `fc10efa`; finding 1
(`getDeclaredMethods` could fail a whole library) closed with `19bc37a`.

**Why it mattered.** `Class.getDeclaredMethods()` eagerly resolves the parameter and return types of
every declared method, so one unresolvable type anywhere in the class threw `NoClassDefFoundError`,
and `JniLoader::load` turned any `Error` into a fatal failure for the whole `.so`. Orange Roulette
bundles AdMob classes next to lime in one dex, which is exactly that shape.

**Two discovery paths** in `JniEnvBackend::find_declared_natives`, picked by the new `arguments`
parameter of the `JniBackend` seam (the argument part of the descriptor for a long-form export,
`nullptr` for a short-form one; `core/src/jni/loader.cpp` passes `decoded->arguments`):

- **Long form** (`Java_pkg_Class_method__<mangled argument types>`) never enumerates.
  `MethodType.fromMethodDescriptorString(arguments + "V", pluginLoader).parameterArray()` resolves
  only the classes that one signature names, `Class.getDeclaredMethod(name, parameters)` picks the
  method, and the descriptor is completed from that `Method`'s real return type with
  `zb/jni_descriptor.h`. A JNI long name encodes parameter types and **never** a return type, so
  `GetMethodID` alone cannot be used and no return type may be guessed.
- **Short form** still enumerates, because every overload of the name must bind.

**New `NativeLookupStatus::Unresolvable`** means "the class loaded, but the types this export needs
did not". The loader skips that one export, logs it once
(`JniLoader::log_unresolvable_once`) and counts it in the new `JniLoadReport::skipped_exports`.
`MissingClass` (cleared and logged once) and real errors are unchanged.

| Export form | Failure | Outcome |
|---|---|---|
| long | `fromMethodDescriptorString` throws (type not present) | `Unresolvable`: skip the export, library loads |
| long | `getDeclaredMethod` throws `NoSuchMethodException` | `Found` with no methods -> loader error "no declared native matches export" |
| long | `getDeclaredMethod` throws anything else (sibling overload's types) | `Unresolvable`: skip |
| long | method is declared but not `native` | `Found` with no methods -> loader error |
| long | `getReturnType()` throws | `Unresolvable`: skip |
| short | `getDeclaredMethods()` throws | `Unresolvable`: skip |
| short | one matching method's types do not resolve | `Unresolvable`: skip |
| both | `loadClass` throws `ClassNotFoundException` / `NoClassDefFoundError` | `MissingClass`: cleared, logged once, `skipped_classes` |
| both | `loadClass` throws anything else | `Error`: rethrown, the library fails |
| both | `PushLocalFrame` fails, or a JNI call returns null with no exception | `Error`: the library fails |

**`MethodType` is API 26** (the launcher's `minSdk`). Its reflection ids are an *optional* group:
if any of them is missing, `Reflection::long_form_ok` stays false and long-form exports fall back to
enumeration, which now skips instead of failing. A failure there never disables reflection for the
process.

**Tests.** `tests/host/jni_env_backend_test.cpp` drives the real ART backend against a fake
reflective `JNIEnv` (a toy Java world with declared methods, `Class` type objects, `MethodType`, and
a type name that cannot be resolved) over the whole matrix above, then repeats it with `MethodType`
unavailable. `jni_loader_test` covers the end-to-end half with the new guest library
`libzbloadskip.so` (`guest/testlib/zbloadskip.c`) and two `MockJvm` knobs,
`fail_declared_enumeration` and `fail_type_resolution`. Host 30/30, guest 9/9,
`LauncherContractsTest` PASS, Android `zbridge` / `zbrun` / `zbproxy` /
`zbjni_reflection_compile_test` link.

**Not verifiable without a device (watch in T7/T8):** which throwable ART's
`MethodType.fromMethodDescriptorString` actually raises for an absent type
(`TypeNotPresentException`, `NoClassDefFoundError` or `IllegalArgumentException` - all three are
treated as a skip), and whether libcore's `Class.getDeclaredMethod` really resolves sibling
overloads' parameter types. The pessimistic case is a skipped export, visible as a Java
`UnsatisfiedLinkError` when the guest calls it, plus the once-per-export `[zb] JNI loader: cannot
resolve the declared natives of ...` line.

## HANDOFF 2026-09-16: Phase 4 complete, Part 4 design approved, Phase 5 next

Claude is out of weekly budget for about a day. Codex continues alone. Everything below is
decided; implement it, do not redesign.

### Where things stand

- **Phase 4 is complete and accepted on the device.** Evidence: `docs/phase4-acceptance.md`.
  All six Orange Roulette arm32 libraries load, both guest `JNI_OnLoad` run, 20 natives
  register, the game then makes 45 GLES calls (18 distinct) into stubs that return 0 and
  crashes on a null GL object. No JNI error anywhere.
- **The last review finding is fixed** (`19bc37a`, `7aff563`): one unresolvable Java type no
  longer fails a whole library.
- Host tests 30/30, guest 9/9, Android links, launcher tests pass.
- Branch `codex/phase4d-launcher`. `origin/main` still points at the older `phase1-zbrun`;
  merge and push only with the user's agreement.

### Building the launcher APK on this machine (new, use it)

No more copying projects to the phone:

```
ninja -C build/android-arm64 zbridge zbproxy; tools/make_launcher_bundle.sh; cd android/launcher; ANDROID_HOME=$HOME/android-sdk ANDROID_SDK_ROOT=$HOME/android-sdk ./gradlew --no-daemon assembleDebug; cd -; cp android/launcher/app/build/outputs/apk/debug/app-debug.apk /sdcard/ZettaBridge-debug.apk
```

The user installs `/sdcard/ZettaBridge-debug.apk`. The APK carries the runtime bundle
(`assets/zb`, 17 files) and `libzbridge.so`.

### Next: write the Part 4 spec and the Phase 5 plan, then implement

The user approved this design. Write
`docs/superpowers/specs/2026-09-16-gles-assets-design.md` (style of the JNI bridge spec) and
`docs/superpowers/plans/2026-09-16-phase5-gles.md` (lean style of the 4c plan: tasks, tests,
decisions, acceptance, no full code), then execute task by task.

**User decisions:**
1. Implement all of GLES 2.0, not only what Orange Roulette needs.
2. `AAsset*` (6 functions) ships in the same phase; without it the game shows nothing.
3. Correctness first, performance later; keep the design friendly to batching, because 3D
   games are a goal.

**Verified numbers (measured here, reuse them):**
- `https://raw.githubusercontent.com/KhronosGroup/OpenGL-Registry/main/xml/gl.xml` downloads
  (2.8 MB) and parses with `xml.etree`.
- `GLES2/gl2.h` declares 142 core functions; all 142 appear in `gl.xml`.
- 81 take no pointers: fully mechanical.
- 57 take pointers whose lengths the registry gives through `len=`: mechanical with rules.
- 4 need hand-written rules: `glBindAttribLocation`, `glGetAttribLocation`,
  `glGetUniformLocation` (NUL-terminated strings) and `glVertexAttribPointer`.

**Design:**
- `tools/gen_gles.py` reads `gl.xml` plus the NDK headers and emits the host-call list and the
  host dispatch handlers with marshaling. Generated files are committed and checked by a ctest,
  like `gen_jni.py`.
- Pointer rules from the registry: guest-to-host copies for inputs (`glShaderSource`,
  `glTexImage2D`, `glBufferData`), host-to-guest for outputs (`glGen*`, `glGet*iv`), and
  NUL-terminated strings for the three name lookups. Bounds-check every guest pointer through
  `GuestMemory`, as the JNI bridge does.
- `glVertexAttribPointer` is the one hand-written case: GLES 2.0 allows client-side vertex
  arrays, so record enabled attributes and copy the referenced guest memory at draw time
  (`glDrawArrays`, `glDrawElements`), deriving the byte range from first/count/stride/type or
  from the index buffer.
- Threading: handlers call the real `gl*` inline on the calling host thread, which borrowed the
  carrier of the Java `GLThread` where the EGL context is current. No cross-thread dispatch.
- Assets: `AAssetManager_fromJava` takes a `jobject` through the JNI bridge; `AAsset*` and
  `AAssetManager*` become 32-bit handles for the guest, like JNI references.
- Testing without a phone: a mock GLES that records calls and returns predictable ids, driven by
  an arm32 guest probe (`guest/testlib/zbglprobe.c`). Separate tests for out-of-bounds and null
  pointers, absurd sizes, and client-side vertex arrays with stride, offset and indices. Only the
  real EGL context, `GLThread` and the first frame need the device.
- Order: generator plus pointerless functions; pointer functions; `glVertexAttribPointer` plus
  draws; `AAsset*`; then the Orange Roulette run.
- Acceptance: Orange Roulette draws its intro screen on the device.

### Cheap diagnostic worth adding early

`zb::RuntimeReport` does not record exports skipped by the JNI loader
(`JniLoadReport::skipped_exports`). If the device run shows an unexpected
`UnsatisfiedLinkError`, add them to the report first.

### Device-test etiquette that works

Put the phone steps in a short numbered block at the top of the message. OxygenOS hides
third-party logcat, so rely on the runtime report, `Diagnostics` and the clipboard.

### Update: the spec and plan are written (`956cd03`)

- `docs/superpowers/specs/2026-09-16-gles-assets-design.md`
- `docs/superpowers/plans/2026-09-16-phase5-gles.md` (9 tasks)

A real generator prototype over `gl.xml` emitted 131 of 142 handlers mechanically and they
compiled clean against the NDK headers. It corrected three things in the numbers above:

1. **No copying for arrays.** Guest memory is host memory at `base() + addr`, so the
   len-carrying functions need a bounds check and a pointer add, not a copy. Real copies remain
   only in `glShaderSource` (array of 32-bit pointers) and `glGetString` (host pointer the guest
   cannot hold).
2. **Hand-written cases are 11, not 4:** `glVertexAttribPointer`, `glDrawArrays`,
   `glDrawElements`, `glShaderSource`, `glGetVertexAttribPointerv`, `glGetString`,
   `glGetUniform{f,i}v`, `glTexImage2D`, `glTexSubImage2D`, `glReadPixels`. The registry cannot
   express `COMPSIZE` lengths (18 params), and pixel sizes also depend on `GL_*_ALIGNMENT`,
   which the registry never mentions.
3. **No GLES 2.0 parameter is 64 bits**, so argument index equals guest word index; none of the
   JNI bridge's 64-bit pair handling applies. `GLintptr`/`GLsizeiptr` widen in `glBufferData`
   and `glBufferSubData` only.

Traps the prototype found, already written into the plan:
- `glGetVertexAttribPointerv` must return the **guest** pointer; the registry marks it a plain
  `len=1` output, and a naive generator would hand the guest a host address.
- A `len=` can name a parameter declared after the pointer (`glShaderBinary`), so a single-pass
  generator fails to compile.
- Never add the NDK sysroot to a host target's include path; it breaks host glibc C++ headers.
  Copy or isolate `GLES2/` and `KHR/` instead.

**Biggest device risk:** the design assumes guest GL calls arrive on the host thread that
borrowed `GLThread`'s carrier, where the EGL context is current. Only JNI has exercised that
path. Plan Task 9 checks it first; if it is wrong, everything fails on device and nothing fails
on this machine.

## Phase 5 Task 1 done (2026-09-16)

- Implemented in this commit on `codex/phase4d-launcher`: pinned Khronos `gl.xml` (SHA-256
  `b9ca2cfa5c676e901c20d34af3407f1687cde0f1336a5ff7a8974d04c7494ad3`),
  `tools/gen_gles.py`, generated host-call constants/dispatch/manual list/backend seam,
  `HostGl`, `MockGles`, `gles_marshal_test` and `gen_gles_check`.
- The generator verifies exactly 142 GLES 2.0 registry commands, exactly 142 NDK declarations,
  the empty symmetric difference, 81 pointerless declarations, and exact agreement with the
  committed guest-stub indices. Builds never use the network.
- `HostGl` owns only indices 0-141 and leaves 142-160 to Task 7's `HostAssets`. It captures
  `r0-r3` before clearing the result, reads later words from guest `sp`, bit-preserves floats,
  and sign-extends the one-word guest `GLintptr` / `GLsizeiptr` values.
- Plan corrections recorded in the spec and plan: `HostGl` must not swallow the asset range;
  `glDrawArrays` and `glGetString` are pointerless by parameter shape but are two of the 11
  semantic handlers. Task 1 therefore has 79 direct backend calls and two safe manual stubs,
  not 81 unsafe direct calls.
- TDD evidence: `gles_marshal_test` first failed because `zb/gl_hostcalls.h` did not exist, then
  passed with coverage of all 81 pointerless dispatches, stack arguments, float bits, widening,
  returns, safe stubs and the asset boundary.
- Fresh verification: `tools/gen_gles.py --check` and `tools/gen_jni.py --check` pass; host
  32/32; guest 9/9; Android arm64 `zbridge` links.
- **NEXT:** Phase 5 Task 2 in `docs/superpowers/plans/2026-09-16-phase5-gles.md`: generate
  bounds-checked marshaling for registry `len=` pointer parameters and add `gles_pointer_test`.

## Phase 5 Task 2 done (2026-09-16)

- Implemented in this commit: the generator now emits 37 additional pointer-taking handlers,
  bringing the direct typed backend surface to 116/142 GLES calls. It accepts literal, named
  parameter and product `len=` forms and intentionally leaves `COMPSIZE`, NUL strings and the
  semantic handlers for Tasks 3-5.
- All scalar arguments are captured before any pointer is translated, so
  `glShaderBinary(binary, length)` correctly uses its later `length` parameter. Length products
  and byte sizes are computed in 64 bits; negative lengths, ranges beyond the 4 GiB guest space,
  unmapped pages and insufficient permissions set `GL_INVALID_VALUE` and skip the driver.
- Input and output arrays are zero-copy aliases of `GuestMemory`: non-null addresses become
  `base() + address` after `kPageRead` or `kPageRead | kPageWrite` validation. A null guest
  pointer stays null, while an impossible length is still rejected before the null shortcut.
- TDD evidence: the new marshal cases and `gles_pointer_test` first failed with
  `GL_INVALID_OPERATION` from the Task 1 stubs. They now cover literal/parameter/product lengths,
  the later-parameter trap, past-end and unmapped addresses, read-only output, guest-space size
  overflow, absurd `glGenTextures`, oversized `glBufferData`, and legal null data.
- Fresh verification: GLES/JNI generator checks pass; host 33/33; guest 9/9; Android arm64
  `zbridge` links.
- **NEXT:** Phase 5 Task 3: implement `COMPSIZE(pname)`, pixel byte sizing/alignment and the
  lazy uniform-location size map.

## Phase 5 Task 3 done (2026-09-16)

- Implemented in this commit: nine one-element `COMPSIZE(pname)` handlers are now generated;
  `glGetBooleanv` / `glGetFloatv` / `glGetIntegerv` use the GLES 2.0 1/2/4/variable count rules;
  and `glTexImage2D`, `glTexSubImage2D`, `glReadPixels`, `glGetUniformfv` and
  `glGetUniformiv` have semantic handlers. The functional surface is 133/142 GLES calls.
- Pixel sizing validates all GLES 2.0 format/type pairs, dimensions and alignments, includes row
  padding except after the final row, and tracks `GL_PACK_ALIGNMENT` / `GL_UNPACK_ALIGNMENT`
  from generated `glPixelStorei` dispatch. Null `glTexImage2D` data remains legal.
- Uniform result sizes are cached per thread/program after `GL_ACTIVE_UNIFORMS` enumeration.
  Array `[0]` suffixes are stripped before `glGetUniformLocation`; scalar/vector/matrix types map
  to 1/2/3/4/9/16 elements. `glLinkProgram` and `glDeleteProgram` invalidate the program cache;
  an unknown location is rejected with `GL_INVALID_OPERATION` before touching guest memory.
- The generated manual list is now real per-function declarations, so a missing semantic
  definition is a link failure. Nine deliberate stubs remain for Tasks 4-5: the three name
  lookups, `glGetString`, `glShaderSource`, `glVertexAttribPointer`, both draws and
  `glGetVertexAttribPointerv`.
- TDD evidence: the test first failed to compile on missing `gl_pname_count` and
  `gl_pixel_bytes`; the first implementation run then correctly exposed a missing
  `GL_ACTIVE_UNIFORM_MAX_LENGTH` behavior in `MockGles`. Final tests cover all eight valid pixel
  format/type pairs at alignments 1/2/4/8, the one-pixel RGB padding edge, variable pnames,
  one-element COMPSIZE, PixelStore tracking, lazy uniform lookup and link invalidation.
- Fresh verification: GLES/JNI generator checks pass; host 33/33; guest 9/9; Android arm64
  `zbridge` links.
- **NEXT:** Phase 5 Task 4: bounded guest strings, process-lifetime `glGetString` copies and
  32-bit-to-64-bit `glShaderSource` pointer-array translation.

## Phase 5 Task 4 done (2026-09-16)

- Implemented in this commit: bounded guest-string marshaling for `glBindAttribLocation`,
  `glGetAttribLocation` and `glGetUniformLocation`; process-lifetime guest copies for
  `glGetString`; and host-width pointer-array construction for `glShaderSource`. The functional
  surface is 138/142 GLES calls.
- Guest strings are scanned page by page, never across an unmapped page and never beyond the
  JNI bridge's 64 MiB cap. Driver strings are copied through the runtime service `malloc` and
  cached by enum, so repeated `glGetString` returns the same 32-bit guest address. Host tests use
  the constructor's allocator seam; production defaults to `LibraryRuntime::call_on_current`.
- `glShaderSource` reads all scalars first, validates the guest `uint32_t` pointer array and
  optional `GLint` length array, translates each source, and keeps embedded NUL bytes when an
  explicit non-negative length is present. Negative lengths use bounded NUL scanning. The
  temporary host pointer array exists only for the driver call.
- TDD evidence: the new test first failed on the missing allocator-aware `HostGl` constructor.
  Final cases cover a normal name, an unterminated name at an unmapped page boundary, two shader
  strings with negative/explicit lengths and an embedded NUL, `count == 0`, a partly unreadable
  pointer array, and stable/readable `glGetString` results across two calls.
- Fresh verification: GLES/JNI generator checks pass; host 33/33; guest 9/9; Android arm64
  `zbridge` links.
- **NEXT:** Phase 5 Task 5: implement client-array state, draw-time pointer materialization,
  `glDrawArrays`, `glDrawElements` and guest-pointer-preserving
  `glGetVertexAttribPointerv`. These are the final four GLES functions.

## Phase 5 Task 5 done (2026-09-16)

- Implemented in this commit: thread-local `GL_ARRAY_BUFFER` / `GL_ELEMENT_ARRAY_BUFFER`, enabled
  attribute and pointer-definition state; deferred guest client pointers; draw-time validation and
  materialization; client and buffer-backed `glDrawElements`; and guest-address-preserving
  `glGetVertexAttribPointerv`. All 142/142 GLES 2.0 calls now have functional handlers.
- Buffer-backed vertex pointers are forwarded as offsets immediately. Client pointers are retained
  as 32-bit guest addresses and re-issued only after the complete draw range is readable. Client
  U8/U16 indices are scanned for their maximum; an element-buffer value remains an opaque offset.
  Invalid layouts, ranges and output pointers set a GL error and never reach the draw call.
- TDD evidence: `gles_client_arrays_test` first failed against the four Task 4 stubs. It now covers
  zero and explicit strides, non-zero `first`, client and buffer-backed attributes together, U8 and
  U16 client indices, an element-buffer offset, an out-of-range selected vertex, draw rejection,
  and `glGetVertexAttribPointerv` returning the original guest address. The full suite also exposed
  and corrected Task 1's obsolete expectation that `glDrawArrays` remained a stub.
- Fresh verification: GLES/JNI generator checks pass; host 34/34; guest 9/9; Android arm64
  `zbridge` and `zbproxy` link.
- **NEXT:** Phase 5 Task 6: build the arm32 `zbglprobe` and exercise the complete guest-stub ->
  `svc` -> `HostGl` -> mock path, including all 142 calls and a 20-run repeat loop.

## Phase 5 Task 8 done (2026-09-16): Android GLES backend, chaining and report

- Tasks 6-7 (the arm32 `zbglprobe` guest probe and the 19-function `AAsset*` bridge) are
  deliberately postponed past Task 8. Reason: the biggest open risk for Orange Roulette on the
  OnePlus 13 is whether guest GL calls really arrive on the host thread whose EGL context is
  current (the spec's threading assumption, `docs/superpowers/specs/2026-09-16-gles-assets-design.md`
  lines ~235-248: "there is no cross-thread dispatch and no GL command queue"). Wiring the real
  driver first and reading the new report fields on a device run tests that assumption directly,
  before spending a task on assets that only matter if rendering already works. Tasks 6-7 are
  still open and come after Task 8's on-device read.
- **New:** `core/android/gl_driver_backend.h/.cpp` (+ generated `gl_driver_backend_overrides.inc`):
  `GlDriverBackend`, a `GlBackend` override that forwards all 141 non-`glGetError` typed calls
  straight to `libGLESv2.so` (`::glFoo(...)`), Android-build-only. `glGetError` and `set_error`
  are hand-written: there is no real API to inject an error into the driver's own queue, so a
  `HostGl` rejection (a bad guest pointer, never reaching the driver) is queued in
  `pending_error_` and returned by the *next* `glGetError()` call ahead of whatever the driver
  itself queued; GL error state is sticky until read, so only the first rejection between two
  `glGetError()` calls survives. `gl_egl_context_current()` links `EGL` only for
  `eglGetCurrentContext() != EGL_NO_CONTEXT`.
- **Chained:** `HostGl::EglContextProbe` (a `std::function<bool()>`, default empty on the host)
  is a new optional 4th constructor argument. `GuestJniEngine` (core/include/zb/proxy_runtime.h,
  core/src/jni/proxy_runtime.cpp) takes an optional `GlBackend*` and the probe; when given, it
  builds a `HostGl` and installs a combined `LibraryRuntime::set_host_call_handler` lambda that
  tries `HostGl::handle_host_call` (indices 0-141) before `HostJni::handle_host_call`
  (0xFB00+) — the ranges never overlap so the order is free. `core/android/guest_jni_runtime.h/.cpp`
  now owns a `GlDriverBackend gl_backend_` member and passes `&gl_backend_` plus
  `gl_egl_context_current` into the `Engine`/`GuestJniEngine` constructor. The host build path
  (`GuestJniEngine(backend)` with no GL args) is unchanged and still chains only `HostJni`.
- **`RuntimeReport` GL section** (core/include/zb/runtime_report.h, core/src/runtime_report.cpp):
  `note_gl_call(function, host_tid)` (every call; only the first is kept), `note_gl_egl_context(bool)`
  (once), `note_gl_error(function, error)` (once, first non-`GL_NO_ERROR` `glGetError()` result).
  All three are hooked generically in `HostGl::handle_host_call` (core/src/gl/host_gl.cpp), which
  is portable and knows the index -> function name table already — no Android dependency needed
  there. Four new `text()` lines: `gl-calls`, `gl-first-call`, `gl-egl-context-current`,
  `gl-first-error`. Example (from `gl_chain_test`):
  ```
  gl-calls: 1
  gl-first-call: glClear tid=4242
  gl-egl-context-current: yes
  gl-first-error: (none)
  ```
  `gl-first-error` looks like `glGetError 0x0502` when the first non-zero result is seen.
- **`ZB_GL_TRACE=1`** (any non-empty, non-`"0"` value) logs every GLES host call and its raw
  r0-r3 words once per call, added in `HostGl::handle_host_call` next to the report hooks.
- Not done in this task, still open: `AAsset*` (`HostAssets`, Task 7) and the guest probe
  (Task 6). Nothing chains `HostAssets` yet, so asset host-call indices still report
  unimplemented — expected until Task 7.
- TDD evidence: `tests/host/gl_chain_test.cpp` (two processes, `no-backend` / `with-backend`,
  since both `HostJni` and the `LibraryRuntime` it lives in are one-per-process by design) proves
  `GuestJniEngine::host_gl()` is null without a `GlBackend`, and with one, is wired to the exact
  object passed in, the EGL probe fires exactly once on the first call, `glClear` reaches the
  mock backend, `RuntimeReport::gl_calls()` counts it, and a JNI-range index still falls through
  to `HostJni`. `runtime_report_test.cpp` gained `check_gl_section()` (sticky-first semantics for
  the EGL and error fields, `clear()` resets them, empty-report defaults).
- Fresh verification: GLES/JNI generator checks pass; host 36/36 (was 34 at `51aa159`; the two
  new cases are `gl_chain_test_no-backend` and `gl_chain_test_with-backend`); guest 9/9; Android
  arm64 `zbridge` and `zbproxy` link (`zbridge` now links `GLESv2`/`EGL`). Launcher APK rebuilt
  via `tools/make_launcher_bundle.sh` + `./gradlew assembleDebug` and copied to
  `/sdcard/ZettaBridge-debug.apk` (~9.1 MB).
- **NEXT:** Phase 5 Task 9 (device run) can now read the GL report fields to confirm or refute
  the threading assumption before Tasks 6-7 are built. If it confirms guest GL calls land on a
  thread with a current EGL context, Tasks 6-7 (guest probe, `AAsset*`) are the remaining work
  before the Orange Roulette intro-screen acceptance test.

## HANDOFF 2026-09-16 after Phase 5 Task 5

- Continue on local branch `codex/phase4d-launcher` at `51aa159`. Do not push without the user's
  agreement. The only expected dirty path is the pre-existing required Dynarmic submodule patch;
  do not stage, reset or change it.
- Phase 5 Tasks 1-5 are committed as `79a1e41`, `dae9309`, `043c927`, `125367b`, `51aa159`.
  The complete GLES 2.0 host surface is implemented: 142/142 functions. The next unstarted work is
  Task 6 in `docs/superpowers/plans/2026-09-16-phase5-gles.md`.
- Last fresh verification at `51aa159`: GLES and JNI generator checks PASS; ctest 34/34 PASS;
  guest build and all 9 guest cases PASS; Android arm64 `zbridge` and `zbproxy` targets link.
- Task 6 must follow TDD and end in its own local commit with this file updated. It adds
  `guest/testlib/zbglprobe.c` and `gles_bridge_test`, drives the real arm32 stub/SVC/HostGl path,
  covers all 142 functions, and repeats the bridge test 20 times.
- Then execute Tasks 7-9 in order: Android asset calls (19 functions), production EGL/GLES/assets
  backend wiring and launcher bundle, then the OnePlus 13 Orange Roulette rerun and first-frame
  acceptance. The runtime report should show zero unimplemented GLES/asset calls after Task 8;
  Task 9 is the first device/render proof.

## Device result after Phase 5 Task 8 (2026-09-16) - threading risk DISPROVEN

The user ran Orange Roulette with the real GLES backend (`8c05689`):
- **`gl-egl-context-current: yes`.** Guest GL calls arrive on the thread with the current EGL
  context, so the no-cross-thread design holds.
- **`gl-calls: 64`, `gl-first-error: (none)`.** All real driver calls succeed.
- **The only unimplemented call left is `libandroid.so AAssetManager_fromJava` x1.** It returns 0,
  and the game dereferences the null manager: `SIGSEGV read of 0x00000004` in
  `libApplicationMain.so+0x26abe4`.

**NEXT: Phase 5 Task 7 (`AAsset*` over 32-bit handles).** It is the only thing between the game and
its first frame. Then Task 6 (guest probe) and Task 9 (intro screen on the device). Rebuild the APK
after Task 7 with the command in the HANDOFF section.

## Phase 5 Task 7 done: `AAsset*` over 32-bit handles (2026-09-16)

- New `HostAssets` (`core/include/zb/host_assets.h`, `core/src/assets/host_assets.cpp`) serves the
  six `AAsset*` functions liblime.so imports (indices 145/146/148/150/155/157 in
  `core/src/gen/hostcalls.inc`, named in `core/include/zb/asset_hostcalls.h`):
  `AAssetManager_fromJava`, `AAssetManager_open`, `AAsset_getLength`, `AAsset_read`, `AAsset_close`,
  `AAsset_openFileDescriptor`. `AAssetManager*`/`AAsset*` never cross into the guest; they are
  32-bit handles into `HostAssets`'s own tables (reusing `GlobalHandles`). The other ~12 AAsset*
  stubs in the reserved 142-160 range stay unimplemented. `HostJni` grew two small public methods,
  `current_env()`/`resolve_ref()`, so `HostAssets` can turn the guest jobject handle into the real
  host `jobject` and use the calling thread's host `JNIEnv` for `AAssetManager_fromJava`, per the
  design. `GuestJniEngine` now takes an optional `AssetBackend*` and chains `HostAssets` into the
  host-call handler alongside `HostGl`/`HostJni` (`core/src/jni/proxy_runtime.cpp`).
- New `AssetBackend` interface (`core/include/zb/asset_backend.h`): `AndroidAssetBackend`
  (`core/android/asset_driver_backend.*`) wraps the real NDK `<android/asset_manager_jni.h>`;
  `MockAssetBackend` (`tests/host/mock_assets.h`) is an in-memory map for host tests. `zbridge` now
  also links `android`.
- New test `asset_chain_test` covers the open/getLength/read/close round trip, a missing file, an
  out-of-bounds read buffer, operations on a closed handle, `AAsset_openFileDescriptor`'s 32-bit
  outputs, and that `GuestJniEngine` chains `HostAssets` the way it chains `HostGl`. Full suite:
  37/37 ctest, 9/9 guest tests, `zbridge`/`zbproxy` link for Android arm64.
- Still unimplemented: `AAssetDir_*` (3), `AAssetManager_openDir`, `AAsset_getBuffer`,
  `AAsset_getLength64`, `AAsset_getRemainingLength(64)`, `AAsset_isAllocated`,
  `AAsset_openFileDescriptor64`, `AAsset_seek(64)`. None of these are imported by liblime.so.
- **NEXT:** rebuild and push the launcher APK, then the OnePlus 13 rerun of Orange Roulette,
  expecting the intro screen (Task 9). Task 6 (guest GL probe) is still open separately.

## Device result after AAsset (`a5fd0ea`, 2026-09-16): game RUNS, screen black

The report shows `guest-exit: (none)`, `unimplemented-host-calls: 0`, `gl-calls: 141589`,
`gl-first-error: (none)` and `gl-egl-context-current: yes`. The render loop runs with no crash,
no missing bridge and no GL error, but the screen stays black.

`glGetError` does not catch the likely causes. **NEXT: a GL visibility diagnostic in
`RuntimeReport` before changing marshaling code.**
- **Shader compile and program link status.** For every `glCompileShader` / `glLinkProgram`, check
  `GL_COMPILE_STATUS` / `GL_LINK_STATUS` on the host side and record the first failure with its
  info log (truncated). Top suspect: `glShaderSource` string assembly.
- **Draw calls.** Count `glDrawArrays` / `glDrawElements`, and record the framebuffer bound at the
  first draw.
- **Last state values.** The last `glViewport` rectangle and the last `glClearColor`.
- **Client-side vertex arrays.** How many draws used them, and the byte range of the first
  materialization.
- **Pixel uploads.** The first `glTexImage2D` size/format/type, and whether its data pointer was
  null.

Suspects in order:
1. shader text marshaling;
2. client-array materialization;
3. viewport or framebuffer binding (drawing into an off-screen FBO, or a zero-size viewport);
4. pixel data marshaling for textures.

Keep the report additive, rebuild the APK, and ask the user for one more run.

**User observation for the black-screen run:**
- On launch: a white screen with the navigation bar visible (the activity background before the
  GL surface exists).
- Then the screen turns black and goes fullscreen, in landscape at once.

So the `GLSurfaceView` is created and its frames are presented, but they show nothing. Rule out
"frames never presented". Focus on shader status, draw calls, the framebuffer bound at draw
time (an off-screen FBO never resolved to framebuffer 0?) and the clear color and viewport.

## Other guests tried on the device (2026-09-17)

After Orange Roulette rendered, two more games were imported on the OnePlus 13. Each failure was
a launcher or loader gap, not a translation bug:
- **Flappy Bird** (`com.dotgears.flappybird`, AndEngine, ProGuard): now runs and is playable.
  Three fixes: unmatched `Java_*` exports are skipped (`6a68580`), Google Play services
  exceptions on the guest main thread are swallowed (`efe55d1`), and explicit intents for plugin
  activities resolve from the plugin's APK so old AdMob finds `AdActivity` (`351c286`).
- **Lane Racer** (`com.boombit.LaneRacer`, Unity + prime31): blocked. Its activity extends
  `NativeActivity`, which needs `ANativeActivity_onCreate` in the library ART loads; our arm64
  proxy has no such symbol. `80a9e6a` fixed the earlier `getActivityInfo` failure (the intent
  names the plugin's own package), so the run now reaches exactly that point.

**NEXT (needs its own spec): the `NativeActivity` bridge.** The proxy must export
`ANativeActivity_onCreate`, build a guest-side `ANativeActivity` with its callbacks, and bridge
`ANativeWindow`, `AInputQueue`/`AInputEvent`, `ALooper`, `AConfiguration` and `AAssetManager`.
Berberis has prior art (`WrapGuestJNIOnLoad`, `ANativeActivity_onCreate`). That unlocks Unity 4/5
and pure-NDK guests. Unity also needs `ZB_PRECISE_FAULTS` (Mono uses SIGSEGV for null checks).

Launcher work that came out of these runs: `MainLooperGuard`, `AdHider` (per-plugin, on by
default, toggled from the long-press menu), and package-manager answers for plugin components.

## HANDOFF 2026-09-17 (evening): Phase 5 accepted, Phase 7a code complete

**Orange Roulette renders and is playable on the OnePlus 13** (`docs/phase5-acceptance.md`). The
black screen was texture uploads in `GL_BGRA_EXT`, which `gl_pixel_bytes` rejected silently
because the guest never calls `glGetError`. Two lessons now enforced in code: rejected GL calls
are recorded in the report (`gl-rejections`, first 4 with arguments), and marshaling tables built
from the core GLES2 spec must also accept widely used extensions.

Guests tried after that, each failure a launcher/loader gap, not a translation bug:
- **Flappy Bird runs and is playable.** Fixes: unmatched `Java_*` exports are skipped as ART does
  (`6a68580`), Google Play services exceptions on the guest main thread are swallowed
  (`efe55d1`, `MainLooperGuard`), explicit intents for plugin activities resolve from the plugin
  APK so old AdMob finds `AdActivity` (`351c286`), component queries naming the plugin's own
  package are answered (`80a9e6a`).
- **Lane Racer (Unity)** needs `ANativeActivity_onCreate`: part 2 of Phase 7.
- **A Flutter guest** failed at guest `dlopen` with `libEGL.so not found`: that is what Phase 7a
  builds.

Launcher work from these runs: `AdHider` (per-plugin, on by default, toggled in the long-press
menu), `GuestWindowStyle` (default theme picked by the plugin's targetSdk, `FEATURE_NO_TITLE` for
translated plugins before their content is set, immersive system bars for fullscreen guests).

### Phase 7a (EGL + ANativeWindow): tasks 1-9 done, task 10 open

Spec `docs/superpowers/specs/2026-09-17-native-surface-design.md`, plan
`docs/superpowers/plans/2026-09-17-phase7a-native-surface.md`. Commits `a4cade4`, `b1f930a`,
`eb6cd42`, `ec52fa6`, `c651666`, `0e04845`, `63377b7`, `0962702`, `6f0e201`, `b83c4ae`.
Host suite is 43/43; guest suite 9/9; `libzbridge.so` links `libEGL.so`.

What exists now:
- guest `libEGL.so` (44 entry points) and `ANativeWindow_*` in guest `libandroid.so`;
- `tools/gen_egl.py` from `third_party/registry/egl.xml`, checked by `gen_egl_check`;
- `HostEgl` (`core/src/gl/host_egl.cpp`, `egl_manual.cpp`): 32-bit handles for every EGL object,
  21 hand-written cases, a thread-local pending error served through `eglGetError`;
- `HostNativeWindow` (`core/src/android/host_native_window.cpp`) plus `HostJni::new_local_handle`;
- report section `egl-*` with the `eglMakeCurrent`/`gl*` thread mismatch check;
- real backends in `core/android/`, wired through `guest_jni_runtime`;
- `zbeglprobe`: window handle -> window surface -> make current -> clear -> swap, end to end.

**Host-call index ranges are now GLES 0-141, AAsset 142-159, ANativeWindow 160-167, EGL 168-211,
JNI 0xFB00+.** New stub names must be **appended**, never merged into an existing sorted list:
`core/include/zb/asset_hostcalls.h` and `core/include/zb/window_hostcalls.h` hold hand-written
literal indices. `gen_gles.py`'s index check now rebuilds its expectation from every stub library
(`eb6cd42`), so adding a library no longer breaks it.

**NEXT (task 10):** the APK at `/sdcard/ZettaBridge-debug.apk` (built 2026-09-17 19:28 UTC)
carries all of this. Ask the user to run the Flutter guest, then write `docs/phase7a-acceptance.md`
from the report and update `CLAUDE.md`. Expected in the report: a created context and window
surface, `egl-swaps` rising, no `egl-thread-mismatch`, `unimplemented-host-calls: 0`. If the guest
needs GLES 3.0 (Impeller), the report names the missing functions; that is the next plan, not a
patch to this one.

Open risks: contexts follow the **host** thread, so a Java thread borrowing a carrier can take a
context somewhere unexpected - the mismatch line exists to catch exactly that. `ANativeWindow_lock`
is deliberately unimplemented (the buffer lives outside the guest's 4 GiB space).

## Phase 7a Task 10 first device result: launcher omitted `libEGL.so`

The first Flutter rerun did not exercise the Phase 7a bridge. Its report stopped before
`JNI_OnLoad` with `libflutter.so guest dlopen failed: library "libEGL.so" not found`. Root cause
was the launcher packaging path: `build/guest/lib/libEGL.so` existed, but
`tools/make_launcher_bundle.sh`, `tools/check_launcher_bundle.py` and
`RuntimeBundle.requiredFilesPresent` still listed only the older guest runtime libraries.

The bundle validator was changed first and observed failing with exactly the missing `libEGL.so`.
The packaging script and runtime completeness check now require it. Launcher contracts, the
bundle validator, Android `zbridge`/`zbproxy` targets and the Gradle debug APK build pass; the
final APK contains `assets/zb/guest/lib/libEGL.so`.

**NEXT:** install `/sdcard/ZettaBridge-debug.apk` (SHA-256
`8c196fbc9ed682d3cf4d364a4e51404c856a8297651cfdb7dbac80a8c8af2db2`), force-stop or reimport
`com.example.perecup_simulator`, rerun it, and send the Last run report. Task 10 remains open
until that report is recorded in `docs/phase7a-acceptance.md`.

## Phase 7a Task 10 second device result: Flutter loader compatibility

After `libEGL.so` was bundled, the next device report stopped at missing
`libjnigraphics.so`. Inspection of avtobuy's armeabi-v7a `libflutter.so` found sixteen direct
Android platform imports outside the Phase 7a core surface: three `AndroidBitmap_*`, two
software `ANativeWindow_*`, two EGLImage KHR functions, one GLES OES function and eight
`ALooper_*` functions. Three unresolved `OPENSSL_memory_*` names are weak and legally remain
null; the real guest linker confirmed they do not block loading.

The append-only compatibility surface now occupies indices 212-227 without moving any old
index. `tools/gen_stubs.py` can append later groups to an existing stub library and generates a
new arm32 `libjnigraphics.so`. `HostPlatformCompat` claims these indices, records every use as an
unimplemented host call, and returns safe failure values: `-ENOSYS` for window locking,
`ANDROID_BITMAP_RESULT_BAD_PARAMETER` for bitmap access, `ALOOPER_POLL_ERROR`/failure values for
loopers, and null/false for EGLImage/OES calls. This is deliberately loader-first; the next report
will identify which full pointer bridge is actually needed.

TDD evidence: `platform_compat_test` failed first on the absent handler and again on absent looper
constants, then passed all sixteen result/report cases. The bundle validator failed first on
missing `libjnigraphics.so`. A real translated `dlopen(RTLD_NOW)` of avtobuy's arm32
`libflutter.so` now succeeds against the generated guest libraries. Fresh verification: host
44/44, guest 9/9, GLES/EGL/JNI generators pass, Android `zbridge`/`zbproxy` link, bundle validator
passes and the Gradle debug APK builds.

**NEXT:** install `/sdcard/ZettaBridge-debug.apk` (SHA-256
`1781da56accd7c25698805a5dd1f15a8b4112d063af2f79d1cea7c1971847455`), force-stop or reimport
`com.example.perecup_simulator`, rerun it, and send the Last run report. If it reaches one of the
safe fallbacks, implement only the first path it actually uses. Task 10 is still open.

## Phase 7a Task 10 third device result: minimal Flutter looper bootstrap

The loader-first APK reached a gray Flutter surface. The report proved that `libflutter.so`
loaded successfully (`JNI_OnLoad` 1.4, 42 natives registered), then called
`ALooper_forThread`, `ALooper_prepare` and `ALooper_acquire` before aborting with status 134.
The compatibility fallback violated the NDK contract by returning null from
`ALooper_prepare`, whose successful return must be a non-null per-thread looper.

`HostPlatformCompat` now assigns a stable non-null opaque handle to each `GuestThread` on its
first `ALooper_prepare`; `ALooper_forThread` returns null before prepare and the same handle
afterward. Concurrent guest threads receive distinct handles. `ALooper_acquire` and
`ALooper_release` maintain a synchronized reference count while preserving the thread-owned
base reference. The remaining fd operations (`addFd`, `pollOnce`, `removeFd`, `wake`) remain
safe reported fallbacks on purpose, so the next device run will show whether Flutter needs a
real fd/callback event loop.

TDD: the expanded `platform_compat_test` first failed at `looper != 0`, then passed with the
minimal implementation. Fresh verification: generators pass, host 44/44, guest 9/9, Android
`zbridge`/`zbproxy` link, launcher bundle validation passes, and Gradle `assembleDebug` passes.

**NEXT:** install `/sdcard/ZettaBridge-debug.apk` (SHA-256
`24a7c18c0dd81ae3ea6639efcd8d9b258c00c3dd912162e21e70dd2ea5cdb21a`), force-stop the launcher,
rerun `com.example.perecup_simulator`, wait about 10 seconds, and send the Last run report. A
likely next boundary is `ALooper_addFd`; do not implement polling or guest callbacks without the
device evidence. Phase 7a Task 10 remains open.

## Phase 7a Task 10 fourth device result: real ALooper is required

The per-thread looper APK advanced to exactly one unimplemented call:
`ALooper_addFd`, then exited with status 134. Loader/JNI state remained healthy: one proxy,
guest `JNI_OnLoad` 1.4, 42 registered natives, and no GL call yet.

The actual arm32 `libflutter.so` was disassembled rather than guessing. It creates an eventfd,
registers it as callback-based input, requires `ALooper_addFd` to return 1, then loops in
`ALooper_pollOnce(-1, NULL, NULL, NULL)`. Its guest callback reads the eventfd and dispatches
Flutter work. Therefore merely returning success from `addFd` would leave an infinite empty
poll and a gray screen.

The user approved the real callback-looper approach. The amended design is in
`docs/superpowers/specs/2026-09-17-native-surface-design.md`: extract a focused `HostLooper`,
poll shared process fds on the host, use an internal eventfd for `wake`, and invoke callbacks
through the existing nested `LibraryRuntime::call_on_current` path. This subset is reusable by
the later NativeActivity bridge; input queues and configuration remain out of Phase 7a.

**NEXT:** review/approve the amended design, amend Task 10 in the Phase 7a implementation plan,
then implement it with a host poll test and a translated guest callback probe. Commit the plan
separately before implementation. Phase 7a Task 10 remains open.

### Update: Flutter ALooper plan approved and written

The user approved the amended design. The Phase 7a plan now has three remaining tasks:
Task 10 extracts `HostLooper` and implements real fd polling/non-callback results; Task 11 adds
the translated arm32 callback probe and nested callback dispatch; Task 12 runs regression,
builds the APK and records the device acceptance result. The plan preserves indices 220-227
and explicitly keeps NativeActivity/input/configuration out of this phase.

**NEXT:** execute Task 10 from
`docs/superpowers/plans/2026-09-17-phase7a-native-surface.md` with RED before production code,
update this file, and commit locally. Then execute Task 11 in its own commit.

### Phase 7a Flutter ALooper Task 10 done

Task 10 extracts all ALooper handling from `HostPlatformCompat` into the focused `HostLooper`.
It supplies per-`GuestThread` opaque handles, reference tracking, real host fd polling,
callback-less results with checked guest output pointers, add/replace/remove, timeout, and an
internal eventfd for cross-thread wakeups. Registered guest fds are never closed; only internal
wake fds belong to the bridge. `GuestJniEngine` always chains this handler before the remaining
loader-only compatibility fallbacks.

TDD evidence: `host_looper_test` first failed to compile because `zb/host_looper.h` did not
exist. It now passes with real eventfds, two guest threads and mapped guest outputs. Focused
`platform_compat_test` and `proxy_runtime_test` also pass. The callback-ready branch still
returns `ALOOPER_POLL_ERROR` deliberately; Task 11 replaces that branch only after the translated
arm32 callback probe is observed failing.

**NEXT:** execute Task 11: add `libzblooperprobe.so`, observe RED against callback polling,
dispatch the callback through `LibraryRuntime::call_on_current`, verify callback return-zero
removal, update this file and commit locally.

### Phase 7a Flutter ALooper Task 11 done

Task 11 adds `guest/testlib/zblooperprobe.c`, built as `libzblooperprobe.so` against the real
generated arm32 `libandroid.so` stubs. The translated probe prepares a looper, registers an
eventfd callback, writes the fd, polls, and proves that the guest callback reads the event and
returns `ALOOPER_POLL_CALLBACK`. A second callback returns zero and proves exact automatic
registration removal.

TDD evidence: before callback dispatch, `host_looper_guest` failed at guest probe line 44, the
literal `ALooper_pollOnce(...) == ALOOPER_POLL_CALLBACK` check. `HostLooper` now calls ready
callbacks through `LibraryRuntime::call_on_current(fd, events, data)` with no mutex held. A zero
result removes only the registration whose serial matches the poll snapshot, so a concurrent
replacement is preserved. Unit and translated modes both pass.

**NEXT:** Task 12: full generators/host/guest/Android regression, build and copy the launcher
APK, record its hash here, then ask for the Flutter device run. Do not write Phase 7a acceptance
until a visible frame and the runtime report satisfy the device gate.

### Phase 7a Flutter ALooper Task 12 local gate ready

Fresh verification after Tasks 10-11:

```text
gen_gles.py --check, gen_egl.py --check, gen_jni.py --check   PASS
ctest --test-dir build/host                                  46/46 PASS
tools/build_guest.sh                                         PASS
tools/run_guest_tests.sh                                     9/9 PASS
Android arm64 zbridge/zbproxy                                PASS
tools/make_launcher_bundle.sh                                PASS (7.1 MiB)
Gradle :app:assembleDebug                                    PASS
```

The ready APK is `/sdcard/ZettaBridge-debug.apk`, 9,226,800 bytes, SHA-256
`956780e0caee29702a903b36c31ca13946cab1918f9d8ec1727d9dfe592ac047`.
Its ZIP contains the generated guest `libandroid.so` and the rebuilt arm64 `libzbridge.so`.

**NEXT/device gate:** install this APK over the current launcher, force-stop it, launch
`com.example.perecup_simulator`, wait at least 10 seconds, and send both the visible result and
Last run report. Acceptance needs a visible Flutter frame, no guest exit, EGL activity/swaps,
no thread mismatch and no unimplemented call. If a new first missing function appears, record it
as a follow-up rather than declaring Phase 7a complete.

## HANDOFF 2026-09-17 late: Flutter crashes in arm32 libc before EGL

The Task 12 APK was installed and the user waited 15 seconds. The screen remained gray. This is
the real result (the earlier all-zero report was read too soon):

```text
proxy-loads: 1
proxy-failures: 0
proxy-loaded: libflutter.so jni=0x00010004
jni-onload-calls: 1
jni-onload: libflutter.so ok jni=0x00010004
registered-natives: 42
unimplemented-host-calls: 0
guest-exit: guest SIGSEGV: read of 0x0000000f, pc 0xfdb3c4c0 in libc.so offset 0x674c0
gl-calls: 0
egl-swaps: 0
```

Thus the loader, `JNI_OnLoad`, native registration and the new callback looper all advance
without a missing host call. The next failure is inside the guest arm32 bionic before EGL/GLES.
Do not reopen the ALooper fallback work unless new evidence points there.

### Current evidence and single hypothesis

Disassembly of `sysroot/system/lib/libc.so` places a Thumb `tbh [pc, r1, lsl #1]` at offset
`0x673f8`; its table starts at `0x673fc`. The reported crash PC `0x674c0` is exactly the table
halfword selected by `r1 == 98`, not an instruction. That entry is `0x007a`, so the architecturally
correct branch destination is `0x673fc + 2 * 0x007a == 0x674f0`.

Working hypothesis: either Dynarmic mishandles this PC-relative TBH case, or normal-mode fault-PC
reporting is stale and a later fault inside `__vfwscanf` merely reports the table address. Do not
patch `TableBranch` from this coincidence alone. The current implementation in
`third_party/dynarmic/src/dynarmic/frontend/A32/translate/impl/thumb32_load_store_dual.cpp`
appears correct on inspection (`PC() + 2 * ZeroExtend(ReadMemory16(...))`).

The first hypothesis is now rejected. `guest/tests/tbh_static.c` executes a real Thumb
PC-relative TBH through zbrun, checks both entry zero and the distant entry at index 98, and
returns the literal associated with each target. The focused translated run passed exactly:
`tbh near=17 distant=98`, exit 0. Keep this regression, but do not change Dynarmic's
`TableBranch`. The device PC was imprecise; the next APK must obtain the real faulting instruction.

### Roadmap for Claude/Codex

1. Make the launcher guest runtime enable `ZB_PRECISE_FAULTS=1` before `Process` construction (or
   add an equivalent explicit engine option) and persist the bounded guest register/fault context
   in `RuntimeReport`. Do this as a diagnostic build, because precise memory stops cost runtime
   performance. Rebuild and rerun the same Flutter plugin on the phone.
2. Symbolize the new exact PC in the bundled arm32 object and disassemble the surrounding basic
   block. Record the guest registers needed to trace the `0x0000000f` read back to its source.
3. Once the real failing instruction and bad input are proven, add the smallest regression at
   that boundary, implement one fix, run focused tests, then the full 46/46 host suite, 10/10 guest
   suite, generators, Android links, launcher bundle and Gradle APK. Commit the completed task and
   update this file.
4. Phase 7a Task 10 remains unaccepted until Flutter renders a visible frame and the report shows
   no guest exit, EGL context/window-surface activity and rising swaps.

Current branch is `codex/phase4d-launcher`. The only dirty path before this handoff was the
intentional existing `third_party/dynarmic` submodule baseline. The last known APK is
`/sdcard/ZettaBridge-debug.apk`, SHA-256
`956780e0caee29702a903b36c31ca13946cab1918f9d8ec1727d9dfe592ac047`.

## Flutter: why no frame is ever drawn (2026-09-18)

Device evidence, after EGL, GLES 3.0, the direct-buffer mirror, `AAsset_getBuffer` and the JNI
mirror-lifetime fix all landed: `gl-calls: 3507`, 140 shaders and 70 programs built,
`egl-surface: created`, `egl-swaps: 0`, zero draw calls, `guest-exit: (none)`, and the report's
thread census shows `1.raster` and `1.io` but **no `1.ui`**.

The looper diagnostics name the cause:
```
looper-prepared: 29365=0x7a000000 29371=0x7a000004 29372=0x7a000008
looper-addfd-1:  tid=29365 looper=0x7a000000 fd=138 ident=-2 events=0x1 result=1
watch: 29365=sys:timerfd_settime x807 stuck=21 S cpu=0 in=futex_wait_queue
```
- 29371 (`1.raster`) and 29372 (`1.io`) are guest-created threads: they poll their loopers and the
  bridge dispatches their callbacks (21 so far). That half works.
- 29365 is a **Java thread** (`flutter-worker-`) that entered the guest on a borrowed carrier. On
  it, the engine built its platform/UI message loop: `ALooper_prepare`, `ALooper_addFd` with a
  callback, `timerfd_settime`. It then returned to Java and never polls.

On a real device that is correct engine behaviour: `MessageLoopAndroid` attaches its timerfd to the
**thread's own Android looper**, and Java's `Looper.loop()` polls it. In our world the guest's
looper is `HostLooper`, a separate thing the Java looper knows nothing about, so the timerfd never
fires, the UI task runner never runs, Dart never starts, and nothing is ever drawn.

**NEXT: attach guest loopers that live on host threads to the real Android looper.** When
`ALooper_prepare`/`addFd`/`wake` happen on a borrower (a host thread that entered the guest), the
fd must be registered with that host thread's real `ALooper` through the NDK, with a host callback
that enters the guest and runs the guest callback. Guest-created threads keep today's path, which
is proven to work. Keep the NDK behind a backend seam (like `GlBackend`/`AssetBackend`) so host
tests can drive it without Android.

### Done 2026-09-18: guest loopers on host threads attach to the real Android looper

`AndroidLooperBackend` (`core/include/zb/android_looper_backend.h`) is the seam over
`<android/looper.h>`; the real one is `core/android/looper_driver_backend.*` and the host-test
mock is `tests/host/mock_looper.h`. `HostLooper` now decides per calling thread:

- a guest-created thread keeps the old poll set and semantics, unchanged;
- a borrower (`LibraryRuntime::is_borrower`, backed by `Process::is_borrower`) prepares the real
  looper of its host thread and registers its fds there. The host callback fires on that thread
  from Java's `Looper.loop()`, enters the guest through `HostJni::call_on_host_thread` (the
  thread's cached carrier, so the guest identity matches the one that registered the fd) and runs
  the guest callback with `(fd, events, data)`. A guest callback returning 0, or a guest that
  cannot be entered, unregisters the fd on both sides.
- `ALooper_wake` forwards to the real looper; `ALooper_pollOnce` on such a looper returns
  `ALOOPER_POLL_WAKE` at once (logged once) instead of blocking a loop Java owns.

New report lines: `looper-attached: <n>` and `looper-host-callbacks: fired=<n> guest=<n>
failed=<n>`.

Local gate: 47/47 host tests, Android `zbridge`/`zbproxy`, launcher bundle and Gradle debug APK
all pass. `/sdcard/ZettaBridge-debug.apk` is 9,344,152 bytes, SHA-256
`d0bd5ffdfa32b5d1801410ae6a3049263ca404bb7b027ecf3740d8873499de42`.

**NEXT/device gate:** install it, launch the Flutter plugin, and check the report for
`looper-attached` at least 1, `looper-host-callbacks` with `guest` rising, a `1.ui` thread in the
census, and rising `egl-swaps`.

## Done 2026-09-18: A32 ASIMD narrowing instructions (VADDHN/VRADDHN/VSUBHN/VRSUBHN)

Flutter rendered 9 frames and 512 draws but died decoding PNGs with `guest SIGILL: undefined
instruction`; the faulting instruction at `libflutter.so+0x3cd85e` is `vraddhn.i16 d24, q10, q11`
in an alpha-premultiply loop (crash `r11 = 0x49444154`, "IDAT"). Dynarmic's A32 decoder had the
whole "add/subtract returning high half" family commented out.

`HighNarrowingOperation` in
`third_party/dynarmic/src/dynarmic/frontend/A32/translate/impl/asimd_three_regs.cpp` implements
all four, following the A64 frontend's `simd_three_same.cpp`: `VectorAdd`/`VectorSub` at
`2 * esize`, then for the rounding variants a `VectorBroadcast` of `1 << (esize - 1)` added at
`2 * esize`, then `VectorLogicalShiftRight` by `esize` and `VectorNarrow`. Sources are Q
registers (low bit of `Vn`/`Vm` must be clear), the destination is a D register, `sz == 0b11` is a
decode error, and the carry out of the `2 * esize` sum is discarded, which is what the ARM ARM's
unbounded-integer pseudocode specifies once bits `<2N-1:N>` are extracted.

Captured as `third_party/patches/dynarmic-0002-asimd-narrowing.patch` (the submodule pointer is
never staged). Still commented out in the A32 decoder: `VQRSHL`, `VQDMLAL`, `VQDMULL`,
`VQDMLAL_scalar`.

`guest/tests/asimd_narrow_static.c` runs each instruction at `.i16`, `.i32` and `.i64` with
hand-computed expected values covering the rounding boundary (a low half of exactly
`1 << (N-1)`), the rounding carry that wraps the whole `2N`-bit value to zero, a carry out of the
sum and a borrow. It is `run_case asimd_narrow_static 0` in `tools/run_guest_tests.sh`.

Local gate: 47/47 host tests, 11/11 guest tests, Android `zbridge`/`zbproxy`, launcher bundle and
Gradle debug APK all pass. `/sdcard/ZettaBridge-debug.apk` is 9,353,448 bytes, SHA-256
`e3855dcbaf9c4f2ea0a4d1e893263782244d36b0032c3b5316b10369cbc75e06`.

**NEXT/device gate:** install it and run the Flutter plugin past PNG decoding; the previous SIGILL
at `libflutter.so+0x3cd85e` must be gone.

## Flutter status 2026-09-18 evening: it runs, text and some images are missing

The Flutter guest (`com.example.perecup_simulator`) starts, runs Dart, presents frames and takes
touch: `egl-swaps` in the hundreds, `gl-draws: 5376+`, `native-calls` includes
`nativeDispatchPointerDataPacket`, no crash, `unimplemented-host-calls: 0`. Gradients, glows,
vector icons and most raster icons are correct on screen. What is wrong: **no text at all**, and
4 of 12 raster icons show Flutter's "broken image" placeholder.

Hypotheses ELIMINATED by device evidence (do not revisit without new data):
- Glyphs rasterize and upload fine: `gl-glyph-atlas: uploads=38 bytes=4622226 nonzero=161022`,
  individual uploads are real glyph shapes (`76x74 GL_ALPHA nonzero=4482/5624`).
- The atlas is uploaded and drawn on the same thread and context (tid of `1.raster`).
- Both EGL contexts share correctly (`egl-context-2: share-handle=0x202 share=0xb4000075...`).
- Image uploads are fully populated (`2048x2048 nonzero=16777216/16777216`) from the io thread.
- Uniform block wiring is right: `FragInfo`->binding 0, `FrameInfo`->binding 1 per program.
- `glBindBufferRange` marshaling is right: `offset-raw32=0x2400 offset-passed64=9216`,
  `size-raw32=0x40 size-passed64=64`.
- No GL errors and no marshaling rejections in any run (`gl-first-error: (none)`, no
  `gl-rejections` line).

**The open lead.** There are no `gl-ubo-buffer-*` lines at all: Impeller never uploads uniforms
with glBufferData/glBufferSubData. It orphans one 1 MB buffer (`gl-buffer-data-1: target=
GL_ARRAY_BUFFER size=1024000 usage=GL_STREAM_DRAW data=null`) and writes both vertex and uniform
data through **glMapBufferRange**, which we serve with a guest mirror
(`core/src/gl/gl_manual.cpp`, `zbgl_manual_glMapBufferRange` / `glFlushMappedBufferRange` /
`glUnmapBuffer`). Vertex data evidently survives that path (shapes draw), so the mirror works in
the common case, but anything subtler about it (when the copy-back happens relative to the draw,
explicit-flush ranges, a second thread mapping the same target, the mapping table being keyed by
target alone in `mappings`) would corrupt exactly the uniform side and leave shapes intact.

NEXT, in order:
1. Record the mirror traffic: for the first mapped ranges, the access flags, the guest mirror
   address, and a checksum of the bytes at map, at each flush and at unmap. If the uniform bytes
   the guest wrote never reach the driver buffer, that is the bug.
2. `mappings` in `gl_manual.cpp` is keyed by GLenum target only and is process-wide. Two contexts
   (raster and io) mapping the same target at once would collide; a collision currently rejects
   the second map. No rejection was observed, but the key should be (context, target, buffer).
3. `gl-text-draw-N` readbacks came back `changed=no` with an all-zero "before", which is expected
   when the draw targets an offscreen framebuffer (`gl-sample-draw-3000: fb=38`). Read from the
   framebuffer that is actually bound at that draw before concluding anything.

### Done 2026-09-18: mapped-buffer mirror traffic diagnostics

The first lead above is implemented without changing map/copy semantics. The first 12 successful
`glMapBufferRange`/`glMapBufferOES` calls now get a stable diagnostic id and report:

- actual host EGL context, target, currently bound buffer, mapped offset/length/access and guest
  mirror address;
- FNV-1a checksums of mirror and driver bytes immediately after map;
- the flushed subrange's mirror and driver checksums after copy-back;
- the complete mirror and driver checksums after unmap copy-back.

A process-wide target collision records both the new and existing context/buffer/map id before
the existing `GL_INVALID_OPERATION` rejection. This will distinguish broken copy-back from the
known architectural risk that the mapping table is keyed only by target.

TDD evidence: `gles_marshal_test` first failed because `gl-map-1` was absent. It now verifies the
real host-call path with context `0xc0ffee`, buffer 77, exact independently computed FNV values at
map/flush/unmap, and a second-map collision. Fresh gate: host 47/47, guest 11/11, Android
`zbridge`/`zbproxy`, launcher bundle and Gradle debug APK all pass.

Ready APK: `/sdcard/ZettaBridge-debug.apk`, 9,370,184 bytes, SHA-256
`6c86772a0d3aa15e4d0e6bb5f44bfa9922512395887585b351d255f1dc62ad40`.

**NEXT/device gate:** install this APK, force-stop the launcher, run avtobuy for long enough to
render the affected screen, then send the complete Last run report. Compare every `gl-map-N`
with its `gl-map-flush-N`/`gl-map-unmap-N`: unequal post-copy checksums prove the mirror path;
any `gl-map-collision-N` proves the target-only key is wrong. If both are clean, stop changing
mapped buffers and investigate the four broken image assets separately from the text pipeline.

### Device result: avtobuy does not use the mapped-buffer mirror

The full report from the diagnostic APK had 122,018 GL calls, 4,608 draws and 63 swaps, but no
`gl-map-*` line at all. There was no guest exit, unimplemented call, GL error or rejection. The
map/flush/unmap hypothesis is therefore rejected for this run; do not change that bridge path to
fix the missing text.

The report instead confirms the real Impeller upload shape: it orphans a 1,024,000-byte
`GL_ARRAY_BUFFER` with null data, then binds slices of the same buffer id 1 through
`glBindBufferRange(GL_UNIFORM_BUFFER, ...)`. Existing diagnostics ignored
`glBufferSubData(GL_ARRAY_BUFFER)`, so the uniform bytes were invisible even though the bind
arguments were correct.

### Done 2026-09-18: correlate combined buffer uploads with UBO ranges

The diagnostic state now keeps a bounded (four buffers, 2 MiB each) byte/knownness shadow for
`glBufferData` and `glBufferSubData` targeting either `GL_ARRAY_BUFFER` or `GL_UNIFORM_BUFFER`.
The first 12 sub-data uploads report buffer, offset, size and FNV. Each of the first eight
`glBindBufferRange` records whether its exact range is fully captured, partially captured or
absent; a fully known range includes its FNV. This changes no driver call or guest semantics.

TDD evidence: `gles_marshal_test` first failed on the absent `gl-buffer-upload-1`. It now orphans
buffer 91, uploads independently known bytes 0..63 through `GL_ARRAY_BUFFER`, binds bytes 16..31
as a UBO, and checks the exact whole-upload and bound-slice FNV values. Fresh gate: host 47/47,
guest 11/11, Android `zbridge`/`zbproxy`, launcher bundle and Gradle debug APK all pass.

Ready APK: `/sdcard/ZettaBridge-debug.apk`, 9,376,552 bytes, SHA-256
`4e367d04cc471ccbe56d92183d3bd27b40a94872ee29e261b84f02ddd4912829`.

**NEXT/device gate:** install this APK, force-stop the launcher, reproduce the missing text, and
send the complete Last run report. The decisive lines are `gl-buffer-upload-*` and
`gl-ubo-bind-*-data`. `captured=no/partial` identifies a missed upload path; `captured=yes` gives
the real uniform bytes' checksum and rules out pointer loss in the bridge. Keep the four broken
raster assets as a separate decoder issue unless the new evidence links them.

### Device result: combined buffer and UBO marshaling are correct

The next report still had no text, but all first eight UBO bindings were `captured=yes`. In
particular, program 1's 64-byte `FrameInfo` and 32-byte `FragInfo` ranges were wholly contained in
the preceding `glBufferSubData(GL_ARRAY_BUFFER)` upload. Buffer id, offsets, sizes and checksums
were stable. This rules out pointer loss, missed sub-data calls and the combined-buffer target
alias as the text cause.

The remaining concrete lead is the glyph texture format. The exact Impeller shader embedded in
avtobuy's arm32 `libflutter.so` samples `_21.w` (alpha) because
`use_alpha_color_channel == 1.0`. The report shows texture 4 allocated and updated as legacy
`GL_ALPHA` (`0x1906`) in an ES3 context. Khronos' registry classifies `GL_ALPHA` only as a pixel
format, not an ES3 internal format; a strict driver can reject the allocation. Flutter's release
build does not call `glGetError`, so that would leave an empty atlas with no report error and make
all text transparent, exactly the symptom.

**NEXT:** extend the existing host test first, then report (a) the first 32 UBO bytes as hex and
(b) the immediate driver error after the first legacy alpha/luminance texture allocation, while
re-queuing that error for guest semantics. If the device reports `GL_INVALID_ENUM`/`VALUE`, add a
tested GLES2-on-GLES3 compatibility translation: `ALPHA -> R8/RED` plus swizzle `(0,0,0,R)`,
`LUMINANCE -> R8/RED` plus `(R,R,R,1)`, and `LUMINANCE_ALPHA -> RG8/RG` plus `(R,R,R,G)`; translate
matching sub-images too. This likely also explains some missing raster assets, but do not land the
translation until the immediate error proves it.

### Ready 2026-09-18: text uniform and legacy texture diagnostic

The diagnostic above is implemented without changing rendering semantics. The first eight captured
UBO ranges now include their first 32 bytes as hex. Immediately after the first legacy
`ALPHA`/`LUMINANCE`/`LUMINANCE_ALPHA` allocation, diagnostics query the real driver error, report
it as `gl-legacy-texture-error`, and re-queue a nonzero error so the guest still observes it.

TDD evidence: the focused GLES test first failed on the absent UBO `head`; it now verifies the exact
bytes, an injected `GL_INVALID_ENUM`, its report line, and that the error remains observable. Fresh
gate: focused test PASS, host 47/47, Android `zbridge`/`zbproxy` link, launcher bundle and Gradle
debug APK build all pass.

Ready APK: `/sdcard/ZettaBridge-debug.apk`, 9,377,080 bytes, SHA-256
`f2b7a3a35a3b153ba9c0e68a36b44be2db6a932378ed6b4b0b1ed14e325f7110`.

**NEXT/device gate:** install this APK, force-stop the launcher, reproduce the missing text and send
`gl-legacy-texture-error`, `gl-ubo-bind-1-data` and `gl-ubo-bind-2-data` (the full report is still
preferred). A nonzero legacy texture error authorizes the tested compatibility translation above;
an error of zero means decode the two `head` fields as little-endian float uniforms before changing
texture formats.

### Device result and A/B build: legacy alpha sampling

The device reported `gl-legacy-texture-error ... error=0x0`, so allocation is accepted. The first
text `FragInfo` is also valid: `is_color_glyph=0`, `use_text_color=1`, and `text_color` is
`(0,0,0,0.5019608)`; the two intervening words are std140 padding. Upload, binding, uniform contents
and allocation error are therefore ruled out. A one-off first-run crash was not reproduced on the
second run and the saved report has no guest exit.

The next build is a controlled A/B test of the remaining sampling-semantic boundary. Guest
`GL_ALPHA` allocations are stored as ES3 `GL_R8/GL_RED`, with texture swizzle `(0,0,0,R)` restoring
the GLES2 sampled value; matching `GL_ALPHA` sub-images use `GL_RED`. Other formats are unchanged.
The host test observed RED first and now asserts the translated allocation, all four swizzles and
the translated sub-image. Fresh gate: host 47/47, Android `zbridge`/`zbproxy`, launcher bundle and
Gradle debug APK all pass.

Ready A/B APK: `/sdcard/ZettaBridge-debug.apk`, 9,377,272 bytes, SHA-256
`92e1e04b66716bc1a41a432658d2e6fa88b21f73a232df39dbf6409344145c39`.

**NEXT/device gate:** install, force-stop and launch avtobuy twice. Report whether text or the four
missing images change and whether either run exits. If there is no improvement, revert this A/B
compatibility commit and instrument a real onscreen text draw (the current first three text samples
are Impeller's 2x2 offscreen warm-up and do not prove the later onscreen draw output).

### A/B result: reverted

The device run with the GL_R8/GL_RED swizzle A/B showed no text improvement, only partial image
recovery and occasional crashes, so the acceptance criterion failed. The A/B compatibility commit
is reverted; the sampling-semantic boundary it targeted is ruled out. Next: instrument the real
onscreen text draws instead of the offscreen warm-up samples.

## 2026-09-30: Unity 4.5 (Thomas Was Alone) brought up to frames; a race remains

Phase 7b (NativeActivity, input, configuration) is implemented through task 7 and pushed to
`main` (31 commits, `61f4656`). On the OnePlus 13, Unity 4.5 with Mono now loads, runs, creates
its EGL context and draws: up to 107k GL calls and 1192 presented frames in a run, with audio
(FMOD) and touch reaching the guest. The game still does not get past its splash.

### Fixed today, each found from one report or log line

- `libzbproxy.so` exports `ANativeActivity_onCreate` (the framework looks it up in the library the
  manifest names, which for a plugin is the proxy). Its own check refused the new export, so the
  proxy silently kept building without it; `tools/check_zbproxy.py` now names both entry points.
- A per-plugin copy of the proxy is replaced when the template is newer; it used to be created
  once and reused forever, so a fixed proxy never reached an already-imported plugin.
- CP15 memory barriers (`mcr p15, 0, rX, c7, c10, 4/5` and `c7, c5, 4`). Mono writes the pre-ARMv7
  form in its own barrier; an unserved CP15 write ended the process through `std::terminate` with
  no catchable exception. `guest/tests/cp15_barrier_dynamic.c`.
- `ALooper_pollAll`, gone from the NDK headers, still imported by Unity 4.5's `libmain.so`.
- `libGLESv1_CM.so`, which `libunity.so` lists as a dependency. Adding it caused the next problem:
  its 145 names got their own host-call indices, and the guest linker bound Unity's texture and
  state calls to those empty stubs. A name exported by two guest libraries is now **one** host
  call (`tools/gen_stubs.py`), reached through either.
- `GL_HALF_FLOAT_OES` uploads (Unity probes half-float support with an 8x8 texture).
- `SA_RESTART`: a syscall that ends in `EINTR` records how it was made, and a signal whose action
  carries the flag rewinds to the `svc` and restores the arguments. Restarting *everything* was a
  regression (a wait with a deadline restarted forever, zero frames); the rule now matches the
  kernel - see `restartable_syscall` in `core/src/syscalls.cpp`.
- Diagnostics that made the above findable: a terminate handler that records the guest pc and the
  library it falls in, a catch around `GuestThread::run`, a catch around host calls, the pixel
  format and type of a rejected upload, and archive opens in the report.

### Ruled out, with evidence

- **The guest's zlib is correct under translation.** `guest/tests/zlib_dynamic.c` compresses and
  decompresses 256 KiB through the sysroot's own `libz.so`: byte-identical, checksums match.
- **Guest file reading is correct.** `guest/tests/filemap_dynamic.c` covers a whole-file mapping,
  a window mapping at an offset, `pread` at unaligned offsets, a read past the end and a seek to a
  large offset: all match the source.
- **The plugin opens the right archive.** The report shows
  `plugins/com.bossastudios.twadroid_humble/base.apk`, not the launcher's APK.

### The open problem

Runs of the same build differ wildly: 3, 413, 617, 1192 presented frames. One run showed 135k
`Inflate Error: invalid stored block lengths` from Unity (corrupt zip input) and exited; the next
run, with only a reporting change in between, showed none and ended in an ANR instead. That is a
race, not a missing feature, and the code most recently touched around it is signal delivery and
the restart of interrupted syscalls.

**Next step (planned, not started): reproduce it on this machine.** A guest stress test where
several threads read a file, wait on futexes and semaphores while another thread signals them
continuously, checking that the bytes read match the source and that no thread stops making
progress. If it reproduces here, the cycle is a rebuild rather than an APK reinstall and a device
run. Start from `guest/tests/sigrestart_dynamic.c` and `guest/tests/filemap_dynamic.c`.

Device loop, for reference: `ninja -C build/android-arm64`, `tools/make_launcher_bundle.sh`,
`cd android/launcher && ANDROID_HOME=$HOME/android-sdk ./gradlew --offline -q assembleRelease`,
copy to `/sdcard`. The report the phone writes is
`/storage/emulated/0/Android/data/com.zettabridge.launcher/files/zb-runtime-report.txt`;
`su -c "logcat -b all --pid=$(su -c pidof com.zettabridge.launcher:guest)"` is the only way to see
the guest's own log, because the ROM drops third-party logcat.

## 2026-10-01: signal/syscall/file-I/O race stress test does not reproduce locally

The planned host-side reproduction is implemented as `guest/tests/sigio_race_dynamic.c` and runs
in the normal guest suite. Three guest threads each perform 1,000 patterned `pread` checks, an
`SA_RESTART` `sem_wait`, and an untimed raw futex wait. A fourth thread continuously sends
`SIGUSR1` to every worker while the main thread releases the semaphore and futex phases. The test
fails on a wrong byte, a short/error read, any leaked `EINTR`, another unexpected wait result, no
signal observed in either wait phase, missing progress, or its 30-second alarm.

Results on the current `codex/unity-signals` / `7c647a4` runtime:

- one focused run: 3,000/3,000 worker iterations, both wait phases signalled, zero errors;
- 100 consecutive focused runs: 0 failures (300,000 worker iterations total);
- the complete guest suite, including the new case: PASS;
- `tools/build_guest.sh` now links the already-existing `zlib_dynamic` probe with `-lz`; without
  that, a clean guest rebuild failed before reaching any test.

This evidence does **not** reproduce the Unity variability on the host and therefore does not
justify another signal-restart change. The restart PC/register path and the basic concurrent
file-read/semaphore/futex combination survive this workload. The next useful step is to run the
same stress shape inside the Android app process, where ART/libsigchain and process-directed host
signals are present, or add bounded device-report counters for `EINTR -> restartable -> delivered
with SA_RESTART -> rewound` transitions and compare a short-frame Unity run with a long one.

## 2026-10-01: Android app-process signal race test APK ready

The first option above is wired into the existing T6 harness. `sigio_race_dynamic` is packaged as
an asset and run in the T6 activity's dedicated `:guest` process, next to ART and libsigchain. The
T6 Gradle project now consumes `build/t6` directly and uses the same external release-signing file
as the launcher, so its release APK is installable without copying generated sources into the
project tree.

Local build verification:

- Android arm64 `zbridge`: PASS;
- T6 bundle: PASS, and its file list contains the executable and expected output;
- Gradle `assembleRelease`: PASS;
- APK Signature Scheme v2 verification: PASS, one signer;
- APK archive contains `lib/arm64-v8a/libzbridge.so`, `assets/zb/guest/sigio_race_dynamic`, its
  expected output and `zb-files.txt`.

Ready APK: `android/launcher/app/build/outputs/apk/release/ZBridgeT6-signal-race-release.apk`,
21,826,717 bytes, SHA-256
`d85fc2e8638376ea01c46ce8c1b8bdf50fa66eb66511bb042cf9513af54b573e`.

**NEXT/device gate:** install this separate `com.zettabridge.t6` APK and open **ZBridge T6**. Send
the screen result around `sigio_race_dynamic` (or the last visible line if the `:guest` process
exits/hangs). `PASS sigio_race_dynamic` means the constructed race also survives the real Android
process; it still does not exonerate the exact Unity workload, so the following step is bounded
restart-transition counters in the launcher report rather than another speculative signal fix.

### Device result: Android signal race stress passes

The signed T6 release APK passed its complete on-device suite, including
`PASS sigio_race_dynamic`, on the OnePlus 13. The constructed concurrent file-read,
`SA_RESTART` semaphore/futex and continuous-signal workload therefore passes both under host
`zbrun` (100/100 repeated runs) and inside a real Android app process next to ART/libsigchain.

This rules out the basic restart mechanism under the tested shape; it does not reproduce the
Unity run-to-run variability. Do not change restart policy from this evidence. **NEXT:** add
bounded runtime-report counters for the real Unity path: syscall returns `EINTR`, classification
as restartable/non-restartable, signal delivery with/without `SA_RESTART`, and actual PC/register
rewind. Compare those counters and the last affected syscall between a short-frame failure and a
long run before proposing a fix.

## 2026-10-01: Unity signal-restart counters ready in launcher release

The bounded counters above are implemented without changing signal or restart semantics. The
runtime report now contains:

- `signal-restart`: totals for syscall `EINTR`, restartable/nonrestartable classification,
  delivery with/without `SA_RESTART`, and actual rewinds;
- `signal-restart-last-eintr`: syscall number/name, guest tid and classification;
- `signal-restart-last-delivery`: associated syscall, tid, action flag and whether PC/registers
  were restored.

The syscall path records only real `-EINTR` results. The signal path records only delivery while
a restartable syscall is pending. Counter notifications use the report writer's existing throttle;
there is no per-event file write and no new thread. TDD RED was the report test failing to compile
without the two event methods; it now checks exact counts, last events and `clear()`.

Fresh verification: focused report test PASS; `sigrestart_dynamic` and `sigio_race_dynamic` PASS;
host 54/54 PASS; complete guest suite PASS; Android arm64 `zbridge`, launcher bundle and Gradle
`assembleRelease` PASS. The APK's packaged `libzbridge.so` hash exactly matches the Android build,
and APK Signature Scheme v2 verification passes with one signer.

Ready launcher: `android/launcher/app/build/outputs/apk/release/app-release.apk`, 6,701,892 bytes,
SHA-256 `e5ddd39e39e97d016f257172c6c66bf45918ee92d62e78b7d9d56bd6fc642417`.

**NEXT/device gate:** install this release over the launcher and force-stop it. Run Thomas Was
Alone repeatedly, copying **Last run report after each run before starting the next**, until there
is one short-frame/exit or ANR run and one materially longer run. Preserve the full reports; the
decisive comparison is the three `signal-restart*` lines together with `guest-exit`, `egl-swaps`
and the watchdog lines. A mismatch between restartable EINTRs and associated deliveries is also
evidence; do not infer a fix from totals alone.

### Device result: Unity restart accounting is balanced; Thomas reaches the menu

Three OnePlus 13 runs of the release above produced a short pre-menu capture, a manually
force-closed capture, and a longer intro-plus-menu capture. None reported a guest exit, an
unimplemented host call, a GL/EGL error, or a proxy/JNI failure. The long run presented 1,305
frames from 121,648 GL calls and continued through the intro into the menu with audio and touch.

The restart transition accounting balances exactly in every run:

- short: 15 EINTRs, 7 restartable, 7 SA_RESTART deliveries, 7 rewinds;
- force-closed: 6 EINTRs, 3 restartable, 3 SA_RESTART deliveries, 3 rewinds;
- intro/menu: 47 EINTRs, 23 restartable, 23 SA_RESTART deliveries, 23 rewinds.

The last non-restartable calls were `nanosleep` or `rt_sigsuspend`, as expected, and every recorded
restartable delivery was a `futex` with `SA_RESTART`. The counts scale with run duration and show
no unmatched restartable EINTR. Together with the 100/100 host stress result and the passing T6
device stress, this rules out the signal-restart path as the cause of the earlier Unity variability.
Do not change restart semantics without new contradictory evidence.

The old saved `unity.txt` is an ANR-era capture with no Inflate Error, and the new reports contain
no archive/decompression failure. Do not add another logging layer until the failure reproduces.
**NEXT/device gate:** start a new run, enter the first level, and play for several minutes. Verify
scene loading, rendering, touch and audio; preserve the report only if it exits, hangs, corrupts
assets, or otherwise fails. A clean level run is the Thomas Was Alone bring-up acceptance point.

### Input ANR reproduced in two Unity games; queue lifecycle diagnostic ready

The next Thomas Was Alone run stays healthy indefinitely when untouched, reaches the menu and
plays music. Its first tap at `14:09:13.980` is followed by Android's Signal Catcher at
`14:09:18.991`, a tombstoned stack dump and loss of top-resumed state: this is the platform's
five-second input-dispatch ANR, not a guest crash or orderly exit. Lane Racer behaves identically,
so the failure is in the shared NativeActivity input bridge rather than either game's code. The
earlier Google Play Games exception is swallowed around 21 seconds before the tap and is not the
five-second trigger.

No input semantics have been changed without root-cause evidence. `HostInput` now publishes one
bounded `jni-input-queue` report line with successful `get`, framework `pre-dispatched`, `finish`,
`release`, and current `outstanding` event counts. The host test observed RED before this line
existed and now checks both an outstanding fetched event and its finished state. Host 54/54,
Android arm64 `zbridge`, launcher bundle and signed release build pass; the APK's packaged
`libzbridge.so` hash matches the Android build and APK Signature Scheme v2 verifies with one signer.

Ready launcher: `android/launcher/app/build/outputs/apk/release/app-release.apk`, 6,718,276 bytes,
SHA-256 `18920f27100a8989640a118f023d5314344474e627468973f61ce3aec0abc2da`.

**NEXT/device gate:** install this release, force-stop the launcher, launch either Thomas Was Alone
or Lane Racer, tap once, wait for the five-second return, and copy Last run report. The decisive
line is `jni-input-queue`: `get=0` localizes the bug before guest dequeue; `get>finish` proves the
guest fetched an event but did not return it; balanced `get/finish` moves the investigation below
the bridge/framework finish boundary. Do not change event ownership until this line is observed.

### Root cause fixed: NativeActivity entered before proxy JNI_OnLoad

The Lane Racer device report had no `jni-input-queue` line: the guest never dequeued an event. Its
log supplies the earlier cause verbatim: `ANativeActivity_onCreate before JNI_OnLoad: no bridge to
call`. Android's NativeActivity loader is allowed to invoke the exported activity entry directly;
the proxy incorrectly assumed ART had already called `JNI_OnLoad`, returned without installing any
callbacks, and left the first framework MotionEvent unfinished until the five-second ANR. This
also explains the identical Thomas Was Alone failure.

`libzbproxy.so` now supports both legal orders. In the observed activity-first order it reads the
VM and current-thread JNIEnv from the stable prefix of the host `ANativeActivity`, identifies its
own proxy path with `dladdr`, runs the existing `ZBridge.onProxyLoaded` path first, and then calls
`ZBridge.onNativeActivityCreated` to install callbacks. The previous JNI_OnLoad-first path is
unchanged. The proxy remains standalone: its NEEDED set is still only liblog/libdl/libc and its
only exports remain `JNI_OnLoad` and `ANativeActivity_onCreate`.

The fake-JNI test observed RED when `ANativeActivity_onCreate` was called before `JNI_OnLoad` and
now verifies guest load followed by activity creation, while retaining all old order/error cases.
Fresh verification: host 54/54, Android `zbridge`/`zbproxy`, proxy structure check, launcher bundle
and signed release build pass. Both packaged native-library hashes match the Android build; APK
Signature Scheme v2 verifies with one signer.

Ready launcher: `android/launcher/app/build/outputs/apk/release/app-release.apk`, 6,718,276 bytes,
SHA-256 `3db0911d8020fff60fa216019077a4ddd7e85cac49401c839896044e51b3b9db`.

**NEXT/device gate:** install, force-stop, launch Thomas Was Alone or Lane Racer and tap. The
five-second ANR should be gone and `jni-input-queue` should show balanced `get` and `finish` after
the touch. If the activity instead fails during creation, preserve logcat plus Last run report;
the likely boundary would then be class lookup during the activity-first bootstrap.

### Activity-first class-loader follow-up ready

The next Lane Racer run confirmed that boundary: the activity-first path executed, but direct
`FindClass("com/zettabridge/core/ZBridge")` used the framework/system loader and failed before
callbacks were installed. The report still had no `jni-input-queue`, and logcat recorded both the
class lookup failure and the proxy-load refusal.

The activity-first path now obtains the plugin `ClassLoader` from the host Activity and calls its
`loadClass("com.zettabridge.core.ZBridge")`; `PluginClassLoader` already delegates that namespace
to the launcher. Both the proxy-load and activity-created calls reuse this resolved class. The
JNI_OnLoad-first path retains direct `FindClass`. The fake-JNI test deliberately makes direct
ZBridge lookup fail in activity-first order, observed RED, and now passes through the activity
loader. Host 54/54, Android proxy structure, launcher bundle and signed release build pass.

Ready launcher: `android/launcher/app/build/outputs/apk/release/app-release.apk`, 6,718,276 bytes,
SHA-256 `90b288fec59ef291ae5ac9b85e580fe716ec99c7560b8ba23bf30a9d480e4e23`.

**NEXT/device gate:** install, force-stop and tap in either Unity game. Success means no five-second
ANR and a `jni-input-queue` line with balanced `get`/`finish`. Preserve logcat and Last run report
if activity creation still fails or the counters do not balance.

### 2026-10-01: Unity is playable through NativeActivity

Lane Racer and Thomas Was Alone run and take touch input on the OnePlus 13. The chain that got
there, each step found from one report or dump line:

- The proxy exports `ANativeActivity_onCreate` and tolerates being entered before `JNI_OnLoad`
  (Codex), and a stale per-plugin copy of it is replaced.
- The guest activity is allocated through a borrowed carrier, not `call_on_current`: this path
  arrives on the Java main thread, which runs no guest code ("cannot allocate the guest activity").
- The JNIEnv for guest lifecycle calls comes from the calling thread, attaching it if needed.
  `HostJni::current_env()` is a transition's env and there is no transition here.
- The activity object crosses as a **global** handle. A local one belongs to the frame that made
  it, and ART ends the process on the first use from the guest's own thread
  ("GetObjectClass: invalid JNI reference").
- `AInputQueue_attachLooper` honours the looper the guest named, carries the guest's data word
  through, and the poll writes the fd, the events and that word back into the guest's pointers.
- A borrower's poll asks the attached queues whether they hold events and answers with the
  queue's ident. **This was the one that made input work.** Unity's NativeActivity path runs on
  the Java main thread, whose real looper is the application's own: an input queue attached there
  is pumped by Java's loop, which knows nothing of the ident the guest waits for. Before this,
  every poll returned a bare wake and the framework waited out its five seconds.

Report lines that localize this area: `jni-input-attach` (ident, looper, owner and caller tids),
`jni-input-queue` (get/finish), `looper-polls` (ident versus wake).

**Open:** both games abort after a while (`guest exited with status 134`). Thomas Was Alone is the
reproducible one: whichever control is pressed first keeps working, and the other one ends the
process - the shape of something initialized once, for one path only. The abort message in the
native dump is the next piece of evidence; the report does not carry it.
