set(ZB_HOST_TESTS
    fault_pc_test
    egl_marshal_test
    configuration_test
    elf_fixups_test
    elf_symbols_test
    guest_memory_test
    hang_watchdog_test
    host_looper_test
    gles_client_arrays_test
    gles_marshal_test
    gles_pointer_test
    input_queue_test
    jni_abi_test
    jni_descriptor_test
    jni_handles_test
    jni_mangle_test
    jni_shorty_test
    native_activity_test
    native_thunk_test
    path_translation_test
    platform_compat_test
    proxy_runtime_test
    runtime_report_test
    sensors_test
    t1_blob_test
)

foreach(test_name ${ZB_HOST_TESTS})
    add_executable(${test_name} ${test_name}.cpp)
    target_link_libraries(${test_name} PRIVATE zbcore)
    add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()

# The committed tools/gen_jni.py outputs match jni.h and the generator.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(NAME gen_jni_check COMMAND Python3::Interpreter ${CMAKE_SOURCE_DIR}/tools/gen_jni.py --check)
add_test(NAME gen_gles_check COMMAND Python3::Interpreter ${CMAKE_SOURCE_DIR}/tools/gen_gles.py --check)
add_test(NAME gen_egl_check COMMAND Python3::Interpreter ${CMAKE_SOURCE_DIR}/tools/gen_egl.py --check)
add_test(NAME gen_ndk_check COMMAND Python3::Interpreter ${CMAKE_SOURCE_DIR}/tools/gen_ndk.py --check)

target_sources(egl_marshal_test PRIVATE mock_egl.cpp)
target_sources(gles_marshal_test PRIVATE mock_gles.cpp)
target_sources(gles_pointer_test PRIVATE mock_gles.cpp)
target_sources(gles_client_arrays_test PRIVATE mock_gles.cpp)

# Proves GuestJniEngine chains a GlBackend's HostGl alongside HostJni (Phase 5 Task 8). Each mode
# is its own process: HostJni (like the LibraryRuntime it lives in) is one per process.
add_executable(gl_chain_test gl_chain_test.cpp mock_gles.cpp)
target_link_libraries(gl_chain_test PRIVATE zbcore zbmockjvm)
foreach(gl_chain_mode no-backend with-backend)
    add_test(NAME gl_chain_test_${gl_chain_mode} COMMAND gl_chain_test ${gl_chain_mode})
endforeach()

# AAsset* host calls against the mock AssetBackend (Phase 5 Task 7), chained the same way.
add_executable(asset_chain_test asset_chain_test.cpp)
target_link_libraries(asset_chain_test PRIVATE zbcore zbmockjvm)
add_test(NAME asset_chain_test COMMAND asset_chain_test)

# ANativeWindow_* host calls against the mock NativeWindowBackend (Phase 7a Task 5).
add_executable(native_window_test native_window_test.cpp)
target_link_libraries(native_window_test PRIVATE zbcore zbmockjvm)
add_test(NAME native_window_test COMMAND native_window_test)

# Proves GuestJniEngine chains EglBackend/NativeWindowBackend alongside HostGl/HostAssets/HostJni
# (Phase 7a Task 6). Each mode is its own process, like gl_chain_test.cpp.
add_executable(egl_chain_test egl_chain_test.cpp mock_egl.cpp)
target_link_libraries(egl_chain_test PRIVATE zbcore zbmockjvm)
foreach(egl_chain_mode no-backend with-backend)
    add_test(NAME egl_chain_test_${egl_chain_mode} COMMAND egl_chain_test ${egl_chain_mode})
endforeach()
# window-surface mode (Phase 7a Task 9) runs guest/tests/zbeglprobe.c through a real
# LibraryRuntime; needs tools/build_guest.sh and the extracted sysroot.
add_test(NAME egl_chain_test_window_surface COMMAND egl_chain_test window-surface
    ${CMAKE_SOURCE_DIR}/sysroot
    ${CMAKE_SOURCE_DIR}/build/guest/zbhost
    ${CMAKE_SOURCE_DIR}/build/guest/lib)

# ZBridge.java and core/android/zbridge_jni.cpp declare and define the same native methods.
add_test(NAME zbridge_natives_check
    COMMAND Python3::Interpreter ${CMAKE_SOURCE_DIR}/tools/check_zbridge_natives.py)

# libzbproxy.so (core/android/zbproxy.c) built for the host against the JDK jni.h, then driven
# by a fake JavaVM/JNIEnv that loads it by absolute path so dladdr runs for real.
find_path(ZB_JDK_JNI_INCLUDE jni.h
    HINTS $ENV{JAVA_HOME}/include
    PATHS /usr/lib/jvm/default-java/include /usr/lib/jvm/java-17-openjdk-arm64/include
    NO_CMAKE_FIND_ROOT_PATH)
if (ZB_JDK_JNI_INCLUDE AND EXISTS ${ZB_JDK_JNI_INCLUDE}/linux/jni_md.h)
    add_executable(jni_env_backend_test
        jni_env_backend_test.cpp
        ${CMAKE_SOURCE_DIR}/core/android/jni_env_backend.cpp
    )
    target_include_directories(jni_env_backend_test PRIVATE
        ${CMAKE_SOURCE_DIR}/core/android
        ${ZB_JDK_JNI_INCLUDE}
        ${ZB_JDK_JNI_INCLUDE}/linux
    )
    target_link_libraries(jni_env_backend_test PRIVATE zbcore)
    target_compile_options(jni_env_backend_test PRIVATE -Wall -Wextra)
    add_test(NAME jni_env_backend_test COMMAND jni_env_backend_test)

    add_library(zbproxy_host SHARED ${CMAKE_SOURCE_DIR}/core/android/zbproxy.c)
    target_include_directories(zbproxy_host PRIVATE ${ZB_JDK_JNI_INCLUDE} ${ZB_JDK_JNI_INCLUDE}/linux)
    set_target_properties(zbproxy_host PROPERTIES C_VISIBILITY_PRESET hidden)
    target_compile_options(zbproxy_host PRIVATE -Wall -Wextra -Werror)
    target_link_options(zbproxy_host PRIVATE -Wl,--no-undefined)

    add_executable(zbproxy_fake_jni_test ${CMAKE_SOURCE_DIR}/tests/proxy/fake_jni.c)
    target_include_directories(zbproxy_fake_jni_test PRIVATE ${ZB_JDK_JNI_INCLUDE} ${ZB_JDK_JNI_INCLUDE}/linux)
    target_compile_options(zbproxy_fake_jni_test PRIVATE -Wall -Wextra)
    target_link_libraries(zbproxy_fake_jni_test PRIVATE ${CMAKE_DL_LIBS})
    add_dependencies(zbproxy_fake_jni_test zbproxy_host)
    add_test(NAME zbproxy_fake_jni_test COMMAND zbproxy_fake_jni_test $<TARGET_FILE:zbproxy_host>)
else()
    message(STATUS "JDK jni.h not found: zbproxy_fake_jni_test is not built")
endif()

# Structure of the Android arm64 proxy from build/android-arm64; skipped when not built there.
set(ZB_ANDROID_PROXY ${CMAKE_SOURCE_DIR}/build/android-arm64/core/libzbproxy.so
    CACHE FILEPATH "Android arm64 libzbproxy.so checked by zbproxy_structure_test")
add_test(NAME zbproxy_structure_test
    COMMAND Python3::Interpreter ${CMAKE_SOURCE_DIR}/tools/check_zbproxy.py --skip-missing ${ZB_ANDROID_PROXY})
set_tests_properties(zbproxy_structure_test PROPERTIES SKIP_RETURN_CODE 77)

# Mock JVM behind JniBackend, shared by the JNI tests.
add_library(zbmockjvm STATIC mock_jvm.cpp)
target_link_libraries(zbmockjvm PUBLIC zbcore)
target_compile_options(zbmockjvm PRIVATE -Wall -Wextra)

target_link_libraries(host_looper_test PRIVATE zbmockjvm)
add_test(NAME host_looper_guest COMMAND host_looper_test
    ${CMAKE_SOURCE_DIR}/sysroot
    ${CMAKE_SOURCE_DIR}/build/guest/zbhost
    ${CMAKE_SOURCE_DIR}/build/guest/lib)

add_executable(mock_jvm_test mock_jvm_test.cpp)
target_link_libraries(mock_jvm_test PRIVATE zbmockjvm)
add_test(NAME mock_jvm_test COMMAND mock_jvm_test)

# Needs build/guest/hello_static from tools/build_guest.sh.
add_executable(elf_loader_test elf_loader_test.cpp)
target_link_libraries(elf_loader_test PRIVATE zbcore)
add_test(NAME elf_loader_test COMMAND elf_loader_test ${CMAKE_SOURCE_DIR}/build/guest/hello_static)

# Needs build/guest/host_call_static from tools/build_guest.sh.
add_executable(guest_call_test guest_call_test.cpp)
target_link_libraries(guest_call_test PRIVATE zbcore)
add_test(NAME guest_call_test COMMAND guest_call_test ${CMAKE_SOURCE_DIR}/build/guest/host_call_static)

add_executable(host_call_dispatch_test host_call_dispatch_test.cpp)
target_link_libraries(host_call_dispatch_test PRIVATE zbcore)
add_test(NAME host_call_dispatch_test COMMAND host_call_dispatch_test
    ${CMAKE_SOURCE_DIR}/build/guest/host_call_static)

add_executable(zbhost_protocol_test zbhost_protocol_test.cpp)
target_link_libraries(zbhost_protocol_test PRIVATE zbcore)
add_test(NAME zbhost_protocol_test COMMAND zbhost_protocol_test
    ${CMAKE_SOURCE_DIR}/sysroot
    ${CMAKE_SOURCE_DIR}/build/guest/zbhost
    ${CMAKE_SOURCE_DIR}/build/guest/lib)

# Needs tools/build_guest.sh and the extracted sysroot.
add_executable(library_runtime_test library_runtime_test.cpp)
target_link_libraries(library_runtime_test PRIVATE zbcore)
# Hold a real signal-forwarding reader at post_signal without a production test hook.
# The wrap relies on no LTO for this target: the call must stay a cross-object symbol reference.
target_link_options(library_runtime_test PRIVATE
    -Wl,--wrap=_ZN2zb11GuestThread11post_signalERKNS_1g9siginfo32E)
add_test(NAME library_runtime_test COMMAND library_runtime_test
    ${CMAKE_SOURCE_DIR}/sysroot
    ${CMAKE_SOURCE_DIR}/build/guest/zbhost
    ${CMAKE_SOURCE_DIR}/build/guest/lib/libzbcallprobe.so)

# Needs tools/build_guest.sh and the extracted sysroot.
add_executable(native_activity_probe_test native_activity_probe_test.cpp)
target_link_libraries(native_activity_probe_test PRIVATE zbcore)
add_test(NAME native_activity_probe_test COMMAND native_activity_probe_test
    ${CMAKE_SOURCE_DIR}/sysroot
    ${CMAKE_SOURCE_DIR}/build/guest/zbhost
    ${CMAKE_SOURCE_DIR}/build/guest/lib/libzbnativeprobe.so)

# Needs tools/build_guest.sh and the extracted sysroot.
add_executable(jni_bridge_test jni_bridge_test.cpp)
target_link_libraries(jni_bridge_test PRIVATE zbmockjvm)
add_test(NAME jni_bridge_test COMMAND jni_bridge_test
    ${CMAKE_SOURCE_DIR}/sysroot
    ${CMAKE_SOURCE_DIR}/build/guest/zbhost
    ${CMAKE_SOURCE_DIR}/build/guest/lib)

# Needs tools/build_guest.sh and the extracted sysroot.
add_executable(jni_loader_test jni_loader_test.cpp)
target_link_libraries(jni_loader_test PRIVATE zbmockjvm)
add_test(NAME jni_loader_test COMMAND jni_loader_test
    ${CMAKE_SOURCE_DIR}/sysroot
    ${CMAKE_SOURCE_DIR}/build/guest/zbhost
    ${CMAKE_SOURCE_DIR}/build/guest/lib)

# Needs tools/build_guest.sh and the extracted sysroot. Each mode needs its own process: the
# guest runtime is process-lifetime.
add_executable(guest_jni_engine_test guest_jni_engine_test.cpp)
target_link_libraries(guest_jni_engine_test PRIVATE zbmockjvm)
foreach(mode load preload-failure)
    add_test(NAME guest_jni_engine_test_${mode} COMMAND guest_jni_engine_test
        ${CMAKE_SOURCE_DIR}/sysroot
        ${CMAKE_SOURCE_DIR}/build/guest/zbhost
        ${CMAKE_SOURCE_DIR}/build/guest/lib
        ${mode})
endforeach()

# Needs tools/build_guest.sh and the extracted sysroot.
add_executable(thread_exit_test thread_exit_test.cpp)
target_link_libraries(thread_exit_test PRIVATE zbcore)
# Observe thread retirement at clear_process_signal_target. The wrap relies on no LTO for this
# target: the call from process.cpp must stay a cross-object symbol reference.
target_link_options(thread_exit_test PRIVATE
    -Wl,--wrap=_ZN2zb7Process27clear_process_signal_targetEPNS_11GuestThreadE)
add_test(NAME thread_exit_test COMMAND thread_exit_test
    ${CMAKE_SOURCE_DIR}/sysroot
    ${CMAKE_SOURCE_DIR}/build/guest/threads_dynamic)

# Needs tools/build_guest.sh and the extracted sysroot.
add_executable(async_signal_test async_signal_test.cpp)
target_link_libraries(async_signal_test PRIVATE zbcore)
add_test(NAME async_signal_test COMMAND async_signal_test ${CMAKE_SOURCE_DIR}/sysroot ${CMAKE_SOURCE_DIR}/build/guest)
