#pragma once
// Coaxial cable: a transmission line built from voxels (square shield, dielectric, 1x1-cell
// inner conductor), and a monitor that measures the waves on it.
//
// At every station along the cable the monitor takes three line integrals of the fields:
//   V    voltage between the inner conductor and the shield: integral of E across the gap
//   I    current on the inner conductor: loop integral of H around it
//   Icm  net current on the outside of the cable: loop integral of H around the whole shield
//        (inner + shield inside + shield outside = the shield's outer-surface current alone)
// as complex amplitudes at the drive frequency (running DFT) and as instantaneous values.
//
// A lossless line carries two waves, V(t) = A e^{-j beta t} + B e^{+j beta t} (t: distance from
// the generator), toward the far end and back. A least-squares fit of the measured V(t) gives
// A and B, and from them the reflection coefficient at the far end, the VSWR, the impedance
// terminating the line and the power delivered. beta is the lattice's own TEM propagation
// constant, sin(beta / 2) = sqrt(eps_r) sin(omega dt / 2) / dt, so the fit is exact on the grid.
// The same fit of I(t) gives the line's impedance V+/I+ as an independent check on Z0.

#include "GLUtils.hpp"
#include "Shader.hpp"

#include <glm/glm.hpp>

#include <complex>
#include <filesystem>
#include <string>
#include <vector>

class SimulationGrid;

struct CoaxSetup {
    std::string name;
    int axis = 0;              // along x, y or z
    int sign = 1;              // direction from the generator toward the far end
    int start = 0;             // lattice plane of the generator end plate's inner face (t = 0)
    glm::vec3 center{0.0f};    // transverse centre (cell centre); the along-axis coordinate is unused
    int length = 0;            // cells from t = 0 to the far end of the dielectric
    int inner = 1, outer = 5, wall = 1; // inner conductor, shield inside, shield wall (cells)
    int sleeveStart = -1, sleeveEnd = -1, sleeveOuter = 0; // balun sleeve: t range and outer size
    float epsilonR = 1.0f;
    float z0Ohm = 50.0f;       // characteristic impedance on the lattice (2-D electrostatic solve)
    float wavelength = 16.0f;  // free-space wavelength of the drive (cells)
    float cellSize = 1e-3f;
    std::string termination;   // "terminals", "open", "short", "load"
    float sourceOhm = 50.0f, loadOhm = 50.0f, drive = 1.0f;
    bool pulsed = false;
};

struct CoaxReading {
    bool valid = false;        // CW analysis available
    std::vector<double> tV, tI;           // station positions (cells from the generator)
    std::vector<std::complex<double>> V, I, Icm;
    std::vector<float> vNow, iNow, cmNow; // instantaneous values
    std::complex<double> A{0.0}, B{0.0};  // forward / backward voltage waves (at t = 0)
    double beta = 0.0;                    // rad per cell
    double z0Measured = 0.0;              // V+ / I+ (ohm)
    std::complex<double> gammaEnd{0.0};   // reflection coefficient at the far end (t = length)
    std::complex<double> zEnd{0.0};       // impedance terminating the line (ohm)
    double vswr = 1.0, returnLossDb = 0.0;
    double forwardFraction = 0.0;         // forward power / generator available power
    double fitResidual = 1.0;             // misfit of the two-wave model
    double commonModeDb = -300.0;         // peak outer-shield current vs the forward line current
    double peakNow = 0.0;                 // pulse mode: running peak |V| for the plot scale
};

class CoaxMonitor {
public:
    explicit CoaxMonitor(const std::filesystem::path& shaderDir);
    ~CoaxMonitor();
    CoaxMonitor(const CoaxMonitor&) = delete;
    CoaxMonitor& operator=(const CoaxMonitor&) = delete;

    void configure(const SimulationGrid& grid, const std::vector<CoaxSetup>& lines, float averagingPeriods = 4.0f);
    void disable();
    void restart(const SimulationGrid& grid) { configure(grid, m_lines, m_averagingPeriods); }
    [[nodiscard]] bool enabled() const noexcept { return !m_lines.empty(); }
    void accumulate(const SimulationGrid& grid); // after every step
    void poll();                                 // once per frame: non-blocking readback + analysis

    [[nodiscard]] const std::vector<CoaxSetup>& lines() const noexcept { return m_lines; }
    [[nodiscard]] const std::vector<CoaxReading>& readings() const noexcept { return m_readings; }
    [[nodiscard]] double periods(const SimulationGrid& grid) const;

    // Geometry factor of the square coax on the lattice: C' = eps G, Z0 = eta0 / (sqrt(eps_r) G).
    static double geometryFactor(int inner, int outer);

private:
    struct LineLayout {
        size_t first = 0;       // first quantity
        size_t stations = 0;    // V, I and Icm per station
    };
    void analyse(size_t line, const glm::vec4* phasors, const float* now);

    Shader m_shader;
    gl::Buffer m_quantityBuffer, m_termBuffer, m_weightBuffer, m_phasorBuffer, m_nowBuffer;
    gl::ReadbackBuffer m_readback;
    GLsync m_fence = nullptr;
    std::vector<CoaxSetup> m_lines;
    std::vector<LineLayout> m_layout;
    std::vector<CoaxReading> m_readings;
    size_t m_quantities = 0;
    float m_averagingPeriods = 4.0f;
    double m_startTime = 0.0, m_dt = 0.5;
};
