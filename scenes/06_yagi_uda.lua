-- Yagi-Uda antenna
-- One driven half-wave dipole plus passive thin metal wires. The longer reflector behind
-- it and the shorter directors in front pick up currents from the driven element and
-- re-radiate with phases that add up in the forward (+x) direction: higher directivity
-- and a large front-to-back ratio.
--
-- Element lengths follow textbook proportions for thin wires: reflector 0.55 lambda,
-- driven 0.45 lambda, directors 0.45 lambda spaced 0.3 lambda.
-- Measured with 4 directors: ~10.9 dBi, ~24 dB front/back.
--
-- The port line shows the feed impedance (~35 - j70 ohm): parasitic elements pull a
-- Yagi's impedance well below 50 ohm, which is why real Yagis use a matching section
-- (gamma match, hairpin). Try the driven-element length (; ') to see the reactance flip.
--
-- Try: , . add / remove directors and compare D_max, front/back and the feed impedance.

scene {
  name        = "Yagi-Uda Antenna",
  description = "Driven dipole + reflector + directors (thin wires). Gain, front/back and feed impedance.",
  size        = {160, 128, 112},
  wavelength  = 20,
}

local directors = param { name = "directors", label = "directors", default = 4, min = 0, max = 6, step = 1, keys = {",", "."} }
local driven    = param { name = "driven", label = "driven length", default = 9, min = 7, max = 13, step = 2, keys = {";", "'"} }

local c = vec(60, grid.ny / 2, grid.nz / 2)   -- driven element (a lattice node)

-- Passive thin metal element along z, `cells` long, centred at height c.z.
local function element(x, cells)
  wire { from = vec(x, c.y, c.z - cells / 2), to = vec(x, c.y, c.z + cells / 2) }
end

dipole { center = c, orientation = "z", length = driven, amplitude = 1 }   -- driven element, 50-ohm feed
element(c.x - 5, 11)                                                     -- reflector, 0.25 lambda behind
for i = 1, directors do
  element(c.x + 4 + 6 * (i - 1), 9)                                      -- directors, 0.3 lambda apart
end

probe     { position = c + vec(50, 0, 0), name = "forward" }
probe     { position = c - vec(30, 0, 0), name = "backward" }
far_field {}

view { gain = 60, distance = 2.6, yaw = -60, pitch = 35 }
