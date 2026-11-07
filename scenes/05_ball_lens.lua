-- Dielectric ball lens
-- Light (here: a 14-cell wave) travels slower inside a dielectric (v = c / sqrt(eps_r)),
-- so the wavefronts passing through the centre of the sphere are delayed the most and
-- the transmitted wave converges. Paraxial focus behind the centre: f = n R / (2 (n - 1)).
--
-- Try: , . change eps_r. The probe and the 12x12 flux window follow the predicted focus.

scene {
  name        = "Dielectric Ball Lens",
  description = "Plane wave focused by a dielectric sphere; the probe tracks the paraxial focus.",
  size        = {192, 128, 128},
  wavelength  = 14,      -- 14 / sqrt(2.5) = 8.9 cells per wavelength inside the lens
}

local eps = param { name = "eps_r", label = "eps_r", default = 2.5, min = 1.0, max = 6.0, step = 0.5, keys = {",", "."} }

local R = 24
local c = vec(80, grid.ny / 2, grid.nz / 2)

sphere     { center = c, radius = R, material = { name = "lens", epsilon_r = eps } }
plane_wave { axis = "x", position = 26, polarization = "z", amplitude = 0.5 }

local n  = math.sqrt(eps)
local f  = (n > 1.0001) and n * R / (2 * (n - 1)) or 4 * R
local fx = math.min(c.x + f, grid.nx - grid.pml - 4)
print(string.format("ball lens: n = %.3f, paraxial focus %.1f cells behind the centre", n, f))

probe      { position = c:with("x", fx), name = "focus" }
flux_plane { axis = "x", position = fx, min = {c.y - 6, c.z - 6}, max = {c.y + 6, c.z + 6}, name = "focal spot" }

view { gain = 1.0, distance = 3.0, yaw = -55, pitch = 30, steps_per_frame = 1  }
