-- VALIDATION: ideal Hertzian (point) dipole
-- An infinitesimally short current element, J_z = I sin(wt): the textbook building block of
-- antenna theory (an "oscillating electron"). It cannot be built, but its fields are known
-- exactly, so this scene checks the solver and the far-field monitor against theory:
--   directivity D = 1.5 (1.76 dBi), HPBW = 90 deg in the elevation cuts, omni in azimuth.
-- Measured: 1.87 dBi / 87.5 deg at 16 cells per wavelength, 1.78 dBi / 89.4 deg at 32.
--
-- For a real, buildable antenna see 04_dipole.lua.
-- Run with:  EM_Lab --script validation/hertzian_dipole

scene {
  name        = "Validation: Hertzian (point) dipole",
  description = "Ideal point current element; compare the far-field result with D = 1.76 dBi, HPBW = 90 deg.",
  size        = {128, 128, 128},
  wavelength  = 16,                        -- cells per wavelength (resolution)
}

local pulsed = param { name = "pulsed", label = "pulsed", default = false, key = "t" }

dipole {
  center      = grid.center,
  orientation = "z",                       -- length = 0: an ideal point dipole
  amplitude   = 7,                         -- peak current (normalized units)
  waveform    = pulsed and "pulse" or "cw",
  pulse_width = pulsed and 0.8 or nil,     -- (nil = key not given)
  interval    = pulsed and 10 or nil,      -- pulse repetition interval (periods)
}

probe    { position = grid.center + vec(24, 0, 0), name = "broadside" }
flux_box { center = grid.center, size = 40, name = "radiated power" }
far_field {}

view { gain = 90, distance = 2.4, yaw = -60, pitch = 22 }
