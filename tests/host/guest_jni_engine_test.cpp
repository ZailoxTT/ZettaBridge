// GuestJniEngine: the real LibraryRuntime + HostJni + JniLoader graph behind ProxyRuntime, over
// the mock JVM and a launcher-shaped files directory built from build/guest.
// Usage: guest_jni_engine_test <sysroot> <zbhost> <guest lib dir> load|preload-failure
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

#include "check.h"
#include "mock_jvm.h"
#include "zb/host_jni.h"
#include "zb/proxy_runtime.h"

namespace {

namespace fs = std::filesystem;
using zb::mock::MockJvm;
using NoArgsFn = std::int32_t (*)(std::uint64_t, std::uint64_t);

constexpr const char* kPackage = "com.example.probe";

bool contains(const std::string& text, const char* part) {
    return text.find(part) != std::string::npos;
}

void link(const fs::path& target, const fs::path& link_path) {
    fs::create_directories(link_path.parent_path());
    fs::create_symlink(fs::absolute(target), link_path);
}

// <files>/zb/{sysroot,guest/zbhost,guest/lib/...}, plugins/<pkg>/{lib,proxy}/lib*.so
// With valid_zbjni false, libzbjni.so exists (the layout check passes) but is not an ELF file, so
// zbhost's preload fails for real and exits with status 4.
fs::path make_files(const fs::path& base, char** argv, bool valid_zbjni) {
    const fs::path files = base / "files";
    link(argv[1], files / "zb/sysroot");
    link(argv[2], files / "zb/guest/zbhost");
    const fs::path guest_lib = argv[3];
    link(guest_lib / "libzbcompat.so", files / "zb/guest/lib/libzbcompat.so");
    if (valid_zbjni) {
        link(guest_lib / "libzbjni.so", files / "zb/guest/lib/libzbjni.so");
    } else {
        std::ofstream(files / "zb/guest/lib/libzbjni.so") << "not an ELF file";
    }
    const fs::path root = files / "plugins" / kPackage;
    for (const char* name : {"libzbloadprobe.so", "libzbloadbad.so", "libzbloadunknown.so",
                             "libzbloadshim.so", "libzbloadhidden.so"}) {
        fs::create_directories(root / "lib");
        fs::copy_file(guest_lib / name, root / "lib" / name);
        fs::create_directories(root / "proxy");
        std::ofstream(root / "proxy" / name) << "proxy";
    }
    return files;
}

}  // namespace

int main(int argc, char** argv) {
    CHECK(argc == 5);
    const std::string mode = argv[4];
    CHECK(mode == "load" || mode == "preload-failure");
    char pattern[] = "/tmp/zb_guest_jni_engine_XXXXXX";
    CHECK(mkdtemp(pattern) != nullptr);
    const fs::path base = pattern;
    const fs::path files = make_files(base, argv, mode == "load");
    const std::string root = (files / "plugins" / kPackage).string();
    const auto proxy = [&](const char* name) { return root + "/proxy/" + name; };

    auto* vm = new MockJvm();
    vm->define_class("zb/Load");
    vm->add_native("zb/Load", "shortExport", "()I", true);
    vm->add_native("zb/Load", "shortExport", "(I)I", true);
    vm->add_native("zb/Load", "over", "(I)I", true);
    vm->add_native("zb/Load", "over", "(Ljava/lang/String;)I", false);
    // The natives of the library the shim opens by itself: Java never loads that library.
    vm->define_class("zb/Hidden");
    vm->add_native("zb/Hidden", "value", "()I", true);
    // Process-lifetime graph: never destroyed, the test ends with _Exit.
    auto* engine = new zb::GuestJniEngine(*vm);
    auto* runtime = new zb::ProxyRuntime(*engine);

    const auto env = vm->thread_env();
    MockJvm::NativeFrame frame(*vm, env);
    const auto loader = frame.local(vm->new_object("zb/Load"));
    std::string error;
    CHECK(runtime->activate_plugin(env, root, 16, loader, error));

    if (mode == "preload-failure") {
        const auto first = runtime->on_proxy_loaded(env, proxy("libzbloadprobe.so"));
        CHECK(!first.ok && contains(first.error, "status 4"));
        const auto second = runtime->on_proxy_loaded(env, proxy("libzbloadbad.so"));
        CHECK(!second.ok && contains(second.error, "status 4"));
        CHECK(!vm->exception_check(env));
        fs::remove_all(base);
        std::puts("guest_jni_engine_test preload-failure PASS");
        std::fflush(stdout);
        std::_Exit(0);
    }

    const auto loaded = runtime->on_proxy_loaded(env, proxy("libzbloadprobe.so"));
    if (!loaded.ok) std::fprintf(stderr, "load error: %s\n", loaded.error.c_str());
    CHECK(loaded.ok && loaded.jni_version == 0x00010006);
    CHECK(engine->host_jni().ready());
    const auto no_args = reinterpret_cast<NoArgsFn>(vm->native_function("zb/Load", "shortExport", "()I"));
    CHECK(no_args != nullptr);
    {
        MockJvm::NativeFrame call(*vm, env);
        CHECK(no_args(env, call.local(vm->class_object("zb/Load"))) == 17);
    }
    // A shim whose JNI_OnLoad opens the real library: ART resolves natives against the library
    // Java loaded, which is the shim, so the natives of the opened library are reachable from
    // nowhere unless the loader sweeps what the guest mapped by itself.
    const auto shim = runtime->on_proxy_loaded(env, proxy("libzbloadshim.so"));
    if (!shim.ok) std::fprintf(stderr, "shim load error: %s\n", shim.error.c_str());
    CHECK(shim.ok && shim.jni_version == 0x00010006);
    const auto hidden = reinterpret_cast<NoArgsFn>(vm->native_function("zb/Hidden", "value", "()I"));
    CHECK(hidden != nullptr);
    {
        MockJvm::NativeFrame call(*vm, env);
        CHECK(hidden(env, call.local(vm->class_object("zb/Hidden"))) == 41);
    }

    // Repeated load is memoized: nothing is registered again (a new registration takes a new thunk).
    const auto again = runtime->on_proxy_loaded(env, proxy("libzbloadprobe.so"));
    CHECK(again.ok && again.jni_version == 0x00010006);
    CHECK(vm->native_function("zb/Load", "shortExport", "()I") == reinterpret_cast<void*>(no_args));
    CHECK(!vm->exception_check(env));

    const auto bad = runtime->on_proxy_loaded(env, proxy("libzbloadbad.so"));
    CHECK(!bad.ok && contains(bad.error, "unsupported JNI version"));
    CHECK(contains(bad.error, "libzbloadbad.so"));

    const auto unknown = runtime->on_proxy_loaded(env, proxy("libzbloadunknown.so"));
    // An export with no matching declared native is skipped; the library still loads.
    CHECK(unknown.ok);
    CHECK(!runtime->load_error(proxy("libzbloadunknown.so")));
    CHECK(!vm->exception_check(env));

    fs::remove_all(base);
    std::puts("guest_jni_engine_test load PASS");
    std::fflush(stdout);
    std::_Exit(0);
}
