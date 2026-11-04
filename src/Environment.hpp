#pragma once
// Environment builder: voxelises analytic shapes / procedural voxel functions into
// a MaterialVolume (-> alpha channels of the field textures), collects sources and
// measurement setup, and provides the predefined educational scenes.
//
// Extending: subclass `Shape` (or use LambdaShape) and call EnvironmentBuilder::fill,
// or use EnvironmentBuilder::voxels for fully procedural structures.

#include "FieldAnalyzer.hpp"
#include "SimulationGrid.hpp"

#include <glm/glm.hpp>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// -----------------------------------------------------------------------------
// Shapes (lattice coordinates: cell (i,j,k) spans [i, i+1) and is sampled at its centre)
// -----------------------------------------------------------------------------
struct AABB {
    glm::vec3 min{0.0f};
    glm::vec3 max{0.0f};
};

class Shape {
public:
    virtual ~Shape() = default;
    [[nodiscard]] virtual bool contains(const glm::vec3& p) const = 0;
    [[nodiscard]] virtual AABB bounds() const = 0;
};

class BoxShape final : public Shape {
public:
    BoxShape(const glm::vec3& min, const glm::vec3& max) : m_min(glm::min(min, max)), m_max(glm::max(min, max)) {}
    bool contains(const glm::vec3& p) const override {
        return glm::all(glm::greaterThanEqual(p, m_min)) && glm::all(glm::lessThan(p, m_max));
    }
    AABB bounds() const override { return {m_min, m_max}; }
private:
    glm::vec3 m_min, m_max;
};

class SphereShape final : public Shape {
public:
    SphereShape(const glm::vec3& c, float r) : m_c(c), m_r(r) {}
    bool contains(const glm::vec3& p) const override { const glm::vec3 d = p - m_c; return glm::dot(d, d) <= m_r * m_r; }
    AABB bounds() const override { return {m_c - m_r, m_c + m_r}; }
private:
    glm::vec3 m_c;
    float m_r;
};

class CylinderShape final : public Shape {
public:
    // Axis-aligned cylinder: `axis` 0/1/2 = x/y/z.
    CylinderShape(const glm::vec3& c, int axis, float radius, float halfLength)
        : m_c(c), m_axis(axis), m_r(radius), m_h(halfLength) {}
    bool contains(const glm::vec3& p) const override;
    AABB bounds() const override;
private:
    glm::vec3 m_c;
    int m_axis;
    float m_r, m_h;
};

class LambdaShape final : public Shape {
public:
    LambdaShape(AABB bounds, std::function<bool(const glm::vec3&)> fn) : m_bounds(bounds), m_fn(std::move(fn)) {}
    bool contains(const glm::vec3& p) const override { return m_fn(p); }
    AABB bounds() const override { return m_bounds; }
private:
    AABB m_bounds;
    std::function<bool(const glm::vec3&)> m_fn;
};

// -----------------------------------------------------------------------------
// Scene description
// -----------------------------------------------------------------------------
struct ViewHints {
    float gainE = 1.0f;          // visual gain applied to |E| in the raymarcher
    float gainB = 1.0f;          // visual gain applied to |B|
    float cameraDistance = 3.4f; // orbit distance (world units, longest grid side = 2)
    float cameraYawDeg = -60.0f;
    float cameraPitchDeg = 28.0f;
};

struct Environment {
    std::string name;
    std::string description;
    GridConfig grid;
    MaterialVolume materials;
    std::vector<SourceDesc> sources;
    glm::vec3 probe{0.0f};
    FluxSurface flux;
    ViewHints view;
};

class EnvironmentBuilder {
public:
    EnvironmentBuilder(std::string name, const GridConfig& grid);

    // Later operations overwrite earlier ones, so carving = filling with vacuum.
    EnvironmentBuilder& fill(const Shape& shape, const Material& m);
    EnvironmentBuilder& box(const glm::vec3& min, const glm::vec3& max, const Material& m);
    EnvironmentBuilder& sphere(const glm::vec3& center, float radius, const Material& m);
    EnvironmentBuilder& cylinder(const glm::vec3& center, int axis, float radius, float halfLength, const Material& m);
    // The 12 edges of a box as square bars of the given thickness.
    EnvironmentBuilder& wireframeBox(const glm::vec3& min, const glm::vec3& max, float thickness, const Material& m);
    // Hollow box whose six faces are a square mesh of bars (pitch `spacing`).
    EnvironmentBuilder& meshCage(const glm::ivec3& min, const glm::ivec3& max, int spacing, int thickness,
                                 const Material& m);
    EnvironmentBuilder& voxel(const glm::ivec3& p, const Material& m);
    // Procedural: called for every cell; return a material to write it, nullopt to keep.
    EnvironmentBuilder& voxels(const std::function<std::optional<Material>(const glm::ivec3&)>& fn);

    EnvironmentBuilder& source(const SourceDesc& s);
    EnvironmentBuilder& sources(const std::vector<SourceDesc>& s);
    EnvironmentBuilder& probe(const glm::vec3& p);
    EnvironmentBuilder& flux(const FluxSurface& f);
    EnvironmentBuilder& view(const ViewHints& v);
    EnvironmentBuilder& description(std::string text);

    [[nodiscard]] const glm::ivec3& size() const noexcept { return m_env.grid.size; }
    [[nodiscard]] Environment build();

private:
    Environment m_env;
};

// -----------------------------------------------------------------------------
// Predefined scenes
// -----------------------------------------------------------------------------
namespace scenes {

enum class SceneId { DoubleSlit = 0, FaradayCage, AntennaArray, Dipole, DielectricLens, Count };
const char* name(SceneId id);

struct DoubleSlitParams {
    glm::ivec3 gridSize{192, 128, 96};
    float wavelength = 12.0f;
    float sourceX = 28.0f;      // plane-wave current sheet position
    int   wallX = 64;           // wall front face
    int   wallThickness = 2;
    float wallSigma = 100.0f;
    float slitWidth = 6.0f;     // along y (polarisation is across the slit: J_y)
    float slitSeparation = 36.0f;
    float slitHeight = 48.0f;   // along z
    float amplitude = 2.0f;
};

struct FaradayCageParams {
    glm::ivec3 gridSize{144, 128, 128};
    // lambda = 9x the mesh pitch, and longer than the cutoff of the lowest cavity
    // mode of the cage (2a/sqrt(2) ~ 34 cells), so leaked energy cannot resonate.
    float wavelength = 36.0f;
    float sourceX = 26.0f;
    int   halfSize = 12;        // cage half edge (cells)
    int   meshSpacing = 4;      // mesh pitch (cells)
    int   barThickness = 2;
    float sigma = 100.0f;
    bool  withCage = true;      // toggle to compare shielded vs unshielded
    float amplitude = 2.0f;
};

struct AntennaArrayParams {
    glm::ivec3 gridSize{160, 160, 96};
    int   elementCount = 8;
    float wavelength = 16.0f;
    float spacing = 8.0f;        // lambda/2: no grating lobes
    float phaseShiftDeg = 0.0f;  // progressive phase between adjacent elements
    float elementLength = 8.0f;  // dipole length along z (lambda/2)
    float amplitude = 1.0f;
};

struct DipoleParams {
    glm::ivec3 gridSize{128, 128, 128};
    float wavelength = 16.0f;
    float amplitude = 4.0f;
    bool  pulsed = false;
};

struct DielectricLensParams {
    glm::ivec3 gridSize{192, 128, 128};
    float wavelength = 12.0f;   // ~7.6 cells inside eps_r = 2.5: still well resolved
    float sourceX = 26.0f;
    float radius = 24.0f;
    float epsilonR = 2.5f;
    float amplitude = 2.0f;
};

Environment doubleSlit(const DoubleSlitParams& p = {});
Environment faradayCage(const FaradayCageParams& p = {});
Environment antennaArray(const AntennaArrayParams& p = {});
std::vector<SourceDesc> antennaArraySources(const AntennaArrayParams& p); // for live phase steering
Environment dipole(const DipoleParams& p = {});
Environment dielectricLens(const DielectricLensParams& p = {});

} // namespace scenes
