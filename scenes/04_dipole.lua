-- Oscillating dipole (an "oscillating electron")
-- A point current J_z = I sin(wt) radiates the classic doughnut pattern:
-- maximum broadside (theta = 90 deg), nulls along the dipole axis.
-- Theory: directivity D = 1.5 (1.76 dBi), HPBW = 90 deg in the elevation cuts.

scene {
  name        = "Oscillating Dipole",
  description = "Hertzian dipole: closed flux box = radiated power; far-field monitor = pattern and directivity.",
  size        = {128, 128, 128},
  wavelength  = 16,                        -- cells per wavelength (resolution)
}

local pulsed = param { name = "pulsed", label = "pulsed", default = false, key = "t" }

dipole {
  center      = grid.center,
  orientation = "z",
  amplitude   = 7,                         -- peak current (normalised units)
  waveform    = pulsed and "pulse" or "cw",
  pulse_width = pulsed and 0.8 or nil,     -- (nil = key not given)
  interval    = pulsed and 10 or nil,      -- pulse repetition interval (periods)
}

probe    { position = grid.center + vec(24, 0, 0), name = "broadside" }
flux_box { center = grid.center, size = 40, name = "radiated power" }
far_field {}

view { gain = 90, distance = 2.4, yaw = -60, pitch = 22 }
