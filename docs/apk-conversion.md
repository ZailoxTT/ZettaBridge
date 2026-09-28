# APK conversion (Step 05)

`tools/apk_convert.py` transforms a signed ARM32 APK or complete split set into
new ARM64-installable APKs. It never installs, removes, or modifies an input APK.
It accepts only the bounded Step 02 preflight structure, one package/version/signing
identity, and ARM32 native libraries. A CLI input may also contain matching x86
copies of the ARM32 libraries; these unused copies are removed and recorded.
ARM64 libraries and x86-only library names are rejected. NativeActivity and
shared-UID inputs are rejected by preflight. A converted APK has a new signing
identity.

## Build inputs

Use the pinned WSL/NDK setup in `docs/development.md` and build the Step 04
runtime bundle (`tools/build_guest.sh`, Android `zbridge` and `zbproxy` targets,
then `tools/make_launcher_bundle.sh`). Build the bootstrap DEX carrier:

```sh
cd android/launcher
ANDROID_HOME="$ANDROID_SDK_ROOT" ./gradlew :step05bootstrap:assembleDebug
```

For a reproducible synthetic ARM32 source, run `tools/make_step05_fixture.sh`
from the repository root after building the guest libraries, then build
`:step05fixture:assembleDebug`. The source fixture contains only Java code and
`lib/armeabi-v7a/libzbstep04a.so`; it has no bridge or bootstrap code.

Create the personal key once, outside the repository, or import an existing
PKCS12 key under the same layout. `init-key` creates a 3072-bit RSA key at
`~/.local/share/zettabridge/signing/signing.p12` and a random password in
`password`. The directory is owner-only and both files are mode 0600. The key
and password are never printed or written to the output report. Back them up
securely; losing them prevents same-signer updates to converted packages.

```sh
python3 tools/apk_convert.py init-key
python3 tools/apk_convert.py convert /path/to/source.apk \
  --output-dir /path/to/new-output \
  --apksigner "$ANDROID_SDK_ROOT/build-tools/35.0.0/apksigner" \
  --zipalign "$ANDROID_SDK_ROOT/build-tools/35.0.0/zipalign"
```

Pass an explicit complete APK split set in one invocation, or a single `.apks`
container. For an imported key, place `signing.p12` with alias `zettabridge` and
its password in a mode-0600 `password` file, then pass `--key-dir`. The output
directory must not exist. It contains `base.apk`, zero or more `split-N.apk`,
and `transformation.json`. The report records source/output hashes, signing
certificate fingerprints, per-library ABI/path/hash mapping, runtime hashes,
and every manifest edit. Resources and original DEX bytes are retained and
checked. Package name and version are unchanged.

For `com.ea.games.nfs13_row`, conversion also applies the narrowly scoped
`nfs13-garage-low-pointer-callback-guard-v1` patch when selected `libapp.so`
matches the pinned NFS Most Wanted 1.3.128 ARM32 library hash. It skips the
observed callback for a null or below-64-KiB object pointer and preserves the
original path for other pointers. A changed NFS library hash is rejected for
review rather than silently converted without the guard. The report records
the source and patched guest hashes and patch offsets under `guest_patches`.
This game-specific workaround does not establish why the invalid callback
object was produced.

The converter copies the inputs into a private staging directory, runs preflight,
edits compiled binary AXML, moves selected ARM32 ELFs into `assets/zb/app/lib`,
adds the ARM64 bridge/proxies and bootstrap DEX, aligns, signs, then verifies
every APK and publishes the directory atomically. Unselected ARM32 ABI variants
are recorded and removed. Unsigned intermediates are deleted before publication
and on failure. Duplicate ZIP names, ambiguous same-ABI library names across
splits, collisions with bridge names, unsupported native ABIs, and
generated asset/name collisions fail closed. Host `.so` files are compressed and
the manifest requests native extraction, so Android extracts real ARM64 files
for `dladdr`; guest `.so` files are only app-private assets.

The injected `ContentProvider` explicitly sets `android:exported=false`. It
extracts the guest bundle under the
installed package UID and activates the Step 04 bridge before Activity, Service,
and Receiver callbacks in the default app process. Android creates an
`Application` before providers, so native loads in `Application.attachBaseContext`
are not covered yet. Secondary and isolated processes also need their own
bootstrap path. Component lifecycle coverage belongs to Step 09. NativeActivity
belongs to Step 10.
This synthetic proof does not claim arbitrary user-app compatibility.

## In-app conversion

The `:manager` app now has a **Choose ARM32 APK, convert and install** action.
It performs conversion as the manager's ordinary app UID, generates the
`transformation.json` report for its own review/install checks, then opens the
existing explicit Install/Update confirmation. The user does not need to run
`tools/apk_convert.py` or select a report. The original source APK is not
modified. The manager build succeeded and was installed and launched on a Pixel
11 Pro XL running Android 17/API 37. The bootstrap DEX verifier was corrected
to use the same default-provider class name as the manifest injector. An
invalidated Android Keystore RSA key is replaced only when no signer anchor or
install records exist. The output minimum SDK is raised to 26, matching the
signing floor needed by the configured signature schemes; the target SDK is at
least that minimum (GP Retro's output is 26, raised from 13). This addresses
signature compatibility and old-target install policy, but does not repair
unrelated manifest parse errors or signer and key failures. Raising the target
SDK further would opt into more platform behavior changes without changing the
APK signer or its signing keys.

The Manager's Java converter now follows the CLI's mixed-ABI rule: it selects
the best ARM32 copy of each library (`armeabi-v7a` before `armeabi`), validates
and removes same-named x86 copies, and rejects x86-only or ARM64 source
libraries. Its report lists every removed ABI variant. The refreshed Manager
bundle includes the CP15 virtual-timer bridge used for the Google Play NFS
Most Wanted run. `:manager:testDebugUnitTest` and `:manager:assembleDebug`
passed; the built APK's embedded bridge SHA-256 matched the Android build.
The pre-version-bump Manager APK with the same converter/runtime update
installed and its activity launched on the Pixel. The release build is version
0.2.0; its APK signature and embedded runtime were verified after rebuilding,
but the Pixel was disconnected before that final package could be installed.
An end-to-end in-app conversion of this particular Play APK was not run during
this update. Updating the existing CLI conversion through Manager requires
importing the same personal signing key to preserve its installed data.

The first converted GP Retro APK was rejected by PackageManager. On-device
probes isolated the cause to its second injected bootstrap provider: Android
accepted a second provider with no process attribute or
`android:process=":swarm"`, but returned null for the same provider with
PackageManager's expanded value `com.smallthinggame.gpretro:swarm`. The
converter now writes package-local processes in manifest shorthand. After that
fix, the manager built and updated on the Pixel 11 Pro XL without clearing
manager data. GP Retro 2.16 completed in-app conversion, signing, and
PackageManager identity/signer validation, and reached the explicit install
review dialog for package `com.smallthinggame.gpretro`, version 16, with one
ARM32 library translated. The dialog was left at its Install/Cancel choice.
Installing and launching the converted app, and validating its runtime
behavior, remain unverified. The preflight warning about native loads in a
custom `Application` initializer or `attachBaseContext` also remains applicable.

Before building the manager, build the guest and ARM64 runtime bundle as in
[development workflow](development.md#android-runtime-link-and-launcher-apk):
run `tools/build_guest.sh`, build the Android `zbridge` and `zbproxy` targets,
and run `tools/make_launcher_bundle.sh`. Then build with:

```sh
cd android/launcher
ANDROID_HOME="$ANDROID_SDK_ROOT" ./gradlew :manager:assembleDebug --no-daemon
```

The manager build task assembles `:step05bootstrap:assembleDebug` and packages
the runtime bundle, ARM64 bridge/proxy libraries, and bootstrap APK into the
manager APK assets. It fails if the runtime bundle is missing. The
installed manager therefore does not need host-side converter tools or a
runtime download.

The in-app prototype accepts a single signed base APK with ARM32 native `.so`
files, DEX code, and a default process layout it can bootstrap. It rejects split
APKs, APKs without ARM32 native libraries or DEX, ARM64/x86 native libraries,
shared UIDs, custom default processes, direct-boot components, NativeActivity,
pure-NDK apps, isolated/external services, multiprocess providers, and more than
eight app processes. It also fails closed on malformed/duplicate ZIP entries,
reserved runtime paths, malformed ARM32 ELF files, oversized inputs, or an
unavailable bundled bridge/bootstrap. These are admission limits, not a claim
that every APK which passes them will run. Certificate-bound apps may fail
because conversion replaces the original signer.

For updates to apps previously converted with the CLI tool, import the same
PKCS#12 personal key into the manager before converting. Otherwise the manager
creates an Android Keystore signing key for its own future outputs. That key is
not exportable; removing manager data or losing the device key can prevent
same-signer updates. The signing key stays on the device and is never included
in the generated report.

## Step 05 validation on 2026-09-24

The WSL2 Ubuntu 24.04 build used Android SDK platform/build-tools 35, NDK r29
runtime artifacts from Step 04, and `:step05bootstrap:assembleDebug` plus
`:step05fixture:assembleDebug` (both passed). Three converter admission tests and
six Step 02 preflight tests passed. The single synthetic output passed `zipalign
-c`, `apksigner verify`, `aapt2 dump badging`, and post-sign preflight. `aapt2`
reported `native-code: 'arm64-v8a'`. Its original DEX/resources matched byte for
byte; only `lib/arm64-v8a/libzbridge.so` and ARM64 `libzbstep04a.so` appeared
under `lib/`, while ARM32 `libzbstep04a.so` appeared under guest assets.

On Pixel 11 Pro XL `67161FDDV0011Q`, Android 17/API 37 build
`CD1A.260905.001.B1`, ARM64-only ABI, `adb install` accepted the output. The
installed package had `primaryCpuAbi=arm64-v8a`, `extractNativeLibs=true`, a
registered private bootstrap provider, and Android-assigned UID 10387. Launch
persisted `PASS uid=10387 native=12 callbacks=1` in its app-private result
file, proving guest JNI execution and callback in the normal app process.

A synthetic two-APK split set placed the ARM32 library in the feature split.
Both converted APKs passed structure/signature checks. `adb install-multiple
-r` accepted the set; `pm path` listed `base.apk` and `split_feature.apk`, and
the fixture again persisted `PASS` under UID 10387. The source set used one
debug signer; both outputs used the same personal signer. This is split
packaging proof for a synthetic fixture, not dynamic-feature behavior proof.

A deliberately missing proxy failed conversion before publication: no output
directory remained, the source SHA-256 remained
`50edb5f2f9862d8889d00053d995dee27d7dbbdd469000489d72ab73db38565a`,
and the already installed synthetic app still passed after force-stop/relaunch.
The first split install attempt used a malformed synthetic feature fixture with
no DEX and default `hasCode=true`; Android rejected the session without
replacing the installed app. The fixture was corrected to `hasCode=false` and
the complete split set installed successfully.

The final single output SHA-256 was
`e62549f3e419b05eb259dbb949ce24bc31ee646768fc94b4fcd5d13b873c291d`.
The personal output certificate SHA-256 was
`d3fe5a914ad2f4139c645ae3a09ba845d484b2f946a50826326beb41d5575c00`;
the synthetic source debug certificate SHA-256 was
`01c34a1d710341d38134574f3dc5b0c309a19f7781863d11e1d051c7178c4a9f`.
Generated APKs and key material are local artifacts, not committed or published.
