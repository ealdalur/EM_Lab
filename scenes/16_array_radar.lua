-- Phased-array radar: detecting metal objects with the 8 x 8 X-band patch array
-- The array of scene 15 becomes a pulsed radar. It transmits a short 10 GHz pulse, and every
-- one of its 64 elements receives the echoes. The receiver works like a modern digital array:
--   1. Calibration: a first pulse into the scene WITHOUT the targets records the radar's own
--      response (feed ringing, coupling between elements, the board's reflections). It is
--      subtracted from the real measurement, leaving only the echoes (clutter cancellation).
--      It is reused while only the targets change, so moving a target takes one pulse.
--   2. Digital beamforming: the 64 recorded echoes are delayed and added for every point of
--      a range x azimuth x elevation grid (back-projection, focused even this close in).
--   3. Detection: peaks above the noise floor, with range, azimuth and elevation, compared
--      with where the target really is.
-- The "Radar" window shows the range-azimuth map (B-scope), the angle map at the target's
-- range, the range profile and the raw signal at one element. In 3D: orange boxes are the
-- targets, red crosses the detections.
--
-- Scale: lambda = 30 mm and the targets sit 2 to 4 wavelengths away, a compact-range model of
-- a radar. Everything here is measured in wavelengths, so it scales to real distances; what
-- does not scale is the range resolution, c / 2B = 7 wavelengths for this array's 7% band,
-- larger than this whole scene. That is why angle (the array) does the separating here, and
-- why real radars use much wider-band waveforms.
--
-- Things to try:
--   Target (G): a sphere echoes the same in every direction; a flat plate facing the radar is a
--     mirror (a huge echo); tilt it 30 degrees and the echo goes away: the principle of stealth
--     shaping. Two spheres (separation: I) are resolved while they are further apart than the
--     beamwidth (~19 degrees with the Hamming taper) and merge into one detection below it;
--     the "missile" is a 3-wavelength metal cylinder seen from the side.
--   Receive taper (Y): with uniform weighting the beam is narrower but its -13 dB sidelobes
--     appear as false targets beside a strong one; the Hamming taper suppresses them.
--   Position (, . range, ; ' azimuth, / \ elevation): move the target and read the detection.
--   Transmitter (T): the central 2 x 2 elements illuminate the whole field of view at once
--     (all directions imaged from one pulse); the full array concentrates the power ahead.
--   Receiver noise (X): with noise, detection depends on signal-to-noise ratio, and the 64-way
--     coherent sum (18 dB of processing gain) is what pulls the echo out of it.

local lambda0 = 40
local f0      = 10e9

local target = param { name = "target", label = "target", choices = {"sphere", "plate", "tilted plate", "two spheres", "missile"}, key = "g" }
local range  = param { name = "range", label = "target range (wavelengths)", default = 3, min = 2, max = 4, step = 0.25, keys = {",", "."} }
local az     = param { name = "azimuth", label = "target azimuth (deg)", default = 0, min = -20, max = 20, step = 5, keys = {";", "'"} }
local el     = param { name = "elevation", label = "target elevation (deg)", default = 0, min = -20, max = 20, step = 5, keys = {"/", "\\"} }
local tx     = param { name = "tx", label = "transmitter", choices = {"centre 2x2", "full array"}, key = "t" }
local noise  = param { name = "noise", label = "receiver noise", choices = {"off", "-50 dB", "-45 dB", "-40 dB"}, key = "x" }
local sep    = param { name = "separation", label = "two spheres: separation (deg)", choices = {"30", "20", "15", "10"}, key = "i" }
local taper  = param { name = "taper", label = "receive taper", choices = {"hamming", "uniform"}, key = "y" }

-- Array (as in scene 15): 8 x 8 patches, 0.5 wavelength apart, on a 1.5 mm RT5880 board.
local N, d   = 8, 20
local Lp, Wp, h, inset, margin, pml = 11, 16, 2, 2, 10, 16
local extent = (N - 1) * d + math.max(Lp, Wp) + 2 * margin
local nxy    = extent + 2 * (pml + 8)
nxy = nxy + nxy % 2
local zg     = pml + 10

scene {
  name        = "Phased-Array Radar",
  description = "8x8 X-band patch array as a pulsed radar: clutter calibration, digital beamforming, detection.",
  size        = {nxy, nxy, zg + 2 + 5 * lambda0 + pml + 12},
  cell_size   = 299792458 / (f0 * lambda0),
  wavelength  = lambda0,
}
material { name = "rt5880", epsilon_r = 2.2, loss_tangent = 0.0009 }

local c    = grid.center
local half = (N - 1) * d / 2
local lo = vec(c.x - half - math.floor(Lp / 2) - margin, c.y - half - Wp / 2 - margin, zg)
local hi = vec(c.x + half - math.floor(Lp / 2) + Lp + margin, c.y + half + Wp / 2 + margin, zg)
sheet { min = lo, max = hi }
box   { min = lo, max = vec(hi.x, hi.y, zg + h), material = "rt5880" }

for i = 0, N - 1 do
  for j = 0, N - 1 do
    local ex, ey = c.x - half + i * d, c.y - half + j * d
    local x0 = ex - math.floor(Lp / 2)
    sheet { min = vec(x0, ey - Wp / 2, zg + h), max = vec(x0 + Lp, ey + Wp / 2, zg + h) }
    local transmits = tx == "full array" or ((i == 3 or i == 4) and (j == 3 or j == 4))
    monopole {
      base = vec(x0 + inset, ey, zg), orientation = "+z", length = h, radius = mm(0.28),
      frequency = f0, waveform = "pulse", pulse_width = 0.5, amplitude = transmits and 1 or 0,
      name = string.format("p[%d,%d]", i + 1, j + 1),
    }
  end
end

-- Targets, placed by range / azimuth / elevation from the array's centre.
local origin = vec(c.x, c.y, zg + h)
local function at(r, a, e)
  local ra, re = math.rad(a), math.rad(e)
  return origin + vec(math.sin(ra) * math.cos(re), math.sin(re), math.cos(ra) * math.cos(re)) * (r * lambda0)
end
local p = at(range, az, el)
if target == "sphere" then
  sphere { center = p, radius = 0.75 * lambda0, material = "metal", target = true, name = "sphere" }
elseif target == "plate" then
  box { center = p, size = vec(2 * lambda0, 2 * lambda0, 2), material = "metal", target = true, name = "plate" }
elseif target == "tilted plate" then
  box { center = p, size = vec(2 * lambda0, 2 * lambda0, 2), rotate = {0, 30, 0}, material = "metal",
        target = true, name = "tilted plate" }
elseif target == "two spheres" then
  local half = tonumber(sep) / 2
  sphere { center = at(range, az - half, el), radius = 0.35 * lambda0, material = "metal", target = true, name = "sphere A" }
  sphere { center = at(range, az + half, el), radius = 0.35 * lambda0, material = "metal", target = true, name = "sphere B" }
else
  cylinder { center = p, axis = "x", radius = 0.4 * lambda0, length = 3 * lambda0, capped = true,
             material = "metal", target = true, name = "missile" }
end

-- Radar receiver. The element phase centre is the patch centre: 3.5 cells from the probe along
-- x, 1.5 cells up from the feed gap.
local noises = { off = nil, ["-50 dB"] = -50, ["-45 dB"] = -45, ["-40 dB"] = -40 }
radar {
  range = {1 * lambda0, 5 * lambda0}, fov = 35, noise = noises[noise], threshold = 13, taper = taper,
  phase_center = vec(3.5, 0, 1.5), log = "radar_detections.csv",
}

view { gain = 40, distance = 2.3, yaw = -40, pitch = 20, steps_per_frame = 16 }
