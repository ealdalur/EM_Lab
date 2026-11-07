-- Planar (2-D) phased array with a reflecting ground plane
-- A 4 x 4 grid of z-dipoles in the y-z plane, a quarter wavelength in front of a PEC
-- plate: the plate's image currents cancel the backward radiation, so the array beams
-- into +x only. Steer in azimuth and elevation independently.
--
-- Try: , . steer in azimuth (positive to the right, seen from behind the array looking out
--      along +x: toward -y), ; ' steer in elevation (positive up, +z),
--      G removes the ground plane (the beam becomes bidirectional: front/back -> 0 dB).

scene {
  name        = "Planar Array with Ground Plane",
  description = "4x4 dipole array over a PEC reflector; 2-D beam steering with the far-field monitor.",
  size        = {160, 176, 176},
  wavelength  = 18,            -- lambda/2 = 9 cells
}

local az     = param { name = "azimuth", label = "steer az (deg, + = right)", default = 0, min = -50, max = 50, step = 10, keys = {",", "."} }
local el     = param { name = "elevation", label = "steer el (deg)", default = 0, min = -50, max = 50, step = 10, keys = {";", "'"} }
local ground = param { name = "ground", label = "ground plane", default = true, key = "g" }

local L = grid.wavelength
local c = grid.center

array {
  center = c, elements = {4, 4}, axis = {"y", "z"}, spacing = {9, 13},   -- 0.5 and ~0.72 lambda
  orientation = "z", length = 9, amplitude = 1, steer = {-az, el},       -- steer is toward +y, +z: right = -y
}

if ground then
  box { center = c - vec(0.25 * L, 0, 0), size = vec(1, 4.5 * L, 4.5 * L), material = "pec" }
end

far_field { axis = "+x" }                        -- beam maps centred on the boresight
view { gain = 10, distance = 2.6, yaw = -40, pitch = 25, steps_per_frame = 1 }
