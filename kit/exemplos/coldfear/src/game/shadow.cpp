// Cold Fear's lamps and flashlight test a depth texture (shadow func GREATER: lit while the stored depth is past r).
// The plain compare flickers along the surfaces the lamp sees, so r moves towards lit by a few depth units and four
// taps half a texel apart average the result, as the old port's renderer did.
#include "gpu/shaders.h"

namespace {

struct ColdFearShaders : gpu::ShaderHooks {
    std::string Shadow() const override {
        return R"(// biased towards lit, four taps half a texel apart
float4 shadow(Texture2D<float> t, SamplerComparisonState s, float2 uv, float z, int fmt, float2 px, float2 size) {
    float mx = fmt == 2 ? 65535.0 : 16777215.0;
    // 128 units of a 16 bit buffer (the old port's default), and more where the depth changes fast across the
    // screen: surfaces the light grazes striped (acne) with a fixed bias
    float unit = fmt == 2 ? 1.0 : 256.0;
    float slope = max(abs(ddx(z)), abs(ddy(z)));
    float bias = 128.0 * unit + 2.0 * slope;
    float r = clamp(z - bias, 0, mx) / mx;
    float2 h = 0.5 / max(size, float2(1, 1));
    float v = t.SampleCmpLevelZero(s, uv + float2(-h.x, -h.y), r);
    v += t.SampleCmpLevelZero(s, uv + float2(h.x, -h.y), r);
    v += t.SampleCmpLevelZero(s, uv + float2(-h.x, h.y), r);
    v += t.SampleCmpLevelZero(s, uv + float2(h.x, h.y), r);
    return (v * 0.25).xxxx;
}
)";
    }
};

const ColdFearShaders g_shaders;
const bool g_installed = (gpu::g_shader_hooks = &g_shaders, true);

}  // namespace
