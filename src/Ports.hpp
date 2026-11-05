#pragma once
// Antenna feed ports: input impedance, reflection coefficient, VSWR and power.
//
// A port is an antenna's feed gap. With a resistive voltage-source feed (EMF V_s behind an
// internal resistance R_s, realised as the gap edge(s) conductivity plus an impressed
// current, i.e. the Norton equivalent), the monitor measures at the analysis frequency:
//   V  = gap voltage,  I = current into the antenna (loop integral of H around the gap)
//   Z_in = V_ant / I   (antenna-terminal convention, in ohms)
//   Gamma = (Z_in - Z0) / (Z_in + Z0) with Z0 = R_s,  VSWR = (1 + |Gamma|) / (1 - |Gamma|)
//   P_accepted = 1/2 Re(V_ant conj(I)),  P_available = |V_s|^2 / (8 R_s)

#include "GLUtils.hpp"
#include "Shader.hpp"

#include <glm/glm.hpp>

#include <complex>
#include <filesystem>
#include <string>
#include <vector>

class SimulationGrid;

struct PortSetup {
    std::string name;
    glm::ivec3 texel{0};      // owning texel of the gap edge (thin wire) or the gap cell (rod)
    int axis = 2;
    float sign = 1.0f;        // feed orientation along the axis
    int edges = 1;            // 1 = thin-wire gap edge, 4 = corner edges of a 1x1-cell rod gap
    float wavelength = 16.0f; // analysis wavelength (cells)
    bool voltageFeed = true;  // false: ideal current source
    float impedanceOhm = 50.0f;
    float drive = 1.0f;       // peak EMF (voltage feed) or peak current (current feed), normalized
};

struct PortReading {
    bool valid = false;
    std::complex<double> V{0.0}, I{0.0};    // normalized phasors at the antenna terminals
    std::complex<double> zOhm{0.0};         // input impedance
    std::complex<double> gamma{0.0};        // reflection coefficient vs the feed impedance
    double vswr = 0.0;
    double returnLossDb = 0.0;              // -20 log10 |Gamma|
    double acceptedPower = 0.0;             // normalized (x E0^2 dx^2 / eta0 for watts)
    double availablePower = 0.0;            // voltage feeds only
};

class PortMonitor {
public:
    explicit PortMonitor(const std::filesystem::path& shaderDir);

    void configure(const SimulationGrid& grid, const std::vector<PortSetup>& ports, float averagingPeriods);
    void disable();
    [[nodiscard]] bool enabled() const noexcept { return !m_ports.empty(); }

    void accumulate(const SimulationGrid& grid);  // after every completed step
    void evaluate();                              // GPU -> CPU readback and derived quantities

    [[nodiscard]] const std::vector<PortSetup>& ports() const noexcept { return m_ports; }
    [[nodiscard]] const std::vector<PortReading>& readings() const noexcept { return m_readings; }
    [[nodiscard]] double accumulatedPeriods(const SimulationGrid& grid) const;

private:
    Shader m_shader;
    gl::Buffer m_portBuffer;
    gl::Buffer m_phasorBuffer;
    std::vector<PortSetup> m_ports;
    std::vector<PortReading> m_readings;
    float m_wavelength = 16.0f;
    float m_averagingPeriods = 4.0f;
    double m_startTime = 0.0;
};
