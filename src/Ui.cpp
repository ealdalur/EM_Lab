#include "Ui.hpp"

#include <imgui.h>
#include <imgui_impl_opengl3.h>
#include <imgui_impl_sdl3.h>
#include <implot.h>

#include <filesystem>

namespace fs = std::filesystem;

namespace {
// First existing font file from a list (system fonts differ between platforms).
std::string findFont(std::initializer_list<const char*> candidates) {
    std::error_code ec;
    for (const char* c : candidates)
        if (fs::is_regular_file(c, ec)) return c;
    return {};
}
} // namespace

Ui::Ui(SDL_Window* window, SDL_GLContext context, bool persistLayout) {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImPlot::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    // No keyboard navigation: arrows, Tab and letters belong to the application (probes, params).

    // Window layout persists between runs, in the per-user settings folder (not the cwd).
    char* pref = persistLayout ? SDL_GetPrefPath("EM_Lab", "EM_Lab") : nullptr;
    if (pref) {
        m_iniPath = (fs::path(pref) / "ui_layout.ini").string();
        SDL_free(pref);
        io.IniFilename = m_iniPath.c_str();
    } else {
        io.IniFilename = nullptr;
    }

    // DPI: scale fonts and spacing with the display's content scale.
    m_scale = SDL_GetWindowDisplayScale(window);
    if (!(m_scale > 0.5f && m_scale < 4.0f)) m_scale = 1.0f;

    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 4.0f;
    style.FrameRounding = 3.0f;
    style.WindowBorderSize = 1.0f;
    style.Colors[ImGuiCol_WindowBg].w = 0.86f; // see the fields behind the panels
    style.ScaleAllSizes(m_scale);
    style.FontScaleDpi = m_scale;

    const std::string text = findFont({"C:/Windows/Fonts/segoeui.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
                                       "/System/Library/Fonts/Supplemental/Arial.ttf"});
    const std::string mono = findFont({"C:/Windows/Fonts/consola.ttf",
                                       "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
                                       "/System/Library/Fonts/Menlo.ttc"});
    if (!text.empty()) io.Fonts->AddFontFromFileTTF(text.c_str(), 16.0f);
    else io.Fonts->AddFontDefault();
    if (!mono.empty()) m_mono = io.Fonts->AddFontFromFileTTF(mono.c_str(), 15.0f);

    ImGui_ImplSDL3_InitForOpenGL(window, context);
    ImGui_ImplOpenGL3_Init("#version 460 core");
}

Ui::~Ui() {
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImPlot::DestroyContext();
    ImGui::DestroyContext();
}

bool Ui::processEvent(const SDL_Event& e) {
    ImGui_ImplSDL3_ProcessEvent(&e);
    if (!m_visible) return false;
    const ImGuiIO& io = ImGui::GetIO();
    switch (e.type) {
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_WHEEL:
            return io.WantCaptureMouse;
        case SDL_EVENT_MOUSE_MOTION:
        case SDL_EVENT_MOUSE_BUTTON_UP:
            return false; // the camera must always see releases and motion of its own drags
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP:
        case SDL_EVENT_TEXT_INPUT:
            return io.WantTextInput; // shortcuts keep working unless a text field has focus
        default:
            return false;
    }
}

void Ui::beginFrame() {
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
}

void Ui::endFrame() {
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}
