#pragma once
// Scene description and voxeliser.
//
// An Environment is everything needed to run a simulation: grid configuration,
// voxelised materials (-> alpha channels of the field textures), current sources,
// measurement instruments and view hints. Environments are normally produced by
// the Lua scene scripts (see ScriptEngine / docs/SCRIPTING.md), which drive the
// EnvironmentBuilder below. Shapes compose: rotate any shape with TransformedShape,
// hollow it out with DifferenceShape, or supply an arbitrary predicate (LambdaShape).

#include "FarField.hpp"
#include "FieldAnalyzer.hpp"
#include "Ports.hpp"
#include "Radar.hpp"
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
using ShapePtr = std::shared_ptr<const Shape>;

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

// Axis-aligned ellipsoid (a sphere when all radii are equal).
class EllipsoidShape final : public Shape {
public:
    EllipsoidShape(const glm::vec3& c, const glm::vec3& radii) : m_c(c), m_r(glm::max(radii, glm::vec3(1e-3f))) {}
    bool contains(const glm::vec3& p) const override { const glm::vec3 d = (p - m_c) / m_r; return glm::dot(d, d) <= 1.0f; }
    AABB bounds() const override { return {m_c - m_r, m_c + m_r}; }
private:
    glm::vec3 m_c, m_r;
};

// Axis-aligned cylinder: `axis` 0/1/2 = x/y/z.
class CylinderShape final : public Shape {
public:
    CylinderShape(const glm::vec3& c, int axis, float radius, float halfLength)
        : m_c(c), m_axis(axis), m_r(radius), m_h(halfLength) {}
    bool contains(const glm::vec3& p) const override;
    AABB bounds() const override;
private:
    glm::vec3 m_c;
    int m_axis;
    float m_r, m_h;
};

// `local` is defined around the origin; it is rotated by `rotation` and moved to `center`.
class TransformedShape final : public Shape {
public:
    TransformedShape(ShapePtr local, const glm::vec3& center, const glm::mat3& rotation);
    bool contains(const glm::vec3& p) const override;
    AABB bounds() const override { return m_bounds; }
private:
    ShapePtr m_local;
    glm::vec3 m_center;
    glm::mat3 m_inverse; // world -> local
    AABB m_bounds;
};

// Points in `a` but not in `b` (hollow shells, holes).
class DifferenceShape final : public Shape {
public:
    DifferenceShape(ShapePtr a, ShapePtr b) : m_a(std::move(a)), m_b(std::move(b)) {}
    bool contains(const glm::vec3& p) const override { return m_a->contains(p) && !m_b->contains(p); }
    AABB bounds() const override { return m_a->bounds(); }
private:
    ShapePtr m_a, m_b;
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

// Infinite slab normal to `axis` spanning [x0, x1), with rectangular or circular
// apertures given in the slab's tangential (u, v) coordinates (x,y,z order skipping axis).
class WallShape final : public Shape {
public:
    struct Aperture {
        bool circular = false;
        glm::vec2 center{0.0f};
        glm::vec2 halfSize{0.0f}; // rectangle
        float radius = 0.0f;      // circle
    };
    WallShape(int axis, float x0, float x1, std::vector<Aperture> apertures, const glm::ivec3& gridSize);
    bool contains(const glm::vec3& p) const override;
    AABB bounds() const override { return m_bounds; }
private:
    int m_axis, m_u, m_v;
    float m_x0, m_x1;
    std::vector<Aperture> m_apertures;
    AABB m_bounds;
};

// -----------------------------------------------------------------------------
// Scene description
// -----------------------------------------------------------------------------
struct ViewHints {
    std::optional<float> gainE, gainB, exposure, density, stepVoxels;
    std::optional<int> mode, signedAxis, stepsPerFrame;
    std::optional<bool> overlay;
    std::optional<glm::vec3> target;   // orbit pivot in lattice coordinates (default: grid centre)
    float cameraDistance = 3.0f;       // world units (longest grid side = 2)
    float cameraYawDeg = -55.0f;
    float cameraPitchDeg = 30.0f;
};

struct ProbeSetup {
    std::string name;
    glm::vec3 position{0.0f};
    std::string logPath;               // optional CSV time series
};

// A thin-wire conductor, for drawing (the solver uses Environment::subcell).
struct WireSegment {
    glm::vec3 a{0.0f}, b{0.0f}; // lattice-node endpoints
    float radius = 0.05f;       // cells
};

// A zero-thickness metal sheet, for drawing (the solver uses Environment::subcell).
struct SheetRect {
    glm::vec3 a{0.0f}, b{0.0f}; // opposite lattice-node corners (equal along the normal axis)
};

struct FluxSetup {
    FluxSurface surface;               // surface.label is the monitor name
    std::string logPath;
};

struct Environment {
    std::string name;
    std::string description;
    GridConfig grid;
    float cellSize = 1.0e-3f;          // metres per cell (physical interpretation / unit conversion)
    float referenceWavelength = 16.0f; // cells; default wavelength of sources and loss tangents
    MaterialVolume materials;
    std::vector<SourceDesc> sources;
    std::vector<ProbeSetup> probes;
    std::vector<FluxSetup> fluxes;
    std::optional<FarFieldSetup> farField;
    SubcellModel subcell;            // thin-wire conductors and lumped feed resistors
    std::vector<WireSegment> wires;  // thin wires, for the overlay
    std::vector<SheetRect> sheets;   // zero-thickness metal sheets, for the overlay
    std::optional<SweepSetup> sweep; // frequency sweep of the antenna ports
    std::vector<PortSetup> ports;    // antenna feeds: impedance, VSWR, accepted power
    std::optional<RadarSetup> radar; // pulsed radar receiver on the antenna ports
    std::vector<TargetInfo> targets; // objects marked target = true (removed for radar calibration)
    ViewHints view;
};

class EnvironmentBuilder {
public:
    EnvironmentBuilder(std::string name, const GridConfig& grid);

    // Later operations overwrite earlier ones, so carving = filling with vacuum.
    // Returns the number of cells written.
    size_t fill(const Shape& shape, const Material& m);
    size_t box(const glm::vec3& min, const glm::vec3& max, const Material& m);
    size_t sphere(const glm::vec3& center, float radius, const Material& m);
    size_t cylinder(const glm::vec3& center, int axis, float radius, float halfLength, const Material& m);
    // The 12 edges of a box as square bars of the given thickness.
    size_t wireframeBox(const glm::vec3& min, const glm::vec3& max, float thickness, const Material& m);
    // Hollow box whose six faces are a square mesh of bars (pitch `spacing`).
    size_t meshCage(const glm::ivec3& min, const glm::ivec3& max, int spacing, int thickness, const Material& m);
    // Procedural: called for every cell; return a material to write it, nullopt to keep.
    size_t voxels(const std::function<std::optional<Material>(const glm::ivec3&)>& fn);

    [[nodiscard]] const glm::ivec3& size() const noexcept { return m_env.grid.size; }
    [[nodiscard]] Environment& env() noexcept { return m_env; }
    [[nodiscard]] Environment build() { return std::move(m_env); }

private:
    Environment m_env;
};

// Rotation matrix from Euler angles in degrees, applied about x, then y, then z.
glm::mat3 rotationFromEulerDegrees(const glm::vec3& degrees);
