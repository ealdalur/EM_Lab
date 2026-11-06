#pragma once
// Near-to-far-field transform (NTFF) monitor: the standard FDTD method for
// antenna radiation patterns.
//
// A closed "Huygens box" surrounding the radiators records running complex
// amplitudes of E and H at one frequency (GPU, every time step). Integrating the
// equivalent surface currents with the free-space Green's function gives the
// far-field radiation intensity in any direction, and together with the radiated
// power through the same surface, the directivity D = 4 pi U / P_rad.
//
// The box must enclose all sources and scatterers, lie in vacuum and stay out of
// the absorbing layer. Plane-wave sources that cross the box invalidate it.

#include "GLUtils.hpp"
#include "Shader.hpp"

#include <glm/glm.hpp>

#include <filesystem>
#include <string>
#include <vector>

class SimulationGrid;

struct FarFieldSetup {
    std::string name = "far field";
    glm::ivec3 boxMin{0};          // Huygens box (inclusive lattice nodes)
    glm::ivec3 boxMax{0};
    float wavelength = 16.0f;      // analysis wavelength in cells
    float averagingPeriods = 4.0f; // EMA time constant of the running DFT
    std::string logPath;           // optional CSV written on every evaluation
};

struct PatternCut {
    std::string name;              // "xy", "xz", "yz"
    glm::vec3 axisA{1, 0, 0};      // direction(alpha) = cos(alpha) axisA + sin(alpha) axisB
    glm::vec3 axisB{0, 1, 0};
    std::vector<float> angleDeg;
    std::vector<float> directivityDbi;
    float peakDbi = -100.0f;
    float peakAngleDeg = 0.0f;
    float beamwidthDeg = 360.0f;   // -3 dB width around the peak (360 = omnidirectional)
    float frontToBackDb = 0.0f;
};

struct FarFieldPattern {
    bool valid = false;
    double radiatedPower = 0.0;
    float maxDirectivityDbi = -100.0f;
    glm::vec3 maxDirection{1, 0, 0};
    std::vector<PatternCut> cuts;
    // Beam maps: directivity (dBi) on a mapSize x mapSize grid of direction cosines
    // u = sin(theta) cos(phi), v = sin(theta) sin(phi) for the upper (+z) and lower hemispheres.
    // Row-major, row 0 at v = +1, column 0 at u = -1; directions outside the unit circle hold -120.
    int mapSize = 0;
    std::vector<float> mapUpperDbi, mapLowerDbi;
};

class FarFieldMonitor {
public:
    explicit FarFieldMonitor(const std::filesystem::path& shaderDir);

    void configure(const SimulationGrid& grid, const FarFieldSetup& setup);
    void disable();
    [[nodiscard]] bool enabled() const noexcept { return m_enabled; }
    [[nodiscard]] const FarFieldSetup& setup() const noexcept { return m_setup; }

    void resetAccumulation();
    // Call after every completed time step.
    void accumulate(const SimulationGrid& grid);
    // Evaluates the radiation integrals for all cut / sphere directions.
    const FarFieldPattern& evaluate(const SimulationGrid& grid);
    [[nodiscard]] const FarFieldPattern& pattern() const noexcept { return m_pattern; }

    // Time over which the running DFT has been accumulating (for "settling" hints).
    [[nodiscard]] double accumulatedTime(const SimulationGrid& grid) const;
    bool writeCsv(const std::filesystem::path& path) const;

private:
    void uploadBoxUniforms(const Shader& s) const;

    Shader m_accumulate;
    Shader m_patternShader;
    gl::Buffer m_phasors;
    gl::Buffer m_directions;
    gl::Buffer m_output;

    bool m_enabled = false;
    FarFieldSetup m_setup;
    int m_faceOffset[7] = {};
    int m_pointCount = 0;
    int m_sphereBegin = 0; // index of the first beam-map direction
    std::vector<int> m_mapIndex; // beam-map cell -> direction index (-1 outside the unit circle)
    std::vector<glm::vec4> m_dirs;
    double m_startTime = 0.0;

    FarFieldPattern m_pattern;
};
