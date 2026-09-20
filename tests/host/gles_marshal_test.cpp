#include <sys/mman.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <string>

#include <dynarmic/interface/exclusive_monitor.h>

#include "check.h"
#include "mock_gles.h"
#include "zb/gl_hostcalls.h"
#include "zb/host_gl.h"
#include "zb/runtime_report.h"

namespace {

constexpr std::uint32_t kStack = 0x10000;
constexpr std::uint32_t kData = 0x20000;

void set_words(zb::LibraryRuntime& runtime, zb::GuestThread& thread,
               const std::array<std::uint32_t, 12>& words) {
    for (unsigned i = 0; i < 4; ++i) thread.regs()[i] = words[i];
    thread.regs()[13] = kStack;
    std::memcpy(runtime.memory().base() + kStack, words.data() + 4,
                (words.size() - 4) * sizeof(words[0]));
}

void dispatch_all_pointerless(zb::HostGl& host, zb::GuestThread& thread,
                              MockGles& backend, zb::LibraryRuntime& runtime) {
    const std::array<std::uint32_t, 12> words{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
    // The sync entry points validate their 32-bit handle before anything reaches the driver, so
    // the junk arguments below stop at HostGl by design. glMapBufferOES is here for a different
    // reason: it asks the backend for GL_BUFFER_SIZE first, and the junk target has no buffer.
    const std::array<const char*, 5> handle_only{"glClientWaitSync", "glDeleteSync", "glIsSync",
                                                 "glWaitSync", "glMapBufferOES"};
    std::size_t seen = 0;
    for (const zb::GlHostCallInfo& info : zb::kGlHostCalls) {
        if (info.has_pointer) continue;
        ++seen;
        if (std::find_if(handle_only.begin(), handle_only.end(), [&](const char* name) {
                return std::strcmp(name, info.name) == 0;
            }) != handle_only.end()) {
            set_words(runtime, thread, words);
            CHECK(host.handle_host_call(info.index, thread));
            continue;
        }
        set_words(runtime, thread, words);
        backend.set_error(0);
        const std::size_t before = backend.calls().size();
        CHECK(host.handle_host_call(info.index, thread));
        // The two semantic pointerless calls now both reach the backend: glDrawArrays first
        // materializes any enabled client arrays, and glGetString copies the returned string.
        CHECK(backend.calls().size() == before + 1);
        CHECK(backend.calls().back().name == info.name);
    }
    CHECK(seen == zb::kGlPointerlessHostCallCount);
}

}  // namespace

int main() {
    CHECK(zb::kGlHostCallCount == 142);
    CHECK(zb::kGlHostCall3Count == 104);
    CHECK(zb::kGlHostCallExtCount == 12);
    CHECK(zb::kGlHostCallTotalCount == 258);
    CHECK(zb::kGlPointerlessHostCallCount == 134);
    CHECK(zb::ZB_GL_HC_glActiveTexture == 0);
    CHECK(zb::ZB_GL_HC_glViewport == 141);
    // GLES 3.0 is appended after every other stub library, so the GLES 2.0, AAsset*,
    // ANativeWindow_* and EGL indices all kept their values.
    CHECK(zb::kGlHostCall3First == 228);
    CHECK(zb::kGlHostCall3Last == 331);
    CHECK(zb::ZB_GL_HC_glBindVertexArray == 234);
    CHECK(zb::gl_host_call(141) != nullptr && zb::gl_host_call(142) == nullptr);
    CHECK(zb::gl_host_call(227) == nullptr);
    CHECK(std::strcmp(zb::gl_host_call_name(228), "glBeginQuery") == 0);
    // The extension block is appended after GLES 3.0 for the same reason it was appended after
    // every other stub library: nothing established moves.
    CHECK(zb::kGlHostCallExtFirst == 332);
    CHECK(zb::kGlHostCallExtLast == 343);
    CHECK(zb::ZB_GL_HC_glFramebufferTexture2DMultisampleEXT == 333);
    CHECK(zb::ZB_GL_HC_glMapBufferOES == 339);
    CHECK(std::strcmp(zb::gl_host_call_name(332), "glRenderbufferStorageMultisampleEXT") == 0);
    CHECK(std::strcmp(zb::gl_host_call_name(343), "glTexStorage3DEXT") == 0);
    CHECK(zb::gl_host_call(344) == nullptr);

    zb::LibraryRuntime runtime;
    CHECK(runtime.memory().map_anon(kStack, 0x1000, PROT_READ | PROT_WRITE));
    CHECK(runtime.memory().map_anon(kData, 0x1000, PROT_READ | PROT_WRITE));
    MockGles backend;
    std::uint32_t next_allocation = kData + 0x800;
    zb::HostGl host(
        runtime, backend,
        [&](std::size_t size) -> std::optional<std::uint32_t> {
            const std::uint32_t result = next_allocation;
            next_allocation += static_cast<std::uint32_t>((size + 7) & ~std::size_t{7});
            if (next_allocation > kData + 0xF00) return std::nullopt;
            return result;
        },
        [] { return std::uintptr_t{0xC0FFEE}; });
    Dynarmic::ExclusiveMonitor monitor(1);
    zb::GuestThread thread(runtime.memory(), &monitor, 0, false, zb::kCarrierCodeCacheSize);

    // GLES 2.0 has no 64-bit parameter: the two pointer-sized host types consume one guest
    // word and widen it. Both are signed in the NDK ABI.
    const std::array<std::uint32_t, 12> wide_words{0xFFFFFFFFu, 0x80000000u};
    set_words(runtime, thread, wide_words);
    zb::HostGl::Call widening(host, thread, zb::ZB_GL_HC_glBufferSubData);
    CHECK(widening.scalar<zb::GLintptr>(0) == static_cast<zb::GLintptr>(-1));
    CHECK(widening.scalar<zb::GLsizeiptr>(1) ==
          static_cast<zb::GLsizeiptr>(INT32_MIN));
    widening.set_result(static_cast<zb::GLint>(-7));
    CHECK(thread.regs()[0] == 0xFFFFFFF9u && thread.regs()[1] == 0);

    // Every registry function with no pointer parameter is dispatched, including the two
    // semantic handlers.
    dispatch_all_pointerless(host, thread, backend, runtime);

    // r0-r3 must be captured before HostGl clears r0/r1 for the default result. Eight arguments
    // also prove that argument positions 4-7 are read from the guest stack in word order.
    backend.clear_calls();
    const std::array<std::uint32_t, 12> copy_words{
        0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0, 0, 0, 0};
    set_words(runtime, thread, copy_words);
    CHECK(host.handle_host_call(zb::ZB_GL_HC_glCopyTexSubImage2D, thread));
    CHECK(backend.calls().size() == 1);
    CHECK(backend.calls()[0].name == "glCopyTexSubImage2D");
    CHECK(backend.calls()[0].arguments.size() == 8);
    for (std::size_t i = 0; i < 8; ++i) CHECK(backend.calls()[0].arguments[i] == 0x10 + i);
    CHECK(thread.regs()[0] == 0 && thread.regs()[1] == 0);

    // GLfloat arguments retain their 32-bit representation rather than being numerically cast.
    backend.clear_calls();
    const std::array<std::uint32_t, 12> float_words{
        std::bit_cast<std::uint32_t>(1.5f), std::bit_cast<std::uint32_t>(-2.25f),
        std::bit_cast<std::uint32_t>(0.0f), std::bit_cast<std::uint32_t>(8.5f)};
    set_words(runtime, thread, float_words);
    CHECK(host.handle_host_call(zb::ZB_GL_HC_glBlendColor, thread));
    CHECK(backend.calls().size() == 1 && backend.calls()[0].arguments.size() == 4);
    for (std::size_t i = 0; i < 4; ++i) CHECK(backend.calls()[0].arguments[i] == float_words[i]);

    // All pointerless return classes: GLenum, GLuint and GLboolean. GLint and pointer returns
    // belong to semantic pointer/string handlers and safely return zero in Task 1.
    backend.set_result("glCheckFramebufferStatus", 0x8CD5);
    set_words(runtime, thread, copy_words);
    CHECK(host.handle_host_call(zb::ZB_GL_HC_glCheckFramebufferStatus, thread));
    CHECK(thread.regs()[0] == 0x8CD5 && thread.regs()[1] == 0);

    backend.set_result("glCreateProgram", 0x12345678);
    set_words(runtime, thread, copy_words);
    CHECK(host.handle_host_call(zb::ZB_GL_HC_glCreateProgram, thread));
    CHECK(thread.regs()[0] == 0x12345678 && thread.regs()[1] == 0);

    backend.set_result("glIsBuffer", 0x101);
    set_words(runtime, thread, copy_words);
    CHECK(host.handle_host_call(zb::ZB_GL_HC_glIsBuffer, thread));
    CHECK(thread.regs()[0] == 1 && thread.regs()[1] == 0);

    // Registry len= shapes: literal, named parameter, product, and a parameter declared after
    // its pointer. Each guest address must become base + address without copying.
    backend.clear_calls();
    std::array<std::uint32_t, 12> pointer_words{3, kData + 0xFF0};
    set_words(runtime, thread, pointer_words);
    CHECK(host.handle_host_call(zb::ZB_GL_HC_glVertexAttrib4fv, thread));
    CHECK(backend.calls().size() == 1 && backend.calls()[0].name == "glVertexAttrib4fv");
    CHECK(backend.calls()[0].arguments[1] ==
          reinterpret_cast<std::uintptr_t>(runtime.memory().base() + kData + 0xFF0));

    backend.clear_calls();
    pointer_words = {2, kData + 0xFF8};
    set_words(runtime, thread, pointer_words);
    CHECK(host.handle_host_call(zb::ZB_GL_HC_glDeleteBuffers, thread));
    CHECK(backend.calls().size() == 1 && backend.calls()[0].name == "glDeleteBuffers");

    backend.clear_calls();
    pointer_words = {7, 2, 0, kData + 0xFB8};  // 2 * 9 floats ends at the page boundary.
    set_words(runtime, thread, pointer_words);
    CHECK(host.handle_host_call(zb::ZB_GL_HC_glUniformMatrix3fv, thread));
    CHECK(backend.calls().size() == 1 && backend.calls()[0].name == "glUniformMatrix3fv");

    backend.clear_calls();
    pointer_words = {2, kData, 0x8DF8, kData + 0x100, 5};
    set_words(runtime, thread, pointer_words);
    CHECK(host.handle_host_call(zb::ZB_GL_HC_glShaderBinary, thread));
    CHECK(backend.calls().size() == 1 && backend.calls()[0].name == "glShaderBinary");
    CHECK(backend.calls()[0].arguments[1] ==
          reinterpret_cast<std::uintptr_t>(runtime.memory().base() + kData));
    CHECK(backend.calls()[0].arguments[3] ==
          reinterpret_cast<std::uintptr_t>(runtime.memory().base() + kData + 0x100));

    // Bounded names are passed as direct guest-memory aliases.
    std::memcpy(runtime.memory().base() + kData + 0x200, "position", 9);
    backend.clear_calls();
    pointer_words = {4, 2, kData + 0x200};
    set_words(runtime, thread, pointer_words);
    CHECK(host.handle_host_call(zb::ZB_GL_HC_glBindAttribLocation, thread));
    CHECK(backend.calls().size() == 1 && backend.calls()[0].name == "glBindAttribLocation");

    // glShaderSource translates the guest uint32_t pointer array to host-width pointers.
    const std::uint32_t source_addresses[] = {kData + 0x240, kData + 0x260};
    std::memcpy(runtime.memory().base() + kData + 0x220, source_addresses,
                sizeof(source_addresses));
    std::memcpy(runtime.memory().base() + kData + 0x240, "one", 4);
    const char source_two[] = {'t', 'w', '\0', 'o', '\0'};
    std::memcpy(runtime.memory().base() + kData + 0x260, source_two, sizeof(source_two));
    const std::int32_t source_lengths[] = {-1, 4};
    std::memcpy(runtime.memory().base() + kData + 0x280, source_lengths,
                sizeof(source_lengths));
    backend.clear_calls();
    pointer_words = {9, 2, kData + 0x220, kData + 0x280};
    set_words(runtime, thread, pointer_words);
    CHECK(host.handle_host_call(zb::ZB_GL_HC_glShaderSource, thread));
    CHECK(backend.calls().size() == 1 && backend.calls()[0].name == "glShaderSource");
    CHECK(backend.shader_sources().strings.size() == 2);
    CHECK(backend.shader_sources().strings[0] == "one");
    CHECK(backend.shader_sources().strings[1] == std::string(source_two, 4));

    backend.clear_calls();
    pointer_words = {9, 0, 0, 0};
    set_words(runtime, thread, pointer_words);
    CHECK(host.handle_host_call(zb::ZB_GL_HC_glShaderSource, thread));
    CHECK(backend.calls().size() == 1 && backend.shader_sources().strings.empty());

    backend.clear_calls();
    backend.set_error(0);
    pointer_words = {9, 2, kData + 0xFFC, 0};
    set_words(runtime, thread, pointer_words);
    CHECK(host.handle_host_call(zb::ZB_GL_HC_glShaderSource, thread));
    CHECK(backend.calls().empty());
    CHECK(backend.error() == zb::kGlInvalidValue);

    // Driver strings are copied into guest memory once and cached by enum.
    backend.set_string(0x1F00, "Mock Vendor");
    pointer_words = {0x1F00};
    set_words(runtime, thread, pointer_words);
    CHECK(host.handle_host_call(zb::ZB_GL_HC_glGetString, thread));
    const std::uint32_t vendor = thread.regs()[0];
    CHECK(vendor != 0);
    CHECK(std::strcmp(reinterpret_cast<const char*>(runtime.memory().base() + vendor),
                      "Mock Vendor") == 0);
    set_words(runtime, thread, pointer_words);
    CHECK(host.handle_host_call(zb::ZB_GL_HC_glGetString, thread));
    CHECK(thread.regs()[0] == vendor);

    // Task 3 helpers: pname vector widths, padded pixel rows and lazy uniform sizing.
    backend.set_integer(0x86A2, 3);  // GL_NUM_COMPRESSED_TEXTURE_FORMATS
    CHECK(zb::gl_pname_count(backend, 0x0B21) == 1);  // GL_LINE_WIDTH
    CHECK(zb::gl_pname_count(backend, 0x846E) == 2);  // GL_ALIASED_LINE_WIDTH_RANGE
    CHECK(zb::gl_pname_count(backend, 0x0C22) == 4);  // GL_COLOR_CLEAR_VALUE
    CHECK(zb::gl_pname_count(backend, 0x86A3) == 3);  // GL_COMPRESSED_TEXTURE_FORMATS

    CHECK(zb::gl_pixel_bytes(0x1907, 0x1401, 1, 2, 4) == 7);   // RGB/U8: 3 + pad + 3
    CHECK(zb::gl_pixel_bytes(0x1907, 0x8363, 3, 2, 8) == 14);  // RGB/565: 6 + pad + 6
    CHECK(zb::gl_pixel_bytes(0x1908, 0x1401, 2, 2, 1) == 16);  // RGBA/U8
    CHECK(zb::gl_pixel_bytes(0x80E1, 0x1401, 2, 2, 1) == 16);  // BGRA_EXT/U8
    CHECK(!zb::gl_pixel_bytes(0x80E1, 0x8033, 1, 1, 4));       // BGRA_EXT/4444 is invalid
    CHECK(!zb::gl_pixel_bytes(0x1907, 0x8033, 1, 1, 4));       // RGB/4444 is invalid
    struct PixelCase {
        std::uint32_t format;
        std::uint32_t type;
        std::uint64_t bytes_per_pixel;
    };
    const PixelCase pixel_cases[] = {
        {0x1906, 0x1401, 1}, {0x1909, 0x1401, 1}, {0x190A, 0x1401, 2},
        {0x1907, 0x1401, 3}, {0x1908, 0x1401, 4}, {0x1907, 0x8363, 2},
        {0x1908, 0x8033, 2}, {0x1908, 0x8034, 2},
    };
    for (const PixelCase& pixel : pixel_cases) {
        for (const std::uint64_t alignment : {1u, 2u, 4u, 8u}) {
            const std::uint64_t row = 3 * pixel.bytes_per_pixel;
            const std::uint64_t stride = (row + alignment - 1) & ~(alignment - 1);
            CHECK(zb::gl_pixel_bytes(pixel.format, pixel.type, 3, 2,
                                     static_cast<std::int32_t>(alignment)) == stride + row);
        }
    }

    // PixelStore state selects row alignment for the semantic texture handler.
    pointer_words = {0x0CF5, 8};  // GL_UNPACK_ALIGNMENT
    set_words(runtime, thread, pointer_words);
    CHECK(host.handle_host_call(zb::ZB_GL_HC_glPixelStorei, thread));
    backend.clear_calls();
    pointer_words = {0x0DE1, 0, 0x1907, 1, 2, 0, 0x1907, 0x1401, kData + 0xFF5};
    set_words(runtime, thread, pointer_words);
    CHECK(host.handle_host_call(zb::ZB_GL_HC_glTexImage2D, thread));
    CHECK(backend.calls().size() == 1 && backend.calls()[0].name == "glTexImage2D");

    // A one-element COMPSIZE(pname) entry is generated rather than left as a stub.
    backend.clear_calls();
    pointer_words = {3, 0x8B81, kData + 0xFFC};  // GL_COMPILE_STATUS
    set_words(runtime, thread, pointer_words);
    CHECK(host.handle_host_call(zb::ZB_GL_HC_glGetShaderiv, thread));
    CHECK(backend.calls().size() == 1 && backend.calls()[0].name == "glGetShaderiv");

    backend.set_active_uniforms(7, {{"uColor", 1, 0x8B52, 5}});  // GL_FLOAT_VEC4
    backend.clear_calls();
    pointer_words = {7, 5, kData + 0xFF0};
    set_words(runtime, thread, pointer_words);
    CHECK(host.handle_host_call(zb::ZB_GL_HC_glGetUniformfv, thread));
    CHECK(!backend.calls().empty() && backend.calls().back().name == "glGetUniformfv");

    backend.set_active_uniforms(7, {{"uScalar", 1, 0x1406, 9}});  // GL_FLOAT
    pointer_words = {7};
    set_words(runtime, thread, pointer_words);
    CHECK(host.handle_host_call(zb::ZB_GL_HC_glLinkProgram, thread));
    backend.clear_calls();
    pointer_words = {7, 9, kData + 0xFFC};
    set_words(runtime, thread, pointer_words);
    CHECK(host.handle_host_call(zb::ZB_GL_HC_glGetUniformfv, thread));
    CHECK(!backend.calls().empty() && backend.calls().back().name == "glGetUniformfv");

    backend.clear_calls();
    backend.set_error(0);
    pointer_words = {7, 99, kData + 0xFF0};
    set_words(runtime, thread, pointer_words);
    CHECK(host.handle_host_call(zb::ZB_GL_HC_glGetUniformfv, thread));
    CHECK(backend.error() == zb::kGlInvalidOperation);
    CHECK(backend.calls().empty() || backend.calls().back().name != "glGetUniformfv");

    backend.set_error(0);
    std::memset(runtime.memory().base() + kData + 0xF00, 'x', 0x100);
    pointer_words = {7, kData + 0xF00};
    set_words(runtime, thread, pointer_words);
    const std::size_t before_manual = backend.calls().size();
    CHECK(host.handle_host_call(zb::ZB_GL_HC_glGetAttribLocation, thread));
    CHECK(backend.calls().size() == before_manual);
    CHECK(backend.error() == zb::kGlInvalidValue);
    CHECK(thread.regs()[0] == 0 && thread.regs()[1] == 0);

    // GLES 3.0: glGetStringi copies the driver string into guest memory and caches it, exactly
    // like glGetString; the guest never sees the driver's pointer.
    backend.clear_calls();
    static const char kExtension[] = "GL_OES_mock_extension";
    backend.set_result("glGetStringi", reinterpret_cast<std::uint64_t>(kExtension));
    backend.set_integer(0x821D, 3);  // GL_NUM_EXTENSIONS
    pointer_words = {0x1F03, 2};  // GL_EXTENSIONS
    set_words(runtime, thread, pointer_words);
    CHECK(host.handle_host_call(zb::ZB_GL_HC_glGetStringi, thread));
    const std::uint32_t extension = thread.regs()[0];
    CHECK(extension != 0 && extension != reinterpret_cast<std::uintptr_t>(kExtension));
    CHECK(std::strcmp(reinterpret_cast<const char*>(runtime.memory().base() + extension),
                      kExtension) == 0);
    // The driver is read while the extension list is built, and not again afterwards.
    const std::size_t after_list = backend.calls().size();
    CHECK(after_list >= 1);
    set_words(runtime, thread, pointer_words);
    CHECK(host.handle_host_call(zb::ZB_GL_HC_glGetStringi, thread));
    CHECK(thread.regs()[0] == extension && backend.calls().size() == after_list);

    // An extension this bridge cannot serve is dropped from what the guest is told, because a
    // guest that finds it in the list calls its entry points without checking that they exist.
    // GL_NUM_EXTENSIONS must agree with the filtered list, or the guest reads past its end.
    pointer_words = {0x821D, kData};  // GL_NUM_EXTENSIONS
    set_words(runtime, thread, pointer_words);
    CHECK(host.handle_host_call(zb::ZB_GL_HC_glGetIntegerv, thread));
    CHECK(*reinterpret_cast<const std::int32_t*>(runtime.memory().base() + kData) == 3);
    {
        MockGles filtered_backend;
        std::uint32_t filtered_allocation = kData + 0x400;
        zb::HostGl filtered(
            runtime, filtered_backend,
            [&](std::size_t size) -> std::optional<std::uint32_t> {
                const std::uint32_t result = filtered_allocation;
                filtered_allocation += static_cast<std::uint32_t>((size + 7) & ~std::size_t{7});
                if (filtered_allocation > kData + 0x7F0) return std::nullopt;
                return result;
            },
            [] { return std::uintptr_t{0xC0FFEE}; });
        filtered_backend.set_integer(0x821D, 2);
        static const char kDropped[] = "GL_KHR_debug";
        filtered_backend.set_result("glGetStringi", reinterpret_cast<std::uint64_t>(kDropped));
        pointer_words = {0x821D, kData};
        set_words(runtime, thread, pointer_words);
        CHECK(filtered.handle_host_call(zb::ZB_GL_HC_glGetIntegerv, thread));
        CHECK(*reinterpret_cast<const std::int32_t*>(runtime.memory().base() + kData) == 0);
        pointer_words = {0x1F03, 0};
        set_words(runtime, thread, pointer_words);
        CHECK(filtered.handle_host_call(zb::ZB_GL_HC_glGetStringi, thread));
        CHECK(filtered_backend.error() == zb::kGlInvalidValue);
    }

    // GLES 3.0 mapped buffers are mirrored: a driver mapping is host memory outside the guest
    // space, so the guest gets a copy that is written back on unmap.
    std::array<std::uint8_t, 64> driver_range;
    driver_range.fill(0xA5);
    backend.clear_calls();
    backend.set_result("glMapBufferRange", reinterpret_cast<std::uint64_t>(driver_range.data()));
    backend.set_result("glUnmapBuffer", 1);
    pointer_words = {0x8892, 0, static_cast<std::uint32_t>(driver_range.size()), 0x0003};
    set_words(runtime, thread, pointer_words);
    CHECK(host.handle_host_call(zb::ZB_GL_HC_glMapBufferRange, thread));
    const std::uint32_t mapped = thread.regs()[0];
    CHECK(mapped != 0 && mapped + driver_range.size() < kData + 0x1000);
    CHECK(std::memcmp(runtime.memory().base() + mapped, driver_range.data(),
                      driver_range.size()) == 0);

    // While the range is mapped, glGetBufferPointerv reports the mirror, never the host pointer.
    backend.clear_calls();
    pointer_words = {0x8892, 0x88BD, kData + 0xFF8};  // GL_BUFFER_MAP_POINTER
    set_words(runtime, thread, pointer_words);
    CHECK(host.handle_host_call(zb::ZB_GL_HC_glGetBufferPointerv, thread));
    std::uint32_t reported = 0;
    std::memcpy(&reported, runtime.memory().base() + kData + 0xFF8, sizeof(reported));
    CHECK(reported == mapped);

    // What the guest wrote into the mirror reaches the driver's memory on unmap.
    std::memset(runtime.memory().base() + mapped, 0x5A, driver_range.size());
    backend.clear_calls();
    pointer_words = {0x8892};
    set_words(runtime, thread, pointer_words);
    CHECK(host.handle_host_call(zb::ZB_GL_HC_glUnmapBuffer, thread));
    CHECK(thread.regs()[0] == 1);
    CHECK(backend.calls().size() == 1 && backend.calls()[0].name == "glUnmapBuffer");
    CHECK(std::count(driver_range.begin(), driver_range.end(), 0x5A) ==
          static_cast<long>(driver_range.size()));

    set_words(runtime, thread, pointer_words);
    pointer_words = {0x8892, 0x88BD, kData + 0xFF8};
    set_words(runtime, thread, pointer_words);
    CHECK(host.handle_host_call(zb::ZB_GL_HC_glGetBufferPointerv, thread));
    std::memcpy(&reported, runtime.memory().base() + kData + 0xFF8, sizeof(reported));
    CHECK(reported == 0);

    // An invalidated mapping is not copied in: the driver's old contents stay out of the guest.
    driver_range.fill(0x11);
    std::memset(runtime.memory().base() + kData + 0x700, 0, 0x40);
    backend.clear_calls();
    pointer_words = {0x8892, 0, static_cast<std::uint32_t>(driver_range.size()), 0x0006};
    set_words(runtime, thread, pointer_words);  // GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_RANGE_BIT
    CHECK(host.handle_host_call(zb::ZB_GL_HC_glMapBufferRange, thread));
    const std::uint32_t invalidated = thread.regs()[0];
    CHECK(invalidated != 0);
    CHECK(std::memcmp(runtime.memory().base() + invalidated, driver_range.data(),
                      driver_range.size()) != 0);
    pointer_words = {0x8892};
    set_words(runtime, thread, pointer_words);
    CHECK(host.handle_host_call(zb::ZB_GL_HC_glUnmapBuffer, thread));

    // GLES extension entry points (eglGetProcAddress names guests call without a null check).
    // EXT_multisampled_render_to_texture: both take only scalars, the shape that killed the
    // Flutter guest (glFramebufferTexture2DMultisampleEXT with samples on the stack).
    backend.clear_calls();
    pointer_words = {0x8D41, 4, 0x8058, 640, 480};  // GL_RENDERBUFFER, 4x, GL_RGBA8
    set_words(runtime, thread, pointer_words);
    CHECK(host.handle_host_call(zb::ZB_GL_HC_glRenderbufferStorageMultisampleEXT, thread));
    CHECK(backend.calls().size() == 1);
    CHECK(backend.calls()[0].name == "glRenderbufferStorageMultisampleEXT");
    CHECK(backend.calls()[0].arguments.size() == 5);
    CHECK(backend.calls()[0].arguments[1] == 4 && backend.calls()[0].arguments[4] == 480);

    backend.clear_calls();
    pointer_words = {0x8D40, 0x8CE0, 0x0DE1, 7, 0, 4};  // FRAMEBUFFER, COLOR_ATTACHMENT0, TEXTURE_2D
    set_words(runtime, thread, pointer_words);
    CHECK(host.handle_host_call(zb::ZB_GL_HC_glFramebufferTexture2DMultisampleEXT, thread));
    CHECK(backend.calls().size() == 1);
    CHECK(backend.calls()[0].name == "glFramebufferTexture2DMultisampleEXT");
    CHECK(backend.calls()[0].arguments.size() == 6);
    CHECK(backend.calls()[0].arguments[3] == 7);
    // level and samples come off the guest stack, not r0-r3.
    CHECK(backend.calls()[0].arguments[4] == 0 && backend.calls()[0].arguments[5] == 4);

    // EXT_discard_framebuffer: a counted attachment array is translated like any other.
    const std::uint32_t attachments[] = {0x8CE0, 0x8D00};
    std::memcpy(runtime.memory().base() + kData + 0x300, attachments, sizeof(attachments));
    backend.clear_calls();
    pointer_words = {0x8D40, 2, kData + 0x300};
    set_words(runtime, thread, pointer_words);
    CHECK(host.handle_host_call(zb::ZB_GL_HC_glDiscardFramebufferEXT, thread));
    CHECK(backend.calls().size() == 1 && backend.calls()[0].name == "glDiscardFramebufferEXT");
    CHECK(backend.calls()[0].arguments[2] ==
          reinterpret_cast<std::uintptr_t>(runtime.memory().base() + kData + 0x300));

    // OES_vertex_array_object: the whole set, including the GLboolean return.
    backend.clear_calls();
    pointer_words = {11};
    set_words(runtime, thread, pointer_words);
    CHECK(host.handle_host_call(zb::ZB_GL_HC_glBindVertexArrayOES, thread));
    CHECK(backend.calls().size() == 1 && backend.calls()[0].name == "glBindVertexArrayOES");
    CHECK(backend.calls()[0].arguments.size() == 1 && backend.calls()[0].arguments[0] == 11);

    backend.clear_calls();
    pointer_words = {2, kData + 0x310};
    set_words(runtime, thread, pointer_words);
    CHECK(host.handle_host_call(zb::ZB_GL_HC_glGenVertexArraysOES, thread));
    CHECK(backend.calls().size() == 1 && backend.calls()[0].name == "glGenVertexArraysOES");
    CHECK(backend.calls()[0].arguments[1] ==
          reinterpret_cast<std::uintptr_t>(runtime.memory().base() + kData + 0x310));

    backend.clear_calls();
    set_words(runtime, thread, pointer_words);
    CHECK(host.handle_host_call(zb::ZB_GL_HC_glDeleteVertexArraysOES, thread));
    CHECK(backend.calls().size() == 1 && backend.calls()[0].name == "glDeleteVertexArraysOES");

    backend.clear_calls();
    backend.set_result("glIsVertexArrayOES", 0x101);
    pointer_words = {11};
    set_words(runtime, thread, pointer_words);
    CHECK(host.handle_host_call(zb::ZB_GL_HC_glIsVertexArrayOES, thread));
    CHECK(thread.regs()[0] == 1 && thread.regs()[1] == 0);

    // A guest pointer that is not accessible is refused before the driver is called.
    backend.clear_calls();
    backend.set_error(0);
    pointer_words = {4, kData + 0xFFC};  // 4 GLuints do not fit before the page end
    set_words(runtime, thread, pointer_words);
    CHECK(host.handle_host_call(zb::ZB_GL_HC_glGenVertexArraysOES, thread));
    CHECK(backend.calls().empty() && backend.error() == zb::kGlInvalidValue);

    // EXT_texture_storage.
    backend.clear_calls();
    pointer_words = {0x0DE1, 3, 0x8058, 64, 32};
    set_words(runtime, thread, pointer_words);
    CHECK(host.handle_host_call(zb::ZB_GL_HC_glTexStorage2DEXT, thread));
    CHECK(backend.calls().size() == 1 && backend.calls()[0].name == "glTexStorage2DEXT");

    backend.clear_calls();
    pointer_words = {0x806F, 3, 0x8058, 64, 32, 8};
    set_words(runtime, thread, pointer_words);
    CHECK(host.handle_host_call(zb::ZB_GL_HC_glTexStorage3DEXT, thread));
    CHECK(backend.calls().size() == 1 && backend.calls()[0].name == "glTexStorage3DEXT");
    CHECK(backend.calls()[0].arguments.size() == 6 && backend.calls()[0].arguments[5] == 8);

    // OES_mapbuffer mirrors the whole data store exactly like the GLES 3 glMapBufferRange path:
    // the driver's pointer never reaches the guest, and the mirror is written back on unmap.
    std::array<std::uint8_t, 32> oes_range;
    oes_range.fill(0x3C);
    backend.clear_calls();
    backend.set_integer(0x8764, static_cast<zb::GLint>(oes_range.size()));  // GL_BUFFER_SIZE
    backend.set_result("glMapBufferOES", reinterpret_cast<std::uint64_t>(oes_range.data()));
    backend.set_result("glUnmapBufferOES", 1);
    pointer_words = {0x8892, 0x88B9};  // GL_ARRAY_BUFFER, GL_WRITE_ONLY_OES
    set_words(runtime, thread, pointer_words);
    CHECK(host.handle_host_call(zb::ZB_GL_HC_glMapBufferOES, thread));
    const std::uint32_t oes_mapped = thread.regs()[0];
    CHECK(oes_mapped != 0);
    CHECK(oes_mapped != reinterpret_cast<std::uintptr_t>(oes_range.data()));
    // A whole-buffer map keeps the data store's contents, so they are copied into the mirror.
    CHECK(std::memcmp(runtime.memory().base() + oes_mapped, oes_range.data(),
                      oes_range.size()) == 0);

    backend.clear_calls();
    pointer_words = {0x8892, 0x88BD, kData + 0xFF8};  // GL_BUFFER_MAP_POINTER
    set_words(runtime, thread, pointer_words);
    CHECK(host.handle_host_call(zb::ZB_GL_HC_glGetBufferPointervOES, thread));
    std::memcpy(&reported, runtime.memory().base() + kData + 0xFF8, sizeof(reported));
    CHECK(reported == oes_mapped);
    CHECK(backend.calls().empty());  // answered from the mirror, never from the driver

    std::memset(runtime.memory().base() + oes_mapped, 0xC3, oes_range.size());
    backend.clear_calls();
    pointer_words = {0x8892};
    set_words(runtime, thread, pointer_words);
    CHECK(host.handle_host_call(zb::ZB_GL_HC_glUnmapBufferOES, thread));
    CHECK(thread.regs()[0] == 1);
    CHECK(backend.calls().size() == 1 && backend.calls()[0].name == "glUnmapBufferOES");
    CHECK(std::count(oes_range.begin(), oes_range.end(), 0xC3) ==
          static_cast<long>(oes_range.size()));

    // Unmapped again: the mirror is gone and the target maps cleanly a second time.
    backend.clear_calls();
    pointer_words = {0x8892, 0x88BD, kData + 0xFF8};
    set_words(runtime, thread, pointer_words);
    CHECK(host.handle_host_call(zb::ZB_GL_HC_glGetBufferPointervOES, thread));
    std::memcpy(&reported, runtime.memory().base() + kData + 0xFF8, sizeof(reported));
    CHECK(reported == 0);

    // An access enum OES_mapbuffer does not define is refused before the driver is called.
    backend.clear_calls();
    backend.set_error(0);
    pointer_words = {0x8892, 0x1234};
    set_words(runtime, thread, pointer_words);
    CHECK(host.handle_host_call(zb::ZB_GL_HC_glMapBufferOES, thread));
    CHECK(backend.calls().empty() && backend.error() == 0x0500);  // GL_INVALID_ENUM
    CHECK(thread.regs()[0] == 0);

    // The bounded device diagnostics must show whether bytes written through a guest mirror
    // reached the real driver mapping, and identify the context/buffer owning that mapping.
    // Without these three records, a missing Flutter uniform cannot be localized to map, flush,
    // or unmap.
    zb::runtime_report().clear();
    zb::enable_gl_diagnostics();
    std::array<std::uint8_t, 16> diagnosed_range;
    diagnosed_range.fill(0x11);
    backend.clear_calls();
    backend.set_integer(0x8894, 77);  // GL_ARRAY_BUFFER_BINDING
    backend.set_result("glMapBufferRange",
                       reinterpret_cast<std::uint64_t>(diagnosed_range.data()));
    backend.set_result("glUnmapBuffer", 1);
    pointer_words = {0x8892, 0, static_cast<std::uint32_t>(diagnosed_range.size()), 0x0013};
    set_words(runtime, thread, pointer_words);  // READ | WRITE | FLUSH_EXPLICIT
    CHECK(host.handle_host_call(zb::ZB_GL_HC_glMapBufferRange, thread));
    const std::uint32_t diagnosed_mirror = thread.regs()[0];
    CHECK(diagnosed_mirror != 0);

    // A process-wide target-only collision is visible with both contexts/buffers instead of
    // looking like an unexplained GL_INVALID_OPERATION on a second rendering context.
    set_words(runtime, thread, pointer_words);
    CHECK(host.handle_host_call(zb::ZB_GL_HC_glMapBufferRange, thread));
    CHECK(thread.regs()[0] == 0);
    std::memset(runtime.memory().base() + diagnosed_mirror + 4, 0x5A, 8);

    pointer_words = {0x8892, 4, 8};
    set_words(runtime, thread, pointer_words);
    CHECK(host.handle_host_call(zb::ZB_GL_HC_glFlushMappedBufferRange, thread));
    pointer_words = {0x8892};
    set_words(runtime, thread, pointer_words);
    CHECK(host.handle_host_call(zb::ZB_GL_HC_glUnmapBuffer, thread));

    const std::string mapping_report = zb::runtime_report().text();
    CHECK(mapping_report.find("gl-map-1: context=0xc0ffee target=0x8892 buffer=77 offset=0 "
                              "length=16 access=0x13 guest=0x") != std::string::npos);
    CHECK(mapping_report.find("mirror-fnv=0c50476a8dc2d715 "
                              "driver-fnv=0c50476a8dc2d715") != std::string::npos);
    CHECK(mapping_report.find("gl-map-flush-1: map=1 offset=4 length=8 "
                              "mirror-fnv=65c229a27a840fe5 driver-fnv=65c229a27a840fe5") !=
          std::string::npos);
    CHECK(mapping_report.find("gl-map-unmap-1: map=1 length=16 "
                              "mirror-fnv=b39844b9e6cdebed driver-fnv=b39844b9e6cdebed") !=
          std::string::npos);
    CHECK(mapping_report.find("gl-map-collision-1: context=0xc0ffee target=0x8892 buffer=77 "
                              "existing-map=1 existing-context=0xc0ffee existing-buffer=77") !=
          std::string::npos);

    // Impeller uploads a combined array/uniform buffer through GL_ARRAY_BUFFER, then binds
    // slices of that same object as UBOs. Preserve enough of the upload to correlate a later
    // glBindBufferRange with the exact bytes that fed its std140 block.
    zb::runtime_report().clear();
    backend.set_integer(0x8894, 91);  // GL_ARRAY_BUFFER_BINDING
    pointer_words = {0x8892, 64, 0, 0x88E8};  // orphan a 64-byte GL_ARRAY_BUFFER
    set_words(runtime, thread, pointer_words);
    CHECK(host.handle_host_call(zb::ZB_GL_HC_glBufferData, thread));
    for (std::uint32_t i = 0; i < 64; ++i) runtime.memory().base()[kData + 0x600 + i] = i;
    pointer_words = {0x8892, 0, 64, kData + 0x600};
    set_words(runtime, thread, pointer_words);
    CHECK(host.handle_host_call(zb::ZB_GL_HC_glBufferSubData, thread));
    pointer_words = {0x8A11, 0, 91, 16, 16};  // GL_UNIFORM_BUFFER, binding 0
    set_words(runtime, thread, pointer_words);
    CHECK(host.handle_host_call(zb::ZB_GL_HC_glBindBufferRange, thread));

    const std::string upload_report = zb::runtime_report().text();
    CHECK(upload_report.find("gl-buffer-upload-1: target=0x8892 buffer=91 offset=0 size=64 "
                             "fnv=8368214f77995ee5") != std::string::npos);
    CHECK(upload_report.find("gl-ubo-bind-1-data: buffer=91 offset=16 size=16 captured=yes "
                             "fnv=f091c81ae28d2c75 head=101112131415161718191a1b1c1d1e1f") !=
          std::string::npos);

    // GLES3 does not accept the GLES2 legacy alpha format as a sized texture allocation on all
    // drivers. Preserve the driver's error for the guest while making it visible in the report.
    backend.set_result("glGetError", 0x0500);
    pointer_words = {0x0DE1, 0, 0x1906, 1, 1, 0, 0x1906, 0x1401, kData + 0x700};
    set_words(runtime, thread, pointer_words);
    CHECK(host.handle_host_call(zb::ZB_GL_HC_glTexImage2D, thread));
    const std::string legacy_report = zb::runtime_report().text();
    CHECK(legacy_report.find("gl-legacy-texture-error: internal=0x1906 format=0x1906 "
                             "error=0x500") != std::string::npos);
    CHECK(backend.error() == 0x0500);

    // The asset range is deliberately not swallowed by HostGl.
    CHECK(!host.handle_host_call(142, thread));
    CHECK(!host.handle_host_call(UINT32_MAX, thread));

    std::puts("gles_marshal_test PASS");
    return 0;
}
