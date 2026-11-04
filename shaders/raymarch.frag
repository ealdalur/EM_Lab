#version 460 core
// =============================================================================
// raymarch.frag - emission/absorption volume renderer for the FDTD fields.
//
//  * |E|  -> neon cyan / blue glow
//  * |B|  -> neon magenta / red glow
//  * metals (high sigma) -> shaded, semi-transparent grey solid
//  * dielectrics (eps_r > 1) -> faint glassy teal
// Front-to-back compositing with early ray termination; per-pixel interleaved
// gradient noise jitters the ray start to trade banding for fine, stable noise.
// =============================================================================

layout(location = 0) in vec2 vNdc;
layout(location = 0) out vec4 oColor;

layout(binding = 0) uniform sampler3D uEField; // rgb = E, a = sigma
layout(binding = 1) uniform sampler3D uBField; // rgb = B, a = eps_r

uniform mat4  uInvViewProj;
uniform vec3  uCamPos;
uniform vec3  uVolMin;      // world-space AABB of the whole grid
uniform vec3  uVolSize;
uniform vec3  uClipMin;     // world-space AABB actually marched (cutaway)
uniform vec3  uClipMax;
uniform vec3  uGridSize;    // cells per axis
uniform float uStepVoxels;  // ray step in voxels
uniform float uGainE;
uniform float uGainB;
uniform float uDensity;     // field opacity multiplier
uniform float uEmission;    // field brightness multiplier
uniform float uMaterialOpacity;
uniform float uExposure;
uniform int   uMode;        // 0 E+B, 1 E, 2 B, 3 Poynting |ExB|, 4 signed E component
uniform int   uSignedAxis;  // component for mode 4

const int MAX_STEPS = 4096;

// Robust slab test; returns (tEnter, tExit), tEnter > tExit means a miss.
vec2 intersectBox(vec3 ro, vec3 rd, vec3 bmin, vec3 bmax) {
    vec3 safeRd = vec3(abs(rd.x) < 1e-8 ? 1e-8 : rd.x,
                       abs(rd.y) < 1e-8 ? 1e-8 : rd.y,
                       abs(rd.z) < 1e-8 ? 1e-8 : rd.z);
    vec3 inv = 1.0 / safeRd;
    vec3 t0 = (bmin - ro) * inv;
    vec3 t1 = (bmax - ro) * inv;
    vec3 tmin = min(t0, t1);
    vec3 tmax = max(t0, t1);
    return vec2(max(max(tmin.x, tmin.y), tmin.z), min(min(tmax.x, tmax.y), tmax.z));
}

float interleavedGradientNoise(vec2 p) {
    return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715))));
}

vec3 background(vec3 rd) {
    float h = clamp(rd.z * 0.5 + 0.5, 0.0, 1.0);
    return mix(vec3(0.004, 0.006, 0.014), vec3(0.018, 0.028, 0.060), h);
}

// Soft-saturating response: quadratic near 0 (suppresses numerical noise),
// asymptotically 1 for strong fields.
float response(float x) { float x2 = x * x; return x2 / (1.0 + x2); }

// Material density used for surface normals: metals from sigma, glass from eps_r.
float metalAmount(float sigma) { return smoothstep(1.0, 20.0, sigma); }
float glassAmount(float epsR)  { return clamp((epsR - 1.0) * 0.5, 0.0, 1.0); }

vec3 materialNormal(vec3 uvw, bool metal, vec3 rd) {
    vec3 h = 1.0 / uGridSize;
    vec3 g;
    if (metal) {
        g = vec3(texture(uEField, uvw + vec3(h.x, 0, 0)).a - texture(uEField, uvw - vec3(h.x, 0, 0)).a,
                 texture(uEField, uvw + vec3(0, h.y, 0)).a - texture(uEField, uvw - vec3(0, h.y, 0)).a,
                 texture(uEField, uvw + vec3(0, 0, h.z)).a - texture(uEField, uvw - vec3(0, 0, h.z)).a);
    } else {
        g = vec3(texture(uBField, uvw + vec3(h.x, 0, 0)).a - texture(uBField, uvw - vec3(h.x, 0, 0)).a,
                 texture(uBField, uvw + vec3(0, h.y, 0)).a - texture(uBField, uvw - vec3(0, h.y, 0)).a,
                 texture(uBField, uvw + vec3(0, 0, h.z)).a - texture(uBField, uvw - vec3(0, 0, h.z)).a);
    }
    float len = length(g);
    // Inside a homogeneous block the gradient vanishes: light it as if facing the viewer.
    return len > 1e-5 ? -g / len : -rd;
}

vec3 shadeSurface(vec3 n, vec3 rd, vec3 albedo, float specular) {
    const vec3 keyDir = normalize(vec3(0.45, -0.35, 0.82));
    vec3 v = -rd;
    float ndl = max(dot(n, keyDir), 0.0);
    float head = max(dot(n, v), 0.0);
    vec3 hv = normalize(keyDir + v);
    float spec = pow(max(dot(n, hv), 0.0), 48.0) * specular;
    float rim = pow(1.0 - head, 3.0);
    return albedo * (0.18 + 0.55 * ndl + 0.30 * head) + spec + rim * vec3(0.35, 0.45, 0.6);
}

vec4 finalize(vec3 color) {
    color = 1.0 - exp(-color * uExposure);                       // filmic-ish tone curve
    color = pow(color, vec3(1.0 / 2.2));                         // linear -> display
    color += (interleavedGradientNoise(gl_FragCoord.xy + 17.0) - 0.5) / 255.0; // dither
    return vec4(color, 1.0);
}

void main() {
    // ---- primary ray -------------------------------------------------------
    vec4 farH = uInvViewProj * vec4(vNdc, 1.0, 1.0);
    vec3 ro = uCamPos;
    vec3 rd = normalize(farH.xyz / farH.w - ro);

    vec3 bg = background(rd);
    vec2 hit = intersectBox(ro, rd, uClipMin, uClipMax);
    hit.x = max(hit.x, 0.0);
    if (hit.x >= hit.y) {
        oColor = finalize(bg);
        return;
    }

    float voxelWorld = uVolSize.x / uGridSize.x;  // isotropic voxels
    float ds = max(uStepVoxels, 0.05) * voxelWorld;
    float stepVox = ds / voxelWorld;
    float t = hit.x + ds * interleavedGradientNoise(gl_FragCoord.xy);
    int steps = min(int((hit.y - t) / ds) + 1, MAX_STEPS);

    vec3 accum = vec3(0.0);
    float alpha = 0.0;

    for (int i = 0; i < steps; ++i, t += ds) {
        vec3 uvw = (ro + rd * t - uVolMin) / uVolSize;
        vec4 e = texture(uEField, uvw);
        vec4 b = texture(uBField, uvw);

        // ---- field emission ------------------------------------------------
        vec3 emission = vec3(0.0);
        float extinction = 0.0;

        if (uMode == 0 || uMode == 1) {
            float iE = response(length(e.rgb) * uGainE);
            vec3 col = mix(vec3(0.02, 0.22, 1.0), vec3(0.05, 0.95, 1.0), iE);
            col = mix(col, vec3(0.8, 1.0, 1.0), smoothstep(0.8, 1.0, iE));
            emission += col * (iE * iE * 2.0);
            extinction += iE;
        }
        if (uMode == 0 || uMode == 2) {
            float iB = response(length(b.rgb) * uGainB);
            vec3 col = mix(vec3(1.0, 0.03, 0.10), vec3(1.0, 0.10, 0.62), iB);
            col = mix(col, vec3(1.0, 0.85, 1.0), smoothstep(0.8, 1.0, iB));
            emission += col * (iB * iB * 2.0);
            extinction += iB;
        }
        if (uMode == 3) {
            float iS = response(length(cross(e.rgb, b.rgb)) * uGainE * uGainB);
            vec3 col = mix(vec3(1.0, 0.35, 0.02), vec3(1.0, 0.85, 0.25), iS);
            col = mix(col, vec3(1.0, 1.0, 0.85), smoothstep(0.8, 1.0, iS));
            emission += col * (iS * iS * 2.0);
            extinction += iS;
        }
        if (uMode == 4) {
            float v = e[uSignedAxis] * uGainE;
            float iV = response(v);
            vec3 col = v >= 0.0 ? mix(vec3(0.02, 0.3, 1.0), vec3(0.1, 1.0, 1.0), iV)
                                : mix(vec3(0.9, 0.15, 0.02), vec3(1.0, 0.65, 0.1), iV);
            emission += col * (iV * iV * 2.0);
            extinction += iV;
        }
        // Per-voxel scales chosen so a ray crossing ~150 voxels of moderate field
        // accumulates O(1) radiance while still letting the background through.
        emission   *= 0.045 * uEmission;
        extinction *= 0.022 * uDensity;

        // ---- materials -----------------------------------------------------
        float metal = metalAmount(e.a);
        float glass = glassAmount(b.a);
        float absorber = smoothstep(0.02, 1.0, e.a) * (1.0 - metal); // lossy (sub-metal) media
        vec3 surface = vec3(0.0);
        float matExt = 0.0;
        if (metal + glass + absorber > 0.01) {
            vec3 n = materialNormal(uvw, metal >= glass, rd);
            vec3 metalCol = shadeSurface(n, rd, vec3(0.50, 0.53, 0.58), 0.9);
            vec3 glassCol = shadeSurface(n, rd, vec3(0.30, 0.65, 0.70), 0.6);
            vec3 lossyCol = shadeSurface(n, rd, vec3(0.35, 0.22, 0.12), 0.1);
            float wM = metal * uMaterialOpacity;
            float wG = glass * uMaterialOpacity * 0.02;  // thick dielectrics stay see-through
            float wA = absorber * uMaterialOpacity * 0.4;
            matExt = wM + wG + wA;
            surface = (metalCol * wM + glassCol * wG + lossyCol * wA) / max(matExt, 1e-6);
        }

        // ---- front-to-back compositing ---------------------------------------
        float aField = 1.0 - exp(-extinction * stepVox);
        float aMat   = 1.0 - exp(-matExt * stepVox);
        float transmittance = 1.0 - alpha;
        accum += transmittance * (emission * stepVox * (1.0 - aMat) + surface * aMat);
        alpha += transmittance * (1.0 - (1.0 - aField) * (1.0 - aMat));

        if (alpha > 0.995) break;
    }

    oColor = finalize(accum + (1.0 - alpha) * bg);
}
