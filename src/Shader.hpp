#pragma once
// GLSL program wrapper: compiles graphics (vertex + fragment) or compute programs
// from files, resolves `#include "file"` directives, and caches uniform locations.

#include "GLUtils.hpp"

#include <glm/glm.hpp>

#include <filesystem>
#include <string>
#include <unordered_map>

class Shader {
public:
    // Throws std::runtime_error with the full compiler/linker log on failure.
    static Shader fromFiles(const std::filesystem::path& vertexPath,
                            const std::filesystem::path& fragmentPath);
    static Shader computeFromFile(const std::filesystem::path& computePath);

    Shader() = default;
    Shader(Shader&&) noexcept = default;
    Shader& operator=(Shader&&) noexcept = default;
    Shader(const Shader&) = delete;
    Shader& operator=(const Shader&) = delete;

    void use() const { glUseProgram(m_program.get()); }
    [[nodiscard]] GLuint id() const noexcept { return m_program.get(); }
    [[nodiscard]] bool valid() const noexcept { return static_cast<bool>(m_program); }

    // Uniform setters operate through DSA (glProgramUniform*) so they work
    // regardless of which program is currently bound.
    void set(const char* name, int v) const;
    void set(const char* name, float v) const;
    void set(const char* name, const glm::vec2& v) const;
    void set(const char* name, const glm::vec3& v) const;
    void set(const char* name, const glm::vec4& v) const;
    void set(const char* name, const glm::ivec2& v) const;
    void set(const char* name, const glm::ivec3& v) const;
    void set(const char* name, const glm::mat4& m) const;
    // int[] uniform array; `name` is the array name without subscript.
    void setArray(const char* name, const int* values, int count) const;

    // Compute workgroup size declared in the shader (layout(local_size_*)).
    [[nodiscard]] glm::uvec3 workGroupSize() const;

    // Dispatch enough groups to cover `invocations` threads (bounds-checked in shader).
    void dispatchCovering(const glm::uvec3& invocations) const;

private:
    explicit Shader(GLuint program) : m_program(program) {}
    GLint location(const char* name) const;

    gl::Program m_program;
    mutable std::unordered_map<std::string, GLint> m_locations;
    mutable glm::uvec3 m_localSize{0u}; // cached compute local size (0 = not queried yet)
};
