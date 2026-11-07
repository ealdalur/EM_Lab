-- Phased antenna array
-- A line of centre-fed metal half-wave dipoles spaced lambda/2 along y. A progressive phase shift
-- dphi = -k d sin(theta) between neighbours steers the main beam to angle theta
-- (measured from broadside, +x, toward +y). Amplitude tapers trade beamwidth for
-- lower side lobes.
--
-- Try: , . steer the beam live, ; ' change the element count (beamwidth ~ 1/N),
--      T cycles the taper (watch the side lobes in the far-field polar plot).

scene {
  name        = "Phased Antenna Array",
  description = "Steerable line array of z-dipoles. The far-field monitor reports directivity and beamwidth.",
  size        = {176, 176, 96},
  wavelength  = 18,            -- lambda/2 = 9 cells: element length and spacing land on whole cells
}

local steer = param { name = "steer", label = "steer (deg)", default = 0, min = -60, max = 60, step = 10, keys = {",", "."} }
local n     = param { name = "elements", label = "elements", default = 8, min = 1, max = 16, step = 1, keys = {";", "'"} }
local taper = param { name = "taper", label = "taper", choices = {"uniform", "hann", "binomial"}, key = "t" }

array {
  center      = grid.center,
  elements    = n,
  axis        = "y",
  spacing     = wavelengths(0.5),
  orientation = "z",
  length      = 9,              -- thin-wire half-wave dipoles (9 cells = 0.5 lambda), each fed at its centre gap
  amplitude   = 1,              -- source EMF per element (each element has its own 50-ohm feed)
  steer       = steer,
  taper       = taper,
}

local c = grid.center
probe      { position = c + vec(40, 0, 0), name = "broadside" }
flux_plane { axis = "x", position = c.x + 40, min = {c.y - 12, c.z - 12}, max = {c.y + 12, c.z + 12},
             name = "broadside window" }
far_field  {}

view { gain = 23, distance = 3.0, yaw = -90, pitch = 62, steps_per_frame = 1  }
