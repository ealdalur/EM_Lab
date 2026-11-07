-- Half-wave dipole antenna
-- The antenna you would actually build: a thin metal wire cut in the middle and fed there
-- by a 50-ohm source (a transmitter behind a 50-ohm line). The source drives the gap; the
-- solver works out how current distributes along the wire (roughly sinusoidal, zero at the
-- tips), and that sets the pattern and the feed impedance.
--
-- Watch the "port" line in the console:
--   Z      input impedance. Near half a wavelength it crosses resonance (reactance X = 0);
--          thin-wire theory: ~63 ohm at resonance, 73 + j42 ohm at exactly 0.5 lambda.
--   Gamma, S11, VSWR   mismatch against the 50-ohm source; P_acc   power the antenna accepts.
--   R_rad  radiation resistance from the far-field power (equals Re Z: no losses).
--
-- Try: , . change the length (2 cells = 0.05 lambda per step) and find resonance;
--      T cycles the wire thickness: thicker wires resonate shorter and have a broader
--      (more forgiving) impedance curve.
--      Longer dipoles: ~1 lambda narrower doughnut, ~1.25 lambda most directive (5.2 dBi),
--      ~1.5 lambda the main lobe splits.
-- For the ideal point dipole used to validate the solver, see validation/hertzian_dipole.lua.

scene {
  name        = "Half-Wave Dipole Antenna",
  description = "Centre-fed thin-wire dipole with a 50-ohm feed: impedance, VSWR, pattern and directivity.",
  size        = {144, 144, 144},
  wavelength  = 40,            -- 40 cells per wavelength: fine length steps and accurate impedance
}

local length    = param { name = "length", label = "length (cells)", default = 19, min = 7, max = 61, step = 2, keys = {",", "."} }
local thickness = param { name = "thickness", label = "wire", choices = {"thin", "very thin", "thick"}, key = "t" }

local radius = ({ ["very thin"] = grid.wavelength / 2000, thin = grid.wavelength / 400, thick = grid.wavelength / 100 })[thickness]
print(string.format("dipole length %d cells = %.3f wavelengths, wire radius %.4f wavelengths",
                    length, length / grid.wavelength, radius / grid.wavelength))

dipole {
  center      = grid.center,
  orientation = "z",
  length      = length,
  radius      = radius,       -- thin wire (model = "thin" is the default)
  amplitude   = 1,            -- source EMF (peak); feed = "voltage", impedance = 50 are the defaults
}

probe    { position = grid.center + vec(40, 0, 0), name = "broadside" }
flux_box { center = grid.center, size = math.max(length + 16, 32), name = "radiated power" }
far_field {}

view { gain = 45, distance = 2.4, yaw = -60, pitch = 22, steps_per_frame = 1 }
