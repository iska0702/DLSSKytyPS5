#version 450
layout(location = 0) out vec2 motion;
layout(binding = 0) uniform sampler2D source;
void main() {
    vec2 value = texelFetch(source, ivec2(gl_FragCoord.xy), 0).rg;
    // Verified producer uses 128 in Y for invalid motion. Reject it before
    // interpolation; DLSS expects normalized current-to-previous displacement.
    bool invalid = any(isnan(value)) || any(isinf(value)) || any(greaterThan(abs(value), vec2(1.0)));
    motion = invalid ? vec2(0.0) : -value;
}
