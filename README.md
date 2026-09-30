# ZettaBridge

**Run 32-bit Android apps on 64-bit-only ARM phones.**
An open-source ARM32 -> ARM64 native code translator and app launcher.

[Читать на русском](README.ru.md)

---

A growing number of recent phone SoCs (for example the Snapdragon 8 Elite) have no AArch32
execution state at all. Old apps that ship only `armeabi` / `armeabi-v7a` native libraries
cannot start on them, and there is no hardware fallback.

ZettaBridge runs such an app **inside its own process**:
- the app's Java/Kotlin code runs natively on the phone's normal 64-bit ART;
- only the app's 32-bit native code is translated to AArch64.

No root, no custom ROM, no system image changes.

> **Status: early development.** Not usable by end users yet. See [Roadmap](#roadmap).

## Orange Roulette, a 2014 armeabi game, on a phone with no 32-bit CPU

| Intro | Menu | Gameplay |
|---|---|---|
| ![Intro screen](images/screenshots/intro.jpg) | ![Main menu](images/screenshots/menu.jpg) | ![Gameplay](images/screenshots/gameplay.jpg) |

Every pixel above was drawn by translated 32-bit ARM code: a OnePlus 13 (Snapdragon 8 Elite) has
no AArch32 execution state, so the game cannot start on it at all without ZettaBridge.

## How it works

```
ZettaBridge launcher (arm64 app)
 |
 +-- imports an APK as a plugin, pins a home-screen shortcut (nothing is installed)
 +-- :guest process
      +-- plugin Java code on the real 64-bit ART
      +-- libzbridge.so (arm64)
           +-- Dynarmic: A32/T32 -> AArch64 JIT, 4 GiB guest address space
           +-- syscall layer: 32-bit Linux ABI -> 64-bit kernel
           +-- guest threads, signals, "carrier" threads for Java callers
           +-- JNI bridge: a synthesized 32-bit JNIEnv / JavaVM
      +-- real arm32 Android system libraries (linker, libc, libm, libc++, ...)
```

- **Native code is translated by [Dynarmic](https://github.com/Vita3K/dynarmic)**
  (0BSD), an ARM dynamic recompiler, with a small local patch for ARMv8 Thumb-2 opcodes.
- **The guest runs the real arm32 bionic.** The Android linker, libc and friends come from
  an AOSP system image. ZettaBridge translates Linux syscalls, not libc functions.
- **Java <-> native goes through a JNI bridge.**
  - `System.loadLibrary` on a 32-bit library loads a tiny arm64 proxy instead.
  - The proxy binds the guest's `Java_*` exports and runs its `JNI_OnLoad`.
  - Guest native code talks to Java through a synthesized 32-bit `JNIEnv`.
- **Old native libraries are fixed up at import.** Text relocations and absolute
  `DT_NEEDED` paths are rewritten, because modern Android linkers refuse them.

## What works today

- **Translator core.** On aarch64 Linux via `zbrun`, and on a real phone both in Termux and
  inside an app process next to ART:
  - static and dynamic arm32 Android executables through the real arm32 linker;
  - threads, signals, C++ exceptions, `dlopen`, kernel user helpers.
- **Launcher.** Imports APKs as plugins with home-screen shortcuts. 64-bit apps already run
  as plugins, including apps with Firebase/AdMob, Jetpack Compose and Flutter.
- **JNI bridge.** Implemented and tested against a mock JVM on the host:
  - guest `JNIEnv`/`JavaVM`;
  - Java -> guest native calls from multiple Java threads;
  - `RegisterNatives`;
  - library loading and `Java_*` binding.
- **GLES 2.0 and 3.0 passthrough, EGL and ANativeWindow.** Guests create their own GL context
  on their own thread; ~490 entry points are generated from the Khronos registry.
- **Games running on the phone.**
  - *Orange Roulette* (2014, Haxe/OpenFL): menus and gameplay, with sound.
  - *Flappy Bird* (AndEngine): playable.
  - A modern Flutter app starts, runs its Dart code, presents frames and takes touch input,
    though rendering is not yet complete.

Not supported yet: `NativeActivity` (so Unity and pure-NDK guests do not start), Vulkan, and
anything that needs a real package installation.

## Roadmap

| Milestone | What it brings |
|---|---|
| **v0.1** | 32-bit apps whose native libraries do not draw on their own: utilities and apps using old native libs for crypto, image processing, parsers, databases. |
| **v0.2** | OpenGL ES passthrough and Android assets: simple 2D games, starting with Orange Roulette. |
| **v0.3+** | 3D games and performance work: GL call batching, faster floating point, JNI fast paths |
| exploring | x86 / x86_64 guests through [Box64](https://github.com/ptitSeb/box64) |

## Honest limits

- **Speed.** Translated code runs slower than native: roughly 2x for integer code and
  3.5x for memory copies on a Snapdragon 8 Elite, more for floating-point-heavy loops.
- **3D games are harder, not off-limits.** Every call from the app into the system, and
  OpenGL ES calls in particular, crosses a translation boundary, so 3D-heavy games will be
  slow at first. Making them playable is a goal. Planned work:
  - batching GL calls;
  - faster floating point;
  - host-side JNI fast paths.
- **Apps that need a real installation are out of scope:** their own UID and
  permissions, visibility to other apps, accounts, push notifications.
- **Some apps refuse to run inside another app on purpose** (Play Integrity / clone
  detection). These are not worked around.

## Building (development)

Requirements:
- aarch64 Linux, clang, CMake, Ninja;
- Boost headers;
- Android NDK r29.

```
git submodule update --init; git -C third_party/dynarmic apply ../patches/dynarmic-0001-thumb32-armv8.patch ../patches/dynarmic-0002-asimd-narrowing.patch
```
```
cmake -S . -B build/host -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++; ninja -C build/host
```
```
tools/extract_sysroot.sh; tools/build_guest.sh; ctest --test-dir build/host; tools/run_guest_tests.sh
```

`tools/extract_sysroot.sh` downloads an AOSP GSI (about 1.2 GB) and extracts the arm32
system libraries into `sysroot/`. The Android build of `libzbridge.so` and the launcher are
described in `CLAUDE.md` and `docs/`.

## Supporting the project

ZettaBridge is written by one person. If it runs an app you needed, [Boosty](https://boosty.to/zailox)
keeps the work going.

## Contributing

The project is young. The most useful contributions right now are issues that name
old 32-bit apps you want to run. Include the app name and version, where the APK comes
from, and what happens.

## Thanks

- **[@kxiserino](https://github.com/kxiserino)**, whose `fix/unity-startup` branch and its
  write-up mapped the Unity startup chain on real hardware: the missing sensor ABI, the IL2CPP
  path mapping, the Boehm collector's stop-the-world handshake and the extension list an Adreno
  driver advertises. The work here is independent, but the findings shortened it considerably,
  and the branch named its limits as carefully as its results.
- Everyone who starred the repository or supported it on Boosty in its first weeks. A project
  like this one is a long stretch of black screens; knowing someone is waiting for it helps.

## License

ZettaBridge is **source-available, not open source**. Two licences apply cumulatively (see
[LICENSE](LICENSE)): **PolyForm Noncommercial 1.0.0** and **PolyForm Perimeter 1.0.1**. Read it,
change it, share it for noncommercial purposes. What both together forbid: making money from it,
and providing others a competing product - a competing product counts even when it is free.

Commercial use of any kind - selling it, shipping it inside a product, preinstalling it in a
device ROM - needs a separate licence from the author, which is available on request.

Contributions are accepted with a copyright assignment; see [CONTRIBUTING.md](CONTRIBUTING.md).

Third-party code keeps its own licence; see `third_party/README.md`. Dynarmic is 0BSD and the
Khronos registry files are Apache-2.0. AOSP system libraries used at run time are Apache-2.0 and
are not part of this repository.
