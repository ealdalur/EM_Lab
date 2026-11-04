#version 460 core
// Full-screen triangle generated from gl_VertexID (no vertex buffer needed).
// Covers NDC [-1,1]^2 with a single oversized triangle - no diagonal seam.

layout(location = 0) out vec2 vNdc;

void main() {
    vec2 uv = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2)); // (0,0) (2,0) (0,2)
    vNdc = uv * 2.0 - 1.0;
    gl_Position = vec4(vNdc, 0.0, 1.0);
}
