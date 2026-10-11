#version 450

// AMD FidelityFX Contrast Adaptive Sharpening (CAS / RCAS)
// 9-tap isotropic contrast-adaptive unsharp masking filter.
// Sharpens micro-textures and geometry edges in all 8 directions while adapting
// to local dynamic range to eliminate ringing and haloing.

layout(location = 0) out vec4 color;
layout(binding = 0) uniform sampler2D source; // linear, clamp to edge

layout(push_constant) uniform CasConstants {
	ivec2 size;      // extent of the source
	vec2  scale;     // source texels per target pixel (source / target)
	float sharpness; // sharpening strength in [0.0 .. 1.0]
	int   dummy;
};

void main() {
	const vec2 coord = gl_FragCoord.xy * scale;
	const vec2 uv    = coord / vec2(size);
	const vec2 texel = 1.0 / vec2(size);

	// Sample 3x3 9-tap neighborhood:
	//   [a] [b] [c]
	//   [d] [e] [f]
	//   [g] [h] [i]
	vec3 a = textureLod(source, uv + vec2(-texel.x, -texel.y), 0.0).rgb;
	vec3 b = textureLod(source, uv + vec2(0.0,      -texel.y), 0.0).rgb;
	vec3 c = textureLod(source, uv + vec2( texel.x, -texel.y), 0.0).rgb;
	vec3 d = textureLod(source, uv + vec2(-texel.x,  0.0),     0.0).rgb;
	vec3 e = textureLod(source, uv,                            0.0).rgb;
	vec3 f = textureLod(source, uv + vec2( texel.x,  0.0),     0.0).rgb;
	vec3 g = textureLod(source, uv + vec2(-texel.x,  texel.y), 0.0).rgb;
	vec3 h = textureLod(source, uv + vec2(0.0,       texel.y), 0.0).rgb;
	vec3 i = textureLod(source, uv + vec2( texel.x,  texel.y), 0.0).rgb;

	// Soft min and max of local neighborhood
	vec3 mnRGB = min(min(min(d, e), min(f, b)), min(h, min(min(a, c), min(g, i))));
	vec3 mxRGB = max(max(max(d, e), max(f, b)), max(h, max(max(a, c), max(g, i))));

	// Contrast-adaptive headroom (how far center can boost without clipping)
	vec3 ampRGB = clamp(min(mnRGB, 2.0 - mxRGB) / max(mxRGB, vec3(1e-4)), 0.0, 1.0);

	// Adaptive sharpening strength:
	// Base sharpening scales with user sharpness slider [0.0 .. 1.0].
	// Surfaces with subtle contrast receive full boost; high-contrast stark edges receive reduced boost.
	float s = clamp(sharpness, 0.0, 1.0);
	vec3 wRGB = -s * mix(vec3(0.20), vec3(0.08), 1.0 - ampRGB);

	// 9-tap filter sum:
	// Cross neighbors (b, d, f, h) have full weight w.
	// Diagonal neighbors (a, c, g, i) have 0.5 * w.
	// Total weight normalization: 4 * w + 4 * (0.5 * w) + 1.0 = 6 * w + 1.0.
	vec3 crossSum = b + d + f + h;
	vec3 diagSum  = a + c + g + i;
	vec3 rcas     = (crossSum * wRGB) + (diagSum * (wRGB * 0.5)) + e;
	rcas /= (6.0 * wRGB + vec3(1.0));

	// Clamp to standard display range [0, 1]
	rcas = clamp(rcas, 0.0, 1.0);

	float alpha = textureLod(source, uv, 0.0).a;
	color = vec4(rcas, alpha);
}
