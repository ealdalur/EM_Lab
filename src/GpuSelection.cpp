#include "GpuSelection.hpp"

#include <glad/glad.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <string>

#if defined(_WIN32)
#include <windows.h>
#include <dxgi1_6.h>

// -----------------------------------------------------------------------------
// Driver hints. These must be exported from the .exe itself (not from a DLL);
// the vendor drivers look them up when the process starts.
// -----------------------------------------------------------------------------
extern "C" {
__declspec(dllexport) DWORD NvOptimusEnablement = 0x00000001;                // NVIDIA Optimus
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 0x00000001; // AMD PowerXpress
}
#endif

namespace gpu {
namespace {

std::string glString(GLenum name) {
    const auto* s = reinterpret_cast<const char*>(glGetString(name));
    return s ? s : "";
}

#if defined(_WIN32)
std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// Heuristic match between a PCI vendor id and the GL_VENDOR / GL_RENDERER strings.
bool glMatchesVendor(unsigned vendorId) {
    const std::string gl = lower(glString(GL_VENDOR) + " " + glString(GL_RENDERER));
    switch (vendorId) {
        case 0x10DE: return gl.find("nvidia") != std::string::npos;
        case 0x1002:
        case 0x1022: return gl.find("amd") != std::string::npos || gl.find("ati") != std::string::npos ||
                            gl.find("radeon") != std::string::npos;
        case 0x8086: return gl.find("intel") != std::string::npos;
        default:     return true; // unknown vendor: don't raise a false alarm
    }
}

std::string narrow(const wchar_t* w) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string out(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, out.data(), n, nullptr, nullptr);
    return out;
}

template <typename T>
void release(T*& p) {
    if (p) p->Release();
    p = nullptr;
}
#endif

} // namespace

void requestHighPerformanceGpu() {
#if defined(__linux__)
    // Never override an explicit user choice.
    if (std::getenv("__NV_PRIME_RENDER_OFFLOAD") || std::getenv("__GLX_VENDOR_LIBRARY_NAME") ||
        std::getenv("DRI_PRIME"))
        return;
    // NVIDIA proprietary driver: only request offload when the driver is actually loaded,
    // otherwise forcing the NVIDIA GLX vendor library would make context creation fail.
    if (FILE* f = std::fopen("/proc/driver/nvidia/version", "r")) {
        std::fclose(f);
        setenv("__NV_PRIME_RENDER_OFFLOAD", "1", 0);
        setenv("__GLX_VENDOR_LIBRARY_NAME", "nvidia", 0);
        setenv("__VK_LAYER_NV_optimus", "NVIDIA_only", 0);
    }
    // Mesa (AMD / Intel / nouveau): picks the secondary GPU if present, else the default one.
    setenv("DRI_PRIME", "1", 0);
#endif
    // Windows: handled by the exported NvOptimusEnablement / AmdPowerXpressRequestHighPerformance.
}

void reportGpuSelection() {
    const std::string renderer = glString(GL_RENDERER);

#if defined(_WIN32)
    IDXGIFactory1* factory1 = nullptr;
    IDXGIFactory6* factory6 = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory1)))) {
        std::printf("GPU: OpenGL renderer '%s' (adapter enumeration unavailable)\n", renderer.c_str());
        return;
    }
    factory1->QueryInterface(__uuidof(IDXGIFactory6), reinterpret_cast<void**>(&factory6));

    // Adapters ordered by the OS's high-performance preference (Windows 10 1803+);
    // fall back to plain enumeration order on older systems.
    std::string preferredName;
    unsigned preferredVendor = 0;
    int hardwareAdapters = 0;
    for (UINT i = 0;; ++i) {
        IDXGIAdapter1* adapter = nullptr;
        HRESULT hr = factory6
            ? factory6->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, __uuidof(IDXGIAdapter1),
                                                   reinterpret_cast<void**>(&adapter))
            : factory1->EnumAdapters1(i, &adapter);
        if (hr == DXGI_ERROR_NOT_FOUND || FAILED(hr)) break;

        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        release(adapter);
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue; // e.g. Microsoft Basic Render Driver

        const std::string name = narrow(desc.Description);
        std::printf("GPU: adapter %d: %s (%.1f GB dedicated VRAM)%s\n", hardwareAdapters, name.c_str(),
                    static_cast<double>(desc.DedicatedVideoMemory) / (1024.0 * 1024.0 * 1024.0),
                    hardwareAdapters == 0 && factory6 ? "  <- high-performance preference" : "");
        if (hardwareAdapters == 0) {
            preferredName = name;
            preferredVendor = desc.VendorId;
        }
        ++hardwareAdapters;
    }
    release(factory6);
    release(factory1);

    std::printf("GPU: OpenGL is running on '%s'\n", renderer.c_str());
    if (hardwareAdapters > 1 && !glMatchesVendor(preferredVendor)) {
        std::fprintf(stderr,
                     "WARNING: OpenGL is not on the high-performance GPU '%s'.\n"
                     "         An explicit per-app setting is probably overriding the driver hint. Set EM_Lab.exe\n"
                     "         to 'High performance' in Windows Settings > System > Display > Graphics, or select\n"
                     "         the discrete GPU for it in the NVIDIA Control Panel (Manage 3D settings).\n",
                     preferredName.c_str());
    }
#else
    std::printf("GPU: OpenGL is running on '%s'\n", renderer.c_str());
#endif
}

} // namespace gpu
