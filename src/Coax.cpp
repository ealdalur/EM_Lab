#include "Coax.hpp"
#include "SimulationGrid.hpp"

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>

namespace fs = std::filesystem;

namespace {
using cd = std::complex<double>;
constexpr double kEta0 = 376.730313668;

int nodeLo(float c, int size) { return static_cast<int>(std::lround(c - 0.5f * static_cast<float>(size))); }
int nodeHi(float c, int size) { return static_cast<int>(std::lround(c + 0.5f * static_cast<float>(size))); }
} // namespace

CoaxMonitor::CoaxMonitor(const fs::path& shaderDir)
    : m_shader(Shader::computeFromFile(shaderDir / "line_accumulate.comp")),
      m_quantityBuffer(gl::createBuffer()),
      m_termBuffer(gl::createBuffer()),
      m_weightBuffer(gl::createBuffer()),
      m_phasorBuffer(gl::createBuffer()),
      m_nowBuffer(gl::createBuffer()) {}

CoaxMonitor::~CoaxMonitor() {
    if (m_fence) glDeleteSync(m_fence);
}

void CoaxMonitor::disable() {
    if (m_fence) glDeleteSync(m_fence);
    m_fence = nullptr;
    m_lines.clear();
    m_layout.clear();
    m_readings.clear();
    m_quantities = 0;
}

// Laplace's equation for the potential on the lattice nodes of the cross-section (inner
// conductor at 1, shield at 0): G = sum over the transverse edges of (delta phi)^2 is the
// lattice's capacitance per unit length in units of eps, exactly what the FDTD's TEM mode sees.
double CoaxMonitor::geometryFactor(int inner, int outer) {
    const int n = outer + 1; // nodes across
    const int lo = (outer - inner) / 2, hi = lo + inner;
    std::vector<double> phi(static_cast<size_t>(n * n), 0.0);
    std::vector<char> fixed(phi.size(), 0);
    for (int j = 0; j < n; ++j)
        for (int i = 0; i < n; ++i) {
            const size_t k = static_cast<size_t>(j * n + i);
            if (i == 0 || j == 0 || i == n - 1 || j == n - 1) fixed[k] = 1;
            if (i >= lo && i <= hi && j >= lo && j <= hi) {
                fixed[k] = 1;
                phi[k] = 1.0;
            }
        }
    const double omega = 2.0 / (1.0 + std::sin(glm::pi<double>() / n)); // optimal SOR
    for (int it = 0; it < 20000; ++it) {
        double change = 0.0;
        for (int j = 1; j < n - 1; ++j)
            for (int i = 1; i < n - 1; ++i) {
                const size_t k = static_cast<size_t>(j * n + i);
                if (fixed[k]) continue;
                const double target = 0.25 * (phi[k - 1] + phi[k + 1] + phi[k - n] + phi[k + n]);
                const double d = omega * (target - phi[k]);
                phi[k] += d;
                change = std::max(change, std::abs(d));
            }
        if (change < 1e-13) break;
    }
    double g = 0.0;
    for (int j = 0; j < n; ++j)
        for (int i = 0; i < n; ++i) {
            const size_t k = static_cast<size_t>(j * n + i);
            if (i + 1 < n) g += (phi[k + 1] - phi[k]) * (phi[k + 1] - phi[k]);
            if (j + 1 < n) g += (phi[k + n] - phi[k]) * (phi[k + n] - phi[k]);
        }
    return g;
}

void CoaxMonitor::configure(const SimulationGrid& grid, const std::vector<CoaxSetup>& lines, float averagingPeriods) {
    if (m_fence) glDeleteSync(m_fence);
    m_fence = nullptr;
    m_lines = lines;
    m_averagingPeriods = std::max(averagingPeriods, 0.5f);
    m_startTime = grid.time();
    m_dt = grid.dt();
    m_layout.clear();
    m_readings.assign(m_lines.size(), CoaxReading{});
    if (m_lines.empty()) return;

    std::vector<glm::ivec4> quantities, terms;
    std::vector<float> weights;
    for (const CoaxSetup& c : m_lines) {
        const int a = c.axis, u = (a + 1) % 3, v = (a + 2) % 3;
        auto texel = [&](int ia, int iu, int iv) {
            glm::ivec4 t(0);
            t[a] = ia;
            t[u] = iu;
            t[v] = iv;
            return t;
        };
        auto add = [&](glm::ivec4 t, int comp, float w) {
            t.w = comp;
            terms.push_back(t);
            weights.push_back(w);
        };
        // Current along +a through the node rectangle [u0, u1] x [v0, v1] (B plane index ia).
        auto loop = [&](int ia, int u0, int u1, int v0, int v1, float w) {
            for (int iv = v0; iv <= v1; ++iv) {
                add(texel(ia, u1, iv), 3 + v, w);
                add(texel(ia, u0 - 1, iv), 3 + v, -w);
            }
            for (int iu = u0; iu <= u1; ++iu) {
                add(texel(ia, iu, v1), 3 + u, -w);
                add(texel(ia, iu, v0 - 1), 3 + u, w);
            }
        };
        const float cu = c.center[u], cv = c.center[v];
        const int iu0 = nodeLo(cu, c.inner), iu1 = nodeHi(cu, c.inner), iv0 = nodeLo(cv, c.inner), iv1 = nodeHi(cv, c.inner);
        const int su0 = nodeLo(cu, c.outer), su1 = nodeHi(cu, c.outer), sv0 = nodeLo(cv, c.outer), sv1 = nodeHi(cv, c.outer);
        const int shield = c.outer + 2 * c.wall;

        LineLayout lay;
        lay.first = quantities.size();
        for (int k = 1; k < c.length; ++k) {
            // V at node plane t = k: the E line integral from the inner conductor to the shield,
            // averaged over the 4 sides and the inner conductor's node rows.
            const int ia = c.start + c.sign * k;
            const size_t first = terms.size();
            const float w = 1.0f / (4.0f * static_cast<float>(c.inner + 1));
            for (int r = iv0; r <= iv1; ++r) {
                for (int i = iu1; i < su1; ++i) add(texel(ia, i, r), u, w);
                for (int i = su0; i < iu0; ++i) add(texel(ia, i, r), u, -w);
            }
            for (int r = iu0; r <= iu1; ++r) {
                for (int i = iv1; i < sv1; ++i) add(texel(ia, r, i), v, w);
                for (int i = sv0; i < iv0; ++i) add(texel(ia, r, i), v, -w);
            }
            quantities.emplace_back(static_cast<int>(first), static_cast<int>(terms.size() - first), 0, 0);

            // I and Icm at t = k + 1/2, positive toward the far end.
            const int ib = c.sign > 0 ? c.start + k : c.start - k - 1;
            const float s = static_cast<float>(c.sign);
            size_t f2 = terms.size();
            loop(ib, iu0, iu1, iv0, iv1, s);
            quantities.emplace_back(static_cast<int>(f2), static_cast<int>(terms.size() - f2), 1, 0);
            const bool inSleeve = c.sleeveOuter > 0 && k >= c.sleeveStart && k < c.sleeveEnd;
            const int out = inSleeve ? c.sleeveOuter : shield;
            f2 = terms.size();
            loop(ib, nodeLo(cu, out), nodeHi(cu, out), nodeLo(cv, out), nodeHi(cv, out), s);
            quantities.emplace_back(static_cast<int>(f2), static_cast<int>(terms.size() - f2), 1, 0);
        }
        lay.stations = static_cast<size_t>(std::max(c.length - 1, 0));
        m_layout.push_back(lay);
    }
    m_quantities = quantities.size();
    glNamedBufferData(m_quantityBuffer.get(), static_cast<GLsizeiptr>(quantities.size() * sizeof(glm::ivec4)),
                      quantities.data(), GL_STATIC_DRAW);
    glNamedBufferData(m_termBuffer.get(), static_cast<GLsizeiptr>(terms.size() * sizeof(glm::ivec4)), terms.data(),
                      GL_STATIC_DRAW);
    glNamedBufferData(m_weightBuffer.get(), static_cast<GLsizeiptr>(weights.size() * sizeof(float)), weights.data(),
                      GL_STATIC_DRAW);
    glNamedBufferData(m_phasorBuffer.get(), static_cast<GLsizeiptr>(m_quantities * sizeof(glm::vec4)), nullptr,
                      GL_DYNAMIC_COPY);
    glClearNamedBufferData(m_phasorBuffer.get(), GL_RGBA32F, GL_RGBA, GL_FLOAT, nullptr);
    glNamedBufferData(m_nowBuffer.get(), static_cast<GLsizeiptr>(m_quantities * sizeof(float)), nullptr, GL_DYNAMIC_COPY);
    glClearNamedBufferData(m_nowBuffer.get(), GL_R32F, GL_RED, GL_FLOAT, nullptr);
    m_readback.ensure(m_quantities * (sizeof(glm::vec4) + sizeof(float)));
    m_shader.set("uGridSize", grid.size());
    m_shader.set("uCount", static_cast<int>(m_quantities));
}

double CoaxMonitor::periods(const SimulationGrid& grid) const {
    if (m_lines.empty()) return 0.0;
    return std::max(grid.time() - m_startTime, 0.0) / std::max(m_lines.front().wavelength, 2.0f);
}

void CoaxMonitor::accumulate(const SimulationGrid& grid) {
    if (m_quantities == 0) return;
    if (grid.time() < m_startTime) m_startTime = grid.time();
    const double w = glm::two_pi<double>() / std::max(m_lines.front().wavelength, 2.0f);
    const double tE = grid.time(), tH = tE - 0.5 * grid.dt();
    const double tau = m_averagingPeriods * m_lines.front().wavelength;
    m_shader.set("uRotation", glm::vec4(static_cast<float>(std::cos(w * tE)), static_cast<float>(std::sin(w * tE)),
                                        static_cast<float>(std::cos(w * tH)), static_cast<float>(std::sin(w * tH))));
    m_shader.set("uBlend", static_cast<float>(1.0 - std::exp(-grid.dt() / (0.5 * tau))));
    m_shader.use();
    glBindTextureUnit(0, grid.eFieldTexture());
    glBindTextureUnit(1, grid.bFieldTexture());
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 6, m_quantityBuffer.get());
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 7, m_termBuffer.get());
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 8, m_weightBuffer.get());
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 9, m_phasorBuffer.get());
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 10, m_nowBuffer.get());
    glDispatchCompute(gl::divUp(static_cast<GLuint>(m_quantities), 64u), 1, 1);
}

void CoaxMonitor::poll() {
    if (m_quantities == 0) return;
    if (m_fence) {
        const GLenum state = glClientWaitSync(m_fence, 0, 0);
        if (state != GL_ALREADY_SIGNALED && state != GL_CONDITION_SATISFIED) return;
        glDeleteSync(m_fence);
        m_fence = nullptr;
        const auto* base = static_cast<const unsigned char*>(m_readback.data());
        const auto* ph = reinterpret_cast<const glm::vec4*>(base);
        const auto* now = reinterpret_cast<const float*>(base + m_quantities * sizeof(glm::vec4));
        for (size_t i = 0; i < m_lines.size(); ++i) analyse(i, ph, now);
    }
    glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
    m_readback.copyFrom(m_phasorBuffer.get(), m_quantities * sizeof(glm::vec4));
    m_readback.copyFrom(m_nowBuffer.get(), m_quantities * sizeof(float), 0, m_quantities * sizeof(glm::vec4));
    m_fence = gl::readbackFence();
}

void CoaxMonitor::analyse(size_t line, const glm::vec4* ph, const float* now) {
    const CoaxSetup& c = m_lines[line];
    const LineLayout& lay = m_layout[line];
    CoaxReading& r = m_readings[line];
    const double peak = r.peakNow;
    r = CoaxReading{};
    const size_t n = lay.stations;
    for (size_t k = 0; k < n; ++k) {
        const size_t q = lay.first + 3 * k;
        r.tV.push_back(static_cast<double>(k + 1));
        r.tI.push_back(static_cast<double>(k + 1) + 0.5);
        r.V.emplace_back(ph[q].z, ph[q].w);
        r.I.emplace_back(ph[q + 1].z, ph[q + 1].w);
        r.Icm.emplace_back(ph[q + 2].z, ph[q + 2].w);
        r.vNow.push_back(now[q]);
        r.iNow.push_back(now[q + 1]);
        r.cmNow.push_back(now[q + 2]);
    }
    double vmax = 0.0;
    for (float x : r.vNow) vmax = std::max(vmax, static_cast<double>(std::abs(x)));
    r.peakNow = std::max(vmax, peak * 0.995);

    // The lattice's TEM propagation constant at the drive frequency.
    const double w = glm::two_pi<double>() / std::max(c.wavelength, 2.0f);
    const double sArg = std::sqrt(static_cast<double>(c.epsilonR)) * std::sin(0.5 * w * m_dt) / m_dt;
    r.beta = 2.0 * std::asin(std::clamp(sArg, -1.0, 1.0));
    if (c.pulsed || n < 8) return;

    // Least-squares fit of the two waves, away from the end regions.
    const double edge = static_cast<double>(c.outer + 2);
    auto fit = [&](const std::vector<double>& t, const std::vector<cd>& x, cd& A, cd& B) {
        cd m11(0.0), m12(0.0), m22(0.0), r1(0.0), r2(0.0);
        for (size_t k = 0; k < t.size(); ++k) {
            if (t[k] < edge || t[k] > c.length - edge) continue;
            const cd e = std::polar(1.0, -r.beta * t[k]), f = std::conj(e);
            m11 += std::norm(e);
            m12 += std::conj(e) * f;
            m22 += std::norm(f);
            r1 += std::conj(e) * x[k];
            r2 += std::conj(f) * x[k];
        }
        const cd det = m11 * m22 - m12 * std::conj(m12);
        if (std::abs(det) < 1e-30) return false;
        A = (r1 * m22 - m12 * r2) / det;
        B = (m11 * r2 - std::conj(m12) * r1) / det;
        return true;
    };
    cd Ai, Bi;
    if (!fit(r.tV, r.V, r.A, r.B) || !fit(r.tI, r.I, Ai, Bi) || std::abs(r.A) < 1e-20) return;
    double res = 0.0, tot = 0.0;
    for (size_t k = 0; k < n; ++k) {
        if (r.tV[k] < edge || r.tV[k] > c.length - edge) continue;
        const cd model = r.A * std::polar(1.0, -r.beta * r.tV[k]) + r.B * std::polar(1.0, r.beta * r.tV[k]);
        res += std::norm(r.V[k] - model);
        tot += std::norm(model);
    }
    r.fitResidual = tot > 0.0 ? std::sqrt(res / tot) : 1.0;
    r.z0Measured = std::abs(Ai) > 1e-30 ? std::abs(r.A / Ai) * kEta0 : 0.0;
    r.gammaEnd = r.B / r.A * std::polar(1.0, 2.0 * r.beta * c.length);
    const double g = std::min(std::abs(r.gammaEnd), 0.999999);
    r.vswr = (1.0 + g) / (1.0 - g);
    r.returnLossDb = g > 1e-12 ? -20.0 * std::log10(g) : 200.0;
    r.zEnd = static_cast<double>(c.z0Ohm) * (1.0 + r.gammaEnd) / (1.0 - r.gammaEnd);
    const double z0n = c.z0Ohm / kEta0, rsn = c.sourceOhm / kEta0;
    const double pFwd = std::norm(r.A) / (2.0 * z0n), pAvail = c.drive * c.drive / (8.0 * rsn);
    r.forwardFraction = pAvail > 0.0 ? pFwd / pAvail : 0.0;
    const double iFwd = std::abs(r.A) / z0n;
    double cm = 0.0;
    for (const cd& x : r.Icm) cm = std::max(cm, std::abs(x));
    r.commonModeDb = cm > 0.0 && iFwd > 0.0 ? 20.0 * std::log10(cm / iFwd) : -300.0;
    r.valid = true;
}
