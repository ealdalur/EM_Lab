-- VALIDATION: thin-wire dipole impedance
-- Compares the simulated feed impedance of a centre-fed thin-wire dipole with the classic
-- induced-EMF (sinusoidal-current) theory. At 40 cells per wavelength, radius lambda/2000:
--
--   length    theory (induced EMF)     measured
--   0.175 L     6.3 - j807 ohm          5.9 - j812
--   0.375 L    34.0 - j222              32.6 - j222
--   0.475 L    63.0 - j11               62.9 - j37
--   0.525 L    84.7 + j98               87.1 + j54
--
-- Radiation resistance agrees within a few percent everywhere; the reactance agrees closely
-- for short dipoles and is lower near resonance (the induced-EMF formula itself is
-- approximate there), placing resonance ~1% of a wavelength longer than the formula.
-- Run with:  EM_Lab --script validation/dipole_impedance --set length=19

scene {
  name        = "Validation: thin-wire dipole impedance",
  description = "Centre-fed thin dipole; compare the port impedance with induced-EMF theory.",
  size        = {160, 160, 160},
  wavelength  = 40,
}

local length = param { name = "length", default = 19, min = 5, max = 41, step = 2, keys = {",", "."} }
local radius = param { name = "radius", default = 0.02, min = 0.005, max = 0.4, step = 0.005, keys = {";", "'"} }

dipole    { center = grid.center, orientation = "z", length = length, radius = radius, amplitude = 1 }
far_field {}

view { gain = 25, distance = 2.4, steps_per_frame = 8 }
