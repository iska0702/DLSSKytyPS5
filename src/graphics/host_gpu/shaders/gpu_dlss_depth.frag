#version 450
layout(location = 0) out float depth;
layout(binding = 0) uniform sampler2D source;
void main() {
    depth = texelFetch(source, ivec2(gl_FragCoord.xy), 0).r;
}
