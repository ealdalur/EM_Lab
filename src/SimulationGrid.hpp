#pragma once
// GPU FDTD solver on a Yee lattice.
//
// Two RGBA32F 3D textures hold the whole simulation state:
//   E_Field_Tex : rgb = (Ex, Ey, Ez) on cell edges,  a = conductivity sigma
//   B_Field_Tex : rgb = (Bx, By, Bz) on cell faces,  a = relative permittivity eps_r
// update_B.comp and update_E.comp alternate (leapfrog) once per time step.
//
// Units are normalised: c = eps0 = mu0 = 1, one cell = 1 length unit, so
// dt = Courant number S and a wave travels S cells per step.

#include "GLUtils.hpp"
#include "Shader.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <filesystem>
#include <vector>

// -----------------------------------------------------------------------------
// Grid / solver configuration
// -----------------------------------------------------------------------------
struct GridConfig {
    glm::ivec3 size{128, 128, 128};
    float courant = 0.5f;        // c*dt/dx; 3D stability limit is 1/sqrt(3) ~ 0.577
    int   pmlThickness = 16;     // absorbing layer depth (cells)
    float pmlSigmaMax = 1.6f;    // outer-edge conductivity; R(normal) ~ exp(-2*int sigma) ~ 1e-6
    float pmlGrading = 3.0f;     // exponential grading factor of the conductivity profile
};

// -----------------------------------------------------------------------------
// Materials (CPU-side voxel description uploaded into the alpha channels)
// -----------------------------------------------------------------------------
struct Material {
    float sigma = 0.0f;     // electric conductivity (normalised); ~100 behaves like a metal
    float epsilonR = 1.0f;  // relative permittivity (>= 1)

    static constexpr Material vacuum() { return {0.0f, 1.0f}; }
    static constexpr Material metal(float sigma = 100.0f) { return {sigma, 1.0f}; }
    static constexpr Material absorber(float sigma = 0.3f) { return {sigma, 1.0f}; }
    static constexpr Material dielectric(float epsR, float sigma = 0.0f) { return {sigma, epsR}; }
};

class MaterialVolume {
public:
    MaterialVolume() = default;
    explicit MaterialVolume(const glm::ivec3& size);

    [[nodiscard]] const glm::ivec3& size() const noexcept { return m_size; }
    [[nodiscard]] bool contains(const glm::ivec3& p) const noexcept {
        return p.x >= 0 && p.y >= 0 && p.z >= 0 && p.x < m_size.x && p.y < m_size.y && p.z < m_size.z;
    }
    [[nodiscard]] size_t index(const glm::ivec3& p) const noexcept {
        return (static_cast<size_t>(p.z) * m_size.y + p.y) * m_size.x + p.x;
    }

    void set(const glm::ivec3& p, const Material& m);
    [[nodiscard]] Material get(const glm::ivec3& p) const;

    [[nodiscard]] const std::vector<float>& sigma() const noexcept { return m_sigma; }
    [[nodiscard]] const std::vector<float>& epsilonR() const noexcept { return m_epsR; }

private:
    glm::ivec3 m_size{0};
    std::vector<float> m_sigma;
    std::vector<float> m_epsR;
};

// -----------------------------------------------------------------------------
// Current-density sources  J(x,t) = polarization * amplitude * w(x) * s(t)
// -----------------------------------------------------------------------------
enum class Waveform : int {
    ContinuousWave = 0, // s(t) = ramp(t) * sin(omega t + phase)
    GaussianPulse = 1,  // s(t) = exp(-((t-t0)/tau)^2) * sin(omega (t-t0) + phase)
};

struct SourceDesc {
    glm::vec3 center{0.0f};      // cell coordinates
    glm::vec3 halfExtent{0.0f};  // flat-top region (0 = point); large = sheet / plane wave
    float softness = 0.75f;      // Gaussian edge width (cells), clamped >= 0.25
    glm::vec3 polarization{0.0f, 0.0f, 1.0f};
    float amplitude = 1.0f;      // J0
    float wavelength = 16.0f;    // vacuum wavelength in cells (omega = 2 pi c / lambda)
    float phase = 0.0f;          // radians
    Waveform waveform = Waveform::ContinuousWave;
    float rampPeriods = 3.0f;    // CW: smooth turn-on duration; pulse: 1/e half-width tau
    float repeatPeriods = 0.0f;  // pulse repetition interval (0 = single shot)
    bool enabled = true;

    // Point-like oscillating dipole (an "oscillating electron") or, with length > 0,
    // a linear antenna element along `axis`.
    static SourceDesc dipole(const glm::vec3& center, const glm::vec3& axis, float wavelength,
                             float amplitude, float length = 0.0f);
    // Infinite current sheet normal to `normalAxis` at `position` -> plane wave.
    static SourceDesc planeWave(int normalAxis, float position, const glm::vec3& polarization,
                                float wavelength, float amplitude);

    [[nodiscard]] double omega() const noexcept;
    // Temporal signal s(t) in double precision (t in normalised time units).
    [[nodiscard]] double signal(double t) const noexcept;
};

// -----------------------------------------------------------------------------
// Solver
// -----------------------------------------------------------------------------
class SimulationGrid {
public:
    static constexpr int kMaxSources = 256;

    explicit SimulationGrid(const std::filesystem::path& shaderDir);

    // (Re)allocates the field textures when the size changes, applies solver
    // parameters and clears all fields and materials.
    void configure(const GridConfig& config);

    // Voxelised environment -> alpha channels. Also clears E and B and time.
    void uploadMaterials(const MaterialVolume& materials);

    // Replaces the active sources. Does not touch the field state, so phase
    // changes (e.g. beam steering) can be applied live.
    void setSources(const std::vector<SourceDesc>& sources);
    [[nodiscard]] const std::vector<SourceDesc>& sources() const noexcept { return m_sources; }

    // Zero E and B (materials preserved) and rewind time to 0.
    void resetFields();

    // Advance `count` leapfrog steps (B half step then E full step each).
    void step(int count);

    [[nodiscard]] GLuint eFieldTexture() const noexcept { return m_eField.get(); }
    [[nodiscard]] GLuint bFieldTexture() const noexcept { return m_bField.get(); }
    [[nodiscard]] const GridConfig& config() const noexcept { return m_config; }
    [[nodiscard]] const glm::ivec3& size() const noexcept { return m_config.size; }
    [[nodiscard]] float dt() const noexcept { return m_config.courant; }
    [[nodiscard]] double time() const noexcept { return static_cast<double>(m_stepCount) * dt(); }
    [[nodiscard]] std::uint64_t stepCount() const noexcept { return m_stepCount; }
    [[nodiscard]] size_t gpuMemoryBytes() const noexcept;

private:
    void applySolverUniforms(const Shader& s) const;
    void uploadSignals(double t);

    Shader m_updateE;
    Shader m_updateB;
    Shader m_clear;

    gl::Texture m_eField;
    gl::Texture m_bField;
    gl::Buffer  m_sourceBuffer;  // static per-source geometry (std430 Source[])
    gl::Buffer  m_signalBuffer;  // per-step s(t) values (std430 float[])

    GridConfig m_config{};
    glm::ivec3 m_allocatedSize{0};
    std::vector<SourceDesc> m_sources;
    std::vector<float> m_signalScratch;
    std::uint64_t m_stepCount = 0;
};
