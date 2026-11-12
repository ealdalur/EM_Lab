-- Validation: a coax{} line with known terminations.
-- A 50-ohm cable (1-cell centre conductor, 5-cell shield, dielectric chosen for 50 ohm) with
-- a matched source, terminated by a matched load, an open, a short, or 100 ohm. The Cable
-- window's two-wave fit gives the reflection coefficient at the far end; theory:
--   load (50 ohm)  Gamma = 0          open   |Gamma| = 1 (the 2-cell capped gap is capacitive)
--   short          Gamma = 1 @ 180    100 ohm  Gamma = 1/3 @ 0, Z = 100 ohm
-- and the line impedance measured from the waves, V+/I+, equals the design Z0.
-- Measured (48 cells/lambda): Z0 50.0 ohm; load Gamma 0.057 (RL 25 dB); open |Gamma| 0.999;
-- short 1.000 @ 180 deg; 100 ohm Gamma 0.338, Z 94 - j22 ohm.
--   T   termination

local lam  = 48
local term = param { name = "termination", label = "termination", choices = {"load", "open", "short", "100 ohm"}, key = "t" }
local pml  = 16
scene {
  name        = "Validation: coax line terminations",
  description = "Reflection from a matched load, open, short and 100-ohm load on a 50-ohm coax{} line.",
  size        = {2 * pml + 200, 64, 64},
  wavelength  = lam,
}
coax { from = vec(pml + 20, 32, 32), axis = "+x", length = 150, z0 = 50,
       termination = term == "100 ohm" and "load" or term, load = term == "100 ohm" and 100 or nil }
view { gain = 4, distance = 2.2, yaw = -90, pitch = 20, steps_per_frame = 8, metal_opacity = 0.08 }
