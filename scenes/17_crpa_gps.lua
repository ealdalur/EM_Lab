-- CRPA: an adaptive GPS antenna array that nulls jammers
-- A controlled-reception-pattern antenna (CRPA) is how military and aviation GPS receivers
-- survive jamming. Four RHCP patch elements (2 x 2, half a wavelength apart) look up at the
-- sky. A GPS satellite transmits from high in the box; 1 to 3 jammers sit at random places.
-- The receiver knows where the satellite is (from its almanac) but nothing about the jammers.
--
-- How it works (the "CRPA" window follows along):
--   1. Listening: everything transmits at once, as in the real world. Each patch has two
--      probes; a 90-degree hybrid combines them into one right-hand-circular (RHCP) channel,
--      the polarization GPS uses. The receiver mixes each channel down to complex baseband
--      (I/Q), samples it, adds its own thermal noise, and estimates the covariance R from those
--      snapshots alone. The jammers are linearly polarized, so up to 3 dB of their power is
--      lost to the wrong polarization (the "cross-pol" column). They transmit a few percent off
--      the carrier so that they are independent of each other, like real noise jammers.
--   2. MVDR (minimum-variance distortionless response) weights: w = R^-1 a / (a^H R^-1 a), with
--      a the steering vector toward the satellite's known direction. It keeps unit gain on the
--      satellite and minimises everything else, which puts deep nulls on the jammers. With 4
--      elements the array has 3 degrees of freedom: it can null up to 3 jammers.
--   3. Scoring: the same recording is split per emitter (each is at its own frequency) to show
--      what the weights did to each one. The weights never see that split.
--   4. Adapted: the array transmits with its receive weights. By reciprocity the fields show the
--      receive pattern: a beam toward the satellite (green) and dark nulls on the jammers (red).
--
-- The satellite is 3 wavelengths away rather than 20,000 km, and transmits right-hand circular
-- toward the array. GPS signals arrive about 20 dB below the receiver noise (they are recovered
-- by despreading); jammers are tens of dB above it.
--
-- Things to try:
--   Jammers (J) and their positions (, . new random draw).
--   Satellite azimuth (; ') and elevation (/ \): compass azimuth, clockwise from north (+y).
--   Jammer-to-noise ratio (X): stronger jammers get deeper nulls, the satellite's SINR barely moves.
--   Weights (T): MVDR vs plain beam steering toward the satellite (no nulling) - compare the
--     "after weights" column and the fields.
--   Steering vector (Y): computed from the satellite's direction (a model of the array) or its
--     measured response (a calibrated array): calibration recovers the satellite gain that
--     mutual coupling costs the model.
--   Adaptation (B): blind (above) or ideal: each emitter measured alone and the covariance built
--     as if from unlimited snapshots, which a real receiver cannot do. The two agree to a few dB.
--     The adapted field view is at the carrier, where the off-carrier jammers' nulls are shallower.
--   A jammer close to the satellite's direction cannot be nulled without losing the satellite:
--     the angular resolution of a 1-wavelength array is coarse.

local lambda0 = 35.4                                  -- cells per wavelength (5.4 mm cells)
local f0      = 1575.42e6                             -- GPS L1

local njam  = param { name = "jammers", label = "jammers", choices = {"1", "2", "3"}, default = "2", key = "j" }
local seed  = param { name = "seed", label = "jammer positions (random draw)", default = 1, min = 1, max = 99, step = 1, keys = {",", "."} }
local satAz = param { name = "sat_azimuth", label = "satellite azimuth (deg from north)", default = 45, min = 0, max = 345, step = 15, wrap = true, keys = {";", "'"} }
local satEl = param { name = "sat_elevation", label = "satellite elevation (deg)", default = 60, min = 10, max = 90, step = 10, keys = {"/", "\\"} }
local jnr   = param { name = "jnr", label = "jammer-to-noise (dB)", choices = {"40", "20", "60"}, key = "x" }
local mode  = param { name = "weights", label = "weights", choices = {"mvdr", "steering"}, key = "t" }
local steer = param { name = "steering", label = "satellite steering vector", choices = {"model", "measured"}, key = "y" }
local adapt = param { name = "adapt", label = "adaptation", choices = {"blind", "ideal"}, key = "b" }

local pml   = 16
local R     = 3 * lambda0                             -- satellite distance (the "sky dome")
local nxy   = 272
local zg    = pml + 10                                -- ground plane
local h     = 2                                       -- RT5880 board, 10.8 mm

scene {
  name        = "CRPA: adaptive GPS antenna",
  description = "2x2 RHCP patch array at GPS L1 nulling 1-3 jammers with MVDR while keeping the satellite.",
  size        = {nxy, nxy, zg + h + R + 10 + pml + 6},
  cell_size   = 299792458 / (f0 * lambda0),
  wavelength  = lambda0,
}
material { name = "rt5880", epsilon_r = 2.2, loss_tangent = 0.0009 }

-- The array: four square patches (10 cells = 54 mm) 18 cells (0.51 wavelength) apart, each with
-- an X probe and a Y probe 2 cells in from adjacent edges (the two orthogonal patch modes).
local c      = grid.center
local zt     = zg + h
local d, side, inset = 18, 10, 2
local edge   = d / 2 + side / 2 + 8                   -- ground plane half-width
sheet { min = vec(c.x - edge, c.y - edge, zg), max = vec(c.x + edge, c.y + edge, zg) }
box   { min = vec(c.x - edge, c.y - edge, zg), max = vec(c.x + edge, c.y + edge, zt), material = "rt5880" }

local elements = {}
local k = 0
for _, sy in ipairs({1, -1}) do
  for _, sx in ipairs({-1, 1}) do
    k = k + 1
    local ex, ey = c.x + sx * d / 2, c.y + sy * d / 2
    local x0, y0 = ex - side / 2, ey - side / 2
    sheet { min = vec(x0, y0, zt), max = vec(x0 + side, y0 + side, zt) }
    for _, probe in ipairs({ {"x", vec(x0 + inset, ey, zg)}, {"y", vec(ex, y0 + inset, zg)} }) do
      monopole { base = probe[2], orientation = "+z", length = h, radius = 0.12, frequency = f0, amplitude = 1,
                 name = string.format("E%d%s", k, probe[1]) }
    end
    elements[#elements + 1] = { x = string.format("E%dx", k), y = string.format("E%dy", k), center = vec(ex, ey, zt) }
  end
end
local origin = vec(c.x, c.y, zt)

-- The GPS satellite on the sky dome: compass azimuth (north = +y, east = +x), elevation.
local function sky(az, el, r)
  local a, e = math.rad(az), math.rad(el)
  return origin + vec(math.cos(e) * math.sin(a), math.cos(e) * math.cos(a), math.sin(e)) * r
end
local satPos = sky(satAz, satEl, R)
emitter { name = "GPS", position = satPos, polarization = "rhcp", toward = origin, frequency = f0 }

-- Jammers: random places in the box (seeded, so a draw is repeatable), at least 1.3 wavelengths
-- from the array and clear of the satellite and of each other; random linear polarization.
math.randomseed(seed)
local lo  = vec(pml + 12, pml + 12, zt + 6)
local hi  = vec(grid.nx - pml - 12, grid.ny - pml - 12, grid.nz - pml - 10)
local placed = {}
for j = 1, tonumber(njam) do
  local p
  for _ = 1, 500 do
    p = vec(lo.x + math.random() * (hi.x - lo.x), lo.y + math.random() * (hi.y - lo.y), lo.z + math.random() * (hi.z - lo.z))
    local ok = (p - origin):length() > 1.3 * lambda0 and (p - satPos):length() > 0.8 * lambda0
    for _, q in ipairs(placed) do ok = ok and (p - q):length() > 0.6 * lambda0 end
    if ok then break end
  end
  placed[#placed + 1] = p
  local pol = vec(math.random() - 0.5, math.random() - 0.5, math.random() - 0.5)
  emitter { name = string.format("J%d", j), position = p, polarization = pol, frequency = f0 }
end

crpa {
  satellite = "GPS", elements = elements, jnr = tonumber(jnr), snr = -20,
  weights = mode, steering = steer, adapt = adapt, periods = 36, log = "crpa.csv",
}
far_field { axis = "+z" }

view { gain = 30, distance = 2.3, yaw = -90, pitch = 18, steps_per_frame = 8 }
