-- Right-angle prism: refraction and total internal reflection
-- A Gaussian beam enters a 45-90-45 prism and meets the hypotenuse at 45 degrees.
-- Total internal reflection happens when n > 1 / sin(45 deg) = 1.414 (eps_r > 2): the
-- beam is turned 90 degrees and leaves through the bottom face. Below that, most of it
-- refracts out through the hypotenuse.
--
-- The prism is a procedural shape{}. Try: , . change eps_r across the TIR threshold and
-- compare the two flux monitors.

scene {
  name        = "Prism: Total Internal Reflection",
  description = "Gaussian beam in a right-angle prism; TIR above eps_r = 2.",
  size        = {176, 176, 104},
  wavelength  = 12,
}

local eps = param { name = "eps_r", label = "eps_r", default = 2.25, min = 1.25, max = 4.0, step = 0.25, keys = {",", "."} }

local S      = 72                  -- leg length
local x0, y0 = 50, 64              -- right-angle corner
local cz     = grid.nz / 2

shape {
  min = vec(x0, y0, cz - 32), max = vec(x0 + S, y0 + S, cz + 32),
  inside = function(x, y, z) return (x - x0) + (y - y0) <= S end,
  material = { name = "prism glass", epsilon_r = eps },
}

-- A wide waist keeps the beam's angular spread (lambda / (pi w0) ~ 16 deg) inside the TIR range.
local beam_y = y0 + 0.45 * S
gaussian_beam { axis = "x", position = 28, center = vec(0, beam_y, cz), waist = 14, polarization = "z", amplitude = 0.6 }

flux_plane { axis = "x", position = x0 + S + 16, name = "refracted (through hypotenuse)" }
flux_plane { axis = "y", position = y0 - 14,     name = "TIR output (bottom face)" }
probe      { position = vec(x0 + S + 0.5 - (beam_y - y0), y0 - 20, cz), name = "below prism" }

view { gain = 4.5, distance = 2.4, yaw = -90, pitch = 70 }
