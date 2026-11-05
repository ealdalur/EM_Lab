// =============================================================================
// EM_Lab - GPU FDTD electromagnetic wave simulator with volumetric raymarching.
//
// Application lifecycle (SDL3 + OpenGL 4.6 Core), input handling, camera
// control, Lua scene loading (with hot reload and interactive parameters) and
// the simulate -> measure -> render loop with console/HUD reporting.
// =============================================================================

#include "Camera.hpp"
#include "Environment.hpp"
#include "GLUtils.hpp"
#include "GpuSelection.hpp"
#include "Instruments.hpp"
#include "ScriptEngine.hpp"
#include "SimulationGrid.hpp"
#include "VolumeRenderer.hpp"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

// -----------------------------------------------------------------------------
// Command line
// -----------------------------------------------------------------------------
struct Options {
    int  scene = 1;          // 1-based index into the scenes directory
    std::string script;      // explicit script path (overrides --scene)
    std::map<std::string, std::string> sets; // --set name=value parameter overrides
    int  frames = 0;         // > 0: run this many frames then exit (self-test / benchmark)
    int  stepsPerFrame = 2;
    bool vsync = true;
    bool glDebug = false;
    bool listScenes = false;
    int  width = 1600;
    int  height = 900;
    std::string screenshot;  // save the last self-test frame to this BMP file
    std::string keys;        // key presses replayed at startup, for scripting/tests
};

void printUsage() {
    std::printf(
        "EM_Lab [options]\n"
        "  --script FILE[.lua]    load a scene script (relative: current folder, then scenes folder)\n"
        "  --scene N              load the N-th script of the scenes directory (default 1)\n"
        "  --set NAME=VALUE       override a script param{} (repeatable)\n"
        "  --list                 list the bundled scenes and exit\n"
        "  --steps N              simulation steps per frame\n"
        "  --novsync  --gldebug  --width W  --height H\n"
        "  --frames N             run N frames then exit (exit code 2 = non-finite fields)\n"
        "  --keys KEYS            replay key presses at startup\n"
        "  --screenshot FILE.bmp  save the last frame of a --frames run\n");
}

Options parseOptions(int argc, char** argv) {
    Options o;
#ifndef NDEBUG
    o.glDebug = true;
#endif
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const bool hasValue = i + 1 < argc;
        auto nextInt = [&](int fallback) { return hasValue ? std::atoi(argv[++i]) : fallback; };
        if (a == "--scene") o.scene = nextInt(o.scene);
        else if (a == "--script" && hasValue) o.script = argv[++i];
        else if (a == "--set" && hasValue) {
            const std::string kv = argv[++i];
            const auto eq = kv.find('=');
            if (eq == std::string::npos || eq == 0) std::fprintf(stderr, "--set expects NAME=VALUE (got '%s')\n", kv.c_str());
            else o.sets[kv.substr(0, eq)] = kv.substr(eq + 1);
        }
        else if (a == "--list") o.listScenes = true;
        else if (a == "--frames") o.frames = nextInt(0);
        else if (a == "--steps") o.stepsPerFrame = nextInt(o.stepsPerFrame);
        else if (a == "--novsync") o.vsync = false;
        else if (a == "--gldebug") o.glDebug = true;
        else if (a == "--width") o.width = nextInt(o.width);
        else if (a == "--height") o.height = nextInt(o.height);
        else if (a == "--screenshot" && hasValue) o.screenshot = argv[++i];
        else if (a == "--keys" && hasValue) o.keys = argv[++i];
        else if (a == "--help" || a == "-h") { printUsage(); std::exit(0); }
        else std::fprintf(stderr, "Ignoring unknown option '%s' (see --help)\n", a.c_str());
    }
    o.stepsPerFrame = std::clamp(o.stepsPerFrame, 0, 64);
    return o;
}

// Looks next to the executable first, then in the source tree (IDE / build-tree runs).
fs::path findDataDir(const char* name, const char* marker, const char* fallback) {
    if (const char* base = SDL_GetBasePath()) {
        const fs::path p = fs::path(base) / name;
        if (fs::exists(p / marker)) return p;
    }
    const fs::path f(fallback);
    if (fs::exists(f / marker)) return f;
    return {};
}

std::vector<fs::path> listScripts(const fs::path& dir) {
    std::vector<fs::path> out;
    std::error_code ec;
    if (dir.empty() || !fs::is_directory(dir, ec)) return out;
    for (const auto& e : fs::directory_iterator(dir, ec))
        if (e.is_regular_file() && e.path().extension() == ".lua") out.push_back(e.path());
    std::sort(out.begin(), out.end());
    return out;
}

// Resolves a --script argument. Absolute paths are used as given; relative paths are
// tried against the current directory first and then the scenes directory. A missing
// ".lua" extension is added. Returns an empty path if nothing exists and lists every
// absolute path that was checked in `tried`.
fs::path resolveScript(const fs::path& requested, const fs::path& sceneDir, std::vector<fs::path>& tried) {
    std::vector<fs::path> names{requested};
    if (requested.extension() != ".lua") names.push_back(fs::path(requested).concat(".lua"));

    std::error_code ec;
    std::vector<fs::path> bases;
    if (requested.is_absolute()) {
        bases.emplace_back();
    } else {
        bases.push_back(fs::current_path(ec));
        if (!sceneDir.empty()) bases.push_back(fs::absolute(sceneDir, ec));
    }
    for (const fs::path& base : bases)
        for (const fs::path& name : names) {
            const fs::path candidate = (base.empty() ? name : base / name).lexically_normal();
            if (std::find(tried.begin(), tried.end(), candidate) != tried.end()) continue;
            tried.push_back(candidate);
            if (fs::is_regular_file(candidate, ec)) return candidate;
        }
    return {};
}

bool sameGrid(const GridConfig& a, const GridConfig& b) {
    return a.size == b.size && a.courant == b.courant && a.pmlThickness == b.pmlThickness &&
           a.pmlSigmaMax == b.pmlSigmaMax && a.pmlGrading == b.pmlGrading;
}

bool sameMaterials(const MaterialVolume& a, const MaterialVolume& b) {
    return a.size() == b.size() && a.sigma() == b.sigma() && a.epsilonR() == b.epsilonR();
}

const char* keyName(char k) {
    static char buf[4];
    buf[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(k)));
    buf[1] = '\0';
    return buf;
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
        explicit GpuResources(const fs::path& shaderDir) : grid(shaderDir), renderer(shaderDir), instruments(shaderDir) {}
        SimulationGrid grid;
        VolumeRenderer renderer;
        Instruments instruments;
        GpuTimer simTimer;
        GpuTimer renderTimer;
    };

    void initWindowAndContext();
    bool loadScript(const fs::path& path, const std::map<std::string, std::string>& overrides, bool newScene);
    void loadSceneIndex(int index);
    void applyScene(ScriptResult&& result, bool newScene);
    void applyView(const Environment& env);
    void reloadWithParams();
    void checkHotReload();
    void handleEvent(const SDL_Event& e);
    void handleKey(const SDL_KeyboardEvent& k);
    void updateCamera(float dt);
    void updateTitle(double fps);
    void printHelp() const;
    void printParams() const;
    void setFlyMode(bool fly);
    void saveScreenshot(const std::string& path) const;

    Options m_opt;
    SDL_Window* m_window = nullptr;
    SDL_GLContext m_context = nullptr;
    std::unique_ptr<GpuResources> m_gpu;

    Camera m_camera;
    RenderSettings m_render;
    glm::ivec2 m_fbSize{0};
    std::vector<LineVertex> m_overlay;

    // Scene state
    std::vector<fs::path> m_sceneList;
    int m_sceneIndex = -1;
    fs::path m_scriptPath;
    fs::file_time_type m_scriptTime{};
    double m_hotReloadTimer = 0.0;
    bool m_hasScene = false;
    bool m_scriptError = false;
    std::string m_sceneName = "(no scene)";
    std::vector<ScriptParam> m_params;
    GridConfig m_gridConfig;
    MaterialVolume m_materials;
    SubcellModel m_subcell;

    // Run state
    bool m_running = true;
    bool m_paused = false;
    bool m_singleStep = false;
    int m_stepsPerFrame = 2;
    bool m_dragRotate = false;
    bool m_dragPan = false;
    bool m_screenshotRequested = false;
    std::uint64_t m_frame = 0;
};

Application::Application(const Options& opt) : m_opt(opt), m_stepsPerFrame(opt.stepsPerFrame) {
    // Prefer the source-tree scenes so that editing them hot-reloads; fall back to the
    // copy shipped next to the executable.
    fs::path sceneDir(EMLAB_SCENE_DIR);
    std::error_code ec;
    if (!fs::is_directory(sceneDir, ec)) sceneDir = findDataDir("scenes", "", EMLAB_SCENE_DIR);
    m_sceneList = listScripts(sceneDir);
    if (opt.listScenes) {
        for (size_t i = 0; i < m_sceneList.size(); ++i)
            std::printf("%2zu  %s\n", i + 1, m_sceneList[i].filename().string().c_str());
        std::exit(0);
    }

    initWindowAndContext();
    const fs::path shaderDir = findDataDir("shaders", "update_E.comp", EMLAB_SHADER_DIR);
    if (shaderDir.empty()) throw std::runtime_error("Cannot locate the shaders directory");
    std::printf("Shaders: %s\nScenes:  %zu script(s) in %s\n", shaderDir.string().c_str(), m_sceneList.size(),
                m_sceneList.empty() ? "(none found)" : m_sceneList.front().parent_path().string().c_str());
    m_gpu = std::make_unique<GpuResources>(shaderDir);

    bool loaded = false;
    if (!opt.script.empty()) {
        std::vector<fs::path> tried;
        const fs::path script = resolveScript(opt.script, sceneDir, tried);
        if (script.empty()) {
            std::fprintf(stderr, "\nSCRIPT ERROR: cannot find script '%s'. Looked for:\n", opt.script.c_str());
            for (const fs::path& t : tried) std::fprintf(stderr, "    %s\n", t.string().c_str());
            std::fprintf(stderr, "  (relative paths are tried in the current folder, then in the scenes folder)\n");
            m_scriptError = true;
        } else {
            std::printf("Script:  %s\n", script.string().c_str());
            // A script from the scenes folder keeps its place in the F6 / F7 sequence.
            for (size_t i = 0; i < m_sceneList.size(); ++i)
                if (fs::equivalent(m_sceneList[i], script, ec)) m_sceneIndex = static_cast<int>(i);
            loaded = loadScript(script, opt.sets, true);
        }
    } else if (!m_sceneList.empty()) {
        m_sceneIndex = std::clamp(opt.scene, 1, static_cast<int>(m_sceneList.size())) - 1;
        loaded = loadScript(m_sceneList[static_cast<size_t>(m_sceneIndex)], opt.sets, true);
    }
    if (!loaded) {
        // Keep the application usable (fix the script and press F5 / save to hot-reload).
        ScriptResult empty;
        GridConfig g;
        g.size = glm::ivec3(96);
        empty.env = EnvironmentBuilder("Empty scene", g).build();
        applyScene(std::move(empty), true);
    }
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

// -----------------------------------------------------------------------------
// Scene loading
// -----------------------------------------------------------------------------
bool Application::loadScript(const fs::path& path, const std::map<std::string, std::string>& overrides, bool newScene) {
    ScriptResult result;
    try {
        result = ScriptEngine::run(path, overrides);
    } catch (const ScriptError& e) {
        std::fprintf(stderr, "\nSCRIPT ERROR in %s:\n  %s\n", path.filename().string().c_str(), e.what());
        m_scriptError = true;
        // Track the file anyway so saving a fix hot-reloads it.
        m_scriptPath = path;
        std::error_code ec;
        m_scriptTime = fs::last_write_time(path, ec);
        return false;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "\nERROR while building %s: %s\n", path.filename().string().c_str(), e.what());
        m_scriptError = true;
        return false;
    }
    m_scriptError = false;
    m_scriptPath = path;
    std::error_code ec;
    m_scriptTime = fs::last_write_time(path, ec);
    applyScene(std::move(result), newScene || !m_hasScene);
    return true;
}

void Application::loadSceneIndex(int index) {
    if (index < 0 || index >= static_cast<int>(m_sceneList.size())) return;
    m_sceneIndex = index;
    loadScript(m_sceneList[static_cast<size_t>(index)], {}, true);
}

void Application::applyScene(ScriptResult&& result, bool newScene) {
    Environment& env = result.env;
    SimulationGrid& grid = m_gpu->grid;

    // Keep the running fields when only sources / instruments / view changed
    // (live parameter tweaks such as beam steering); rebuild otherwise.
    // The sub-cell model includes thin wires and feed resistors, so it counts as geometry.
    const bool keepFields = m_hasScene && !newScene && sameGrid(env.grid, m_gridConfig) &&
                            sameMaterials(env.materials, m_materials) && env.subcell == m_subcell;
    if (!keepFields) {
        grid.configure(env.grid);
        grid.uploadMaterials(env.materials, env.subcell);
        m_gridConfig = grid.config();
        m_materials = std::move(env.materials);
        m_subcell = env.subcell;
    }
    grid.setSources(env.sources);
    m_gpu->instruments.configure(grid, env, result.outputDir, result.warnings);
    m_params = std::move(result.params);
    m_hasScene = true;
    m_sceneName = env.name;
    if (newScene) applyView(env);

    for (const std::string& w : result.warnings) std::fprintf(stderr, "  warning: %s\n", w.c_str());
    if (newScene) {
        const glm::ivec3 s = grid.size();
        std::printf("\n=== %s ===  (%s)\n", env.name.c_str(), m_scriptPath.filename().string().c_str());
        if (!env.description.empty()) std::printf("%s\n", env.description.c_str());
        std::printf("Grid %dx%dx%d (%.1f M cells, %.0f MB GPU), dt = %.3f, PML %d, lambda_ref = %.1f cells "
                    "(%.4g GHz at %.3g mm cells), %zu source term(s)\n",
                    s.x, s.y, s.z, static_cast<double>(s.x) * s.y * s.z * 1e-6,
                    static_cast<double>(grid.gpuMemoryBytes()) / (1 << 20), grid.dt(), grid.config().pmlThickness,
                    env.referenceWavelength, 299792458.0 / (env.referenceWavelength * env.cellSize) * 1e-9,
                    env.cellSize * 1e3, grid.sources().size());
        printParams();
        if (m_gpu->instruments.empty()) std::printf("%s\n", Instruments::kNoInstrumentsHint);
    } else if (!keepFields) {
        std::printf("Reloaded '%s' (geometry changed: fields reset)\n", env.name.c_str());
    } else {
        std::printf("Reloaded '%s' (fields kept)\n", env.name.c_str());
    }
}

void Application::applyView(const Environment& env) {
    const ViewHints& v = env.view;
    RenderSettings r;
    r.resolutionScale = m_render.resolutionScale; // user preference, not scene content
    if (v.gainE) r.gainE = *v.gainE;
    if (v.gainB) r.gainB = *v.gainB;
    if (v.exposure) r.exposure = *v.exposure;
    if (v.density) r.density = *v.density;
    if (v.stepVoxels) r.stepVoxels = *v.stepVoxels;
    if (v.overlay) r.showOverlay = *v.overlay;
    if (v.mode) r.mode = static_cast<RenderSettings::Mode>(std::clamp(*v.mode, 0, 4));
    if (v.signedAxis) {
        r.signedAxis = *v.signedAxis;
    } else if (!env.sources.empty()) {
        const glm::vec3 pol = glm::abs(env.sources.front().polarization);
        r.signedAxis = (pol.x >= pol.y && pol.x >= pol.z) ? 0 : (pol.y >= pol.z ? 1 : 2);
    }
    m_render = r;
    if (v.stepsPerFrame) m_stepsPerFrame = *v.stepsPerFrame;

    const glm::ivec3 g = env.grid.size;
    const glm::vec3 target = VolumeRenderer::latticeToWorld(g, v.target.value_or(glm::vec3(g) * 0.5f));
    m_camera.setOrbit(target, v.cameraDistance, v.cameraYawDeg, v.cameraPitchDeg);
    setFlyMode(false);
}

void Application::reloadWithParams() {
    std::map<std::string, std::string> overrides;
    for (const ScriptParam& p : m_params) overrides[p.name] = p.overrideString();
    loadScript(m_scriptPath, overrides, false);
}

void Application::checkHotReload() {
    if (m_scriptPath.empty()) return;
    std::error_code ec;
    const auto t = fs::last_write_time(m_scriptPath, ec);
    if (ec || t == m_scriptTime) return;
    m_scriptTime = t;
    std::printf("\n%s changed on disk: reloading...\n", m_scriptPath.filename().string().c_str());
    reloadWithParams();
}

void Application::printParams() const {
    if (m_params.empty()) return;
    std::printf("Parameters:\n");
    for (const ScriptParam& p : m_params) {
        std::string keys;
        if (p.keyToggle) keys = std::string("[") + keyName(p.keyToggle) + "]";
        if (p.keyDecrease) keys = std::string("[") + keyName(p.keyDecrease) + "/" + keyName(p.keyIncrease) + "]";
        std::printf("  %-8s %-24s = %s\n", keys.c_str(), p.label.c_str(), p.valueString().c_str());
    }
}

void Application::printHelp() const {
    std::printf(
        "\nControls\n"
        "  1-9, 0         load bundled scene 1..10      F6 / F7  previous / next scene\n"
        "  F5             reload script (also automatic when the file is saved; drag & drop a .lua to open)\n"
        "  LMB drag       orbit / look      RMB or MMB drag  pan      wheel  zoom\n"
        "  C              toggle orbit / fly camera (fly: WASD, Q/E down/up, Shift fast)\n"
        "  Space          pause / resume    N  single step (paused)     R  reset fields\n"
        "  [ / ]          steps per frame   - / +  visual gain          M  render mode\n"
        "  V              signed-mode axis  K / L  cutaway (z) down/up  O  overlay  H  half-res\n"
        "  F3 / F4        ray step size     Tab  select probe  Arrows/PgUp/PgDn  move probe (Shift x5)\n"
        "  P              print measurement report      F12 screenshot   F1 help   Esc quit\n"
        "  Script parameters use the keys listed under 'Parameters' (from: %s)\n\n",
        ScriptEngine::kParamKeys);
    printParams();
}

void Application::setFlyMode(bool fly) {
    m_camera.setMode(fly ? Camera::Mode::Fly : Camera::Mode::Orbit);
    SDL_SetWindowRelativeMouseMode(m_window, fly);
}

// -----------------------------------------------------------------------------
// Input
// -----------------------------------------------------------------------------
void Application::handleKey(const SDL_KeyboardEvent& k) {
    const bool shift = (k.mod & SDL_KMOD_SHIFT) != 0;
    const float probeStep = shift ? 5.0f : 1.0f;
    Instruments& inst = m_gpu->instruments;
    const glm::ivec3 gs = m_gpu->grid.size();

    switch (k.key) {
        case SDLK_ESCAPE:
            if (m_camera.mode() == Camera::Mode::Fly) setFlyMode(false);
            else m_running = false;
            return;
        case SDLK_1: case SDLK_2: case SDLK_3: case SDLK_4: case SDLK_5:
        case SDLK_6: case SDLK_7: case SDLK_8: case SDLK_9:
            loadSceneIndex(static_cast<int>(k.key - SDLK_1));
            return;
        case SDLK_0: loadSceneIndex(9); return;
        case SDLK_F6: if (!m_sceneList.empty()) loadSceneIndex((std::max(m_sceneIndex, 0) + static_cast<int>(m_sceneList.size()) - 1) % static_cast<int>(m_sceneList.size())); return;
        case SDLK_F7: if (!m_sceneList.empty()) loadSceneIndex((m_sceneIndex + 1) % static_cast<int>(m_sceneList.size())); return;
        case SDLK_F5:
            std::printf("\nReloading %s...\n", m_scriptPath.filename().string().c_str());
            reloadWithParams();
            return;
        case SDLK_SPACE: m_paused = !m_paused; return;
        case SDLK_N: m_singleStep = true; return;
        case SDLK_R:
            m_gpu->grid.resetFields();
            inst.resetHistory(m_gpu->grid);
            return;
        case SDLK_LEFTBRACKET:  m_stepsPerFrame = std::max(m_stepsPerFrame - 1, 1); return;
        case SDLK_RIGHTBRACKET: m_stepsPerFrame = std::min(m_stepsPerFrame + 1, 64); return;
        case SDLK_EQUALS: case SDLK_KP_PLUS: m_render.gainE *= 1.25f; m_render.gainB *= 1.25f; return;
        case SDLK_MINUS: case SDLK_KP_MINUS: m_render.gainE /= 1.25f; m_render.gainB /= 1.25f; return;
        case SDLK_M: {
            const int n = static_cast<int>(RenderSettings::Mode::Count);
            m_render.mode = static_cast<RenderSettings::Mode>((static_cast<int>(m_render.mode) + 1) % n);
            std::printf("Render mode: %s\n", RenderSettings::modeName(m_render.mode));
            return;
        }
        case SDLK_V:
            m_render.signedAxis = (m_render.signedAxis + 1) % 3;
            std::printf("Signed mode shows E%c\n", "xyz"[m_render.signedAxis]);
            return;
        case SDLK_C: setFlyMode(m_camera.mode() == Camera::Mode::Orbit); return;
        case SDLK_K: m_render.clipMax.z = std::max(m_render.clipMax.z - 0.05f, 0.05f); return;
        case SDLK_L: m_render.clipMax.z = std::min(m_render.clipMax.z + 0.05f, 1.0f); return;
        case SDLK_O: m_render.showOverlay = !m_render.showOverlay; return;
        case SDLK_H:
            m_render.resolutionScale = m_render.resolutionScale < 0.99f ? 1.0f : 0.5f;
            std::printf("Render scale: %.2f\n", m_render.resolutionScale);
            return;
        case SDLK_F3: m_render.stepVoxels = std::max(m_render.stepVoxels / 1.25f, 0.1f);
                      std::printf("Ray step: %.2f voxels\n", m_render.stepVoxels); return;
        case SDLK_F4: m_render.stepVoxels = std::min(m_render.stepVoxels * 1.25f, 4.0f);
                      std::printf("Ray step: %.2f voxels\n", m_render.stepVoxels); return;
        case SDLK_TAB: inst.selectNextProbe(); inst.measure(m_gpu->grid); inst.printReport(m_gpu->grid); return;
        case SDLK_LEFT:     inst.moveSelectedProbe({-probeStep, 0.0f, 0.0f}, gs); return;
        case SDLK_RIGHT:    inst.moveSelectedProbe({+probeStep, 0.0f, 0.0f}, gs); return;
        case SDLK_DOWN:     inst.moveSelectedProbe({0.0f, -probeStep, 0.0f}, gs); return;
        case SDLK_UP:       inst.moveSelectedProbe({0.0f, +probeStep, 0.0f}, gs); return;
        case SDLK_PAGEDOWN: inst.moveSelectedProbe({0.0f, 0.0f, -probeStep}, gs); return;
        case SDLK_PAGEUP:   inst.moveSelectedProbe({0.0f, 0.0f, +probeStep}, gs); return;
        case SDLK_P:
            inst.measure(m_gpu->grid);
            inst.evaluateFarField(m_gpu->grid);
            inst.printReport(m_gpu->grid);
            if (inst.hasPorts()) inst.printAllPorts(m_gpu->grid);
            return;
        case SDLK_F12: m_screenshotRequested = true; return;
        case SDLK_F1: printHelp(); return;
        default: break;
    }

    // Script parameters
    if (k.key < 128) {
        const char c = static_cast<char>(std::tolower(static_cast<int>(k.key)));
        for (ScriptParam& p : m_params) {
            if (p.handleKey(c)) {
                std::printf("%s = %s\n", p.label.c_str(), p.valueString().c_str());
                reloadWithParams();
                return;
            }
        }
    }
}

void Application::handleEvent(const SDL_Event& e) {
    switch (e.type) {
        case SDL_EVENT_QUIT: m_running = false; break;
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
            m_fbSize = {e.window.data1, e.window.data2};
            if (m_fbSize.y > 0) m_camera.setAspect(static_cast<float>(m_fbSize.x) / static_cast<float>(m_fbSize.y));
            break;
        case SDL_EVENT_DROP_FILE:
            if (e.drop.data) {
                const fs::path p(e.drop.data);
                if (p.extension() == ".lua") {
                    m_sceneIndex = -1;
                    loadScript(p, {}, true);
                } else {
                    std::fprintf(stderr, "Dropped file is not a .lua scene script: %s\n", e.drop.data);
                }
            }
            break;
        case SDL_EVENT_KEY_DOWN: {
            const SDL_Keycode key = e.key.key;
            const bool repeatable = key == SDLK_LEFT || key == SDLK_RIGHT || key == SDLK_UP || key == SDLK_DOWN ||
                                    key == SDLK_PAGEUP || key == SDLK_PAGEDOWN;
            if (!e.key.repeat || repeatable) handleKey(e.key);
            break;
        }
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

// -----------------------------------------------------------------------------
// Reporting
// -----------------------------------------------------------------------------
void Application::updateTitle(double fps) {
    std::string params;
    for (const ScriptParam& p : m_params) params += " " + p.label + "=" + p.valueString();
    char head[256];
    std::snprintf(head, sizeof(head), "EM_Lab | %s%s | t=%.0f | %.0f fps (sim %.2f ms, render %.2f ms) | %d steps/frame%s",
                  m_sceneName.c_str(), m_scriptError ? " [SCRIPT ERROR - see console]" : "", m_gpu->grid.time(), fps,
                  m_gpu->simTimer.milliseconds(), m_gpu->renderTimer.milliseconds(), m_stepsPerFrame,
                  m_paused ? " [PAUSED]" : "");
    std::string title = head;
    if (!params.empty()) title += " |" + params;
    title += m_gpu->instruments.titleSummary();
    SDL_SetWindowTitle(m_window, title.c_str());
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

// -----------------------------------------------------------------------------
// Main loop
// -----------------------------------------------------------------------------
int Application::run() {
    Uint64 last = SDL_GetPerformanceCounter();
    const double freq = static_cast<double>(SDL_GetPerformanceFrequency());
    double titleTimer = 0.0, reportTimer = 0.0, farFieldTimer = 0.0;
    int framesSinceTitle = 0;
    SimulationGrid& grid = m_gpu->grid;
    Instruments& inst = m_gpu->instruments;

    while (m_running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) handleEvent(e);

        const Uint64 now = SDL_GetPerformanceCounter();
        const float dt = static_cast<float>(static_cast<double>(now - last) / freq);
        last = now;
        updateCamera(std::min(dt, 0.1f));

        m_hotReloadTimer += dt;
        if (m_hotReloadTimer > 0.5) {
            m_hotReloadTimer = 0.0;
            checkHotReload();
        }

        // ---- simulate --------------------------------------------------------
        const int steps = m_paused ? (m_singleStep ? 1 : 0) : m_stepsPerFrame;
        m_singleStep = false;
        if (steps > 0) {
            m_gpu->simTimer.begin();
            if (inst.needsEveryStep()) grid.step(steps, [&] { inst.onStep(grid); });
            else grid.step(steps);
            m_gpu->simTimer.end();
        }

        // ---- measure (readbacks are small but synchronous: throttle) ----------
        if (m_frame % 2 == 0 || steps == 0) inst.measure(grid);
        farFieldTimer += dt;
        if (inst.needsEveryStep() && (farFieldTimer > 1.0 || (m_opt.frames > 0 && m_frame % 50 == 49))) {
            farFieldTimer = 0.0;
            inst.evaluateFarField(grid);
        }

        // ---- render ----------------------------------------------------------
        m_gpu->renderTimer.begin();
        m_gpu->renderer.render(m_camera, grid, m_render, m_fbSize);
        if (m_render.showOverlay) {
            VolumeRenderer::buildGridOverlay(m_overlay, grid.size(), grid.config().pmlThickness);
            VolumeRenderer::appendSources(m_overlay, grid.size(), grid.sources());
            inst.appendOverlay(m_overlay, grid.size());
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
        // Periodic reports only when there is something to report (P always prints).
        if ((!selfTest && reportTimer >= 2.0) || (selfTest && m_frame % 100 == 0)) {
            if (!inst.empty()) inst.printReport(grid);
            reportTimer = 0.0;
        }
        if (selfTest && m_frame >= static_cast<std::uint64_t>(m_opt.frames)) {
            inst.measure(grid);
            inst.evaluateFarField(grid);
            if (!inst.empty()) inst.printReport(grid);
            std::printf("GPU timings: sim %.3f ms/frame (%d steps), render %.3f ms\n", m_gpu->simTimer.milliseconds(),
                        m_stepsPerFrame, m_gpu->renderTimer.milliseconds());
            m_running = false;
        }
    }
    if (m_scriptError && m_opt.frames > 0) return 3;
    return inst.unstable() ? 2 : 0;
}

} // namespace

int main(int argc, char** argv) {
    // Unbuffered console so stdout reports and stderr errors stay in order when piped.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    // Must run before SDL loads the OpenGL driver (Linux PRIME offload variables).
    gpu::requestHighPerformanceGpu();
    const Options options = parseOptions(argc, argv);
    try {
        Application app(options);
        return app.run();
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "Fatal: %s\n", ex.what());
        // A dialog would block unattended (--frames) runs until someone clicks it.
        if (options.frames == 0) SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "EM_Lab", ex.what(), nullptr);
        return 1;
    }
}
