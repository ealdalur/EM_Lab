#include "Shader.hpp"

#include <glm/gtc/type_ptr.hpp>

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <unordered_set>
#include <vector>

namespace fs = std::filesystem;

namespace {

std::string readFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("Shader: cannot open '" + path.string() + "'");
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Minimal preprocessor: expands `#include "relative/path"` recursively (each file
// at most once) and emits `#line` directives so compiler errors map back to the
// right source line. Included files must not contain a #version directive.
void expandIncludes(const fs::path& path, std::unordered_set<std::string>& visited,
                    std::ostringstream& out, bool isRoot) {
    const std::string canonical = fs::weakly_canonical(path).string();
    if (!visited.insert(canonical).second) return; // include guard

    std::istringstream src(readFile(path));
    std::string line;
    int lineNo = 0;
    bool versionSeen = !isRoot;
    while (std::getline(src, line)) {
        ++lineNo;
        if (!line.empty() && line.back() == '\r') line.pop_back();

        const auto first = line.find_first_not_of(" \t");
        const bool isInclude = first != std::string::npos && line.compare(first, 8, "#include") == 0;
        if (isInclude) {
            const auto q0 = line.find('"', first);
            const auto q1 = (q0 == std::string::npos) ? q0 : line.find('"', q0 + 1);
            if (q1 == std::string::npos)
                throw std::runtime_error("Shader: malformed #include in " + path.string() + ":" +
                                         std::to_string(lineNo));
            const fs::path inc = path.parent_path() / line.substr(q0 + 1, q1 - q0 - 1);
            out << "#line 1\n";
            expandIncludes(inc, visited, out, false);
            out << "#line " << (lineNo + 1) << "\n";
            continue;
        }
        out << line << '\n';
        if (!versionSeen && first != std::string::npos && line.compare(first, 8, "#version") == 0) {
            versionSeen = true;
            out << "#line " << (lineNo + 1) << "\n";
        }
    }
}

std::string loadSource(const fs::path& path) {
    std::unordered_set<std::string> visited;
    std::ostringstream out;
    expandIncludes(path, visited, out, true);
    return out.str();
}

GLuint compileStage(GLenum stage, const fs::path& path) {
    const std::string source = loadSource(path);
    const char* src = source.c_str();

    GLuint shader = glCreateShader(stage);
    glShaderSource(shader, 1, &src, nullptr);
    glCompileShader(shader);

    GLint ok = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (ok != GL_TRUE) {
        GLint len = 0;
        glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &len);
        std::vector<char> log(static_cast<size_t>(len > 1 ? len : 1));
        glGetShaderInfoLog(shader, len, nullptr, log.data());
        glDeleteShader(shader);
        throw std::runtime_error("Shader compile error in '" + path.string() + "':\n" + log.data());
    }
    return shader;
}

GLuint linkProgram(const std::vector<GLuint>& stages, const std::string& label) {
    GLuint program = glCreateProgram();
    for (GLuint s : stages) glAttachShader(program, s);
    glLinkProgram(program);
    for (GLuint s : stages) {
        glDetachShader(program, s);
        glDeleteShader(s);
    }

    GLint ok = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (ok != GL_TRUE) {
        GLint len = 0;
        glGetProgramiv(program, GL_INFO_LOG_LENGTH, &len);
        std::vector<char> log(static_cast<size_t>(len > 1 ? len : 1));
        glGetProgramInfoLog(program, len, nullptr, log.data());
        glDeleteProgram(program);
        throw std::runtime_error("Shader link error (" + label + "):\n" + log.data());
    }
    return program;
}

} // namespace

Shader Shader::fromFiles(const fs::path& vertexPath, const fs::path& fragmentPath) {
    GLuint vs = compileStage(GL_VERTEX_SHADER, vertexPath);
    GLuint fs = 0;
    try {
        fs = compileStage(GL_FRAGMENT_SHADER, fragmentPath);
    } catch (...) {
        glDeleteShader(vs);
        throw;
    }
    return Shader(linkProgram({vs, fs}, vertexPath.filename().string() + " + " +
                                            fragmentPath.filename().string()));
}

Shader Shader::computeFromFile(const fs::path& computePath) {
    GLuint cs = compileStage(GL_COMPUTE_SHADER, computePath);
    return Shader(linkProgram({cs}, computePath.filename().string()));
}

GLint Shader::location(const char* name) const {
    auto it = m_locations.find(name);
    if (it != m_locations.end()) return it->second;
    const GLint loc = glGetUniformLocation(m_program.get(), name);
    m_locations.emplace(name, loc);
    return loc; // -1 (optimized away / absent) is silently ignored by glProgramUniform*
}

void Shader::set(const char* n, int v) const               { glProgramUniform1i(id(), location(n), v); }
void Shader::set(const char* n, float v) const             { glProgramUniform1f(id(), location(n), v); }
void Shader::set(const char* n, const glm::vec2& v) const  { glProgramUniform2fv(id(), location(n), 1, glm::value_ptr(v)); }
void Shader::set(const char* n, const glm::vec3& v) const  { glProgramUniform3fv(id(), location(n), 1, glm::value_ptr(v)); }
void Shader::set(const char* n, const glm::vec4& v) const  { glProgramUniform4fv(id(), location(n), 1, glm::value_ptr(v)); }
void Shader::set(const char* n, const glm::ivec2& v) const { glProgramUniform2iv(id(), location(n), 1, glm::value_ptr(v)); }
void Shader::set(const char* n, const glm::ivec3& v) const { glProgramUniform3iv(id(), location(n), 1, glm::value_ptr(v)); }
void Shader::set(const char* n, const glm::mat4& m) const  { glProgramUniformMatrix4fv(id(), location(n), 1, GL_FALSE, glm::value_ptr(m)); }

void Shader::setArray(const char* n, const int* values, int count) const {
    glProgramUniform1iv(id(), location(n), count, values);
}

glm::uvec3 Shader::workGroupSize() const {
    if (m_localSize.x != 0u) return m_localSize;
    GLint size[3] = {1, 1, 1};
    glGetProgramiv(id(), GL_COMPUTE_WORK_GROUP_SIZE, size);
    m_localSize = {static_cast<unsigned>(size[0]), static_cast<unsigned>(size[1]),
                   static_cast<unsigned>(size[2])};
    return m_localSize;
}

void Shader::dispatchCovering(const glm::uvec3& invocations) const {
    const glm::uvec3 local = workGroupSize();
    glDispatchCompute(gl::divUp(invocations.x, local.x),
                      gl::divUp(invocations.y, local.y),
                      gl::divUp(invocations.z, local.z));
}
