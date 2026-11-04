# EM_Lab — GPU FDTD Electromagnetic Wave Simulator

A real-time 3D Maxwell solver (FDTD on a Yee lattice) that runs entirely in OpenGL 4.6 compute shaders and is visualised with a volumetric raymarcher. Built with C++17, SDL3, GLAD and GLM.

## Build

Requirements: CMake ≥ 3.20, a C++17 compiler, a GPU/driver with OpenGL 4.6 Core, and the vcpkg packages `sdl3` and `glm`.

```sh
cmake -S . -B build -A x64 -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release
build/Release/EM_Lab.exe --scene 1
```

Shaders are copied next to the executable after each build. If they aren't found there, the app falls back to `<source>/shaders`.

**GPU selection.** On hybrid laptops the app runs on the discrete GPU by default and falls back to the integrated GPU when there isn't one. On Windows, the executable exports `NvOptimusEnablement` and `AmdPowerXpressRequestHighPerformance`, which tell the NVIDIA and AMD drivers to use the discrete GPU. On Linux, it sets the PRIME render-offload variables, but only if you haven't set them yourself. At startup the console lists the hardware adapters and the one OpenGL is running on. It warns if a per-app setting in Windows Graphics Settings or the NVIDIA Control Panel has forced a different GPU, because those settings override the driver hints. See [src/GpuSelection.cpp](src/GpuSelection.cpp).

Command-line options: `--scene 1-5`, `--steps N` (steps per frame), `--novsync`, `--gldebug`, `--width/--height`. For scripted runs: `--frames N` exits after N frames, `--keys "3...."` replays key presses at startup, and `--screenshot out.bmp` saves the last frame. The exit code is 2 if any non-finite field value is detected.

## Scenes and controls

| Key | Scene | What to look for |
|---|---|---|
| 1 | Double slit | Plane wave (J_y sheet), σ = 100 wall with two slits; interference fringes behind the wall |
| 2 | Faraday cage | Plane wave on a metal mesh cage (pitch λ/9). **G** toggles the cage: interior power drops by about 40 dB |
| 3 | Phased antenna array | 8 half-wave dipoles, λ/2 spacing. **, / .** change the phase step by ±15° to steer the beam (sin θ = −Δφ / kd) |
| 4 | Oscillating dipole | J_z = J₀ sin ωt. The closed flux box measures total radiated power. **T** switches between CW and pulsed |
| 5 | Dielectric ball lens | ε_r = 2.5 sphere that focuses a plane wave. The probe sits at the paraxial focus |

**Camera**
- LMB drag orbits, RMB/MMB drag pans, the wheel zooms.
- **C** switches to fly mode (WASD to move, Q/E for down/up, Shift to go faster).

**Simulation**
- **Space** pauses and resumes; **N** advances one step while paused; **R** resets the fields.
- **[ / ]** change the number of steps per frame.

**View**
- **M** cycles render modes: E+B, \|E\|, \|B\|, Poynting \|E×B\|, and signed E (crests vs. troughs). **V** picks which E component the signed mode shows.
- **− / +** adjust visual gain.
- **K / L** move the z cutaway.
- **O** toggles the overlay; **H** toggles half-resolution rendering.
- **F3 / F4** change the ray step size.

**Measurement and output**
- Arrow keys and PgUp/PgDn move the probe (hold Shift for ×5).
- **P** prints a probe and flux report; **F12** saves a screenshot.

Live readings are shown in the window title, and a report is printed to the console every 2 s.

## Architecture

```
src/
  Main.cpp            SDL3 lifecycle, input, simulate -> measure -> render loop, HUD, GPU timers
  Camera.*            Z-up orbit / fly camera (shared yaw/pitch, pitch clamped to avoid lookAt singularity)
  Shader.*            Graphics + compute programs, recursive #include, uniform cache, dispatch helper
  SimulationGrid.*    Yee textures, solver params, source SSBOs, leapfrog dispatches, field reset
  Environment.*       Shape voxeliser + fluent EnvironmentBuilder + the five scene presets
  FieldAnalyzer.*     Virtual E/B/S probe (staggering-aware), GPU Poynting flux through planes/boxes
  VolumeRenderer.*    Full-screen raymarch pass, optional reduced-resolution target, line overlay
  GLUtils.hpp         RAII GL handles, debug-output callback
shaders/
  common.glsl         Units, Yee layout, graded absorber profile, stable exponential coefficients
  update_B.comp       Faraday step   (reads E as sampler, updates B in place)
  update_E.comp       Ampère step + conduction + analytic current injection
  clear_fields.comp   Zero fields, keep materials
  poynting_flux.comp  Shared-memory reduction of (E×B)·n over a rectangle
  fullscreen.vert / raymarch.frag / lines.*
external/glad         GLAD 0.1.36 loader, OpenGL 4.6 Core (generated)
```

### Physics notes

- **Units.** c = ε₀ = μ₀ = 1 and one cell is one length unit, so dt equals the Courant number S (0.5, below the 3D limit of 1/√3). The B texture stores c·B, which means the Poynting vector is simply S = E × B.
- **Memory layout.** `E_Field_Tex` RGBA32F holds (Ex, Ey, Ez, σ). `B_Field_Tex` RGBA32F holds (Bx, By, Bz, ε_r). The staggered positions are documented in `common.glsl`. Each pass reads and writes only its own texel of the field it updates, so the update runs in place with no ping-pong buffers.
- **Lossy media.** The conduction term uses an exponential time integrator: E ← e^(−x)·E + (dt/ε)·φ(x)·(∇×B − J), where x = σ·dt/ε and φ(x) = (1 − e^(−x))/x is evaluated with a series for small x. It is unconditionally stable. Unlike the textbook Crank–Nicolson coefficients, it never gives a negative decay factor (those ring at σ = 100).
- **Absorbing boundary.** The outer 16 cells carry an exponentially graded conductivity σ(u) = σ_max·(e^(gu) − 1)/(e^g − 1). A matched magnetic loss σ_m = σ is applied in the B update, so the layer's impedance equals free space. Normal-incidence reflection is about 10⁻⁶.
- **Sources.** Each source is J = p·A·w(x)·s(t). The footprint w(x) is a soft box or Gaussian evaluated at each staggered E position, which covers points, line dipoles, and infinite current sheets. The time signal s(t) is evaluated per source on the CPU in double precision for every sub-step, so the phase doesn't drift during long runs. CW sources ramp up with a C² smootherstep to keep DC charge build-up negligible.

### Validation (measured with this code)

- Dipole radiated power: the closed-box flux reads 0.3665, against the analytic Hertzian value of 0.367 for the discretised current moment.
- Faraday cage: interior |E| goes from 0.68 to 0.016, and interior power from 229 to 0.015 (−42 dB).
- Phased array: broadside power is 0.46 at Δφ = 0°, 0.26 at 90°, and 0.035 at 180° (the predicted broadside null).

## Extending

```cpp
EnvironmentBuilder b("My scene", GridConfig{{160, 128, 128}});
b.box({60, 0, 0}, {62, 128, 128}, Material::metal())          // wall
 .cylinder({61, 64, 64}, 0, 8.0f, 2.0f, Material::vacuum())    // circular aperture
 .sphere({100, 64, 64}, 12.0f, Material::dielectric(4.0f))
 .fill(LambdaShape({{0,0,0},{160,128,128}}, [](const glm::vec3& p) { return p.y > 100 && p.z < 20; }),
       Material::absorber(0.5f))
 .voxels([](const glm::ivec3& c) -> std::optional<Material> { return std::nullopt; }) // procedural
 .source(SourceDesc::planeWave(0, 24.0f, {0, 0, 1}, 16.0f, 2.0f))
 .probe({120, 64, 64})
 .flux(FluxSurface::box({110, 54, 54}, {130, 74, 74}, "detector"));
Environment env = b.build();
grid.configure(env.grid); grid.uploadMaterials(env.materials); grid.setSources(env.sources);
```

For custom geometry, subclass `Shape` (implement `contains` and `bounds`) and pass it to `fill`.
