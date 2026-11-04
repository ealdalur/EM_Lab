#include "FieldAnalyzer.hpp"
#include "SimulationGrid.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace fs = std::filesystem;

namespace {

constexpr int kBlock = 3; // probe readback neighbourhood (texels per axis)
constexpr GLuint kFluxTile = 16;

using Block = std::array<glm::vec4, kBlock * kBlock * kBlock>;

// Trilinear interpolation of one channel of a 3x3x3 block at lattice coordinate `q`
// (in texel-index space, i.e. already shifted by the component's stagger offset).
float sampleBlock(const Block& block, const glm::ivec3& origin, const glm::ivec3& extent,
                  const glm::vec3& q, int channel) {
    const glm::vec3 fl = glm::floor(q);
    const glm::vec3 f = q - fl;
    const glm::ivec3 i0 = glm::ivec3(fl) - origin;

    auto at = [&](int x, int y, int z) {
        x = std::clamp(x, 0, extent.x - 1);
        y = std::clamp(y, 0, extent.y - 1);
        z = std::clamp(z, 0, extent.z - 1);
        return block[static_cast<size_t>((z * kBlock + y) * kBlock + x)][channel];
    };

    float result = 0.0f;
    for (int dz = 0; dz <= 1; ++dz)
        for (int dy = 0; dy <= 1; ++dy)
            for (int dx = 0; dx <= 1; ++dx) {
                const float w = (dx ? f.x : 1.0f - f.x) * (dy ? f.y : 1.0f - f.y) * (dz ? f.z : 1.0f - f.z);
                if (w != 0.0f) result += w * at(i0.x + dx, i0.y + dy, i0.z + dz);
            }
    return result;
}

} // namespace

FluxSurface FluxSurface::plane(int axis, int index, const glm::ivec2& rectMin, const glm::ivec2& rectMax,
                               std::string label) {
    FluxSurface s;
    s.kind = Kind::Plane;
    s.axis = std::clamp(axis, 0, 2);
    s.index = index;
    s.rectMin = rectMin;
    s.rectMax = rectMax;
    s.label = std::move(label);
    return s;
}

FluxSurface FluxSurface::box(const glm::ivec3& boxMin, const glm::ivec3& boxMax, std::string label) {
    FluxSurface s;
    s.kind = Kind::Box;
    s.boxMin = glm::min(boxMin, boxMax);
    s.boxMax = glm::max(boxMin, boxMax);
    s.label = std::move(label);
    return s;
}

FieldAnalyzer::FieldAnalyzer(const fs::path& shaderDir)
    : m_fluxShader(Shader::computeFromFile(shaderDir / "poynting_flux.comp")),
      m_partials(gl::createBuffer()) {}

ProbeSample FieldAnalyzer::probe(const SimulationGrid& grid, const glm::vec3& latticePos) const {
    const glm::ivec3 size = grid.size();
    const glm::vec3 pos = glm::clamp(latticePos, glm::vec3(0.0f), glm::vec3(size - 1));

    const glm::ivec3 origin = glm::clamp(glm::ivec3(glm::floor(pos)) - 1, glm::ivec3(0), glm::max(size - kBlock, glm::ivec3(0)));
    const glm::ivec3 extent = glm::min(glm::ivec3(kBlock), size - origin);

    // Make compute-shader image writes visible to texture readback.
    glMemoryBarrier(GL_TEXTURE_UPDATE_BARRIER_BIT);

    Block eBlock{}, bBlock{};
    const GLsizei bytes = static_cast<GLsizei>(sizeof(Block));
    // Block is laid out with a fixed 3-texel row pitch; extent < 3 only at tiny grids,
    // so read with pack row length/image height set to the block pitch.
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glPixelStorei(GL_PACK_ROW_LENGTH, kBlock);
    glPixelStorei(GL_PACK_IMAGE_HEIGHT, kBlock);
    glGetTextureSubImage(grid.eFieldTexture(), 0, origin.x, origin.y, origin.z, extent.x, extent.y, extent.z,
                         GL_RGBA, GL_FLOAT, bytes, eBlock.data());
    glGetTextureSubImage(grid.bFieldTexture(), 0, origin.x, origin.y, origin.z, extent.x, extent.y, extent.z,
                         GL_RGBA, GL_FLOAT, bytes, bBlock.data());
    glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    glPixelStorei(GL_PACK_IMAGE_HEIGHT, 0);

    ProbeSample s;
    s.position = pos;
    // Yee stagger offsets (see common.glsl).
    s.E.x = sampleBlock(eBlock, origin, extent, pos - glm::vec3(0.5f, 0.0f, 0.0f), 0);
    s.E.y = sampleBlock(eBlock, origin, extent, pos - glm::vec3(0.0f, 0.5f, 0.0f), 1);
    s.E.z = sampleBlock(eBlock, origin, extent, pos - glm::vec3(0.0f, 0.0f, 0.5f), 2);
    s.B.x = sampleBlock(bBlock, origin, extent, pos - glm::vec3(0.0f, 0.5f, 0.5f), 0);
    s.B.y = sampleBlock(bBlock, origin, extent, pos - glm::vec3(0.5f, 0.0f, 0.5f), 1);
    s.B.z = sampleBlock(bBlock, origin, extent, pos - glm::vec3(0.5f, 0.5f, 0.0f), 2);
    s.S = glm::cross(s.E, s.B);

    // Materials are cell-constant: take the texel containing the probe.
    const glm::ivec3 cell = glm::clamp(glm::ivec3(glm::floor(pos)) - origin, glm::ivec3(0), extent - 1);
    const size_t ci = static_cast<size_t>((cell.z * kBlock + cell.y) * kBlock + cell.x);
    s.sigma = eBlock[ci].w;
    s.epsilonR = bBlock[ci].w;
    return s;
}

double FieldAnalyzer::planeFlux(const SimulationGrid& grid, int axis, int index, glm::ivec2 rectMin,
                                glm::ivec2 rectMax) const {
    const glm::ivec3 size = grid.size();
    if (index < 0 || index >= size[axis]) return 0.0;

    const int u = axis == 0 ? 1 : 0;
    const int v = axis == 2 ? 1 : 2;
    rectMin = glm::max(rectMin, glm::ivec2(0));
    rectMax = glm::min(rectMax, glm::ivec2(size[u], size[v]));
    const glm::ivec2 rectSize = rectMax - rectMin;
    if (rectSize.x <= 0 || rectSize.y <= 0) return 0.0;

    const GLuint gx = gl::divUp(static_cast<GLuint>(rectSize.x), kFluxTile);
    const GLuint gy = gl::divUp(static_cast<GLuint>(rectSize.y), kFluxTile);
    const size_t groups = static_cast<size_t>(gx) * gy;

    if (groups > m_partialCapacity) {
        glNamedBufferData(m_partials.get(), static_cast<GLsizeiptr>(groups * sizeof(float)), nullptr, GL_DYNAMIC_READ);
        m_partialCapacity = groups;
    }

    m_fluxShader.set("uGridSize", size);
    m_fluxShader.set("uAxis", axis);
    m_fluxShader.set("uPlaneIndex", index);
    m_fluxShader.set("uRectMin", rectMin);
    m_fluxShader.set("uRectSize", rectSize);

    m_fluxShader.use();
    glBindTextureUnit(0, grid.eFieldTexture());
    glBindTextureUnit(1, grid.bFieldTexture());
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, m_partials.get());
    glDispatchCompute(gx, gy, 1);
    glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);

    m_partialScratch.resize(groups);
    glGetNamedBufferSubData(m_partials.get(), 0, static_cast<GLsizeiptr>(groups * sizeof(float)),
                            m_partialScratch.data());

    double sum = 0.0;
    for (float p : m_partialScratch) sum += static_cast<double>(p);
    return sum;
}

double FieldAnalyzer::measureFlux(const SimulationGrid& grid, const FluxSurface& s) const {
    if (s.kind == FluxSurface::Kind::Plane) return planeFlux(grid, s.axis, s.index, s.rectMin, s.rectMax);

    // Closed box: sum the six faces with outward normals.
    double total = 0.0;
    for (int axis = 0; axis < 3; ++axis) {
        const int u = axis == 0 ? 1 : 0;
        const int v = axis == 2 ? 1 : 2;
        const glm::ivec2 rMin(s.boxMin[u], s.boxMin[v]);
        const glm::ivec2 rMax(s.boxMax[u] + 1, s.boxMax[v] + 1);
        total -= planeFlux(grid, axis, s.boxMin[axis], rMin, rMax); // outward = -axis
        total += planeFlux(grid, axis, s.boxMax[axis], rMin, rMax); // outward = +axis
    }
    return total;
}
