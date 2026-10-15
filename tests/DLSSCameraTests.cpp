#include "graphics/presentation/dlss/dlssCamera.h"
#include <cstdio>
#include <limits>

using namespace Libs::Graphics;
int main() {
    int failures = 0;
    auto check = [&](bool ok, const char* name) {
        if (!ok) { std::printf("FAIL: %s\n", name); ++failures; }
    };
    // Captured Astro view block, 1708x960. Test actual projection/raster signs,
    // invalid inputs and temporal continuity rather than implementation details.
    std::array<float, 124> v {};
    v[0] = 1.20628512f; v[5] = 2.14450693f;
    v[10] = -1.00010002f; v[11] = -1; v[14] = -0.100010008f;
    v[16] = v[21] = v[26] = v[31] = 1;
    v[48] = v[53] = v[58] = v[63] = 1;
    v[106] = 0.100000009f; v[107] = 999.934143f;
    v[108] = 1708; v[109] = 960;
    auto set_jitter = [&](float x, float y) {
        v[96] = x / 1708; v[97] = y / 960;
        v[8] = 2 * v[96]; v[9] = -2 * v[97];
        std::copy_n(v.data(), 16, v.data() + 32);
    };
    set_jitter(0.15625f, -0.175926f);
    auto a = DecodeAstroDLSSCamera(v, 1708, 960);
    check(a.valid && a.reset, "first valid frame resets");
    check(std::abs(a.jitter_pixels[0] + 0.15625f) < 1e-6f &&
          std::abs(a.jitter_pixels[1] - 0.175926f) < 1e-6f, "raster jitter sign and render pixel units");
    check(a.projection[8] == 0 && a.projection[9] == 0, "unjittered projection");
    DLSSMatrix identity {}; identity[0] = identity[5] = identity[10] = identity[15] = 1;
    check(DLSSMatricesMatch(DLSSMultiply(a.projection, a.inverse_projection), identity), "projection inverse");
    std::copy_n(v.data() + 32, 16, v.data() + 80);
    set_jitter(-0.25f, 0.25f);
    auto b = DecodeAstroDLSSCamera(v, 1708, 960, &a);
    check(b.valid && !b.reset, "consecutive jittered frames retain history");
    check(DLSSMatricesMatch(b.clip_to_previous, identity), "stationary camera reprojection excludes jitter");
    check(std::abs(b.motion_jitter_delta[0] - (-0.40625f / 1708)) < 1e-7f, "motion jitter delta");
    std::copy_n(v.data(), 16, v.data() + 80);
    // Real block order: VP at 0, view at 64, projection at 128, inverse view at 192.
    // Translating view must not be mistaken for a non-perspective projection.
    v[28] = -2; v[60] = 2;
    DLSSMatrix moving_view, moving_projection;
    std::copy_n(v.data() + 16, 16, moving_view.data());
    std::copy_n(v.data() + 32, 16, moving_projection.data());
    auto moving_vp = DLSSMultiply(moving_view, moving_projection);
    std::copy_n(moving_vp.data(), 16, v.data());
    auto moving = DecodeAstroDLSSCamera(v, 1708, 960, &b);
    check(moving.valid && !moving.reset && moving.position[0] == 2, "moving camera block layout");
    check(!DLSSMatricesMatch(moving.clip_to_previous, identity) &&
          DLSSMatricesMatch(DLSSMultiply(moving.clip_to_previous, moving.previous_to_clip), identity),
          "moving camera reprojection and inverse");
    auto transform = [](const std::array<float, 4>& point, const DLSSMatrix& matrix) {
        std::array<float, 4> result {};
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r) result[c] += point[r] * matrix[r * 4 + c];
        return result;
    };
    const std::array<float, 4> point {0, 0, -4, 1};
    const auto current_clip = transform(point, DLSSMultiply(moving_view, moving.projection));
    const auto resolved_previous = transform(current_clip, moving.clip_to_previous);
    const auto expected_previous = transform(point, b.projection);
    // Float reprojection has cancellation with this 0.1..1000 depth range.
    // 5e-6 NDC is below 0.005 render pixels at this viewport.
    check(std::abs(resolved_previous[0] / resolved_previous[3] - expected_previous[0] / expected_previous[3]) < 5e-6f &&
          std::abs(resolved_previous[1] / resolved_previous[3] - expected_previous[1] / expected_previous[3]) < 5e-6f,
          "depth camera-motion reprojection agrees with independently projected static point");
    check(!DecodeAstroDLSSCamera(v, 1920, 1080).valid, "foreign viewport rejected");
    v[0] = std::numeric_limits<float>::quiet_NaN();
    check(!DecodeAstroDLSSCamera(v, 1708, 960).valid, "NaN rejected");
    v[0] = 1.20628512f; v[28] = v[60] = 0; set_jitter(0.9375f, -0.85185f);
    check(DecodeAstroDLSSCamera(v, 1708, 960).valid, "Astro gameplay jitter amplitude retained");
    set_jitter(2, 0);
    check(!DecodeAstroDLSSCamera(v, 1708, 960).valid, "out-of-range jitter rejected");
    DLSSMatrix singular {};
    check(!DLSSInvert(singular, identity), "singular matrix rejected");
    // Captured ASTRO BOT 01.018 view prefix from PS 92E45C84A2FF97DA.
    // Its 576-byte descriptor extends this 496-byte matrix/jitter/viewport prefix.
    const std::array<float, 124> bot_view {
        -0.803137362f, -0.00315293507f, -0.0219958778f, -0.0219944101f,
        -1.62719016e-06f, 1.42100763f, -0.0999812335f, -0.0999745578f,
        -0.0177416205f, 0.142598599f, 0.994813204f, 0.994746804f,
        -0.256908178f, -0.298909456f, 0.818720758f, 0.918666124f,
        -0.999755621f, -0.00220995327f, 0.0219944101f, 0.0f,
        0.0f, 0.994989932f, 0.0999745578f, 0.0f,
        -0.0221051574f, 0.0999501273f, -0.994746804f, 0.0f,
        -0.319821328f, -0.209204867f, -0.918666124f, 1.0f,
        0.803333342f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.42814815f, 0.0f, 0.0f,
        -1.62760425e-05f, 0.000145747588f, -1.00006676f, -1.0f,
        0.0f, 0.0f, -0.100006677f, 0.0f,
        -0.999755681f, 1.48405496e-10f, -0.0221051574f, 0.0f,
        -0.0022099535f, 0.994989991f, 0.0999501422f, -0.0f,
        0.0219944101f, 0.0999745727f, -0.994746923f, 0.0f,
        -0.299999982f, 0.299999982f, -0.899999976f, 1.0f,
        1.24481332f, -0.0f, -0.0f, -0.0f,
        -0.0f, 0.700207472f, -0.0f, -0.0f,
        -0.0f, -0.0f, -0.0f, -9.99933243f,
        -2.02606334e-05f, 0.000102053549f, -1.0f, 9.99999905f,
        -0.803135931f, -0.00315632927f, -0.0219958778f, -0.0219944101f,
        4.88157048e-06f, 1.42099226f, -0.0999812335f, -0.0999745578f,
        -0.0178063828f, 0.142752111f, 0.994813204f, 0.994746804f,
        -0.256967992f, -0.298767686f, 0.818720758f, 0.918666124f,
        -8.13802126e-06f, -7.28737941e-05f, 4.06901054e-05f, -4.71536332e-05f,
        -0.299999982f, 0.299999982f, -0.899999976f, 0.0f,
        1.00006664f, -0.100006677f, 0.100000009f, 1500.0f,
        1920.0f, 1080.0f, 2.0f, 0.0f,
        0.700207412f, -0.700207412f, -1.2448132f, 1.2448132f,
        -0.299999982f, 0.299999982f, -0.899999976f, 0.0f,
        1867.22046f, 1050.31152f, 1920.0f, 1080.0f,
    };
    const auto bot_camera = DecodeAstroDLSSCamera(bot_view, 1920, 1080);
    check(bot_camera.valid, "captured ASTRO BOT view block validates");
    check(bot_camera.valid && std::abs(bot_camera.near_plane - 0.1f) < 1e-5f &&
          std::abs(bot_camera.far_plane - 1500.0f) < 0.1f, "ASTRO BOT clipping planes");
    check(!DecodeAstroDLSSCamera(bot_view, 3840, 2160).valid, "guest 4K presentation is not the ASTRO BOT camera grid");
    std::printf("DLSS camera tests: %d failures\n", failures);
    return failures != 0;
}
