#pragma once
// Small RAII helpers around OpenGL object names plus debug-output plumbing.

#include <glad/glad.h>

#include <cstdio>
#include <utility>

namespace gl {

// Move-only owner of a single GL object name. `Deleter` is a stateless functor
// that releases the name (e.g. glDeleteTextures(1, &id)).
template <typename Deleter>
class Handle {
public:
    Handle() = default;
    explicit Handle(GLuint id) noexcept : m_id(id) {}
    ~Handle() { reset(); }

    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;

    Handle(Handle&& other) noexcept : m_id(std::exchange(other.m_id, 0u)) {}
    Handle& operator=(Handle&& other) noexcept {
        if (this != &other) {
            reset();
            m_id = std::exchange(other.m_id, 0u);
        }
        return *this;
    }

    void reset(GLuint id = 0) noexcept {
        if (m_id != 0) Deleter{}(m_id);
        m_id = id;
    }

    [[nodiscard]] GLuint get() const noexcept { return m_id; }
    [[nodiscard]] explicit operator bool() const noexcept { return m_id != 0; }

private:
    GLuint m_id = 0;
};

struct TextureDeleter     { void operator()(GLuint id) const noexcept { glDeleteTextures(1, &id); } };
struct BufferDeleter      { void operator()(GLuint id) const noexcept { glDeleteBuffers(1, &id); } };
struct VertexArrayDeleter { void operator()(GLuint id) const noexcept { glDeleteVertexArrays(1, &id); } };
struct ProgramDeleter     { void operator()(GLuint id) const noexcept { glDeleteProgram(id); } };
struct FramebufferDeleter { void operator()(GLuint id) const noexcept { glDeleteFramebuffers(1, &id); } };

using Texture     = Handle<TextureDeleter>;
using Buffer      = Handle<BufferDeleter>;
using VertexArray = Handle<VertexArrayDeleter>;
using Program     = Handle<ProgramDeleter>;
using Framebuffer = Handle<FramebufferDeleter>;

inline Texture createTexture(GLenum target) {
    GLuint id = 0;
    glCreateTextures(target, 1, &id);
    return Texture(id);
}

inline Buffer createBuffer() {
    GLuint id = 0;
    glCreateBuffers(1, &id);
    return Buffer(id);
}

// Non-blocking GPU -> CPU readback: a persistently mapped, coherent buffer. The GPU copies
// results into it; once a fence placed after the copy has signalled, the CPU reads the
// mapped memory directly. No glGet*Data call, so the driver never drains its queue.
class ReadbackBuffer {
public:
    void ensure(size_t bytes) {
        if (bytes <= m_size && m_ptr) return;
        const GLbitfield flags = GL_MAP_READ_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;
        m_buffer = createBuffer(); // the old buffer (and its mapping) is released
        glNamedBufferStorage(m_buffer.get(), static_cast<GLsizeiptr>(bytes), nullptr, flags);
        m_ptr = glMapNamedBufferRange(m_buffer.get(), 0, static_cast<GLsizeiptr>(bytes), flags);
        m_size = bytes;
    }
    // Queue a copy from `src`; follow with fence() and read data() once it has signalled.
    void copyFrom(GLuint src, size_t bytes, size_t srcOffset = 0, size_t dstOffset = 0) {
        ensure(dstOffset + bytes);
        glCopyNamedBufferSubData(src, m_buffer.get(), static_cast<GLintptr>(srcOffset), static_cast<GLintptr>(dstOffset),
                                 static_cast<GLsizeiptr>(bytes));
    }
    [[nodiscard]] const void* data() const noexcept { return m_ptr; }

private:
    Buffer m_buffer;
    void* m_ptr = nullptr;
    size_t m_size = 0;
};

// Fence after queued copies into ReadbackBuffers (makes the writes visible to the mapping).
inline GLsync readbackFence() {
    glMemoryBarrier(GL_CLIENT_MAPPED_BUFFER_BARRIER_BIT);
    GLsync f = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    glFlush();
    return f;
}

inline Framebuffer createFramebuffer() {
    GLuint id = 0;
    glCreateFramebuffers(1, &id);
    return Framebuffer(id);
}

inline VertexArray createVertexArray() {
    GLuint id = 0;
    glCreateVertexArrays(1, &id);
    return VertexArray(id);
}

// Ceil-divide used for compute dispatch sizes.
constexpr GLuint divUp(GLuint value, GLuint divisor) noexcept {
    return (value + divisor - 1u) / divisor;
}

inline void APIENTRY debugCallback(GLenum source, GLenum type, GLuint id, GLenum severity,
                                   GLsizei /*length*/, const GLchar* message, const void* /*user*/) {
    if (severity == GL_DEBUG_SEVERITY_NOTIFICATION) return;
    // Buffer/texture placement hints from NVIDIA drivers are pure noise. 131186 is the
    // (intentional, few-hundred-byte) flux partial-sum readback to host memory.
    if (id == 131185 || id == 131186 || id == 131218 || id == 131204) return;

    const char* sev = severity == GL_DEBUG_SEVERITY_HIGH     ? "HIGH"
                    : severity == GL_DEBUG_SEVERITY_MEDIUM   ? "MEDIUM"
                                                             : "LOW";
    const char* typ = type == GL_DEBUG_TYPE_ERROR               ? "ERROR"
                    : type == GL_DEBUG_TYPE_PERFORMANCE         ? "PERFORMANCE"
                    : type == GL_DEBUG_TYPE_DEPRECATED_BEHAVIOR ? "DEPRECATED"
                    : type == GL_DEBUG_TYPE_UNDEFINED_BEHAVIOR  ? "UNDEFINED"
                                                                : "OTHER";
    (void)source;
    std::fprintf(stderr, "[GL %s/%s #%u] %s\n", sev, typ, id, message);
}

inline void enableDebugOutput() {
    GLint flags = 0;
    glGetIntegerv(GL_CONTEXT_FLAGS, &flags);
    if (!(flags & GL_CONTEXT_FLAG_DEBUG_BIT)) return;
    glEnable(GL_DEBUG_OUTPUT);
    glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
    glDebugMessageCallback(debugCallback, nullptr);
    glDebugMessageControl(GL_DONT_CARE, GL_DONT_CARE, GL_DONT_CARE, 0, nullptr, GL_TRUE);
}

} // namespace gl
