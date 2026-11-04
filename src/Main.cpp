// =============================================================================
// EM_Lab - GPU FDTD electromagnetic wave simulator with volumetric raymarching.
//
// Application lifecycle (SDL3 + OpenGL 4.6 Core), input handling, camera
// control, the simulate -> measure -> render loop and console/HUD reporting.
// =============================================================================

#include "Camera.hpp"
#include "Environment.hpp"
#include "FieldAnalyzer.hpp"
#include "GLUtils.hpp"
#include "GpuSelection.hpp"
#include "SimulationGrid.hpp"
#include "VolumeRenderer.hpp"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using scenes::SceneId;

namespace {

// -----------------------------------------------------------------------------
// Command line
// -----------------------------------------------------------------------------
struct Options {
    int  scene = 1;          // 1-based scene index
    int  frames = 0;         // > 0: run this many frames then exit (self-test / benchmark)
    int  stepsPerFrame = 2;
    bool vsync = true;
    bool glDebug = false;
    int  width = 1600;
    int  height = 900;
    std::string screenshot; // save the last self-test frame to this BMP file
    std::string keys;       // key presses replayed at startup (e.g. "3..." or "2g"), for scripting/tests
};

Options parseOptions(int argc, char** argv) {
    Options o;
#ifndef NDEBUG
    o.glDebug = true;
#endif
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](int fallback) { return (i + 1 < argc) ? std::atoi(argv[++i]) : fallback; };
        if (a == "--scene") o.scene = next(o.scene);
        else if (a == "--frames") o.frames = next(0);
        else if (a == "--steps") o.stepsPerFrame = next(o.stepsPerFrame);
        else if (a == "--novsync") o.vsync = false;
        else if (a == "--gldebug") o.glDebug = true;
        else if (a == "--width") o.width = next(o.width);
        else if (a == "--height") o.height = next(o.height);
        else if (a == "--screenshot" && i + 1 < argc) o.screenshot = argv[++i];
        else if (a == "--keys" && i + 1 < argc) o.keys = argv[++i];
        else if (a == "--help" || a == "-h") {
            std::printf("EM_Lab [--scene 1-5] [--steps N] [--frames N] [--novsync] [--gldebug] "
                        "[--width W] [--height H] [--screenshot file.bmp] [--keys KEYS]\n");
            std::exit(0);
        }
    }
    o.scene = std::clamp(o.scene, 1, static_cast<int>(SceneId::Count));
    o.stepsPerFrame = std::clamp(o.stepsPerFrame, 0, 64);
    return o;
}

fs::path findShaderDir() {
    if (const char* base = SDL_GetBasePath()) {
        const fs::path p = fs::path(base) / "shaders";
        if (fs::exists(p / "update_E.comp")) return p;
    }
    const fs::path fallback(EMLAB_SHADER_DIR);
    if (fs::exists(fallback / "update_E.comp")) return fallback;
    throw std::runtime_error("Cannot locate the shaders directory (looked next to the executable and in " +
                             fallback.string() + ")");
}

// -----------------------------------------------------------------------------
// Non-stalling GPU timer (GL_TIME_ELAPSED, results read one frame late).
// -----------------------------------------------------------------------------
class GpuTimer {
public:
    GpuTimer() { glGenQueries(static_cast<GLsizei>(m_queries.size()), m_queries.data()); }
    ~GpuTimer() { glDeleteQueries(static_cast<GLsizei>(m_queries.size()), m_queries.data()); }
    GpuTimer(const GpuTimer&) = delete;
    GpuTimer& operator=(const GpuTimer&) = delete;

    void begin() { glBeginQuery(GL_TIME_ELAPSED, m_queries[m_index]); }
    void end() {
        glEndQuery(GL_TIME_ELAPSED);
        m_issued[m_index] = true;
        m_index = (m_index + 1) % m_queries.size();
        // Harvest the oldest query if the GPU has finished it.
        if (m_issued[m_index]) {
            GLint available = 0;
            glGetQueryObjectiv(m_queries[m_index], GL_QUERY_RESULT_AVAILABLE, &available);
            if (available) {
                GLuint64 ns = 0;
                glGetQueryObjectui64v(m_queries[m_index], GL_QUERY_RESULT, &ns);
                const double ms = static_cast<double>(ns) * 1e-6;
                m_ms = m_ms <= 0.0 ? ms : m_ms + 0.1 * (ms - m_ms);
                m_issued[m_index] = false;
            }
        }
    }
    [[nodiscard]] double milliseconds() const { return m_ms; }

private:
    std::array<GLuint, 3> m_queries{};
    std::array<bool, 3> m_issued{};
    size_t m_index = 0;
    double m_ms = 0.0;
};

// -----------------------------------------------------------------------------
// Application
// -----------------------------------------------------------------------------
class Application {
public:
    explicit Application(const Options& opt);
    ~Application();
    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    int run();

private:
    struct GpuResources {
        explicit GpuResources(const fs::path& shaderDir)
            : grid(shaderDir), renderer(shaderDir), analyzer(shaderDir) {}
        SimulationGrid grid;
        VolumeRenderer renderer;
        FieldAnalyzer analyzer;
        GpuTimer simTimer;
        GpuTimer renderTimer;
    };

    void initWindowAndContext();
    void loadScene(SceneId id, bool resetCamera = true);
    void handleEvent(const SDL_Event& e);
    void handleKey(const SDL_KeyboardEvent& k);
    void updateCamera(float dt);
    void measure();
    void updateTitle(double fps);
    void printHelp() const;
    void printReport() const;
    void moveProbe(const glm::vec3& delta);
    void setFlyMode(bool fly);
    void saveScreenshot(const std::string& path) const;

    Options m_opt;
    SDL_Window* m_window = nullptr;
    SDL_GLContext m_context = nullptr;
    std::unique_ptr<GpuResources> m_gpu;

    Camera m_camera;
    RenderSettings m_render;
    glm::ivec2 m_fbSize{0};

    // Scene state
    SceneId m_sceneId = SceneId::DoubleSlit;
    std::string m_sceneName;
    glm::vec3 m_probePos{0.0f};
    FluxSurface m_flux;
    std::vector<LineVertex> m_overlay;
    bool m_overlayDirty = true;
    scenes::DoubleSlitParams m_slitParams;
    scenes::FaradayCageParams m_cageParams;
    scenes::AntennaArrayParams m_arrayParams;
    scenes::DipoleParams m_dipoleParams;
    scenes::DielectricLensParams m_lensParams;

    // Run state
    bool m_running = true;
    bool m_paused = false;
    bool m_singleStep = false;
    int m_stepsPerFrame = 2;
    bool m_dragRotate = false;
    bool m_dragPan = false;
    ProbeSample m_probe;
    std::uint64_t m_probeStep = 0;   // step at which m_probe was measured
    double m_probeTime = 0.0;
    bool m_unstable = false;
    bool m_screenshotRequested = false;
    std::uint64_t m_frame = 0;
};

Application::Application(const Options& opt) : m_opt(opt), m_stepsPerFrame(opt.stepsPerFrame) {
    initWindowAndContext();
    const fs::path shaderDir = findShaderDir();
    std::printf("Shaders: %s\n", shaderDir.string().c_str());
    m_gpu = std::make_unique<GpuResources>(shaderDir);
    loadScene(static_cast<SceneId>(opt.scene - 1));
    printHelp();
    // Replay scripted key presses (letters/digits/punctuation map 1:1 to SDL keycodes).
    for (char c : opt.keys) {
        SDL_KeyboardEvent k{};
        k.type = SDL_EVENT_KEY_DOWN;
        k.key = static_cast<SDL_Keycode>(std::tolower(static_cast<unsigned char>(c)));
        handleKey(k);
    }
}

Application::~Application() {
    m_gpu.reset(); // GL objects must die while the context is still current
    if (m_context) SDL_GL_DestroyContext(m_context);
    if (m_window) SDL_DestroyWindow(m_window);
    SDL_Quit();
}

void Application::initWindowAndContext() {
    if (!SDL_Init(SDL_INIT_VIDEO)) throw std::runtime_error(std::string("SDL_Init failed: ") + SDL_GetError());

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 6);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, m_opt.glDebug ? SDL_GL_CONTEXT_DEBUG_FLAG : 0);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);

    m_window = SDL_CreateWindow("EM_Lab", m_opt.width, m_opt.height,
                                SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!m_window) throw std::runtime_error(std::string("SDL_CreateWindow failed: ") + SDL_GetError());

    m_context = SDL_GL_CreateContext(m_window);
    if (!m_context)
        throw std::runtime_error(std::string("OpenGL 4.6 Core context creation failed (update your GPU driver): ") +
                                 SDL_GetError());
    SDL_GL_MakeCurrent(m_window, m_context);

    if (!gladLoadGLLoader(reinterpret_cast<GLADloadproc>(SDL_GL_GetProcAddress)) || !GLAD_GL_VERSION_4_6)
        throw std::runtime_error("Failed to load OpenGL 4.6 Core entry points");

    SDL_GL_SetSwapInterval(m_opt.vsync ? 1 : 0);
    if (m_opt.glDebug) gl::enableDebugOutput();

    std::printf("OpenGL %s | %s | %s\n", reinterpret_cast<const char*>(glGetString(GL_VERSION)),
                reinterpret_cast<const char*>(glGetString(GL_RENDERER)),
                reinterpret_cast<const char*>(glGetString(GL_VENDOR)));
    gpu::reportGpuSelection();

    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(m_window, &w, &h);
    m_fbSize = {w, h};
    m_camera.setAspect(h > 0 ? static_cast<float>(w) / static_cast<float>(h) : 1.0f);
}

void Application::loadScene(SceneId id, bool resetCamera) {
    Environment env;
    switch (id) {
        case SceneId::DoubleSlit:     env = scenes::doubleSlit(m_slitParams); break;
        case SceneId::FaradayCage:    env = scenes::faradayCage(m_cageParams); break;
        case SceneId::AntennaArray:   env = scenes::antennaArray(m_arrayParams); break;
        case SceneId::Dipole:         env = scenes::dipole(m_dipoleParams); break;
        case SceneId::DielectricLens: env = scenes::dielectricLens(m_lensParams); break;
        default: return;
    }

    SimulationGrid& grid = m_gpu->grid;
    grid.configure(env.grid);
    grid.uploadMaterials(env.materials);
    grid.setSources(env.sources);
    m_gpu->analyzer.resetFluxHistory();

    m_sceneId = id;
    m_sceneName = env.name;
    m_probePos = env.probe;
    m_flux = env.flux;
    m_unstable = false;
    m_overlayDirty = true;
    if (resetCamera) {
        m_render.gainE = env.view.gainE;
        m_render.gainB = env.view.gainB;
        // Signed-field mode shows the component the (first) source drives.
        if (!env.sources.empty()) {
            const glm::vec3 pol = glm::abs(env.sources.front().polarization);
            m_render.signedAxis = (pol.x >= pol.y && pol.x >= pol.z) ? 0 : (pol.y >= pol.z ? 1 : 2);
        }
        m_render.clipMin = glm::vec3(0.0f);
        m_render.clipMax = glm::vec3(1.0f);
        m_camera.setOrbit(glm::vec3(0.0f), env.view.cameraDistance, env.view.cameraYawDeg, env.view.cameraPitchDeg);
        setFlyMode(false);
    }

    const glm::ivec3 s = grid.size();
    std::printf("\n=== Scene %d: %s ===\n%s\n", static_cast<int>(id) + 1, env.name.c_str(), env.description.c_str());
    std::printf("Grid %dx%dx%d (%.1f M cells, %.0f MB GPU), dt = %.3f, PML %d cells, %zu source(s)\n", s.x, s.y,
                s.z, static_cast<double>(s.x) * s.y * s.z * 1e-6, static_cast<double>(grid.gpuMemoryBytes()) / (1 << 20),
                grid.dt(), grid.config().pmlThickness, grid.sources().size());
}

void Application::printHelp() const {
    std::printf(
        "\nControls\n"
        "  1-5            load scene (1 double slit, 2 Faraday cage, 3 antenna array, 4 dipole, 5 lens)\n"
        "  LMB drag       orbit / look      RMB or MMB drag  pan      wheel  zoom\n"
        "  C              toggle orbit / fly camera (fly: WASD, Q/E down/up, Shift fast)\n"
        "  Space          pause / resume    N  single step (paused)     R  reset fields\n"
        "  [ / ]          steps per frame   - / +  visual gain          M  render mode\n"
        "  V              signed-mode axis  K / L  cutaway (z) down/up  O  overlay  H  half-res\n"
        "  F3 / F4        ray step size     Arrows/PgUp/PgDn  move probe (Shift x5)\n"
        "  , / .          antenna phase shift -/+ 15 deg (scene 3)\n"
        "  G              toggle Faraday cage (scene 2)   T  CW / pulsed dipole (scene 4)\n"
        "  P              print probe + flux report       F12 screenshot   F1 help   Esc quit\n\n");
}

void Application::setFlyMode(bool fly) {
    m_camera.setMode(fly ? Camera::Mode::Fly : Camera::Mode::Orbit);
    SDL_SetWindowRelativeMouseMode(m_window, fly);
}

void Application::moveProbe(const glm::vec3& delta) {
    m_probePos = glm::clamp(m_probePos + delta, glm::vec3(0.0f), glm::vec3(m_gpu->grid.size() - 1));
    m_overlayDirty = true;
    measure();
}

void Application::handleKey(const SDL_KeyboardEvent& k) {
    const bool shift = (k.mod & SDL_KMOD_SHIFT) != 0;
    const float probeStep = shift ? 5.0f : 1.0f;

    switch (k.key) {
        case SDLK_ESCAPE:
            if (m_camera.mode() == Camera::Mode::Fly) setFlyMode(false);
            else m_running = false;
            break;
        case SDLK_1: case SDLK_2: case SDLK_3: case SDLK_4: case SDLK_5:
            loadScene(static_cast<SceneId>(k.key - SDLK_1));
            break;
        case SDLK_SPACE: m_paused = !m_paused; break;
        case SDLK_N: m_singleStep = true; break;
        case SDLK_R:
            m_gpu->grid.resetFields();
            m_gpu->analyzer.resetFluxHistory();
            m_unstable = false;
            break;
        case SDLK_LEFTBRACKET:  m_stepsPerFrame = std::max(m_stepsPerFrame - 1, 1); break;
        case SDLK_RIGHTBRACKET: m_stepsPerFrame = std::min(m_stepsPerFrame + 1, 64); break;
        case SDLK_EQUALS: case SDLK_KP_PLUS:
            m_render.gainE *= 1.25f; m_render.gainB *= 1.25f; break;
        case SDLK_MINUS: case SDLK_KP_MINUS:
            m_render.gainE /= 1.25f; m_render.gainB /= 1.25f; break;
        case SDLK_M: {
            const int n = static_cast<int>(RenderSettings::Mode::Count);
            m_render.mode = static_cast<RenderSettings::Mode>((static_cast<int>(m_render.mode) + 1) % n);
            std::printf("Render mode: %s\n", RenderSettings::modeName(m_render.mode));
            break;
        }
        case SDLK_V:
            m_render.signedAxis = (m_render.signedAxis + 1) % 3;
            std::printf("Signed mode shows E%c\n", "xyz"[m_render.signedAxis]);
            break;
        case SDLK_C: setFlyMode(m_camera.mode() == Camera::Mode::Orbit); break;
        case SDLK_K: m_render.clipMax.z = std::max(m_render.clipMax.z - 0.05f, 0.05f); break;
        case SDLK_L: m_render.clipMax.z = std::min(m_render.clipMax.z + 0.05f, 1.0f); break;
        case SDLK_O: m_render.showOverlay = !m_render.showOverlay; break;
        case SDLK_H:
            m_render.resolutionScale = m_render.resolutionScale < 0.99f ? 1.0f : 0.5f;
            std::printf("Render scale: %.2f\n", m_render.resolutionScale);
            break;
        case SDLK_F3: m_render.stepVoxels = std::max(m_render.stepVoxels / 1.25f, 0.1f);
                      std::printf("Ray step: %.2f voxels\n", m_render.stepVoxels); break;
        case SDLK_F4: m_render.stepVoxels = std::min(m_render.stepVoxels * 1.25f, 4.0f);
                      std::printf("Ray step: %.2f voxels\n", m_render.stepVoxels); break;
        case SDLK_LEFT:     moveProbe({-probeStep, 0.0f, 0.0f}); break;
        case SDLK_RIGHT:    moveProbe({+probeStep, 0.0f, 0.0f}); break;
        case SDLK_DOWN:     moveProbe({0.0f, -probeStep, 0.0f}); break;
        case SDLK_UP:       moveProbe({0.0f, +probeStep, 0.0f}); break;
        case SDLK_PAGEDOWN: moveProbe({0.0f, 0.0f, -probeStep}); break;
        case SDLK_PAGEUP:   moveProbe({0.0f, 0.0f, +probeStep}); break;
        case SDLK_COMMA:
        case SDLK_PERIOD:
            if (m_sceneId == SceneId::AntennaArray) {
                float& ph = m_arrayParams.phaseShiftDeg;
                ph += (k.key == SDLK_PERIOD ? 15.0f : -15.0f);
                if (ph > 180.0f) ph -= 360.0f;
                if (ph < -180.0f) ph += 360.0f;
                m_gpu->grid.setSources(scenes::antennaArraySources(m_arrayParams));
                const double kd = glm::two_pi<double>() * m_arrayParams.spacing / m_arrayParams.wavelength;
                const double s = -glm::radians(static_cast<double>(ph)) / kd;
                if (std::abs(s) <= 1.0)
                    std::printf("Phase shift %.0f deg -> main beam at %.1f deg from broadside (+x toward +y)\n", ph,
                                glm::degrees(std::asin(s)));
                else
                    std::printf("Phase shift %.0f deg -> |sin(theta)| > 1: no propagating main beam\n", ph);
            }
            break;
        case SDLK_G:
            if (m_sceneId == SceneId::FaradayCage) {
                m_cageParams.withCage = !m_cageParams.withCage;
                loadScene(SceneId::FaradayCage, false);
            }
            break;
        case SDLK_T:
            if (m_sceneId == SceneId::Dipole) {
                m_dipoleParams.pulsed = !m_dipoleParams.pulsed;
                loadScene(SceneId::Dipole, false);
            }
            break;
        case SDLK_P: measure(); printReport(); break;
        case SDLK_F12: m_screenshotRequested = true; break;
        case SDLK_F1: printHelp(); break;
        default: break;
    }
}

void Application::handleEvent(const SDL_Event& e) {
    switch (e.type) {
        case SDL_EVENT_QUIT: m_running = false; break;
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
            m_fbSize = {e.window.data1, e.window.data2};
            if (m_fbSize.y > 0) m_camera.setAspect(static_cast<float>(m_fbSize.x) / static_cast<float>(m_fbSize.y));
            break;
        case SDL_EVENT_KEY_DOWN:
            if (!e.key.repeat || e.key.key == SDLK_LEFT || e.key.key == SDLK_RIGHT || e.key.key == SDLK_UP ||
                e.key.key == SDLK_DOWN || e.key.key == SDLK_PAGEUP || e.key.key == SDLK_PAGEDOWN)
                handleKey(e.key);
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP: {
            const bool down = e.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
            if (e.button.button == SDL_BUTTON_LEFT) m_dragRotate = down;
            if (e.button.button == SDL_BUTTON_RIGHT || e.button.button == SDL_BUTTON_MIDDLE) m_dragPan = down;
            break;
        }
        case SDL_EVENT_MOUSE_MOTION:
            if (m_camera.mode() == Camera::Mode::Fly || m_dragRotate) m_camera.rotate(e.motion.xrel, e.motion.yrel);
            else if (m_dragPan) m_camera.pan(e.motion.xrel, e.motion.yrel);
            break;
        case SDL_EVENT_MOUSE_WHEEL: {
            const float y = e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -e.wheel.y : e.wheel.y;
            m_camera.zoom(y);
            break;
        }
        default: break;
    }
}

void Application::updateCamera(float dt) {
    if (m_camera.mode() != Camera::Mode::Fly) return;
    const bool* ks = SDL_GetKeyboardState(nullptr);
    glm::vec3 d(0.0f);
    if (ks[SDL_SCANCODE_W]) d.y += 1.0f;
    if (ks[SDL_SCANCODE_S]) d.y -= 1.0f;
    if (ks[SDL_SCANCODE_D]) d.x += 1.0f;
    if (ks[SDL_SCANCODE_A]) d.x -= 1.0f;
    if (ks[SDL_SCANCODE_E]) d.z += 1.0f;
    if (ks[SDL_SCANCODE_Q]) d.z -= 1.0f;
    m_camera.fly(d, dt, ks[SDL_SCANCODE_LSHIFT] || ks[SDL_SCANCODE_RSHIFT]);
}

void Application::measure() {
    SimulationGrid& grid = m_gpu->grid;
    m_probe = m_gpu->analyzer.probe(grid, m_probePos);
    m_probeStep = grid.stepCount();
    m_probeTime = grid.time();

    // Time-average over ~3 periods of the (first) source.
    const double period = grid.sources().empty() ? 16.0 : grid.sources().front().wavelength;
    m_gpu->analyzer.updateFlux(grid, m_flux, 3.0 * period);

    const bool finite = std::isfinite(m_probe.E.x) && std::isfinite(m_probe.E.y) && std::isfinite(m_probe.E.z) &&
                        std::isfinite(m_probe.B.x) && std::isfinite(m_probe.B.y) && std::isfinite(m_probe.B.z) &&
                        std::isfinite(m_gpu->analyzer.fluxReading().instantaneous);
    if (!finite && !m_unstable) {
        m_unstable = true;
        std::fprintf(stderr, "ERROR: non-finite field values detected at step %llu\n",
                     static_cast<unsigned long long>(grid.stepCount()));
    }
}

void Application::printReport() const {
    const FluxReading& f = m_gpu->analyzer.fluxReading();
    const ProbeSample& p = m_probe;
    std::printf("[t=%8.1f step %7llu] probe (%.1f, %.1f, %.1f): |E|=%.5f E=(%+.5f %+.5f %+.5f) |B|=%.5f "
                "S=(%+.2e %+.2e %+.2e) u=%.3e sigma=%.1f eps_r=%.2f\n",
                m_probeTime, static_cast<unsigned long long>(m_probeStep), p.position.x,
                p.position.y, p.position.z, p.eMagnitude(), p.E.x, p.E.y, p.E.z, p.bMagnitude(), p.S.x, p.S.y,
                p.S.z, p.energyDensity(), p.sigma, p.epsilonR);
    std::printf("                         flux [%s]: P=%+.4e  <P>=%+.4e  peak|P|=%.4e\n", m_flux.label.c_str(),
                f.instantaneous, f.timeAveraged, f.peakAbs);
}

void Application::updateTitle(double fps) {
    const FluxReading& f = m_gpu->analyzer.fluxReading();
    char title[512];
    std::snprintf(title, sizeof(title),
                  "EM_Lab | %s | t=%.0f | %.0f fps (sim %.2f ms, render %.2f ms) | %d steps/frame%s | "
                  "probe |E|=%.4f |B|=%.4f | <P> %s = %+.3e%s",
                  m_sceneName.c_str(), m_gpu->grid.time(), fps, m_gpu->simTimer.milliseconds(),
                  m_gpu->renderTimer.milliseconds(), m_stepsPerFrame, m_paused ? " [PAUSED]" : "",
                  m_probe.eMagnitude(), m_probe.bMagnitude(), m_flux.label.c_str(), f.timeAveraged,
                  m_sceneId == SceneId::AntennaArray
                      ? (" | dphi=" + std::to_string(static_cast<int>(m_arrayParams.phaseShiftDeg)) + " deg").c_str()
                      : "");
    SDL_SetWindowTitle(m_window, title);
}

void Application::saveScreenshot(const std::string& path) const {
    const int w = m_fbSize.x, h = m_fbSize.y;
    if (w <= 0 || h <= 0) return;
    std::vector<unsigned char> pixels(static_cast<size_t>(w) * h * 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadBuffer(GL_BACK);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    // GL origin is bottom-left; image files are top-down.
    const size_t row = static_cast<size_t>(w) * 4;
    std::vector<unsigned char> flipped(pixels.size());
    for (int y = 0; y < h; ++y)
        std::copy_n(pixels.data() + static_cast<size_t>(h - 1 - y) * row, row, flipped.data() + static_cast<size_t>(y) * row);

    SDL_Surface* surface = SDL_CreateSurfaceFrom(w, h, SDL_PIXELFORMAT_RGBA32, flipped.data(), static_cast<int>(row));
    if (surface && SDL_SaveBMP(surface, path.c_str())) std::printf("Saved screenshot %s\n", path.c_str());
    else std::fprintf(stderr, "Screenshot failed: %s\n", SDL_GetError());
    if (surface) SDL_DestroySurface(surface);
}

int Application::run() {
    Uint64 last = SDL_GetPerformanceCounter();
    const double freq = static_cast<double>(SDL_GetPerformanceFrequency());
    double titleTimer = 0.0, reportTimer = 0.0;
    int framesSinceTitle = 0;

    while (m_running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) handleEvent(e);

        const Uint64 now = SDL_GetPerformanceCounter();
        const float dt = static_cast<float>(static_cast<double>(now - last) / freq);
        last = now;
        updateCamera(std::min(dt, 0.1f));

        // ---- simulate --------------------------------------------------------
        const int steps = m_paused ? (m_singleStep ? 1 : 0) : m_stepsPerFrame;
        m_singleStep = false;
        if (steps > 0) {
            m_gpu->simTimer.begin();
            m_gpu->grid.step(steps);
            m_gpu->simTimer.end();
        }

        // ---- measure (readbacks are tiny but synchronous: throttle) ----------
        if (m_frame % 2 == 0 || steps == 0) measure();

        // ---- render ----------------------------------------------------------
        m_gpu->renderTimer.begin();
        m_gpu->renderer.render(m_camera, m_gpu->grid, m_render, m_fbSize);
        if (m_render.showOverlay) {
            if (m_overlayDirty) {
                VolumeRenderer::buildOverlay(m_overlay, m_gpu->grid.size(), m_gpu->grid.config().pmlThickness,
                                             m_probePos, m_flux);
                m_overlayDirty = false;
            }
            m_gpu->renderer.drawLines(m_camera, m_overlay);
        }
        m_gpu->renderTimer.end();

        const bool lastSelfTestFrame = m_opt.frames > 0 && m_frame + 1 >= static_cast<std::uint64_t>(m_opt.frames);
        if (m_screenshotRequested) {
            char name[64];
            std::snprintf(name, sizeof(name), "em_lab_%06llu.bmp", static_cast<unsigned long long>(m_frame));
            saveScreenshot(name);
            m_screenshotRequested = false;
        } else if (lastSelfTestFrame && !m_opt.screenshot.empty()) {
            saveScreenshot(m_opt.screenshot);
        }
        SDL_GL_SwapWindow(m_window);

        // ---- reporting -------------------------------------------------------
        ++m_frame;
        ++framesSinceTitle;
        titleTimer += dt;
        reportTimer += dt;
        if (titleTimer >= 0.25) {
            updateTitle(framesSinceTitle / titleTimer);
            titleTimer = 0.0;
            framesSinceTitle = 0;
        }
        const bool selfTest = m_opt.frames > 0;
        if (reportTimer >= 2.0 || (selfTest && m_frame % 50 == 0)) {
            printReport();
            reportTimer = 0.0;
        }
        if (selfTest && m_frame >= static_cast<std::uint64_t>(m_opt.frames)) {
            measure();
            printReport();
            std::printf("GPU timings: sim %.3f ms/frame (%d steps), render %.3f ms\n", m_gpu->simTimer.milliseconds(),
                        m_stepsPerFrame, m_gpu->renderTimer.milliseconds());
            m_running = false;
        }
    }
    return m_unstable ? 2 : 0;
}

} // namespace

int main(int argc, char** argv) {
    // Must run before SDL loads the OpenGL driver (Linux PRIME offload variables).
    gpu::requestHighPerformanceGpu();
    try {
        Application app(parseOptions(argc, argv));
        return app.run();
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "Fatal: %s\n", ex.what());
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "EM_Lab", ex.what(), nullptr);
        return 1;
    }
}
