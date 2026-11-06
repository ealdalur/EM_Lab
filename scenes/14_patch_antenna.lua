-- Microstrip patch antenna (probe fed)
-- A copper rectangle on a dielectric board above a ground plane, fed from below by a coax
-- probe through the board. It resonates when its length is about half a wavelength in the
-- substrate (minus the fringing-field extension of each edge); the two open edges radiate
-- in phase, so the beam points broadside (+z, away from the ground).
--
-- The patch is sized for 2.4 GHz with the standard transmission-line (TL) model (computed
-- below and printed when the scene loads). The board is 0.05 wavelength thick (6.25 mm):
-- thick boards widen the bandwidth and are resolved by 3 grid cells here.
--
-- What the full-wave simulation shows (rt5880): the patch resonates at ~2.21 GHz, 8% below the
-- TL prediction (the TL model's fringing correction is too small for a board this thick; the
-- result is grid-converged: twice the resolution gives 2.20 GHz). That is why real patches are
-- trimmed in a field solver. The coax pin adds a series inductance (+j55 ohm here), so the
-- best match sits ~4% above the resonance and moves down as the feed goes deeper. The defaults
-- are the untrimmed TL design (best match 2.31 GHz); the tuned design is trim -1, inset 3:
-- S11 -20 dB at 2.44 GHz with a -10 dB band of 2.37-2.52 GHz, covering the whole 2.400-2.4835
-- GHz ISM band.
--
-- Modes (T):
--   sweep  a short pulse drives the probe; one run yields Z, S11 and VSWR from 1.9 to
--          2.9 GHz. The S11 curve is drawn bottom-right (0 dB top, -30 dB bottom, dashed line
--          -10 dB, green line = 2.4 GHz). Allow ~80 periods (~1200 frames) for the ring-down.
--   cw     2.4 GHz continuous wave: impedance at the design frequency, the broadside
--          radiation pattern, directivity and radiation efficiency (substrate loss).
-- Feed inset (, .): the resistance at resonance falls from ~190 ohm with the probe 1 cell in
--          from the radiating edge to ~40 ohm at 6 cells (TL model: R_edge cos^2(pi d / L));
--          around 3-4 cells it matches 50 ohm. That is how probe-fed patches are matched.
-- Length trim (; '): patch length in cells relative to the TL design.
-- Substrate (X): rt5880 (eps_r 2.2, low loss, 99% efficient).
--          fr4 (eps_r 4.4, tan d 0.02): smaller patch; tuned at trim -1, inset 1 (S11 -26 dB at
--          2.41 GHz). Its -10 dB band looks wider (8%), but only because the loss lowers the Q:
--          in cw mode the radiation efficiency drops to ~80%, the rest heats the board.
--          foam (eps_r 1.07): the largest patch, and the TL model is nearly right in air
--          (S11 -29 dB at 2.40 GHz untrimmed).
-- Sweep vs cw cross-check: the sweep's 2.4 GHz point and the cw measurement agree to 0.1%.

local lambda0 = 60                                    -- cells per free-space wavelength at 2.4 GHz
local f0      = 2.4e9

scene {
  name        = "Microstrip Patch Antenna",
  description = "Probe-fed patch on a dielectric board: frequency sweep (S11 / bandwidth), feed matching, pattern.",
  size        = {144, 144, 112},
  cell_size   = 299792458 / (f0 * lambda0),           -- 2.08 mm
  wavelength  = lambda0,
}

local mode      = param { name = "mode", label = "mode", choices = {"sweep", "cw"}, key = "t" }
local substrate = param { name = "substrate", label = "substrate", choices = {"rt5880", "fr4", "foam"}, key = "x" }
local inset     = param { name = "inset", label = "feed inset (cells)", default = 4, min = 1, max = 10, step = 1, keys = {",", "."} }
local trim      = param { name = "trim", label = "length trim (cells)", default = 0, min = -4, max = 4, step = 1, keys = {";", "'"} }

material { name = "rt5880", epsilon_r = 2.2,  loss_tangent = 0.0009 }   -- PTFE / glass microfibre laminate
material { name = "foam",   epsilon_r = 1.07, loss_tangent = 0.0001 }
local eps = ({ rt5880 = 2.2, fr4 = 4.4, foam = 1.07 })[substrate]

-- Transmission-line design (all lengths in cells).
local h      = 3                                                            -- substrate thickness
local W      = lambda0 / 2 * math.sqrt(2 / (eps + 1))                       -- patch width
local eeff   = (eps + 1) / 2 + (eps - 1) / 2 / math.sqrt(1 + 12 * h / W)     -- effective permittivity
local dL     = 0.412 * h * (eeff + 0.3) * (W / h + 0.264) / ((eeff - 0.258) * (W / h + 0.8))   -- fringing
local L      = lambda0 / (2 * math.sqrt(eeff)) - 2 * dL                     -- resonant length
local Wc, Lc = math.floor(W + 0.5), math.floor(L + 0.5) + trim
local fTL    = 299792458 / (2 * (Lc + 2 * dL) * math.sqrt(eeff) * grid.cell_size)   -- TL resonance of the built patch
-- Edge resistance from the radiating-slot conductance G1 = W / (120 lambda0) (1 - (k h)^2 / 24).
local G1     = W / (120 * lambda0) * (1 - (2 * math.pi * h / lambda0) ^ 2 / 24)
local Redge  = 1 / (2 * G1)
local d50    = Lc / math.pi * math.acos(math.sqrt(math.min(50 / Redge, 1)))
print(string.format("patch design (%s, eps_r %.2f): W = %d cells (%.1f mm), L = %d cells (%.1f mm, trim %+d), eps_eff = %.3f, TL resonance %.3f GHz",
                    substrate, eps, Wc, Wc * grid.cell_size * 1e3, Lc, Lc * grid.cell_size * 1e3, trim, eeff, fTL * 1e-9))
print(string.format("transmission-line model: edge resistance ~%.0f ohm (slot only), 50-ohm inset ~%.1f cells; feed inset now %d cells",
                    Redge, d50, inset))

-- Board: ground foil, substrate, patch foil. The patch's resonant length runs along x.
local c      = grid.center
local zg     = c.z - 8                                -- ground plane height (a lattice plane)
local margin = 12                                     -- ground / substrate extension beyond the patch
local x0, x1 = c.x - math.floor(Lc / 2), c.x - math.floor(Lc / 2) + Lc
local y0, y1 = c.y - math.floor(Wc / 2), c.y - math.floor(Wc / 2) + Wc

sheet { min = vec(x0 - margin, y0 - margin, zg), max = vec(x1 + margin, y1 + margin, zg) }           -- ground
box   { min = vec(x0 - margin, y0 - margin, zg), max = vec(x1 + margin, y1 + margin, zg + h), material = substrate }
sheet { min = vec(x0, y0, zg + h), max = vec(x1, y1, zg + h) }                                       -- patch

-- Coax probe: inner conductor (radius 0.65 mm, an SMA pin) from the ground up to the patch,
-- `inset` cells in from the radiating edge at x0, on the centre line. 50-ohm feed at the ground.
local pulse = mode == "sweep"
monopole {
  base = vec(x0 + inset, c.y, zg), orientation = "+z", length = h, radius = mm(0.65), amplitude = 1,
  name = "probe", frequency = f0,
  waveform = pulse and "pulse" or "cw", pulse_width = pulse and 0.4 or nil,
}

if pulse then
  sweep { frequencies = {1.9e9, 2.9e9}, points = 201, log = "patch_s11.csv" }
else
  far_field { log = "patch_pattern.csv" }
end
probe { position = vec(c.x, c.y, zg + h + 25), name = "broadside" }

view { gain = pulse and 60 or 30, distance = 2.2, yaw = -55, pitch = 35, steps_per_frame = 8 }
