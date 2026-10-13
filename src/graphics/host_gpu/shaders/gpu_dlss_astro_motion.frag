#version 450
layout(location = 0) out vec2 motion;
layout(binding = 0) uniform sampler2D source;
layout(binding = 1) uniform sampler2D depth_source;
layout(push_constant) uniform Constants {
    layout(offset = 8) vec2 jitter_delta;
    layout(offset = 32) mat4 clip_to_previous;
    layout(offset = 96) uint camera_valid;
    layout(offset = 104) vec2 jitter_uv;
} constants;
void main() {
    vec2 value = texelFetch(source, ivec2(gl_FragCoord.xy), 0).rg;
    // Verified producer uses 128 in Y for invalid motion. Reject it before
    // interpolation; DLSS expects normalized current-to-previous displacement.
    bool invalid = any(isnan(value)) || any(isinf(value)) || any(greaterThan(abs(value), vec2(1.0)));
    // Projected UV moves opposite to the projection sample offset. The negated
    // guest vector therefore contains current minus previous sample jitter.
    motion = invalid ? vec2(0.0) : -value - constants.jitter_delta;
    // Static surfaces can use the guest's 128 sentinel instead of explicit
    // object motion. Resolve camera motion from depth, rather than treating
    // those surfaces as stationary on screen during a camera pan.
    if (invalid && constants.camera_valid != 0u) {
        vec2 uv = gl_FragCoord.xy / vec2(textureSize(source, 0)) + constants.jitter_uv;
        float depth = texelFetch(depth_source, ivec2(gl_FragCoord.xy), 0).r;
        vec4 previous = constants.clip_to_previous * vec4(2.0 * uv.x - 1.0, 1.0 - 2.0 * uv.y, depth, 1.0);
        if (!any(isnan(previous)) && !any(isinf(previous)) && previous.w > 0.000001) {
            vec2 previous_uv = vec2(previous.x, -previous.y) / previous.w * 0.5 + 0.5;
            vec2 resolved = previous_uv - uv;
            if (!any(isnan(resolved)) && !any(isinf(resolved)) && all(lessThanEqual(abs(resolved), vec2(1.0))))
                motion = resolved;
        }
    }
}
