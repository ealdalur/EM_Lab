-- Monopole antenna: ground plane vs counterpoise
-- A quarter-wave monopole is half of a dipole: a single thin wire fed against a ground. It
-- is unbalanced, so the feed's return current has to flow somewhere; the ground supplies the
-- "missing half". Over a large ground plane, currents induced in the plane act as a mirror
-- image of the wire, so the antenna behaves like a dipole whose lower half is the reflection:
-- the same doughnut, only above the ground, with twice the directivity and half the feed
-- resistance (theory over an infinite plane: 5.15 dBi at the horizon, ~36 ohm).
--
-- Compare (G cycles through the grounds); all are fed by the same 50-ohm source:
--   plate4    4 x 4 lambda square plate: close to an "infinite" ground plane, but finite plates
--             lift the beam above the horizon.
--   plate2    2 x 2 lambda plate: the beam tilts up further, more radiation leaks under the plate.
--   plate1    1 x 1 lambda plate: the ground is small; the pattern approaches a dipole's.
--   radials   counterpoise of four horizontal quarter-wave thin-wire radials (a "ground-plane
--             antenna"); textbook feed resistance ~20-25 ohm.
--   drooping  four radials sloping 45 degrees down, which raises the feed resistance toward
--             50 ohm (built as metal staircases, so treat it as approximate: diagonal
--             conductors can only be staircased on the grid).
--   dipole    reference: a free-space centre-fed half-wave dipole (needs no ground).
--
-- Watch the port line (Z, VSWR, accepted power), D_max and the elevation of the peak
-- (theta in the far-field line; 90 = horizon).

scene {
  name        = "Monopole: Ground Plane vs Counterpoise",
  description = "Quarter-wave thin-wire monopole over plates and radials, plus a dipole reference.",
  size        = {144, 144, 144},
  wavelength  = 20,
}

local ground = param { name = "ground", label = "ground",
                       choices = {"plate4", "plate2", "plate1", "radials", "drooping", "dipole"}, key = "g" }

local L       = grid.wavelength
local c       = grid.center              -- feed point: a lattice node on the ground surface
local quarter = L / 4                    -- 5 cells

-- Square ground plate whose top face is at z = c.z (one cell thick).
local function plate(side)
  local h = side / 2
  box { min = vec(c.x - h, c.y - h, c.z - 1), max = vec(c.x + h + 1, c.y + h + 1, c.z), material = "metal" }
end

-- Four horizontal quarter-wave thin-wire radials from the feed point.
local function horizontal_radials()
  for _, d in ipairs({X, -X, Y, -Y}) do
    wire { from = c, to = c + d * quarter }
  end
end

-- Four quarter-wave radials sloping 45 degrees down from a metal hub cell, built as
-- face-connected metal staircases (cells within 0.8 cell of each radial's axis).
local function drooping_radials()
  box { min = vec(c.x - 1, c.y - 1, c.z - 1), max = vec(c.x + 1, c.y + 1, c.z), material = "metal" }   -- hub
  local start = c + vec(0, 0, -0.5)
  for _, d in ipairs({vec(1, 0, -1), vec(-1, 0, -1), vec(0, 1, -1), vec(0, -1, -1)}) do
    local dir = d:normalize()
    local tip = start + dir * quarter
    shape {
      min = vec(math.min(start.x, tip.x) - 1, math.min(start.y, tip.y) - 1, tip.z - 1),
      max = vec(math.max(start.x, tip.x) + 1, math.max(start.y, tip.y) + 1, start.z + 1),
      material = "metal",
      inside = function(x, y, z)
        local p = vec(x, y, z) - start
        local t = clamp(p:dot(dir), 0, quarter)
        return (p - dir * t):length() <= 0.8
      end,
    }
  end
end

if ground == "dipole" then
  dipole { center = c, orientation = "z", length = 9, amplitude = 1 }          -- 0.45 lambda
else
  if     ground == "plate4"   then plate(4 * L)
  elseif ground == "plate2"   then plate(2 * L)
  elseif ground == "plate1"   then plate(1 * L)
  elseif ground == "radials"  then horizontal_radials()
  elseif ground == "drooping" then drooping_radials() end
  monopole { base = c, orientation = "+z", length = quarter, amplitude = 1 }  -- 50-ohm feed at the base
end

probe    { position = c + vec(40, 0, 0),   name = "horizon" }
probe    { position = c + vec(28, 0, 28),  name = "45 deg up" }
probe    { position = c + vec(28, 0, -28), name = "45 deg down" }
flux_box { center = c, size = 96, name = "radiated power" }
far_field {}

view { gain = 45, distance = 2.4, yaw = -60, pitch = 15, steps_per_frame = 1 }
