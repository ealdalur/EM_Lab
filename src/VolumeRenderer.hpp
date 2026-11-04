#pragma once
// Volumetric raymarch renderer for the FDTD field textures plus a thin line
// overlay (grid bounds, absorbing layer, probe, flux surface).
//
// Lattice -> world mapping: the grid is centred at the origin and uniformly
// scaled so its longest side spans 2 world units (voxels stay cubic).

#include "GLUtils.hpp"
#include "Shader.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <filesystem>
#include <vector>

class Camera;
class SimulationGrid;
struct SourceDesc;

struct RenderSettings {
    enum class Mode : int { Fields = 0, EOnly = 1, BOnly = 2, Poynting = 3, SignedE = 4, Count = 5 };

    Mode  mode = Mode::Fields;
    int   signedAxis = 2;          // component shown in SignedE mode (0=x,1=y,2=z)
    float gainE = 1.0f;
    float gainB = 1.0f;
    float density = 1.0f;          // field opacity multiplier
    float emission = 1.0f;         // field brightness multiplier
    float stepVoxels = 0.75f;      // ray step in voxels (smaller = sharper, slower)
    float materialOpacity = 0.45f; // per-voxel extinction of metals
    float exposure = 1.25f;
    float resolutionScale = 1.0f;  // offscreen render scale (0.25 .. 1)
    glm::vec3 clipMin{0.0f};       // cutaway box in normalised grid coordinates
    glm::vec3 clipMax{1.0f};
    bool  showOverlay = true;

    static const char* modeName(Mode m);
};

struct LineVertex {
    glm::vec3 position;
    glm::vec4 color;
};

class VolumeRenderer {
public:
    explicit VolumeRenderer(const std::filesystem::path& shaderDir);

    // Renders the volume into the currently bound default framebuffer.
    void render(const Camera& camera, const SimulationGrid& grid, const RenderSettings& settings,
                const glm::ivec2& viewportSize);

    // Additively blended overlay lines in world space.
    void drawLines(const Camera& camera, const std::vector<LineVertex>& lines);

    // Grid bounds and absorbing-layer boundary (clears `out` first).
    static void buildGridOverlay(std::vector<LineVertex>& out, const glm::ivec3& gridSize, int pmlThickness);
    // Source footprints: points as crosses, wires as segments, sheets as rectangles.
    static void appendSources(std::vector<LineVertex>& out, const glm::ivec3& gridSize,
                              const std::vector<SourceDesc>& sources);

    // --- lattice <-> world mapping ------------------------------------------
    static float worldScale(const glm::ivec3& gridSize);
    static glm::vec3 latticeToWorld(const glm::ivec3& gridSize, const glm::vec3& p);
    static glm::vec3 worldMin(const glm::ivec3& gridSize);
    static glm::vec3 worldSize(const glm::ivec3& gridSize);

    static void appendBox(std::vector<LineVertex>& out, const glm::vec3& mn, const glm::vec3& mx,
                          const glm::vec4& color);
    static void appendCross(std::vector<LineVertex>& out, const glm::vec3& c, float halfSize,
                            const glm::vec4& color);

private:
    void ensureOffscreen(const glm::ivec2& size);

    Shader m_raymarch;
    Shader m_lines;
    gl::VertexArray m_emptyVao;   // full-screen triangle (attribute-less)
    gl::VertexArray m_lineVao;
    gl::Buffer m_lineVbo;
    size_t m_lineCapacity = 0;

    gl::Framebuffer m_fbo;        // reduced-resolution target
    gl::Texture m_fboColor;
    glm::ivec2 m_fboSize{0};
};
