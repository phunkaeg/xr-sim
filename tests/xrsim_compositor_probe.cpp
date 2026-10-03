// Executes the compositor's quad-pass vertex shader - the exact bytes in
// xrsim_compositor_shader.h - on WARP, and checks that every corner lands where
// the C++ row-vector math says it should.
//
// Why this exists: the quad pass transposed its matrix. The C++ side builds mvp
// in row-vector convention (translation in row 3, mvp = model * view * proj) and
// memcpy's Mat4::m - row-major memory - into the cbuffer. HLSL's default
// column_major packing reads those bytes as columns, so the shader computed
// v * transpose(mvp). Every non-identity quad layer was misplaced: displaced
// beams, skewed panels. Identity survives a transpose, so a check that only ever
// exercised identity could not see it.
//
// So every case here except the control is deliberately non-identity, and the
// probe proves it can see the bug: uploading transpose(mvp) must FAIL the same
// comparison. A check that has never been observed failing is decoration.
//
// Exit codes follow xrsim_probe: 0 pass, 1 fail, 77 device skip.

#include <openxr/openxr.h>

#include "xrsim_compositor_shader.h"
#include "xrsim_math.h"

#include <d3d11.h>
#include <d3dcompiler.h>

#include <cmath>
#include <cstdio>
#include <cstring>

using namespace xrsim;

namespace {

template <typename T>
void release(T*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

// Must mirror VSMain's quad branch exactly.
void corner(unsigned vid, float& x, float& y) {
    x = (vid == 1 || vid == 3) ? 1.0f : -1.0f;
    y = (vid >= 2) ? -1.0f : 1.0f;
}

// Row-vector reference: out = (x, y, 0, 1) * M, which is what the C++ side means.
void reference(const Mat4& M, unsigned vid, float out[4]) {
    float x, y;
    corner(vid, x, y);
    const float v[4] = {x, y, 0.0f, 1.0f};
    for (int c = 0; c < 4; ++c)
        out[c] = v[0] * M.m[0][c] + v[1] * M.m[1][c] + v[2] * M.m[2][c] + v[3] * M.m[3][c];
}

Mat4 transpose(const Mat4& a) {
    Mat4 o{};
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) o.m[r][c] = a.m[c][r];
    return o;
}

// The compositor's quad-pass model matrix: basis rows scaled by the HALF extent,
// translation in row 3.
Mat4 quad_model(const Pose& quadWorld, float width, float height) {
    Mat4 m = mat4_identity();
    const Vec3 rx = quat_rotate(quadWorld.q, Vec3{width * 0.5f, 0.0f, 0.0f});
    const Vec3 ry = quat_rotate(quadWorld.q, Vec3{0.0f, height * 0.5f, 0.0f});
    const Vec3 rz = quat_rotate(quadWorld.q, Vec3{0.0f, 0.0f, 1.0f});
    m.m[0][0] = rx.x; m.m[0][1] = rx.y; m.m[0][2] = rx.z;
    m.m[1][0] = ry.x; m.m[1][1] = ry.y; m.m[1][2] = ry.z;
    m.m[2][0] = rz.x; m.m[2][1] = rz.y; m.m[2][2] = rz.z;
    m.m[3][0] = quadWorld.p.x;
    m.m[3][1] = quadWorld.p.y;
    m.m[3][2] = quadWorld.p.z;
    return m;
}

Mat4 full_mvp(const Pose& quadWorld, float w, float h, const Pose& eye, const Fov& fov) {
    return mat4_mul(mat4_mul(quad_model(quadWorld, w, h), mat4_view_from_pose(eye)),
                    mat4_projection_fov(fov, 0.02f, 1000.0f));
}

struct Gpu {
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    ID3D11VertexShader* vs = nullptr;
    ID3D11GeometryShader* so = nullptr;
    ID3D11Buffer* cb = nullptr;
    ID3D11Buffer* soBuf = nullptr;
    ID3D11Buffer* staging = nullptr;

    ~Gpu() {
        release(staging); release(soBuf); release(cb);
        release(so); release(vs); release(ctx); release(dev);
    }

    // Upload exactly as the compositor does, stream out VSMain's four SV_POSITIONs.
    bool run(const Mat4& mvp, float out[4][4]) {
        CompositorConstants c{};
        memcpy(c.mvp, mvp.m, sizeof(c.mvp));
        c.quadParams[0] = 1.0f;
        ctx->UpdateSubresource(cb, 0, nullptr, &c, 0, 0);

        const UINT offset = 0;
        ctx->IASetInputLayout(nullptr);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
        ctx->VSSetShader(vs, nullptr, 0);
        ctx->VSSetConstantBuffers(0, 1, &cb);
        ctx->GSSetShader(so, nullptr, 0);
        ctx->SOSetTargets(1, &soBuf, &offset);
        ctx->Draw(4, 0);
        ID3D11Buffer* none = nullptr;
        ctx->SOSetTargets(1, &none, &offset);

        ctx->CopyResource(staging, soBuf);
        D3D11_MAPPED_SUBRESOURCE m{};
        if (FAILED(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &m))) return false;
        memcpy(out, m.pData, sizeof(float) * 16);
        ctx->Unmap(staging, 0);
        return true;
    }
};

// Returns 0 ok, 77 skip, 1 fail.
int init(Gpu& g) {
    D3D_FEATURE_LEVEL fl{};
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
                                 D3D11_SDK_VERSION, &g.dev, &fl, &g.ctx))) {
        std::printf("SKIP: no WARP device\n");
        return 77;
    }
    ID3DBlob* vsb = nullptr;
    ID3DBlob* err = nullptr;
    if (FAILED(D3DCompile(kCompositorShaderSource, std::strlen(kCompositorShaderSource), "xrsim",
                          nullptr, nullptr, "VSMain", "vs_4_0", D3DCOMPILE_OPTIMIZATION_LEVEL1, 0,
                          &vsb, &err))) {
        std::printf("FAIL: VSMain compile: %s\n",
                    err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
        release(err);
        return 1;
    }
    // Stream output straight from the vertex shader: no geometry stage, no raster.
    const D3D11_SO_DECLARATION_ENTRY decl{0, "SV_POSITION", 0, 0, 4, 0};
    const UINT stride = sizeof(float) * 4;
    const bool ok =
        SUCCEEDED(g.dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr,
                                            &g.vs)) &&
        SUCCEEDED(g.dev->CreateGeometryShaderWithStreamOutput(
            vsb->GetBufferPointer(), vsb->GetBufferSize(), &decl, 1, &stride, 1,
            D3D11_SO_NO_RASTERIZED_STREAM, nullptr, &g.so));
    release(vsb);
    release(err);
    if (!ok) {
        std::printf("FAIL: shader objects\n");
        return 1;
    }

    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = sizeof(CompositorConstants);
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(g.dev->CreateBuffer(&bd, nullptr, &g.cb))) return 1;

    bd.ByteWidth = sizeof(float) * 16;
    bd.BindFlags = D3D11_BIND_STREAM_OUTPUT;
    if (FAILED(g.dev->CreateBuffer(&bd, nullptr, &g.soBuf))) return 1;

    bd.Usage = D3D11_USAGE_STAGING;
    bd.BindFlags = 0;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(g.dev->CreateBuffer(&bd, nullptr, &g.staging))) return 1;
    return 0;
}

float max_error(const Mat4& expected, const float got[4][4]) {
    float worst = 0.0f;
    for (unsigned vid = 0; vid < 4; ++vid) {
        float ref[4];
        reference(expected, vid, ref);
        for (int c = 0; c < 4; ++c) {
            const float e = std::fabs(got[vid][c] - ref[c]) / (1.0f + std::fabs(ref[c]));
            if (e > worst) worst = e;
        }
    }
    return worst;
}

} // namespace

int main() {
    Gpu g;
    if (const int rc = init(g)) return rc;

    const Fov symmetric{deg2rad(-45.0f), deg2rad(45.0f), deg2rad(45.0f), deg2rad(-45.0f)};
    const Fov headset{-0.80f, 0.70f, 0.75f, -0.85f};   // asymmetric, as real optics are
    const Pose eyeL{quat_from_ypr(deg2rad(4.0f), deg2rad(-3.0f), 0.0f), Vec3{-0.032f, 1.70f, 0.0f}};

    struct Case {
        const char* name;
        Mat4 mvp;
        bool control;
    };
    const Case cases[] = {
        {"identity (control: blind to a transpose)", mat4_identity(), true},
        {"translated quad", quad_model(Pose{quat_identity(), Vec3{0.3f, -0.2f, -1.5f}}, 2.0f, 2.0f), false},
        {"yawed quad", quad_model(Pose{quat_from_ypr(deg2rad(30.0f), 0, 0), Vec3{0, 0, -2.0f}}, 2.0f, 2.0f), false},
        {"non-uniform extent", quad_model(Pose{quat_identity(), Vec3{0.1f, 0.2f, -1.0f}}, 0.6f, 0.35f), false},
        {"projection only, symmetric fov", mat4_projection_fov(symmetric, 0.02f, 1000.0f), false},
        {"HUD panel through an offset eye",
         full_mvp(Pose{quat_identity(), Vec3{0.0f, 1.60f, -1.0f}}, 0.5f, 0.3f, eyeL, symmetric), false},
        {"pitched + rolled panel, asymmetric fov",
         full_mvp(Pose{quat_from_ypr(deg2rad(-20.0f), deg2rad(15.0f), deg2rad(10.0f)), Vec3{0.4f, 1.3f, -0.8f}},
                  0.4f, 0.25f, eyeL, headset), false},
        {"thin far beam",
         full_mvp(Pose{quat_from_ypr(deg2rad(60.0f), deg2rad(-35.0f), 0), Vec3{-0.2f, 1.2f, -6.0f}},
                  0.01f, 4.0f, eyeL, headset), false},
    };

    // The pass bar comes from float32 arithmetic; the miss bar from the size of the
    // defect. Measured headroom between them is several orders of magnitude.
    const float passTol = 1e-5f;
    const float missTol = 1e-2f;
    int failures = 0;

    for (const Case& k : cases) {
        float got[4][4], gotT[4][4];
        if (!g.run(k.mvp, got) || !g.run(transpose(k.mvp), gotT)) {
            std::printf("FAIL  %-42s readback failed\n", k.name);
            return 1;
        }
        const float err = max_error(k.mvp, got);
        const float errT = max_error(k.mvp, gotT);
        bool pass = err <= passTol;
        // The negative control: a transposed upload must be caught, except for the
        // identity case where it is mathematically invisible.
        const bool controlOk = k.control ? errT <= passTol : errT > missTol;
        if (!controlOk) pass = false;
        // Name the finding, not just the failure.
        const char* note = "";
        if (k.control && !controlOk)
            note = "  <- control misbehaved: the harness itself is wrong";
        else if (!k.control && err > missTol && errT <= passTol)
            note = "  <- shader reads mvp TRANSPOSED: only a transposed upload lands correctly";
        else if (!k.control && errT <= missTol)
            note = "  <- this case cannot detect a transpose (matrix is near-symmetric)";
        std::printf("%s  %-42s err=%.3g  transposed-upload err=%.3g%s\n", pass ? "PASS" : "FAIL",
                    k.name, err, errT, note);
        if (!pass) ++failures;
    }

    std::printf("%s: %d of %zu cases failed\n", failures ? "FAIL" : "PASS", failures,
                sizeof(cases) / sizeof(cases[0]));
    return failures ? 1 : 0;
}
