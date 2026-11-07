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
-- Layout: the array stands in the y-z plane near the -x end of the grid and looks down +x.
-- z is up. Angles are as seen from behind the array looking out along +x: azimuth is the
-- horizontal angle, positive to the right (toward -y); elevation the vertical one, positive up
-- (toward +z). The camera starts on the -y side looking along +y, so range runs to the right of
-- the screen, elevation up, and positive azimuth comes toward the camera.
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
--   Scene depth (U): 5 to 30 wavelengths (15 cm to 90 cm at 10 GHz). The echo weakens as
--     1 / range^4 (12 dB per doubling), which matters once receiver noise is on. The box stays
--     5.6 wavelengths wide, so far away a target fits only a few degrees off boresight.
--   Transmitter (T): the central 2 x 2 elements illuminate the whole field of view at once
--     (all directions imaged from one pulse); the full array concentrates the power ahead.
--   Receiver noise (X): with noise, detection depends on signal-to-noise ratio, and the 64-way
--     coherent sum (18 dB of processing gain) is what pulls the echo out of it.

local lambda0 = 40
local f0      = 10e9

-- Scene depth: how far the grid (and the radar's image) reaches along +x. Each extra
-- wavelength adds 2 M cells (128 MB of GPU memory), and a pulse takes longer to come back, so
-- the time per pulse grows with the square of the depth. First image (calibration + measurement)
-- on an RTX PRO 2000: about 35 s at 5 wavelengths, 70 s at 10, 3.5 min at 20, 4 min or more at 30.
local depth  = param { name = "depth", label = "scene depth (wavelengths)", choices = {"5", "10", "15", "20", "30"}, default = "10", key = "u" }
local D      = tonumber(depth)
local target = param { name = "target", label = "target", choices = {"sphere", "plate", "tilted plate", "two spheres", "missile"}, key = "g" }
local range  = param { name = "range", label = "target range (wavelengths)", default = 5, min = 2, max = D - 0.5,
                       step = D <= 5 and 0.25 or (D <= 10 and 0.5 or 1), keys = {",", "."} }
local az     = param { name = "azimuth", label = "target azimuth (deg, + = right)", default = 0, min = -20, max = 20, step = 5, keys = {";", "'"} }
local el     = param { name = "elevation", label = "target elevation (deg)", default = 0, min = -20, max = 20, step = 5, keys = {"/", "\\"} }
local tx     = param { name = "tx", label = "transmitter", choices = {"centre 2x2", "full array"}, key = "t" }
local noise  = param { name = "noise", label = "receiver noise", choices = {"off", "-50 dB", "-45 dB", "-40 dB"}, key = "x" }
local sep    = param { name = "separation", label = "two spheres: separation (deg)", choices = {"30", "20", "15", "10"}, key = "i" }
local taper  = param { name = "taper", label = "receive taper", choices = {"hamming", "uniform"}, key = "y" }

-- Array (as in scene 15): 8 x 8 patches, 0.5 wavelength apart, on a 1.5 mm RT5880 board.
local N, d   = 8, 20
local Lp, Wp, h, inset, margin, pml = 11, 16, 2, 2, 10, 16
local extent = (N - 1) * d + math.max(Lp, Wp) + 2 * margin
local nyz    = extent + 2 * (pml + 8)
nyz = nyz + nyz % 2
local xg     = pml + 10                               -- ground plane (the board's back face)

scene {
  name        = "Phased-Array Radar",
  description = "8x8 X-band patch array as a pulsed radar: clutter calibration, digital beamforming, detection.",
  size        = {xg + 2 + D * lambda0 + pml + 12, nyz, nyz},
  cell_size   = 299792458 / (f0 * lambda0),
  wavelength  = lambda0,
}
material { name = "rt5880", epsilon_r = 2.2, loss_tangent = 0.0009 }

local c    = grid.center
local half = (N - 1) * d / 2
local lo = vec(xg, c.y - half - math.floor(Lp / 2) - margin, c.z - half - Wp / 2 - margin)
local hi = vec(xg, c.y + half - math.floor(Lp / 2) + Lp + margin, c.z + half + Wp / 2 + margin)
sheet { min = lo, max = hi }
box   { min = lo, max = vec(xg + h, hi.y, hi.z), material = "rt5880" }

for i = 0, N - 1 do
  for j = 0, N - 1 do
    local ey, ez = c.y - half + i * d, c.z - half + j * d
    local y0 = ey - math.floor(Lp / 2)
    sheet { min = vec(xg + h, y0, ez - Wp / 2), max = vec(xg + h, y0 + Lp, ez + Wp / 2) }
    local transmits = tx == "full array" or ((i == 3 or i == 4) and (j == 3 or j == 4))
    monopole {
      base = vec(xg, y0 + inset, ez), orientation = "+x", length = h, radius = mm(0.28),
      frequency = f0, waveform = "pulse", pulse_width = 0.5, amplitude = transmits and 1 or 0,
      name = string.format("p[%d,%d]", i + 1, j + 1),
    }
  end
end

-- Targets, placed by range / azimuth / elevation from the array's centre.
local origin = vec(xg + h, c.y, c.z)
local function at(r, a, e)       -- range (wavelengths), azimuth to the right (-y), elevation up (+z)
  local ra, re = math.rad(a), math.rad(e)
  return origin + vec(math.cos(ra) * math.cos(re), -math.sin(ra) * math.cos(re), math.sin(re)) * (r * lambda0)
end
-- The box is only 224 cells (5.6 wavelengths) wide, so far away a target can only sit a few
-- degrees off boresight before it reaches the absorbing layer: keep it inside.
local halfY, halfZ = 0.75 * lambda0, 0.75 * lambda0                      -- target half-size
if target == "plate" or target == "tilted plate" then halfY, halfZ = lambda0, lambda0 end
if target == "missile" then halfY, halfZ = 1.5 * lambda0, 0.4 * lambda0 end
if target == "two spheres" then
  local spread = range * lambda0 * math.sin(math.rad(tonumber(sep) / 2))
  halfY, halfZ = spread + 0.35 * lambda0, 0.35 * lambda0
end
local room = nyz / 2 - pml - 4                                           -- usable half-width
local r = range * lambda0
local maxEl = math.deg(math.asin(math.max(0, math.min(1, (room - halfZ) / r))))
if math.abs(el) > maxEl then
  warning(string.format("elevation %g deg puts the target outside the box at %g wavelengths: using %+.1f deg", el, range,
                        el > 0 and maxEl or -maxEl))
  el = el > 0 and maxEl or -maxEl
end
local maxAz = math.deg(math.asin(math.max(0, math.min(1, (room - halfY) / (r * math.cos(math.rad(el)))))))
if math.abs(az) > maxAz then
  warning(string.format("azimuth %g deg puts the target outside the box at %g wavelengths: using %+.1f deg", az, range,
                        az > 0 and maxAz or -maxAz))
  az = az > 0 and maxAz or -maxAz
end
if room - halfY < 0 then
  warning("the two spheres are too far apart to fit in the box at this range: reduce the separation (I) or the range")
end

local p = at(range, az, el)
if target == "sphere" then
  sphere { center = p, radius = 0.75 * lambda0, material = "metal", target = true, name = "sphere" }
elseif target == "plate" then
  box { center = p, size = vec(2, 2 * lambda0, 2 * lambda0), material = "metal", target = true, name = "plate" }
elseif target == "tilted plate" then          -- turned 30 degrees about the vertical (z) axis
  box { center = p, size = vec(2, 2 * lambda0, 2 * lambda0), rotate = {0, 0, 30}, material = "metal",
        target = true, name = "tilted plate" }
elseif target == "two spheres" then
  local half = tonumber(sep) / 2
  sphere { center = at(range, az - half, el), radius = 0.35 * lambda0, material = "metal", target = true, name = "sphere A" }
  sphere { center = at(range, az + half, el), radius = 0.35 * lambda0, material = "metal", target = true, name = "sphere B" }
else
  cylinder { center = p, axis = "y", radius = 0.4 * lambda0, length = 3 * lambda0, capped = true,
             material = "metal", target = true, name = "missile" }
end

-- Radar receiver. The element phase centre is the patch centre: 1.5 cells in front of the feed
-- gap (x) and 3.5 cells from the probe along the patch (y).
local noises = { off = nil, ["-50 dB"] = -50, ["-45 dB"] = -45, ["-40 dB"] = -40 }
radar {
  range = {1 * lambda0, (D - 0.25) * lambda0}, fov = 35, noise = noises[noise], threshold = 13, taper = taper,
  phase_center = vec(1.5, 3.5, 0), log = "radar_detections.csv",
}

view { gain = 180, distance = 2.4, yaw = -90, pitch = 0, steps_per_frame = 4 }   -- from -y, looking along +y (z up)
