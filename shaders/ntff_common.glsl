// =============================================================================
// ntff_common.glsl - closed Huygens box used by the near-to-far-field transform.
//
// The box surface is the set of integer lattice nodes on the six faces of
// [uBoxMin, uBoxMax]. Face f has normal axis a = f >> 1 and side f & 1
// (0 = min face, outward -a; 1 = max face, outward +a). Each face uses all nodes
// [min, max] along its two tangential axes (u, v) with trapezoid-rule area weights
// (1/2 on face edges, 1/4 at corners), which integrates the closed surface to
// second order without gaps or double counting. The points of all faces are packed
// into one linear index range using the prefix sums uFaceOffset[0..6].
// =============================================================================

uniform ivec3 uBoxMin;
uniform ivec3 uBoxMax;
uniform int   uFaceOffset[7];

struct Phasor {
    vec4 eRe; // complex E amplitude (xyz), real part
    vec4 eIm; //                             imaginary part
    vec4 hRe; // complex H amplitude (H = B in normalised units)
    vec4 hIm;
};

// Two cascaded exponential averaging stages (stage2 is the estimate).
struct PhasorPair {
    Phasor stage1;
    Phasor stage2;
};

void ntffPoint(int index, out ivec3 node, out vec3 normal, out float weight) {
    int f = 0;
    for (int i = 1; i < 6; ++i)
        if (index >= uFaceOffset[i]) f = i;

    int a = f >> 1;
    int side = f & 1;
    int u = (a == 0) ? 1 : 0;
    int v = (a == 2) ? 1 : 2;
    int nu = uBoxMax[u] - uBoxMin[u];       // intervals; nu + 1 nodes
    int nv = uBoxMax[v] - uBoxMin[v];
    int local = index - uFaceOffset[f];
    int iu = local % (nu + 1);
    int iv = local / (nu + 1);

    node = uBoxMin;
    node[a] = (side == 1) ? uBoxMax[a] : uBoxMin[a];
    node[u] = uBoxMin[u] + iu;
    node[v] = uBoxMin[v] + iv;
    weight = ((iu == 0 || iu == nu) ? 0.5 : 1.0) * ((iv == 0 || iv == nv) ? 0.5 : 1.0);

    normal = vec3(0.0);
    normal[a] = (side == 1) ? 1.0 : -1.0;
}
