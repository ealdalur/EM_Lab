#include "Ports.hpp"
#include "SimulationGrid.hpp"

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>

namespace fs = std::filesystem;

namespace {
constexpr double kEta0 = 376.730313668; // normalized impedance unit (ohm)
constexpr GLuint kMaxPorts = 256;

struct GPUPort {
    glm::ivec4 texelAxis;
    glm::ivec4 info;
};
static_assert(sizeof(GPUPort) == 32, "must match struct Port in port_accumulate.comp");
} // namespace

PortMonitor::PortMonitor(const fs::path& shaderDir)
    : m_shader(Shader::computeFromFile(shaderDir / "port_accumulate.comp")),
      m_portBuffer(gl::createBuffer()),
      m_phasorBuffer(gl::createBuffer()) {
    glNamedBufferData(m_portBuffer.get(), sizeof(GPUPort) * kMaxPorts, nullptr, GL_DYNAMIC_DRAW);
    glNamedBufferData(m_phasorBuffer.get(), sizeof(glm::vec4) * 2 * kMaxPorts, nullptr, GL_DYNAMIC_READ); // read back by the CPU
}

void PortMonitor::configure(const SimulationGrid& grid, const std::vector<PortSetup>& ports, float averagingPeriods) {
    m_ports.assign(ports.begin(), ports.begin() + std::min<size_t>(ports.size(), kMaxPorts));
    m_readings.assign(m_ports.size(), PortReading{});
    m_averagingPeriods = std::max(averagingPeriods, 0.5f);
    m_wavelength = m_ports.empty() ? 16.0f : std::max(m_ports.front().wavelength, 2.0f);
    m_startTime = grid.time();
    if (m_ports.empty()) return;

    std::vector<GPUPort> packed;
    for (const PortSetup& p : m_ports)
        packed.push_back({glm::ivec4(p.texel, p.axis), glm::ivec4(p.edges, p.sign >= 0.0f ? 1 : -1, 0, 0)});
    glNamedBufferSubData(m_portBuffer.get(), 0, static_cast<GLsizeiptr>(packed.size() * sizeof(GPUPort)), packed.data());
    glClearNamedBufferData(m_phasorBuffer.get(), GL_RGBA32F, GL_RGBA, GL_FLOAT, nullptr);
    m_shader.set("uGridSize", grid.size());
    m_shader.set("uPortCount", static_cast<int>(m_ports.size()));
}

void PortMonitor::disable() {
    m_ports.clear();
    m_readings.clear();
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
    m_shader.use();
    glBindTextureUnit(0, grid.eFieldTexture());
    glBindTextureUnit(1, grid.bFieldTexture());
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 6, m_portBuffer.get());
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 7, m_phasorBuffer.get());
    glDispatchCompute(gl::divUp(static_cast<GLuint>(m_ports.size()), 64u), 1, 1);
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
            r.gamma = (r.zOhm - z0) / (r.zOhm + z0);
            const double g = std::min(std::abs(r.gamma), 0.999999);
            r.vswr = (1.0 + g) / (1.0 - g);
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
