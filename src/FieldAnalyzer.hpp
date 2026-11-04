#pragma once
// Measurement instruments for the educational overlay:
//  * Virtual probe : exact E / B / S at an arbitrary floating-point lattice position,
//                    with correct per-component Yee staggering.
//  * Flux meter    : power P = integral of (E x B) . n dA through an axis-aligned
//                    rectangle or a closed box (net outward power), computed on the GPU.

#include "GLUtils.hpp"
#include "Shader.hpp"

#include <glm/glm.hpp>

#include <filesystem>
#include <string>
#include <vector>

class SimulationGrid;

struct ProbeSample {
    glm::vec3 position{0.0f}; // lattice coordinates (cells)
    glm::vec3 E{0.0f};
    glm::vec3 B{0.0f};
    glm::vec3 S{0.0f};        // Poynting vector E x B
    float sigma = 0.0f;       // material at the probe
    float epsilonR = 1.0f;
    [[nodiscard]] float eMagnitude() const { return glm::length(E); }
    [[nodiscard]] float bMagnitude() const { return glm::length(B); }
    [[nodiscard]] float energyDensity() const { return 0.5f * (epsilonR * glm::dot(E, E) + glm::dot(B, B)); }
};

// Axis-aligned measurement surface in lattice coordinates.
struct FluxSurface {
    enum class Kind { Plane, Box };
    Kind kind = Kind::Plane;
    std::string label = "flux";

    // Plane: normal along `axis` at lattice index `index`, covering cells
    // [rectMin, rectMax) along the two tangential axes (in x,y,z order, skipping `axis`).
    int axis = 0;
    int index = 0;
    glm::ivec2 rectMin{0};
    glm::ivec2 rectMax{0};

    // Box: closed surface [boxMin, boxMax] (inclusive lattice node indices).
    glm::ivec3 boxMin{0};
    glm::ivec3 boxMax{0};

    static FluxSurface plane(int axis, int index, const glm::ivec2& rectMin, const glm::ivec2& rectMax,
                             std::string label);
    static FluxSurface box(const glm::ivec3& boxMin, const glm::ivec3& boxMax, std::string label);
};

struct FluxReading {
    double instantaneous = 0.0; // P(t) at the current step
    double timeAveraged = 0.0;  // exponential moving average (~ several periods)
    double peakAbs = 0.0;       // max |P| seen since reset
};

class FieldAnalyzer {
public:
    explicit FieldAnalyzer(const std::filesystem::path& shaderDir);

    // GPU -> CPU readback of a 3x3x3 texel neighbourhood; trilinear interpolation
    // is performed per component at its own staggered location.
    [[nodiscard]] ProbeSample probe(const SimulationGrid& grid, const glm::vec3& latticePos) const;

    // Power through the surface (normalised units: sum of S.n over unit cell faces).
    // Box surfaces return the net outward power.
    [[nodiscard]] double measureFlux(const SimulationGrid& grid, const FluxSurface& surface) const;

private:
    double planeFlux(const SimulationGrid& grid, int axis, int index, glm::ivec2 rectMin,
                     glm::ivec2 rectMax) const;

    Shader m_fluxShader;
    gl::Buffer m_partials;
    mutable size_t m_partialCapacity = 0; // partial-sum buffer capacity (floats)
    mutable std::vector<float> m_partialScratch;
};
