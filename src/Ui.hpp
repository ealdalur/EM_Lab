#pragma once
// On-screen user interface: Dear ImGui (windows, tables) and ImPlot (plots) on top of the
// SDL3 window and OpenGL context. The application and the instruments draw their panels
// between beginFrame() and endFrame(); this class owns the library state.

#include <SDL3/SDL.h>

#include <string>

struct ImFont;

class Ui {
public:
    // persistLayout: remember window positions between runs (off for unattended test runs).
    Ui(SDL_Window* window, SDL_GLContext context, bool persistLayout);
    ~Ui();
    Ui(const Ui&) = delete;
    Ui& operator=(const Ui&) = delete;

    // Forwards an event to the UI. Returns true if the UI uses it: the application then
    // ignores it (typing in a field, dragging a window or a plot).
    bool processEvent(const SDL_Event& e);

    void beginFrame();
    void endFrame(); // renders into the current framebuffer

    [[nodiscard]] bool visible() const noexcept { return m_visible; }
    void toggleVisible() noexcept { m_visible = !m_visible; }

    // Monospaced font for numeric tables (nullptr: the default font).
    [[nodiscard]] ImFont* monoFont() const noexcept { return m_mono; }
    [[nodiscard]] float scale() const noexcept { return m_scale; }

private:
    std::string m_iniPath;
    ImFont* m_mono = nullptr;
    float m_scale = 1.0f;
    bool m_visible = true;
};
