-- Pyramidal horn antenna
-- A rectangular waveguide (TE10 mode, fed by a coax-style probe: a short metal monopole
-- rising from the bottom wall a quarter guide-wavelength in front of the shorted back wall)
-- flares into a horn. The large aperture forms a narrow beam:
-- directivity ~ e_ap * 4 pi A B / lambda^2 with aperture efficiency e_ap ~ 0.5.
--
-- The horn is built procedurally with shape{}: a point is metal if it lies within
-- `t` cells outside the (linearly flaring) inner cross-section.
--
-- Try: , . change the flare length (longer = less phase error across the aperture).

scene {
  name        = "Pyramidal Horn Antenna",
  description = "Waveguide-fed pyramidal horn built with a procedural shape{}; far-field shows its narrow beam.",
  size        = {160, 112, 96},
  wavelength  = 16,
}

local flare = param { name = "flare", label = "flare length", default = 40, min = 20, max = 60, step = 10, keys = {",", "."} }

local lambda = grid.wavelength
local a, b   = 14, 7          -- waveguide inner width (y, broad wall) and height (z)
local A, B   = 40, 30         -- horn aperture
local Lg     = 16             -- waveguide section length
local t      = 1              -- wall thickness
local x0     = 50             -- throat (waveguide -> horn transition)
local cy, cz = 56, 48

local function horn_wall(x, y, z)
  local u = x - x0
  if u < -Lg or u > flare then return false end
  local s  = math.max(u, 0) / flare
  local hy = a / 2 + (A - a) / 2 * s
  local hz = b / 2 + (B - b) / 2 * s
  local dy, dz = math.abs(y - cy), math.abs(z - cz)
  if dy > hy + t or dz > hz + t then return false end
  if u < -Lg + t then return true end          -- shorted back wall
  return dy >= hy or dz >= hz                  -- side walls
end

shape {
  min = vec(x0 - Lg, cy - A / 2 - t, cz - B / 2 - t),
  max = vec(x0 + flare + 1, cy + A / 2 + t + 1, cz + B / 2 + t + 1),
  inside = horn_wall, material = "pec",
}

-- TE10 guide wavelength; the probe stands lambda_g / 4 in front of the short, on the inner
-- surface of the bottom wall (z = cz - b/2 rounded to the lattice), reaching half-way up.
local lambda_g = lambda / math.sqrt(1 - (lambda / (2 * a)) ^ 2)
local floor_z  = math.floor(cz - b / 2 + 0.5)
monopole { base = vec(x0 - Lg + t + lambda_g / 4, cy, floor_z), orientation = "+z", length = 4, amplitude = 1 }   -- thin-wire probe, 50-ohm feed

probe     { position = vec(x0 + flare + 30, cy, cz), name = "boresight" }
far_field {}

view { gain = 12, distance = 2.6, yaw = -50, pitch = 30 }
