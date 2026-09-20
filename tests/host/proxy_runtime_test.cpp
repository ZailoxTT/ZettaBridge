// ProxyRuntime state machine and path/config validation against a fake engine: no guest code runs.
#include <unistd.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "check.h"
#include "zb/proxy_runtime.h"
#include "zb/runtime_report.h"

namespace {

namespace fs = std::filesystem;
using zb::JniBackend;

constexpr JniBackend::Env kEnv = 0x1234;
constexpr JniBackend::Ref kLoader = 0x77;

bool contains(const std::string& text, const char* part) {
    return text.find(part) != std::string::npos;
}

void touch(const fs::path& path) {
    fs::create_directories(path.parent_path());
    std::ofstream(path) << "x";
}

// <base>/files/{zb/sysroot,zb/guest,plugins/<package>/{lib,proxy}}
struct Tree {
    fs::path files;
    fs::path root(const char* package) const { return files / "plugins" / package; }
    std::string proxy(const char* package, const char* library) const {
        return (root(package) / "proxy" / library).string();
    }
    void library(const char* package, const char* library) const {
        touch(root(package) / "lib" / library);
        touch(root(package) / "proxy" / library);
    }
};

Tree make_tree(const fs::path& base, bool runtime) {
    Tree tree{base / "files"};
    if (runtime) {
        touch(tree.files / "zb/sysroot/system/bin/linker");
        touch(tree.files / "zb/guest/zbhost");
        touch(tree.files / "zb/guest/lib/libzbcompat.so");
        touch(tree.files / "zb/guest/lib/libzbjni.so");
    }
    for (const char* package : {"com.example.a", "com.example.b"}) {
        fs::create_directories(tree.root(package) / "lib");
        fs::create_directories(tree.root(package) / "proxy");
    }
    return tree;
}

class FakeEngine final : public zb::ProxyLoadEngine {
public:
    bool bind_class_loader(JniBackend::Env env, JniBackend::Ref loader, std::string& error) override {
        std::lock_guard<std::mutex> lock(mutex_);
        CHECK(env == kEnv);
        ++binds;
        if (!bind_ok) {
            error = "a different plugin class loader is already bound";
            return false;
        }
        bound_loader = loader;
        return true;
    }

    bool start(const zb::LibraryRuntimeOptions& options, std::string& error) override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ++starts;
            started = options;
        }
        std::this_thread::sleep_for(start_delay);
        if (!start_error.empty()) {
            error = start_error;
            return false;
        }
        return true;
    }

    void add_path_alias(const std::string& guest_prefix, const std::string& host_prefix) override {
        std::lock_guard<std::mutex> lock(mutex_);
        aliases.emplace_back(guest_prefix, host_prefix);
    }

    zb::JniLoadReport load(JniBackend::Env env, const std::string& guest_library) override {
        std::unique_lock<std::mutex> lock(mutex_);
        CHECK(env == kEnv);
        loads.push_back(guest_library);
        const std::string name = fs::path(guest_library).filename().string();
        if (gated.count(name) != 0) {
            ++in_gate_;
            cv_.notify_all();
            CHECK(cv_.wait_for(lock, std::chrono::seconds(10), [this] { return open_; }));
        }
        const auto it = reports.find(name);
        if (it != reports.end()) return it->second;
        zb::JniLoadReport ok;
        ok.ok = true;
        ok.guest_handle = 1;
        return ok;
    }

    bool wait_in_gate(int count) {
        std::unique_lock<std::mutex> lock(mutex_);
        return cv_.wait_for(lock, std::chrono::seconds(10), [&] { return in_gate_ >= count; });
    }
    void open_gate() {
        std::lock_guard<std::mutex> lock(mutex_);
        open_ = true;
        cv_.notify_all();
    }
    std::size_t load_count(const std::string& name) {
        std::lock_guard<std::mutex> lock(mutex_);
        std::size_t count = 0;
        for (const auto& path : loads) count += fs::path(path).filename() == name ? 1 : 0;
        return count;
    }
    int start_count() {
        std::lock_guard<std::mutex> lock(mutex_);
        return starts;
    }

    int binds = 0;
    bool bind_ok = true;
    JniBackend::Ref bound_loader = 0;
    int starts = 0;
    zb::LibraryRuntimeOptions started;
    std::string start_error;
    std::chrono::milliseconds start_delay{0};
    std::vector<std::string> loads;
    std::vector<std::pair<std::string, std::string>> aliases;
    std::map<std::string, zb::JniLoadReport> reports;
    std::map<std::string, int> gated;

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    int in_gate_ = 0;
    bool open_ = false;
};

void test_paths(const Tree& tree) {
    const std::string root = fs::canonical(tree.root("com.example.a")).string();
    const std::string files = fs::canonical(tree.files).string();
    zb::ProxyLocation location;
    std::string error;
    CHECK(zb::parse_proxy_path(root + "/proxy/libfoo.so", location, error));
    CHECK(location.proxy == root + "/proxy/libfoo.so");
    CHECK(location.files_dir == files);
    CHECK(location.plugin_root == root);
    CHECK(location.package == "com.example.a");
    CHECK(location.library == "libfoo.so");
    CHECK(location.guest_library == root + "/lib/libfoo.so");

    for (const std::string& bad : {
             std::string("proxy/libfoo.so"),                  // relative
             root + "/lib/libfoo.so",                         // not in proxy/
             root + "/proxy/foo.so",                          // no lib prefix
             root + "/proxy/lib.so",                          // empty name
             root + "/proxy/libfoo.txt",                      // no .so suffix
             root + "/proxy/sub/libfoo.so",                   // nested
             files + "/other/com.example.a/proxy/libfoo.so",  // not under plugins/
             files + "/plugins/proxy/libfoo.so",              // no package directory
             root + "/proxy/../proxy/libfoo.so",              // not canonical
             std::string("/proxy/libfoo.so"),
         }) {
        error.clear();
        CHECK(!zb::parse_proxy_path(bad, location, error));
        CHECK(!error.empty());
    }

    std::string canonical;
    CHECK(zb::canonical_path(tree.files.string() + "/plugins/./com.example.a", canonical, error));
    CHECK(canonical == root);
    CHECK(!zb::canonical_path(tree.files.string() + "/missing", canonical, error));
    CHECK(contains(error, "missing"));

    zb::GuestRuntimeLayout layout;
    CHECK(zb::resolve_guest_runtime_layout(files, layout, error));
    CHECK(layout.sysroot == files + "/zb/sysroot");
    CHECK(layout.zbhost == files + "/zb/guest/zbhost");
    CHECK(layout.guest_lib_dir == files + "/zb/guest/lib");

    // The host environment never reaches the guest.
    setenv("LD_PRELOAD", "/host/libpreload.so", 1);
    setenv("LD_LIBRARY_PATH", "/host/lib64", 1);
    const zb::LibraryRuntimeOptions options = zb::guest_runtime_options(layout, root, 16);
    CHECK(options.zbhost == layout.zbhost);
    CHECK(options.sysroot == layout.sysroot);
    CHECK(options.target_sdk == 16);
    CHECK(options.preload == "libzbjni.so");
    CHECK(options.guest_environment.size() == 1);
    CHECK(options.guest_environment[0] == "LD_LIBRARY_PATH=" + files + "/zb/guest/lib:" + root + "/lib");
}

void test_bad_runtime_layout(const fs::path& base) {
    const Tree tree = make_tree(base, false);
    touch(tree.files / "zb/guest/zbhost");
    touch(tree.files / "zb/guest/lib/libzbcompat.so");
    const std::string files = fs::canonical(tree.files).string();
    zb::GuestRuntimeLayout layout;
    std::string error;
    CHECK(!zb::resolve_guest_runtime_layout(files, layout, error));
    CHECK(contains(error, "libzbjni.so"));
    touch(tree.files / "zb/guest/lib/libzbjni.so");
    CHECK(!zb::resolve_guest_runtime_layout(files, layout, error));
    CHECK(contains(error, "linker"));

    // A bad layout fails activation and locks nothing in.
    FakeEngine engine;
    zb::ProxyRuntime runtime(engine);
    CHECK(!runtime.activate_plugin(kEnv, tree.root("com.example.a").string(), 16, kLoader, error));
    CHECK(contains(error, "linker"));
    CHECK(engine.binds == 0);
    touch(tree.files / "zb/sysroot/system/bin/linker");
    CHECK(runtime.activate_plugin(kEnv, tree.root("com.example.a").string(), 16, kLoader, error));
    CHECK(engine.binds == 1 && engine.bound_loader == kLoader);
    // Activation redirects the plugin's arm64 proxy directory at its arm32 libraries, so a guest
    // that opens a file beside "its own" library reaches the real one.
    CHECK(engine.aliases.size() == 1);
    CHECK(engine.aliases[0].first == tree.root("com.example.a").string() + "/proxy/");
    CHECK(engine.aliases[0].second == tree.root("com.example.a").string() + "/lib/");
}

void test_activation_and_loads(const Tree& tree) {
    FakeEngine engine;
    zb::ProxyRuntime runtime(engine);
    std::string error;

    // No active plugin: the class loader was never bound, so nothing is started.
    tree.library("com.example.a", "libfoo.so");
    auto result = runtime.on_proxy_loaded(kEnv, tree.proxy("com.example.a", "libfoo.so"));
    CHECK(!result.ok && contains(result.error, "activatePlugin"));
    CHECK(engine.start_count() == 0);

    // Invalid activations.
    CHECK(!runtime.activate_plugin(kEnv, tree.root("com.example.a").string(), 0, kLoader, error));
    CHECK(contains(error, "targetSdk"));
    CHECK(!runtime.activate_plugin(kEnv, tree.root("com.example.a").string(), 16, 0, error));
    CHECK(contains(error, "class loader"));
    CHECK(!runtime.activate_plugin(kEnv, (tree.files / "zb").string(), 16, kLoader, error));
    CHECK(contains(error, "plugins"));
    CHECK(!runtime.activate_plugin(kEnv, (tree.files / "plugins/com.example.missing").string(), 16, kLoader,
                                   error));
    engine.bind_ok = false;
    CHECK(!runtime.activate_plugin(kEnv, tree.root("com.example.a").string(), 16, kLoader, error));
    CHECK(contains(error, "class loader"));
    engine.bind_ok = true;

    // First start. A failure memoized before activation is kept for its path: ART never retries.
    CHECK(runtime.activate_plugin(kEnv, tree.root("com.example.a").string(), 16, kLoader, error));
    tree.library("com.example.a", "libone.so");
    engine.reports["libone.so"].ok = true;
    engine.reports["libone.so"].jni_version = 0x00010004;
    result = runtime.on_proxy_loaded(kEnv, tree.proxy("com.example.a", "libone.so"));
    CHECK(result.ok && result.jni_version == 0x00010004 && result.error.empty());
    CHECK(engine.start_count() == 1);
    const std::string root = fs::canonical(tree.root("com.example.a")).string();
    CHECK(engine.started.target_sdk == 16);
    CHECK(engine.started.preload == "libzbjni.so");
    CHECK(engine.loads.back() == root + "/lib/libone.so");
    result = runtime.on_proxy_loaded(kEnv, tree.proxy("com.example.a", "libfoo.so"));
    CHECK(!result.ok && contains(result.error, "activatePlugin"));
    CHECK(engine.load_count("libfoo.so") == 0);

    // Repeated same proxy, also through a symlinked alias: memoized by canonical path.
    fs::create_symlink(tree.root("com.example.a"), tree.files / "alias");
    for (const std::string& path :
         {tree.proxy("com.example.a", "libone.so"), (tree.files / "alias/proxy/libone.so").string()}) {
        result = runtime.on_proxy_loaded(kEnv, path);
        CHECK(result.ok && result.jni_version == 0x00010004);
    }
    CHECK(engine.load_count("libone.so") == 1);
    CHECK(engine.start_count() == 1);
    CHECK(!runtime.load_error(tree.proxy("com.example.a", "libone.so")));

    // Same plugin and targetSdk re-activates (the backend checks the loader); others are rejected.
    CHECK(runtime.activate_plugin(kEnv, (tree.files / "alias").string(), 16, kLoader, error));
    CHECK(!runtime.activate_plugin(kEnv, tree.root("com.example.a").string(), 19, kLoader, error));
    CHECK(contains(error, "restart"));
    CHECK(!runtime.activate_plugin(kEnv, tree.root("com.example.b").string(), 16, kLoader, error));
    CHECK(contains(error, "com.example.b") && contains(error, "restart"));

    // A proxy of the second plugin is rejected too, without touching the guest.
    tree.library("com.example.b", "libother.so");
    result = runtime.on_proxy_loaded(kEnv, tree.proxy("com.example.b", "libother.so"));
    CHECK(!result.ok && contains(result.error, "com.example.b") && contains(result.error, "restart"));
    CHECK(engine.load_count("libother.so") == 0);

    // Bad proxy layout, missing proxy file, missing guest library.
    touch(tree.root("com.example.a") / "other/libodd.so");
    result = runtime.on_proxy_loaded(kEnv, (tree.root("com.example.a") / "other/libodd.so").string());
    CHECK(!result.ok && contains(result.error, "proxy"));
    const std::string missing = tree.proxy("com.example.a", "libmissing.so");
    result = runtime.on_proxy_loaded(kEnv, missing);
    CHECK(!result.ok && contains(result.error, "libmissing.so"));
    CHECK(runtime.load_error(missing) && *runtime.load_error(missing) == result.error);
    touch(tree.root("com.example.a") / "proxy/libnolib.so");
    result = runtime.on_proxy_loaded(kEnv, tree.proxy("com.example.a", "libnolib.so"));
    CHECK(!result.ok && contains(result.error, "lib/libnolib.so"));
    CHECK(engine.load_count("libnolib.so") == 0);

    // Bind failure: detail kept per path and as the latest error; never retried.
    tree.library("com.example.a", "libbind.so");
    engine.reports["libbind.so"].error = "RegisterNatives failed for zb.Load.over(I)I";
    const std::string bind_proxy = tree.proxy("com.example.a", "libbind.so");
    result = runtime.on_proxy_loaded(kEnv, bind_proxy);
    CHECK(!result.ok && contains(result.error, "RegisterNatives failed for zb.Load.over(I)I"));
    CHECK(contains(result.error, "libbind.so"));
    CHECK(runtime.load_error(bind_proxy) && *runtime.load_error(bind_proxy) == result.error);
    CHECK(runtime.last_load_error() && contains(*runtime.last_load_error(), "RegisterNatives"));
    const auto again = runtime.on_proxy_loaded(kEnv, bind_proxy);
    CHECK(!again.ok && again.error == result.error);
    CHECK(engine.load_count("libbind.so") == 1);

    // Unsupported JNI versions, including 1.1 which the guest loader accepts but ART does not.
    for (const auto& [name, version] : {std::pair<const char*, std::int32_t>{"libv11.so", 0x00010001},
                                        std::pair<const char*, std::int32_t>{"libvbad.so", 0x7fff}}) {
        tree.library("com.example.a", name);
        engine.reports[name].ok = true;
        engine.reports[name].jni_version = version;
        result = runtime.on_proxy_loaded(kEnv, tree.proxy("com.example.a", name));
        CHECK(!result.ok && contains(result.error, "unsupported JNI version"));
    }
    CHECK(runtime.last_load_error() && contains(*runtime.last_load_error(), "0x00007fff"));

    // Concurrent loads of different proxies run in parallel.
    tree.library("com.example.a", "libbar.so");
    tree.library("com.example.a", "libbaz.so");
    engine.gated = {{"libbar.so", 1}, {"libbaz.so", 1}, {"libslow.so", 1}};
    zb::ProxyLoadResult bar, baz;
    std::thread first([&] { bar = runtime.on_proxy_loaded(kEnv, tree.proxy("com.example.a", "libbar.so")); });
    std::thread second([&] { baz = runtime.on_proxy_loaded(kEnv, tree.proxy("com.example.a", "libbaz.so")); });
    CHECK(engine.wait_in_gate(2));

    // The same proxy requested while it loads waits for the first result.
    tree.library("com.example.a", "libslow.so");
    zb::ProxyLoadResult slow1, slow2;
    std::thread slow_first([&] { slow1 = runtime.on_proxy_loaded(kEnv, tree.proxy("com.example.a", "libslow.so")); });
    CHECK(engine.wait_in_gate(3));
    std::thread slow_second(
        [&] { slow2 = runtime.on_proxy_loaded(kEnv, tree.proxy("com.example.a", "libslow.so")); });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    engine.open_gate();
    first.join();
    second.join();
    slow_first.join();
    slow_second.join();
    CHECK(bar.ok && baz.ok && slow1.ok && slow2.ok);
    CHECK(slow1.jni_version == 0x00010006 && slow2.jni_version == 0x00010006);
    CHECK(engine.load_count("libslow.so") == 1);
    CHECK(engine.start_count() == 1);
}

void test_concurrent_first_start(const Tree& tree) {
    FakeEngine engine;
    engine.start_delay = std::chrono::milliseconds(100);
    zb::ProxyRuntime runtime(engine);
    std::string error;
    CHECK(runtime.activate_plugin(kEnv, tree.root("com.example.b").string(), 21, kLoader, error));
    tree.library("com.example.b", "libx.so");
    tree.library("com.example.b", "liby.so");
    zb::ProxyLoadResult x, y;
    std::thread first([&] { x = runtime.on_proxy_loaded(kEnv, tree.proxy("com.example.b", "libx.so")); });
    std::thread second([&] { y = runtime.on_proxy_loaded(kEnv, tree.proxy("com.example.b", "liby.so")); });
    first.join();
    second.join();
    CHECK(x.ok && y.ok);
    CHECK(engine.start_count() == 1);
    CHECK(engine.started.target_sdk == 21);
}

void test_preload_failure(const Tree& tree) {
    FakeEngine engine;
    engine.start_error = "zbhost could not preload a library (status 4)";
    zb::ProxyRuntime runtime(engine);
    std::string error;
    CHECK(runtime.activate_plugin(kEnv, tree.root("com.example.b").string(), 16, kLoader, error));
    tree.library("com.example.b", "libp1.so");
    tree.library("com.example.b", "libp2.so");
    const auto first = runtime.on_proxy_loaded(kEnv, tree.proxy("com.example.b", "libp1.so"));
    CHECK(!first.ok && contains(first.error, "status 4") && contains(first.error, "start"));
    // A failed start is sticky: the process-lifetime runtime is never restarted.
    const auto second = runtime.on_proxy_loaded(kEnv, tree.proxy("com.example.b", "libp2.so"));
    CHECK(!second.ok && contains(second.error, "status 4"));
    CHECK(engine.start_count() == 1);
    CHECK(engine.loads.empty());
    const auto stored = runtime.load_error(tree.proxy("com.example.b", "libp1.so"));
    CHECK(stored && *stored == first.error);
    CHECK(runtime.last_load_error() && *runtime.last_load_error() == second.error);
}

// Everything a device run must leave behind about plugin activation and proxy loads.
void test_runtime_report(const Tree& tree) {
    zb::RuntimeReport& report = zb::runtime_report();
    report.clear();

    FakeEngine engine;
    zb::ProxyRuntime runtime(engine);
    std::string error;
    CHECK(runtime.activate_plugin(kEnv, tree.root("com.example.a").string(), 16, kLoader, error));

    tree.library("com.example.a", "libgood.so");
    engine.reports["libgood.so"].ok = true;
    engine.reports["libgood.so"].jni_version = 0x00010006;
    CHECK(runtime.on_proxy_loaded(kEnv, tree.proxy("com.example.a", "libgood.so")).ok);
    // A memoized repeat is one load, not two.
    CHECK(runtime.on_proxy_loaded(kEnv, tree.proxy("com.example.a", "libgood.so")).ok);

    tree.library("com.example.a", "libbad.so");
    engine.reports["libbad.so"].ok = false;
    engine.reports["libbad.so"].error = "guest dlopen failed: cannot locate symbol";
    CHECK(!runtime.on_proxy_loaded(kEnv, tree.proxy("com.example.a", "libbad.so")).ok);

    CHECK(report.proxy_loads() == 1);
    const std::string text = report.text();
    const std::string root = fs::canonical(tree.root("com.example.a")).string();
    CHECK(contains(text, ("plugin: " + root + " targetSdk 16").c_str()));
    CHECK(contains(text, "proxy-loads: 1"));
    CHECK(contains(text, "proxy-loaded: libgood.so jni=0x00010006"));
    CHECK(contains(text, "proxy-failures: 1"));
    CHECK(contains(text, "proxy-failed: libbad.so guest dlopen failed: cannot locate symbol"));
}

}  // namespace

int main() {
    char pattern[] = "/tmp/zb_proxy_runtime_XXXXXX";
    CHECK(mkdtemp(pattern) != nullptr);
    const fs::path base = pattern;

    const Tree tree = make_tree(base / "good", true);
    test_paths(tree);
    test_bad_runtime_layout(base / "bad");
    test_activation_and_loads(tree);
    test_concurrent_first_start(make_tree(base / "concurrent", true));
    test_preload_failure(make_tree(base / "preload", true));
    test_runtime_report(make_tree(base / "report", true));

    fs::remove_all(base);
    std::puts("proxy_runtime_test PASS");
    return 0;
}
