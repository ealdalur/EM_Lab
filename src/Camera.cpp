#include "Camera.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>

namespace {
constexpr float kMaxPitch = glm::radians(89.0f);
constexpr float kMinDistance = 0.05f;
constexpr float kMaxDistance = 50.0f;
} // namespace

Camera::Camera() { m_flyPos = position(); }

void Camera::setAspect(float aspect) {
    if (std::isfinite(aspect) && aspect > 1e-4f) m_aspect = aspect;
}

void Camera::setFovDegrees(float fovDeg) {
    m_fovY = glm::radians(std::clamp(fovDeg, 10.0f, 120.0f));
}

void Camera::setClipPlanes(float nearZ, float farZ) {
    m_near = std::max(nearZ, 1e-4f);
    m_far = std::max(farZ, m_near * 2.0f);
}

void Camera::setOrbit(const glm::vec3& target, float distance, float yawDeg, float pitchDeg) {
    m_target = target;
    m_distance = std::clamp(distance, kMinDistance, kMaxDistance);
    m_yaw = glm::radians(yawDeg);
    m_pitch = std::clamp(glm::radians(pitchDeg), -kMaxPitch, kMaxPitch);
    m_flyPos = m_target + m_distance * orbitOffsetDir();
    m_mode = Mode::Orbit;
}

void Camera::setMode(Mode mode) {
    if (mode == m_mode) return;
    if (mode == Mode::Fly) {
        // Start flying from the current orbit eye, looking the same way.
        m_flyPos = m_target + m_distance * orbitOffsetDir();
    } else {
        // Re-pivot in front of the fly camera at the previous orbit distance.
        m_target = m_flyPos + forward() * m_distance;
    }
    m_mode = mode;
}

void Camera::toggleMode() { setMode(m_mode == Mode::Orbit ? Mode::Fly : Mode::Orbit); }

glm::vec3 Camera::orbitOffsetDir() const {
    const float cp = std::cos(m_pitch);
    return {cp * std::cos(m_yaw), cp * std::sin(m_yaw), std::sin(m_pitch)};
}

glm::vec3 Camera::forward() const { return -orbitOffsetDir(); }

glm::vec3 Camera::right() const {
    // |forward x up| >= cos(89 deg) thanks to the pitch clamp -> always normalizable.
    return glm::normalize(glm::cross(forward(), kWorldUp));
}

glm::vec3 Camera::up() const { return glm::cross(right(), forward()); }

glm::vec3 Camera::position() const {
    return m_mode == Mode::Orbit ? m_target + m_distance * orbitOffsetDir() : m_flyPos;
}

void Camera::rotate(float dx, float dy) {
    // forward = -(cos p cos y, cos p sin y, sin p).
    // Orbit: drag right -> eye swings around the pivot (yaw decreases);
    //        drag down  -> eye rises, looking down onto the scene (pitch increases).
    // Fly:   drag right -> look right (forward heading = yaw + pi, so yaw decreases);
    //        drag down  -> look down (forward.z = -sin p, so pitch increases).
    // Both modes therefore share the same mapping.
    m_yaw -= dx * m_rotateSpeed;
    m_pitch = std::clamp(m_pitch + dy * m_rotateSpeed, -kMaxPitch, kMaxPitch);
    m_yaw = std::remainder(m_yaw, glm::two_pi<float>());
}

void Camera::pan(float dx, float dy) {
    const float scale = (m_mode == Mode::Orbit ? m_distance : 1.0f) * 0.0015f;
    const glm::vec3 delta = (-dx * right() + dy * up()) * scale;
    if (m_mode == Mode::Orbit) m_target += delta;
    else m_flyPos += delta;
}

void Camera::zoom(float wheelTicks) {
    if (m_mode == Mode::Orbit) {
        m_distance = std::clamp(m_distance * std::pow(0.88f, wheelTicks), kMinDistance, kMaxDistance);
    } else {
        m_flyPos += forward() * (wheelTicks * 0.15f);
    }
}

void Camera::fly(const glm::vec3& localDir, float dt, bool fast) {
    if (m_mode != Mode::Fly || glm::dot(localDir, localDir) < 1e-8f) return;
    const glm::vec3 worldDir = localDir.x * right() + localDir.y * forward() + localDir.z * kWorldUp;
    m_flyPos += glm::normalize(worldDir) * (m_flySpeed * (fast ? 3.0f : 1.0f) * dt);
}

glm::mat4 Camera::view() const {
    const glm::vec3 eye = position();
    return glm::lookAt(eye, eye + forward(), kWorldUp);
}

glm::mat4 Camera::projection() const {
    return glm::perspective(m_fovY, m_aspect, m_near, m_far);
}
