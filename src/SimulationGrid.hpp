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
#include <functional>
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
// Sub-cell model: things the cell-based materials cannot express
// -----------------------------------------------------------------------------
// Conductivity of one Yee E edge, replacing the cell-averaged value. Used for thin-wire
// conductors (a single line of perfectly conducting edges) and lumped feed resistors.
struct EdgeOverride {
    glm::ivec3 texel{0};   // owning texel: the edge E_axis sits at texel + 0.5 * e_axis
    int axis = 0;
    float sigma = 0.0f;    // normalized conductivity
    bool operator==(const EdgeOverride& o) const { return texel == o.texel && axis == o.axis && sigma == o.sigma; }
};

// Thin-wire (Umashankar-Taflove) correction: multiplies one term of the curl of E in the
// update of a B component that circles a wire. With the B component c and c1 = c+1,
// c2 = c+2 (mod 3), curl_c E = term0 - term1, term0 = d_{c1} E_{c2}, term1 = d_{c2} E_{c1}.
struct WireCoefficient {
    glm::ivec3 texel{0};   // owning texel of the B component
    int component = 0;     // 0, 1, 2 = Bx, By, Bz
    int term = 0;          // 0 or 1 (see above)
    float factor = 1.0f;   // 2 / ln(cell / radius)
    bool operator==(const WireCoefficient& o) const {
        return texel == o.texel && component == o.component && term == o.term && factor == o.factor;
    }
};

struct SubcellModel {
    std::vector<EdgeOverride> edges;
    std::vector<WireCoefficient> wireTerms;
    [[nodiscard]] bool empty() const noexcept { return edges.empty() && wireTerms.empty(); }
    bool operator==(const SubcellModel& o) const { return edges == o.edges && wireTerms == o.wireTerms; }
};

constexpr float kPerfectEdgeSigma = 6.0e4f; // largest sigma the half-float edge texture holds: a perfect conductor

// -----------------------------------------------------------------------------
// Current-density sources  J(x,t) = polarization * amplitude * w(x) * s(t)
// -----------------------------------------------------------------------------
enum class Waveform : int {
    ContinuousWave = 0, // s(t) = ramp(t) * sin(omega t + phase)
    GaussianPulse = 1,  // s(t) = exp(-((t-t0)/tau)^2) * sin(omega (t-t0) + phase)
};

struct SourceDesc {
    static constexpr float kDipoleSoftness = 0.75f; // transverse Gaussian radius of wire currents
    static constexpr float kSheetSoftness = 1.0f;   // sheet thickness: sampled sum is offset-invariant

    glm::vec3 center{0.0f};      // cell coordinates
    glm::vec3 halfExtent{0.0f};  // flat-top region (0 = point); large = sheet / plane wave
    glm::vec3 softness{kDipoleSoftness}; // per-axis Gaussian edge width (cells), clamped >= 0.25
    glm::vec3 polarization{0.0f, 0.0f, 1.0f};
    float amplitude = 1.0f;      // raw current density J0 (see the factories for physical inputs)
    float wavelength = 16.0f;    // vacuum wavelength in cells (omega = 2 pi c / lambda)
    float phase = 0.0f;          // radians
    Waveform waveform = Waveform::ContinuousWave;
    float rampPeriods = 3.0f;    // CW: smooth turn-on duration; pulse: 1/e half-width tau
    float repeatPeriods = 0.0f;  // pulse repetition interval (0 = single shot)
    float delayPeriods = 0.0f;   // start delay
    bool enabled = true;
    int emitter = -1;            // index into Environment::emitters, if this source belongs to one
    int port = -1;               // index into Environment::ports, if this is an antenna feed

    // Wire current along `axis` carrying peak current `current` (total current through the
    // cross-section). length = 0 gives a point (Hertzian) dipole, an "oscillating electron".
    static SourceDesc dipole(const glm::vec3& center, const glm::vec3& axis, float wavelength,
                             float current, float length = 0.0f);
    // Infinite current sheet normal to `normalAxis` at `position` radiating a plane wave
    // of E-field amplitude `fieldAmplitude` to each side.
    static SourceDesc planeWave(int normalAxis, float position, const glm::vec3& polarization,
                                float wavelength, float fieldAmplitude);

    // Sheet current density J0 (Gaussian thickness `softness`) whose radiated plane wave has
    // amplitude E0 on each side:  E0 = J0 * softness * sqrt(pi) * exp(-(k softness)^2 / 4) / 2.
    static float sheetCurrentForField(float fieldAmplitude, float wavelength, float softness);
    // Current density J0 such that a wire of Gaussian radius `softness` carries `current`.
    static float wireCurrentDensity(float current, float softness);

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
    void uploadMaterials(const MaterialVolume& materials, const SubcellModel& subcell = {});

    // Replaces the active sources. Does not touch the field state, so phase
    // changes (e.g. beam steering) can be applied live.
    void setSources(const std::vector<SourceDesc>& sources);
    [[nodiscard]] const std::vector<SourceDesc>& sources() const noexcept { return m_sources; }

    // Zero E and B (materials preserved) and rewind time to 0.
    void resetFields();

    // Advance `count` leapfrog steps (B half step then E full step each).
    // `afterEachStep` (optional) runs after every completed step, e.g. for monitors
    // that must sample every time step (far-field DFT accumulation).
    void step(int count, const std::function<void()>& afterEachStep = {});

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
    Shader m_applySources;       // source currents, only over the cells sources reach

    gl::Texture m_eField;
    gl::Texture m_bField;
    gl::Texture m_edgeMaterial;  // per-component (edge-averaged) sigma / eps_r, packed halves (RGBA32UI)
    gl::Texture m_wireCoef;      // thin-wire curl-term factors for the B update, packed halves (RGBA32UI)
    glm::ivec3 m_wireCoefSize{0};
    bool m_hasThinWires = false;
    gl::Buffer  m_sourceBuffer;  // static per-source geometry (std430 Source[])
    gl::Buffer  m_signalBuffer;  // per-step s(t) values (std430 float[])
    gl::Buffer  m_sourceCells;   // ivec4(cell, first list entry) per affected cell, plus a sentinel
    gl::Buffer  m_sourceLists;   // source indices per affected cell
    int m_sourceCellCount = 0;

    GridConfig m_config{};
    glm::ivec3 m_allocatedSize{0};
    std::vector<SourceDesc> m_sources;
    std::vector<float> m_signalScratch;
    std::uint64_t m_stepCount = 0;
};
