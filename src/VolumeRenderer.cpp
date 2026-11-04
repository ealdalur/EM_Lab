#include "VolumeRenderer.hpp"

#include "Camera.hpp"
#include "SimulationGrid.hpp"

#include <glm/gtc/matrix_inverse.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace fs = std::filesystem;

const char* RenderSettings::modeName(Mode m) {
    switch (m) {
        case Mode::Fields:   return "E (cyan) + B (magenta)";
        case Mode::EOnly:    return "|E| only";
        case Mode::BOnly:    return "|B| only";
        case Mode::Poynting: return "Poynting |E x B|";
        case Mode::SignedE:  return "signed E component";
        default:             return "?";
    }
}

VolumeRenderer::VolumeRenderer(const fs::path& shaderDir)
    : m_raymarch(Shader::fromFiles(shaderDir / "fullscreen.vert", shaderDir / "raymarch.frag")),
      m_lines(Shader::fromFiles(shaderDir / "lines.vert", shaderDir / "lines.frag")),
      m_emptyVao(gl::createVertexArray()),
      m_lineVao(gl::createVertexArray()),
      m_lineVbo(gl::createBuffer()) {
    const GLuint vao = m_lineVao.get();
    glVertexArrayVertexBuffer(vao, 0, m_lineVbo.get(), 0, sizeof(LineVertex));
    glEnableVertexArrayAttrib(vao, 0);
    glVertexArrayAttribFormat(vao, 0, 3, GL_FLOAT, GL_FALSE, offsetof(LineVertex, position));
    glVertexArrayAttribBinding(vao, 0, 0);
    glEnableVertexArrayAttrib(vao, 1);
    glVertexArrayAttribFormat(vao, 1, 4, GL_FLOAT, GL_FALSE, offsetof(LineVertex, color));
    glVertexArrayAttribBinding(vao, 1, 0);
}

// -----------------------------------------------------------------------------
// Mapping
// -----------------------------------------------------------------------------
float VolumeRenderer::worldScale(const glm::ivec3& g) {
    const int maxDim = std::max({g.x, g.y, g.z, 1});
    return 2.0f / static_cast<float>(maxDim);
}

glm::vec3 VolumeRenderer::latticeToWorld(const glm::ivec3& g, const glm::vec3& p) {
    return (p - 0.5f * glm::vec3(g)) * worldScale(g);
}

glm::vec3 VolumeRenderer::worldMin(const glm::ivec3& g) { return -0.5f * glm::vec3(g) * worldScale(g); }
glm::vec3 VolumeRenderer::worldSize(const glm::ivec3& g) { return glm::vec3(g) * worldScale(g); }

// -----------------------------------------------------------------------------
// Volume pass
// -----------------------------------------------------------------------------
void VolumeRenderer::ensureOffscreen(const glm::ivec2& size) {
    if (m_fbo && size == m_fboSize) return;
    m_fboColor = gl::createTexture(GL_TEXTURE_2D);
    glTextureStorage2D(m_fboColor.get(), 1, GL_RGBA8, size.x, size.y);
    glTextureParameteri(m_fboColor.get(), GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTextureParameteri(m_fboColor.get(), GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    m_fbo = gl::createFramebuffer();
    glNamedFramebufferTexture(m_fbo.get(), GL_COLOR_ATTACHMENT0, m_fboColor.get(), 0);
    m_fboSize = size;
}

void VolumeRenderer::render(const Camera& camera, const SimulationGrid& grid, const RenderSettings& rs,
                            const glm::ivec2& viewport) {
    if (viewport.x <= 0 || viewport.y <= 0) return;

    const float scale = std::clamp(rs.resolutionScale, 0.25f, 1.0f);
    const bool offscreen = scale < 0.999f;
    const glm::ivec2 target = offscreen
        ? glm::max(glm::ivec2(glm::round(glm::vec2(viewport) * scale)), glm::ivec2(1))
        : viewport;

    if (offscreen) {
        ensureOffscreen(target);
        glBindFramebuffer(GL_FRAMEBUFFER, m_fbo.get());
    }
    glViewport(0, 0, target.x, target.y);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);

    const glm::ivec3 gs = grid.size();
    const glm::vec3 volMin = worldMin(gs);
    const glm::vec3 volSize = worldSize(gs);
    const glm::vec3 cMin = glm::clamp(glm::min(rs.clipMin, rs.clipMax), glm::vec3(0.0f), glm::vec3(1.0f));
    const glm::vec3 cMax = glm::clamp(glm::max(rs.clipMin, rs.clipMax), glm::vec3(0.0f), glm::vec3(1.0f));

    const Shader& s = m_raymarch;
    s.set("uInvViewProj", glm::inverse(camera.viewProjection()));
    s.set("uCamPos", camera.position());
    s.set("uVolMin", volMin);
    s.set("uVolSize", volSize);
    s.set("uClipMin", volMin + cMin * volSize);
    s.set("uClipMax", volMin + cMax * volSize);
    s.set("uGridSize", glm::vec3(gs));
    s.set("uStepVoxels", std::clamp(rs.stepVoxels, 0.1f, 4.0f));
    s.set("uGainE", rs.gainE);
    s.set("uGainB", rs.gainB);
    s.set("uDensity", rs.density);
    s.set("uEmission", rs.emission);
    s.set("uMaterialOpacity", rs.materialOpacity);
    s.set("uExposure", rs.exposure);
    s.set("uMode", static_cast<int>(rs.mode));
    s.set("uSignedAxis", std::clamp(rs.signedAxis, 0, 2));

    s.use();
    glBindTextureUnit(0, grid.eFieldTexture());
    glBindTextureUnit(1, grid.bFieldTexture());
    glBindVertexArray(m_emptyVao.get());
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);

    if (offscreen) {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glBlitNamedFramebuffer(m_fbo.get(), 0, 0, 0, target.x, target.y, 0, 0, viewport.x, viewport.y,
                               GL_COLOR_BUFFER_BIT, GL_LINEAR);
        glViewport(0, 0, viewport.x, viewport.y);
    }
}

// -----------------------------------------------------------------------------
// Overlay
// -----------------------------------------------------------------------------
void VolumeRenderer::drawLines(const Camera& camera, const std::vector<LineVertex>& lines) {
    if (lines.empty()) return;
    const size_t bytes = lines.size() * sizeof(LineVertex);
    if (bytes > m_lineCapacity) {
        m_lineCapacity = bytes * 2;
        glNamedBufferData(m_lineVbo.get(), static_cast<GLsizeiptr>(m_lineCapacity), nullptr, GL_DYNAMIC_DRAW);
    }
    glNamedBufferSubData(m_lineVbo.get(), 0, static_cast<GLsizeiptr>(bytes), lines.data());

    m_lines.set("uViewProj", camera.viewProjection());
    m_lines.use();
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE);
    glBindVertexArray(m_lineVao.get());
    glDrawArrays(GL_LINES, 0, static_cast<GLsizei>(lines.size()));
    glBindVertexArray(0);
    glDisable(GL_BLEND);
}

void VolumeRenderer::appendBox(std::vector<LineVertex>& out, const glm::vec3& mn, const glm::vec3& mx,
                               const glm::vec4& color) {
    const glm::vec3 c[8] = {{mn.x, mn.y, mn.z}, {mx.x, mn.y, mn.z}, {mx.x, mx.y, mn.z}, {mn.x, mx.y, mn.z},
                            {mn.x, mn.y, mx.z}, {mx.x, mn.y, mx.z}, {mx.x, mx.y, mx.z}, {mn.x, mx.y, mx.z}};
    static constexpr int edges[12][2] = {{0, 1}, {1, 2}, {2, 3}, {3, 0}, {4, 5}, {5, 6},
                                         {6, 7}, {7, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
    for (const auto& e : edges) {
        out.push_back({c[e[0]], color});
        out.push_back({c[e[1]], color});
    }
}

void VolumeRenderer::appendCross(std::vector<LineVertex>& out, const glm::vec3& c, float h, const glm::vec4& color) {
    for (int a = 0; a < 3; ++a) {
        glm::vec3 d(0.0f);
        d[a] = h;
        out.push_back({c - d, color});
        out.push_back({c + d, color});
    }
}

void VolumeRenderer::buildGridOverlay(std::vector<LineVertex>& out, const glm::ivec3& g, int pml) {
    out.clear();
    const glm::vec3 n(g);
    appendBox(out, latticeToWorld(g, glm::vec3(0.0f)), latticeToWorld(g, n), {0.75f, 0.8f, 0.9f, 0.30f});
    if (pml > 0)
        appendBox(out, latticeToWorld(g, glm::vec3(static_cast<float>(pml))),
                  latticeToWorld(g, n - static_cast<float>(pml)), {0.3f, 0.6f, 1.0f, 0.12f});
}

void VolumeRenderer::appendSources(std::vector<LineVertex>& out, const glm::ivec3& g,
                                   const std::vector<SourceDesc>& sources) {
    const glm::vec4 color{1.0f, 0.45f, 0.3f, 0.6f};
    const glm::vec3 n(g);
    for (const SourceDesc& s : sources) {
        const glm::vec3 lo = glm::clamp(s.center - s.halfExtent, glm::vec3(0.0f), n);
        const glm::vec3 hi = glm::clamp(s.center + s.halfExtent, glm::vec3(0.0f), n);
        const glm::vec3 ext = hi - lo;
        const int extended = (ext.x > 0.5f) + (ext.y > 0.5f) + (ext.z > 0.5f);
        if (extended == 0) {
            appendCross(out, latticeToWorld(g, s.center), 2.0f * worldScale(g), color);
        } else if (extended == 1) {
            out.push_back({latticeToWorld(g, lo), color});
            out.push_back({latticeToWorld(g, hi), color});
        } else {
            appendBox(out, latticeToWorld(g, lo), latticeToWorld(g, hi), color);
        }
    }
}
