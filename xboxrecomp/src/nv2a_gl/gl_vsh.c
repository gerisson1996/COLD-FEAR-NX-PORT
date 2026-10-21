/*
 * gl_vsh.c -- NV2A vertex microcode to GLSL.
 *
 * The translation mirrors the executor's CPU interpreter (vp_run in
 * nv2a_pb_exec.c) instruction for instruction: the same field decode (the
 * 128-bit layout documented by xemu's vsh.c), the same parallel issue -- MAC
 * and ILU read every source before either writes -- and the same output
 * registers. The two are kept deliberately alike so a picture that differs
 * between the CPU and GPU renderers points at the renderer, not at the
 * decode.
 *
 * All three transform paths end the same way. The shader produces a
 * position in real surface pixels with w still the clip w, and turns that
 * into GL clip space as (screen * w) mapped to NDC. For every path the
 * executor handles, screen*w is linear in the clip-space position, so the
 * GPU's own near-plane clipping is exact -- no CPU clipping is needed.
 *
 * Surface rows are top-first in guest memory and in the framebuffer objects
 * alike (y_ndc = 2y/H - 1), so a surface later sampled as a texture needs no
 * flip; only presentation flips.
 */
#include "gl_vsh.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    char  *p;
    size_t left;
    int    overflow;
} Out;

static void emit(Out *o, const char *fmt, ...)
{
    va_list ap;
    int n;

    if (o->overflow)
        return;
    va_start(ap, fmt);
    n = vsnprintf(o->p, o->left, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= o->left) {
        o->overflow = 1;
        return;
    }
    o->p += n;
    o->left -= (size_t)n;
}

static uint32_t f(const uint32_t *t, int dw, int pos, int bits)
{
    return (t[dw] >> pos) & ((1u << bits) - 1u);
}

static const char *const s_swz = "xyzw";
extern int nv2a_shader_vk;

/* Source A (0), B (1) or C (2) as a GLSL vec4 expression. */
static void src(Out *o, const uint32_t *t, int which, int consts_local)
{
    uint32_t mux, reg, neg, sw;
    char comp[5];

    switch (which) {
    case 0:  mux = f(t, 2, 26, 2); reg = f(t, 2, 28, 4);
             neg = f(t, 1, 8, 1);  sw = f(t, 1, 0, 8);  break;
    case 1:  mux = f(t, 2, 11, 2); reg = f(t, 2, 13, 4);
             neg = f(t, 2, 25, 1); sw = f(t, 2, 17, 8); break;
    default: mux = f(t, 3, 28, 2);
             reg = (f(t, 2, 0, 2) << 2) | f(t, 3, 30, 2);
             neg = f(t, 2, 10, 1); sw = f(t, 2, 2, 8);  break;
    }
    comp[0] = s_swz[(sw >> 6) & 3];
    comp[1] = s_swz[(sw >> 4) & 3];
    comp[2] = s_swz[(sw >> 2) & 3];
    comp[3] = s_swz[sw & 3];
    comp[4] = 0;

    emit(o, "%s", neg ? "-" : "");
    if (mux == 1) {
        if (reg < 12)
            emit(o, "R%u.%s", reg, comp);
        else if (reg == 12)
            emit(o, "oPos.%s", comp);
        else
            emit(o, "vec4(0.0).%s", comp);
    } else if (mux == 2) {
        emit(o, "v%u.%s", f(t, 1, 9, 4), comp);
    } else if (mux == 3) {
        uint32_t ci = f(t, 1, 13, 8);
        const char *arr = consts_local ? "cl" : "c";
        /* Vulkan: one 192-constant set per instance (nv2a_vk instancing). */
        const char *base = nv2a_shader_vk && !consts_local ? "nv2a_ib + " : "";
        if (f(t, 3, 1, 1))
            emit(o, "%s[%sclamp(%u + a0, 0, 191)].%s", arr, base, ci, comp);
        else
            emit(o, "%s[%s%u].%s", arr, base, ci < 192 ? ci : 191, comp);
    } else {
        emit(o, "vec4(0.0).%s", comp);
    }
}

/* ".xz" for mask bits 3=x .. 0=w; empty when nothing is written. */
static void mask_str(uint32_t mask, char out[6])
{
    int i, n = 0;

    out[n++] = '.';
    for (i = 0; i < 4; i++)
        if (mask & (8u >> i))
            out[n++] = s_swz[i];
    out[n] = 0;
}

/* dst.mask = val.mask; */
static void write_masked(Out *o, const char *dst, uint32_t mask, const char *val)
{
    char m[6];

    if (!mask)
        return;
    mask_str(mask, m);
    emit(o, "        %s%s = (%s)%s;\n", dst, m, val, m);
}

static const char *const s_out_names[13] = {
    "oPos", "o1", "o2", "oD0", "oD1", "oFog", "oPts", "oB0", "oB1",
    "oT0", "oT1", "oT2", "oT3",
};

int nv2a_gl_vsh_program(const uint32_t (*prog)[4], uint32_t slots,
                        uint32_t start, char *buf, size_t cap)
{
    Out o = { buf, cap, 0 };
    uint32_t pc;
    int writes_consts = 0;

    for (pc = start; pc < slots; pc++) {
        const uint32_t *t = prog[pc];
        if (f(t, 3, 12, 4) && !f(t, 3, 11, 1))
            writes_consts = 1;
        if (f(t, 3, 0, 1))
            break;
    }

    emit(&o, "void nv2a_program(void)\n{\n");
    emit(&o, "    int a0 = 0;\n");
    if (writes_consts)
        emit(&o, "    vec4 cl[192];\n    for (int i = 0; i < 192; i++) cl[i] = c[%si];\n",
             nv2a_shader_vk ? "nv2a_ib + " : "");

    for (pc = start; pc < slots; pc++) {
        const uint32_t *t = prog[pc];
        uint32_t mac = f(t, 1, 21, 4), ilu = f(t, 1, 25, 3);
        uint32_t omask = f(t, 3, 12, 4);
        uint32_t rd = f(t, 3, 20, 4);

        emit(&o, "    { /* %u */\n        vec4 A = ", pc);
        src(&o, t, 0, writes_consts);
        emit(&o, ";\n        vec4 B = ");
        src(&o, t, 1, writes_consts);
        emit(&o, ";\n        vec4 C = ");
        src(&o, t, 2, writes_consts);
        emit(&o, ";\n        vec4 m = vec4(0.0), l = vec4(0.0);\n");

        switch (mac) {
        case 1:  emit(&o, "        m = A;\n"); break;
        case 2:  emit(&o, "        m = A * B;\n"); break;
        case 3:  emit(&o, "        m = A + C;\n"); break;
        case 4:  emit(&o, "        m = A * B + C;\n"); break;
        case 5:  emit(&o, "        m = vec4(dot(A.xyz, B.xyz));\n"); break;
        case 6:  emit(&o, "        m = vec4(dot(A.xyz, B.xyz) + B.w);\n"); break;
        case 7:  emit(&o, "        m = vec4(dot(A, B));\n"); break;
        case 8:  emit(&o, "        m = vec4(1.0, A.y * B.y, A.z, B.w);\n"); break;
        case 9:  emit(&o, "        m = min(A, B);\n"); break;
        case 10: emit(&o, "        m = max(A, B);\n"); break;
        case 11: emit(&o, "        m = vec4(lessThan(A, B));\n"); break;
        case 12: emit(&o, "        m = vec4(greaterThanEqual(A, B));\n"); break;
        default: break;
        }
        switch (ilu) {
        case 1: emit(&o, "        l = C;\n"); break;
        case 2: emit(&o, "        l = vec4(nv2a_rcp(C.x));\n"); break;
        case 3: emit(&o, "        l = vec4(nv2a_rcc(C.x));\n"); break;
        case 4: emit(&o, "        l = vec4(nv2a_rcp(sqrt(abs(C.x))));\n"); break;
        case 5: emit(&o, "        l = nv2a_exp(C.x);\n"); break;
        case 6: emit(&o, "        l = nv2a_log(C.x);\n"); break;
        case 7: emit(&o, "        l = nv2a_lit(C);\n"); break;
        default: break;
        }

        if (mac == 13) {
            emit(&o, "        a0 = int(floor(A.x + 0.001));\n");
        } else if (mac && rd < 13) {
            char dst[8];
            if (rd == 12) snprintf(dst, sizeof dst, "oPos");
            else          snprintf(dst, sizeof dst, "R%u", rd);
            write_masked(&o, dst, f(t, 3, 24, 4), "m");
        }
        if (ilu) {
            uint32_t ld = mac ? 1 : rd;              /* paired ILU -> R1 */
            if (ld < 13) {
                char dst[8];
                if (ld == 12) snprintf(dst, sizeof dst, "oPos");
                else          snprintf(dst, sizeof dst, "R%u", ld);
                write_masked(&o, dst, f(t, 3, 16, 4), "l");
            }
        }
        if (omask) {
            const char *val = f(t, 3, 2, 1) ? "l" : "m";
            uint32_t addr = f(t, 3, 3, 8);
            if (f(t, 3, 11, 1)) {
                if (addr < 13)
                    write_masked(&o, s_out_names[addr], omask, val);
            } else {
                char dst[48];
                if (f(t, 3, 1, 1))
                    snprintf(dst, sizeof dst, "cl[clamp(%u + a0, 0, 191)]", addr);
                else
                    snprintf(dst, sizeof dst, "cl[%u]", addr < 192 ? addr : 191);
                write_masked(&o, dst, omask, val);
            }
        }
        emit(&o, "    }\n");
        if (f(t, 3, 0, 1))                          /* FINAL */
            break;
    }
    emit(&o, "}\n");
    return o.overflow ? -1 : (int)(cap - o.left);
}

/* The NV2A rasterizer takes screen positions as fixed point with a 4-bit
 * fraction, truncated (xemu's roundScreenCoords). Xbox D3D's viewport
 * offset carries a +0.53125 bias, so a clip-space full-screen pass starts at
 * 0.53125 -- 0.5 on the hardware, which covers row and column 0; unsnapped,
 * GL left them out and NFSU2's glow buffer kept a stale edge that its
 * composite added back as a light line at the top and left.
 * Under a render scale k (RECOMP_GL_SCALE) a title pixel is k x k real
 * pixels, and the first real centre of title pixel i sits at i + 0.5/k, not
 * i + 0.5: an edge snapped to 0.5 still missed real row/column 0. u_surf.w =
 * 0.5 - 0.5/k moves positions so that real centre lands on i + 0.5 (0 at
 * k = 1). */
#define NV2A_SNAP_GLSL \
    "vec2 nv2a_snap(vec2 sw, float w) {\n" \
    "    return w > 0.0 ? (trunc(sw / w * 16.0) / 16.0 - u_surf.w) * w : sw;\n" \
    "}\n"

/* Everything around the program body: inputs, the register file, the ILU
 * helpers (same constants as the interpreter), and the three position
 * paths. u_xform selects the path at run time so one program object serves
 * a vertex program under any surface. */
/* Vulkan GLSL (nv2a_vk): the same shader with explicit locations, the
 * uniforms in a std140 block (binding 0) and Vulkan's 0..1 clip depth. */
static const char s_vk_prelude[] =
    "#version 450\n"
    "layout(location = 0) in vec4 v0;\n"
    "layout(location = 1) in vec4 v1;\n"
    "layout(location = 2) in vec4 v2;\n"
    "layout(location = 3) in vec4 v3;\n"
    "layout(location = 4) in vec4 v4;\n"
    "layout(location = 5) in vec4 v5;\n"
    "layout(location = 6) in vec4 v6;\n"
    "layout(location = 7) in vec4 v7;\n"
    "layout(location = 8) in vec4 v8;\n"
    "layout(location = 9) in vec4 v9;\n"
    "layout(location = 10) in vec4 v10;\n"
    "layout(location = 11) in vec4 v11;\n"
    "layout(location = 12) in vec4 v12;\n"
    "layout(location = 13) in vec4 v13;\n"
    "layout(location = 14) in vec4 v14;\n"
    "layout(location = 15) in vec4 v15;\n"
    "layout(std140, set = 0, binding = 0) uniform VsU {\n"
    "    vec4 u_surf;\n"
    "    vec4 u_m[4];\n"
    "    vec4 u_vpoff;\n"
    "    vec2 u_aa;\n"
    "    int u_xform;\n"
    "    int u_zcull;\n"
    "    vec4 u_fog;               /* FOG_PARAMS 0, 1; mode; enable */\n"
    "    ivec4 u_lctl;             /* lighting on, light mask, colour material */\n"
    "    vec4 u_mv[4];             /* model-view rows */\n"
    "    vec4 u_lscene;            /* scene ambient, material alpha */\n"
    "    vec4 u_lemis;             /* material emission */\n"
    "    vec4 u_light[48];         /* 6 per light, see nv2a_light */\n"
    "    ivec4 u_tctl;             /* texgen / texture matrices in use */\n"
    "    ivec4 u_tmen;             /* texture matrix enable per stage */\n"
    "    ivec4 u_tgen[4];          /* texgen mode per stage and coordinate */\n"
    "    vec4 u_tmat[16];          /* texture matrix rows */\n"
    "    vec4 u_tplane[16];        /* texgen planes S/T/R/Q */\n"
    "};\n"
    /* Transform constants, 192 per instance: an instanced draw (nv2a_vk.c)
     * repeats one mesh with each instance's own set. NV2A_VK_INSTANCES. */
    "layout(std140, set = 0, binding = 6) uniform VsC { vec4 c[192 * 16]; };\n"
    "int nv2a_ib;\n"
    "layout(location = 0) out vec4 vD0; layout(location = 1) out vec4 vD1;\n"
    "layout(location = 2) out vec4 vT0; layout(location = 3) out vec4 vT1;\n"
    "layout(location = 4) out vec4 vT2; layout(location = 5) out vec4 vT3;\n"
    "layout(location = 6) out float vFog;\n"
    /* Multi-pass rendering draws the same mesh again (depth LEQUAL/EQUAL):
     * each pass is its own program, and without invariance the compiler may
     * contract the shared position maths differently in each (an fma here,
     * not there), leaving the passes an ulp apart -- z-fighting at best. */
    "invariant gl_Position;\n"
    "vec4 R0, R1, R2, R3, R4, R5, R6, R7, R8, R9, R10, R11;\n"
    "vec4 oPos, o1, o2, oD0, oD1, oFog, oPts, oB0, oB1, oT0, oT1, oT2, oT3;\n"
    "float nv2a_rcp(float x) { return x != 0.0 ? 1.0 / x : 1.884467e+19; }\n"
    "float nv2a_rcc(float x) {\n"
    "    float y = nv2a_rcp(x); float a = clamp(abs(y), 5.42101e-20, 1.884467e+19);\n"
    "    return y < 0.0 ? -a : a;\n"
    "}\n"
    "vec4 nv2a_exp(float x) { float f = floor(x); return vec4(exp2(f), x - f, exp2(x), 1.0); }\n"
    "vec4 nv2a_log(float x) {\n"
    "    float a = abs(x);\n"
    "    if (a == 0.0) return vec4(-1.884467e+19, 1.0, -1.884467e+19, 1.0);\n"
    "    float e = floor(log2(a));\n"
    "    return vec4(e, a / exp2(e), log2(a), 1.0);\n"
    "}\n"
    "vec4 nv2a_lit(vec4 s) {\n"
    "    float lx = max(s.x, 0.0), ly = max(s.y, 0.0), w = clamp(s.w, -127.996, 127.996);\n"
    "    return vec4(1.0, lx, (lx > 0.0 && ly > 0.0) ? exp2(w * log2(ly)) : 0.0, 1.0);\n"
    "}\n"
    /* z clamped only in front of the eye (w > 0). Behind it (w < 0) the
     * vertex is clipped by x/y anyway and its true z is about w: clamping it
     * to 0 skewed the interpolated depth of triangles crossing the eye plane
     * (walls in hood view turned transparent, objects showed through them). */
    /* u_zcull: SET_ZMIN_MAX_CONTROL asks for near/far culling rather than
     * clamping, so geometry between the eye and the near plane is cut away
     * as on the NV2A instead of being drawn clamped to depth 0 (Cold Fear's
     * ship decks turned into wedges across the screen). Depth clamp stays on
     * in the pipeline, so the cut is done with clip distances. */
    NV2A_SNAP_GLSL
    "vec4 nv2a_clip(vec3 sw, float w) {\n"
    "    float z = sw.z * u_surf.z;\n"
    "    sw.xy = nv2a_snap(sw.xy, w);\n"
    "    gl_ClipDistance[0] = u_zcull != 0 ? z : 1.0;\n"
    "    gl_ClipDistance[1] = u_zcull != 0 ? w - z : 1.0;\n"
    "    return vec4(sw.x * u_surf.x - w, sw.y * u_surf.y - w,\n"
    "                w > 0.0 ? clamp(z, 0.0, w) : z, w);\n"
    "}\n"
    /* The fog unit. A program's oFog.x (or the fixed pipeline's eye
     * distance) is a fog DISTANCE; the factor the final combiner reads as
     * FOG.a comes from SET_FOG_MODE / SET_FOG_PARAMS, as xemu computes it.
     * Passing the distance through clamped every factor to 0 or 1: Cold
     * Fear's ship drowned in its blue fog colour. Fog off: factor 1. */
    "float nv2a_fog(float d) {\n"
    "    if (u_fog.w == 0.0) return 1.0;\n"
    "    int m = int(u_fog.z);\n"
    "    if (m >= 4) d = abs(d);\n"
    "    if ((m & 3) == 0) return u_fog.x + d * u_fog.y - 1.0;\n"
    "    if ((m & 3) == 1) return u_fog.x + exp2(d * u_fog.y * 16.0) - 1.5;\n"
    "    return u_fog.x + exp2(-d * d * u_fog.y * u_fog.y * 32.0) - 1.5;\n"
    "}\n"
    "float nv2a_fog_ffp(float d) { return nv2a_fog(d); }\n"
    /* Fixed-function lighting, as xemu does it. XDK D3D pre-multiplies the
     * material into the light colours. The base colour is the ambient source
     * times MATERIAL_EMISSION plus the emission source: with both from the
     * material, D3D folds everything into SCENE_AMBIENT and zeroes the
     * emission register; with the ambient from the vertex it puts the global
     * ambient in the emission register. Colour material sources: 0 material,
     * 1 diffuse, 2 specular. Lights (mask, 2 bits each): 1 infinite,
     * 2 local, 3 spot. Per light in u_light: ambient + range, diffuse, eye
     * position, attenuation, direction (infinite), spot direction.
     * Normals go through the model-view rows (exact for rotations and
     * uniform scale). No specular yet: D1 keeps only the specular alpha. */
    "void nv2a_light(inout vec4 d0, inout vec4 d1) {\n"
    "    vec4 p = vec4(v0.xyz, 1.0);\n"
    "    vec3 e = vec3(dot(u_mv[0], p), dot(u_mv[1], p), dot(u_mv[2], p));\n"
    "    vec3 n = vec3(dot(u_mv[0].xyz, v2.xyz), dot(u_mv[1].xyz, v2.xyz), dot(u_mv[2].xyz, v2.xyz));\n"
    "    float nl = length(n);\n"
    "    if (nl > 1e-8) n /= nl;\n"
    /* u_lctl.w: light the side the viewer sees. Cold Fear's ceilings carry
     * normals facing away from the room, and lit as stored they took no
     * light from the lamps below: black clouds across the ceiling. */
    "    if (u_lctl.w != 0 && dot(n, e) > 0.0) n = -n;\n"
    "    int cm = u_lctl.z;\n"
    "    int se = cm & 3, sa = (cm >> 2) & 3, sd = (cm >> 4) & 3;\n"
    "    vec4 dif = v3, spe = v4;\n"
    "    vec3 amb = sa == 1 ? dif.rgb : sa == 2 ? spe.rgb : u_lscene.rgb;\n"
    "    vec3 c = amb * u_lemis.rgb + (se == 1 ? dif.rgb : se == 2 ? spe.rgb : u_lscene.rgb);\n"
    "    float a = sd == 1 ? dif.a : sd == 2 ? spe.a : u_lscene.a;\n"
    "    for (int i = 0; i < 8; i++) {\n"
    "        int kind = (u_lctl.y >> (2 * i)) & 3;\n"
    "        if (kind == 0) continue;\n"
    "        int b = i * 6;\n"
    "        vec3 l; float att = 1.0;\n"
    "        if (kind == 1) {\n"
    "            l = normalize(u_light[b + 4].xyz);\n"
    "        } else {\n"
    "            vec3 vp = u_light[b + 2].xyz - e;\n"
    "            float d = length(vp);\n"
    "            if (u_light[b].w > 0.0 && d > u_light[b].w) continue;\n"
    "            l = d > 1e-8 ? vp / d : vp;\n"
    "            vec3 k = u_light[b + 3].xyz;\n"
    "            float q = k.x + k.y * d + k.z * d * d;\n"
    "            att = q > 1e-8 ? 1.0 / q : 1.0;\n"
    "            if (kind == 3) {\n"
    "                vec4 sdir = u_light[b + 5];\n"
    "                float inv = 1.0 / max(length(sdir.xyz), 1e-8);\n"
    "                float cphi = -inv * sdir.w, ctheta = inv + cphi;\n"
    "                float sdl = dot(sdir.xyz, l), rho = inv * sdl;\n"
    "                if (rho <= cphi) att = 0.0;\n"
    "                else if (rho <= ctheta) att *= sdl + sdir.w;\n"
    "            }\n"
    "        }\n"
    "        vec3 ld = u_light[b + 1].rgb * (att * max(dot(n, l), 0.0));\n"
    "        c += u_light[b].rgb * att;\n"
    "        c += sd == 1 ? dif.rgb * ld : sd == 2 ? spe.rgb * ld : ld;\n"
    "    }\n"
    "    d0 = vec4(c, a);\n"
    "    d1 = vec4(0.0, 0.0, 0.0, spe.a);\n"
    "}\n"
    /* Texture coordinate generation, then the texture matrix, per stage,
     * as xemu does it: eye linear dots the plane with the eye-space
     * position, object linear with the vertex position; sphere, normal and
     * reflection maps from the eye-space normal. Without them a projected
     * texture (a light or a shadow buffer) read the mesh's own UVs. */
    "void nv2a_texgen(inout vec4 t[4]) {\n"
    "    vec4 p = vec4(v0.xyz, 1.0);\n"
    "    vec4 e = vec4(dot(u_mv[0], p), dot(u_mv[1], p), dot(u_mv[2], p), dot(u_mv[3], p));\n"
    "    vec3 n = vec3(dot(u_mv[0].xyz, v2.xyz), dot(u_mv[1].xyz, v2.xyz), dot(u_mv[2].xyz, v2.xyz));\n"
    "    float nl = length(n);\n"
    "    if (nl > 1e-8) n /= nl;\n"
    "    float el = length(e.xyz);\n"
    "    vec3 rf = reflect(el > 1e-8 ? e.xyz / el : e.xyz, n);\n"
    "    float m = 2.0 * sqrt(rf.x * rf.x + rf.y * rf.y + (rf.z + 1.0) * (rf.z + 1.0));\n"
    "    m = m > 1e-8 ? m : 1.0;\n"
    "    for (int i = 0; i < 4; i++) {\n"
    "        for (int c = 0; c < 4; c++) {\n"
    "            int g = u_tgen[i][c];\n"
    "            if (g == 0) continue;\n"
    "            vec4 pl = u_tplane[i * 4 + c];\n"
    "            float v;\n"
    "            if (g == 1) v = dot(pl, e);\n"
    "            else if (g == 2) v = dot(pl, p);\n"
    "            else if (g == 3) v = c == 0 ? rf.x / m + 0.5 : c == 1 ? rf.y / m + 0.5 : t[i][c];\n"
    "            else if (g == 4) v = c < 3 ? n[c] : 1.0;\n"
    "            else v = c < 3 ? rf[c] : 1.0;\n"
    "            t[i][c] = v;\n"
    "        }\n"
    "        if (u_tmen[i] != 0) {\n"
    "            vec4 s = t[i];\n"
    "            t[i] = vec4(dot(u_tmat[i * 4], s), dot(u_tmat[i * 4 + 1], s),\n"
    "                        dot(u_tmat[i * 4 + 2], s), dot(u_tmat[i * 4 + 3], s));\n"
    "        }\n"
    "    }\n"
    "}\n";

int nv2a_shader_vk;

const char *nv2a_gl_vsh_prelude(void)
{
    if (nv2a_shader_vk)
        return s_vk_prelude;
    return
        "#version 330 core\n"
        "layout(location = 0) in vec4 v0;\n"
        "layout(location = 1) in vec4 v1;\n"
        "layout(location = 2) in vec4 v2;\n"
        "layout(location = 3) in vec4 v3;\n"
        "layout(location = 4) in vec4 v4;\n"
        "layout(location = 5) in vec4 v5;\n"
        "layout(location = 6) in vec4 v6;\n"
        "layout(location = 7) in vec4 v7;\n"
        "layout(location = 8) in vec4 v8;\n"
        "layout(location = 9) in vec4 v9;\n"
        "layout(location = 10) in vec4 v10;\n"
        "layout(location = 11) in vec4 v11;\n"
        "layout(location = 12) in vec4 v12;\n"
        "layout(location = 13) in vec4 v13;\n"
        "layout(location = 14) in vec4 v14;\n"
        "layout(location = 15) in vec4 v15;\n"
        "uniform vec4 c[192];\n"
        "uniform vec4 u_surf;      /* 2/W, 2/H, 1/zmax, scale offset */\n"
        "uniform vec2 u_aa;        /* logical -> real pixels */\n"
        "uniform vec4 u_m[4];      /* fixed-function composite rows */\n"
        "uniform vec4 u_vpoff;     /* fixed-function viewport offset */\n"
        "out vec4 vD0; out vec4 vD1; out vec4 vT0; out vec4 vT1;\n"
        "out vec4 vT2; out vec4 vT3; out float vFog;\n"
        "vec4 R0, R1, R2, R3, R4, R5, R6, R7, R8, R9, R10, R11;\n"
        "vec4 oPos, o1, o2, oD0, oD1, oFog, oPts, oB0, oB1, oT0, oT1, oT2, oT3;\n"
        "float nv2a_rcp(float x) { return x != 0.0 ? 1.0 / x : 1.884467e+19; }\n"
        "float nv2a_rcc(float x) {\n"
        "    float y = nv2a_rcp(x); float a = clamp(abs(y), 5.42101e-20, 1.884467e+19);\n"
        "    return y < 0.0 ? -a : a;\n"
        "}\n"
        "vec4 nv2a_exp(float x) { float f = floor(x); return vec4(exp2(f), x - f, exp2(x), 1.0); }\n"
        "vec4 nv2a_log(float x) {\n"
        "    float a = abs(x);\n"
        "    if (a == 0.0) return vec4(-1.884467e+19, 1.0, -1.884467e+19, 1.0);\n"
        "    float e = floor(log2(a));\n"
        "    return vec4(e, a / exp2(e), log2(a), 1.0);\n"
        "}\n"
        "vec4 nv2a_lit(vec4 s) {\n"
        "    float lx = max(s.x, 0.0), ly = max(s.y, 0.0), w = clamp(s.w, -127.996, 127.996);\n"
        "    return vec4(1.0, lx, (lx > 0.0 && ly > 0.0) ? exp2(w * log2(ly)) : 0.0, 1.0);\n"
        "}\n"
        "/* screen*w (real pixels) and clip w to GL clip space. */\n"
        /* z clamped to the clip volume: the NV2A clamps depth rather than
         * clipping at the near and far planes, and GL_DEPTH_CLAMP alone did
         * not reach every driver -- on Eden NFSU2's loading screen, drawn at
         * z = 1.0, still vanished. */
        NV2A_SNAP_GLSL
        "vec4 nv2a_clip(vec3 sw, float w) {\n"
        "    float z = sw.z * u_surf.z * 2.0 - w;\n"
        "    sw.xy = nv2a_snap(sw.xy, w);\n"
        "    return vec4(sw.x * u_surf.x - w, sw.y * u_surf.y - w,\n"
        "                clamp(z, -abs(w), abs(w)), w);\n"
        "}\n"
        /* The GL back end keeps passing the distance through (no fog unit). */
        "float nv2a_fog(float d) { return d; }\n"
        "float nv2a_fog_ffp(float d) { return 0.0; }\n";
}

#define NV2A_VSH_MAIN(set_ib) \
    "void main() {\n" \
    "    R0 = R1 = R2 = R3 = R4 = R5 = R6 = R7 = R8 = R9 = R10 = R11 = vec4(0.0);\n" \
    "    oPos = o1 = o2 = oFog = oPts = vec4(0.0);\n" \
    "    oD0 = oD1 = oB0 = oB1 = vec4(0.0, 0.0, 0.0, 1.0);\n" \
    "    oT0 = oT1 = oT2 = oT3 = vec4(0.0, 0.0, 0.0, 1.0);\n" \
    set_ib \
    "    nv2a_program();\n" \
    "    float w = abs(oPos.w) < 1e-6 ? 1e-6 : oPos.w;\n" \
    "    vec3 s = vec3(oPos.xy * u_aa, oPos.z);\n" \
    "    gl_Position = nv2a_clip(s * w, w);\n" \
    "    vD0 = clamp(oD0, 0.0, 1.0); vD1 = clamp(oD1, 0.0, 1.0);\n" \
    "    vT0 = oT0; vT1 = oT1; vT2 = oT2; vT3 = oT3; vFog = nv2a_fog(oFog.x);\n" \
    "}\n"

const char *nv2a_gl_vsh_main_program(void)
{
    if (nv2a_shader_vk)
        return NV2A_VSH_MAIN("    nv2a_ib = gl_InstanceIndex * 192;\n");
    return NV2A_VSH_MAIN("");
}

const char *nv2a_gl_vsh_fixed(void)
{
    /* Vulkan: u_xform is a member of the prelude's uniform block. */
    static const char decl[] = "uniform int u_xform;      /* 0 pre-transformed, 1 fixed-function */\n";
    static const char body[] =
        "uniform int u_xform;      /* 0 pre-transformed, 1 fixed-function */\n"
        "void main() {\n"
        "    float fogd = v4.a;\n"
        "    if (u_xform == 1) {\n"
        "        vec4 clip = vec4(dot(u_m[0], v0), dot(u_m[1], v0),\n"
        "                         dot(u_m[2], v0), dot(u_m[3], v0));\n"
        "        float w = abs(clip.w) < 1e-6 ? 1e-6 : clip.w;\n"
        "        vec3 sw = clip.xyz + u_vpoff.xyz * w;\n"
        "        sw.xy *= u_aa;\n"
        "        fogd = w;\n"
        "        gl_Position = nv2a_clip(sw, w);\n"
        "    } else {\n"
        "        float w = (v0.w > 0.0) ? 1.0 / v0.w : 1.0;\n"
        "        gl_Position = nv2a_clip(v0.xyz * w, w);\n"
        "    }\n"
        "    vD0 = clamp(v3, 0.0, 1.0); vD1 = clamp(v4, 0.0, 1.0);\n"
        "    vT0 = v9; vT1 = v10; vT2 = v11; vT3 = v12; vFog = nv2a_fog_ffp(fogd);\n"
        "}\n";
    /* Vulkan: the same, plus fixed-function lighting (nv2a_light). */
    static const char vk_body[] =
        "void main() {\n"
        "    float fogd = v4.a;\n"
        "    vec4 d0 = v3, d1 = v4;\n"
        "    vec4 t[4] = vec4[4](v9, v10, v11, v12);\n"
        "    if (u_xform == 1) {\n"
        "        vec4 clip = vec4(dot(u_m[0], v0), dot(u_m[1], v0),\n"
        "                         dot(u_m[2], v0), dot(u_m[3], v0));\n"
        "        float w = abs(clip.w) < 1e-6 ? 1e-6 : clip.w;\n"
        "        vec3 sw = clip.xyz + u_vpoff.xyz * w;\n"
        "        sw.xy *= u_aa;\n"
        "        fogd = w;\n"
        "        gl_Position = nv2a_clip(sw, w);\n"
        "        if (u_lctl.x != 0) nv2a_light(d0, d1);\n"
        "        if (u_tctl.x != 0) nv2a_texgen(t);\n"
        "    } else {\n"
        "        float w = (v0.w > 0.0) ? 1.0 / v0.w : 1.0;\n"
        "        gl_Position = nv2a_clip(v0.xyz * w, w);\n"
        "    }\n"
        "    vD0 = clamp(d0, 0.0, 1.0); vD1 = clamp(d1, 0.0, 1.0);\n"
        "    vT0 = t[0]; vT1 = t[1]; vT2 = t[2]; vT3 = t[3]; vFog = nv2a_fog_ffp(fogd);\n"
        "}\n";
    (void)decl;
    return nv2a_shader_vk ? vk_body : body;
}
