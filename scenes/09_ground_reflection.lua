-- Antenna over ground: two-ray interference
-- The wave reflected by the ground interferes with the direct wave, splitting the
-- vertical pattern into lobes. The number of lobes grows with height (h / lambda), and
-- the reflection strength depends on the ground material (PEC > concrete > dry soil).
--
-- Try: , . change the antenna height, G cycles the ground material.
--      The column of probes at x + 40 samples the "height-gain" curve (press P).

scene {
  name        = "Dipole over Ground (multipath)",
  description = "Horizontal dipole above a ground half-space; a vertical column of probes shows the lobing.",
  size        = {144, 112, 128},
  wavelength  = 20,
}

local height = param { name = "height", label = "height (cells)", default = 30, min = 10, max = 60, step = 5, keys = {",", "."} }
local ground = param { name = "ground", label = "ground", choices = {"pec", "concrete", "ground_dry", "none"}, key = "g" }

local zg = 24                                        -- ground surface
local c  = vec(grid.nx / 2 - 20, grid.ny / 2, zg + height)

if ground ~= "none" then
  box { min = vec(0, 0, 0), max = vec(grid.nx, grid.ny, zg), material = ground }
end

dipole { center = c, orientation = "y", length = 9, amplitude = 1 }   -- thin-wire half-wave dipole, 50-ohm feed

for z = zg + 4, grid.nz - grid.pml - 4, 8 do
  probe { position = vec(c.x + 40, c.y, z), name = string.format("z=%d", z) }
end

view { gain = 40, distance = 2.6, yaw = -90, pitch = 8, mode = "fields", steps_per_frame = 1 }
