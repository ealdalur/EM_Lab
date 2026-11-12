-- Coax-fed dipole: traveling and standing waves, and why antennas need a balun
-- A generator (50 ohm source) drives a coaxial cable; at the far end the cable feeds a
-- half-wave dipole: the centre conductor to the upper arm, the shield to the lower arm.
--
-- The cable is real metal: a square shield, a dielectric and a centre conductor, all on the
-- grid. Its dielectric is chosen so that it is a 50 (or 75) ohm line. The "Cable" window
-- measures the voltage between the conductors at every point along it:
--   - A matched load absorbs everything: a pure traveling wave. The live voltage (white)
--     slides along under a flat envelope (blue).
--   - An open or short reflects everything: a pure standing wave. The live voltage pulses in
--     place between fixed nodes half a wavelength apart.
--   - The dipole is in between: part of the wave reflects, so the envelope ripples. The
--     ripple's max / min is the VSWR, its position the phase of the reflection.
-- On the dipole itself the current is always a standing wave: it is zero at the open tips.
--
-- The balun. A coax is unbalanced: its current flows on the inside of the shield, but at the
-- end the dipole's lower arm is fed from the shield, and some current runs back down the
-- OUTSIDE of the shield. The cable then becomes part of the antenna: it radiates, tilts the
-- pattern, and the feed impedance depends on the cable's length and routing. The "shield"
-- plot shows that current. A quarter-wave sleeve balun ("bazooka": a metal tube round the end
-- of the cable, shorted to the shield a quarter wavelength back) forms a shorted quarter-wave
-- stub with the shield, which looks like an open circuit to current on the outside of the
-- shield at the feed point and chokes it off.
--
-- Things to try:
--   B       balun: sleeve / none. Watch the shield current, the radiation pattern and Z.
--   / \     balun sleeve length (wavelengths): the choke only works near a quarter wave.
--   , .     dipole length (wavelengths): the reflection grows off resonance.
--   T       termination: the dipole, a matched load, an open or a short.
--   I       cable impedance: 50 or 75 ohm (the dipole is ~68 ohm: a better match on 75).
--   ; '     cable length (wavelengths in air).
--   U       signal: continuous wave or a single pulse. With the pulse, watch it run down the
--           cable and come back; the frequency-sweep window shows S11 at the generator.
--   M / V   render mode / field: look at E or B, magnitude or signed. - / + change the gain:
--           the fields inside the cable are ~20 dB stronger than the radiated wave.
--
-- Measured (50 ohm cable, 0.52-wavelength dipole, 96 cells per wavelength):
--   sleeve balun: Z at the cable end 68 - j8 ohm, VSWR 1.39, shield current -27 dB,
--                 pattern 3.0 dBi, round in azimuth within 1.8 dB, beam horizontal;
--   no balun:     Z 70 - j19 ohm, shield current -9 dB (it carries a standing wave too),
--                 5.3 dBi with the beam tilted 32 degrees up toward the cable;
--   75 ohm cable with the balun: VSWR 1.17.
-- The cable here is electrically thick (0.07 wavelength; real coax is far thinner), so even
-- with the balun its end and sleeve sit close to the dipole and tilt the pattern a little.

local lambda0 = 96                                    -- cells per wavelength in air (keeps the cable thin)
local f0      = 300e6                                 -- 1 m wavelength

local term    = param { name = "termination", label = "termination", choices = {"dipole", "load", "open", "short"}, key = "t" }
local dlen    = param { name = "dipole_length", label = "dipole length (wavelengths)", default = 0.52, min = 0.30, max = 0.70, step = 0.02, keys = {",", "."} }
local balun   = param { name = "balun", label = "balun", choices = {"sleeve", "none"}, key = "b" }
local blen    = param { name = "balun_length", label = "balun sleeve length (wavelengths)", default = 0.24, min = 0.10, max = 0.40, step = 0.02, keys = {"/", "\\"} }
local zline   = param { name = "z0", label = "cable impedance (ohm)", choices = {"50", "75"}, key = "i" }
local clen    = param { name = "cable_length", label = "cable length (wavelengths)", default = 2.0, min = 1.0, max = 3.0, step = 0.25, keys = {";", "'"} }
local signal  = param { name = "signal", label = "signal", choices = {"cw", "pulse"}, key = "u" }

local pml     = 16
local L       = math.floor(clen * lambda0 + 0.5)       -- cable length (cells)
local maxArm  = math.ceil(0.70 * lambda0 / 2)
local clear   = math.floor(lambda0 / 2)                -- half a wavelength to the absorbing layer
local nx      = pml + 16 + L + 3 + clear + pml
local ny      = 2 * (pml + clear) + 13
local nz      = 2 * (pml + clear + maxArm) + 8

scene {
  name        = "Coax-fed dipole and balun",
  description = "A generator, a long coax and a dipole: traveling vs standing waves, and the balun.",
  size        = {nx, ny, nz},
  cell_size   = 299792458 / (f0 * lambda0),
  wavelength  = lambda0,
}

local yc, zc = math.floor(ny / 2) + 0.5, math.floor(nz / 2) + 0.5
local pulse  = signal == "pulse"
local cable  = coax {
  name = "cable", from = vec(pml + 16, yc, zc), axis = "+x", length = L, z0 = tonumber(zline),
  termination = term == "dipole" and "terminals" or term, terminals = "+z",
  balun = balun == "sleeve" and term == "dipole", balun_length = math.floor(blen * lambda0 + 0.5),
  frequency = f0, waveform = pulse and "pulse" or "cw", pulse_width = pulse and 0.6 or nil,
}

if term == "dipole" then
  -- Arms from the two terminals, centred on the middle of the feed region.
  local ti, to = cable.inner_terminal, cable.outer_terminal
  local mid    = 0.5 * (ti.z + to.z)
  local half   = 0.5 * dlen * lambda0
  wire { from = ti, to = vec(ti.x, ti.y, math.floor(mid + half + 0.5)), radius = 0.15 }
  wire { from = to, to = vec(to.x, to.y, math.floor(mid - half + 0.5)), radius = 0.15 }
  if not pulse then far_field { axis = "+y", frequency = f0 } end
end
if pulse then sweep { frequencies = {0.6 * f0, 1.4 * f0}, points = 161 } end

view { gain = 10, distance = 2.0, yaw = -90, pitch = 12, steps_per_frame = 8, metal_opacity = 0.08 }
