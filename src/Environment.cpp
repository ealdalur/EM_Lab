#include "Environment.hpp"

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>

// =============================================================================
// Shapes
// =============================================================================
bool CylinderShape::contains(const glm::vec3& p) const {
    const glm::vec3 d = p - m_c;
    if (std::abs(d[m_axis]) > m_h) return false;
    const float r2 = glm::dot(d, d) - d[m_axis] * d[m_axis];
    return r2 <= m_r * m_r;
}

AABB CylinderShape::bounds() const {
    glm::vec3 ext(m_r);
    ext[m_axis] = m_h;
    return {m_c - ext, m_c + ext};
}

// =============================================================================
// EnvironmentBuilder
// =============================================================================
EnvironmentBuilder::EnvironmentBuilder(std::string name, const GridConfig& grid) {
    m_env.name = std::move(name);
    m_env.grid = grid;
    m_env.materials = MaterialVolume(grid.size);
    m_env.probe = glm::vec3(grid.size) * 0.5f;
    m_env.flux = FluxSurface::plane(0, grid.size.x / 2, glm::ivec2(grid.pmlThickness),
                                    glm::ivec2(grid.size.y, grid.size.z) - grid.pmlThickness, "mid-plane x");
}

EnvironmentBuilder& EnvironmentBuilder::fill(const Shape& shape, const Material& m) {
    const glm::ivec3 size = m_env.grid.size;
    const AABB b = shape.bounds();
    // Cells whose centre (i + 0.5) can lie inside the bounds.
    const glm::ivec3 lo = glm::clamp(glm::ivec3(glm::floor(b.min - 0.5f)), glm::ivec3(0), size);
    const glm::ivec3 hi = glm::clamp(glm::ivec3(glm::ceil(b.max + 0.5f)), glm::ivec3(0), size);
    for (int z = lo.z; z < hi.z; ++z)
        for (int y = lo.y; y < hi.y; ++y)
            for (int x = lo.x; x < hi.x; ++x)
                if (shape.contains(glm::vec3(x, y, z) + 0.5f)) m_env.materials.set({x, y, z}, m);
    return *this;
}

EnvironmentBuilder& EnvironmentBuilder::box(const glm::vec3& min, const glm::vec3& max, const Material& m) {
    return fill(BoxShape(min, max), m);
}

EnvironmentBuilder& EnvironmentBuilder::sphere(const glm::vec3& c, float r, const Material& m) {
    return fill(SphereShape(c, r), m);
}

EnvironmentBuilder& EnvironmentBuilder::cylinder(const glm::vec3& c, int axis, float r, float h, const Material& m) {
    return fill(CylinderShape(c, std::clamp(axis, 0, 2), r, h), m);
}

EnvironmentBuilder& EnvironmentBuilder::wireframeBox(const glm::vec3& mn, const glm::vec3& mx, float t,
                                                     const Material& m) {
    for (int axis = 0; axis < 3; ++axis) {
        const int u = (axis + 1) % 3;
        const int v = (axis + 2) % 3;
        for (int su = 0; su < 2; ++su)
            for (int sv = 0; sv < 2; ++sv) {
                glm::vec3 lo = mn, hi = mx;
                lo[u] = su ? mx[u] - t : mn[u];
                hi[u] = lo[u] + t;
                lo[v] = sv ? mx[v] - t : mn[v];
                hi[v] = lo[v] + t;
                box(lo, hi, m);
            }
    }
    return *this;
}

EnvironmentBuilder& EnvironmentBuilder::meshCage(const glm::ivec3& mn, const glm::ivec3& mx, int spacing,
                                                 int thickness, const Material& m) {
    const int t = std::max(thickness, 1);
    const int s = std::max(spacing, t + 1);
    for (int z = mn.z; z <= mx.z; ++z)
        for (int y = mn.y; y <= mx.y; ++y)
            for (int x = mn.x; x <= mx.x; ++x) {
                const glm::ivec3 p(x, y, z);
                bool solid = false;
                for (int a = 0; a < 3 && !solid; ++a) {
                    const bool onFace = p[a] < mn[a] + t || p[a] > mx[a] - t;
                    if (!onFace) continue;
                    const int u = (a + 1) % 3;
                    const int v = (a + 2) % 3;
                    auto onBar = [&](int axis) {
                        return (p[axis] - mn[axis]) % s < t || p[axis] > mx[axis] - t; // grid lines + far rim
                    };
                    solid = onBar(u) || onBar(v);
                }
                if (solid) m_env.materials.set(p, m);
            }
    return *this;
}

EnvironmentBuilder& EnvironmentBuilder::voxel(const glm::ivec3& p, const Material& m) {
    m_env.materials.set(p, m);
    return *this;
}

EnvironmentBuilder& EnvironmentBuilder::voxels(const std::function<std::optional<Material>(const glm::ivec3&)>& fn) {
    const glm::ivec3 size = m_env.grid.size;
    for (int z = 0; z < size.z; ++z)
        for (int y = 0; y < size.y; ++y)
            for (int x = 0; x < size.x; ++x)
                if (auto mat = fn({x, y, z})) m_env.materials.set({x, y, z}, *mat);
    return *this;
}

EnvironmentBuilder& EnvironmentBuilder::source(const SourceDesc& s) { m_env.sources.push_back(s); return *this; }
EnvironmentBuilder& EnvironmentBuilder::sources(const std::vector<SourceDesc>& s) {
    m_env.sources.insert(m_env.sources.end(), s.begin(), s.end());
    return *this;
}
EnvironmentBuilder& EnvironmentBuilder::probe(const glm::vec3& p) { m_env.probe = p; return *this; }
EnvironmentBuilder& EnvironmentBuilder::flux(const FluxSurface& f) { m_env.flux = f; return *this; }
EnvironmentBuilder& EnvironmentBuilder::view(const ViewHints& v) { m_env.view = v; return *this; }
EnvironmentBuilder& EnvironmentBuilder::description(std::string text) { m_env.description = std::move(text); return *this; }

Environment EnvironmentBuilder::build() { return std::move(m_env); }

// =============================================================================
// Scenes
// =============================================================================
namespace scenes {

const char* name(SceneId id) {
    switch (id) {
        case SceneId::DoubleSlit:     return "Double-Slit Experiment";
        case SceneId::FaradayCage:    return "Faraday Cage";
        case SceneId::AntennaArray:   return "Phased Antenna Array";
        case SceneId::Dipole:         return "Oscillating Dipole";
        case SceneId::DielectricLens: return "Dielectric Ball Lens";
        default:                      return "Unknown";
    }
}

namespace {
GridConfig makeGrid(const glm::ivec3& size) {
    GridConfig g;
    g.size = size;
    return g;
}

FluxSurface interiorPlaneX(const GridConfig& g, int x, const char* label) {
    const int L = g.pmlThickness;
    return FluxSurface::plane(0, x, glm::ivec2(L), glm::ivec2(g.size.y, g.size.z) - L, label);
}
} // namespace

Environment doubleSlit(const DoubleSlitParams& p) {
    const GridConfig g = makeGrid(p.gridSize);
    const glm::vec3 n(g.size);
    const float cy = 0.5f * n.y, cz = 0.5f * n.z;
    const float x0 = static_cast<float>(p.wallX);
    const float x1 = x0 + static_cast<float>(std::max(p.wallThickness, 1));

    EnvironmentBuilder b("Double-Slit Experiment", g);
    b.description("Plane wave (J_y current sheet) hits a metal wall with two slits; "
                  "the transmitted cylindrical wavelets interfere behind the wall.");

    // Wall spans the full cross-section (also into the absorber) so nothing leaks around it.
    b.box({x0, 0.0f, 0.0f}, {x1, n.y, n.z}, Material::metal(p.wallSigma));
    for (float side : {-1.0f, 1.0f}) {
        const float yc = cy + side * 0.5f * p.slitSeparation;
        b.box({x0, yc - 0.5f * p.slitWidth, cz - 0.5f * p.slitHeight},
              {x1, yc + 0.5f * p.slitWidth, cz + 0.5f * p.slitHeight}, Material::vacuum());
    }

    b.source(SourceDesc::planeWave(0, p.sourceX, {0.0f, 1.0f, 0.0f}, p.wavelength, p.amplitude));
    b.probe({x1 + 60.0f, cy, cz});
    b.flux(interiorPlaneX(g, p.wallX + p.wallThickness + 40, "screen plane behind slits"));
    b.view({2.2f, 2.2f, 3.0f, -58.0f, 38.0f});
    return b.build();
}

Environment faradayCage(const FaradayCageParams& p) {
    const GridConfig g = makeGrid(p.gridSize);
    const glm::ivec3 c(g.size.x / 2 + 8, g.size.y / 2, g.size.z / 2);
    const glm::ivec3 mn = c - p.halfSize, mx = c + p.halfSize;

    EnvironmentBuilder b(p.withCage ? "Faraday Cage" : "Faraday Cage (removed)", g);
    b.description("Plane wave (J_z sheet) illuminates a metal mesh cage whose pitch is much smaller "
                  "than the wavelength: the interior stays field-free. Press G to toggle the cage.");
    if (p.withCage) b.meshCage(mn, mx, p.meshSpacing, p.barThickness, Material::metal(p.sigma));

    b.source(SourceDesc::planeWave(0, p.sourceX, {0.0f, 0.0f, 1.0f}, p.wavelength, p.amplitude));
    b.probe(glm::vec3(c) + 0.5f);
    const int in = p.halfSize - p.barThickness - 2;
    b.flux(FluxSurface::plane(0, c.x, glm::ivec2(c.y - in, c.z - in), glm::ivec2(c.y + in, c.z + in),
                              "plane through cage interior"));
    b.view({0.9f, 0.9f, 2.6f, -35.0f, 30.0f});
    return b.build();
}

std::vector<SourceDesc> antennaArraySources(const AntennaArrayParams& p) {
    std::vector<SourceDesc> out;
    const glm::vec3 c = glm::vec3(p.gridSize) * 0.5f;
    const int count = std::clamp(p.elementCount, 1, SimulationGrid::kMaxSources);
    const float dphi = glm::radians(p.phaseShiftDeg);
    for (int i = 0; i < count; ++i) {
        const float offset = (static_cast<float>(i) - 0.5f * static_cast<float>(count - 1)) * p.spacing;
        SourceDesc s = SourceDesc::dipole({c.x, c.y + offset, c.z}, {0.0f, 0.0f, 1.0f}, p.wavelength,
                                          p.amplitude, p.elementLength);
        s.phase = static_cast<float>(i) * dphi;
        out.push_back(s);
    }
    return out;
}

Environment antennaArray(const AntennaArrayParams& p) {
    const GridConfig g = makeGrid(p.gridSize);
    const glm::vec3 c = glm::vec3(g.size) * 0.5f;

    EnvironmentBuilder b("Phased Antenna Array", g);
    b.description("Line array of z-directed dipoles along y. A progressive phase shift dphi steers the "
                  "main beam to sin(theta) = -dphi / (k d). Use , and . to steer.");
    b.sources(antennaArraySources(p));
    b.probe({c.x + 40.0f, c.y, c.z});
    // A 24x24-cell window on the broadside axis acts as a "beam detector": steering
    // the beam away (, and . keys) visibly reduces the power it collects.
    const glm::ivec2 axis(static_cast<int>(c.y), static_cast<int>(c.z));
    b.flux(FluxSurface::plane(0, static_cast<int>(c.x) + 40, axis - 12, axis + 12, "broadside window"));
    b.view({45.0f, 45.0f, 3.0f, -90.0f, 62.0f});
    return b.build();
}

Environment dipole(const DipoleParams& p) {
    const GridConfig g = makeGrid(p.gridSize);
    const glm::vec3 c = glm::vec3(g.size) * 0.5f;

    EnvironmentBuilder b("Oscillating Dipole", g);
    b.description("A point current J_z = J0 sin(wt) (an oscillating electron). The closed flux box "
                  "measures the total radiated power (net outward Poynting flux).");
    SourceDesc s = SourceDesc::dipole(c, {0.0f, 0.0f, 1.0f}, p.wavelength, p.amplitude);
    if (p.pulsed) {
        s.waveform = Waveform::GaussianPulse;
        s.rampPeriods = 0.8f;
        s.repeatPeriods = 10.0f;
    }
    b.source(s);
    b.probe(c + glm::vec3(24.0f, 0.0f, 0.0f));
    const glm::ivec3 ci(c);
    b.flux(FluxSurface::box(ci - 20, ci + 20, "closed box around dipole"));
    b.view({90.0f, 90.0f, 2.4f, -60.0f, 22.0f});
    return b.build();
}

Environment dielectricLens(const DielectricLensParams& p) {
    const GridConfig g = makeGrid(p.gridSize);
    const glm::vec3 n(g.size);
    const glm::vec3 center(80.0f, 0.5f * n.y, 0.5f * n.z);
    const float ri = std::sqrt(std::max(p.epsilonR, 1.0f));
    // Paraxial focal length of a ball lens measured from its centre: f = n R / (2 (n - 1)).
    const float f = ri > 1.0001f ? ri * p.radius / (2.0f * (ri - 1.0f)) : 4.0f * p.radius;
    const float focusX = std::min(center.x + f, n.x - g.pmlThickness - 4.0f);

    EnvironmentBuilder b("Dielectric Ball Lens", g);
    b.description("A plane wave is refracted by a sphere with eps_r > 1 (slower phase velocity) and "
                  "converges to a focus behind it.");
    b.sphere(center, p.radius, Material::dielectric(p.epsilonR));
    b.source(SourceDesc::planeWave(0, p.sourceX, {0.0f, 0.0f, 1.0f}, p.wavelength, p.amplitude));
    b.probe({focusX, center.y, center.z});
    const glm::ivec2 axis(static_cast<int>(center.y), static_cast<int>(center.z));
    b.flux(FluxSurface::plane(0, static_cast<int>(focusX), axis - 6, axis + 6, "focal spot (12x12 cells)"));
    b.view({0.8f, 0.8f, 3.0f, -55.0f, 30.0f});
    return b.build();
}

} // namespace scenes
