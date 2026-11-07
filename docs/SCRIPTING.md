# EM_Lab Scene Scripting Reference

EM_Lab scenes are written in **Lua** (5.4 or newer). A script *declares* a scene by calling builder functions such as `scene{}`, `box{}`, `dipole{}` and `far_field{}`. Because scripts are real programs, they can also use loops, math and functions to generate geometry procedurally.

```lua
scene { name = "Hello dipole", size = {128, 128, 128}, wavelength = 16 }

dipole    { center = grid.center, orientation = "z", amplitude = 7 }
sphere    { center = grid.center + vec(30, 0, 0), radius = 10, material = "glass" }
probe     { position = grid.center + vec(-30, 0, 0) }
far_field {}
```

Contents:
1. [Running scripts](#1-running-scripts)
2. [Units and coordinates](#2-units-and-coordinates)
3. [Built-in helpers](#3-built-in-helpers)
4. [Scene setup](#4-scene-setup): `scene`, `view`, `material`, `param`
5. [Geometry](#5-geometry): `box`, `sphere`, `cylinder`, `wall`, `grating`, `cage`, `frame`, `shape`
6. [Material library](#6-material-library)
7. [Sources](#7-sources): `plane_wave`, `gaussian_beam`, `dipole`, `loop`, `array`, `current_source`
8. [Instruments](#8-instruments): `probe`, `flux_plane`, `flux_box`, `far_field`
9. [Accuracy guidelines](#9-accuracy-guidelines)
10. [Limitations](#10-limitations)

---

## 1. Running scripts

| How | What happens |
|---|---|
| Keys **1–9, 0** | Load bundled scenes 1–10 from `scenes/` (sorted by file name). **F6 / F7** step through all of them. |
| Drag and drop a `.lua` file onto the window | Load that script. |
| `EM_Lab --script my_scene.lua` | Load a script from the command line. A relative path is tried in the current folder, then in `scenes/`, and `.lua` may be omitted. `--list` prints the bundled scenes. |
| Save the file | **Hot reload.** The script is re-run automatically about 0.5 s after you save it. **F5** reloads manually. |
| `--set name=value` | Override a `param{}` at startup. Repeat the option for several parameters. |

**Reloading.** If a reload changes only sources, instruments or view settings, the running fields are kept. That's how live beam steering works. If the grid size or any material changes, the fields are reset. Your view (camera, gain, render mode) is kept across reloads too, except for `view{}` settings whose scripted value changed: a script can tie a setting to a parameter, e.g. `gain = pulse and 60 or 30`, and switching the parameter applies the new value.

**Parameters on screen.** Every `param{}` also appears in the **Scene** panel as a slider, menu or checkbox with its key next to it. A slider re-runs the script when you release it.

**Errors.** A failed script reports the file, line and reason in the console and in red in the Scene panel, for example `my_scene.lua:12: sphere: 'radius' must be > 0`. The previous scene keeps running. Unknown or misspelled keys trigger warnings with suggestions, such as `unknown field 'radious' ignored (did you mean 'radius'?)`.

**Sandbox.** Scripts run without the `io` and `os` libraries and cannot load C modules. A script that runs longer than 30 s is stopped. `require "name"` loads `name.lua` from the script's folder or its `lib/` subfolder, so you can keep shared component libraries there. `print()` writes to the console.

---

## 2. Units and coordinates

- **Length: cells.** All positions and sizes are in lattice cells. The origin is at the grid corner, so cell `(i, j, k)` spans `[i, i+1)` on each axis. **+z is up**, and the camera orbits about z.
- **Wavelength: cells per wavelength.** `scene{ wavelength = 16 }` sets the reference wavelength. Sources, loss tangents and `wavelengths()` use it by default, and it determines accuracy (see §9).
- **Physical scale.** `scene{ cell_size = 1e-3 }` (in metres, default 1 mm) maps cells to real units. Wherever a `wavelength` can be given, you can give `frequency` (Hz) instead, and materials can take a `conductivity` in S/m. For example, with 1 mm cells, `frequency = 10e9` means λ = 30 cells.
- **Time.** The speed of light is c = 1 cell per time unit, and one period equals λ time units. Time-related source keys (`ramp`, `pulse_width`, `delay`, `interval`) are given **in periods**.
- **Fields.** Normalised units with ε₀ = μ₀ = c = 1. The B channel stores c·B, so the Poynting vector is S = E × B and the wave impedance is 1. To convert any input or output to SI units (V/m, A, W, seconds…), see **[UNITS.md](UNITS.md)**.
- **Absorbing boundary.** The outer `pml` cells (default 16) absorb outgoing waves. Fields there are not physical, so keep objects and instruments out of that layer. Walls and ground planes may extend into it.

The `grid` table, which is read-only, describes the active grid:

| Field | Meaning |
|---|---|
| `grid.size`, `grid.center` | vectors |
| `grid.nx`, `grid.ny`, `grid.nz` | integers |
| `grid.pml` | absorbing-layer thickness |
| `grid.interior_min`, `grid.interior_max` | the region inside the absorbing layer |
| `grid.wavelength` | reference wavelength (cells) |
| `grid.cell_size` | metres per cell |
| `grid.dt` | time step |

Reading `grid` freezes the scene settings, so call `scene{}` first.

---

## 3. Built-in helpers

| Helper | Description |
|---|---|
| `vec(x, y, z)`, `vec(s)`, `vec(t)` | 3-vector. Supports `+ - * /` (component-wise, with numbers or vectors), unary `-` and `tostring`. Methods: `v:length()`, `v:normalize()`, `v:dot(w)`, `v:cross(w)`, `v:with("x", value)`. Fields `v.x v.y v.z`. Wherever a vector is expected you can also pass `{x, y, z}`, `{x=..,y=..,z=..}`, or a plain number (which sets all three components). |
| `X`, `Y`, `Z` | unit vectors |
| `wavelengths(n)` | n reference wavelengths, in cells |
| `meters(x)`, `mm(x)` | physical length to cells (uses `cell_size`) |
| `deg(x)` | degrees to radians, for your own math. Angles passed to builder functions are already in degrees. |
| `lerp(a, b, t)`, `clamp(x, lo, hi)` | interpolation and clamping |
| `range(a, b, step)` | inclusive numeric iterator: `for x in range(0, 1, 0.25) do ... end` |
| `pi`, `c0` | π and the speed of light (m/s) |
| `warning(text)` | Adds a warning to the scene's list (shown in the Scene panel and the console), e.g. when the script had to adjust a parameter. |

The standard `math`, `string`, `table` and `utf8` libraries are available.

---

## 4. Scene setup

### `scene{}`
This must be the **first** call in the script.

| Key | Default | Description |
|---|---|---|
| `name`, `description` | | Shown in the title bar and console. |
| `size` | `{128,128,128}` | Grid cells per axis. GPU memory is 48 bytes per cell. |
| `wavelength` / `frequency` | 16 cells | Reference wavelength (see §2). |
| `cell_size` | `1e-3` | Metres per cell. |
| `courant` | `0.5` | c·dt/dx. Must be below 0.577. |
| `pml` | `16` | Absorbing-layer thickness, or `{thickness=16, sigma_max=1.6, grading=3}`. |
| `output_dir` | `output/<script name>` | Folder for CSV logs, relative to the script. |

### `view{}`
Initial camera and rendering settings. Can be called anywhere in the script.

| Key | Description |
|---|---|
| `mode` | `"fields"` (E cyan + B magenta), `"e"`, `"b"`, `"poynting"`, or `"signed"` (one E component, crests vs. troughs) |
| `component` | axis for `"signed"` mode (defaults to the first source's polarization) |
| `gain`, `gain_e`, `gain_b` | visual field gain (adjust live with − / +) |
| `exposure`, `density`, `step` | tone mapping, gas opacity, ray step in voxels |
| `distance`, `yaw`, `pitch`, `target` | orbit camera. Angles in degrees; `target` in lattice coordinates (default: grid centre). |
| `steps_per_frame`, `overlay` | simulation speed, overlay on/off |

### `material{}`
Defines a named material and returns its name.

```lua
material { name = "foam",  epsilon_r = 1.05 }
material { name = "lossy", epsilon_r = 4, loss_tangent = 0.05 }
material { name = "ferrite_like_absorber", epsilon_r = 12, conductivity = 0.5 }   -- S/m
```

`epsilon_r` must be ≥ 1. Give losses as at most one of:
- `sigma`: normalised conductivity, where 100 or more behaves like a metal;
- `conductivity`: S/m, converted using `cell_size`;
- `loss_tangent`: tan δ at the reference frequency.

Anywhere a material is expected, you can also write it inline: `material = { epsilon_r = 2.2 }`.

### `param{}`
Declares an interactive parameter and **returns its current value**. The script is re-run when you press the bound key.

```lua
local steer  = param { name = "steer", default = 0, min = -60, max = 60, step = 10, keys = {",", "."} }
local on     = param { name = "cage",  default = true, key = "g" }                -- boolean toggle
local taper  = param { name = "taper", choices = {"uniform", "hann"}, key = "t" } -- cycles through choices
```

| Key | Description |
|---|---|
| `name` (required), `label` | identifier (for `--set`) and display text |
| `default` | a number, `true`/`false`, or one of `choices` |
| `min`, `max`, `step`, `wrap` | numeric range. `wrap = true` wraps around at the ends. |
| `keys = {dec, inc}` / `key` | key binding for numbers / for booleans and choices |

Parameters may use these keys: `B F G I J T U X Y Z , . ; ' / \ `` ` ``. All other keys belong to the application. Current values appear in the Scene panel (where they can also be changed) and in the title bar.

---

## 5. Geometry

Shapes are voxelised in the order they are called, and **later shapes overwrite earlier ones**. To cut a hole, fill a region with `material = "vacuum"`. Each cell holds one material, sampled at the cell centre.

Common keys:
- `material`: a name or inline table. Default `"metal"` (copper).
- `rotate = {rx, ry, rz}`: Euler angles in degrees, applied about x, then y, then z, around the shape's centre. Supported by `box`, `sphere` and `cylinder`.
- `thickness`: makes the shape hollow, leaving a shell of that thickness. Supported by `box`, `sphere` and `cylinder`.
- `name`: optional label used in messages.
- `target = true`: marks the object as something a `radar{}` should detect (`box`, `sphere`, `cylinder` and `shape`). See §8.

| Primitive | Keys | Notes |
|---|---|---|
| `box{}` | `min`+`max` **or** `center`+`size` | Plates, blocks, rods, enclosures (with `thickness`). |
| `sphere{}` | `center`, `radius` | A vector `radius` gives an ellipsoid. With `thickness`: a spherical shell, such as a radome. |
| `cylinder{}` | `center`, `axis="z"`, `radius`, `length`, `thickness` or `inner_radius`, `capped=false` | Hollow cylinders are open tubes (circular waveguides) unless `capped = true`. |
| `wall{}` | `axis="x"`, `position`, `thickness=2`, `apertures`, `slits` | An infinite slab across the whole cross-section, extending into the absorbing layer so nothing leaks round it. |
| `grating{}` | `axis`, `position`, `thickness=1`, `period=4`, `width=1`, `direction` (bar axis), `min`/`max` (tangential extent) | Parallel bars, e.g. a wire-grid polarizer or diffraction grating. |
| `cage{}` | `min`/`max` or `center`/`size`, `spacing=4`, `bar=2` | Hollow box whose six faces are a square mesh of bars (Faraday cage). |
| `frame{}` | `min`/`max` or `center`/`size`, `bar=2` | The 12 edges of a box. |
| `shape{}` | `min`/`max` or `center`/`size` (bounds), `inside = function(x,y,z) ... end` | **Arbitrary geometry.** `inside` receives the cell-centre coordinates and returns `true` for cells to fill. |
| `sheet{}` | `min`+`max` **or** `center`+`size`, equal along one axis (the normal) | **Zero-thickness perfect conductor** in a lattice plane: copper foil, a patch, a microstrip, a ground layer on a board. See below. |

**Sheets.** `sheet{}` doesn't fill cells. It makes every E-field edge lying in the rectangle a perfect conductor, so a foil can sit exactly on the surface of a dielectric `box{}` (a patch on its substrate) without taking up a cell of the board. Corners are lattice nodes, so give integer coordinates; the two in-plane extents must be non-zero (`sheet{ min = vec(10, 10, 40), max = vec(50, 30, 40) }` is normal to z). Sheets are applied after the voxels, so a later `box{ material = "vacuum" }` does not cut a hole in them. A thin-wire end that lands on a sheet is joined to it, which is how a coax probe connects a ground plane to a patch. The overlay draws sheets as hatched copper rectangles.

Wall apertures use the wall's **tangential coordinates `{u, v}`**: the two remaining axes in x, y, z order. For example, for `axis = "x"`, `u = y` and `v = z`.

```lua
wall { axis = "x", position = 60, material = "pec",
       apertures = { { center = {64, 48}, size = {6, 40} },      -- rectangle (w along u, h along v)
                     { center = {90, 48}, radius = 5 } } }        -- circle
wall { axis = "x", position = 60,
       slits = { count = 2, width = 6, spacing = 36, length = 48, direction = "z" } }
```
`slits.direction` is the long axis of the slits; they repeat along the other tangential axis, centred on `center` (default: the middle of the wall).

Procedural example, a pyramidal horn wall (see `scenes/07_horn_antenna.lua`):

```lua
shape { min = vec(30, 30, 30), max = vec(90, 80, 70), material = "pec",
        inside = function(x, y, z)
          local s  = (x - 30) / 60                 -- 0 at the throat, 1 at the aperture
          local hy, hz = 7 + 13 * s, 4 + 11 * s
          local dy, dz = math.abs(y - 55), math.abs(z - 50)
          return dy <= hy + 1 and dz <= hz + 1 and (dy >= hy or dz >= hz)
        end }
```

---

## 6. Material library

Conductivities in S/m are converted with `cell_size`, and loss tangents with the reference frequency. You can redefine any of these with `material{}`.

| Name | ε_r | Loss | Name | ε_r | Loss |
|---|---|---|---|---|---|
| `vacuum`, `air` | 1 | – | `ptfe` / `teflon` | 2.1 | tan δ 2e-4 |
| `pec` | 1 | ideal conductor | `polyethylene` | 2.25 | tan δ 3e-4 |
| `metal` (= `copper`) | 1 | 5.8e7 S/m | `rexolite` | 2.53 | tan δ 5e-4 |
| `silver` | 1 | 6.3e7 S/m | `polystyrene` | 2.6 | tan δ 5e-4 |
| `gold` | 1 | 4.1e7 S/m | `acrylic` | 2.6 | tan δ 8e-3 |
| `aluminum` | 1 | 3.77e7 S/m | `quartz` | 3.8 | tan δ 1e-4 |
| `brass` | 1 | 1.5e7 S/m | `fr4` | 4.4 | tan δ 0.02 |
| `steel` (stainless) | 1 | 1.45e6 S/m | `glass` (window) | 6.3 | 0.004 S/m |
| `absorber` | 1 | σ = 0.3 (normalised) | `alumina` | 9.8 | tan δ 1e-4 |
| `concrete` | 5.24 | 0.05 S/m | `silicon` | 11.7 | – |
| `brick` | 3.91 | 0.024 S/m | `water` | 78 | tan δ 0.1 |
| `wood` | 1.99 | 0.005 S/m | `seawater` | 74 | 4 S/m |
| `drywall` | 2.73 | 0.0085 S/m | `ground_dry` / `ground_wet` | 4 / 25 | 0.001 / 0.02 S/m |

Building-material values follow ITU-R P.2040 near 1 GHz. Real materials are dispersive, so treat these as single-frequency approximations.

---

## 7. Sources

All sources share these timing keys:

| Key | Default | Description |
|---|---|---|
| `wavelength` / `frequency` | scene reference | Wavelength in cells, or frequency in Hz. |
| `amplitude` | 1 | **E-field amplitude** for `plane_wave` and `gaussian_beam`. For antennas (`dipole`, `monopole`, `loop`, `array` elements): the **source EMF** with the default voltage feed, or the **feed current** with `feed = "current"`. Raw current density J₀ for `current_source`. |
| `phase` | 0 | Degrees. |
| `waveform` | `"cw"` | `"cw"` (continuous sine) or `"pulse"` (Gaussian-modulated sine, a wave packet). |
| `ramp` | 3 | CW turn-on time in periods. The smooth ramp keeps the start-up transient small. |
| `pulse_width` | 1 | Pulse 1/e half-width in periods. |
| `interval` | 0 | Pulse repetition interval in periods (0 = a single pulse). |
| `delay` | 0 | Start delay in periods. |
| `enabled` | true | |

| Primitive | Keys | Notes |
|---|---|---|
| `plane_wave{}` | `axis="x"`, `position`, `polarization`, `direction="+"` | A uniform current sheet across the domain. With `direction = "+"` or `"-"`, two sheets λ/4 apart (the second delayed by T/4) radiate one way only. For CW this is exact; for pulses there's a small backward leak. `"both"` uses a single sheet radiating both ways. `polarization` must be perpendicular to `axis` (default z, or x for sheets normal to z). |
| `gaussian_beam{}` | as `plane_wave`, plus `center` (transverse position) and `waist` (1/e field radius, default 2λ) | Finite-width beam with its waist at the source plane. |
| `dipole{}` | `center`, `orientation="z"`, `length=0`, plus the antenna keys | With `length > 0`: a **real centre-fed dipole**, a thin wire with a feed gap at its centre (see below). With `length = 0`: an ideal Hertzian point dipole (an "oscillating electron"; any orientation). |
| `monopole{}` | `base`, `orientation="+z"`, `length=λ/4`, plus the antenna keys | A **real monopole**: a conductor standing on a ground, fed across a one-cell gap at its base. `base` is the lattice point on the conducting surface (plate, radials, waveguide wall); build that ground yourself. A warning appears if nothing conducts at the base. |
| `loop{}` | `center`, `normal="z"`, `size=4`, plus the antenna keys | A square wire loop (side in cells) fed across a gap in one side. A loop much smaller than λ is a magnetic dipole; one about a wavelength around radiates along its axis. |
| `wire{}` | `from`, `to`, `radius=λ/400` | A passive thin conductor (reflector, director, radial) between two lattice points along x, y or z. |
| `array{}` | `center`, `elements` (`8` or `{4,4}`), `axis` (`"y"` or `{"y","z"}`), `spacing` (default λ/2, or `{dy, dz}`), `orientation="z"`, `length` (default 0.45λ), `steer` or `phase_step`, `taper`, plus the antenna keys | Phased array of centre-fed dipoles, each with its own feed (port). `steer` (degrees from broadside toward +axis) sets phase step Δφ = −k·d·sin θ; `phase_step` sets it directly in degrees. Use `{a, b}` per axis for 2-D arrays. `taper`: `"uniform"`, `"binomial"`, `"hann"`, `"cosine"`, `"triangular"`, or a list of weights (1-D). |
| `current_source{}` | `min`/`max` or `center`/`size`, `polarization`, `softness={sx,sy,sz}` | Generic impressed current density with a soft-edged box footprint. The building block behind all the others. |

**Antenna keys** (`dipole` with a length, `monopole`, `loop`, `array`): `model` (`"thin"` default, `"rod"`, `"current"`), `radius` (thin wires, cells, default λ/400), `feed` (`"voltage"` default, `"current"`), `impedance` (Ω, default 50), `material` (rods). See below.

Illumination sources are *soft*: reflected waves pass through them. Ideal (`model = "current"`) wire elements have a Gaussian cross-section about 0.75 cells in radius.

### Real antennas: thin wires, feeds and ports

`dipole`, `monopole`, `loop` and `array` build antennas the way they are built in practice: **metal conductors driven by a feed across a small gap**. The feed is at the centre of a dipole, the base of a monopole, or one side of a loop. The solver works out how current distributes over the metal. On a half-wave dipole that's roughly sinusoidal and zero at the tips, and it sets both the pattern and the feed impedance. Nearby metal (reflectors, directors, ground planes, radials) picks up induced currents the same way.

**Conductor models** (key `model`):

| Model | What it is | Use for |
|---|---|---|
| `"thin"` (default) | A wire of real radius `radius` (cells; default λ/400) running along a line of grid edges. It uses the Umashankar–Taflove **sub-cell thin-wire model**: the wire's edges are perfect conductors, and the four magnetic-field updates circling each edge use the factor 2/ln(Δ/a), which represents the 1/r fields of a wire much thinner than a cell. Radius must be ≤ 0.4 cells. | Real antenna wires, which are almost always thinner than a cell |
| `"rod"` | A solid 1 × 1-cell metal conductor (voxels), electrically a fat wire (~0.3-cell equivalent radius). Accepts `material`. | Thick conductors, or comparison with thin wires |
| `"current"` | Ideal impressed current, no metal. | Theory checks: textbook array factors, Hertzian dipole |

**Feeds** (key `feed`):

| Feed | What it is | `amplitude` means |
|---|---|---|
| `"voltage"` (default) | A **resistive voltage source**: EMF V_s behind an internal resistance R_s (key `impedance`, default 50 Ω), like a transmitter behind a matched line. It's implemented as its Norton equivalent: the gap edge(s) get conductance 1/R_s plus an impressed current −V_s/R_s, so the open-circuit gap voltage is V_s. | peak EMF V_s (normalized; ×E₀Δ → volts) |
| `"current"` | Ideal current source: the gap current is forced regardless of the load. | peak feed current |

**Every feed is a measurement port.** At the analysis frequency the console reports:

```
port  dipole  Z =  64.3 -21.1j ohm | Gamma 0.220 @ -45 deg (S11 -13.2 dB), VSWR 1.56 vs 50 ohm | P_acc 8.937e-01 of 9.418e-01 avail (95%)
radiation resistance R_rad = 2 P_rad / |I|^2 = 64.0 ohm, radiation efficiency P_rad / P_acc = 100%
```

| Quantity | Meaning |
|---|---|
| Z = R + jX | Input impedance at the feed, from the complex amplitudes of the gap voltage and the current through the gap (the loop integral of H around it). R is the part that absorbs power (radiation + losses); X is the reactance (energy stored near the antenna). |
| Γ, S11 | Reflection coefficient against the feed impedance R_s: Γ = (Z − R_s)/(Z + R_s); S11 = 20 log₁₀\|Γ\| dB. |
| VSWR | (1 + \|Γ\|)/(1 − \|Γ\|): 1 is a perfect match; 2 means about 11% of the power is reflected. |
| P_acc / avail | Power the antenna accepts, ½Re(V·I*), versus the source's available power V_s²/(8R_s). Their ratio is 1 − \|Γ\|². |
| R_rad, efficiency | Radiation resistance 2P_rad/\|I\|² from the far-field monitor's radiated power (single-port scenes), and P_rad/P_acc. With perfect conductors, R_rad = Re Z and the efficiency is ~100%. This is an independent check on the port. |

In arrays every element is its own port, and the reported impedances are **active impedances**: they include mutual coupling and change as the beam is steered.

```lua
dipole   { center = grid.center, orientation = "z", length = 19, radius = 0.1 }        -- 50-ohm voltage feed
box      { min = vec(24, 24, 63), max = vec(105, 105, 64), material = "metal" }        -- ground plate, top face z = 64
monopole { base = vec(64, 64, 64), orientation = "+z", length = 5 }                    -- quarter-wave on the plate
wire     { from = vec(70, 64, 60), to = vec(70, 64, 70), radius = 0.1 }                -- passive element (parasitic)
```

`wire{}` builds a passive thin conductor between two lattice points that differ along one axis. Use it for reflectors, directors and radials. A wire end that touches another wire or a metal cell is electrically joined to it.

**Measured accuracy** (thin wires, induced-EMF theory for comparison):

| Antenna | Measured | Theory |
|---|---|---|
| Dipole 0.175λ, 40 cells/λ | 5.9 − j812 Ω | 6.3 − j807 Ω |
| Dipole 0.375λ | 32.6 − j222 Ω | 34.0 − j222 Ω |
| Dipole 0.475λ | 62.9 − j37 Ω, 2.11 dBi | 63.0 − j11 Ω, 2.12 dBi |
| Monopole on a 4λ plate (20 cells/λ) | 34.8 − j6 Ω, 5.7 dBi, beam 28° up | ~36 Ω on an *infinite* plane (5.15 dBi at the horizon) |
| Yagi, 4 directors | 10.9 dBi, F/B 24 dB, Z = 35 − j70 Ω | typical: ~11 dBi, low feed impedance |

Radiation resistance agrees within a few percent. Reactance agrees closely for short antennas and is somewhat lower near resonance, which puts resonance about 1% of λ (at 40 cells/λ) to 3% (at 20 cells/λ) longer than the induced-EMF formula.

Things to know:
- **End correction.** The lattice node at the free end of a thin wire holds more charge than a real wire's tip, which made wires act about one cell longer per free end. Each free end is therefore built one cell short, automatically, so the *electrical* length matches the length you ask for. Ends that touch another conductor (another wire, a metal plate, a radial hub, the corners of a loop) aren't free and aren't shortened. The overlay draws wires at their requested length.
- **Lengths in whole cells.** A dipole is two arms plus the one-cell gap, so its length is odd (`length = 8` becomes 7 or 9, with a warning). A monopole is the gap plus the conductor; a thin one needs 3 cells, or only 2 when its top joins a conductor (a probe feeding a patch through its board), because a joined end gets no end correction. Wires follow grid lines; feed positions snap to the nearest lattice line.
- **Thin wires are invisible in the volume render** (they're thinner than a cell). The overlay draws them as bright lines.
- **Straight wires only.** Thin wires run along x, y or z. Slanted conductors (such as drooping radials) can be staircased from metal cells with `shape{}`, which is approximate.
- **Far field with grounds.** A ground plate must fit *inside* the far-field box, so a truly infinite plane (one reaching into the absorbing layer) can't be combined with `far_field{}`. Finite plates show the real-world behavior anyway.
- **Not modelled:** feed lines, baluns and matching networks (the port shows the bare antenna impedance), and wire losses (wires are perfect conductors).

`12_monopole_ground.lua` compares ground planes and counterpoises; `13_antenna_matching.lua` shows what a mismatch costs; `validation/dipole_impedance.lua` reproduces the table above.

### Frequency sweeps: `sweep{}`

A continuous wave measures the ports at one frequency. To get Z, S11 and VSWR across a band in **one run**, drive the antennas with a short pulse and add `sweep{}`:

```lua
monopole { base = vec(40, 64, 50), length = 3, radius = mm(0.65), frequency = 2.4e9,
           waveform = "pulse", pulse_width = 0.4 }                  -- a short pulse is a broadband signal
sweep    { frequencies = {1.9e9, 2.9e9}, points = 201, log = "s11.csv" }
```

| Key | Default | Meaning |
|---|---|---|
| `frequencies = {lo, hi}` (Hz) **or** `wavelengths = {lo, hi}` (cells) | required | The band to analyse. |
| `points` | 81 | Frequencies, evenly spaced (2–2001). |
| `log` | none | CSV: `frequency_Hz, wavelength_cells`, then `R_ohm, X_ohm, S11_dB, VSWR` per port. |

How it works: every time step the GPU records each port's gap voltage and current. Their Fourier transforms (double precision, on the CPU) give Z(f) = V(f)/I(f), exactly as a network analyser does with a time-domain pulse. Because the pulse spectrum cancels in the ratio, its exact shape doesn't matter, as long as it carries energy across the band. A Gaussian pulse of `pulse_width` τ periods has a spectrum that falls to 1/e at f₀ ± f₀/(π τ); for example, 0.4 gives about ±80%.

What you see:
- **Plot.** The **Frequency sweep** window has three tabs: S11 (dB), VSWR and impedance (R and X) against frequency in GHz. It marks the −10 dB line, the source (design) frequency, the −10 dB band edges and the best match (frequency, S11, VSWR, Z). It shows up to four ports.
- **Readouts.** The **Instruments** window lists, for each port, the best match (frequency, S11, VSWR, Z) and the −10 dB bandwidth (VSWR < 1.92). The single-frequency port readings are hidden; with a pulse they would be meaningless. **P** prints the same to the console.
- **Title bar.** It shows the best match.

```
sweep: 9600 steps recorded, ring-down 2.2e-08 of peak (settled)
sweep probe   best match 2.4437 GHz (lambda 58.93 cells): S11 -20.4 dB, VSWR 1.21, Z = 52.8 +9.5j ohm | -10 dB band 2.3697-2.5222 GHz (6.2%)
```

Rules:
- **Let it ring down.** The transform is only right once the antenna has stopped ringing. The report gives the ring-down (late voltage relative to the peak) and says "settled" below 10⁻³. High-Q antennas (narrow-band patches, small loops) ring for tens of periods.
- **Restarts.** A sweep always restarts the fields when the script reloads, because the record must start at t = 0.
- **Record length.** The record holds up to 2²³ samples shared by all ports (about 4 million steps for one port). If it fills, the report says so.
- **Accuracy.** The sweep and a CW run agree to 0.1% at the same frequency (patch: 74.13 + j8.59 Ω swept, 74.1 + j8.6 Ω CW). The usual resolution guidelines (§9) apply at the *highest* swept frequency; the script warns when the band goes below 6 cells per wavelength.

`14_patch_antenna.lua` sweeps a probe-fed microstrip patch.

---

## 8. Instruments

Results appear on screen in the **Instruments** window, as tables that update in place at the refresh interval (0.5 s by default; change it under View in the Scene panel). Hover over a column header for an explanation. Plots get their own windows (frequency sweep, radiation pattern, array elements, radar). **P** prints the full report to the console, and unattended `--frames` runs print it periodically. A scene without instruments shows no Instruments window. The first probe, the first flux monitor, the first antenna port (Z and VSWR, or the best match during a frequency sweep) and D_max are also shown in the title bar. Antenna feeds are ports automatically (see §7); **P** lists every port. **Tab** cycles through probes; the arrow keys and PgUp/PgDn move the selected probe. Add `log = "file.csv"` to any instrument to record data. Relative paths go in the scene's output folder.

| Primitive | Keys | Measures | CSV columns |
|---|---|---|---|
| `probe{}` | `position`, `name`, `log` | E, B, S = E×B at a point, with exact Yee-staggered interpolation. Also the local σ and ε_r. | `step,time,Ex,Ey,Ez,Bx,By,Bz,absE,absB,Sx,Sy,Sz` |
| `flux_plane{}` | `axis`, `position`, `min={u,v}`, `max={u,v}` (default: the full interior), `name`, `log` | Power through a rectangle, ∫ S·n dA, with n pointing toward +axis. Reported instantaneous and time-averaged (τ = 3 periods). | `step,time,P,P_avg` |
| `flux_box{}` | `min`/`max` or `center`/`size`, `name`, `log` | **Net outward** power through a closed box. This is the radiated power of the sources inside, or minus the power absorbed by lossy objects inside. | `step,time,P,P_avg` |
| `far_field{}` | box (`min`/`max` or `center`/`size`, default: the interior minus `margin=4`), `wavelength`/`frequency`, `averaging=4` (periods), `axis="+z"` (the antenna's boresight: centre of the front beam map), `name`, `log` | **Antenna far field** via the near-to-far-field transform: radiation pattern, directivity D (dBi), total radiated power, principal-plane cuts with peak, −3 dB beamwidth and front/back ratio. Drawn as polar curves: xy orange, xz cyan, yz green, 30 dB scale. | `angle_deg,xy_dBi,xz_dBi,yz_dBi` |

Cut angles:
- **xy**: measured from +x toward +y (azimuth).
- **xz**: from +x toward +z.
- **yz**: from +y toward +z.

D_max is searched over the three cuts plus two **beam maps**: 81 × 81 grids in direction cosines for the hemisphere around `axis` and the one behind it (about 2° near the axis). The maps are drawn as seen from behind the antenna looking out along `axis`: u is the direction's component to the right, v its component up (up is +z, or +y for an axis along z). For `"+x"`, u is along −y and v along +z; for `"+z"`, u is along −x and v along +y. The **Radiation pattern** window shows them as heatmaps with rings at 30°, 60° and 90° from the axis, so a beam steered in any direction is visible, not only one in a principal plane. The Cuts tab plots the three principal planes in dBi.

**Arrays.** When the ports form a regular rectangular lattice (a phased array), the **Array elements** window shows each element's active VSWR, S11, accepted power, R or X as a map laid out like the array. In an array, an element's impedance depends on how its neighbours are driven (mutual coupling), so it changes with steering: this is the active, or scan, impedance. `15_patch_array.lua` shows it.

**Far-field rules.** The far-field box must enclose all sources and objects, lie in vacuum, and stay out of the absorbing layer. The app warns otherwise. Plane-wave sources cross the box, so the pattern is only meaningful for localized antennas. The pattern is computed at one frequency (CW). After a change, allow about 3 × `averaging` periods for it to settle; the report says "still settling" until then.

**Validated accuracy.** For a Hertzian dipole (`scenes/validation/hertzian_dipole.lua`; theory 1.76 dBi, HPBW 90°), the far-field monitor gives 1.87 dBi and 87.5° at 16 cells/λ, and 1.78 dBi and 89.4° at 32 cells/λ. The error is second order: it falls about 4× each time the resolution doubles. The closed flux box matches the analytic radiated power to within 0.3%.

### Radar: `radar{}`

`radar{}` turns the scene's antenna feeds into a pulsed radar. The feeds transmit (drive them with `waveform = "pulse"`; feeds with `amplitude = 0` only receive), and every feed records its received voltage at every time step. Mark what the radar should detect with `target = true`:

```lua
monopole { ..., waveform = "pulse", pulse_width = 0.5 }                       -- each array element
sphere   { center = vec(112, 112, 148), radius = 30, material = "metal", target = true, name = "sphere" }
radar    { range = {40, 200}, fov = 35, noise = -45, log = "detections.csv" }
```

| Key | Default | Meaning |
|---|---|---|
| `range = {min, max}` | 1 λ to the absorber | Image range in cells from the transmit centre. |
| `fov` | 40 | Image half-angle in degrees, or `{azimuth, elevation}`. Boresight is the feeds' direction. Angles are as seen from behind the array looking out along it: elevation is positive up (+z; +y for an array facing along z), azimuth positive to the right. For a +x-facing array (scene 16) azimuth is toward −y and elevation toward +z. |
| `noise` | none | Receiver noise: rms within the radar's band, in dB relative to the transmit pulse peak at a port (e.g. `-45`). |
| `threshold` | 13 | Detection threshold in dB above the image floor (the median level). |
| `dynamic_range` | 25 | Detections must lie within this many dB of the strongest return. |
| `taper` | `"hamming"` | Receive amplitude taper: `"uniform"` (narrowest beam, −13 dB sidelobes that show up as false targets), `"hamming"` or `"hann"`. |
| `phase_center` | `{0,0,0}` | Element phase centre relative to its feed gap (cells), e.g. the centre of a patch. |
| `tx` | drive-weighted feed centroid | Transmit phase centre, for range and angle. |
| `reference` | 1.5 λ | Distance of the timing reference point on boresight. |
| `log` | none | CSV of the detections after each measurement. |

How it works:
1. **Calibration (clutter cancellation).** The script is run a second time without the `target = true` objects, and the radar first transmits into that scene. This records everything that isn't a target: the pulse ringing in the feeds, coupling between elements, reflections from fixed structures. Then the targets are swapped in, the fields are reset, and the pulse is sent again. The difference of the two recordings is the echo.
2. **Pulse compression.** During calibration, a reference point on boresight records the transmitted field. With the transmit array's near-field factor divided out, squared and divided by the drive spectrum, it gives the radar's complete two-way response T = S·H². Correlating each echo with T peaks at exactly the round-trip path delay, whatever the antennas' group delay.
3. **Back-projection (digital beamforming).** For each point of a range × azimuth × elevation grid, the compressed echoes of all elements are taken at that point's exact round-trip delay and added coherently, with the receive taper. Because the geometry is exact, the image is focused in the array's near field too.
4. **Detection.** The image only covers the simulated volume: points outside the absorbing layer are left out. Then it finds local maxima above the floor plus `threshold` and within `dynamic_range` of the strongest, refined between grid points. Each is matched with the nearest target and compared with the target's point nearest the radar. For a convex object, that's where the mirror-like (specular) "glint" return comes from.

**Listening window.** During calibration the radar watches the timing reference ring down. Once it has fallen below the fidelity level, the radar's two-way response length is measured: the length of its pulse-compression template down to that level. The fidelity level is 10 dB below `dynamic_range`, so dropped tails cannot create, remove or move a detection. Both passes then stop when the echo from the farthest range has fully arrived: 2 × range + the array's size + the response length. For the 8 × 8 patch array the response is about 18 periods; the board and the 64 coupled elements ring for a long time.

**Progressive display.** While the target pass records, the image is recomputed about every half second from the record so far, on a background thread. An echo from range r has fully arrived once the record reaches 2r + the array's size + the response length. Everything nearer than that is **final**: it will not change when more data comes in. The Radar window states how far the image is final, marks the boundary with a "final" line on the range-azimuth image and the range profile, and dims the part beyond it. The noise floor and the detections use only the final part, so detections appear as soon as they are certain and never move afterwards.

**Reuse.** The calibration is kept while only the targets change, so moving a target costs one pulse. Changing only processing settings (`noise`, `threshold`, `fov`, `taper`, `dynamic_range`) re-processes the stored echoes instantly, without re-simulating. **R** transmits again.

The **Radar** window shows the range-azimuth image (B-scope: strongest return over elevation), the azimuth-elevation image at the strongest return's range, the range profile with the floor and threshold, the raw signal at the centre element (calibration, measurement and their difference), and the detection table. In 3D, targets are orange boxes and detections red crosses.

**Measured accuracy** (`16_array_radar.lua`, 8 × 8 patches at 10 GHz, λ = 30 mm, targets 2–4 λ away):

| Target | Range error | Angle error |
|---|---|---|
| Sphere, plate, cylinder (2 × 2 transmitter) | −0.3, +1.3, 0.0 mm | < 1° |
| Sphere at 4 λ, az 15°, el −10° | +2.4 mm | 2.5° az, 0.5° el (relative to the nearest point) |
| Full 8 × 8 transmitter | +0.6 to +3.8 mm | < 0.5° |
| Plate at 10 λ and 15 λ (scene depth 20 λ) | −4.8, −2.2 mm | < 0.3° |
| Sphere at 10 λ / 15 λ / 19 λ | +0.7 / +19 / +13 mm | < 0.3° |

At long range the sphere reads up to 0.6 λ long while the plate stays within 0.15 λ. A sphere is not a single point reflector: the wave creeping around its back returns within the same range cell as its mirror-like front return, and the nearest-surface truth ignores that. A strong target also shows a **multipath ghost** at about twice its range: the echo reflects off the metal array, goes back out and returns (−24 dB for the plate at 10 λ).

Two spheres are resolved 30° apart and merge at 20°: the Hamming-tapered beam is about 19° wide. **Range resolution** is a different matter. Two targets at the same angle are only separated if they are c / 2B apart, where B is the bandwidth: about 7 λ for this patch array's 7% band, which is larger than the whole scene. Real radars use much wider-band waveforms for this.

**Limits.** The calibration assumes the fixed scene doesn't change between the two runs. Targets must be voxel objects (`box`, `sphere`, `cylinder`, `shape`). The transmit model (for range and the template) treats the elements as cos θ radiators. Multiple reflections between a target and the array appear as later "ghost" returns, as they do in a real radar.

---

## 9. Accuracy guidelines

- **Resolution.** Use at least 12–16 cells per wavelength in the *densest* material: λ_material = λ / √ε_r. The app warns below 8. Numerical dispersion makes waves slightly slow; the error falls with the square of the resolution.
- **Clearance.** Keep sources, objects and instruments at least λ/2 away from the absorbing layer. For far fields, also leave a few cells between the objects and the far-field box.
- **Steady state.** CW results (patterns, time-averaged flux) need the start-up transient to pass. That takes the ramp time plus the time for waves to cross the domain, typically 10–20 periods. Press **R** to restart a measurement.
- **Staircasing.** Curved and tilted surfaces are approximated with cubes, and features thinner than one cell vanish. Each E component uses the average material of the four cells around its Yee edge. That makes metal objects conduct on every face and averages permittivity at interfaces.
- **Wires.** Use thin wires (the default) for antennas. A 1 × 1-cell metal rod (`model = "rod"` or a `box`) behaves like a *fat* wire, with an equivalent radius of about 0.3 cells, resonating near 0.42λ instead of ~0.48λ. Give geometry lengths in whole cells: a cell is filled when its centre lies inside the shape, so fractional lengths round unpredictably.
- **Impedance.** For accurate impedances use 30–40 cells per wavelength (resonant length within ~1% of λ at 40 cells/λ, ~3% at 20). Let the port average settle (the report says when); after changing a parameter, wait about 12 periods or press **R**.
- **Metals.** `metal`, `copper`, `pec` and friends all behave as near-perfect conductors at practical cell sizes, because their skin depth is far below one cell. To study finite conductivity, use a lossy `material{ sigma = ... }` in the range 0.1–10.

---

## 10. Limitations

- Non-magnetic materials only (μ_r = 1), and no frequency dispersion.
- Materials are isotropic and constant within each cell.
- Thin wires must run along x, y or z, at least a couple of cells from other conductors (except where they join). Wires are perfect conductors (no ohmic loss).
- Antenna feeds are lumped sources in a one-cell gap. Feed lines, baluns and matching networks aren't modelled. Impedance is evaluated at one frequency per run with CW, or across a band with a pulsed `sweep{}`.
- Metal sheets (`sheet{}`) are perfect conductors lying in lattice planes.
- The radar processes one transmitted pulse per measurement (no Doppler, no moving targets). Its range resolution is set by the antennas' bandwidth.
- One far-field monitor per scene, at a single frequency.
- There is no total-field/scattered-field formulation, so radar cross-section isn't computed. Measure scattering with probes and flux monitors instead.
- Physical units are optional. Without them, all results are in normalised units (ε₀ = μ₀ = c = 1, one cell = 1).
