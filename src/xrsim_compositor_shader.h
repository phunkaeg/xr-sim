// The compositor's HLSL and the C++ mirror of its constant buffer.
//
// They live in one header so the two cannot drift apart, and so a test can compile
// and execute the exact bytes the compositor ships rather than a copy of them.
//
// One VS drives both passes: vertex id 0..2 makes a fullscreen triangle for the
// projection pass, 0..3 makes a quad in clip space for the quad pass.

#pragma once

#include <cstddef>

namespace xrsim {

inline constexpr char kCompositorShaderSource[] = R"HLSL(
cbuffer Constants : register(b0) {
    float4 eyeTan;      // l, r, u, d  (tangents of the EYE's fov)
    float4 layTan;      // l, r, u, d  (tangents of the LAYER's fov)
    float4 rectMin;     // xy = uv min of the submitted subimage
    float4 rectMax;     // xy = uv max
    float4 qRel;        // rotation from eye space into layer space
    // row_major is load-bearing. The C++ side builds mvp row-vector style
    // (translation in row 3, model * view * proj) and memcpy's Mat4::m, so the
    // bytes arrive rows-first. The default column_major packing reads them as
    // columns and the shader computes v * transpose(mvp): every non-identity quad
    // layer misplaced, while identity - its own transpose - passes. Guarded by
    // xrsim_compositor_probe.
    row_major float4x4 mvp;   // quad pass only
    float4 quadParams;  // x = pass (0 projection, 1 quad), y = flipY
};

struct VSOut {
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

#ifdef XRSIM_TEXTURE_ARRAY
Texture2DArray srcTex : register(t0);
#else
Texture2D      srcTex : register(t0);
#endif
SamplerState srcSmp : register(s0);

float4 sample_src(float2 uv) {
#ifdef XRSIM_TEXTURE_ARRAY
    // The SRV exposes exactly the submitted slice, starting at array layer 0.
    return srcTex.Sample(srcSmp, float3(uv, 0.0));
#else
    return srcTex.Sample(srcSmp, uv);
#endif
}

float3 rotate_by(float4 q, float3 v) {
    float3 u = q.xyz;
    float3 t = 2.0 * cross(u, v);
    return v + q.w * t + cross(u, t);
}

VSOut VSMain(uint vid : SV_VertexID) {
    VSOut o;
    if (quadParams.x < 0.5) {
        // Fullscreen triangle.
        float2 uv = float2((vid << 1) & 2, vid & 2);
        o.uv = uv;
        o.pos = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.0, 1.0);
    } else {
        // Unit quad in the layer's local plane, transformed by mvp.
        float2 c = float2((vid == 1 || vid == 3) ? 1.0 : -1.0,
                          (vid >= 2) ? -1.0 : 1.0);
        o.uv = float2(c.x * 0.5 + 0.5, 0.5 - c.y * 0.5);
        o.pos = mul(float4(c.x, c.y, 0.0, 1.0), mvp);
    }
    return o;
}

// Projection layer: for each output pixel, build the eye ray, rotate it into the
// layer's frame, project onto the layer's tangent plane, and sample there.
float4 PSProjection(VSOut i) : SV_TARGET {
    float tanX = lerp(eyeTan.x, eyeTan.y, i.uv.x);
    float tanY = lerp(eyeTan.z, eyeTan.w, i.uv.y);
    float3 dEye = normalize(float3(tanX, tanY, -1.0));
    float3 dLay = rotate_by(qRel, dEye);
    if (dLay.z >= -0.0001) return float4(0, 0, 0, 1);   // behind the layer plane

    float2 t = dLay.xy / -dLay.z;
    float2 lay = float2((t.x - layTan.x) / (layTan.y - layTan.x),
                        (t.y - layTan.z) / (layTan.w - layTan.z));
    if (lay.x < 0.0 || lay.x > 1.0 || lay.y < 0.0 || lay.y > 1.0)
        return float4(0, 0, 0, 1);                       // outside what was submitted

    float2 uv = lerp(rectMin.xy, rectMax.xy, lay);
    return float4(sample_src(uv).rgb, 1.0);
}

float4 PSQuad(VSOut i) : SV_TARGET {
    float2 uv = lerp(rectMin.xy, rectMax.xy, i.uv);
    return sample_src(uv);
}
)HLSL";

// C++ mirror of `cbuffer Constants`. It is filled field by field and the matrix is
// copied with memcpy from Mat4::m, so the bytes arrive in row-major memory order.
struct CompositorConstants {
    float eyeTan[4];
    float layTan[4];
    float rectMin[4];
    float rectMax[4];
    float qRel[4];
    float mvp[16];
    float quadParams[4];
};

static_assert(offsetof(CompositorConstants, mvp) == 80, "mvp must sit at cbuffer register 5");
static_assert(sizeof(CompositorConstants) == 160, "the cbuffer is ten float4 registers");

} // namespace xrsim
