add_library(zbcore STATIC
    src/android/host_looper.cpp
    src/android/host_native_window.cpp
    src/android/host_native_activity.cpp
    src/android/host_sensors.cpp
    src/android/host_platform_compat.cpp
    src/assets/host_assets.cpp
    src/cp15.cpp
    src/elf_fixups.cpp
    src/elf_symbols.cpp
    src/elf_loader.cpp
    src/guest_memory.cpp
    src/guest_thread.cpp
    src/hang_watchdog.cpp
    src/gl/egl_manual.cpp
    src/gl/gl_diagnostics.cpp
    src/gl/gl_manual.cpp
    src/gl/host_egl.cpp
    src/gl/host_gl.cpp
    src/initial_stack.cpp
    src/jni/descriptor.cpp
    src/jni/handles.cpp
    src/jni/host_jni.cpp
    src/jni/host_jni_data.cpp
    src/jni/host_jni_natives.cpp
    src/jni/host_jni_objects.cpp
    src/jni/host_jni_values.cpp
    src/jni/host_jni_vm.cpp
    src/jni/loader.cpp
    src/jni/mangle.cpp
    src/jni/native_call.cpp
    src/jni/native_thunks.cpp
    src/jni/proxy_runtime.cpp
    src/jni/shorty.cpp
    src/jni/thunks.S
    src/library_runtime.cpp
    src/log.cpp
    src/proc_files.cpp
    src/process.cpp
    src/runtime_report.cpp
    src/signals.cpp
    src/syscalls.cpp
    src/zbridge_c_api.cpp
)
target_include_directories(zbcore PUBLIC include PRIVATE src)
target_link_libraries(zbcore PUBLIC dynarmic)
target_compile_options(zbcore PRIVATE -Wall -Wextra)

if (ANDROID)
    target_link_libraries(zbcore PUBLIC log)

    # libzbridge.so: the translator as loaded by the launcher's :guest process (Phase 3).
    # android/jni_env_backend.cpp: the JniBackend over the real JNIEnv, used by the JNI bridge (4d).
    # android/guest_jni_runtime.cpp: the process-lifetime runtime behind ZBridge.onProxyLoaded (4d).
    add_library(zbridge SHARED src/zbridge_c_api.cpp android/zbridge_jni.cpp android/jni_env_backend.cpp
        android/guest_jni_runtime.cpp android/gl_driver_backend.cpp android/asset_driver_backend.cpp
        android/egl_driver_backend.cpp android/native_window_driver_backend.cpp
        android/looper_driver_backend.cpp)
    # GLESv3 for the GLES 3.0 half of GlDriverBackend. On the device both live in the one real
    # libGLESv2.so; the NDK splits the stub libraries by version.
    target_link_libraries(zbridge PRIVATE zbcore GLESv2 GLESv3 EGL android)
    set_target_properties(zbridge PROPERTIES CXX_VISIBILITY_PRESET hidden)
    target_compile_definitions(zbridge PRIVATE ZB_EXPORT_C_API)
    # --exclude-libs keeps the statically linked libc++ and friends from being exported, so
    # they cannot clash with the app's own libc++_shared.so in the same process.
    target_link_options(zbridge PRIVATE -Wl,--gc-sections -Wl,--strip-all -Wl,--exclude-libs,ALL)

    # libzbproxy.so: the arm64 proxy ART loads for each arm32 plugin library (Phase 4d). Plain C
    # linked against nothing of ours; every link is checked by tools/check_zbproxy.py.
    add_library(zbproxy SHARED android/zbproxy.c)
    target_compile_options(zbproxy PRIVATE -Wall -Wextra -Werror)
    target_link_libraries(zbproxy PRIVATE log)
    target_link_options(zbproxy PRIVATE -Wl,--as-needed -Wl,--no-undefined -Wl,--gc-sections
        -Wl,--strip-all -Wl,--exclude-libs,ALL)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_custom_command(TARGET zbproxy POST_BUILD
        COMMAND Python3::Interpreter ${CMAKE_SOURCE_DIR}/tools/check_zbproxy.py $<TARGET_FILE:zbproxy>
        VERBATIM)

    # Link-only ART reflection seam; it is never packaged or run. Keeping the calls in one
    # exported function makes missing JniEnvBackend overrides fail the Android build.
    add_library(zbjni_reflection_compile_test SHARED
        android/jni_reflection_compile_test.cpp
        android/jni_env_backend.cpp
    )
    # zbcore supplies the zb/ headers and the portable descriptor helpers.
    target_link_libraries(zbjni_reflection_compile_test PRIVATE zbcore)
    target_compile_options(zbjni_reflection_compile_test PRIVATE -Wall -Wextra)
    target_link_options(zbjni_reflection_compile_test PRIVATE -Wl,--no-undefined)
endif()
