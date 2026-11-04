-- Yagi-Uda antenna
-- One driven half-wave dipole plus passive metal rods. The longer reflector behind it
-- and the shorter (capacitive) directors in front re-radiate with phases that add up
-- in the forward (+x) direction: higher directivity and a large front-to-back ratio.
--
-- Element lengths are tuned for these 1x1-cell square PEC rods (equivalent radius
-- ~0.3 cell = lambda/65, i.e. "fat" wires, which resonate shorter than thin ones):
-- reflector 0.5 lambda, directors 0.3 lambda, spacing 0.25 lambda.
-- Rod lengths are whole cells so they voxelise exactly.
--
-- Try: , . add / remove directors and compare D_max and front/back in the xy-cut
--      (expect ~11 dBi and ~19 dB front/back with 4 directors).

scene {
  name        = "Yagi-Uda Antenna",
  description = "Driven dipole + reflector + directors (PEC rods). Far-field monitor shows gain and front/back.",
  size        = {160, 128, 112},
  wavelength  = 20,
}

local directors = param { name = "directors", label = "directors", default = 4, min = 0, max = 6, step = 1, keys = {",", "."} }

local c = vec(60, grid.ny / 2, grid.nz / 2)   -- driven element (integer lattice point)

-- Square PEC rod along z, 1 cell across, `cells` long, centred on c.z.
local function rod(x, cells)
  box { min = vec(x, c.y, c.z - cells / 2), max = vec(x + 1, c.y + 1, c.z + cells / 2), material = "pec" }
end

dipole { center = c, orientation = "z", length = 9, amplitude = 2 }   -- driven element
rod(c.x - 5, 10)                                                    -- reflector, 0.25 lambda behind
for i = 1, directors do
  rod(c.x + 4 + 5 * (i - 1), 6)                                     -- directors, 0.25 lambda apart
end

probe     { position = c + vec(50, 0, 0), name = "forward" }
probe     { position = c - vec(30, 0, 0), name = "backward" }
far_field {}

view { gain = 35, distance = 2.6, yaw = -60, pitch = 35 }
