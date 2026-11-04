#include "Environment.hpp"

#include <glm/gtc/matrix_transform.hpp>

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

TransformedShape::TransformedShape(ShapePtr local, const glm::vec3& center, const glm::mat3& rotation)
    : m_local(std::move(local)), m_center(center), m_inverse(glm::transpose(rotation)) {
    // World AABB of the rotated local AABB (8 corners).
    const AABB lb = m_local->bounds();
    glm::vec3 mn(1e30f), mx(-1e30f);
    for (int i = 0; i < 8; ++i) {
        const glm::vec3 corner((i & 1) ? lb.max.x : lb.min.x, (i & 2) ? lb.max.y : lb.min.y,
                               (i & 4) ? lb.max.z : lb.min.z);
        const glm::vec3 w = center + rotation * corner;
        mn = glm::min(mn, w);
        mx = glm::max(mx, w);
    }
    m_bounds = {mn, mx};
}

bool TransformedShape::contains(const glm::vec3& p) const { return m_local->contains(m_inverse * (p - m_center)); }

WallShape::WallShape(int axis, float x0, float x1, std::vector<Aperture> apertures, const glm::ivec3& gridSize)
    : m_axis(std::clamp(axis, 0, 2)),
      m_u(m_axis == 0 ? 1 : 0),
      m_v(m_axis == 2 ? 1 : 2),
      m_x0(std::min(x0, x1)),
      m_x1(std::max(x0, x1)),
      m_apertures(std::move(apertures)) {
    m_bounds.min = glm::vec3(0.0f);
    m_bounds.max = glm::vec3(gridSize);
    m_bounds.min[m_axis] = m_x0;
    m_bounds.max[m_axis] = m_x1;
}

bool WallShape::contains(const glm::vec3& p) const {
    if (p[m_axis] < m_x0 || p[m_axis] >= m_x1) return false;
    const glm::vec2 t(p[m_u], p[m_v]);
    for (const Aperture& a : m_apertures) {
        const glm::vec2 d = t - a.center;
        if (a.circular ? glm::dot(d, d) <= a.radius * a.radius
                       : (std::abs(d.x) < a.halfSize.x && std::abs(d.y) < a.halfSize.y))
            return false;
    }
    return true;
}

glm::mat3 rotationFromEulerDegrees(const glm::vec3& deg) {
    const glm::mat4 rx = glm::rotate(glm::mat4(1.0f), glm::radians(deg.x), glm::vec3(1, 0, 0));
    const glm::mat4 ry = glm::rotate(glm::mat4(1.0f), glm::radians(deg.y), glm::vec3(0, 1, 0));
    const glm::mat4 rz = glm::rotate(glm::mat4(1.0f), glm::radians(deg.z), glm::vec3(0, 0, 1));
    return glm::mat3(rz * ry * rx); // x first, then y, then z
}

// =============================================================================
// EnvironmentBuilder
// =============================================================================
EnvironmentBuilder::EnvironmentBuilder(std::string name, const GridConfig& grid) {
    m_env.name = std::move(name);
    m_env.grid = grid;
    m_env.materials = MaterialVolume(grid.size);
}

size_t EnvironmentBuilder::fill(const Shape& shape, const Material& m) {
    const glm::ivec3 size = m_env.grid.size;
    const AABB b = shape.bounds();
    // Cells whose centre (i + 0.5) can lie inside the bounds.
    const glm::ivec3 lo = glm::clamp(glm::ivec3(glm::floor(b.min - 0.5f)), glm::ivec3(0), size);
    const glm::ivec3 hi = glm::clamp(glm::ivec3(glm::ceil(b.max + 0.5f)), glm::ivec3(0), size);
    size_t written = 0;
    for (int z = lo.z; z < hi.z; ++z)
        for (int y = lo.y; y < hi.y; ++y)
            for (int x = lo.x; x < hi.x; ++x)
                if (shape.contains(glm::vec3(x, y, z) + 0.5f)) {
                    m_env.materials.set({x, y, z}, m);
                    ++written;
                }
    return written;
}

size_t EnvironmentBuilder::box(const glm::vec3& min, const glm::vec3& max, const Material& m) {
    return fill(BoxShape(min, max), m);
}

size_t EnvironmentBuilder::sphere(const glm::vec3& c, float r, const Material& m) {
    return fill(EllipsoidShape(c, glm::vec3(r)), m);
}

size_t EnvironmentBuilder::cylinder(const glm::vec3& c, int axis, float r, float h, const Material& m) {
    return fill(CylinderShape(c, std::clamp(axis, 0, 2), r, h), m);
}

size_t EnvironmentBuilder::wireframeBox(const glm::vec3& mn, const glm::vec3& mx, float t, const Material& m) {
    size_t written = 0;
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
                written += box(lo, hi, m);
            }
    }
    return written;
}

size_t EnvironmentBuilder::meshCage(const glm::ivec3& mn, const glm::ivec3& mx, int spacing, int thickness,
                                    const Material& m) {
    const int t = std::max(thickness, 1);
    const int s = std::max(spacing, t + 1);
    size_t written = 0;
    for (int z = mn.z; z <= mx.z; ++z)
        for (int y = mn.y; y <= mx.y; ++y)
            for (int x = mn.x; x <= mx.x; ++x) {
                const glm::ivec3 p(x, y, z);
                if (!m_env.materials.contains(p)) continue;
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
                if (solid) {
                    m_env.materials.set(p, m);
                    ++written;
                }
            }
    return written;
}

size_t EnvironmentBuilder::voxels(const std::function<std::optional<Material>(const glm::ivec3&)>& fn) {
    const glm::ivec3 size = m_env.grid.size;
    size_t written = 0;
    for (int z = 0; z < size.z; ++z)
        for (int y = 0; y < size.y; ++y)
            for (int x = 0; x < size.x; ++x)
                if (auto mat = fn({x, y, z})) {
                    m_env.materials.set({x, y, z}, *mat);
                    ++written;
                }
    return written;
}
