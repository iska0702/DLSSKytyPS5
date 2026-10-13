#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace Libs::Graphics {

using DLSSMatrix = std::array<float, 16>;

inline DLSSMatrix DLSSMultiply(const DLSSMatrix& a, const DLSSMatrix& b) {
    DLSSMatrix out {};
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            for (int k = 0; k < 4; ++k) out[r * 4 + c] += a[r * 4 + k] * b[k * 4 + c];
    return out;
}

inline bool DLSSInvert(const DLSSMatrix& matrix, DLSSMatrix& inverse) {
    double a[4][8] {};
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) a[r][c] = matrix[r * 4 + c];
        a[r][r + 4] = 1.0;
    }
    for (int c = 0; c < 4; ++c) {
        int pivot = c;
        for (int r = c + 1; r < 4; ++r) if (std::abs(a[r][c]) > std::abs(a[pivot][c])) pivot = r;
        if (!std::isfinite(a[pivot][c]) || std::abs(a[pivot][c]) < 1e-12) return false;
        for (int k = 0; k < 8; ++k) std::swap(a[c][k], a[pivot][k]);
        const double scale = a[c][c];
        for (int k = 0; k < 8; ++k) a[c][k] /= scale;
        for (int r = 0; r < 4; ++r) if (r != c) {
            const double factor = a[r][c];
            for (int k = 0; k < 8; ++k) a[r][k] -= factor * a[c][k];
        }
    }
    for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) {
        inverse[r * 4 + c] = static_cast<float>(a[r][c + 4]);
        if (!std::isfinite(inverse[r * 4 + c])) return false;
    }
    return true;
}

inline bool DLSSMatricesMatch(const DLSSMatrix& a, const DLSSMatrix& b) {
    for (int i = 0; i < 16; ++i)
        if (std::abs(a[i] - b[i]) > 1e-4f * (1.0f + std::abs(a[i]))) return false;
    return true;
}

struct DLSSCamera {
    DLSSMatrix projection {}, inverse_projection {}, clip_to_previous {}, previous_to_clip {};
    DLSSMatrix jittered_view_projection {};
    std::array<float, 3> position {}, right {}, up {}, forward {};
    std::array<float, 2> jitter_uv {}, jitter_pixels {}, motion_jitter_delta {};
    float near_plane = 0, far_plane = 0, fov = 0, aspect = 0;
    bool valid = false;
    bool reset = true;
};

// Only call for a shader-certified Astro 496-byte view block. Descriptor size
// alone is not a certificate. The viewport and matrix identities are validated.
inline DLSSCamera DecodeAstroDLSSCamera(const std::array<float, 124>& v,
                                      uint32_t width, uint32_t height,
                                      const DLSSCamera* previous = nullptr) {
    DLSSCamera out;
    if (width == 0 || height == 0) return out;
    for (float value : v) if (!std::isfinite(value)) return out;
    if (v[108] != static_cast<float>(width) || v[109] != static_cast<float>(height)) return out;
    DLSSMatrix projection, view, view_inverse, vp, prev_vp;
    std::copy_n(v.data() + 32, 16, projection.data());
    std::copy_n(v.data() + 16, 16, view.data());
    std::copy_n(v.data(), 16, vp.data());
    std::copy_n(v.data() + 48, 16, view_inverse.data());
    std::copy_n(v.data() + 80, 16, prev_vp.data());
    if (std::abs(projection[11] + 1.0f) > 1e-5f || std::abs(projection[15]) > 1e-5f ||
        projection[0] <= 0 || projection[5] <= 0 || !DLSSMatricesMatch(DLSSMultiply(view, projection), vp)) return out;
    DLSSMatrix identity {};
    identity[0] = identity[5] = identity[10] = identity[15] = 1;
    if (!DLSSMatricesMatch(DLSSMultiply(view, view_inverse), identity)) return out;
    out.jitter_uv = {v[96], v[97]};
    // w=-view.z: these projection coefficients shift raster UV by -jitter_uv.
    // Streamline uses raster displacement, in render pixels, with motion's axes.
    out.jitter_pixels = {-v[96] * width, -v[97] * height};
    // Astro doubles the sample pattern in gameplay (up to one render pixel).
    // Preserve that actual displacement; clipping/rejecting half the samples
    // would misalign reconstruction or reset temporal history repeatedly.
    if (std::abs(out.jitter_pixels[0]) > 1.001f || std::abs(out.jitter_pixels[1]) > 1.001f) return out;
    // Projection jitter is removed before supplying matrices to Streamline.
    projection[8] -= 2.0f * out.jitter_uv[0];
    projection[9] += 2.0f * out.jitter_uv[1];
    out.projection = projection;
    if (!DLSSInvert(projection, out.inverse_projection)) return out;
    out.jittered_view_projection = vp;
    out.clip_to_previous = out.previous_to_clip = identity;
    if (previous != nullptr && previous->valid &&
        DLSSMatricesMatch(prev_vp, previous->jittered_view_projection)) {
        const auto current_vp = DLSSMultiply(view, projection);
        for (int r = 0; r < 4; ++r) {
            prev_vp[r * 4] += 2.0f * previous->jitter_uv[0] * prev_vp[r * 4 + 3];
            prev_vp[r * 4 + 1] -= 2.0f * previous->jitter_uv[1] * prev_vp[r * 4 + 3];
        }
        DLSSMatrix inverse_vp;
        if (!DLSSInvert(current_vp, inverse_vp)) return out;
        out.clip_to_previous = DLSSMultiply(inverse_vp, prev_vp);
        if (!DLSSInvert(out.clip_to_previous, out.previous_to_clip)) return out;
        out.motion_jitter_delta = {out.jitter_uv[0] - previous->jitter_uv[0],
                                   out.jitter_uv[1] - previous->jitter_uv[1]};
        out.reset = !DLSSMatricesMatch(out.projection, previous->projection);
    }
    out.position = {view_inverse[12], view_inverse[13], view_inverse[14]};
    out.right = {view_inverse[0], view_inverse[1], view_inverse[2]};
    out.up = {view_inverse[4], view_inverse[5], view_inverse[6]};
    out.forward = {-view_inverse[8], -view_inverse[9], -view_inverse[10]};
    out.near_plane = v[106]; out.far_plane = v[107];
    if (out.near_plane <= 0 || out.far_plane <= out.near_plane) return out;
    out.fov = 2.0f * std::atan(1.0f / projection[5]);
    out.aspect = projection[5] / projection[0];
    out.valid = true;
    return out;
}

} // namespace Libs::Graphics
