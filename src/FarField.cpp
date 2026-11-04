#include "FarField.hpp"
#include "SimulationGrid.hpp"

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <stdexcept>

namespace fs = std::filesystem;

namespace {

constexpr int kCutSamples = 180;    // 2 degree resolution per principal-plane cut
constexpr int kSphereTheta = 18;    // 10 degree sphere sampling for the global maximum
constexpr int kSpherePhi = 36;
constexpr GLuint kLocal = 256;

struct GPUPhasorPair {
    glm::vec4 stage1[4]; // eRe, eIm, hRe, hIm
    glm::vec4 stage2[4];
};
static_assert(sizeof(GPUPhasorPair) == 128, "must match struct PhasorPair in ntff_common.glsl");

float toDbi(double d) { return d > 1e-12 ? static_cast<float>(10.0 * std::log10(d)) : -120.0f; }

void analyseCut(PatternCut& cut) {
    const int n = static_cast<int>(cut.directivityDbi.size());
    if (n == 0) return;
    const auto it = std::max_element(cut.directivityDbi.begin(), cut.directivityDbi.end());
    const int peak = static_cast<int>(it - cut.directivityDbi.begin());
    cut.peakDbi = *it;
    cut.peakAngleDeg = cut.angleDeg[static_cast<size_t>(peak)];
    const float step = 360.0f / static_cast<float>(n);
    const float threshold = cut.peakDbi - 3.0f;
    auto at = [&](int i) { return cut.directivityDbi[static_cast<size_t>(((i % n) + n) % n)]; };

    // Walk outwards from the peak to the -3 dB crossings (linear interpolation).
    float halfWidth[2] = {-1.0f, -1.0f};
    for (int dir = 0; dir < 2; ++dir) {
        const int sgn = dir == 0 ? 1 : -1;
        for (int k = 1; k <= n / 2; ++k) {
            const float a = at(peak + sgn * (k - 1)), b = at(peak + sgn * k);
            if (b < threshold) {
                const float frac = (a - threshold) / std::max(a - b, 1e-6f);
                halfWidth[dir] = (static_cast<float>(k - 1) + frac) * step;
                break;
            }
        }
    }
    cut.beamwidthDeg = (halfWidth[0] < 0.0f || halfWidth[1] < 0.0f) ? 360.0f : halfWidth[0] + halfWidth[1];
    cut.frontToBackDb = cut.peakDbi - at(peak + n / 2);
}

} // namespace

FarFieldMonitor::FarFieldMonitor(const fs::path& shaderDir)
    : m_accumulate(Shader::computeFromFile(shaderDir / "ntff_accumulate.comp")),
      m_patternShader(Shader::computeFromFile(shaderDir / "ntff_pattern.comp")),
      m_phasors(gl::createBuffer()),
      m_directions(gl::createBuffer()),
      m_output(gl::createBuffer()) {
    // Direction set: three principal cuts followed by a coarse full-sphere grid.
    const struct { const char* name; glm::vec3 a, b; } cuts[3] = {
        {"xy", {1, 0, 0}, {0, 1, 0}},  // azimuth plane (z = 0), angle from +x toward +y
        {"xz", {1, 0, 0}, {0, 0, 1}},  // elevation plane through x, angle from +x toward +z
        {"yz", {0, 1, 0}, {0, 0, 1}},  // elevation plane through y, angle from +y toward +z
    };
    for (const auto& c : cuts) {
        PatternCut cut;
        cut.name = c.name;
        cut.axisA = c.a;
        cut.axisB = c.b;
        for (int i = 0; i < kCutSamples; ++i) {
            const float deg = 360.0f * static_cast<float>(i) / kCutSamples;
            const float rad = glm::radians(deg);
            cut.angleDeg.push_back(deg);
            m_dirs.emplace_back(std::cos(rad) * c.a + std::sin(rad) * c.b, 0.0f);
        }
        m_pattern.cuts.push_back(std::move(cut));
    }
    m_sphereBegin = static_cast<int>(m_dirs.size());
    for (int it = 0; it < kSphereTheta; ++it) {
        const float th = glm::radians(5.0f + 10.0f * static_cast<float>(it));
        for (int ip = 0; ip < kSpherePhi; ++ip) {
            const float ph = glm::radians(10.0f * static_cast<float>(ip));
            m_dirs.emplace_back(std::sin(th) * std::cos(ph), std::sin(th) * std::sin(ph), std::cos(th), 0.0f);
        }
    }
    m_dirs.emplace_back(0.0f, 0.0f, 1.0f, 0.0f);
    m_dirs.emplace_back(0.0f, 0.0f, -1.0f, 0.0f);

    // Immutable: written once here, then only read by the pattern shader.
    glNamedBufferStorage(m_directions.get(), static_cast<GLsizeiptr>(m_dirs.size() * sizeof(glm::vec4)), m_dirs.data(), 0);
    glNamedBufferData(m_output.get(), static_cast<GLsizeiptr>(m_dirs.size() * 2 * sizeof(float)), nullptr,
                      GL_DYNAMIC_READ);
}

void FarFieldMonitor::configure(const SimulationGrid& grid, const FarFieldSetup& setup) {
    const glm::ivec3 n = grid.size();
    const int L = grid.config().pmlThickness;
    // Keep every node (and its -1 neighbours used for co-location) out of the absorber.
    const glm::ivec3 lo(L + 1), hi = n - L - 2;
    FarFieldSetup s = setup;
    s.boxMin = glm::clamp(glm::min(setup.boxMin, setup.boxMax), lo, hi);
    s.boxMax = glm::clamp(glm::max(setup.boxMin, setup.boxMax), lo, hi);
    if (glm::any(glm::lessThan(s.boxMax - s.boxMin, glm::ivec3(4))))
        throw std::runtime_error("far_field box is degenerate after clamping to the region inside the absorbing layer");
    s.wavelength = std::max(s.wavelength, 2.0f);
    s.averagingPeriods = std::max(s.averagingPeriods, 0.5f);

    m_faceOffset[0] = 0;
    for (int f = 0; f < 6; ++f) {
        const int a = f >> 1;
        const int u = a == 0 ? 1 : 0;
        const int v = a == 2 ? 1 : 2;
        const int count = (s.boxMax[u] - s.boxMin[u] + 1) * (s.boxMax[v] - s.boxMin[v] + 1); // inclusive nodes
        m_faceOffset[f + 1] = m_faceOffset[f] + count;
    }
    m_pointCount = m_faceOffset[6];

    glNamedBufferData(m_phasors.get(), static_cast<GLsizeiptr>(m_pointCount) * sizeof(GPUPhasorPair), nullptr,
                      GL_DYNAMIC_COPY);
    m_setup = s;
    m_enabled = true;
    uploadBoxUniforms(m_accumulate);
    uploadBoxUniforms(m_patternShader);
    m_accumulate.set("uGridSize", n);
    m_accumulate.set("uPointCount", m_pointCount);
    m_patternShader.set("uPointCount", m_pointCount);
    m_patternShader.set("uK", static_cast<float>(glm::two_pi<double>() / s.wavelength));
    m_patternShader.set("uCenter", 0.5f * glm::vec3(s.boxMin + s.boxMax));
    resetAccumulation();
    m_startTime = grid.time();
}

void FarFieldMonitor::uploadBoxUniforms(const Shader& s) const {
    s.set("uBoxMin", m_setup.boxMin);
    s.set("uBoxMax", m_setup.boxMax);
    s.setArray("uFaceOffset", m_faceOffset, 7);
}

void FarFieldMonitor::disable() {
    m_enabled = false;
    m_pattern.valid = false;
}

void FarFieldMonitor::resetAccumulation() {
    if (!m_enabled) return;
    glClearNamedBufferData(m_phasors.get(), GL_RGBA32F, GL_RGBA, GL_FLOAT, nullptr);
    m_pattern.valid = false;
}

double FarFieldMonitor::accumulatedTime(const SimulationGrid& grid) const {
    return std::max(grid.time() - m_startTime, 0.0);
}

void FarFieldMonitor::accumulate(const SimulationGrid& grid) {
    if (!m_enabled) return;
    if (grid.time() < m_startTime) m_startTime = grid.time(); // simulation was reset

    const double w = glm::two_pi<double>() / m_setup.wavelength;
    const double dt = grid.dt();
    const double tE = grid.time();      // E after the completed step: t = n dt
    const double tH = tE - 0.5 * dt;    // B lags by half a step
    const glm::vec4 rot(static_cast<float>(std::cos(w * tE)), static_cast<float>(std::sin(w * tE)),
                        static_cast<float>(std::cos(w * tH)), static_cast<float>(std::sin(w * tH)));
    const double tau = m_setup.averagingPeriods * m_setup.wavelength;

    m_accumulate.set("uRotation", rot);
    m_accumulate.set("uBlend", static_cast<float>(1.0 - std::exp(-dt / (0.5 * tau)))); // per cascade stage
    m_accumulate.use();
    glBindTextureUnit(0, grid.eFieldTexture());
    glBindTextureUnit(1, grid.bFieldTexture());
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, m_phasors.get());
    glDispatchCompute(gl::divUp(static_cast<GLuint>(m_pointCount), kLocal), 1, 1);
}

const FarFieldPattern& FarFieldMonitor::evaluate(const SimulationGrid& /*grid*/) {
    if (!m_enabled) return m_pattern;

    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
    m_patternShader.use();
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, m_phasors.get());
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 4, m_directions.get());
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 5, m_output.get());
    glDispatchCompute(static_cast<GLuint>(m_dirs.size()), 1, 1);
    glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);

    std::vector<float> out(m_dirs.size() * 2);
    glGetNamedBufferSubData(m_output.get(), 0, static_cast<GLsizeiptr>(out.size() * sizeof(float)), out.data());

    const double P = out[1];
    m_pattern.radiatedPower = P;
    m_pattern.valid = std::isfinite(P) && P > 1e-12;
    if (!m_pattern.valid) return m_pattern;

    // D = 4 pi U / P with U = k^2 |F|^2 / (32 pi^2)  ->  D = k^2 |F|^2 / (8 pi P)
    const double k = glm::two_pi<double>() / m_setup.wavelength;
    const double scale = k * k / (8.0 * glm::pi<double>() * P);
    auto directivity = [&](size_t i) { return scale * static_cast<double>(out[2 * i]); };

    double best = -1.0;
    for (size_t i = 0; i < m_dirs.size(); ++i) {
        const double d = directivity(i);
        if (d > best) {
            best = d;
            m_pattern.maxDirection = glm::vec3(m_dirs[i]);
        }
    }
    m_pattern.maxDirectivityDbi = toDbi(best);

    size_t idx = 0;
    for (PatternCut& cut : m_pattern.cuts) {
        cut.directivityDbi.resize(cut.angleDeg.size());
        for (float& v : cut.directivityDbi) v = toDbi(directivity(idx++));
        analyseCut(cut);
    }
    if (!m_setup.logPath.empty()) writeCsv(m_setup.logPath);
    return m_pattern;
}

bool FarFieldMonitor::writeCsv(const fs::path& path) const {
    if (!m_pattern.valid) return false;
    std::ofstream f(path, std::ios::trunc);
    if (!f) return false;
    f << "angle_deg";
    for (const PatternCut& c : m_pattern.cuts) f << "," << c.name << "_dBi";
    f << "\n";
    for (size_t i = 0; i < m_pattern.cuts.front().angleDeg.size(); ++i) {
        f << m_pattern.cuts.front().angleDeg[i];
        for (const PatternCut& c : m_pattern.cuts) f << "," << c.directivityDbi[i];
        f << "\n";
    }
    return true;
}
