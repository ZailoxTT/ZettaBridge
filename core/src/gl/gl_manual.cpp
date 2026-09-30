#include "zb/host_gl.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "gl/gl_diagnostics.h"
#include "zb/log.h"

namespace zb {

namespace {

constexpr GLenum kGlPackAlignment = 0x0D05;
constexpr GLenum kGlUnpackAlignment = 0x0CF5;
constexpr GLenum kGlActiveUniforms = 0x8B86;
constexpr GLenum kGlActiveUniformMaxLength = 0x8B87;
constexpr GLenum kGlNumCompressedTextureFormats = 0x86A2;
constexpr GLenum kGlCompressedTextureFormats = 0x86A3;
constexpr GLenum kGlNumShaderBinaryFormats = 0x8DF9;
constexpr GLenum kGlShaderBinaryFormats = 0x8DF8;
constexpr GLenum kGlArrayBuffer = 0x8892;
constexpr GLenum kGlElementArrayBuffer = 0x8893;
constexpr GLenum kGlPixelPackBuffer = 0x88EB;
constexpr GLenum kGlPixelUnpackBuffer = 0x88EC;
constexpr GLenum kGlBufferMapPointer = 0x88BD;
constexpr GLenum kGlUniformBlockActiveUniforms = 0x8A42;
constexpr GLenum kGlUniformBlockActiveUniformIndices = 0x8A43;
// glMapBufferRange access bits.
constexpr GLbitfield kGlMapRead = 0x0001;
constexpr GLbitfield kGlMapWrite = 0x0002;
constexpr GLbitfield kGlMapInvalidateRange = 0x0004;
constexpr GLbitfield kGlMapInvalidateBuffer = 0x0008;

struct GlThreadState {
    struct Attribute {
        bool defined = false;
        bool enabled = false;
        GLint size = 4;
        GLenum type = 0x1406;
        GLboolean normalized = 0;
        GLsizei stride = 0;
        std::uint32_t guest_pointer = 0;
        GLuint buffer = 0;
        // GLES 3.0 glVertexAttribIPointer: the attribute must be re-issued through the integer
        // entry point when its client array is materialized.
        bool integer = false;
    };

    const HostGl* owner = nullptr;
    GLint pack_alignment = 4;
    GLint unpack_alignment = 4;
    GLuint array_buffer = 0;
    GLuint element_array_buffer = 0;
    GLuint pixel_pack_buffer = 0;
    GLuint pixel_unpack_buffer = 0;
    std::unordered_map<GLuint, Attribute> attributes;
    std::unordered_map<GLuint, std::unordered_map<GLint, std::uint64_t>> uniforms;
    std::unordered_map<GLenum, std::uint32_t> strings;
    std::unordered_map<std::uint64_t, std::uint32_t> indexed_strings;
    // The driver's extensions minus the ones this bridge cannot serve, in driver order.
    std::vector<std::string> extensions;
    bool extensions_known = false;
};

constexpr GLenum kGlExtensions = 0x1F03;
constexpr GLenum kGlNumExtensions = 0x821D;

// Extensions whose entry points this bridge does not serve. A driver advertises what the hardware
// can do, and an engine that reads an extension out of the string calls its functions without
// checking what eglGetProcAddress returned: Unity jumps straight into a null pointer that way.
// Dropping them from the string is what a driver without the extension already looks like.
constexpr std::string_view kUnsupportedExtensions[] = {
#define ZB_GL_UNSUPPORTED_EXTENSION(name) name,
#include "gen/gl_unsupported_extensions.inc"
#undef ZB_GL_UNSUPPORTED_EXTENSION
};

bool extension_served(std::string_view name) {
    // The generated list is sorted, so the lookup is a binary search.
    return !std::binary_search(std::begin(kUnsupportedExtensions), std::end(kUnsupportedExtensions), name);
}

thread_local GlThreadState t_state;

GlThreadState& state(const HostGl& host) {
    if (t_state.owner != &host) {
        t_state = {};
        t_state.owner = &host;
    }
    return t_state;
}

// The driver's extensions this bridge can serve, in the order the driver lists them. GLES 3.0
// enumerates them one by one; a GLES 2.0 context has only the single space-separated string.
const std::vector<std::string>& served_extensions(HostGl& host) {
    GlThreadState& current = state(host);
    if (current.extensions_known) return current.extensions;
    current.extensions_known = true;

    GLint count = 0;
    host.backend().glGetIntegerv(kGlNumExtensions, &count);
    if (count > 0) {
        for (GLint i = 0; i < count; ++i) {
            const GLubyte* entry = host.backend().glGetStringi(kGlExtensions, static_cast<GLuint>(i));
            if (entry == nullptr) continue;
            const std::string name(reinterpret_cast<const char*>(entry));
            if (extension_served(name)) current.extensions.push_back(name);
        }
        return current.extensions;
    }
    const GLubyte* all = host.backend().glGetString(kGlExtensions);
    if (all == nullptr) return current.extensions;
    std::string_view rest(reinterpret_cast<const char*>(all));
    while (!rest.empty()) {
        const std::size_t space = rest.find(' ');
        const std::string_view name = rest.substr(0, space);
        if (!name.empty() && extension_served(name)) current.extensions.emplace_back(name);
        if (space == std::string_view::npos) break;
        rest.remove_prefix(space + 1);
    }
    return current.extensions;
}

// Copies a host string into guest memory and answers the call with its guest address.
std::uint32_t answer_with_string(HostGl& host, HostGl::Call& call, std::string_view text) {
    const auto address = host.allocate_guest(text.size() + 1);
    std::uint8_t* destination =
        address ? host.runtime().memory().host_ptr(*address, text.size() + 1, kPageRead | kPageWrite) : nullptr;
    if (destination == nullptr) {
        host.reject(call, kGlOutOfMemory, "guest allocation for driver string failed");
        return 0;
    }
    std::memcpy(destination, text.data(), text.size());
    destination[text.size()] = 0;
    call.set_result(*address);
    return *address;
}


std::uint64_t uniform_components(GLenum type) {
    switch (type) {
    case 0x1404:  // GL_INT
    case 0x1406:  // GL_FLOAT
    case 0x8B56:  // GL_BOOL
    case 0x8B5E:  // GL_SAMPLER_2D
    case 0x8B60:  // GL_SAMPLER_CUBE
        return 1;
    case 0x8B50:  // GL_FLOAT_VEC2
    case 0x8B53:  // GL_INT_VEC2
    case 0x8B57:  // GL_BOOL_VEC2
        return 2;
    case 0x8B51:  // GL_FLOAT_VEC3
    case 0x8B54:  // GL_INT_VEC3
    case 0x8B58:  // GL_BOOL_VEC3
        return 3;
    case 0x8B52:  // GL_FLOAT_VEC4
    case 0x8B55:  // GL_INT_VEC4
    case 0x8B59:  // GL_BOOL_VEC4
    case 0x8B5A:  // GL_FLOAT_MAT2
        return 4;
    case 0x8B5B:  // GL_FLOAT_MAT3
        return 9;
    case 0x8B5C:  // GL_FLOAT_MAT4
        return 16;
    default:
        return 0;
    }
}

bool checked_multiply(std::uint64_t a, std::uint64_t b, std::uint64_t& result) {
    if (a != 0 && b > std::numeric_limits<std::uint64_t>::max() / a) return false;
    result = a * b;
    return true;
}

template <typename T>
bool serve_pname(HostGl& host, HostGl::Call& call, void (GlBackend::*function)(GLenum, T*)) {
    const GLenum pname = call.scalar<GLenum>(0);
    const std::uint64_t count = gl_pname_count(host.backend(), pname);
    T* data = call.pointer<T>(1, count, kPageRead | kPageWrite);
    if (!call.valid()) return true;
    (host.backend().*function)(pname, data);
    return true;
}

// The pixel argument of a texture or read-back call. With a GLES 3.0 pixel buffer object bound
// the argument is a byte offset into that buffer and is passed through untouched; otherwise it is
// a guest address whose whole image (depth images of height padded rows) must be accessible.
bool pixel_pointer(HostGl& host, HostGl::Call& call, bool pack, GLenum format, GLenum type,
                   GLsizei width, GLsizei height, GLsizei depth, GLint alignment,
                   unsigned position, std::uint8_t permission, void*& out) {
    if (host.pixel_buffer(pack) != 0) {
        out = reinterpret_cast<void*>(static_cast<std::uintptr_t>(call.arg(position)));
        return call.valid();
    }
    if (depth < 0) {
        call.fail(kGlInvalidValue, "image depth is negative");
        return false;
    }
    std::uint64_t rows = 0;
    if (!checked_multiply(static_cast<std::uint64_t>(height), static_cast<std::uint64_t>(depth),
                          rows) ||
        rows > static_cast<std::uint64_t>(std::numeric_limits<GLsizei>::max())) {
        call.fail(kGlInvalidValue, "image dimensions overflowed");
        return false;
    }
    // Images are contiguous, so depth images of height padded rows measure exactly as one
    // height*depth tall image: only the very last row is unpadded.
    const auto bytes = gl_pixel_bytes(format, type, width, static_cast<GLsizei>(rows), alignment);
    if (!bytes) {
        // Naming the pair is the whole point: a rejected upload is a texture that stays black,
        // and which format and type were refused decides what to add. Orange Roulette's black
        // screen was one such pair (GL_BGRA_EXT), found the same way.
        char reason[128];
        std::snprintf(reason, sizeof reason,
                      "pixel format 0x%x type 0x%x %dx%d alignment %d is not accepted",
                      static_cast<unsigned>(format), static_cast<unsigned>(type), width,
                      static_cast<int>(rows), static_cast<int>(alignment));
        call.fail(kGlInvalidValue, reason);
        return false;
    }
    out = call.pointer<void>(position, *bytes, permission);
    return call.valid();
}

const GLchar* guest_string(HostGl& host, HostGl::Call& call, std::uint32_t address) {
    constexpr std::uint64_t kMaxGuestString = 64u << 20;
    if (address == 0) {
        call.fail(kGlInvalidValue, "string pointer is null");
        return nullptr;
    }
    std::uint64_t scanned = 0;
    while (scanned < kMaxGuestString) {
        const std::uint64_t current = static_cast<std::uint64_t>(address) + scanned;
        if (current >= kGuestSpaceSize) break;
        const std::uint64_t page_left = kPageSize - (current & kPageMask);
        const std::uint64_t chunk = std::min({page_left, kMaxGuestString - scanned,
                                              kGuestSpaceSize - current});
        const std::uint8_t* data = host.runtime().memory().host_ptr(
            static_cast<std::uint32_t>(current), chunk, kPageRead);
        if (data == nullptr) break;
        if (std::memchr(data, 0, static_cast<std::size_t>(chunk)) != nullptr) {
            return reinterpret_cast<const GLchar*>(host.runtime().memory().base() + address);
        }
        scanned += chunk;
    }
    call.fail(kGlInvalidValue, "string is unreadable or not terminated within 64 MiB");
    return nullptr;
}

std::uint64_t attribute_component_bytes(GLenum type) {
    switch (type) {
    case 0x1400:  // GL_BYTE
    case 0x1401:  // GL_UNSIGNED_BYTE
        return 1;
    case 0x1402:  // GL_SHORT
    case 0x1403:  // GL_UNSIGNED_SHORT
        return 2;
    case 0x140B:  // GL_HALF_FLOAT
        return 2;
    case 0x1404:  // GL_INT
    case 0x1405:  // GL_UNSIGNED_INT
    case 0x1406:  // GL_FLOAT
    case 0x140C:  // GL_FIXED
        return 4;
    default:
        return 0;
    }
}

// The byte width of one index of a glDrawElements-family type, or 0 when the type is invalid.
std::uint64_t element_index_bytes(GLenum type) {
    switch (type) {
    case 0x1401:  // GL_UNSIGNED_BYTE
        return 1;
    case 0x1403:  // GL_UNSIGNED_SHORT
        return 2;
    case 0x1405:  // GL_UNSIGNED_INT (GLES 3.0)
        return 4;
    default:
        return 0;
    }
}

bool materialize_client_arrays(HostGl& host, HostGl::Call& call, std::uint64_t first,
                               std::uint64_t last) {
    struct MaterializedAttribute {
        GLuint index;
        const GlThreadState::Attribute* attribute;
        const void* pointer;
    };

    GlThreadState& current = state(host);
    std::vector<MaterializedAttribute> materialized;
    for (const auto& [index, attribute] : current.attributes) {
        if (!attribute.enabled || !attribute.defined || attribute.buffer != 0) continue;
        const std::uint64_t component = attribute_component_bytes(attribute.type);
        if (attribute.size < 1 || attribute.size > 4 || component == 0 || attribute.stride < 0) {
            call.fail(kGlInvalidOperation, "client vertex attribute layout is invalid");
            return false;
        }
        const std::uint64_t element = static_cast<std::uint64_t>(attribute.size) * component;
        const std::uint64_t stride = attribute.stride == 0
                                         ? element
                                         : static_cast<std::uint64_t>(attribute.stride);
        std::uint64_t first_offset = 0;
        std::uint64_t last_offset = 0;
        if (!checked_multiply(first, stride, first_offset) ||
            !checked_multiply(last, stride, last_offset) ||
            last_offset > std::numeric_limits<std::uint64_t>::max() - element) {
            call.fail(kGlInvalidOperation, "client vertex attribute range overflowed");
            return false;
        }
        const std::uint64_t range_start =
            static_cast<std::uint64_t>(attribute.guest_pointer) + first_offset;
        const std::uint64_t range_end =
            static_cast<std::uint64_t>(attribute.guest_pointer) + last_offset + element;
        if (range_start >= kGuestSpaceSize || range_end > kGuestSpaceSize ||
            range_end < range_start ||
            host.runtime().memory().host_ptr(static_cast<std::uint32_t>(range_start),
                                             range_end - range_start, kPageRead) == nullptr) {
            call.fail(kGlInvalidOperation, "client vertex attribute range is unreadable");
            return false;
        }
        const void* pointer = host.runtime().memory().base() + attribute.guest_pointer;
        materialized.push_back({index, &attribute, pointer});
    }

    const bool rebound = !materialized.empty() && current.array_buffer != 0;
    if (rebound) host.backend().glBindBuffer(kGlArrayBuffer, 0);
    for (const auto& item : materialized) {
        if (item.attribute->integer) {
            host.backend().glVertexAttribIPointer(item.index, item.attribute->size,
                                                  item.attribute->type, item.attribute->stride,
                                                  item.pointer);
        } else {
            host.backend().glVertexAttribPointer(
                item.index, item.attribute->size, item.attribute->type,
                item.attribute->normalized, item.attribute->stride, item.pointer);
        }
    }
    if (rebound) host.backend().glBindBuffer(kGlArrayBuffer, current.array_buffer);
    return true;
}

}  // namespace

std::uint64_t gl_pname_count(GlBackend& backend, GLenum pname) {
    switch (pname) {
    case 0x846D:  // GL_ALIASED_POINT_SIZE_RANGE
    case 0x846E:  // GL_ALIASED_LINE_WIDTH_RANGE
    case 0x0B70:  // GL_DEPTH_RANGE
    case 0x0D3A:  // GL_MAX_VIEWPORT_DIMS
        return 2;
    case 0x8005:  // GL_BLEND_COLOR
    case 0x0C22:  // GL_COLOR_CLEAR_VALUE
    case 0x0C23:  // GL_COLOR_WRITEMASK
    case 0x0C10:  // GL_SCISSOR_BOX
    case 0x0BA2:  // GL_VIEWPORT
        return 4;
    case kGlCompressedTextureFormats:
    case kGlShaderBinaryFormats: {
        GLint count = 0;
        backend.glGetIntegerv(pname == kGlCompressedTextureFormats
                                  ? kGlNumCompressedTextureFormats
                                  : kGlNumShaderBinaryFormats,
                              &count);
        return count > 0 ? static_cast<std::uint64_t>(count) : 0;
    }
    default: {
        static std::mutex mutex;
        static std::unordered_set<GLenum> logged;
        std::lock_guard<std::mutex> lock(mutex);
        if (logged.insert(pname).second) {
            log("GLES pname 0x%x assumed to have one result element", pname);
        }
        return 1;
    }
    }
}

std::optional<std::uint64_t> gl_pixel_bytes(GLenum format, GLenum type, GLsizei width,
                                            GLsizei height, GLint alignment) {
    if (width < 0 || height < 0 || (alignment != 1 && alignment != 2 && alignment != 4 && alignment != 8)) {
        return std::nullopt;
    }
    std::uint64_t components = 0;
    switch (format) {
    case 0x1901:  // GL_STENCIL_INDEX
    case 0x1902:  // GL_DEPTH_COMPONENT
    case 0x1903:  // GL_RED
    case 0x1906:  // GL_ALPHA
    case 0x1909:  // GL_LUMINANCE
    case 0x84F9:  // GL_DEPTH_STENCIL
    case 0x8D94:  // GL_RED_INTEGER
        components = 1;
        break;
    case 0x190A:  // GL_LUMINANCE_ALPHA
    case 0x8227:  // GL_RG
    case 0x8228:  // GL_RG_INTEGER
        components = 2;
        break;
    case 0x1907:  // GL_RGB
    case 0x8D98:  // GL_RGB_INTEGER
        components = 3;
        break;
    case 0x1908:  // GL_RGBA
    case 0x80E1:  // GL_BGRA_EXT (EXT_texture_format_BGRA8888, used by old NME/lime builds)
    case 0x8D99:  // GL_RGBA_INTEGER
        components = 4;
        break;
    default:
        return std::nullopt;
    }

    // A packed type fixes the whole pixel size and pairs with one or two formats only.
    struct PackedType {
        GLenum type;
        std::uint64_t bytes;
        GLenum format;
        GLenum other_format;
    };
    static constexpr PackedType kPacked[] = {
        {0x8363, 2, 0x1907, 0},       // GL_UNSIGNED_SHORT_5_6_5 with GL_RGB
        {0x8033, 2, 0x1908, 0},       // GL_UNSIGNED_SHORT_4_4_4_4 with GL_RGBA
        {0x8034, 2, 0x1908, 0},       // GL_UNSIGNED_SHORT_5_5_5_1 with GL_RGBA
        {0x8368, 4, 0x1908, 0x8D99},  // GL_UNSIGNED_INT_2_10_10_10_REV, also RGBA_INTEGER
        {0x8C3B, 4, 0x1907, 0},       // GL_UNSIGNED_INT_10F_11F_11F_REV with GL_RGB
        {0x8C3E, 4, 0x1907, 0},       // GL_UNSIGNED_INT_5_9_9_9_REV with GL_RGB
        {0x84FA, 4, 0x84F9, 0},       // GL_UNSIGNED_INT_24_8 with GL_DEPTH_STENCIL
        {0x8DAD, 8, 0x84F9, 0},       // GL_FLOAT_32_UNSIGNED_INT_24_8_REV with GL_DEPTH_STENCIL
    };
    std::uint64_t bytes_per_pixel = 0;
    for (const PackedType& packed : kPacked) {
        if (packed.type != type) continue;
        if (packed.format != format && packed.other_format != format) return std::nullopt;
        bytes_per_pixel = packed.bytes;
    }
    if (bytes_per_pixel == 0) {
        std::uint64_t component_bytes = 0;
        switch (type) {
        case 0x1400:  // GL_BYTE
        case 0x1401:  // GL_UNSIGNED_BYTE
            component_bytes = 1;
            break;
        case 0x1402:  // GL_SHORT
        case 0x1403:  // GL_UNSIGNED_SHORT
        case 0x140B:  // GL_HALF_FLOAT
        // OES_texture_half_float gave the same type a different number, and engines still use it:
        // Unity 4.5 probes for half-float support by uploading an 8x8 texture with it.
        case 0x8D61:  // GL_HALF_FLOAT_OES
            component_bytes = 2;
            break;
        case 0x1404:  // GL_INT
        case 0x1405:  // GL_UNSIGNED_INT
        case 0x1406:  // GL_FLOAT
            component_bytes = 4;
            break;
        default:
            return std::nullopt;
        }
        bytes_per_pixel = components * component_bytes;
    }

    std::uint64_t row = 0;
    if (!checked_multiply(static_cast<std::uint64_t>(width), bytes_per_pixel, row)) return std::nullopt;
    if (height == 0 || row == 0) return 0;
    const std::uint64_t align = static_cast<std::uint64_t>(alignment);
    if (row > std::numeric_limits<std::uint64_t>::max() - (align - 1)) return std::nullopt;
    const std::uint64_t stride = (row + align - 1) & ~(align - 1);
    std::uint64_t preceding = 0;
    if (!checked_multiply(static_cast<std::uint64_t>(height - 1), stride, preceding) ||
        preceding > std::numeric_limits<std::uint64_t>::max() - row) {
        return std::nullopt;
    }
    return preceding + row;
}

void HostGl::note_pixel_store(GLenum pname, GLint param) {
    if (param != 1 && param != 2 && param != 4 && param != 8) return;
    GlThreadState& current = state(*this);
    if (pname == kGlPackAlignment) current.pack_alignment = param;
    if (pname == kGlUnpackAlignment) current.unpack_alignment = param;
}

void HostGl::note_bind_buffer(GLenum target, GLuint buffer) {
    GlThreadState& current = state(*this);
    if (target == kGlArrayBuffer) current.array_buffer = buffer;
    if (target == kGlElementArrayBuffer) current.element_array_buffer = buffer;
    if (target == kGlPixelPackBuffer) current.pixel_pack_buffer = buffer;
    if (target == kGlPixelUnpackBuffer) current.pixel_unpack_buffer = buffer;
}

GLuint HostGl::pixel_buffer(bool pack) const {
    const GlThreadState& current = state(*this);
    return pack ? current.pixel_pack_buffer : current.pixel_unpack_buffer;
}

void HostGl::note_vertex_attrib_enabled(GLuint index, bool enabled) {
    state(*this).attributes[index].enabled = enabled;
}

GLint HostGl::pixel_alignment(bool pack) const {
    const GlThreadState& current = state(*this);
    return pack ? current.pack_alignment : current.unpack_alignment;
}

void HostGl::invalidate_uniforms(GLuint program) {
    state(*this).uniforms.erase(program);
}

std::optional<std::uint32_t> HostGl::allocate_guest(std::size_t size) {
    if (size == 0 || size > UINT32_MAX) return std::nullopt;
    if (allocator_) return allocator_(size);
    GuestCall args;
    args.regs = {static_cast<std::uint32_t>(size), 0, 0, 0};
    const auto result = runtime_.call_on_current(runtime_.service_api().malloc_fn, args);
    if (!result || result->r0 == 0) return std::nullopt;
    return result->r0;
}

void HostGl::free_guest(std::uint32_t address) {
    if (address == 0 || allocator_) return;
    GuestCall args;
    args.regs = {address, 0, 0, 0};
    (void)runtime_.call_on_current(runtime_.service_api().free_fn, args);
}

std::optional<std::uint64_t> HostGl::uniform_elements(GLuint program, GLint location) {
    GlThreadState& current = state(*this);
    auto cached = current.uniforms.find(program);
    if (cached == current.uniforms.end()) {
        std::unordered_map<GLint, std::uint64_t> locations;
        GLint count = 0;
        GLint max_length = 0;
        backend_.glGetProgramiv(program, kGlActiveUniforms, &count);
        backend_.glGetProgramiv(program, kGlActiveUniformMaxLength, &max_length);
        if (count < 0) count = 0;
        max_length = std::clamp(max_length, 1, 1 << 20);
        std::vector<GLchar> name(static_cast<std::size_t>(max_length));
        for (GLint index = 0; index < count; ++index) {
            GLsizei written = 0;
            GLint size = 0;
            GLenum type = 0;
            backend_.glGetActiveUniform(program, static_cast<GLuint>(index), max_length,
                                        &written, &size, &type, name.data());
            if (written < 0 || written >= max_length) continue;
            name[static_cast<std::size_t>(written)] = '\0';
            std::string uniform(name.data(), static_cast<std::size_t>(written));
            if (uniform.ends_with("[0]")) uniform.resize(uniform.size() - 3);
            const GLint uniform_location = backend_.glGetUniformLocation(program, uniform.c_str());
            const std::uint64_t elements = uniform_components(type);
            if (uniform_location >= 0 && elements != 0) locations[uniform_location] = elements;
        }
        cached = current.uniforms.emplace(program, std::move(locations)).first;
    }
    const auto found = cached->second.find(location);
    if (found == cached->second.end()) return std::nullopt;
    return found->second;
}

bool zbgl_manual_glGetBooleanv(HostGl& host, HostGl::Call& call) {
    return serve_pname(host, call, &GlBackend::glGetBooleanv);
}

bool zbgl_manual_glGetFloatv(HostGl& host, HostGl::Call& call) {
    return serve_pname(host, call, &GlBackend::glGetFloatv);
}

bool zbgl_manual_glGetIntegerv(HostGl& host, HostGl::Call& call) {
    if (call.scalar<GLenum>(0) == kGlNumExtensions) {
        // Must match what glGetStringi hands out, or the guest reads past the filtered list.
        GLint* data = call.pointer<GLint>(1, 1, kPageRead | kPageWrite);
        if (!call.valid()) return true;
        *data = static_cast<GLint>(served_extensions(host).size());
        return true;
    }
    return serve_pname(host, call, &GlBackend::glGetIntegerv);
}

bool zbgl_manual_glTexImage2D(HostGl& host, HostGl::Call& call) {
    const GLenum target = call.scalar<GLenum>(0);
    const GLint level = call.scalar<GLint>(1);
    const GLint internalformat = call.scalar<GLint>(2);
    const GLsizei width = call.scalar<GLsizei>(3);
    const GLsizei height = call.scalar<GLsizei>(4);
    const GLint border = call.scalar<GLint>(5);
    const GLenum format = call.scalar<GLenum>(6);
    const GLenum type = call.scalar<GLenum>(7);
    void* pixels = nullptr;
    if (!pixel_pointer(host, call, false, format, type, width, height, 1,
                       host.pixel_alignment(false), 8, kPageRead, pixels)) return true;
    host.backend().glTexImage2D(target, level, internalformat, width, height, border,
                                format, type, pixels);
    return true;
}

bool zbgl_manual_glTexSubImage2D(HostGl& host, HostGl::Call& call) {
    const GLenum target = call.scalar<GLenum>(0);
    const GLint level = call.scalar<GLint>(1);
    const GLint xoffset = call.scalar<GLint>(2);
    const GLint yoffset = call.scalar<GLint>(3);
    const GLsizei width = call.scalar<GLsizei>(4);
    const GLsizei height = call.scalar<GLsizei>(5);
    const GLenum format = call.scalar<GLenum>(6);
    const GLenum type = call.scalar<GLenum>(7);
    void* pixels = nullptr;
    if (!pixel_pointer(host, call, false, format, type, width, height, 1,
                       host.pixel_alignment(false), 8, kPageRead, pixels)) return true;
    host.backend().glTexSubImage2D(target, level, xoffset, yoffset, width, height,
                                   format, type, pixels);
    return true;
}

bool zbgl_manual_glReadPixels(HostGl& host, HostGl::Call& call) {
    const GLint x = call.scalar<GLint>(0);
    const GLint y = call.scalar<GLint>(1);
    const GLsizei width = call.scalar<GLsizei>(2);
    const GLsizei height = call.scalar<GLsizei>(3);
    const GLenum format = call.scalar<GLenum>(4);
    const GLenum type = call.scalar<GLenum>(5);
    void* pixels = nullptr;
    if (!pixel_pointer(host, call, true, format, type, width, height, 1,
                       host.pixel_alignment(true), 6, kPageRead | kPageWrite, pixels)) return true;
    host.backend().glReadPixels(x, y, width, height, format, type, pixels);
    return true;
}

bool zbgl_manual_glGetUniformfv(HostGl& host, HostGl::Call& call) {
    const GLuint program = call.scalar<GLuint>(0);
    const GLint location = call.scalar<GLint>(1);
    const auto elements = host.uniform_elements(program, location);
    if (!elements) {
        host.reject(call, kGlInvalidOperation, "uniform location is not active in the program");
        return true;
    }
    GLfloat* params = call.pointer<GLfloat>(2, *elements, kPageRead | kPageWrite);
    if (call.valid()) host.backend().glGetUniformfv(program, location, params);
    return true;
}

bool zbgl_manual_glGetUniformiv(HostGl& host, HostGl::Call& call) {
    const GLuint program = call.scalar<GLuint>(0);
    const GLint location = call.scalar<GLint>(1);
    const auto elements = host.uniform_elements(program, location);
    if (!elements) {
        host.reject(call, kGlInvalidOperation, "uniform location is not active in the program");
        return true;
    }
    GLint* params = call.pointer<GLint>(2, *elements, kPageRead | kPageWrite);
    if (call.valid()) host.backend().glGetUniformiv(program, location, params);
    return true;
}

bool zbgl_manual_glBindAttribLocation(HostGl& host, HostGl::Call& call) {
    const GLuint program = call.scalar<GLuint>(0);
    const GLuint index = call.scalar<GLuint>(1);
    const GLchar* name = guest_string(host, call, call.arg(2));
    if (call.valid()) host.backend().glBindAttribLocation(program, index, name);
    return true;
}

bool zbgl_manual_glGetAttribLocation(HostGl& host, HostGl::Call& call) {
    const GLuint program = call.scalar<GLuint>(0);
    const GLchar* name = guest_string(host, call, call.arg(1));
    if (call.valid()) call.set_result(host.backend().glGetAttribLocation(program, name));
    return true;
}

bool zbgl_manual_glGetUniformLocation(HostGl& host, HostGl::Call& call) {
    const GLuint program = call.scalar<GLuint>(0);
    const GLchar* name = guest_string(host, call, call.arg(1));
    if (call.valid()) call.set_result(host.backend().glGetUniformLocation(program, name));
    return true;
}

bool zbgl_manual_glGetString(HostGl& host, HostGl::Call& call) {
    const GLenum name = call.scalar<GLenum>(0);
    GlThreadState& current = state(host);
    const auto cached = current.strings.find(name);
    if (cached != current.strings.end()) {
        call.set_result(cached->second);
        return true;
    }
    if (name == kGlExtensions) {
        std::string served;
        for (const std::string& extension : served_extensions(host)) {
            if (!served.empty()) served.push_back(' ');
            served += extension;
        }
        if (const std::uint32_t address = answer_with_string(host, call, served)) current.strings[name] = address;
        return true;
    }
    const GLubyte* source = host.backend().glGetString(name);
    if (source == nullptr) return true;
    constexpr std::size_t kMaxDriverString = 64u << 20;
    const std::size_t length = strnlen(reinterpret_cast<const char*>(source), kMaxDriverString);
    if (length == kMaxDriverString) {
        host.reject(call, kGlInvalidOperation, "driver string exceeds 64 MiB");
        return true;
    }
    const auto address = host.allocate_guest(length + 1);
    if (!address) {
        host.reject(call, kGlOutOfMemory, "guest allocation for driver string failed");
        return true;
    }
    std::uint8_t* destination = host.runtime().memory().host_ptr(
        *address, length + 1, kPageRead | kPageWrite);
    if (destination == nullptr) {
        host.reject(call, kGlInvalidOperation, "guest allocator returned an unreadable buffer");
        return true;
    }
    std::memcpy(destination, source, length + 1);
    current.strings[name] = *address;
    call.set_result(*address);
    return true;
}

bool zbgl_manual_glShaderSource(HostGl& host, HostGl::Call& call) {
    const GLuint shader = call.scalar<GLuint>(0);
    const GLsizei count = call.scalar<GLsizei>(1);
    if (count < 0) {
        call.fail(kGlInvalidValue, "shader source count is negative");
        return true;
    }
    const std::uint64_t items = call.length(count);
    const std::uint32_t* guest_sources =
        call.pointer<const std::uint32_t>(2, items, kPageRead);
    const GLint* lengths = call.pointer<const GLint>(3, items, kPageRead);
    if (!call.valid()) return true;

    std::vector<const GLchar*> sources(static_cast<std::size_t>(items));
    for (std::size_t i = 0; i < sources.size(); ++i) {
        const std::uint32_t address = guest_sources[i];
        if (lengths == nullptr || lengths[i] < 0) {
            sources[i] = guest_string(host, call, address);
        } else if (address == 0) {
            call.fail(kGlInvalidValue, "shader source pointer is null");
        } else {
            const std::uint8_t* source = host.runtime().memory().host_ptr(
                address, static_cast<std::uint64_t>(lengths[i]), kPageRead);
            if (source == nullptr) {
                call.fail(kGlInvalidValue, "shader source range is unreadable");
            } else {
                sources[i] = reinterpret_cast<const GLchar*>(source);
            }
        }
        if (!call.valid()) return true;
    }
    host.backend().glShaderSource(shader, count,
                                  sources.empty() ? nullptr : sources.data(), lengths);
    return true;
}

bool zbgl_manual_glVertexAttribPointer(HostGl& host, HostGl::Call& call) {
    const GLuint index = call.scalar<GLuint>(0);
    GlThreadState::Attribute& attribute = state(host).attributes[index];
    attribute.defined = true;
    attribute.size = call.scalar<GLint>(1);
    attribute.type = call.scalar<GLenum>(2);
    attribute.normalized = call.scalar<GLboolean>(3);
    attribute.stride = call.scalar<GLsizei>(4);
    attribute.guest_pointer = call.arg(5);
    attribute.integer = false;
    attribute.buffer = state(host).array_buffer;
    if (!call.valid()) return true;
    if (attribute.buffer != 0) {
        host.backend().glVertexAttribPointer(
            index, attribute.size, attribute.type, attribute.normalized, attribute.stride,
            reinterpret_cast<const void*>(static_cast<std::uintptr_t>(attribute.guest_pointer)));
    }
    return true;
}

bool zbgl_manual_glDrawArrays(HostGl& host, HostGl::Call& call) {
    const GLenum mode = call.scalar<GLenum>(0);
    const GLint first = call.scalar<GLint>(1);
    const GLsizei count = call.scalar<GLsizei>(2);
    if (!call.valid()) return true;
    if (count > 0) {
        if (first < 0 || !materialize_client_arrays(
                             host, call, static_cast<std::uint64_t>(first),
                             static_cast<std::uint64_t>(first) + static_cast<std::uint64_t>(count) - 1)) {
            return true;
        }
    }
    host.backend().glDrawArrays(mode, first, count);
    return true;
}

// Shared by glDrawElements, glDrawElementsInstanced and glDrawRangeElements: the indices
// argument is a guest pointer when no element array buffer is bound and a buffer offset when one
// is, and any enabled client array is materialized for the index range that will be read.
// Returns false when the call was rejected.
bool prepare_elements(HostGl& host, HostGl::Call& call, GLsizei count, GLenum type,
                      std::uint32_t guest_indices, const void*& driver_indices) {
    if (count < 0) {
        call.fail(kGlInvalidOperation, "element count is negative");
        return false;
    }
    GlThreadState& current = state(host);
    driver_indices = nullptr;
    std::uint64_t max_index = 0;
    if (current.element_array_buffer == 0) {
        const std::uint64_t index_size = element_index_bytes(type);
        if (index_size == 0) {
            call.fail(kGlInvalidOperation, "element index type is invalid");
            return false;
        }
        const std::uint64_t bytes = static_cast<std::uint64_t>(count) * index_size;
        const std::uint8_t* indices = count == 0 && guest_indices == 0
                                          ? nullptr
                                          : host.runtime().memory().host_ptr(guest_indices, bytes, kPageRead);
        if (count != 0 && indices == nullptr) {
            call.fail(kGlInvalidOperation, "client element indices are unreadable");
            return false;
        }
        for (GLsizei i = 0; i < count; ++i) {
            std::uint64_t value = indices[i];
            if (index_size == 2) {
                std::uint16_t value16;
                std::memcpy(&value16, indices + static_cast<std::size_t>(i) * 2, sizeof(value16));
                value = value16;
            } else if (index_size == 4) {
                std::uint32_t value32;
                std::memcpy(&value32, indices + static_cast<std::size_t>(i) * 4, sizeof(value32));
                value = value32;
            }
            max_index = std::max(max_index, value);
        }
        driver_indices = indices;
    } else {
        driver_indices = reinterpret_cast<const void*>(static_cast<std::uintptr_t>(guest_indices));
        if (count > 0) max_index = static_cast<std::uint64_t>(count - 1);
    }
    if (count > 0 && !materialize_client_arrays(host, call, 0, max_index)) return false;
    return true;
}

bool zbgl_manual_glDrawElements(HostGl& host, HostGl::Call& call) {
    const GLenum mode = call.scalar<GLenum>(0);
    const GLsizei count = call.scalar<GLsizei>(1);
    const GLenum type = call.scalar<GLenum>(2);
    const std::uint32_t guest_indices = call.arg(3);
    if (!call.valid()) return true;
    const void* driver_indices = nullptr;
    if (!prepare_elements(host, call, count, type, guest_indices, driver_indices)) return true;
    host.backend().glDrawElements(mode, count, type, driver_indices);
    return true;
}

bool zbgl_manual_glGetVertexAttribPointerv(HostGl& host, HostGl::Call& call) {
    const GLuint index = call.scalar<GLuint>(0);
    (void)call.scalar<GLenum>(1);
    std::uint32_t* pointer = call.pointer<std::uint32_t>(2, 1, kPageRead | kPageWrite);
    if (!call.valid()) return true;
    if (pointer == nullptr) {
        call.fail(kGlInvalidValue, "vertex attribute pointer output is null");
        return true;
    }
    const auto found = state(host).attributes.find(index);
    *pointer = found == state(host).attributes.end() ? 0 : found->second.guest_pointer;
    return true;
}

namespace {

constexpr GLenum kGlInvalidEnum = 0x0500;
constexpr GLenum kGlColorBuffer = 0x1800;

// GLES 3.0 sync objects. A GLsync is a driver pointer, so the guest only ever sees a 32-bit
// handle; the table is process-wide because sync objects are shared between contexts.
std::mutex sync_mutex;
std::vector<GLsync> sync_objects;  // handle - 1 indexes this; a null slot is a deleted object

std::uint32_t register_sync(GLsync sync) {
    if (sync == nullptr) return 0;
    std::lock_guard<std::mutex> lock(sync_mutex);
    for (std::size_t i = 0; i < sync_objects.size(); ++i) {
        if (sync_objects[i] == nullptr) {
            sync_objects[i] = sync;
            return static_cast<std::uint32_t>(i + 1);
        }
    }
    sync_objects.push_back(sync);
    return static_cast<std::uint32_t>(sync_objects.size());
}

GLsync lookup_sync(std::uint32_t handle) {
    std::lock_guard<std::mutex> lock(sync_mutex);
    if (handle == 0 || handle > sync_objects.size()) return nullptr;
    return sync_objects[handle - 1];
}

GLsync release_sync(std::uint32_t handle) {
    std::lock_guard<std::mutex> lock(sync_mutex);
    if (handle == 0 || handle > sync_objects.size()) return nullptr;
    const GLsync sync = sync_objects[handle - 1];
    sync_objects[handle - 1] = nullptr;
    return sync;
}

bool resolve_sync(HostGl& host, HostGl::Call& call, unsigned position, GLsync& sync) {
    const std::uint32_t handle = call.arg(position);
    if (!call.valid()) return false;
    sync = lookup_sync(handle);
    if (sync == nullptr) {
        host.reject(call, kGlInvalidValue, "sync handle is not a live sync object");
        return false;
    }
    return true;
}

// AAPCS32 puts a 64-bit argument in an even register pair (or an 8-byte aligned stack slot),
// low word first.
std::uint64_t argument64(HostGl::Call& call, unsigned low) {
    return static_cast<std::uint64_t>(call.arg(low)) |
           (static_cast<std::uint64_t>(call.arg(low + 1)) << 32);
}

// A guest mirror of a host-mapped buffer range: the driver's mapped memory lives outside the
// guest address space, so glMapBufferRange hands the guest a copy of the range and
// glUnmapBuffer writes it back. Keyed by the target the buffer was mapped through, which is how
// glUnmapBuffer, glFlushMappedBufferRange and glGetBufferPointerv name it again. Process-wide,
// like the buffer objects themselves.
struct BufferMapping {
    std::uint32_t guest = 0;
    std::uint8_t* data = nullptr;
    std::uint64_t length = 0;
    GLbitfield access = 0;
    GlMapDiagnostic diagnostic;
};

std::mutex mapping_mutex;
std::unordered_map<GLenum, BufferMapping> mappings;

bool find_mapping(GLenum target, BufferMapping& mapping) {
    std::lock_guard<std::mutex> lock(mapping_mutex);
    const auto found = mappings.find(target);
    if (found == mappings.end()) return false;
    mapping = found->second;
    return true;
}

template <typename T>
bool serve_uniform(HostGl& host, HostGl::Call& call,
                   void (GlBackend::*function)(GLuint, GLint, T*)) {
    const GLuint program = call.scalar<GLuint>(0);
    const GLint location = call.scalar<GLint>(1);
    const auto elements = host.uniform_elements(program, location);
    if (!elements) {
        host.reject(call, kGlInvalidOperation, "uniform location is not active in the program");
        return true;
    }
    T* params = call.pointer<T>(2, *elements, kPageRead | kPageWrite);
    if (call.valid()) (host.backend().*function)(program, location, params);
    return true;
}

// GL_OES_mapbuffer maps a whole data store rather than a range, so it needs the buffer's size
// and translates its access enum to the glMapBufferRange bits the mirror logic is written in.
constexpr GLenum kGlBufferSize = 0x8764;
constexpr GLenum kGlReadOnlyOes = 0x88B8;
constexpr GLenum kGlWriteOnlyOes = 0x88B9;
constexpr GLenum kGlReadWriteOes = 0x88BA;

// glUnmapBuffer (GLES 3.0) and glUnmapBufferOES (GL_OES_mapbuffer) differ only in which driver
// entry point ends the mapping: both write the guest mirror back first.
bool unmap_mirrored(HostGl& host, HostGl::Call& call, bool oes) {
    const GLenum target = call.scalar<GLenum>(0);
    if (!call.valid()) return true;
    BufferMapping mapping;
    bool mirrored = false;
    {
        std::lock_guard<std::mutex> lock(mapping_mutex);
        const auto found = mappings.find(target);
        if (found != mappings.end()) {
            mapping = found->second;
            mappings.erase(found);
            mirrored = true;
        }
    }
    if (mirrored && (mapping.access & kGlMapWrite) != 0) {
        const std::uint8_t* mirror =
            host.runtime().memory().host_ptr(mapping.guest, mapping.length, kPageRead);
        if (mirror == nullptr) {
            host.reject(call, kGlInvalidOperation, "mapped buffer mirror is unreadable");
        } else {
            std::memcpy(mapping.data, mirror, static_cast<std::size_t>(mapping.length));
            gl_diagnose_map_unmap(mapping.diagnostic, mapping.length, mirror, mapping.data);
        }
    }
    const GLboolean result =
        oes ? host.backend().glUnmapBufferOES(target) : host.backend().glUnmapBuffer(target);
    if (mirrored) host.free_guest(mapping.guest);
    call.set_result(result);
    return true;
}

// glGetBufferPointerv / glGetBufferPointervOES. void** in the guest is one 32-bit slot, and what
// belongs in it is the mirror's guest address, never the driver's mapped pointer.
bool report_buffer_pointer(HostGl& host, HostGl::Call& call) {
    const GLenum target = call.scalar<GLenum>(0);
    const GLenum pname = call.scalar<GLenum>(1);
    std::uint32_t* params = call.pointer<std::uint32_t>(2, 1, kPageRead | kPageWrite);
    if (!call.valid()) return true;
    if (params == nullptr) {
        host.reject(call, kGlInvalidValue, "buffer pointer output is null");
        return true;
    }
    if (pname != kGlBufferMapPointer) {
        host.reject(call, kGlInvalidEnum, "buffer pointer pname is not GL_BUFFER_MAP_POINTER");
        return true;
    }
    BufferMapping mapping;
    *params = find_mapping(target, mapping) ? mapping.guest : 0;
    return true;
}

// The guest passes an array of 32-bit pointers to strings; the driver needs host-width pointers.
bool guest_string_array(HostGl& host, HostGl::Call& call, unsigned position, std::uint64_t items,
                        std::vector<const GLchar*>& strings) {
    const std::uint32_t* guest_strings =
        call.pointer<const std::uint32_t>(position, items, kPageRead);
    if (!call.valid()) return false;
    if (items != 0 && guest_strings == nullptr) {
        call.fail(kGlInvalidValue, "string array pointer is null");
        return false;
    }
    strings.resize(static_cast<std::size_t>(items));
    for (std::size_t i = 0; i < strings.size(); ++i) {
        strings[i] = guest_string(host, call, guest_strings[i]);
        if (!call.valid()) return false;
    }
    return true;
}

}  // namespace

bool zbgl_manual_glGetStringi(HostGl& host, HostGl::Call& call) {
    const GLenum name = call.scalar<GLenum>(0);
    const GLuint index = call.scalar<GLuint>(1);
    if (!call.valid()) return true;
    GlThreadState& current = state(host);
    const std::uint64_t key = (static_cast<std::uint64_t>(name) << 32) | index;
    const auto cached = current.indexed_strings.find(key);
    if (cached != current.indexed_strings.end()) {
        call.set_result(cached->second);
        return true;
    }
    if (name == kGlExtensions) {
        const std::vector<std::string>& served = served_extensions(host);
        if (index >= served.size()) {
            host.reject(call, kGlInvalidValue, "extension index is out of range");
            return true;
        }
        if (const std::uint32_t address = answer_with_string(host, call, served[index])) {
            current.indexed_strings[key] = address;
        }
        return true;
    }
    const GLubyte* source = host.backend().glGetStringi(name, index);
    if (source == nullptr) return true;
    constexpr std::size_t kMaxDriverString = 64u << 20;
    const std::size_t length = strnlen(reinterpret_cast<const char*>(source), kMaxDriverString);
    if (length == kMaxDriverString) {
        host.reject(call, kGlInvalidOperation, "driver string exceeds 64 MiB");
        return true;
    }
    const auto address = host.allocate_guest(length + 1);
    std::uint8_t* destination =
        address ? host.runtime().memory().host_ptr(*address, length + 1, kPageRead | kPageWrite)
                : nullptr;
    if (destination == nullptr) {
        host.reject(call, kGlOutOfMemory, "guest allocation for driver string failed");
        return true;
    }
    std::memcpy(destination, source, length + 1);
    current.indexed_strings[key] = *address;
    call.set_result(*address);
    return true;
}

bool zbgl_manual_glMapBufferRange(HostGl& host, HostGl::Call& call) {
    const GLenum target = call.scalar<GLenum>(0);
    const GLintptr offset = call.scalar<GLintptr>(1);
    const GLsizeiptr length = call.scalar<GLsizeiptr>(2);
    const GLbitfield access = call.scalar<GLbitfield>(3);
    if (!call.valid()) return true;
    if (offset < 0 || length <= 0) {
        host.reject(call, kGlInvalidValue, "mapped range offset or length is invalid");
        return true;
    }
    BufferMapping existing;
    if (find_mapping(target, existing)) {
        gl_diagnose_map_collision(host, target, existing.diagnostic);
        host.reject(call, kGlInvalidOperation, "buffer target is already mapped");
        return true;
    }
    void* mapped = host.backend().glMapBufferRange(target, offset, length, access);
    if (mapped == nullptr) return true;  // The driver queued its own error; the guest gets NULL.
    const auto address = host.allocate_guest(static_cast<std::size_t>(length));
    std::uint8_t* mirror =
        address ? host.runtime().memory().host_ptr(*address, static_cast<std::uint64_t>(length),
                                                   kPageRead | kPageWrite)
                : nullptr;
    if (mirror == nullptr) {
        if (address) host.free_guest(*address);
        host.backend().glUnmapBuffer(target);
        host.reject(call, kGlOutOfMemory, "guest mirror for the mapped buffer range failed");
        return true;
    }
    // GL_MAP_INVALIDATE_RANGE_BIT / GL_MAP_INVALIDATE_BUFFER_BIT say the previous contents are
    // undefined, so they are never copied in; otherwise a reader and a partial writer both need
    // to see what the buffer holds now.
    const bool invalidated = (access & (kGlMapInvalidateRange | kGlMapInvalidateBuffer)) != 0;
    if (!invalidated && (access & (kGlMapRead | kGlMapWrite)) != 0) {
        std::memcpy(mirror, mapped, static_cast<std::size_t>(length));
    }
    const GlMapDiagnostic diagnostic =
        gl_diagnose_map(host, target, offset, length, access, *address, mirror,
                        static_cast<const std::uint8_t*>(mapped));
    {
        std::lock_guard<std::mutex> lock(mapping_mutex);
        mappings[target] = BufferMapping{
            *address, static_cast<std::uint8_t*>(mapped), static_cast<std::uint64_t>(length),
            access, diagnostic};
    }
    call.set_result(*address);
    return true;
}

bool zbgl_manual_glUnmapBuffer(HostGl& host, HostGl::Call& call) {
    return unmap_mirrored(host, call, false);
}

bool zbgl_manual_glFlushMappedBufferRange(HostGl& host, HostGl::Call& call) {
    const GLenum target = call.scalar<GLenum>(0);
    const GLintptr offset = call.scalar<GLintptr>(1);
    const GLsizeiptr length = call.scalar<GLsizeiptr>(2);
    if (!call.valid()) return true;
    BufferMapping mapping;
    if (find_mapping(target, mapping) && (mapping.access & kGlMapWrite) != 0) {
        // The flushed range is relative to the start of the mapped range.
        if (offset < 0 || length < 0 ||
            static_cast<std::uint64_t>(offset) + static_cast<std::uint64_t>(length) >
                mapping.length) {
            host.reject(call, kGlInvalidValue, "flushed range is outside the mapped range");
            return true;
        }
        const std::uint8_t* mirror = host.runtime().memory().host_ptr(
            mapping.guest + static_cast<std::uint32_t>(offset),
            static_cast<std::uint64_t>(length), kPageRead);
        if (mirror == nullptr) {
            host.reject(call, kGlInvalidOperation, "mapped buffer mirror is unreadable");
            return true;
        }
        std::memcpy(mapping.data + offset, mirror, static_cast<std::size_t>(length));
        gl_diagnose_map_flush(mapping.diagnostic, offset, length, mirror,
                              mapping.data + offset);
    }
    host.backend().glFlushMappedBufferRange(target, offset, length);
    return true;
}

bool zbgl_manual_glGetBufferPointerv(HostGl& host, HostGl::Call& call) {
    return report_buffer_pointer(host, call);
}

// GL_OES_mapbuffer: the GLES 2.0 spelling of the same three entry points. glMapBufferOES maps
// the whole data store, so the mirror is the buffer's GL_BUFFER_SIZE bytes.
bool zbgl_manual_glMapBufferOES(HostGl& host, HostGl::Call& call) {
    const GLenum target = call.scalar<GLenum>(0);
    const GLenum access = call.scalar<GLenum>(1);
    if (!call.valid()) return true;
    GLbitfield bits = 0;
    switch (access) {
    case kGlReadOnlyOes: bits = kGlMapRead; break;
    case kGlWriteOnlyOes: bits = kGlMapWrite; break;
    case kGlReadWriteOes: bits = kGlMapRead | kGlMapWrite; break;
    default:
        host.reject(call, kGlInvalidEnum, "glMapBufferOES access is not a GL_*_ONLY enum");
        return true;
    }
    BufferMapping existing;
    if (find_mapping(target, existing)) {
        host.reject(call, kGlInvalidOperation, "buffer target is already mapped");
        return true;
    }
    GLint size = 0;
    host.backend().glGetBufferParameteriv(target, kGlBufferSize, &size);
    if (size <= 0) {
        host.reject(call, kGlInvalidOperation, "no buffer with a data store is bound to the target");
        return true;
    }
    void* mapped = host.backend().glMapBufferOES(target, access);
    if (mapped == nullptr) return true;  // The driver queued its own error; the guest gets NULL.
    const auto address = host.allocate_guest(static_cast<std::size_t>(size));
    std::uint8_t* mirror =
        address ? host.runtime().memory().host_ptr(*address, static_cast<std::uint64_t>(size),
                                                   kPageRead | kPageWrite)
                : nullptr;
    if (mirror == nullptr) {
        if (address) host.free_guest(*address);
        host.backend().glUnmapBufferOES(target);
        host.reject(call, kGlOutOfMemory, "guest mirror for the mapped buffer failed");
        return true;
    }
    // Unlike glMapBufferRange there is no invalidate bit: the data store keeps its contents, so
    // a partial writer must see them.
    std::memcpy(mirror, mapped, static_cast<std::size_t>(size));
    const GlMapDiagnostic diagnostic =
        gl_diagnose_map(host, target, 0, size, bits, *address, mirror,
                        static_cast<const std::uint8_t*>(mapped));
    {
        std::lock_guard<std::mutex> lock(mapping_mutex);
        mappings[target] = BufferMapping{
            *address, static_cast<std::uint8_t*>(mapped), static_cast<std::uint64_t>(size), bits,
            diagnostic};
    }
    call.set_result(*address);
    return true;
}

bool zbgl_manual_glUnmapBufferOES(HostGl& host, HostGl::Call& call) {
    return unmap_mirrored(host, call, true);
}

bool zbgl_manual_glGetBufferPointervOES(HostGl& host, HostGl::Call& call) {
    return report_buffer_pointer(host, call);
}

bool zbgl_manual_glTexImage3D(HostGl& host, HostGl::Call& call) {
    const GLenum target = call.scalar<GLenum>(0);
    const GLint level = call.scalar<GLint>(1);
    const GLint internalformat = call.scalar<GLint>(2);
    const GLsizei width = call.scalar<GLsizei>(3);
    const GLsizei height = call.scalar<GLsizei>(4);
    const GLsizei depth = call.scalar<GLsizei>(5);
    const GLint border = call.scalar<GLint>(6);
    const GLenum format = call.scalar<GLenum>(7);
    const GLenum type = call.scalar<GLenum>(8);
    void* pixels = nullptr;
    if (!pixel_pointer(host, call, false, format, type, width, height, depth,
                       host.pixel_alignment(false), 9, kPageRead, pixels)) return true;
    host.backend().glTexImage3D(target, level, internalformat, width, height, depth, border,
                                format, type, pixels);
    return true;
}

bool zbgl_manual_glTexSubImage3D(HostGl& host, HostGl::Call& call) {
    const GLenum target = call.scalar<GLenum>(0);
    const GLint level = call.scalar<GLint>(1);
    const GLint xoffset = call.scalar<GLint>(2);
    const GLint yoffset = call.scalar<GLint>(3);
    const GLint zoffset = call.scalar<GLint>(4);
    const GLsizei width = call.scalar<GLsizei>(5);
    const GLsizei height = call.scalar<GLsizei>(6);
    const GLsizei depth = call.scalar<GLsizei>(7);
    const GLenum format = call.scalar<GLenum>(8);
    const GLenum type = call.scalar<GLenum>(9);
    void* pixels = nullptr;
    if (!pixel_pointer(host, call, false, format, type, width, height, depth,
                       host.pixel_alignment(false), 10, kPageRead, pixels)) return true;
    host.backend().glTexSubImage3D(target, level, xoffset, yoffset, zoffset, width, height, depth,
                                   format, type, pixels);
    return true;
}

bool zbgl_manual_glDrawArraysInstanced(HostGl& host, HostGl::Call& call) {
    const GLenum mode = call.scalar<GLenum>(0);
    const GLint first = call.scalar<GLint>(1);
    const GLsizei count = call.scalar<GLsizei>(2);
    const GLsizei instancecount = call.scalar<GLsizei>(3);
    if (!call.valid()) return true;
    if (count > 0) {
        if (first < 0 || !materialize_client_arrays(
                             host, call, static_cast<std::uint64_t>(first),
                             static_cast<std::uint64_t>(first) + static_cast<std::uint64_t>(count) - 1)) {
            return true;
        }
    }
    host.backend().glDrawArraysInstanced(mode, first, count, instancecount);
    return true;
}

bool zbgl_manual_glDrawElementsInstanced(HostGl& host, HostGl::Call& call) {
    const GLenum mode = call.scalar<GLenum>(0);
    const GLsizei count = call.scalar<GLsizei>(1);
    const GLenum type = call.scalar<GLenum>(2);
    const std::uint32_t guest_indices = call.arg(3);
    const GLsizei instancecount = call.scalar<GLsizei>(4);
    if (!call.valid()) return true;
    const void* driver_indices = nullptr;
    if (!prepare_elements(host, call, count, type, guest_indices, driver_indices)) return true;
    host.backend().glDrawElementsInstanced(mode, count, type, driver_indices, instancecount);
    return true;
}

bool zbgl_manual_glDrawRangeElements(HostGl& host, HostGl::Call& call) {
    const GLenum mode = call.scalar<GLenum>(0);
    const GLuint start = call.scalar<GLuint>(1);
    const GLuint end = call.scalar<GLuint>(2);
    const GLsizei count = call.scalar<GLsizei>(3);
    const GLenum type = call.scalar<GLenum>(4);
    const std::uint32_t guest_indices = call.arg(5);
    if (!call.valid()) return true;
    const void* driver_indices = nullptr;
    if (!prepare_elements(host, call, count, type, guest_indices, driver_indices)) return true;
    host.backend().glDrawRangeElements(mode, start, end, count, type, driver_indices);
    return true;
}

bool zbgl_manual_glVertexAttribIPointer(HostGl& host, HostGl::Call& call) {
    const GLuint index = call.scalar<GLuint>(0);
    GlThreadState::Attribute& attribute = state(host).attributes[index];
    attribute.defined = true;
    attribute.size = call.scalar<GLint>(1);
    attribute.type = call.scalar<GLenum>(2);
    attribute.normalized = 0;
    attribute.stride = call.scalar<GLsizei>(3);
    attribute.guest_pointer = call.arg(4);
    attribute.integer = true;
    attribute.buffer = state(host).array_buffer;
    if (!call.valid()) return true;
    if (attribute.buffer != 0) {
        host.backend().glVertexAttribIPointer(
            index, attribute.size, attribute.type, attribute.stride,
            reinterpret_cast<const void*>(static_cast<std::uintptr_t>(attribute.guest_pointer)));
    }
    return true;
}

bool zbgl_manual_glGetUniformuiv(HostGl& host, HostGl::Call& call) {
    return serve_uniform(host, call, &GlBackend::glGetUniformuiv);
}

bool zbgl_manual_glGetInteger64v(HostGl& host, HostGl::Call& call) {
    return serve_pname(host, call, &GlBackend::glGetInteger64v);
}

bool zbgl_manual_glGetFragDataLocation(HostGl& host, HostGl::Call& call) {
    const GLuint program = call.scalar<GLuint>(0);
    const GLchar* name = guest_string(host, call, call.arg(1));
    if (call.valid()) call.set_result(host.backend().glGetFragDataLocation(program, name));
    return true;
}

bool zbgl_manual_glGetUniformBlockIndex(HostGl& host, HostGl::Call& call) {
    const GLuint program = call.scalar<GLuint>(0);
    const GLchar* name = guest_string(host, call, call.arg(1));
    if (call.valid()) call.set_result(host.backend().glGetUniformBlockIndex(program, name));
    return true;
}

namespace {

// GL_COLOR takes four components; GL_DEPTH and GL_STENCIL take one.
std::uint64_t clear_buffer_elements(GLenum buffer) { return buffer == kGlColorBuffer ? 4 : 1; }

}  // namespace

bool zbgl_manual_glClearBufferiv(HostGl& host, HostGl::Call& call) {
    const GLenum buffer = call.scalar<GLenum>(0);
    const GLint drawbuffer = call.scalar<GLint>(1);
    const GLint* value = call.pointer<const GLint>(2, clear_buffer_elements(buffer), kPageRead);
    if (call.valid()) host.backend().glClearBufferiv(buffer, drawbuffer, value);
    return true;
}

bool zbgl_manual_glClearBufferuiv(HostGl& host, HostGl::Call& call) {
    const GLenum buffer = call.scalar<GLenum>(0);
    const GLint drawbuffer = call.scalar<GLint>(1);
    const GLuint* value = call.pointer<const GLuint>(2, clear_buffer_elements(buffer), kPageRead);
    if (call.valid()) host.backend().glClearBufferuiv(buffer, drawbuffer, value);
    return true;
}

bool zbgl_manual_glClearBufferfv(HostGl& host, HostGl::Call& call) {
    const GLenum buffer = call.scalar<GLenum>(0);
    const GLint drawbuffer = call.scalar<GLint>(1);
    const GLfloat* value = call.pointer<const GLfloat>(2, clear_buffer_elements(buffer), kPageRead);
    if (call.valid()) host.backend().glClearBufferfv(buffer, drawbuffer, value);
    return true;
}

bool zbgl_manual_glGetUniformIndices(HostGl& host, HostGl::Call& call) {
    const GLuint program = call.scalar<GLuint>(0);
    const GLsizei count = call.scalar<GLsizei>(1);
    if (!call.valid()) return true;
    if (count < 0) {
        call.fail(kGlInvalidValue, "uniform name count is negative");
        return true;
    }
    const std::uint64_t items = call.length(count);
    std::vector<const GLchar*> names;
    if (!guest_string_array(host, call, 2, items, names)) return true;
    GLuint* indices = call.pointer<GLuint>(3, items, kPageRead | kPageWrite);
    if (!call.valid()) return true;
    if (items != 0 && indices == nullptr) {
        host.reject(call, kGlInvalidValue, "uniform index output is null");
        return true;
    }
    host.backend().glGetUniformIndices(program, count, names.empty() ? nullptr : names.data(),
                                       indices);
    return true;
}

bool zbgl_manual_glTransformFeedbackVaryings(HostGl& host, HostGl::Call& call) {
    const GLuint program = call.scalar<GLuint>(0);
    const GLsizei count = call.scalar<GLsizei>(1);
    const GLenum bufferMode = call.scalar<GLenum>(3);
    if (!call.valid()) return true;
    if (count < 0) {
        call.fail(kGlInvalidValue, "varying count is negative");
        return true;
    }
    const std::uint64_t items = call.length(count);
    std::vector<const GLchar*> varyings;
    if (!guest_string_array(host, call, 2, items, varyings)) return true;
    host.backend().glTransformFeedbackVaryings(
        program, count, varyings.empty() ? nullptr : varyings.data(), bufferMode);
    return true;
}

bool zbgl_manual_glGetActiveUniformsiv(HostGl& host, HostGl::Call& call) {
    const GLuint program = call.scalar<GLuint>(0);
    const GLsizei uniformCount = call.scalar<GLsizei>(1);
    if (!call.valid()) return true;
    if (uniformCount < 0) {
        call.fail(kGlInvalidValue, "uniform count is negative");
        return true;
    }
    const std::uint64_t items = call.length(uniformCount);
    const GLuint* indices = call.pointer<const GLuint>(2, items, kPageRead);
    const GLenum pname = call.scalar<GLenum>(3);
    // Every GLES 3.0 pname of this query returns one value per named uniform.
    GLint* params = call.pointer<GLint>(4, items, kPageRead | kPageWrite);
    if (!call.valid()) return true;
    host.backend().glGetActiveUniformsiv(program, uniformCount, indices, pname, params);
    return true;
}

bool zbgl_manual_glGetActiveUniformBlockiv(HostGl& host, HostGl::Call& call) {
    const GLuint program = call.scalar<GLuint>(0);
    const GLuint uniformBlockIndex = call.scalar<GLuint>(1);
    const GLenum pname = call.scalar<GLenum>(2);
    if (!call.valid()) return true;
    std::uint64_t elements = 1;
    if (pname == kGlUniformBlockActiveUniformIndices) {
        GLint active = 0;
        host.backend().glGetActiveUniformBlockiv(program, uniformBlockIndex,
                                                 kGlUniformBlockActiveUniforms, &active);
        elements = active > 0 ? static_cast<std::uint64_t>(active) : 0;
    }
    GLint* params = call.pointer<GLint>(3, elements, kPageRead | kPageWrite);
    if (call.valid()) {
        host.backend().glGetActiveUniformBlockiv(program, uniformBlockIndex, pname, params);
    }
    return true;
}

bool zbgl_manual_glFenceSync(HostGl& host, HostGl::Call& call) {
    const GLenum condition = call.scalar<GLenum>(0);
    const GLbitfield flags = call.scalar<GLbitfield>(1);
    if (!call.valid()) return true;
    call.set_result(register_sync(host.backend().glFenceSync(condition, flags)));
    return true;
}

bool zbgl_manual_glIsSync(HostGl& host, HostGl::Call& call) {
    const std::uint32_t handle = call.arg(0);
    if (!call.valid()) return true;
    const GLsync sync = lookup_sync(handle);
    // An unknown handle is GL_FALSE, not an error.
    call.set_result(sync == nullptr ? GLboolean{0} : host.backend().glIsSync(sync));
    return true;
}

bool zbgl_manual_glDeleteSync(HostGl& host, HostGl::Call& call) {
    const std::uint32_t handle = call.arg(0);
    if (!call.valid()) return true;
    if (handle == 0) return true;  // Deleting the zero sync is a no-op, like the real entry point.
    const GLsync sync = release_sync(handle);
    if (sync == nullptr) {
        host.reject(call, kGlInvalidValue, "sync handle is not a live sync object");
        return true;
    }
    host.backend().glDeleteSync(sync);
    return true;
}

bool zbgl_manual_glClientWaitSync(HostGl& host, HostGl::Call& call) {
    GLsync sync = nullptr;
    if (!resolve_sync(host, call, 0, sync)) return true;
    const GLbitfield flags = call.scalar<GLbitfield>(1);
    const GLuint64 timeout = argument64(call, 2);
    if (!call.valid()) return true;
    call.set_result(host.backend().glClientWaitSync(sync, flags, timeout));
    return true;
}

bool zbgl_manual_glWaitSync(HostGl& host, HostGl::Call& call) {
    GLsync sync = nullptr;
    if (!resolve_sync(host, call, 0, sync)) return true;
    const GLbitfield flags = call.scalar<GLbitfield>(1);
    const GLuint64 timeout = argument64(call, 2);
    if (!call.valid()) return true;
    host.backend().glWaitSync(sync, flags, timeout);
    return true;
}

bool zbgl_manual_glGetSynciv(HostGl& host, HostGl::Call& call) {
    GLsync sync = nullptr;
    if (!resolve_sync(host, call, 0, sync)) return true;
    const GLenum pname = call.scalar<GLenum>(1);
    const GLsizei count = call.scalar<GLsizei>(2);
    GLsizei* length = call.pointer<GLsizei>(3, 1, kPageRead | kPageWrite);
    GLint* values = call.pointer<GLint>(4, call.length(count), kPageRead | kPageWrite);
    if (!call.valid()) return true;
    host.backend().glGetSynciv(sync, pname, count, length, values);
    return true;
}

#define ZB_GL_STUB(name)                                                            \
    bool zbgl_manual_##name(HostGl& host, HostGl::Call& call) {                     \
        host.reject(call, kGlInvalidOperation, #name " is not implemented yet");   \
        return true;                                                                \
    }

#undef ZB_GL_STUB

}  // namespace zb
