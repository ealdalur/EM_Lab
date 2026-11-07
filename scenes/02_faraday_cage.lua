-- Faraday cage
-- A plane wave illuminates a copper mesh box. The mesh pitch is much smaller than the
-- wavelength and the box is smaller than its lowest cavity resonance, so the induced
-- surface currents cancel the field inside: the interior stays (almost) field free.
--
-- Try: G removes the cage (compare the probe / flux numbers), , . change the mesh pitch:
--      once the openings approach lambda/2 the shielding collapses.

scene {
  name        = "Faraday Cage",
  description = "Copper mesh cage (pitch << lambda) shielding its interior from a plane wave. G toggles the cage.",
  size        = {144, 128, 128},
  wavelength  = 36,      -- longer than the cage's lowest cavity-mode cutoff (~2a/sqrt(2))
}

local cage_on = param { name = "cage", label = "cage", default = true, key = "g" }
local pitch   = param { name = "pitch", label = "mesh pitch", default = 4, min = 3, max = 20, step = 1, keys = {",", "."} }

local c    = vec(grid.nx / 2 + 8, grid.ny / 2, grid.nz / 2)
local half = 18

plane_wave { axis = "x", position = 26, polarization = "z", amplitude = 0.6 }

if cage_on then
  cage { min = c - half, max = c + half + 1, spacing = pitch, bar = 2, material = "copper" }
end

probe { position = c + 0.5, name = "inside cage" }
probe { position = c - vec(30, 0, 0), name = "outside" }
flux_plane { axis = "x", position = c.x, min = {c.y - 8, c.z - 8}, max = {c.y + 8, c.z + 8}, name = "cage interior" }

view { gain = 0.9, distance = 2.6, yaw = -35, pitch = 30, steps_per_frame = 1 }
