# NFS Most Wanted compatibility work

This is a focused implementation handoff for the user's copy of **Need for
Speed Most Wanted** on the Pixel 11 Pro XL. It is separate from `plan.md`.
Use it to drive a playable local-game result, while implementing reusable
ZettaBridge capabilities rather than package-name-specific exceptions.

**Status (2026-09-26 Play extraction):** Gates 0 and 1 passed for the APK and
matching OBB pulled from the user's Google Play installation. The Pixel
recognized the OBB, loaded all five guest libraries, passed `libapp.so`
`JNI_OnLoad`, rendered the loading screen, and reached EA's first-run agreement.
The user completed the first local race in first place with correct scene and
HUD rendering; touch events and FMOD processing are recorded. The user confirmed
sound and responsive controls. Browsing across locked cars later caused one
guest `SIGSEGV` in `libapp.so` (write to `0x30`); Gate 4 is therefore not fully
accepted. The diagnostic run and normal-speed retest did not reproduce it.
Android Home/resume and a subsequent cold process launch succeeded with the
new save intact. The earlier
EasyAPK build
exited in its own protector before
ZettaBridge ran; its evidence remains below as a separate historical attempt.
See [device evidence](docs/nfs-device-evidence.md) for the gate results and
local artifact inventory.

**Latest status (2026-09-28):** The reproduced garage crash was a guest
callback storing through a null object at `libapp.so+0x473a50`. A narrow guard
was added for the exact affected `libapp.so` build; it skips the callback when
its payload object is null or below address `0x10000`, and leaves normal object
pointers on the original path. The guard was hot-applied to the installed
game's extracted library with a verified rollback copy. During a roughly
three-minute user-led garage session with repeated car browsing, the process
stayed alive and the user could not reproduce the crash. The converter now
applies the guard only to the matching library hash, and a converted,
verified APK was produced with the selected default Android debug key. Its
certificate differed from the previous install, so after a verified backup
the old package was removed and the rebuilt APK installed. App-private and
device-protected data, app-specific external files, and the OBB were restored
under the new UID; the OBB hash matched. Android Keystore state was not backed
up, so keystore-backed game state may not have survived. The new process loaded
all five guest libraries without proxy or unimplemented-host-call errors and
stayed alive for about five minutes while the logs showed car-model loading.
The upstream cause of the invalid callback remains unknown, and longer-play
stability remains unverified. Gate 4 remains open.

## Goal and scope

Convert a legally obtained, matching APK and expansion file into an installed
ARM64 package on the Pixel 11 Pro XL (Android 17/API 37). The app must run as
its own Android-assigned UID, load its ARM32 game and middleware libraries
through ZettaBridge, recognize its expansion data, render a race, accept touch
input, play audio, and survive pause/resume and a fresh launch. Preserve the
source manifest, DEX, resources, and app behavior except recorded conversion
edits. Keep root confined to explicitly selected management operations.

The game's Play licensing, online services, and in-app purchases are external
dependencies. Test their real behavior with the user's purchased account;
do not treat a re-signed APK as the original signer or bypass a license or
integrity decision. If signing identity prevents authorized play, report that
boundary. Core gameplay is the initial target; the bundled live wallpaper,
optional purchases, and online social features are separate follow-ups.

## Active inputs and verified baseline (2026-09-26)

The active pair came from the user's previously played Google Play installation
on a Xiaomi 22071212AG (Android API 35). Only its installed base APK and
matching OBB were pulled; no save or app-private data was copied. The Pixel's
earlier converted test package was uninstalled before installing this pair, so
the Pixel game began with fresh app data. The old phone's progress was untouched.

| Item | Observed value |
| --- | --- |
| Pixel | Pixel 11 Pro XL `67161FDDV0011Q`, Android 17/API 37, build `CD1A.260905.001.B1` |
| Original phone | Xiaomi `AIBAJFNZCMV8LFLN`, Android API 35, installer `com.android.vending`; no APK splits |
| APK | `play-phone/NFS-Play-base.apk`, 19,445,609 bytes; package `com.ea.games.nfs13_row`, version `1003128`/`1.3.128`, min SDK 16, target SDK 28 |
| APK SHA-256 | `de219abfec8d98aec29fb7f0cc3c22de360a87ce543487a940b0db998912df5f` |
| Source signer | `5bff7d614e1ba11a566abb589c863b013f79aa747bd2146733366c5625a5f0d2` (EAM DSA) |
| OBB | `main.1003128.com.ea.games.nfs13_row.obb`, 623,470,192 bytes |
| OBB SHA-256 | `66dd4e695e698929f789e7c825eabe3ba5a50ed2ce28b628c96e5dbc008043a1` |
| Pair check | `unzip -t` passed; shared `gothamblack.ttf` hash matched in APK and OBB |
| Native code | Five `armeabi-v7a` libraries and five matching x86 variants; no ARM64 libraries |
| Preflight | `analyzed`, zero missing guest libraries and zero unresolved strong symbols with sysroot and guest library roots |
| Converted APK | `converted-timer/base.apk`, SHA-256 `8d230e277aa206e14e41b9eebc320714d259700b774dc105970bab0509256301`, signed with personal certificate `d3fe5a914ad2f4139c645ae3a09ba845d484b2f946a50826326beb41d5575c00` |

The converter now accepts matching x86 variants while selecting the ARM32
libraries as guests and removing the unused x86 copies. It still rejects x86
library names with no ARM32 counterpart. The Play APK uses
`android.support.multidex.MultiDexApplication`, so the EasyAPK protector path
described below does not apply. See the evidence file for the device run.

## Historical EasyAPK input and first device attempt

| Item | Observed value |
| --- | --- |
| Device | Pixel 11 Pro XL `67161FDDV0011Q`, Android API 37 |
| APK | `/sdcard/Download/NFS.apk`, 11,580,449 bytes |
| APK identity | `com.ea.games.nfs13_row`, version code `1003128`, version name `1.3.128`, min SDK 21, target SDK 28 |
| APK SHA-256 | `a1ec79d5427321870d20c844ba61356cf7ca684ec5ef40e3a564be0c55327c5e` |
| APK signer | v1/v2 verified, certificate SHA-256 `1ccecacceb9068e7ac83c88f088069a8019cb25a07bb4b6a5f69afdceff53eb1` |
| OBB | `/sdcard/Download/main.1003128.com.ea.games.nfs13_row.obb`, 683,748,520 bytes |
| OBB SHA-256 | `258ca6b6983920e026cdba7bc26ee35c0bdf831b6e1d44a599699534de194a9a` |
| OBB archive check | Device `unzip -t` exited 0; its ZIP contains `published/...` game assets |
| APK expansion hint | `assets/obb.size` contains `0`; it is not a usable byte-size check |
| Cross-file asset | `published/fonts/gothamblack.ttf` has SHA-256 `a4ae98b474a05b9a047bf3c1643964ab166e99c08525f4cafda6b3243e3a6b89` in both APK and OBB |
| Repository at assessment | `main` at `7e4bcdd`; `third_party/dynarmic` was already modified and must be preserved |

**Historical pairing result:** The APK package and version code match the OBB filename,
the OBB passes `unzip -t`, and one shared game asset matches byte-for-byte.
These are sufficient static checks to begin implementation. They do not prove
the game will locate the OBB, accept a re-signed APK, or play on Android 17.
The APK's signer differs from the earlier `1003103` APK's certificate
(`a40da80a59d170caa950cf15c18c454d47a39b26989d8b640ecd745ba71bf5dc`).
The user obtained the updated APK from EasyAPK on Telegram, not directly from
Google Play. The local files do not establish which certificate Google Play
uses for this version. Keep signing, Play-license behavior and APK provenance
as explicit runtime/evidence gates. A failure of this third-party build is
not automatically a failure of the official game APK.

DEX inspection found an obfuscated custom Application wrapper:
`com.ea.games.nfs13.GameActivity.attachBaseContext` calls
`a/a/a/e/a.<init>` and `a/a/a/d/a.k` before providers can run. Its
`onCreate` reflectively creates another Application. Static inspection has
not established whether these paths load native code or another DEX, or
check the source signer. This is a concrete early-startup compatibility risk.

The earlier `NFS.apk` was version `1003103`, SHA-256
`e98f30675b1b40bd35a13f09cb3b2daf8281300fb940b5f21e1a255efe61a828`.
It had mixed ARM32/x86 libraries and 31 unresolved guest GL imports, but it
is no longer the selected input. Do not carry those blockers into work on
version `1003128`.

The EasyAPK APK has one base package and five `armeabi-v7a` libraries:
`libapp.so`, `libNimble.so`, `libc++_shared.so`, `libfmodevent.so` and
`libfmodex.so`. It has no x86 libraries. It uses a Java `GLSurfaceView`,
ARM32 JNI entry points, GLES 3 declarations, FMOD and Java `AudioTrack`.
It is not a NativeActivity or Vulkan guest. The embedded
`res/raw/wallpaper.apk` is an optional secondary APK, not the game's OBB.
At the time of the EasyAPK assessment, there was no installed package or OBB
under this package's `/sdcard/Android/obb` directory on the Pixel.

Read-only preflight with the current sysroot and `build/guest/lib` reports
zero missing guest libraries and zero unresolved strong symbols for the
selected ARM32 set. `libapp.so` still has 145 direct `gl*` imports and one
`egl*` import; linkage alone does not validate graphics behavior. Preflight
warns that custom `Application` class `com.ea.games.nfs13.GameActivity`
might load native code before the injected bootstrap provider runs. Review
its `attachBaseContext` callees and reflective `onCreate` path. Preflight
status `analyzed` only means the input was parsed safely; it does not mean
the app will run.

## Implementation order and acceptance gates

### 0. Preserve and confirm the selected APK/OBB pair

1. Recheck the active pair's hashes and package/version before using either
   file. Record its Google Play installer provenance and keep user-owned
   APK/OBB files out of Git and releases.
2. Inspect the game's expansion lookup and validation behavior. Preserve the
   OBB filename and contents; confirm recognition during the device run.
3. Store original inputs separately from any converted APK and retain their
   hashes. Check for an original-signed installed package and its data before
   any install or replacement.

**Gate:** Static package/version, archive and shared-asset checks passed for
the Play pair. The Pixel also reported `EXPANSION FILE DELIVERED!` for the
placed, hash-matched OBB.

### 1. Convert the selected ARM32 input and review early startup

The active Play APK contains matching `armeabi-v7a` and x86 libraries. The
converter selects the ARM32 set and removes x86 copies. Then run the existing
converter and output verifier. Fix only
observed generic conversion/startup failures. Preserve source DEX, resources,
manifest, package identity and optional wallpaper asset.

**Gate:** The selected real source produces a signed, verified
ARM64-installable APK with all five ARM32 libraries in guest assets, matching
ARM64 proxies, and no ARM32 host-loadable libraries.
Record source/output hashes and the converter transformation report. This is
conversion evidence, not game startup evidence.

### 2. Verify guest startup and the game's graphics path

Repeat `tools/apk_preflight.py` with both `--sysroot sysroot` and
`--guest-lib-dir build/guest/lib` against any changed input or runtime.
The current selected APK has no static missing-symbol gap. Confirm that the
installed proxy actually loads `libapp.so`, reaches `JNI_OnLoad`, and executes
the game's JNI startup without a loader or relocation failure. Then audit the
GLES/EGL calls the game really executes: extension availability, pointer
sizes, buffers, errors, context/thread ownership, surface recreation and
texture paths. Do not advertise unsupported extensions or return fake success.

**Gate:** Preflight continues to report zero unresolved strong guest symbols.
The Pixel runtime report confirms guest `libapp.so` and `JNI_OnLoad` succeed
without missing-symbol or relocation errors. Synthetic tests cover any new
marshaling/extension behavior. The first screen and an actual race render
correctly through pause/resume.
This contributes to Step 12, but one game does not complete that step's broad
EGL/GLES acceptance criteria.

### 3. Install, expansion data, licensing and startup

Use the verified converted APK and only the selected, validated OBB. Place
the OBB under the installed package's normal `Android/obb` location using
its proven expected filename. Do not change the package name, signer policy,
target SDK, storage permissions or OBB bytes speculatively. Test Android 17
install policy and any storage failure as separate observations. Record the
assigned UID, `primaryCpuAbi`, signing fingerprint, package version and
whether the original app/data existed before installation.

Collect the per-process ZettaBridge report before and after startup, plus
bounded logcat and screenshots. Identify the first failure in the order it
occurs: provider/bootstrap, proxy/ELF load, JNI, game data, Play license,
graphics, or later Java/native behavior. Test the purchased account's real
license result and whether the re-signed package can access game data. If a
service or original certificate is required, report that constraint rather
than masking it.

**Gate:** A cold launch reaches the menu with the expansion data recognized;
no JNI/proxy failures or unexplained missing-symbol errors remain. A passing
package install or splash screen alone is insufficient.

### 4. Validate playable local behavior and fix observed gaps

Run a local race and check onscreen geometry/textures, touch steering and
menus, tilt steering if selected, FMOD/`AudioTrack` sound, orientation/focus,
pause/resume, surface recreation and force-stop/relaunch. Inspect runtime
reports for actual used-but-unsupported calls, guest faults and syscalls.
Implement reusable audio/input/platform or ARM32 historical behavior only
when a failure is reproduced; add a focused synthetic regression for each
bridge fix. Record startup time, frame rate and memory only after correctness
is established. Preserve the guest's ordinary app UID and SELinux policy.

**Gate:** At least one race can be started and completed with visible graphics,
working controls and audio; pause/resume and a fresh process launch remain
usable. Record any optional online, purchase, controller or wallpaper limits
separately. If the game cannot be made playable because of signing, license,
server or incompatible OBB data, record the precise observed boundary and
the last successful gate instead of declaring compatibility.

## Evidence and roadmap bookkeeping

For each implementation slice, record the source and OBB hashes, conversion
report, output APK hash/signature, source revision, device build/API, package
UID, runtime report, relevant screenshots or short capture, tests run, and
remaining failures. Distinguish static analysis, host/QEMU, Android build,
synthetic device, and this real-game evidence. Keep artifacts local; never
commit the commercial APK, OBB, personal signing key, game data or account
tokens. Preserve the existing modified `third_party/dynarmic` checkout.

Update `plan.md` only when a broader step's own acceptance criteria are met;
this game passing is an integration check, not proof of generic compatibility.
Likely roadmap contributions are Step 05/06 (only if conversion needs a generic
fix), Step 12 (GLES), Step 14 (audio/input if needed), Step 15 (only for observed ISA/syscall
gaps), and Step 17 (diagnostics/evidence). Step 10 NativeActivity and Step 13
Vulkan are not currently required by this APK. Step 18 remains a separate
release gate.

## Starting commands for the next implementation session

Use the instructions above as the task scope; recheck the current checkout,
toolchain, device, input hashes and available OBB/APK pair first. For read-only
input checks, the relevant commands are:

```sh
sha256sum /path/to/NFS-Play-base.apk
sha256sum /path/to/main.1003128.com.ea.games.nfs13_row.obb
python3 tools/apk_preflight.py \
  --apksigner "$ANDROID_SDK_ROOT/build-tools/35.0.0/apksigner" \
  --sysroot sysroot --guest-lib-dir build/guest/lib \
  /path/to/selected.apk > /path/outside-git/preflight.json
```

The active Play pair is stored outside Git under
`C:\Users\Netanel\Documents\Codex\nfs-compat-20260926\play-phone`. If either
hash changes, rerun preflight and reassess the findings before relying on this
handoff.
