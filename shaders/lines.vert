#version 460 core
// Overlay lines: grid bounds, absorber boundary, probe marker, flux surface.

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec4 aColor;

layout(location = 0) out vec4 vColor;

uniform mat4 uViewProj;

void main() {
    vColor = aColor;
    gl_Position = uViewProj * vec4(aPos, 1.0);
}
