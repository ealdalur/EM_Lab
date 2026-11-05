# Units in EM_Lab and Converting to SI

EM_Lab computes in **normalized units**. Every number the simulator prints or logs (probe fields, Poynting vectors, flux, radiated power, simulation time) is in these units. This page explains the unit system, why the field scale is yours to choose, and how to convert any quantity to SI units and back.

Contents:
1. [The normalized system](#1-the-normalized-system)
2. [The free field scale E₀](#2-the-free-field-scale-e0)
3. [Conversion table](#3-conversion-table)
4. [What script inputs mean](#4-what-script-inputs-mean)
5. [What the outputs mean](#5-what-the-outputs-mean)
6. [Worked examples](#6-worked-examples)
7. [Setting up a scene from a physical specification](#7-setting-up-a-scene-from-a-physical-specification)
8. [Peak, RMS and time averages](#8-peak-rms-and-time-averages)
9. [Derivation](#9-derivation)

---

## 1. The normalized system

The solver sets three constants to 1:

| Constant | Normalized value | SI value |
|---|---|---|
| Speed of light c | 1 | 299 792 458 m/s |
| Permittivity of free space ε₀ | 1 | 8.854 × 10⁻¹² F/m |
| Permeability of free space μ₀ | 1 | 1.2566 × 10⁻⁶ H/m |
| Free-space wave impedance η₀ = √(μ₀/ε₀) | 1 | 376.73 Ω |

Two base scales come from the scene:

- **Length: one cell.** The physical size of a cell is Δ, set by `scene{ cell_size = ... }` in metres. The default is 1 mm.
- **Time: the time light takes to cross one cell**, Δ/c. At Δ = 1 mm that's 3.336 ps. The solver's time step is half of that (the Courant number, 0.5).

The magnetic field is stored as **c·B**, which has the same units as E. So in a plane wave in vacuum |E| = |c·B|, and the Poynting vector is simply S = E × (c·B).

## 2. The free field scale E₀

Lengths and times map to SI through Δ alone, but field strengths need one more number: the **field scale E₀**, in V/m. It's the SI electric field that corresponds to a normalized field of 1.

**E₀ isn't set anywhere in the program, and doesn't need to be.** Maxwell's equations are linear: doubling every source doubles every field, so the simulation gives identical results for any E₀. You choose E₀ *after* the simulation, to make one known quantity match reality. Every other quantity then follows from it.

Common ways to fix E₀:

| You know… | Then E₀ = | Where `a` is… |
|---|---|---|
| The available power P (W) of a voltage-fed antenna (the transmitter power) | √(P · η₀ / (P_avail,n · Δ²)) | P_avail,n = the "avail" value on the port line |
| The peak EMF V (V) of a voltage-fed antenna | V / (a · Δ) | the antenna's `amplitude` |
| The peak current I (A) of a current-fed antenna or ideal source | η₀ · I / (a · Δ) | the source's `amplitude` |
| The peak incident field E (V/m) of a plane wave or beam | E / a | the source's `amplitude` |
| The radiated or transmitted power P (W) | √(P · η₀ / (P_n · Δ²)) | the normalized power P_n the simulator reports |
| The current density J (A/m²) of a `current_source` | η₀ · Δ · J / a | the source's `amplitude` |

Once E₀ is fixed, it applies to the whole scene.

## 3. Conversion table

To convert a normalized value to SI, multiply by the factor in the second column. To go the other way, divide by it.

Symbols: Δ = cell size (m), c = speed of light, ε₀, η₀ = 376.73 Ω, E₀ = field scale (V/m).

| Quantity | Normalized → SI factor | SI unit | Notes |
|---|---|---|---|
| Length, position | Δ | m | lattice coordinates × Δ |
| Area | Δ² | m² | |
| Volume | Δ³ | m³ | |
| Time t | Δ / c | s | 3.336 ps per unit at Δ = 1 mm |
| Time step dt | 0.5 · Δ / c | s | Courant number 0.5 |
| Wavelength λ (in cells) | Δ | m | |
| Frequency | f = c / (λ_cells · Δ) | Hz | λ = 16 cells at 1 mm → 18.74 GHz |
| Angular frequency ω | c / Δ | rad/s | normalized ω = 2π / λ_cells |
| Wavenumber k | 1 / Δ | rad/m | normalized k = 2π / λ_cells |
| Electric field E | E₀ | V/m | |
| Magnetic flux density B | E₀ / c | T | the simulator reports c·B |
| Magnetic field H | E₀ / η₀ | A/m | normalized H = c·B |
| Poynting vector S (power per area) | E₀² / η₀ | W/m² | |
| Power (flux P, `<P>`, `P_rad`) | E₀² · Δ² / η₀ | W | |
| Energy density u | ε₀ · E₀² | J/m³ | normalized u = ½(ε_r·E² + (cB)²) |
| Energy | ε₀ · E₀² · Δ³ | J | |
| Current density J | E₀ / (η₀ · Δ) | A/m² | |
| Current I | E₀ · Δ / η₀ | A | |
| Current moment I·l | E₀ · Δ² / η₀ | A·m | |
| Conductivity σ (`sigma`) | 1 / (η₀ · Δ) | S/m | σ_SI = σ_n / (376.73 · Δ) |
| Relative permittivity ε_r | 1 | – | dimensionless, unchanged |
| Loss tangent tan δ | 1 | – | dimensionless, unchanged |
| Wave impedance | η₀ | Ω | normalized vacuum impedance = 1 |
| Antenna impedance Z, R_rad, feed `impedance` | — | Ω | already reported in ohms (normalized × η₀); independent of E₀ and Δ |
| Voltage (gap EMF, `amplitude` of voltage feeds) | E₀ · Δ | V | |
| Directivity D, dBi, beamwidth, angles | 1 | – | dimensionless, unchanged |

## 4. What script inputs mean

| Script input | Unit |
|---|---|
| Positions, sizes, `length`, `radius`, `spacing`, `thickness` | cells (multiply by Δ for metres; `mm()` and `meters()` convert for you) |
| `wavelength` | cells |
| `frequency` | **Hz (SI)**, converted with λ_cells = c / (f · Δ) |
| `cell_size` | **metres (SI)** |
| `plane_wave` / `gaussian_beam` `amplitude` | peak **E-field** amplitude, normalized (× E₀ → V/m) |
| `dipole` / `monopole` / `loop` / `array` `amplitude` | voltage feed (default): peak **source EMF**, normalized (× E₀Δ → V); `feed = "current"`: peak **feed current** (× E₀Δ/η₀ → A) |
| antenna `impedance` | **ohms (SI)**: the source's internal resistance |
| thin-wire `radius` | cells (× Δ → m; `mm()` converts) |
| `current_source` `amplitude` | peak **current density** J, normalized (× E₀/(η₀Δ) → A/m²) |
| `material{ sigma = ... }` | conductivity, normalized (σ_SI = σ_n / (η₀Δ)) |
| `material{ conductivity = ... }` | **S/m (SI)**, converted with σ_n = σ_SI · η₀ · Δ |
| `material{ loss_tangent = ... }` | dimensionless, converted at the scene's reference frequency with σ_n = tan δ · (2π / λ_cells) · ε_r |
| `phase`, `rotate`, `steer`, `phase_step` | degrees |
| `ramp`, `pulse_width`, `delay`, `interval` | periods of the source frequency |

Example: copper's conductivity is 5.8 × 10⁷ S/m. At Δ = 1 mm that's σ_n = 5.8 × 10⁷ × 376.73 × 0.001 ≈ 2.2 × 10⁷, an essentially perfect conductor. Seawater (4 S/m) becomes σ_n ≈ 1.5, a strongly lossy medium.

## 5. What the outputs mean

| Output | Normalized quantity | To SI |
|---|---|---|
| `[t=… step …]` | time t; step count (t = step × 0.5) | t × Δ/c |
| Probe `E=(…)`, `\|E\|` | electric field, instantaneous | × E₀ (V/m) |
| Probe `\|B\|` | c·B, instantaneous | × E₀/c (T), or × E₀/η₀ for H (A/m) |
| Probe `S=(…)` | Poynting vector, instantaneous | × E₀²/η₀ (W/m²) |
| Probe `sigma`, `eps_r` | material at the probe (normalized σ, ε_r) | σ / (η₀Δ) (S/m); ε_r unchanged |
| Flux `P` | net power through the surface, instantaneous | × E₀²Δ²/η₀ (W) |
| Flux `<P>` | time-averaged power (≈ 3-period average) | × E₀²Δ²/η₀ (W) |
| Flux `peak\|P\|` | largest \|P\| since reset | × E₀²Δ²/η₀ (W) |
| Far field `P_rad` | time-averaged radiated power | × E₀²Δ²/η₀ (W) |
| Port `Z`, `R_rad` | impedance, radiation resistance | already in Ω |
| Port `Gamma`, `S11`, `VSWR`, percentages | dimensionless | unchanged |
| Port `P_acc`, `avail` | accepted and available power | × E₀²Δ²/η₀ (W) |
| Far field `D_max`, dBi, HPBW, front/back | dimensionless | unchanged |
| CSV `time` column | t | × Δ/c (s) |
| CSV field / power columns | as for the console | as above |

## 6. Worked examples

### Example 1: a dipole driven with 1 A

Scene `validation/hertzian_dipole.lua`: an ideal point dipole with `amplitude = 7`, default Δ = 1 mm, λ = 16 cells. The far-field monitor reports `P_rad = 0.34876`. The same steps apply to any antenna; the `amplitude` of `dipole`, `monopole`, `loop` and `array` is always the peak feed current.

1. Choose E₀ so that `amplitude = 7` means a 1 A peak current:
   E₀ = η₀ · I / (a · Δ) = 376.73 × 1 / (7 × 0.001) ≈ 53 820 V/m.
2. Radiated power:
   P = 0.34876 × E₀² · Δ² / η₀ = 0.34876 × 53 820² × 0.001² / 376.73 ≈ **2.68 W**.
   (The flux box's `<P> = 0.355` gives 2.73 W.)
3. Frequency: f = c / (16 × 0.001) ≈ 18.7 GHz.
4. Field at the `broadside` probe (24 mm away), if it reads |E| = 0.0117: 0.0117 × 53 820 ≈ 630 V/m (peak, instantaneous).

Sanity check against the textbook formula for a short dipole, P = η₀ (k·l)² I² / (12π). The source's current is spread along z with an effective length l = 0.75 × √π ≈ 1.33 cells = 1.33 mm, and k = 2π/λ = 392.7 rad/m. That gives (k·l)² ≈ 0.272 and P ≈ 9.99 × 0.272 × 1² ≈ **2.72 W**, matching the simulation.

### Example 2: a dipole driven by a 1 W transmitter

Scene `04_dipole.lua`: a thin-wire dipole with the default 50 Ω voltage feed, `amplitude = 1`, Δ = 1 mm, λ = 40 cells (7.5 GHz). The port line reads `Z = 64.3 −21.1j ohm … P_acc 8.937e-01 of 9.418e-01 avail`.

1. The impedance (64.3 − j21.1 Ω), Γ, VSWR (1.56) and the 95% are already physical: no conversion needed.
2. Choose E₀ so that the source makes 1 W available (a 1 W transmitter):
   E₀ = √(P · η₀ / (P_avail,n · Δ²)) = √(1 × 376.73 / (0.9418 × 0.001²)) = 20 000 V/m.
3. The source EMF is then a · E₀ · Δ = 1 × 20 000 × 0.001 = 20 V peak. Check: V²/(8 R_s) = 400/400 = 1 W.
4. Power accepted by the antenna: 0.8937 × E₀²Δ²/η₀ = 0.8937 × 1.062 ≈ **0.95 W**, and it's all radiated (efficiency 100%).

### Example 3: power through the double slit

Scene `01_double_slit.lua`: a plane wave with `amplitude = 0.5`. The `power through slits` monitor reports `<P> ≈ 44.8`.

1. Suppose the real incident wave is 10 V/m peak. Then E₀ = 10 / 0.5 = 20 V/m.
2. Power through the slits: 44.8 × 20² × 0.001² / 376.73 ≈ 4.8 × 10⁻⁵ W = **48 µW**.
3. Sanity check: the incident intensity is E²/(2η₀) = 10² / 753.5 ≈ 0.133 W/m². The two slits are 6 mm × 48 mm each (5.76 × 10⁻⁴ m² in total), so their geometric share would be 76 µW. Slits only half a wavelength wide pass less than their area would suggest, so about 60% of that is plausible.

### Example 4: simulation time

At Δ = 1 mm, `t = 1177` is 1177 × 3.336 ps ≈ **3.93 ns**. At λ = 16 cells that's 1177 / 16 ≈ 74 periods.

## 7. Setting up a scene from a physical specification

Going the other way, from SI to normalized:

1. **Choose the cell size** from the shortest wavelength that matters, including inside dielectrics, where λ_material = λ / √ε_r. Aim for at least 16 cells per wavelength, e.g. at 2.4 GHz (λ = 125 mm) use Δ ≈ 125 / 16 ≈ 7.8 mm, rounded to `cell_size = 7.5e-3`.
2. **Give frequencies in Hz** (`frequency = 2.4e9`) and **physical lengths with `mm()` or `meters()`**: `length = mm(62.5)` is a half-wave dipole at 2.4 GHz.
3. **Give material losses in S/m** with `conductivity`, or as `loss_tangent`.
4. **Pick convenient source amplitudes** (they only set the field scale), simulate, and convert the outputs with E₀ from §2.

```lua
scene { name = "2.4 GHz half-wave dipole", size = {128, 128, 128}, cell_size = 7.5e-3, frequency = 2.4e9 }
dipole    { center = grid.center, orientation = "z", length = mm(62.5), radius = mm(1), amplitude = 1 }   -- thin wire, 50-ohm feed
far_field {}
-- Afterwards: Z, VSWR are already in ohms / ratios. For a P-watt transmitter, E0 = sqrt(P * 376.73 / (avail * 7.5e-3^2))
-- (avail = the port line's available power); multiply power outputs by E0^2 * (7.5e-3)^2 / 376.73.
```

## 8. Peak, RMS and time averages

- **Source amplitudes are peak values.** A CW current `amplitude = I` means i(t) = I · sin(ωt). The RMS value is I/√2.
- **Probe readings are instantaneous** and oscillate. The peak over a period is the amplitude, which reaches its steady value after the start-up ramp.
- **`<P>` and `P_rad` are time-averaged powers.** For a sinusoid that's half of peak × peak, the same as RMS × RMS. That's the number to compare with "radiated power" or "transmitted power" in datasheets and textbooks. For example, the short-dipole formula P = η₀ (kl)² I²/(12π) uses the peak current I.
- **Instantaneous `P`** oscillates at twice the source frequency around `<P>`.

## 9. Derivation

The scales follow from making Maxwell's equations dimensionless. Write x = Δ·ξ, t = (Δ/c)·τ, E = E₀·e, H = (E₀/η₀)·h and J = E₀/(η₀Δ)·j.

Faraday's law, ∂B/∂t = −∇×E with B = μ₀H:

  μ₀ (E₀/η₀) (c/Δ) ∂h/∂τ = −(E₀/Δ) ∇′×e.

Since μ₀ c / η₀ = 1, this becomes ∂h/∂τ = −∇′×e.

Ampère's law, ε₀ ∂E/∂t = ∇×H − J:

  ε₀ E₀ (c/Δ) ∂e/∂τ = (E₀/(η₀Δ)) ∇′×h − E₀/(η₀Δ) · j.

Since ε₀ c η₀ = 1, this becomes ∂e/∂τ = ∇′×h − j.

So the dimensionless equations have no constants left, which is exactly the system the solver integrates (with h stored as c·B/E₀). The remaining factors follow:
- S = E × H = (E₀²/η₀) (e × h).
- Power through an area: × Δ².
- Current: J × Δ² = E₀Δ/η₀.
- Energy density: ½ε₀E² + ½μ₀H² = ε₀E₀² · ½(e² + h²).
- Conductivity: from the σE term, σ_n = σ · η₀ · Δ.
