#pragma once
// Antenna feed ports: input impedance, reflection coefficient, VSWR and power, at the
// source frequency (continuous wave) and optionally across a frequency sweep.
//
// A port is an antenna's feed gap. With a resistive voltage-source feed (EMF V_s behind an
// internal resistance R_s, realised as the gap edge(s) conductivity plus an impressed
// current, i.e. the Norton equivalent), the monitor measures:
//   V  = gap voltage,  I = current into the antenna (loop integral of H around the gap)
//   Z_in = V_ant / I   (antenna-terminal convention, in ohms)
//   Gamma = (Z_in - Z0) / (Z_in + Z0) with Z0 = R_s,  VSWR = (1 + |Gamma|) / (1 - |Gamma|)
//   P_accepted = 1/2 Re(V_ant conj(I)),  P_available = |V_s|^2 / (8 R_s)
//
// Frequency sweep: with a pulsed source, the port's V(t) and I(t) are recorded every step
// and Fourier transformed at many frequencies; Z(f) = V(f) / I(f) does not depend on the
// pulse spectrum, so one run gives the whole impedance / S11 / VSWR curve.

#include "GLUtils.hpp"
#include "Shader.hpp"

#include <glm/glm.hpp>

#include <complex>
#include <filesystem>
#include <optional>
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

struct SweepSetup {
    float wavelengthMin = 12.0f; // cells (highest frequency)
    float wavelengthMax = 20.0f; // cells (lowest frequency)
    int points = 81;             // evenly spaced in frequency
    std::string logPath;         // optional CSV
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

struct SweepPoint {
    double wavelength = 0.0;                // cells
    std::complex<double> zOhm{0.0};
    double s11Db = 0.0;
    double vswr = 0.0;
};

struct PortSweep {
    std::vector<SweepPoint> points;
    size_t best = 0;                        // index of the minimum |S11|
    double bestWavelength = 0.0;            // interpolated resonance (minimum S11)
    bool hasBand = false;                   // S11 < -10 dB somewhere
    double bandLowWavelength = 0.0;         // -10 dB band edges (bandHigh < bandLow in wavelength)
    double bandHighWavelength = 0.0;
};

struct SweepResult {
    bool valid = false;
    size_t samples = 0;                     // recorded time steps
    bool full = false;                      // recording buffer filled
    double residual = 1.0;                  // late-time |V| relative to its peak (ring-down)
    std::vector<PortSweep> ports;
};

class PortMonitor {
public:
    explicit PortMonitor(const std::filesystem::path& shaderDir);

    void configure(const SimulationGrid& grid, const std::vector<PortSetup>& ports, float averagingPeriods,
                   const std::optional<SweepSetup>& sweep = std::nullopt, float cellSize = 1.0e-3f,
                   size_t recordSteps = 0);
    void disable();
    [[nodiscard]] bool enabled() const noexcept { return !m_ports.empty(); }
    [[nodiscard]] bool sweeping() const noexcept { return m_sweepSetup.has_value() && !m_ports.empty(); }

    void accumulate(const SimulationGrid& grid);  // after every completed step
    void evaluate();                              // single-frequency readings (GPU -> CPU)
    void evaluateSweep(double dt);                // frequency sweep from the recorded series
    // Time series recording (sweeps, or `recordSteps` > 0 in configure): (V, I) per
    // [step][port] for the steps recorded so far.
    [[nodiscard]] size_t recordedSteps() const noexcept { return m_recorded; }
    void readSeries(std::vector<glm::vec2>& out) const;

    [[nodiscard]] const std::vector<PortSetup>& ports() const noexcept { return m_ports; }
    [[nodiscard]] const std::vector<PortReading>& readings() const noexcept { return m_readings; }
    [[nodiscard]] const SweepResult& sweep() const noexcept { return m_sweep; }
    [[nodiscard]] const std::optional<SweepSetup>& sweepSetup() const noexcept { return m_sweepSetup; }
    [[nodiscard]] double accumulatedPeriods(const SimulationGrid& grid) const;
    [[nodiscard]] float cellSize() const noexcept { return m_cellSize; }

private:
    bool writeSweepCsv() const;

    Shader m_shader;
    gl::Buffer m_portBuffer;
    gl::Buffer m_phasorBuffer;
    gl::Buffer m_seriesBuffer;
    std::vector<PortSetup> m_ports;
    std::vector<PortReading> m_readings;
    float m_wavelength = 16.0f;
    float m_averagingPeriods = 4.0f;
    double m_startTime = 0.0;

    std::optional<SweepSetup> m_sweepSetup;
    size_t m_capacitySteps = 0;
    size_t m_recorded = 0;
    size_t m_recordSteps = 0;   // record-only mode (no sweep analysis)
    double m_seriesT0 = 0.0;
    float m_cellSize = 1.0e-3f;
    SweepResult m_sweep;
};
