#pragma once
// Discrete-GPU selection on hybrid (iGPU + dGPU) systems.
//
// Windows: the executable exports NvOptimusEnablement / AmdPowerXpressRequestHighPerformance
//          (see GpuSelection.cpp). NVIDIA Optimus and AMD switchable-graphics drivers read
//          these at process start and create the OpenGL context on the discrete GPU. When no
//          discrete GPU exists they are ignored and the integrated GPU is used.
// Linux:   PRIME render offload is requested through environment variables, which must be
//          set before the GL library is loaded (i.e. before the first window is created).
//
// An explicit per-app choice in Windows Settings > Display > Graphics, or in the NVIDIA /
// AMD control panel, overrides the exported hints.

namespace gpu {

// Call first thing in main(), before SDL_Init. No-op on Windows (the exports act
// automatically); on Linux sets PRIME offload variables unless the user already set them.
void requestHighPerformanceGpu();

// Call after the GL context exists. Logs the available hardware adapters and warns
// if OpenGL is not running on the system's high-performance adapter.
void reportGpuSelection();

} // namespace gpu
