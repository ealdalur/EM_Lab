-- 8 x 8 microstrip patch phased array (X band, 10 GHz)
-- Sixty-four probe-fed patches on one RT5880 board, each with its own 50-ohm feed. Setting the
-- phase of every feed steers the beam electronically, with no moving parts: the principle of
-- every modern radar, from fighter-jet AESA noses to weather radars and 5G base stations.
--
-- The element is the patch of scene 14 scaled to 10 GHz (lambda = 30 mm = 40 cells, 0.75 mm
-- cells): 8.2 x 12 mm on a 1.5 mm board, its length trimmed one cell below the transmission-
-- line design and the probe 2 cells in from the radiating edge (S11 -26 dB at 10.2 GHz alone).
--
-- Layout: the board stands in the y-z plane near the -x end of the grid and faces +x (its
-- boresight); z is up. The patches resonate along y, so the radiated E field is horizontal
-- (along y). The camera starts on the -y side looking along +y: the array is on the left and
-- the beam goes right; steering in elevation tilts it up the screen.
-- Steering angles, as in scene 16, as seen from behind the array looking out along +x:
-- azimuth positive to the right (toward -y, toward the camera), elevation positive up (+z).
--
-- Things to try:
--   Steering (, . azimuth, ; ' elevation): watch the "Radiation pattern" beam map and the
--     3D view: the beam moves, widens as 1 / cos(angle off boresight) and loses gain (the
--     projected aperture shrinks).
--   Active impedance ("Array elements" window): every element's impedance depends on what its
--     neighbours do (mutual coupling). At broadside the edge elements differ from the centre
--     ones; as the beam scans, the match degrades: the active (scan) VSWR.
--   Taper (G): uniform illumination gives the narrowest beam but -13 dB sidelobes; Hamming and
--     Hann tapers trade beamwidth and gain for much lower sidelobes (radar clutter rejection).
--   Spacing (I): at 0.5 lambda the array can scan anywhere. At 0.7 and 0.9 lambda, grating
--     lobes (copies of the main beam) appear as soon as it scans: d < lambda / (1 + sin(scan)).
--   Elements (U): 4 x 4 vs 8 x 8: twice the size, half the beamwidth, 6 dB more gain.

local lambda0 = 40                                    -- cells per free-space wavelength
local f0      = 10e9

local size_n  = param { name = "elements", label = "array size", choices = {"8x8", "4x4"}, default = "4x4", key = "u" }
local spacing = param { name = "spacing", label = "element spacing (wavelengths)", choices = {"0.5", "0.7", "0.9"}, key = "i" }
local az      = param { name = "azimuth", label = "beam azimuth (deg, + = right)", default = 0, min = -60, max = 60, step = 5, keys = {",", "."} }
local el      = param { name = "elevation", label = "beam elevation (deg, + = up)", default = 0, min = -60, max = 60, step = 5, keys = {";", "'"} }
local taper   = param { name = "taper", label = "amplitude taper", choices = {"uniform", "hamming", "hann"}, key = "g" }

local N      = size_n == "8x8" and 8 or 4
local d      = math.floor(tonumber(spacing) * lambda0 / 2 + 0.5) * 2   -- even, so element centres are integers
local Lp, Wp = 11, 16                                 -- patch length (resonant, along y) and width (along z), cells
local h      = 2                                      -- substrate thickness (1.5 mm)
local inset  = 2                                      -- probe distance from the radiating edge at y0
local margin = 10                                     -- ground / substrate beyond the outer patches
local pml    = 16

local extent = (N - 1) * d + math.max(Lp, Wp) + 2 * margin
local nyz    = extent + 2 * (pml + 8)
nyz = nyz + nyz % 2
-- The box is a cube: 5 wavelengths in front of the board, as wide and as tall (room to see the
-- beam steer in azimuth and elevation). It grows if the array itself needs more room.
local ncube  = math.max(pml + 10 + 2 + 5 * lambda0 + pml + 12, nyz)

scene {
  name        = size_n .. " Patch Phased Array",
  description = "X-band probe-fed patch array: electronic beam steering, active impedance, tapers, grating lobes.",
  size        = {ncube, ncube, ncube},
  cell_size   = 299792458 / (f0 * lambda0),           -- 0.75 mm
  wavelength  = lambda0,
}

material { name = "rt5880", epsilon_r = 2.2, loss_tangent = 0.0009 }

local c  = grid.center
local xg = pml + 10                                   -- ground plane (the board's back face)
local half = (N - 1) * d / 2
local lo = vec(xg, c.y - half - math.floor(Lp / 2) - margin, c.z - half - Wp / 2 - margin)
local hi = vec(xg, c.y + half - math.floor(Lp / 2) + Lp + margin, c.z + half + Wp / 2 + margin)

sheet { min = lo, max = hi }                                                       -- ground
box   { min = lo, max = vec(xg + h, hi.y, hi.z), material = "rt5880" }             -- board

-- Amplitude taper (product of 1-D windows) and steering phase for each element.
local function window(n)
  if taper == "uniform" or N == 1 then return 1 end
  local a = taper == "hamming" and 0.54 or 0.5
  -- Window over N + 2 points with the end zeros dropped, so the edge elements stay driven.
  return a - (1 - a) * math.cos(2 * math.pi * (n + 1) / (N + 1))
end
local k  = 2 * math.pi / lambda0
-- Beam direction (cos el cos az, cos el sin az, sin el): its y and z parts set the phase slope.
local uy = -math.cos(math.rad(el)) * math.sin(math.rad(az))   -- right = -y
local uz = math.sin(math.rad(el))
local scan = math.deg(math.acos(math.cos(math.rad(el)) * math.cos(math.rad(az))))   -- angle off boresight

for i = 0, N - 1 do
  for j = 0, N - 1 do
    local ey, ez = c.y - half + i * d, c.z - half + j * d    -- element centre on the board
    local y0 = ey - math.floor(Lp / 2)
    sheet { min = vec(xg + h, y0, ez - Wp / 2), max = vec(xg + h, y0 + Lp, ez + Wp / 2) }
    local phase = -math.deg(k * ((ey - c.y) * uy + (ez - c.z) * uz))
    monopole {
      base = vec(xg, y0 + inset, ez), orientation = "+x", length = h, radius = mm(0.28),
      frequency = f0, amplitude = window(i) * window(j), phase = phase,
      name = string.format("p[%d,%d]", i + 1, j + 1),
    }
  end
end

far_field { axis = "+x" }                             -- beam maps centred on the boresight
print(string.format("%dx%d array, spacing %d cells (%.2f lambda), beam azimuth %d, elevation %d (%.0f deg off boresight), %s taper",
                    N, N, d, d / lambda0, az, el, scan, taper))
local dmax = d / lambda0
if dmax > 0.5 then
  print(string.format("grating lobes appear beyond %.0f deg off boresight (d = %.2f lambda)",
                      math.deg(math.asin(math.min(1, 1 / dmax - 1))), dmax))
end

view { gain = 16, distance = 2.4, yaw = -90, pitch = 0, steps_per_frame = 4 }    -- from -y, looking along +y (z up)