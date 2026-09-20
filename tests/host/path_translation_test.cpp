// Guest system paths map into the sysroot, which holds only the 32-bit binaries. Anything the
// sysroot does not have - fonts above all - must fall through to the device's own file, or a
// guest can load its libraries and still be unable to draw a single letter (Flutter, 2026-09-19).
#include <cstdio>
#include <filesystem>
#include <string>

#include "check.h"
#include "zb/process.h"

namespace fs = std::filesystem;

int main() {
    const fs::path root = fs::temp_directory_path() / "zb-path-translation-test";
    fs::remove_all(root);
    fs::create_directories(root / "system" / "lib");
    std::FILE* library = std::fopen((root / "system" / "lib" / "libc.so").c_str(), "w");
    CHECK(library != nullptr);
    std::fputs("not a real library", library);
    std::fclose(library);

    zb::Process process;
    process.set_sysroot(root.string());

    // A file the sysroot has: the guest gets the sysroot copy, never the device's own.
    CHECK(process.translate_path("/system/lib/libc.so") == (root / "system/lib/libc.so").string());
    // The APEX bionic path collapses onto the same copy.
    CHECK(process.translate_path("/apex/com.android.runtime/lib/bionic/libc.so") ==
          (root / "system/lib/libc.so").string());

    // A file the sysroot does not have: the guest gets the device path unchanged. Fonts are the
    // reason this rule exists; they are architecture-independent and live outside the sysroot.
    CHECK(process.translate_path("/system/fonts/Roboto-Regular.ttf") ==
          std::string("/system/fonts/Roboto-Regular.ttf"));
    CHECK(process.translate_path("/system/etc/fonts.xml") == std::string("/system/etc/fonts.xml"));

    // An alias redirects a whole directory, and wins over the sysroot rules: ART loads an arm64
    // proxy library, so paths reaching the guest from Java name the proxy directory while the
    // arm32 files live beside it.
    process.add_path_alias("/data/user/0/app/files/plugins/com.example/proxy/",
                           "/data/user/0/app/files/plugins/com.example/lib/");
    CHECK(process.translate_path("/data/user/0/app/files/plugins/com.example/proxy/libil2cpp.so") ==
          std::string("/data/user/0/app/files/plugins/com.example/lib/libil2cpp.so"));
    // A path that only shares the prefix's parent is untouched.
    CHECK(process.translate_path("/data/user/0/app/files/plugins/com.example/assets/data") ==
          std::string("/data/user/0/app/files/plugins/com.example/assets/data"));
    // The longest matching alias wins, and re-adding a prefix replaces its target.
    process.add_path_alias("/data/user/0/app/files/plugins/com.example/proxy/sub/", "/tmp/sub/");
    CHECK(process.translate_path("/data/user/0/app/files/plugins/com.example/proxy/sub/thing") ==
          std::string("/tmp/sub/thing"));
    process.add_path_alias("/data/user/0/app/files/plugins/com.example/proxy/sub/", "/tmp/other/");
    CHECK(process.translate_path("/data/user/0/app/files/plugins/com.example/proxy/sub/thing") ==
          std::string("/tmp/other/thing"));
    // An alias applies to a system path too, before the sysroot mapping.
    process.add_path_alias("/system/fonts/", "/tmp/fonts/");
    CHECK(process.translate_path("/system/fonts/Roboto-Regular.ttf") ==
          std::string("/tmp/fonts/Roboto-Regular.ttf"));

    // Paths outside the mapped prefixes are never touched.
    CHECK(process.translate_path("/data/data/com.example/files/thing.ttf") ==
          std::string("/data/data/com.example/files/thing.ttf"));

    fs::remove_all(root);
    std::puts("path_translation_test PASS");
    return 0;
}
