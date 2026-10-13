#version 450

// Presents one mip level of a prepared frame to a smaller swapchain image (see PresentFilter).
// Each target pixel averages a two-texel box of the source on every downscaled axis, which
// cancels one-texel dither patterns at any phase; an axis that is not downscaled interpolates
// linearly like a blit.

layout(location = 0) out vec4 color;
layout(binding = 0) uniform sampler2D source; // linear, clamp to edge
layout(push_constant) uniform Downscale {
	ivec2 size;  // extent of the source level
	vec2  scale; // source texels per target pixel
	int   level;
};

// The box [c - 1, c + 1] (source texels) covers texels t, t + 1 and t + 2 with weights
// (1 - f) / 2, 1 / 2 and f / 2, where t + f = c - 1. One linear tap at a reads the first two,
// one at b the third. Positions are in texels, weights sum to one.
void Taps(float c, float s, out float a, out float wa, out float b, out float wb) {
	if (s > 1.0) {
		const float t = floor(c - 1.0);
		const float f = c - 1.0 - t;
		a             = t + 0.5 + 1.0 / (2.0 - f);
		wa            = 1.0 - 0.5 * f;
		b             = t + 2.5;
		wb            = 0.5 * f;
	} else {
		a  = c;
		wa = 1.0;
		b  = c;
		wb = 0.0;
	}
}

void main() {
	const vec2 c = gl_FragCoord.xy * scale;
	if (all(lessThanEqual(scale, vec2(1.0)))) {
		// Catmull-Rom spatial reconstruction keeps logo/text edges sharper than
		// bilinear magnification. Clamp to the central neighborhood to avoid halos.
		vec2 p = c - 0.5;
		ivec2 base = ivec2(floor(p));
		vec2 f = fract(p), f2 = f * f, f3 = f2 * f;
		vec4 wx = vec4(-0.5 * f.x + f2.x - 0.5 * f3.x,
		               1.0 - 2.5 * f2.x + 1.5 * f3.x,
		               0.5 * f.x + 2.0 * f2.x - 1.5 * f3.x,
		               -0.5 * f2.x + 0.5 * f3.x);
		vec4 wy = vec4(-0.5 * f.y + f2.y - 0.5 * f3.y,
		               1.0 - 2.5 * f2.y + 1.5 * f3.y,
		               0.5 * f.y + 2.0 * f2.y - 1.5 * f3.y,
		               -0.5 * f2.y + 0.5 * f3.y);
		vec4 sum = vec4(0.0), lo = vec4(1e30), hi = vec4(-1e30);
		for (int y = 0; y < 4; ++y) for (int x = 0; x < 4; ++x) {
			vec4 tap = texelFetch(source, clamp(base + ivec2(x - 1, y - 1), ivec2(0), size - 1), level);
			sum += tap * wx[x] * wy[y];
			if (x >= 1 && x <= 2 && y >= 1 && y <= 2) {
				lo = min(lo, tap); hi = max(hi, tap);
			}
		}
		color = clamp(sum, lo, hi);
		return;
	}
	float      ax, wax, bx, wbx, ay, way, by, wby;
	Taps(c.x, scale.x, ax, wax, bx, wbx);
	Taps(c.y, scale.y, ay, way, by, wby);
	const vec2  texel = 1.0 / vec2(size);
	const float lod   = float(level);
	color = (textureLod(source, vec2(ax, ay) * texel, lod) * wax +
	         textureLod(source, vec2(bx, ay) * texel, lod) * wbx) *
	            way +
	        (textureLod(source, vec2(ax, by) * texel, lod) * wax +
	         textureLod(source, vec2(bx, by) * texel, lod) * wbx) *
	            wby;
}
