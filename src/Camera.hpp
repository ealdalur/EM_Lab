#pragma once
// Orbit / free-fly camera. World is Z-up (matches the physics convention where a
// dipole antenna drives J_z vertically). Yaw rotates about +Z, pitch is elevation.
// Both modes share one yaw/pitch so switching modes keeps the view continuous.

#include <glm/glm.hpp>

class Camera {
public:
    enum class Mode { Orbit, Fly };

    Camera();

    // --- configuration -------------------------------------------------------
    void setAspect(float aspect);
    void setFovDegrees(float fovDeg);
    void setClipPlanes(float nearZ, float farZ);
    void setOrbit(const glm::vec3& target, float distance, float yawDeg, float pitchDeg);

    void setMode(Mode mode);
    void toggleMode();
    [[nodiscard]] Mode mode() const noexcept { return m_mode; }

    // --- input ---------------------------------------------------------------
    // Mouse deltas in pixels.
    void rotate(float dxPixels, float dyPixels);
    void pan(float dxPixels, float dyPixels);
    // Wheel ticks; positive zooms in (orbit) or moves forward (fly).
    void zoom(float wheelTicks);
    // Fly-mode translation; `localDir` = (right, forward, up) in [-1, 1].
    void fly(const glm::vec3& localDir, float dtSeconds, bool fast);

    // --- matrices ------------------------------------------------------------
    [[nodiscard]] glm::mat4 view() const;
    [[nodiscard]] glm::mat4 projection() const;
    [[nodiscard]] glm::mat4 viewProjection() const { return projection() * view(); }

    [[nodiscard]] glm::vec3 position() const;
    [[nodiscard]] glm::vec3 forward() const;
    [[nodiscard]] glm::vec3 right() const;
    [[nodiscard]] glm::vec3 up() const;

    static constexpr glm::vec3 kWorldUp{0.0f, 0.0f, 1.0f};

private:
    [[nodiscard]] glm::vec3 orbitOffsetDir() const; // unit vector target -> eye

    Mode      m_mode = Mode::Orbit;
    glm::vec3 m_target{0.0f};     // orbit pivot
    glm::vec3 m_flyPos{0.0f};     // fly-mode eye position
    float     m_distance = 4.0f;  // orbit radius
    float     m_yaw = 0.0f;       // radians
    float     m_pitch = 0.0f;     // radians, clamped to avoid the up-vector singularity

    float m_fovY = glm::radians(50.0f);
    float m_aspect = 16.0f / 9.0f;
    float m_near = 0.01f;
    float m_far = 100.0f;

    float m_rotateSpeed = 0.005f;  // rad / pixel
    float m_flySpeed = 1.2f;       // world units / s
};
