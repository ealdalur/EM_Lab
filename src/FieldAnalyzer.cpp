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
      m_gatherShader(Shader::computeFromFile(shaderDir / "probe_gather.comp")),
      m_partials(gl::createBuffer()) {
    for (Slot& s : m_slots) {
        s.origins = gl::createBuffer();
        s.blocks = gl::createBuffer();
        s.partials = gl::createBuffer();
    }
}

FieldAnalyzer::~FieldAnalyzer() { discard(); }

void FieldAnalyzer::discard() {
    for (Slot& s : m_slots) {
        if (s.fence) glDeleteSync(s.fence);
        s.fence = nullptr;
        s.busy = false;
    }
}

bool FieldAnalyzer::pending() const noexcept { return m_slots[0].busy || m_slots[1].busy; }

size_t FieldAnalyzer::planeGroups(const SimulationGrid& grid, int axis, glm::ivec2 rectMin, glm::ivec2 rectMax) {
    const glm::ivec3 size = grid.size();
    const int u = axis == 0 ? 1 : 0;
    const int v = axis == 2 ? 1 : 2;
    rectMin = glm::max(rectMin, glm::ivec2(0));
    rectMax = glm::min(rectMax, glm::ivec2(size[u], size[v]));
    const glm::ivec2 rectSize = rectMax - rectMin;
    if (rectSize.x <= 0 || rectSize.y <= 0) return 0;
    return static_cast<size_t>(gl::divUp(static_cast<GLuint>(rectSize.x), kFluxTile)) *
           gl::divUp(static_cast<GLuint>(rectSize.y), kFluxTile);
}

size_t FieldAnalyzer::dispatchPlane(const SimulationGrid& grid, int axis, int index, glm::ivec2 rectMin,
                                    glm::ivec2 rectMax, GLuint partials, size_t offset) const {
    const glm::ivec3 size = grid.size();
    if (index < 0 || index >= size[axis]) return 0;
    const int u = axis == 0 ? 1 : 0;
    const int v = axis == 2 ? 1 : 2;
    rectMin = glm::max(rectMin, glm::ivec2(0));
    rectMax = glm::min(rectMax, glm::ivec2(size[u], size[v]));
    const glm::ivec2 rectSize = rectMax - rectMin;
    if (rectSize.x <= 0 || rectSize.y <= 0) return 0;
    const GLuint gx = gl::divUp(static_cast<GLuint>(rectSize.x), kFluxTile);
    const GLuint gy = gl::divUp(static_cast<GLuint>(rectSize.y), kFluxTile);
    m_fluxShader.set("uGridSize", size);
    m_fluxShader.set("uAxis", axis);
    m_fluxShader.set("uPlaneIndex", index);
    m_fluxShader.set("uRectMin", rectMin);
    m_fluxShader.set("uRectSize", rectSize);
    m_fluxShader.set("uPartialOffset", static_cast<int>(offset));
    m_fluxShader.use();
    glBindTextureUnit(0, grid.eFieldTexture());
    glBindTextureUnit(1, grid.bFieldTexture());
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, partials);
    glDispatchCompute(gx, gy, 1);
    return static_cast<size_t>(gx) * gy;
}

void FieldAnalyzer::submit(const SimulationGrid& grid, const MeasureRequest& request) {
    // A free slot, or the oldest one (whose result is then dropped: it is out of date anyway).
    Slot* slot = !m_slots[0].busy ? &m_slots[0] : (!m_slots[1].busy ? &m_slots[1] : nullptr);
    if (!slot) {
        slot = m_slots[0].serial < m_slots[1].serial ? &m_slots[0] : &m_slots[1];
        if (slot->fence) glDeleteSync(slot->fence);
        slot->fence = nullptr;
    }
    Slot& s = *slot;
    s.request = request;
    s.serial = ++m_serial;
    s.origins3.clear();
    s.extents.clear();
    s.planes.clear();
    s.partialCount = 0;
    const glm::ivec3 size = grid.size();
    glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT | GL_SHADER_STORAGE_BARRIER_BIT);

    // Probes: one gather pass for all of them.
    if (!request.probes.empty()) {
        std::vector<glm::ivec4> origins;
        for (const glm::vec3& lp : request.probes) {
            const glm::vec3 pos = glm::clamp(lp, glm::vec3(0.0f), glm::vec3(size - 1));
            const glm::ivec3 origin =
                glm::clamp(glm::ivec3(glm::floor(pos)) - 1, glm::ivec3(0), glm::max(size - kBlock, glm::ivec3(0)));
            s.origins3.push_back(origin);
            s.extents.push_back(glm::min(glm::ivec3(kBlock), size - origin));
            origins.emplace_back(origin, 0);
        }
        const size_t n = origins.size();
        if (n > s.originCap) {
            s.originCap = n;
            glNamedBufferData(s.origins.get(), static_cast<GLsizeiptr>(n * sizeof(glm::ivec4)), nullptr, GL_DYNAMIC_DRAW);
        }
        if (n * 54 > s.blockCap) {
            s.blockCap = n * 54;
            glNamedBufferData(s.blocks.get(), static_cast<GLsizeiptr>(s.blockCap * sizeof(glm::vec4)), nullptr, GL_DYNAMIC_COPY);
        }
        glNamedBufferSubData(s.origins.get(), 0, static_cast<GLsizeiptr>(n * sizeof(glm::ivec4)), origins.data());
        m_gatherShader.set("uProbeCount", static_cast<int>(n));
        m_gatherShader.set("uGridSize", size);
        m_gatherShader.use();
        glBindTextureUnit(0, grid.eFieldTexture());
        glBindTextureUnit(1, grid.bFieldTexture());
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, s.origins.get());
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, s.blocks.get());
        glDispatchCompute(gl::divUp(static_cast<GLuint>(n * 54), 64u), 1, 1);
    }

    // Flux surfaces: every plane's partial sums into one buffer.
    if (!request.fluxes.empty()) {
        struct PlaneSpec { size_t flux; double sign; int axis, index; glm::ivec2 mn, mx; };
        std::vector<PlaneSpec> specs;
        for (size_t f = 0; f < request.fluxes.size(); ++f) {
            const FluxSurface& fs = request.fluxes[f];
            if (fs.kind == FluxSurface::Kind::Plane) {
                specs.push_back({f, 1.0, fs.axis, fs.index, fs.rectMin, fs.rectMax});
                continue;
            }
            for (int axis = 0; axis < 3; ++axis) {
                const int u = axis == 0 ? 1 : 0;
                const int v = axis == 2 ? 1 : 2;
                const glm::ivec2 rMin(fs.boxMin[u], fs.boxMin[v]);
                const glm::ivec2 rMax(fs.boxMax[u] + 1, fs.boxMax[v] + 1);
                specs.push_back({f, -1.0, axis, fs.boxMin[axis], rMin, rMax}); // outward = -axis
                specs.push_back({f, +1.0, axis, fs.boxMax[axis], rMin, rMax}); // outward = +axis
            }
        }
        size_t total = 0;
        for (const PlaneSpec& p : specs) total += planeGroups(grid, p.axis, p.mn, p.mx);
        if (total > s.partialCap) {
            s.partialCap = total;
            glNamedBufferData(s.partials.get(), static_cast<GLsizeiptr>(std::max<size_t>(total, 1) * sizeof(float)),
                              nullptr, GL_DYNAMIC_COPY);
        }
        size_t offset = 0;
        for (const PlaneSpec& p : specs) {
            const size_t groups = dispatchPlane(grid, p.axis, p.index, p.mn, p.mx, s.partials.get(), offset);
            s.planes.push_back({p.flux, p.sign, offset, groups});
            offset += groups;
        }
        s.partialCount = offset;
    }
    glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
    const size_t blockBytes = s.request.probes.size() * 54 * sizeof(glm::vec4);
    const size_t partialBytes = s.partialCount * sizeof(float);
    s.readback.ensure(std::max<size_t>(blockBytes + partialBytes, 16));
    if (blockBytes) s.readback.copyFrom(s.blocks.get(), blockBytes, 0, 0);
    if (partialBytes) s.readback.copyFrom(s.partials.get(), partialBytes, 0, blockBytes);
    s.fence = gl::readbackFence();
    s.busy = true;
}

bool FieldAnalyzer::collect(MeasureResult& out, bool wait) {
    Slot* s = nullptr;
    for (Slot& c : m_slots)
        if (c.busy && (!s || c.serial < s->serial)) s = &c;
    if (!s) return false;
    const GLenum state = glClientWaitSync(s->fence, 0, wait ? GLuint64(5'000'000'000) : 0);
    if (state != GL_ALREADY_SIGNALED && state != GL_CONDITION_SATISFIED) return false;
    glDeleteSync(s->fence);
    s->fence = nullptr;
    s->busy = false;

    out = MeasureResult{};
    out.time = s->request.time;
    out.step = s->request.step;
    const size_t n = s->request.probes.size();
    if (n > 0) {
        const auto* mapped = static_cast<const glm::vec4*>(s->readback.data());
        const std::vector<glm::vec4> data(mapped, mapped + n * 54);
        for (size_t i = 0; i < n; ++i) {
            Block eBlock{}, bBlock{};
            std::copy_n(data.begin() + static_cast<std::ptrdiff_t>(i * 54), 27, eBlock.begin());
            std::copy_n(data.begin() + static_cast<std::ptrdiff_t>(i * 54 + 27), 27, bBlock.begin());
            const glm::ivec3 origin = s->origins3[i], extent = s->extents[i];
            ProbeSample p;
            const glm::vec3 pos = s->request.probes[i];
            p.position = pos;
            // Yee stagger offsets (see common.glsl).
            p.E.x = sampleBlock(eBlock, origin, extent, pos - glm::vec3(0.5f, 0.0f, 0.0f), 0);
            p.E.y = sampleBlock(eBlock, origin, extent, pos - glm::vec3(0.0f, 0.5f, 0.0f), 1);
            p.E.z = sampleBlock(eBlock, origin, extent, pos - glm::vec3(0.0f, 0.0f, 0.5f), 2);
            p.B.x = sampleBlock(bBlock, origin, extent, pos - glm::vec3(0.0f, 0.5f, 0.5f), 0);
            p.B.y = sampleBlock(bBlock, origin, extent, pos - glm::vec3(0.5f, 0.0f, 0.5f), 1);
            p.B.z = sampleBlock(bBlock, origin, extent, pos - glm::vec3(0.5f, 0.5f, 0.0f), 2);
            p.S = glm::cross(p.E, p.B);
            const glm::ivec3 cell = glm::clamp(glm::ivec3(glm::floor(pos)) - origin, glm::ivec3(0), extent - 1);
            const size_t ci = static_cast<size_t>((cell.z * kBlock + cell.y) * kBlock + cell.x);
            p.sigma = eBlock[ci].w;
            p.epsilonR = bBlock[ci].w;
            out.probes.push_back(p);
        }
    }
    out.fluxes.assign(s->request.fluxes.size(), 0.0);
    if (s->partialCount > 0) {
        const auto* base = static_cast<const unsigned char*>(s->readback.data()) + n * 54 * sizeof(glm::vec4);
        const auto* mappedPartials = reinterpret_cast<const float*>(base);
        const std::vector<float> partials(mappedPartials, mappedPartials + s->partialCount);
        for (const PlaneRef& pr : s->planes) {
            double sum = 0.0;
            for (size_t k = 0; k < pr.groups; ++k) sum += static_cast<double>(partials[pr.offset + k]);
            out.fluxes[pr.flux] += pr.sign * sum;
        }
    }
    return true;
}

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
    m_fluxShader.set("uPartialOffset", 0);

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
