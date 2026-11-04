#include "SimulationGrid.hpp"

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace fs = std::filesystem;

namespace {

// Must match `struct Source` in update_E.comp (std430: 4 x vec4 = 64 bytes).
struct GPUSource {
    glm::vec4 centerSoft;  // xyz centre, w softness
    glm::vec4 halfExtent;  // xyz half extent, w cull radius
    glm::vec4 polAmp;      // xyz unit polarisation, w amplitude
    glm::vec4 reserved;
};
static_assert(sizeof(GPUSource) == 64, "GPUSource must match the std430 layout");

constexpr GLbitfield kFieldBarrier = GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT;
constexpr float kMaxCourant = 0.99f / 1.7320508f; // strictly below 1/sqrt(3)

void bindImage(GLuint unit, GLuint texture) {
    glBindImageTexture(unit, texture, 0, GL_TRUE, 0, GL_READ_WRITE, GL_RGBA32F);
}

} // namespace

// =============================================================================
// MaterialVolume
// =============================================================================
MaterialVolume::MaterialVolume(const glm::ivec3& size)
    : m_size(glm::max(size, glm::ivec3(0))) {
    const size_t n = static_cast<size_t>(m_size.x) * m_size.y * m_size.z;
    m_sigma.assign(n, 0.0f);
    m_epsR.assign(n, 1.0f);
}

void MaterialVolume::set(const glm::ivec3& p, const Material& m) {
    if (!contains(p)) return;
    const size_t i = index(p);
    m_sigma[i] = std::max(m.sigma, 0.0f);
    m_epsR[i] = std::max(m.epsilonR, 1.0f);
}

Material MaterialVolume::get(const glm::ivec3& p) const {
    if (!contains(p)) return Material::vacuum();
    const size_t i = index(p);
    return {m_sigma[i], m_epsR[i]};
}

// =============================================================================
// SourceDesc
// =============================================================================
SourceDesc SourceDesc::dipole(const glm::vec3& center, const glm::vec3& axis, float wavelength,
                              float amplitude, float length) {
    SourceDesc s;
    s.center = center;
    s.polarization = axis;
    s.wavelength = wavelength;
    s.amplitude = amplitude;
    s.softness = 0.75f;
    const float len2 = glm::dot(axis, axis);
    const glm::vec3 n = len2 > 0.0f ? axis / std::sqrt(len2) : glm::vec3(0.0f, 0.0f, 1.0f);
    s.halfExtent = glm::abs(n) * (0.5f * std::max(length, 0.0f));
    return s;
}

SourceDesc SourceDesc::planeWave(int normalAxis, float position, const glm::vec3& polarization,
                                 float wavelength, float amplitude) {
    SourceDesc s;
    const int a = std::clamp(normalAxis, 0, 2);
    s.center = glm::vec3(0.0f);
    s.center[a] = position;
    s.halfExtent = glm::vec3(1.0e5f);
    s.halfExtent[a] = 0.0f;
    s.softness = 0.5f;
    s.polarization = polarization;
    s.wavelength = wavelength;
    s.amplitude = amplitude;
    return s;
}

double SourceDesc::omega() const noexcept {
    return glm::two_pi<double>() / std::max(static_cast<double>(wavelength), 1e-3);
}

double SourceDesc::signal(double t) const noexcept {
    if (!enabled || t < 0.0) return 0.0;
    const double period = std::max(static_cast<double>(wavelength), 1e-3); // c = 1
    const double w = omega();

    if (waveform == Waveform::ContinuousWave) {
        const double rampT = std::max(static_cast<double>(rampPeriods), 0.0) * period;
        double ramp = 1.0;
        if (rampT > 0.0 && t < rampT) {
            const double r = t / rampT;
            ramp = r * r * r * (10.0 + r * (-15.0 + 6.0 * r)); // C2 smootherstep: minimal DC/HF
        }
        return ramp * std::sin(w * t + phase);
    }

    // Gaussian-modulated sinusoid (wave packet).
    const double tau = std::max(static_cast<double>(rampPeriods), 0.1) * period;
    const double t0 = 3.0 * tau;
    double tl = t;
    if (repeatPeriods > 0.0f) {
        const double rep = std::max(static_cast<double>(repeatPeriods) * period, 2.0 * t0);
        tl = std::fmod(t, rep);
    }
    const double u = (tl - t0) / tau;
    if (std::abs(u) > 6.0) return 0.0; // exp(-36) ~ 2e-16
    return std::exp(-u * u) * std::sin(w * (tl - t0) + phase);
}

// =============================================================================
// SimulationGrid
// =============================================================================
SimulationGrid::SimulationGrid(const fs::path& shaderDir)
    : m_updateE(Shader::computeFromFile(shaderDir / "update_E.comp")),
      m_updateB(Shader::computeFromFile(shaderDir / "update_B.comp")),
      m_clear(Shader::computeFromFile(shaderDir / "clear_fields.comp")),
      m_sourceBuffer(gl::createBuffer()),
      m_signalBuffer(gl::createBuffer()) {
    glNamedBufferData(m_sourceBuffer.get(), sizeof(GPUSource) * kMaxSources, nullptr, GL_DYNAMIC_DRAW);
    glNamedBufferData(m_signalBuffer.get(), sizeof(float) * kMaxSources * 16, nullptr, GL_STREAM_DRAW);
    m_signalScratch.reserve(kMaxSources * 16);
}

void SimulationGrid::configure(const GridConfig& requested) {
    GridConfig cfg = requested;
    cfg.pmlThickness = std::clamp(cfg.pmlThickness, 0, 64);
    const int minSize = 2 * cfg.pmlThickness + 8;
    cfg.size = glm::max(cfg.size, glm::ivec3(minSize));
    cfg.courant = std::clamp(cfg.courant, 0.05f, kMaxCourant);
    cfg.pmlSigmaMax = std::max(cfg.pmlSigmaMax, 0.0f);
    cfg.pmlGrading = std::clamp(cfg.pmlGrading, 0.25f, 12.0f);

    GLint maxTex3D = 0;
    glGetIntegerv(GL_MAX_3D_TEXTURE_SIZE, &maxTex3D);
    if (cfg.size.x > maxTex3D || cfg.size.y > maxTex3D || cfg.size.z > maxTex3D)
        throw std::runtime_error("SimulationGrid: grid exceeds GL_MAX_3D_TEXTURE_SIZE");

    m_config = cfg;

    if (cfg.size != m_allocatedSize || !m_eField) {
        auto makeField = [&cfg]() {
            gl::Texture tex = gl::createTexture(GL_TEXTURE_3D);
            glTextureStorage3D(tex.get(), 1, GL_RGBA32F, cfg.size.x, cfg.size.y, cfg.size.z);
            glTextureParameteri(tex.get(), GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTextureParameteri(tex.get(), GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTextureParameteri(tex.get(), GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTextureParameteri(tex.get(), GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTextureParameteri(tex.get(), GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
            return tex;
        };
        m_eField = makeField();
        m_bField = makeField();
        m_allocatedSize = cfg.size;
    }

    // Vacuum everywhere: E = (0,0,0, sigma=0), B = (0,0,0, eps_r=1).
    const float eInit[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    const float bInit[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    glClearTexImage(m_eField.get(), 0, GL_RGBA, GL_FLOAT, eInit);
    glClearTexImage(m_bField.get(), 0, GL_RGBA, GL_FLOAT, bInit);

    applySolverUniforms(m_updateE);
    applySolverUniforms(m_updateB);
    m_clear.set("uGridSize", m_config.size);
    m_stepCount = 0;
}

void SimulationGrid::applySolverUniforms(const Shader& s) const {
    s.set("uGridSize", m_config.size);
    s.set("uDt", m_config.courant);
    s.set("uPmlThickness", m_config.pmlThickness);
    s.set("uPmlSigmaMax", m_config.pmlSigmaMax);
    s.set("uPmlGrading", m_config.pmlGrading);
}

void SimulationGrid::uploadMaterials(const MaterialVolume& materials) {
    if (materials.size() != m_config.size)
        throw std::runtime_error("SimulationGrid: material volume size does not match the grid");

    const size_t n = materials.sigma().size();
    std::vector<glm::vec4> texels(n);

    for (size_t i = 0; i < n; ++i) texels[i] = glm::vec4(0.0f, 0.0f, 0.0f, materials.sigma()[i]);
    glTextureSubImage3D(m_eField.get(), 0, 0, 0, 0, m_config.size.x, m_config.size.y, m_config.size.z,
                        GL_RGBA, GL_FLOAT, texels.data());

    for (size_t i = 0; i < n; ++i) texels[i] = glm::vec4(0.0f, 0.0f, 0.0f, materials.epsilonR()[i]);
    glTextureSubImage3D(m_bField.get(), 0, 0, 0, 0, m_config.size.x, m_config.size.y, m_config.size.z,
                        GL_RGBA, GL_FLOAT, texels.data());

    m_stepCount = 0;
}

void SimulationGrid::setSources(const std::vector<SourceDesc>& sources) {
    m_sources.clear();
    std::vector<GPUSource> packed;
    packed.reserve(std::min<size_t>(sources.size(), kMaxSources));

    for (const SourceDesc& s : sources) {
        if (packed.size() >= static_cast<size_t>(kMaxSources)) break;
        const float len = glm::length(s.polarization);
        if (!s.enabled || !(len > 1e-6f) || !std::isfinite(len)) continue;

        const float soft = std::max(s.softness, 0.25f);
        GPUSource g{};
        g.centerSoft = glm::vec4(s.center, soft);
        g.halfExtent = glm::vec4(glm::clamp(s.halfExtent, glm::vec3(0.0f), glm::vec3(1.0e5f)), 3.5f * soft);
        g.polAmp = glm::vec4(s.polarization / len, s.amplitude);
        packed.push_back(g);
        m_sources.push_back(s);
    }

    if (!packed.empty())
        glNamedBufferSubData(m_sourceBuffer.get(), 0, static_cast<GLsizeiptr>(packed.size() * sizeof(GPUSource)),
                             packed.data());
    m_updateE.set("uSourceCount", static_cast<int>(packed.size()));
}

void SimulationGrid::resetFields() {
    m_clear.use();
    bindImage(0, m_eField.get());
    bindImage(1, m_bField.get());
    m_clear.dispatchCovering(glm::uvec3(m_config.size));
    glMemoryBarrier(kFieldBarrier);
    m_stepCount = 0;
}

void SimulationGrid::step(int count) {
    if (count <= 0) return;

    // Pre-compute every source's temporal signal for all sub-steps of this call in
    // double precision: E^{n+1} uses J at t = (n + 1/2) dt. One upload per call.
    const size_t nSrc = m_sources.size();
    if (nSrc > 0) {
        m_signalScratch.resize(nSrc * static_cast<size_t>(count));
        for (int k = 0; k < count; ++k) {
            const double t = (static_cast<double>(m_stepCount + static_cast<std::uint64_t>(k)) + 0.5) * dt();
            for (size_t s = 0; s < nSrc; ++s)
                m_signalScratch[static_cast<size_t>(k) * nSrc + s] = static_cast<float>(m_sources[s].signal(t));
        }
        const GLsizeiptr bytes = static_cast<GLsizeiptr>(m_signalScratch.size() * sizeof(float));
        GLint64 capacity = 0;
        glGetNamedBufferParameteri64v(m_signalBuffer.get(), GL_BUFFER_SIZE, &capacity);
        if (bytes > capacity) // orphan + grow (rare: only when steps*sources increases)
            glNamedBufferData(m_signalBuffer.get(), bytes * 2, nullptr, GL_STREAM_DRAW);
        glNamedBufferSubData(m_signalBuffer.get(), 0, bytes, m_signalScratch.data());
    }

    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, m_sourceBuffer.get());
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, m_signalBuffer.get());

    const glm::uvec3 cells(m_config.size);
    for (int k = 0; k < count; ++k) {
        // --- Faraday: B^{n+1/2} = B^{n-1/2} - dt curl E^n ---------------------
        m_updateB.use();
        bindImage(0, m_bField.get());
        glBindTextureUnit(1, m_eField.get());
        m_updateB.dispatchCovering(cells);
        glMemoryBarrier(kFieldBarrier);

        // --- Ampere: E^{n+1} = Ca E^n + Cb (curl B^{n+1/2} - J^{n+1/2}) -------
        m_updateE.use();
        m_updateE.set("uSignalBase", k * static_cast<int>(nSrc));
        bindImage(0, m_eField.get());
        glBindTextureUnit(1, m_bField.get());
        m_updateE.dispatchCovering(cells);
        glMemoryBarrier(kFieldBarrier);
    }

    glBindTextureUnit(1, 0);
    m_stepCount += static_cast<std::uint64_t>(count);
}

size_t SimulationGrid::gpuMemoryBytes() const noexcept {
    return 2ull * static_cast<size_t>(m_config.size.x) * m_config.size.y * m_config.size.z * sizeof(glm::vec4);
}
