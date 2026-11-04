-- Wire-grid polarizer
-- Thin parallel wires spaced well below a wavelength short out the electric field
-- component parallel to them (reflected) while the perpendicular component passes.
--
-- Try: X switches the incident polarization between parallel (z, blocked) and
--      perpendicular (y, transmitted) to the wires; G removes the grid for reference.
--      Compare the "transmitted" flux.

scene {
  name        = "Wire-Grid Polarizer",
  description = "PEC wires along z, pitch lambda/6: E parallel to the wires is reflected, perpendicular passes.",
  size        = {160, 96, 96},
  wavelength  = 24,
}

local pol     = param { name = "polarization", label = "E polarization", choices = {"z", "y"}, key = "x" }
local grid_on = param { name = "grid", label = "wire grid", default = true, key = "g" }

plane_wave { axis = "x", position = 28, polarization = pol, amplitude = 0.5 }

if grid_on then
  grating { axis = "x", position = 70, thickness = 1, period = 4, width = 1, direction = "z", material = "pec" }
end

probe      { position = vec(110, grid.ny / 2, grid.nz / 2), name = "behind grid" }
flux_plane { axis = "x", position = 100, name = "transmitted" }

view { gain = 1.8, distance = 2.8, yaw = -40, pitch = 25 }
