// =============================================================================
// field_sampling.glsl - co-located E and B at integer lattice nodes.
//
// The Yee lattice stores each component at its own staggered sub-position (see
// common.glsl). Quantities that combine E and B (Poynting vector, surface
// currents for the near-to-far-field transform) need both at one point, so the
// components are averaged onto the integer node (i,j,k):
//   E: 2-point average along the component's own axis
//   B: 4-point average over the two axes transverse to the component
//
// Requires the includer to declare: sampler3D uEField, sampler3D uBField, ivec3 uGridSize.
// =============================================================================

vec4 fetchFieldE(ivec3 p) { return texelFetch(uEField, clamp(p, ivec3(0), uGridSize - 1), 0); }
vec4 fetchFieldB(ivec3 p) { return texelFetch(uBField, clamp(p, ivec3(0), uGridSize - 1), 0); }

vec3 eAtNode(ivec3 p) {
    const ivec3 X = ivec3(1, 0, 0), Y = ivec3(0, 1, 0), Z = ivec3(0, 0, 1);
    vec4 e0 = fetchFieldE(p);
    return 0.5 * vec3(e0.x + fetchFieldE(p - X).x,
                      e0.y + fetchFieldE(p - Y).y,
                      e0.z + fetchFieldE(p - Z).z);
}

vec3 bAtNode(ivec3 p) {
    const ivec3 X = ivec3(1, 0, 0), Y = ivec3(0, 1, 0), Z = ivec3(0, 0, 1);
    vec4 b0 = fetchFieldB(p);
    float bx = b0.x + fetchFieldB(p - Y).x + fetchFieldB(p - Z).x + fetchFieldB(p - Y - Z).x;
    float by = b0.y + fetchFieldB(p - X).y + fetchFieldB(p - Z).y + fetchFieldB(p - X - Z).y;
    float bz = b0.z + fetchFieldB(p - X).z + fetchFieldB(p - Y).z + fetchFieldB(p - X - Y).z;
    return 0.25 * vec3(bx, by, bz);
}
