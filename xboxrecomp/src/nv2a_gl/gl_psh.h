/* gl_psh.h -- NV2A register combiners as GLSL (see gl_psh.c). */
#ifndef NV2A_GL_PSH_H
#define NV2A_GL_PSH_H

#include <stddef.h>
#include <stdint.h>

/* Everything that changes the generated code, and nothing that does not
 * (factors, fog colour and the alpha reference are uniforms). Compared with
 * memcmp, so fill every field. */
typedef struct {
    uint32_t color_icw[8], color_ocw[8];
    uint32_t alpha_icw[8], alpha_ocw[8];
    uint32_t control;          /* NV097_SET_COMBINER_CONTROL */
    uint32_t final0, final1;   /* SPECULAR_FOG_CW0 / CW1 */
    uint32_t shader_program;   /* NV097_SET_SHADER_STAGE_PROGRAM */
    uint32_t other_input;      /* NV097_SET_SHADER_OTHER_STAGE_INPUT */
    /* What each stage samples (Vulkan only; 0 elsewhere), 4 bits per stage:
     * NV2A_TEX_2D, _3D (a volume, sampler3D), _SHADOW24 / _SHADOW16 (a depth
     * texture under PROJECT3D: compared against r/q, as the NV2A does for
     * shadow buffers). Bits 16-18: SET_SHADOW_DEPTH_FUNC. */
    uint32_t tex_kind;
} Nv2aPshKey;

#define NV2A_TEX_2D       0u
#define NV2A_TEX_3D       1u
#define NV2A_TEX_SHADOW24 2u
#define NV2A_TEX_SHADOW16 3u
#define NV2A_TEX_KIND(k, i) (((k)->tex_kind >> (4 * (i))) & 0xFu)

/* Nonzero: emit Vulkan GLSL (nv2a_vk) instead of GLSL 3.30 (gl_vsh.c). */
extern int nv2a_shader_vk;

/* Fragment shader source for `k`. Returns bytes written, -1 on overflow. */
int nv2a_gl_psh(const Nv2aPshKey *k, char *buf, size_t cap);

#endif
