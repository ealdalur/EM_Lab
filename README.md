# EM_Lab — GPU FDTD Electromagnetic Wave Simulator

A real-time 3D Maxwell solver (FDTD on a Yee lattice) that runs entirely in OpenGL 4.6 compute shaders and is visualised with a volumetric raymarcher. Scenes are **Lua scripts**, so you can build your own experiments on wave propagation, antennas and how materials affect fields. Built with C++17, SDL3, GLAD, GLM, Lua, and Dear ImGui / ImPlot for the on-screen instrument panels.

## Build

Requirements: CMake ≥ 3.20, a C++17 compiler, a GPU/driver with OpenGL 4.6 Core, and the vcpkg packages `sdl3`, `glm`, `lua` (5.4 or newer), `imgui` (with the SDL3 and OpenGL 3 bindings) and `implot`.

```sh
vcpkg install sdl3 glm lua "imgui[sdl3-binding,opengl3-binding]" implot
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
| Geometry | `box`, `sphere` / ellipsoid, `cylinder` / tube, `wall` with slits and apertures, `grating`, `cage`, `frame`, procedural `shape{ inside = function(x, y, z) ... end }`, and zero-thickness metal `sheet`s (foils, patches, board ground planes). Shapes can be rotated and hollowed. |
| Materials | 30 built-in (metals, PEC, dielectrics, building materials, water, ground) plus custom ones defined by ε_r with σ, conductivity in S/m, or loss tangent |
| Antennas | Real antennas: thin wires (sub-cell thin-wire model with real radius) fed by a 50 Ω resistive voltage source: `dipole`, `monopole`, `loop`, phased `array` (1-D or 2-D, steering, tapers); passive `wire` elements. Every feed is a port reporting input impedance, Γ / S11, VSWR, accepted power and radiation resistance; a pulsed `sweep` gives Z, S11 and VSWR across a band in one run, with an on-screen S11 plot and the −10 dB bandwidth. `emitter{}` point transmitters (linear or circular), `crpa{}` adaptive GPS arrays (MVDR nulling), and `coax{}` cables with a standing-wave monitor and an optional sleeve balun. |
| Sources | Illumination: `plane_wave` (one-way or two-way), `gaussian_beam`. Ideal elements for theory checks: point (Hertzian) dipole, `model = "current"`, generic `current_source`. Waveforms: CW or pulses. |
| Instruments | `probe`, `flux_plane`, `flux_box`, `far_field` (radiation pattern, directivity in dBi, beamwidth, front/back) and `sweep` (port impedance vs frequency), each with optional CSV logging |
| Interactivity | `param{}` binds keys to values. The script is re-run when a key is pressed, keeping the fields when only sources changed. Saving the file hot-reloads it. |

The full reference, covering units, every key, the material table, accuracy guidelines and limitations, is in **[docs/SCRIPTING.md](docs/SCRIPTING.md)**.

**Validation scenes** in [scenes/validation/](scenes/validation/) use idealized sources that can't be built but have exact textbook answers, to check the solver: `EM_Lab --script validation/hertzian_dipole`.

**Bundled scenes** (in [scenes/](scenes/); keys 1–9 and 0 load scenes 1–10, F6/F7 step through all of them, `--scene N` opens any). Every antenna is built from metal and fed the way a real one is:

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
| 13 | Antenna matching: length vs VSWR | length `, .`; dipole / monopole `G`; 50 / 75 Ω line `I` | Detune a resonant antenna and watch VSWR rise and radiated power fall (97% → 25% accepted two steps short) |
| 14 | Microstrip patch antenna | sweep / CW `T`; feed inset `, .`; length trim `; '`; substrate `X` | Probe-fed patch on RT5880 / FR4 / foam: S11 sweep and bandwidth, inset matching (R falls from 186 to 39 Ω as the probe moves in), where the transmission-line design model is off by 8%, 7.8 dBi broadside beam, FR4 loss (80% efficiency) |
| 15 | 4×4 / 8×8 patch phased array (X band) | beam azimuth `, .`; elevation `; '`; taper `G`; spacing `I`; size `U` | 64 probe-fed patches with their own feeds: 23.4 dBi, 12.4° beam; electronic steering in any direction; active (scan) impedance of every element; sidelobe tapers; grating lobes at wide spacing |
| 16 | Phased-array radar | target `G`; range `, .`; azimuth `; '`; elevation `/ \`; scene depth `U` (5–30 λ); transmitter `T`; noise `X`; separation `I`; taper `Y` | The array as a pulsed radar: clutter calibration, pulse compression, digital beamforming; detects a sphere, plate, tilted plate (stealth shaping), two spheres (angular resolution) or a missile-like cylinder to within ~1 mm in range and 1° in angle |
| 17 | CRPA: adaptive GPS antenna | jammers `J`; positions `, .`; satellite azimuth `; '` and elevation `/ \`; JNR `X`; weights `T`; steering vector `Y`; adaptation `B` | 2×2 RHCP dual-probe patch array at GPS L1 with a satellite and 1–3 random jammers. Blind MVDR from I/Q snapshots of everything at once (as a real receiver), or ideal from separately measured responses: SINR +43 dB, adapted pattern shown by reciprocity |
| 18 | Coax-fed dipole and balun | termination `T`; dipole length `, .`; balun `B` and its length `/ \`; cable impedance `I`; cable length `; '`; CW / pulse `U` | A generator, 2λ of real coax and a dipole: traveling vs standing waves along the cable (VSWR, Γ, Z at the antenna), and the current on the outside of the shield without a balun (−9 dB, beam tilted 32°) vs with a quarter-wave sleeve balun (−27 dB, beam level) |

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
- **O** toggles the overlay; **F3 / F4** change the ray step. The field volume is rendered below the window resolution on large windows (half above ~1.5 megapixels, three-quarters above 0.8), which is invisible because the glow is smooth, while lines and panels stay sharp; **H** switches between full and half resolution.

**Measurement**
- **Tab** selects a probe; the arrows and PgUp/PgDn move it (Shift for ×5).
- **P** prints the full measurement report to the console; **F12** saves a screenshot.

**On-screen panels** (**F1** help, **F2** shows or hides all panels; panels can be moved, resized and collapsed, and the layout is remembered):

| Panel | Shows |
|---|---|
| Scene | Status, Pause / Step / Reset / Reload, steps per frame, every script parameter as a slider, menu or checkbox (with its key), view settings, script errors and warnings |
| Instruments | Probes, power flow, antenna ports (Z, \|Γ\|, S11, VSWR, accepted power), sweep summary and far-field numbers in tables that update in place (refresh interval selectable); hover a column header for an explanation |
| Frequency sweep | S11, VSWR and impedance against frequency, with axes, the −10 dB line, the design frequency and the best match annotated |
| Radiation pattern | Beam maps of both hemispheres in direction cosines (u, v), and the three principal-plane cuts in dBi |
| Array elements | For phased arrays: every element's active VSWR, S11, accepted power or impedance laid out as the array is built |
| Radar | Range-azimuth and azimuth-elevation images, the range profile, the raw element signal and the detection table |

Script parameters can also be changed with the keys shown next to them in the Scene panel. The periodic console report is only printed in unattended `--frames` runs.

**Overlay**

| Colour | Shows |
|---|---|
| Light grey box | grid bounds |
| Faint blue box | absorbing layer |
| Orange | sources |
| Yellow crosses | probes (the selected one is brighter) |
| Green | flux monitors |
| Purple box | far-field box |
| Hatched copper rectangles | zero-thickness metal sheets |
| Orange boxes, red crosses | radar targets, radar detections |
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
  InstrumentsUi.cpp   On-screen instrument panels and plots (Dear ImGui / ImPlot)
  Radar.*             Pulsed radar: clutter calibration, pulse compression, back-projection imaging, detection
  Crpa.*              Adaptive GPS array: per-emitter steering vectors, RHCP hybrid, MVDR weights, adapted pattern
  Coax.*              Coaxial cable monitor: line integrals of E and H along the cable, two-wave fit (Gamma, VSWR, Z)
  Ui.*                Dear ImGui / ImPlot setup, DPI scaling, fonts, event routing
  FarField.*          GPU near-to-far-field transform: running DFT on a Huygens box, pattern and directivity
  FieldAnalyzer.*     Staggering-aware probe readback, GPU Poynting flux through planes / boxes
  VolumeRenderer.*    Full-screen raymarch pass, optional reduced-resolution target, line overlay
  Camera.*, Shader.*, GLUtils.hpp, GpuSelection.*
shaders/
  common.glsl         Units, Yee layout, graded absorber profile, stable exponential coefficients
  update_B.comp       Faraday step   (reads E as sampler, updates B in place)
  update_E.comp       Ampère step + conduction
  apply_sources.comp  Source currents, only over the cells sources reach (per-cell source lists)
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
- **Frequency sweeps.** Pulsed excitation; the GPU records each port's V and I every step, and a double-precision DFT on the CPU gives Z(f) = V(f)/I(f) at up to 2001 frequencies, with the half-step offset between V and I compensated.
- **Sheets.** A zero-thickness conductor is a set of perfectly conducting E edges in one lattice plane (sub-cell overrides applied after material averaging), so a foil lies exactly on a dielectric's surface.
- **Source pass.** Source currents enter the E update additively (E ← C_a E + C_b (∇×B − J)), so they are applied by a separate pass over only the cells a source reaches (per-cell source lists built when the sources change). The cost no longer grows with the number of sources times the grid size: a 64-element array runs 2.7× faster.
- **Radar.** A calibration run without the targets is subtracted from the run with them (clutter cancellation). Pulse compression uses a measured two-way template T = REF² / S: the boresight reference field of the calibration run, with the transmit array's near-field factor divided out, squared and divided by the drive spectrum. Back-projection then sums the 64 compressed echoes coherently at each image point's exact round-trip delay, which focuses in the near field too. The listening window is set from the antennas' measured ring-down. The image is processed progressively on a background thread, and the part of it whose echoes have fully arrived is marked final.
- **Far field.** A running single-frequency DFT of E and H on a closed Huygens box (two cascaded averages, about 0.2% ripple), trapezoid-weighted surface integration, and radiation integrals evaluated on the GPU: each workgroup handles 128 directions and streams the box surface through shared memory, and interactive updates are spread over a few frames (no stall). D = 4πU / P_rad, with P_rad taken from the same surface.

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
| Patch antenna resonance (RT5880, h = 0.05λ) | 2.215 GHz; 2.200 GHz at twice the resolution | 2.43 GHz transmission-line model (known to read high on thick boards) |
| Frequency sweep vs CW (same patch, 2.4 GHz) | 74.13 + j8.59 Ω | 74.1 + j8.6 Ω (CW port) |
| 8×8 patch array at broadside (0.5λ spacing) | 23.4 dBi, HPBW 12.3° | 23.0 dBi (4πA/λ²), 12.7° |
| Same array steered to θ = 30°, φ = 45° | peak at θ 30°, φ 45°, 22.7 dBi | scan loss 10 log cos 30° = −0.6 dB |
| Radar range accuracy (sphere / plate / cylinder, 2×2 transmitter) | −0.3 / +1.3 / 0.0 mm | true nearest surface; λ = 30 mm |
| Radar at long range (plate, 20 λ deep scene) | −2.2 mm at 15 λ, −4.8 mm at 10 λ; multipath ghost (array ↔ plate) at twice the range, −24 dB | true front surface |
| Coax line (`coax{}`, 50 Ω) | Z0 from the waves 50.0 Ω; short Γ = 1.000 ∠ 180°; 100 Ω load Γ = 0.338; matched load return loss 25–31 dB | Γ = 0.333 for 100 Ω |
| CRPA nulls (2×2 RHCP array, 2 jammers at 40 dB JNR) | blind (160 snapshots): jammers 21–24 dB below noise; ideal: 40–53 dB below, real transmitted nulls 47–59 dB vs steering; both lift SINR 43 dB | N − 1 = 3 nullable jammers |
| Radar angular resolution (two spheres, Hamming receive taper) | resolved at 30°, merged at 20° | beamwidth ≈ 19° |
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
