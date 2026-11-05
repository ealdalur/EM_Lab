# EM_Lab — GPU FDTD Electromagnetic Wave Simulator

A real-time 3D Maxwell solver (FDTD on a Yee lattice) that runs entirely in OpenGL 4.6 compute shaders and is visualised with a volumetric raymarcher. Scenes are **Lua scripts**, so you can build your own experiments on wave propagation, antennas and how materials affect fields. Built with C++17, SDL3, GLAD, GLM and Lua.

## Build

Requirements: CMake ≥ 3.20, a C++17 compiler, a GPU/driver with OpenGL 4.6 Core, and the vcpkg packages `sdl3`, `glm` and `lua` (Lua 5.4 or newer).

```sh
vcpkg install sdl3 glm lua
cmake -S . -B build -A x64 -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release
build/Release/EM_Lab.exe              # first bundled scene
build/Release/EM_Lab.exe --script my_scene.lua
```

Shaders and scenes are copied next to the executable after each build. When the source tree is available, the app loads scenes from `<source>/scenes`, so edits to the bundled scripts hot-reload.

**GPU selection.** On hybrid laptops the app runs on the discrete GPU by default and falls back to the integrated GPU when there isn't one. On Windows, the executable exports `NvOptimusEnablement` and `AmdPowerXpressRequestHighPerformance`, which tell the NVIDIA and AMD drivers to use the discrete GPU. On Linux, it sets the PRIME render-offload variables, but only if you haven't set them yourself. At startup the console lists the hardware adapters and the one OpenGL is running on. It warns if a per-app setting in Windows Graphics Settings or the NVIDIA Control Panel has forced a different GPU, because those settings override the driver hints. See [src/GpuSelection.cpp](src/GpuSelection.cpp).

**Command line**

| Option | Purpose |
|---|---|
| `--script FILE.lua` | load a scene script. A relative path is tried in the current folder, then in `scenes/`; `.lua` is optional, so `--script test` finds `scenes/test.lua`. |
| `--scene N` | load the N-th bundled scene |
| `--list` | list the bundled scenes |
| `--set NAME=VALUE` | override a script parameter (repeatable) |
| `--steps N` | simulation steps per frame |
| `--novsync`, `--gldebug`, `--width W`, `--height H` | display and debugging |
| `--frames N` | run N frames, then exit |
| `--keys "..."` | replay key presses at startup |
| `--screenshot out.bmp` | save the last frame of a `--frames` run |

Exit codes (for `--frames` runs): 2 means non-finite fields were detected, 3 means the scene script failed to load. In interactive use, a failing script never exits the app; the error is printed and the previous scene keeps running.

## Scripted scenes

```lua
scene { name = "Dipole and glass sphere", size = {128, 128, 128}, wavelength = 16 }

local d = param { name = "distance", default = 30, min = 15, max = 45, step = 5, keys = {",", "."} }

dipole    { center = grid.center, orientation = "z", amplitude = 7 }
sphere    { center = grid.center + vec(d, 0, 0), radius = 10, material = "glass" }
probe     { position = grid.center + vec(-30, 0, 0), name = "behind" }
flux_box  { center = grid.center, size = 24, name = "radiated" }
far_field {}
```

**Building blocks**

| Category | Primitives |
|---|---|
| Geometry | `box`, `sphere` / ellipsoid, `cylinder` / tube, `wall` with slits and apertures, `grating`, `cage`, `frame`, and procedural `shape{ inside = function(x, y, z) ... end }`. Shapes can be rotated and hollowed. |
| Materials | 30 built-in (metals, PEC, dielectrics, building materials, water, ground) plus custom ones defined by ε_r with σ, conductivity in S/m, or loss tangent |
| Antennas | Real antennas: thin wires (sub-cell thin-wire model with real radius) fed by a 50 Ω resistive voltage source: `dipole`, `monopole`, `loop`, phased `array` (1-D or 2-D, steering, tapers); passive `wire` elements. Every feed is a port reporting input impedance, Γ / S11, VSWR, accepted power and radiation resistance. |
| Sources | Illumination: `plane_wave` (one-way or two-way), `gaussian_beam`. Ideal elements for theory checks: point (Hertzian) dipole, `model = "current"`, generic `current_source`. Waveforms: CW or pulses. |
| Instruments | `probe`, `flux_plane`, `flux_box`, and `far_field` (radiation pattern, directivity in dBi, beamwidth, front/back), each with optional CSV logging |
| Interactivity | `param{}` binds keys to values. The script is re-run when a key is pressed, keeping the fields when only sources changed. Saving the file hot-reloads it. |

The full reference, covering units, every key, the material table, accuracy guidelines and limitations, is in **[docs/SCRIPTING.md](docs/SCRIPTING.md)**.

**Validation scenes** in [scenes/validation/](scenes/validation/) use idealized sources that can't be built but have exact textbook answers, to check the solver: `EM_Lab --script validation/hertzian_dipole`.

**Bundled scenes** (in [scenes/](scenes/); keys 1–9 and 0, F6/F7 to step through). Every antenna is built from metal and fed the way a real one is:

| # | Scene | Parameters | What it shows |
|---|---|---|---|
| 1 | Double slit | slit width `, .`; separation `; '` | Interference fringes; spacing ∝ λ/d |
| 2 | Faraday cage | cage `G`; mesh pitch `, .` | −56 dB interior field; shielding collapses as the openings approach λ/2 |
| 3 | Phased array | steer `, .`; elements `; '`; taper `T` | Half-wave elements with 50 Ω feeds: beam steering, beamwidth ∝ 1/N, sidelobe tapers, active (scan) impedance |
| 4 | Half-wave dipole | length `, .`; wire thickness `T` | Thin-wire dipole, 50 Ω feed: 64 − j21 Ω, VSWR 1.56 at 0.475λ; find resonance; pattern vs length |
| 5 | Ball lens | ε_r `, .` | Focusing; the probe tracks the paraxial focus |
| 6 | Yagi-Uda | directors `, .`; driven length `; '` | 10.9 dBi, 24 dB front/back; low feed impedance (why Yagis need matching) |
| 7 | Pyramidal horn | flare length `, .` | Procedural geometry, thin-wire coax-probe feed; 16.5 dBi |
| 8 | Wire-grid polarizer | polarization `X`; grid `G` | E ∥ wires −24 dB, E ⊥ wires passes |
| 9 | Dipole over ground | height `, .`; ground material `G` | Two-ray interference lobes |
| 10 | Prism | ε_r `, .` | Total internal reflection above ε_r = 2 |
| 11 | Planar array + reflector | azimuth `, .`; elevation `; '`; ground `G` | 2-D steering, 18.7 dBi, 42 dB front/back |
| 12 | Monopole: ground plane vs counterpoise | ground `G` | Plates, horizontal / drooping radials, dipole reference: feed impedance (35 Ω on a 4λ plate), beam tilt |

## Controls

**Scenes**
- **1–9, 0** load a bundled scene; **F6 / F7** step to the previous or next one.
- Drag and drop a `.lua` file onto the window to open it. **F5** reloads the current script (saving also reloads it).

**Camera**
- LMB drag orbits, RMB/MMB drag pans, the wheel zooms.
- **C** switches to fly mode: WASD to move, Q/E down and up, Shift for speed.

**Simulation**
- **Space** pauses and resumes; **N** advances one step while paused; **R** resets the fields.
- **[ / ]** change the number of steps per frame.

**View**
- **M** cycles render modes: E+B, \|E\|, \|B\|, Poynting, and signed E. **V** picks the component for signed mode.
- **− / +** adjust visual gain; **K / L** move the z cutaway.
- **O** toggles the overlay; **H** toggles half-resolution rendering; **F3 / F4** change the ray step.

**Measurement**
- **Tab** selects a probe; the arrows and PgUp/PgDn move it (Shift for ×5).
- **P** prints the full measurement report; **F12** saves a screenshot.

Script parameters use the keys listed under "Parameters" in the console.

**Overlay**

| Colour | Shows |
|---|---|
| Light grey box | grid bounds |
| Faint blue box | absorbing layer |
| Orange | sources |
| Yellow crosses | probes (the selected one is brighter) |
| Green | flux monitors |
| Purple box | far-field box |
| Orange / cyan / green polar curves | xy / xz / yz radiation-pattern cuts, on a 30 dB scale |

## Architecture

```
src/
  Main.cpp            SDL3 lifecycle, input, scene loading / hot reload / params, simulate -> measure -> render loop
  ScriptEngine.*      Sandboxed Lua runtime and scene API (validation, typo hints, units, material library)
  Environment.*       Scene description, composable shapes (transform / difference / lambda / wall), voxeliser
  SimulationGrid.*    Yee textures, edge-averaged materials + sub-cell overrides, thin-wire B coefficients, sources, leapfrog
  Ports.*             Antenna feed ports: gap voltage / current DFT -> impedance, Gamma, VSWR, power
  Instruments.*       Probes, flux monitors, far-field monitor: averaging, CSV logs, reports, overlay
  FarField.*          GPU near-to-far-field transform: running DFT on a Huygens box, pattern and directivity
  FieldAnalyzer.*     Staggering-aware probe readback, GPU Poynting flux through planes / boxes
  VolumeRenderer.*    Full-screen raymarch pass, optional reduced-resolution target, line overlay
  Camera.*, Shader.*, GLUtils.hpp, GpuSelection.*
shaders/
  common.glsl         Units, Yee layout, graded absorber profile, stable exponential coefficients
  update_B.comp       Faraday step   (reads E as sampler, updates B in place)
  update_E.comp       Ampère step + conduction + analytic current injection
  field_sampling.glsl E / B co-located at lattice nodes (shared by flux and NTFF)
  poynting_flux.comp  Shared-memory reduction of (E×B)·n over a rectangle
  ntff_*.comp/.glsl   Far-field: phasor accumulation and radiation integrals
  port_accumulate.comp Antenna ports: running DFT of gap voltage and current
  clear_fields.comp, fullscreen.vert, raymarch.frag, lines.*
scenes/               Bundled Lua scenes (scenes/validation/: idealized analytic checks)
docs/SCRIPTING.md     Scripting reference
docs/UNITS.md         Normalized units and conversion to SI
external/glad         GLAD 0.1.36 loader, OpenGL 4.6 Core (generated)
```

### Physics notes

- **Units.** c = ε₀ = μ₀ = 1 and one cell is one length unit, so dt equals the Courant number S (0.5, below the 3D limit of 1/√3). The B texture stores c·B, so the Poynting vector is simply S = E × B. A script's `cell_size` sets the physical length scale; [docs/UNITS.md](docs/UNITS.md) explains how to convert every input and output to SI units.
- **Memory layout.** `E_Field_Tex` RGBA32F holds (Ex, Ey, Ez, σ) and `B_Field_Tex` RGBA32F holds (Bx, By, Bz, ε_r), with σ and ε_r per cell; the staggered positions are documented in `common.glsl`. A third texture holds the edge-averaged materials, so GPU memory is 48 bytes per cell. Each pass updates its own field in place.
- **Edge-averaged materials.** Each E component sits on a Yee edge shared by four cells. The solver uses those cells' average σ and ε_r, precomputed into a packed half-float texture. This zeros tangential E on every face of a metal object, so staircased PEC is symmetric and thin rods behave like proper wires. It also gives the standard averaged permittivity at dielectric interfaces.
- **Lossy media.** An exponential time integrator, E ← e^(−x)·E + (dt/ε)·φ(x)·(∇×B − J) with x = σ·dt/ε, is unconditionally stable for any conductivity. Unlike Crank–Nicolson, it has no sign-flip ringing in metals. Edge conductivities are stored as half floats and clamped at 6×10⁴ (normalized), which is already a perfect conductor (field decay factor e^(−30000) per step).
- **Absorbing boundary.** Exponentially graded conductivity with a matched magnetic loss, so the layer's impedance equals free space. Normal-incidence reflection is about 10⁻⁶.
- **Sources.** J = p·A·w(x)·s(t). The footprint w(x) is a soft box with per-axis Gaussian edges, covering points, wires, sheets and beams. The signal s(t) is evaluated in double precision on the CPU for each sub-step. One-way plane waves use two sheets λ/4 apart, the second delayed by T/4.
- **Thin wires.** Umashankar–Taflove sub-cell model: the wire's grid edges are perfect conductors, and the four B updates circling each edge use 2/ln(Δ/a) for the 1/r near field of a wire of radius a. Free wire ends are built one cell short to cancel the grid's end-capacitance effect, so the electrical length matches the requested length.
- **Feeds and ports.** A resistive voltage source (EMF behind 50 Ω) as its Norton equivalent: gap conductance plus impressed current. Ports record the gap voltage and the current through the gap at the analysis frequency, giving Z, Γ, VSWR and accepted power. The far-field radiated power provides an independent check (R_rad = Re Z).
- **Far field.** A running single-frequency DFT of E and H on a closed Huygens box (two cascaded averages, about 0.2% ripple), trapezoid-weighted surface integration, and radiation integrals evaluated on the GPU in one workgroup per direction. D = 4πU / P_rad, with P_rad taken from the same surface.

### Validation (measured with this code)

| Test | Result | Reference |
|---|---|---|
| Hertzian dipole directivity (`validation/hertzian_dipole`) | 1.87 dBi at 16 cells/λ, 1.78 dBi at 32 cells/λ | 1.76 dBi (second-order convergence) |
| Hertzian dipole HPBW | 87.5° / 89.4° | 90° |
| Thin-wire dipole impedance, 0.175λ / 0.375λ / 0.475λ (`validation/dipole_impedance`) | 5.9 − j812 / 32.6 − j222 / 62.9 − j37 Ω | 6.3 − j807 / 34.0 − j222 / 63.0 − j11 Ω (induced EMF) |
| Port vs far field (every antenna scene) | R_rad from radiated power = Re Z within 3% | equal for lossless antennas |
| Monopole on a 4λ plate | 34.8 − j6 Ω, 5.7 dBi, beam 28° up | ~36 Ω, 5.15 dBi at the horizon on an infinite plane |
| Dipole radiated power (closed flux box) | 0.359 | 0.360 analytic for the discretised current |
| 8-element λ/2 array beamwidth (thin-wire elements) | 12.7° | 12.7° |
| 4×4 planar array over a reflector | 18.7 dBi | 18.6 dBi (4πA/λ², A = 2λ × 2.9λ) |
| Faraday cage (pitch λ/9) | interior \|E\| −56 dB, power −76 dB | — |
| Wire grid, period λ/6 | E ∥ −24 dB, E ⊥ −0.1 dB | — |
| Right-angle prism | 87% TIR at n = 1.5; 81% transmitted at n = 1.22 | TIR threshold n = √2 |

## Using the C++ API directly

Scripts drive `EnvironmentBuilder` (see [src/Environment.hpp](src/Environment.hpp)), which you can also call from C++:

```cpp
EnvironmentBuilder b("My scene", GridConfig{{160, 128, 128}});
b.box({60, 0, 0}, {62, 128, 128}, Material::metal());
b.fill(TransformedShape(std::make_shared<BoxShape>(glm::vec3(-10), glm::vec3(10)), {100, 64, 64},
                        rotationFromEulerDegrees({0, 0, 30})), Material::dielectric(4.0f));
Environment& env = b.env();
env.sources.push_back(SourceDesc::planeWave(0, 24.0f, {0, 0, 1}, 16.0f, 0.5f));
env.probes.push_back({"detector", {120, 64, 64}, ""});
```
