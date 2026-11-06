#include "Ports.hpp"
#include "SimulationGrid.hpp"

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>

namespace fs = std::filesystem;

namespace {
constexpr double kEta0 = 376.730313668; // normalized impedance unit (ohm)
constexpr double kC0 = 299792458.0;
constexpr GLuint kMaxPorts = 256;
constexpr size_t kSeriesFloats = size_t(1) << 23; // 32 MB of recorded (V, I) samples at most

struct GPUPort {
    glm::ivec4 texelAxis;
    glm::ivec4 info;
};
static_assert(sizeof(GPUPort) == 32, "must match struct Port in port_accumulate.comp");

std::complex<double> reflection(const std::complex<double>& z, double z0) { return (z - z0) / (z + z0); }

double vswrOf(double g) {
    g = std::min(g, 0.999999);
    return (1.0 + g) / (1.0 - g);
}
} // namespace

PortMonitor::PortMonitor(const fs::path& shaderDir)
    : m_shader(Shader::computeFromFile(shaderDir / "port_accumulate.comp")),
      m_portBuffer(gl::createBuffer()),
      m_phasorBuffer(gl::createBuffer()),
      m_seriesBuffer(gl::createBuffer()) {
    glNamedBufferData(m_portBuffer.get(), sizeof(GPUPort) * kMaxPorts, nullptr, GL_DYNAMIC_DRAW);
    glNamedBufferData(m_phasorBuffer.get(), sizeof(glm::vec4) * 2 * kMaxPorts, nullptr, GL_DYNAMIC_READ); // read back by the CPU
    glNamedBufferData(m_seriesBuffer.get(), sizeof(glm::vec2), nullptr, GL_DYNAMIC_READ);
}

void PortMonitor::configure(const SimulationGrid& grid, const std::vector<PortSetup>& ports, float averagingPeriods,
                            const std::optional<SweepSetup>& sweep, float cellSize, size_t recordSteps) {
    m_ports.assign(ports.begin(), ports.begin() + std::min<size_t>(ports.size(), kMaxPorts));
    m_readings.assign(m_ports.size(), PortReading{});
    m_averagingPeriods = std::max(averagingPeriods, 0.5f);
    m_wavelength = m_ports.empty() ? 16.0f : std::max(m_ports.front().wavelength, 2.0f);
    m_startTime = grid.time();
    m_cellSize = cellSize;
    m_sweepSetup = sweep;
    m_sweep = SweepResult{};
    m_recorded = 0;
    m_capacitySteps = 0;
    m_recordSteps = recordSteps;
    if (m_ports.empty()) return;

    std::vector<GPUPort> packed;
    for (const PortSetup& p : m_ports)
        packed.push_back({glm::ivec4(p.texel, p.axis), glm::ivec4(p.edges, p.sign >= 0.0f ? 1 : -1, 0, 0)});
    glNamedBufferSubData(m_portBuffer.get(), 0, static_cast<GLsizeiptr>(packed.size() * sizeof(GPUPort)), packed.data());
    glClearNamedBufferData(m_phasorBuffer.get(), GL_RGBA32F, GL_RGBA, GL_FLOAT, nullptr);
    m_shader.set("uGridSize", grid.size());
    m_shader.set("uPortCount", static_cast<int>(m_ports.size()));

    if (m_sweepSetup || m_recordSteps > 0) {
        m_capacitySteps = kSeriesFloats / (2 * m_ports.size());
        if (!m_sweepSetup) m_capacitySteps = std::min(m_capacitySteps, m_recordSteps);
        glNamedBufferData(m_seriesBuffer.get(),
                          static_cast<GLsizeiptr>(m_capacitySteps * m_ports.size() * sizeof(glm::vec2)), nullptr,
                          GL_DYNAMIC_READ);
    }
}

void PortMonitor::disable() {
    m_ports.clear();
    m_readings.clear();
    m_sweepSetup.reset();
    m_sweep = SweepResult{};
}

double PortMonitor::accumulatedPeriods(const SimulationGrid& grid) const {
    return std::max(grid.time() - m_startTime, 0.0) / m_wavelength;
}

void PortMonitor::accumulate(const SimulationGrid& grid) {
    if (m_ports.empty()) return;
    if (grid.time() < m_startTime) m_startTime = grid.time(); // simulation was reset

    const double w = glm::two_pi<double>() / m_wavelength;
    const double tE = grid.time(), tH = tE - 0.5 * grid.dt();
    const double tau = m_averagingPeriods * m_wavelength;
    m_shader.set("uRotation", glm::vec4(static_cast<float>(std::cos(w * tE)), static_cast<float>(std::sin(w * tE)),
                                        static_cast<float>(std::cos(w * tH)), static_cast<float>(std::sin(w * tH))));
    m_shader.set("uBlend", static_cast<float>(1.0 - std::exp(-grid.dt() / (0.5 * tau))));

    const bool record = (m_sweepSetup || m_recordSteps > 0) && m_recorded < m_capacitySteps;
    if (record && m_recorded == 0) m_seriesT0 = tE;
    m_shader.set("uRecordIndex", record ? static_cast<int>(m_recorded) : -1);

    m_shader.use();
    glBindTextureUnit(0, grid.eFieldTexture());
    glBindTextureUnit(1, grid.bFieldTexture());
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 6, m_portBuffer.get());
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 7, m_phasorBuffer.get());
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 8, m_seriesBuffer.get());
    glDispatchCompute(gl::divUp(static_cast<GLuint>(m_ports.size()), 64u), 1, 1);
    if (record) ++m_recorded;
}

void PortMonitor::readSeries(std::vector<glm::vec2>& out) const {
    out.assign(m_recorded * m_ports.size(), glm::vec2(0.0f));
    if (out.empty()) return;
    glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
    glGetNamedBufferSubData(m_seriesBuffer.get(), 0, static_cast<GLsizeiptr>(out.size() * sizeof(glm::vec2)), out.data());
}

void PortMonitor::evaluate() {
    if (m_ports.empty()) return;
    glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
    std::vector<glm::vec4> data(m_ports.size() * 2);
    glGetNamedBufferSubData(m_phasorBuffer.get(), 0, static_cast<GLsizeiptr>(data.size() * sizeof(glm::vec4)), data.data());

    for (size_t i = 0; i < m_ports.size(); ++i) {
        const PortSetup& p = m_ports[i];
        const glm::vec4 s2 = data[i * 2 + 1];
        PortReading r;
        // The measured gap voltage is along the feed direction; the antenna terminal voltage
        // (positive terminal = the arm the gap field points away from) is its negative.
        r.V = -std::complex<double>(s2.x, s2.y);
        r.I = std::complex<double>(s2.z, s2.w);
        r.valid = std::abs(r.I) > 1e-12 && std::isfinite(std::abs(r.V));
        if (r.valid) {
            const std::complex<double> zn = r.V / r.I;
            r.zOhm = zn * kEta0;
            const double z0 = std::max(static_cast<double>(p.impedanceOhm), 1e-6);
            r.gamma = reflection(r.zOhm, z0);
            const double g = std::abs(r.gamma);
            r.vswr = vswrOf(g);
            r.returnLossDb = g > 1e-12 ? -20.0 * std::log10(g) : 200.0;
            r.acceptedPower = 0.5 * std::real(r.V * std::conj(r.I));
        }
        if (p.voltageFeed) {
            const double rs = std::max(static_cast<double>(p.impedanceOhm), 1e-6) / kEta0; // normalized ohms
            r.availablePower = static_cast<double>(p.drive) * p.drive / (8.0 * rs);
        }
        m_readings[i] = r;
    }
}

void PortMonitor::evaluateSweep(double dt) {
    if (!sweeping() || m_recorded < 16) return;
    const size_t nPorts = m_ports.size();
    const size_t n = m_recorded;
    glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
    std::vector<glm::vec2> series(n * nPorts);
    glGetNamedBufferSubData(m_seriesBuffer.get(), 0, static_cast<GLsizeiptr>(series.size() * sizeof(glm::vec2)),
                            series.data());

    const SweepSetup& s = *m_sweepSetup;
    const int points = std::max(s.points, 2);
    const double nuLo = 1.0 / std::max(s.wavelengthMax, s.wavelengthMin); // normalized frequency 1 / lambda
    const double nuHi = 1.0 / std::min(s.wavelengthMax, s.wavelengthMin);

    SweepResult res;
    res.samples = n;
    res.full = m_recorded >= m_capacitySteps;
    res.ports.resize(nPorts);

    // Ring-down: late-time voltage relative to its peak (the first port).
    double peak = 0.0, late = 0.0;
    for (size_t k = 0; k < n; ++k) {
        const double v = std::abs(series[k * nPorts].x);
        peak = std::max(peak, v);
        if (k >= n - std::max<size_t>(n / 20, 1)) late = std::max(late, v);
    }
    res.residual = peak > 0.0 ? late / peak : 1.0;

    for (size_t p = 0; p < nPorts; ++p) {
        PortSweep& ps = res.ports[p];
        const double z0 = std::max(static_cast<double>(m_ports[p].impedanceOhm), 1e-6);
        for (int f = 0; f < points; ++f) {
            const double nu = nuLo + (nuHi - nuLo) * f / (points - 1);
            const double w = glm::two_pi<double>() * nu;
            // V sampled at t0 + k dt, I (from B) at t0 + (k - 1/2) dt. A common time origin
            // cancels in V / I, so the phase is referred to t0.
            const std::complex<double> step = std::polar(1.0, -w * dt);
            std::complex<double> rot(1.0, 0.0), sumV(0.0), sumI(0.0);
            for (size_t k = 0; k < n; ++k) {
                const glm::vec2 vi = series[k * nPorts + p];
                sumV += static_cast<double>(vi.x) * rot;
                sumI += static_cast<double>(vi.y) * rot;
                rot *= step;
                if ((k & 1023) == 1023) rot /= std::abs(rot); // keep the recurrence on the unit circle
            }
            sumI *= std::polar(1.0, w * 0.5 * dt);
            SweepPoint pt;
            pt.wavelength = 1.0 / nu;
            if (std::abs(sumI) > 1e-30) {
                pt.zOhm = -sumV / sumI * kEta0;
                const double g = std::abs(reflection(pt.zOhm, z0));
                pt.s11Db = g > 1e-12 ? 20.0 * std::log10(g) : -200.0;
                pt.vswr = vswrOf(g);
            }
            ps.points.push_back(pt);
        }

        // Resonance (minimum S11, parabolic refinement) and the -10 dB band around it.
        size_t best = 0;
        for (size_t i = 1; i < ps.points.size(); ++i)
            if (ps.points[i].s11Db < ps.points[best].s11Db) best = i;
        ps.best = best;
        double nuBest = 1.0 / ps.points[best].wavelength;
        if (best > 0 && best + 1 < ps.points.size()) {
            const double a = ps.points[best - 1].s11Db, b = ps.points[best].s11Db, c = ps.points[best + 1].s11Db;
            const double denom = a - 2.0 * b + c;
            const double dnu = (nuHi - nuLo) / (points - 1);
            if (std::abs(denom) > 1e-12) nuBest += std::clamp(0.5 * (a - c) / denom, -1.0, 1.0) * dnu;
        }
        ps.bestWavelength = 1.0 / nuBest;
        if (ps.points[best].s11Db < -10.0) {
            auto edge = [&](int dir) {
                int i = static_cast<int>(best);
                while (i + dir >= 0 && i + dir < static_cast<int>(ps.points.size()) &&
                       ps.points[static_cast<size_t>(i + dir)].s11Db < -10.0)
                    i += dir;
                const int j = i + dir;
                const double nuI = 1.0 / ps.points[static_cast<size_t>(i)].wavelength;
                if (j < 0 || j >= static_cast<int>(ps.points.size())) return nuI; // band runs past the sweep
                const double nuJ = 1.0 / ps.points[static_cast<size_t>(j)].wavelength;
                const double sI = ps.points[static_cast<size_t>(i)].s11Db, sJ = ps.points[static_cast<size_t>(j)].s11Db;
                return nuI + (nuJ - nuI) * (-10.0 - sI) / (sJ - sI);
            };
            ps.hasBand = true;
            ps.bandLowWavelength = 1.0 / edge(-1);  // lower frequency edge
            ps.bandHighWavelength = 1.0 / edge(+1); // upper frequency edge
        }
    }
    res.valid = true;
    m_sweep = std::move(res);
    if (!m_sweepSetup->logPath.empty()) writeSweepCsv();
}

bool PortMonitor::writeSweepCsv() const {
    std::ofstream f(m_sweepSetup->logPath, std::ios::trunc);
    if (!f || !m_sweep.valid) return false;
    f << "frequency_Hz,wavelength_cells";
    for (const PortSetup& p : m_ports) f << "," << p.name << "_R_ohm," << p.name << "_X_ohm," << p.name << "_S11_dB," << p.name << "_VSWR";
    f << "\n";
    const size_t points = m_sweep.ports.front().points.size();
    for (size_t i = 0; i < points; ++i) {
        const double wl = m_sweep.ports.front().points[i].wavelength;
        f << kC0 / (wl * m_cellSize) << "," << wl;
        for (const PortSweep& ps : m_sweep.ports) {
            const SweepPoint& pt = ps.points[i];
            f << "," << pt.zOhm.real() << "," << pt.zOhm.imag() << "," << pt.s11Db << "," << pt.vswr;
        }
        f << "\n";
    }
    return true;
}
