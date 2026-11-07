-- Antenna matching: why length matters (VSWR and power transfer)
-- A transmitter delivers its maximum power only into a load equal to its own impedance
-- (here a 50-ohm source, as if behind a matched 50-ohm line). An antenna's feed impedance
-- depends on its length: at resonance it is almost purely resistive; shorter antennas turn
-- capacitive (X < 0), longer ones inductive (X > 0) and their resistance changes too. The
-- mismatch reflects part of the transmitter's power back instead of radiating it.
--
-- Start: a resonant half-wave dipole (19 cells = 0.487 wavelength, ~70 ohm).
--   , .   shorten / lengthen the antenna (one step = 0.05 wavelength of total length)
--   G     switch between the dipole and a quarter-wave monopole on a 2 x 2 wavelength
--         ground plate (10 cells; ~40 ohm). Each keeps the same length steps.
--   I     feed line / transmitter impedance: 50 ohm or 75 ohm (TV-style coax), always with the
--         same transmitter power. A resonant dipole (~70 ohm) is matched by 75 ohm (VSWR 1.12,
--         100% accepted); a monopole (~45 ohm) is better on 50 ohm.
--
-- Watch:
--   - the glow of the radiated wave dims as the antenna is detuned (the view gain is fixed);
--   - the port line: Z, Gamma, VSWR and "P_acc ... of ... avail (%)", the share of the
--     transmitter's available power the antenna accepts; the rest is reflected (|Gamma|^2);
--   - the flux box: the power actually radiated, which follows the accepted power;
--   - the title bar shows Z and VSWR live.
-- Measured (50 ohm): resonant dipole 69.5 - j6 ohm, VSWR 1.41, 97% accepted; two steps shorter
-- 34 - j141 ohm, VSWR 13.5, 25%; two steps longer 141 + j135 ohm, VSWR 5.6, 52%.
-- A badly detuned antenna still glows near the wire: that is energy sloshing in its reactive
-- near field rather than leaving as radiation.
-- Rule of thumb: VSWR 1.5 reflects 4% of the power, 2 reflects 11%, 3 reflects 25%, 6 reflects 51%.
-- After each change the impedance needs ~12 periods (about 2 s) to settle; the report says so.

local L = 39                             -- wavelength in cells (puts the dipole's resonance on a whole cell)

scene {
  name        = "Antenna Matching: Length vs VSWR",
  description = "Detune a resonant dipole or monopole and watch the mismatch (VSWR) cut the transmitted power.",
  size        = {144, 144, 144},
  wavelength  = L,
}

local antenna = param { name = "antenna", label = "antenna", choices = {"dipole", "monopole"}, key = "g" }
local trim    = param { name = "trim", label = "length steps", default = 0, min = -5, max = 5, step = 1, keys = {",", "."} }
local line    = param { name = "line", label = "line", choices = {"50", "75"}, key = "i" }

local c   = grid.center                  -- feed point (a lattice node)
local z0  = tonumber(line)
-- Same transmitter power for either line: available power V^2 / (8 R) stays constant
-- when the source EMF scales with sqrt(R).
local emf = math.sqrt(z0 / 50)

local length, resonant
if antenna == "dipole" then
  resonant = 19                          -- 0.487 wavelength: reactance ~0
  length   = resonant + 2 * trim         -- a centre-fed dipole grows by one cell per arm
  dipole { center = c, orientation = "z", length = length, amplitude = emf, impedance = z0, name = "dipole" }
else
  resonant = 10                          -- ~0.26 wavelength: closest whole-cell length to resonance
  length   = resonant + trim
  -- 2 x 2 wavelength ground plate, top face at z = c.z (one cell thick).
  box { min = vec(c.x - L, c.y - L, c.z - 1), max = vec(c.x + L + 1, c.y + L + 1, c.z), material = "metal" }
  monopole { base = c, orientation = "+z", length = length, amplitude = emf, impedance = z0, name = "monopole" }
end

print(string.format("%s length %d cells = %.3f wavelengths (resonant: %d cells = %.3f), %s-ohm feed",
                    antenna, length, length / L, resonant, resonant / L, line))

probe    { position = c + vec(40, 0, 0), name = "broadside" }
flux_box { center = c, size = 100, name = "radiated power" }
far_field {}

view { gain = 45, distance = 2.4, yaw = -60, pitch = 20, steps_per_frame = 8, steps_per_frame = 1 }
