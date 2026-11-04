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
| `EM_Lab --script my_scene.lua` | Load a script from the command line. `--list` prints the bundled scenes. |
| Save the file | **Hot reload.** The script is re-run automatically about 0.5 s after you save it. **F5** reloads manually. |
| `--set name=value` | Override a `param{}` at startup. Repeat the option for several parameters. |

**Reloading.** If a reload changes only sources, instruments or view settings, the running fields are kept. That's how live beam steering works. If the grid size or any material changes, the fields are reset.

**Errors.** A failed script reports the file, line and reason in the console, for example `my_scene.lua:12: sphere: 'radius' must be > 0`. The previous scene keeps running. Unknown or misspelled keys trigger warnings with suggestions, such as `unknown field 'radious' ignored (did you mean 'radius'?)`.

**Sandbox.** Scripts run without the `io` and `os` libraries and cannot load C modules. A script that runs longer than 30 s is stopped. `require "name"` loads `name.lua` from the script's folder or its `lib/` subfolder, so you can keep shared component libraries there. `print()` writes to the console.

---

## 2. Units and coordinates

- **Length: cells.** All positions and sizes are in lattice cells. The origin is at the grid corner, so cell `(i, j, k)` spans `[i, i+1)` on each axis. **+z is up**, and the camera orbits about z.
- **Wavelength: cells per wavelength.** `scene{ wavelength = 16 }` sets the reference wavelength. Sources, loss tangents and `wavelengths()` use it by default, and it determines accuracy (see §9).
- **Physical scale.** `scene{ cell_size = 1e-3 }` (in metres, default 1 mm) maps cells to real units. Wherever a `wavelength` can be given, you can give `frequency` (Hz) instead, and materials can take a `conductivity` in S/m. For example, with 1 mm cells, `frequency = 10e9` means λ = 30 cells.
- **Time.** The speed of light is c = 1 cell per time unit, and one period equals λ time units. Time-related source keys (`ramp`, `pulse_width`, `delay`, `interval`) are given **in periods**.
- **Fields.** Normalised units with ε₀ = μ₀ = c = 1. The B channel stores c·B, so the Poynting vector is S = E × B and the wave impedance is 1.
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

Parameters may use these keys: `B F G I J T U X Y Z , . ; ' / \ `` ` ``. All other keys belong to the application. Current values appear in the title bar.

---

## 5. Geometry

Shapes are voxelised in the order they are called, and **later shapes overwrite earlier ones**. To cut a hole, fill a region with `material = "vacuum"`. Each cell holds one material, sampled at the cell centre.

Common keys:
- `material`: a name or inline table. Default `"metal"` (copper).
- `rotate = {rx, ry, rz}`: Euler angles in degrees, applied about x, then y, then z, around the shape's centre. Supported by `box`, `sphere` and `cylinder`.
- `thickness`: makes the shape hollow, leaving a shell of that thickness. Supported by `box`, `sphere` and `cylinder`.
- `name`: optional label used in messages.

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
| `amplitude` | 1 | **E-field amplitude** for `plane_wave` and `gaussian_beam`. **Peak current** for wires (`dipole`, `loop`, `array`). Raw current density J₀ for `current_source`. |
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
| `dipole{}` | `center`, `orientation="z"`, `length=0` | `length = 0` gives a Hertzian point dipole (an "oscillating electron"; any orientation). `length > 0` gives a uniform-current wire antenna (axis-aligned only). |
| `loop{}` | `center`, `normal="z"`, `size=4` | Small square current loop (magnetic dipole), counter-clockwise about `normal`. |
| `array{}` | `center`, `elements` (`8` or `{4,4}`), `axis` (`"y"` or `{"y","z"}`), `spacing` (default λ/2, or `{dy, dz}`), `orientation="z"`, `length` (default λ/2), `steer` or `phase_step`, `taper` | Phased array of dipoles. `steer` (degrees from broadside toward +axis) sets phase step Δφ = −k·d·sin θ; `phase_step` sets it directly in degrees. Use `{a, b}` per axis for 2-D arrays. `taper`: `"uniform"`, `"binomial"`, `"hann"`, `"cosine"`, `"triangular"`, or a list of weights (1-D). |
| `current_source{}` | `min`/`max` or `center`/`size`, `polarization`, `softness={sx,sy,sz}` | Generic impressed current density with a soft-edged box footprint. The building block behind all the others. |

Sources are *soft* current sources: reflected waves pass through them. All wires have a Gaussian cross-section about 0.75 cells in radius.

---

## 8. Instruments

Results appear in the console every 2 s, and immediately when you press **P**. The first probe and flux monitor are also shown in the title bar, along with D_max. **Tab** cycles through probes; the arrow keys and PgUp/PgDn move the selected probe. Add `log = "file.csv"` to any instrument to record data. Relative paths go in the scene's output folder.

| Primitive | Keys | Measures | CSV columns |
|---|---|---|---|
| `probe{}` | `position`, `name`, `log` | E, B, S = E×B at a point, with exact Yee-staggered interpolation. Also the local σ and ε_r. | `step,time,Ex,Ey,Ez,Bx,By,Bz,absE,absB,Sx,Sy,Sz` |
| `flux_plane{}` | `axis`, `position`, `min={u,v}`, `max={u,v}` (default: the full interior), `name`, `log` | Power through a rectangle, ∫ S·n dA, with n pointing toward +axis. Reported instantaneous and time-averaged (τ = 3 periods). | `step,time,P,P_avg` |
| `flux_box{}` | `min`/`max` or `center`/`size`, `name`, `log` | **Net outward** power through a closed box. This is the radiated power of the sources inside, or minus the power absorbed by lossy objects inside. | `step,time,P,P_avg` |
| `far_field{}` | box (`min`/`max` or `center`/`size`, default: the interior minus `margin=4`), `wavelength`/`frequency`, `averaging=4` (periods), `name`, `log` | **Antenna far field** via the near-to-far-field transform: radiation pattern, directivity D (dBi), total radiated power, principal-plane cuts with peak, −3 dB beamwidth and front/back ratio. Drawn as polar curves: xy orange, xz cyan, yz green, 30 dB scale. | `angle_deg,xy_dBi,xz_dBi,yz_dBi` |

Cut angles:
- **xy**: measured from +x toward +y (azimuth).
- **xz**: from +x toward +z.
- **yz**: from +y toward +z.

D_max is searched over the three cuts plus a 10° grid covering the whole sphere.

**Far-field rules.** The far-field box must enclose all sources and objects, lie in vacuum, and stay out of the absorbing layer. The app warns otherwise. Plane-wave sources cross the box, so the pattern is only meaningful for localized antennas. The pattern is computed at one frequency (CW). After a change, allow about 3 × `averaging` periods for it to settle; the report says "still settling" until then.

**Validated accuracy.** For a Hertzian dipole (theory 1.76 dBi, HPBW 90°), the far-field monitor gives 1.87 dBi and 87.5° at 16 cells/λ, and 1.78 dBi and 89.4° at 32 cells/λ. The error is second order: it falls about 4× each time the resolution doubles. The closed flux box matches the analytic radiated power to within 0.3%.

---

## 9. Accuracy guidelines

- **Resolution.** Use at least 12–16 cells per wavelength in the *densest* material: λ_material = λ / √ε_r. The app warns below 8. Numerical dispersion makes waves slightly slow; the error falls with the square of the resolution.
- **Clearance.** Keep sources, objects and instruments at least λ/2 away from the absorbing layer. For far fields, also leave a few cells between the objects and the far-field box.
- **Steady state.** CW results (patterns, time-averaged flux) need the start-up transient to pass. That takes the ramp time plus the time for waves to cross the domain, typically 10–20 periods. Press **R** to restart a measurement.
- **Staircasing.** Curved and tilted surfaces are approximated with cubes, and features thinner than one cell vanish. Each E component uses the average material of the four cells around its Yee edge. That makes metal objects conduct on every face and averages permittivity at interfaces.
- **Thin wires.** A 1 × 1-cell metal rod behaves like a *fat* wire, with an equivalent radius of about 0.3 cells. At 20 cells/λ its first resonance is about 0.42λ, shorter than a thin wire's ~0.48λ, so tune parasitic elements accordingly (see `06_yagi_uda.lua`). Give lengths in whole cells with `min`/`max`: a cell is filled when its centre lies inside the shape, so fractional lengths round unpredictably.
- **Metals.** `metal`, `copper`, `pec` and friends all behave as near-perfect conductors at practical cell sizes, because their skin depth is far below one cell. To study finite conductivity, use a lossy `material{ sigma = ... }` in the range 0.1–10.

---

## 10. Limitations

- Non-magnetic materials only (μ_r = 1), and no frequency dispersion.
- Materials are isotropic and constant within each cell.
- Sources are impressed currents. There is no feed-gap voltage, so input impedance isn't available.
- One far-field monitor per scene, at a single frequency.
- There is no total-field/scattered-field formulation, so radar cross-section isn't computed. Measure scattering with probes and flux monitors instead.
- Physical units are optional. Without them, all results are in normalised units (ε₀ = μ₀ = c = 1, one cell = 1).
