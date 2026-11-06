-- 8 x 8 microstrip patch phased array (X band, 10 GHz)
-- Sixty-four probe-fed patches on one RT5880 board, each with its own 50-ohm feed. Setting the
-- phase of every feed steers the beam electronically, with no moving parts: the principle of
-- every modern radar, from fighter-jet AESA noses to weather radars and 5G base stations.
--
-- The element is the patch of scene 14 scaled to 10 GHz (lambda = 30 mm = 40 cells, 0.75 mm
-- cells): 8.2 x 12 mm on a 1.5 mm board, its length trimmed one cell below the transmission-
-- line design and the probe 2 cells in from the radiating edge (S11 -26 dB at 10.2 GHz alone).
--
-- Things to try:
--   Steering (, . and ; '): scan angle theta from broadside and the scan plane phi. Watch the
--     "Radiation pattern" beam map: the beam moves, widens as 1 / cos(theta) and loses gain
--     (the projected aperture shrinks).
--   Active impedance ("Array elements" window): every element's impedance depends on what its
--     neighbours do (mutual coupling). At broadside the edge elements differ from the centre
--     ones; as the beam scans, the match degrades: the active (scan) VSWR.
--   Taper (G): uniform illumination gives the narrowest beam but -13 dB sidelobes; Hamming and
--     Hann tapers trade beamwidth and gain for much lower sidelobes (radar clutter rejection).
--   Spacing (I): at 0.5 lambda the array can scan anywhere. At 0.7 and 0.9 lambda, grating
--     lobes (copies of the main beam) appear as soon as it scans: d < lambda / (1 + sin theta).
--   Elements (U): 4 x 4 vs 8 x 8: twice the size, half the beamwidth, 6 dB more gain.

local lambda0 = 40                                    -- cells per free-space wavelength
local f0      = 10e9

local size_n  = param { name = "elements", label = "array size", choices = {"8x8", "4x4"}, key = "u" }
local spacing = param { name = "spacing", label = "element spacing (wavelengths)", choices = {"0.5", "0.7", "0.9"}, key = "i" }
local theta   = param { name = "theta", label = "scan angle theta (deg)", default = 0, min = 0, max = 60, step = 5, keys = {",", "."} }
local phi     = param { name = "phi", label = "scan plane phi (deg)", default = 0, min = 0, max = 345, step = 15, wrap = true, keys = {";", "'"} }
local taper   = param { name = "taper", label = "amplitude taper", choices = {"uniform", "hamming", "hann"}, key = "g" }

local N      = size_n == "8x8" and 8 or 4
local d      = math.floor(tonumber(spacing) * lambda0 / 2 + 0.5) * 2   -- even, so element centres are integers
local Lp, Wp = 11, 16                                 -- patch length (resonant, along x) and width, cells
local h      = 2                                      -- substrate thickness (1.5 mm)
local inset  = 2                                      -- probe distance from the radiating edge at x0
local margin = 10                                     -- ground / substrate beyond the outer patches
local pml    = 16

local extent = (N - 1) * d + math.max(Lp, Wp) + 2 * margin
local nxy    = extent + 2 * (pml + 8)
nxy = nxy + nxy % 2

scene {
  name        = "8x8 Patch Phased Array",
  description = "X-band probe-fed patch array: electronic beam steering, active impedance, tapers, grating lobes.",
  size        = {nxy, nxy, 96},
  cell_size   = 299792458 / (f0 * lambda0),           -- 0.75 mm
  wavelength  = lambda0,
}

material { name = "rt5880", epsilon_r = 2.2, loss_tangent = 0.0009 }

local c  = grid.center
local zg = pml + 10                                   -- ground plane
local half = (N - 1) * d / 2
local lo = vec(c.x - half - math.floor(Lp / 2) - margin, c.y - half - Wp / 2 - margin, zg)
local hi = vec(c.x + half - math.floor(Lp / 2) + Lp + margin, c.y + half + Wp / 2 + margin, zg)

sheet { min = lo, max = hi }                                                       -- ground
box   { min = lo, max = vec(hi.x, hi.y, zg + h), material = "rt5880" }             -- board

-- Amplitude taper (product of 1-D windows) and steering phase for each element.
local function window(n)
  if taper == "uniform" or N == 1 then return 1 end
  local a = taper == "hamming" and 0.54 or 0.5
  -- Window over N + 2 points with the end zeros dropped, so the edge elements stay driven.
  return a - (1 - a) * math.cos(2 * math.pi * (n + 1) / (N + 1))
end
local k  = 2 * math.pi / lambda0
local st = math.sin(math.rad(theta))
local ux, uy = st * math.cos(math.rad(phi)), st * math.sin(math.rad(phi))

for i = 0, N - 1 do
  for j = 0, N - 1 do
    local ex, ey = c.x - half + i * d, c.y - half + j * d    -- element centre
    local x0 = ex - math.floor(Lp / 2)
    sheet { min = vec(x0, ey - Wp / 2, zg + h), max = vec(x0 + Lp, ey + Wp / 2, zg + h) }
    local phase = -math.deg(k * ((ex - c.x) * ux + (ey - c.y) * uy))
    monopole {
      base = vec(x0 + inset, ey, zg), orientation = "+z", length = h, radius = mm(0.28),
      frequency = f0, amplitude = window(i) * window(j), phase = phase,
      name = string.format("p[%d,%d]", i + 1, j + 1),
    }
  end
end

far_field {}
print(string.format("%dx%d array, spacing %d cells (%.2f lambda), steering theta %d, phi %d, %s taper",
                    N, N, d, d / lambda0, theta, phi, taper))
local dmax = d / lambda0
if dmax > 0.5 then
  print(string.format("grating lobes appear beyond theta = %.0f deg (d = %.2f lambda)",
                      math.deg(math.asin(math.min(1, 1 / dmax - 1))), dmax))
end

view { gain = 12, distance = 2.0, yaw = -35, pitch = 30, steps_per_frame = 8 }
