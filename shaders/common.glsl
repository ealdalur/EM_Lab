// =============================================================================
// common.glsl - shared FDTD definitions (included by the compute passes).
//
// Normalised units: c = epsilon0 = mu0 = 1 and dx = 1 cell, so dt = Courant number.
// The B texture therefore stores c*B (same units as E), which also makes the
// Poynting vector simply S = E x B.
//
// Yee lattice layout - texel (i,j,k) stores:
//   Ex @ (i+1/2, j    , k    )     Bx @ (i    , j+1/2, k+1/2)
//   Ey @ (i    , j+1/2, k    )     By @ (i+1/2, j    , k+1/2)
//   Ez @ (i    , j    , k+1/2)     Bz @ (i+1/2, j+1/2, k    )
// E.alpha = electric conductivity sigma, B.alpha = relative permittivity eps_r.
// =============================================================================

uniform ivec3 uGridSize;      // cells per axis
uniform float uDt;            // time step (= Courant number in normalised units)
uniform int   uPmlThickness;  // absorbing layer depth in cells
uniform float uPmlSigmaMax;   // conductivity at the outer edge of the layer
uniform float uPmlGrading;    // exponential grading factor g (> 0)

// Exponentially graded conductivity profile, u in [0,1] from inner to outer edge:
//   sigma(u) = sigmaMax * (exp(g u) - 1) / (exp(g) - 1)
float pmlProfile(float depth) {
    if (depth <= 0.0) return 0.0;
    float u = min(depth / float(uPmlThickness), 1.0);
    return uPmlSigmaMax * (exp(uPmlGrading * u) - 1.0) / (exp(uPmlGrading) - 1.0);
}

// Absorber conductivity at an arbitrary (staggered) lattice position.
// Contributions from each axis add up in edges/corners of the domain.
float pmlSigma(vec3 x) {
    vec3 L = vec3(float(uPmlThickness));
    vec3 hi = vec3(uGridSize - 1) - L;
    vec3 depth = max(L - x, x - hi);
    return pmlProfile(depth.x) + pmlProfile(depth.y) + pmlProfile(depth.z);
}

// True if any part of cell p (including its +1/2 staggered samples) can be
// inside the absorbing layer. Lets interior cells skip 9 exp() calls.
bool inPml(ivec3 p) {
    return any(lessThan(p, ivec3(uPmlThickness))) ||
           any(greaterThanEqual(p, uGridSize - ivec3(uPmlThickness + 1)));
}

// phi(x) = (1 - exp(-x)) / x, evaluated without cancellation for small x.
// Used by the exponential (exact-integrating) update for lossy media:
//   F^{n+1} = exp(-x) F^n + dt * phi(x) * (source term),  x = sigma*dt/eps
// This is unconditionally stable and, unlike the Crank-Nicolson form, never
// produces a negative decay factor (no sign-flip ringing inside metals).
vec3 phi(vec3 x) {
    vec3 series = 1.0 - x * (0.5 - x * (1.0 / 6.0 - x * (1.0 / 24.0)));
    vec3 exact  = (1.0 - exp(-x)) / max(x, vec3(1e-12));
    return mix(series, exact, step(vec3(1e-2), x));
}
