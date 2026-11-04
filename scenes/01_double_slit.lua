-- Double-slit experiment
-- A plane wave hits a metal wall with two slits. Each slit acts as a line source;
-- their cylindrical wavelets interfere behind the wall. Bright fringes appear where
-- the path difference is a whole number of wavelengths: sin(theta_m) = m * lambda / d.
--
-- Try: change the slit separation (; ') and watch the fringe spacing scale as 1/d,
--      narrow the slits (, .) to see the single-slit envelope widen.

scene {
  name        = "Double-Slit Experiment",
  description = "Plane wave (E along y) on a PEC wall with two slits; interference fringes form behind it.",
  size        = {192, 128, 96},
  wavelength  = 12,
}

local width      = param { name = "slit_width", label = "slit width", default = 6, min = 2, max = 18, step = 1, keys = {",", "."} }
local separation = param { name = "separation", label = "slit separation", default = 36, min = 12, max = 72, step = 4, keys = {";", "'"} }

local wall_x = 64

plane_wave { axis = "x", position = 28, polarization = "y", amplitude = 0.5 }   -- one-way (+x) by default

wall {
  axis = "x", position = wall_x, thickness = 2, material = "pec",
  slits = { count = 2, width = width, spacing = separation, length = 48, direction = "z" },
}

local mid = vec(0, grid.ny / 2, grid.nz / 2)
probe      { position = mid:with("x", wall_x + 62), name = "screen centre" }
flux_plane { axis = "x", position = wall_x + 42, name = "power through slits" }

view { gain = 2.2, distance = 3.0, yaw = -58, pitch = 38 }
