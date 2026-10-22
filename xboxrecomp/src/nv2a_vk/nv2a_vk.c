/*
 * nv2a_vk.c -- a Vulkan 1.3 renderer for the pushbuffer executor.
 *
 * The same job as nv2a_gl.c, on Vulkan: registered as the executor's back end
 * (nv2a_backend.h, draw_raw), it turns the method shadow into draws. Built
 * instead of nv2a_gl for the Switch's NVK (mesa-switch, linked statically)
 * and, on Linux, any Vulkan driver (lavapipe runs it headless).
 *
 *   surfaces   one image per colour-surface address (B8G8R8A8, rows top-first
 *              like guest memory), depth per zeta address (as nv2a_gl). All
 *              images live in VK_IMAGE_LAYOUT_GENERAL: a surface is drawn
 *              into, sampled and blitted without layout changes, ordered by
 *              memory barriers between rendering passes.
 *   textures   the GL renderer's cache and decoders; uploaded through the
 *              frame's staging ring. DXT stays compressed (BC1/2/3).
 *   shaders    gl_vsh.c / gl_psh.c in their Vulkan dialect, compiled to
 *              SPIR-V at run time by glslang, cached by what generated them.
 *   pipelines  keyed by the shader pair, blend state, colour mask, whether
 *              there is a depth buffer and the topology class; everything
 *              else (depth, stencil, cull, winding, topology, vertex layout,
 *              viewport) is dynamic state.
 *   uniforms   two std140 blocks per draw in the frame ring, textures as
 *              combined image samplers, all through push descriptors.
 *
 * Two frames in flight. Everything runs on the executor's thread.
 *
 * Runtime switches (the GL renderer's names where they mean the same):
 *   RECOMP_GL_DUMP=<prefix>[,every]  write presented frames as BMP
 *   RECOMP_GL_TRACE=1                shader sources on compile errors
 *   RECOMP_GL_SCALE=<k>              render surfaces at k times the title's
 *                                    resolution, fractions allowed (1.5;
 *                                    0.5..4, default 1)
 *   RECOMP_VK_VALIDATION=1           (Linux) enable the Khronos validation layer
 *   RECOMP_VK_HEADLESS=1             (Linux) no window, no present
 */
#include "nv2a_vk.h"
#include "../nv2a_gl/gl_psh.h"
#include "../nv2a_gl/gl_vsh.h"
#include "../kernel/nv2a_backend.h"

#if defined(__SWITCH__)
#include <switch.h>
#define VK_USE_PLATFORM_VI_NN 1
#endif
#include <vulkan/vulkan.h>
#if !defined(__SWITCH__)
#include <SDL.h>
#include <SDL_vulkan.h>
#endif
#include <glslang/Include/glslang_c_interface.h>
#include <glslang/Public/resource_limits_c.h>

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

extern ptrdiff_t xbox_GetMemoryOffset(void);

#define LOGE(...) fprintf(stderr, "  [VK] " __VA_ARGS__)

static int s_state;            /* 0 untried, 1 ready, -1 failed */
static int s_trace;
/* RECOMP_VK_TRACE=<n>: name every step of the first n draws, clears and
 * flips (a crash inside the driver then ends the log at the step). */
static int s_vtrace;
#define VT(...) do { if (s_vtrace > 0) { fprintf(stderr, "  [VKT] " __VA_ARGS__); fflush(stderr); } } while (0)
static uint32_t s_frame;
/* RECOMP_TEX_CHANGES=1: log every cached texture whose guest bytes changed
 * (re-uploaded). Finds textures a title frees while still drawing them. */
static int s_tex_changes = -1;
static int s_drew_any;
static uint32_t s_frame_draws;
static const uint32_t *s_regs;
static const Nv2aRawBatch *s_batch;

/* Hitch accounting, as in nv2a_gl. */
static uint32_t s_hz_progs, s_hz_texs, s_hz_draws;
static uint64_t s_hz_prog_ns, s_hz_tex_ns, s_hz_tex_bytes;
static uint64_t hz_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* ── Device ───────────────────────────────────────────────────────── */

static VkInstance       s_inst;
static VkPhysicalDevice s_pd;
static VkDevice         s_dev;
static VkQueue          s_queue;
static uint32_t         s_qfam;
static VkPhysicalDeviceMemoryProperties s_memprops;
static VkDeviceSize     s_ubo_align = 256;
static uint32_t         s_ubo_range = 16384;
static VkFormat         s_depth_fmt = VK_FORMAT_D24_UNORM_S8_UINT;
static int              s_have_bc, s_have_depth_clamp, s_have_clip_dist;

static PFN_vkCmdPushDescriptorSetKHR  p_push_desc;
static PFN_vkCmdSetVertexInputEXT     p_vertex_input;
static PFN_vkCmdBeginRendering        p_begin_rendering;
static PFN_vkCmdEndRendering          p_end_rendering;
static PFN_vkCmdSetCullMode           p_cull_mode;
static PFN_vkCmdSetFrontFace          p_front_face;
static PFN_vkCmdSetPrimitiveTopology  p_topology;
static PFN_vkCmdSetDepthTestEnable    p_depth_test;
static PFN_vkCmdSetDepthWriteEnable   p_depth_write;
static PFN_vkCmdSetDepthCompareOp     p_depth_op;
static PFN_vkCmdSetStencilTestEnable  p_stencil_test;
static PFN_vkCmdSetStencilOp          p_stencil_op;

/* Presentation. */
static VkSurfaceKHR   s_surface;
static VkSwapchainKHR s_swapchain;
static VkFormat       s_sc_format;
static VkExtent2D     s_sc_extent;
static uint32_t       s_sc_count;
static VkImage        s_sc_images[8];
static int            s_headless;
#if !defined(__SWITCH__)
static SDL_Window    *s_win;
#endif

static uint32_t mem_type(uint32_t bits, VkMemoryPropertyFlags want)
{
    uint32_t i;
    for (i = 0; i < s_memprops.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (s_memprops.memoryTypes[i].propertyFlags & want) == want)
            return i;
    return UINT32_MAX;
}

static VkDeviceMemory alloc_mem(VkMemoryRequirements rq, VkMemoryPropertyFlags want)
{
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    VkDeviceMemory m = VK_NULL_HANDLE;
    ai.allocationSize = rq.size;
    ai.memoryTypeIndex = mem_type(rq.memoryTypeBits, want);
    if (ai.memoryTypeIndex == UINT32_MAX && (want & VK_MEMORY_PROPERTY_HOST_CACHED_BIT))
        ai.memoryTypeIndex = mem_type(rq.memoryTypeBits, want & ~VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
    if (ai.memoryTypeIndex == UINT32_MAX)
        return VK_NULL_HANDLE;
    if (vkAllocateMemory(s_dev, &ai, NULL, &m) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    return m;
}

/* ── Frames in flight ─────────────────────────────────────────────── */

#define VK_FRAMES 2
#define RING_BYTES (48u << 20)

typedef struct {
    VkImage image; VkImageView view; VkDeviceMemory mem; VkPipeline pipe;
} Garbage;

typedef struct {
    VkCommandPool   pool;
    VkCommandBuffer cb;
    VkFence         fence;
    VkSemaphore     acquired, rendered;
    VkBuffer        ring;
    VkDeviceMemory  ring_mem;
    uint8_t        *ring_ptr;
    VkDeviceSize    ring_off;
    int             submitted;
    Garbage        *garbage;
    uint32_t        ngarbage, garbage_cap;
} Frame;

static Frame  s_fr[VK_FRAMES];
static Frame *s_f;             /* the frame being recorded */
static uint32_t s_fi;
static VkCommandBuffer s_cb;   /* s_f->cb while recording */
static uint32_t s_cb_gen;      /* bumped at every vkBeginCommandBuffer */
static int s_dyn_valid;

static void garbage_add_pipe(VkPipeline pipe);
static void garbage_add(VkImage img, VkImageView view, VkDeviceMemory mem)
{
    Frame *f = s_f;
    if (f->ngarbage == f->garbage_cap) {
        f->garbage_cap = f->garbage_cap ? f->garbage_cap * 2 : 64;
        f->garbage = (Garbage *)realloc(f->garbage, f->garbage_cap * sizeof *f->garbage);
    }
    f->garbage[f->ngarbage].image = img;
    f->garbage[f->ngarbage].view = view;
    f->garbage[f->ngarbage].mem = mem;
    f->garbage[f->ngarbage].pipe = VK_NULL_HANDLE;
    f->ngarbage++;
}

static void garbage_add_pipe(VkPipeline pipe)
{
    garbage_add(VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE);
    s_f->garbage[s_f->ngarbage - 1].pipe = pipe;
}

static void garbage_free(Frame *f)
{
    uint32_t i;
    for (i = 0; i < f->ngarbage; i++) {
        if (f->garbage[i].view) vkDestroyImageView(s_dev, f->garbage[i].view, NULL);
        if (f->garbage[i].image) vkDestroyImage(s_dev, f->garbage[i].image, NULL);
        if (f->garbage[i].mem) vkFreeMemory(s_dev, f->garbage[i].mem, NULL);
        if (f->garbage[i].pipe) vkDestroyPipeline(s_dev, f->garbage[i].pipe, NULL);
    }
    f->ngarbage = 0;
}

static void frame_begin(void)
{
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    s_f = &s_fr[s_fi];
    if (s_f->submitted) {
        vkWaitForFences(s_dev, 1, &s_f->fence, VK_TRUE, UINT64_MAX);
        vkResetFences(s_dev, 1, &s_f->fence);
        s_f->submitted = 0;
    }
    garbage_free(s_f);
    s_f->ring_off = 0;
    vkResetCommandPool(s_dev, s_f->pool, 0);
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(s_f->cb, &bi);
    s_cb = s_f->cb;
    s_cb_gen++;
    s_dyn_valid = 0;
}

/* Space in this frame's ring: returns the offset, *ptr the mapping. */
static void flush_frame_wait(void);
static VkDeviceSize ring_alloc(VkDeviceSize size, VkDeviceSize align, void **ptr)
{
    VkDeviceSize at = (s_f->ring_off + align - 1) & ~(align - 1);
    if (at + size > RING_BYTES) {
        /* A frame larger than the ring: finish what is recorded, wait for
         * it, and start over in the same ring. Slow, but only in extremes. */
        static int warned;
        if (!warned++)
            LOGE("frame ring (%u MB) full: flushing mid-frame\n", RING_BYTES >> 20);
        flush_frame_wait();
        at = 0;
        if (size > RING_BYTES) {
            *ptr = NULL;
            return 0;
        }
    }
    s_f->ring_off = at + size;
    *ptr = s_f->ring_ptr + at;
    return at;
}

/* ── Rendering passes and barriers ────────────────────────────────── */

/* w, h are the surface in the title's pixels (anti-aliasing included) --
 * what the shaders, clip rectangles and texture lookups work in. pw, ph are
 * the image: w, h times the render scale. */
typedef struct {
    uint32_t va, w, h, pw, ph;
    VkImage image; VkImageView view; VkDeviceMemory mem;
    uint32_t used;
    uint32_t aa_sx, aa_sy;
    int dirty;                 /* written since the last barrier */
    uint32_t gen;              /* bumped by every draw and clear into it */
} VkSurf;

typedef struct {
    uint32_t va, pw, ph;
    VkImage image; VkImageView view; VkDeviceMemory mem;
    VkImageView sview;         /* depth aspect only: sampled as a shadow buffer */
    uint32_t used;
    int dirty;                 /* drawn or cleared since the last barrier */
    uint32_t clr_frame;        /* s_frame + 1 of the last depth clear */
    uint32_t zonly_frame;      /* s_frame + 1 of the last depth-only draw */
    uint32_t tw, th;           /* title size of the surface last drawn with it */
    uint64_t wseq;             /* s_depth_seq at the last draw or clear into it */
} VkDepthBuf;
static uint64_t s_depth_seq;
/* Mod menu switches (menu_init sets them from mods.txt; -1 = not read yet). */
static int s_mod_fps = -1, s_mod_fog = -1, s_mod_shadows = -1;

/* Depth-only (shadow buffer) passes per 10 s, for [VKSTAT]. */
static uint32_t s_zonly_passes, s_zonly_noclear;
static uint32_t s_clr_z, s_clr_s;          /* depth / stencil clears */
/* Draws by transform mode (0 pre-transformed, 1 fixed function, 2 program)
 * and fixed-function draws with lighting on, which nv2a_vk does not light. */
static uint32_t s_xf_n[3], s_ffp_lit;

#define VK_MAX_SURF 32
#define VK_MAX_DEPTH 16

/* RECOMP_GL_SCALE, as in nv2a_gl: only the pixels behind a surface grow, so
 * the viewport, clear rectangles, read-backs and the present blit scale and
 * nothing else does. Capped per surface by the device's image limits. */
static double   s_scale = 1.0;
static uint32_t s_max_size = 4096;

/* v title pixels of a surface l wide, in the p stored pixels behind it. */
static uint32_t to_stored(uint32_t v, uint32_t p, uint32_t l)
{
    return (uint32_t)(((uint64_t)v * p + l / 2) / l);
}
static VkSurf     s_surf[VK_MAX_SURF];
static VkDepthBuf s_depth[VK_MAX_DEPTH];
static VkSurf    *s_last;
static VkSurf    *s_display_surf;

static int         s_in_render;
static VkSurf     *s_rt;
static VkDepthBuf *s_rt_depth;

static void barrier_all(void)
{
    VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
    int i;
    mb.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier(s_cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    for (i = 0; i < VK_MAX_SURF; i++)
        s_surf[i].dirty = 0;
}

static void end_rendering(void)
{
    if (s_in_render) {
        p_end_rendering(s_cb);
        s_in_render = 0;
    }
}

static VkRect2D s_sc_cur;               /* scissor set in this pass */

static void set_viewport(uint32_t pw, uint32_t ph)
{
    VkViewport vp = { 0, 0, (float)pw, (float)ph, 0.0f, 1.0f };
    VkRect2D sc = { { 0, 0 }, { pw, ph } };
    vkCmdSetViewport(s_cb, 0, 1, &vp);
    vkCmdSetScissor(s_cb, 0, 1, &sc);
    s_sc_cur = sc;
}

/* SET_SURFACE_CLIP as the scissor, in s's stored pixels. NFSU2's split
 * screen draws each player's world with the clip set to that player's
 * half; the vertex programs place it there but do not stop at the edge.
 * Same as nv2a_gl.c (and xemu). Call inside the pass (begin_rendering
 * resets it to the whole surface). */
static void set_surface_scissor(const uint32_t *r, const VkSurf *s)
{
    uint32_t x0 = (r[0x200 / 4] & 0xFFFF) * s->aa_sx, x1 = x0 + (r[0x200 / 4] >> 16) * s->aa_sx;
    uint32_t y0 = (r[0x204 / 4] & 0xFFFF) * s->aa_sy, y1 = y0 + (r[0x204 / 4] >> 16) * s->aa_sy;
    VkRect2D sc = { { 0, 0 }, { s->pw, s->ph } };

    if (x1 > s->w) x1 = s->w;
    if (y1 > s->h) y1 = s->h;
    if (x1 > x0 && y1 > y0) {
        sc.offset.x = (int32_t)to_stored(x0, s->pw, s->w);
        sc.offset.y = (int32_t)to_stored(y0, s->ph, s->h);
        sc.extent.width = to_stored(x1, s->pw, s->w) - (uint32_t)sc.offset.x;
        sc.extent.height = to_stored(y1, s->ph, s->h) - (uint32_t)sc.offset.y;
    }
    if (memcmp(&sc, &s_sc_cur, sizeof sc)) {
        vkCmdSetScissor(s_cb, 0, 1, &sc);
        s_sc_cur = sc;
    }
}

static void begin_rendering(VkSurf *s, VkDepthBuf *d)
{
    VkRenderingAttachmentInfo ca = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    VkRenderingAttachmentInfo da = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    VkRenderingInfo ri = { VK_STRUCTURE_TYPE_RENDERING_INFO };

    if (s_in_render && s_rt == s && s_rt_depth == d)
        return;
    end_rendering();
    barrier_all();
    ca.imageView = s->view;
    ca.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    ca.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    ri.renderArea.extent.width = s->pw;
    ri.renderArea.extent.height = s->ph;
    if (d) {
        if (d->pw < ri.renderArea.extent.width) ri.renderArea.extent.width = d->pw;
        if (d->ph < ri.renderArea.extent.height) ri.renderArea.extent.height = d->ph;
    }
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &ca;
    if (d) {
        da.imageView = d->view;
        da.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        da.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        da.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        ri.pDepthAttachment = &da;
        ri.pStencilAttachment = &da;
    }
    p_begin_rendering(s_cb, &ri);
    s_in_render = 1;
    s_rt = s;
    s_rt_depth = d;
    set_viewport(s->pw, s->ph);
}

/* An image in GENERAL layout, from UNDEFINED; recorded outside rendering. */
static void image_to_general(VkImage img, VkImageAspectFlags aspect)
{
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    end_rendering();
    b.srcAccessMask = 0;
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange.aspectMask = aspect;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.layerCount = VK_REMAINING_ARRAY_LAYERS;
    vkCmdPipelineBarrier(s_cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
}

/* layers: 1 = a 2D image, 6 = a cube map (cube view). */
static int make_image_n(uint32_t w, uint32_t h, uint32_t layers, VkFormat fmt,
                        VkImageUsageFlags usage, VkImageAspectFlags aspect,
                        VkImage *img, VkImageView *view, VkDeviceMemory *mem)
{
    VkImageCreateInfo ci = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    VkMemoryRequirements rq;

    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = fmt;
    ci.extent.width = w;
    ci.extent.height = h;
    ci.extent.depth = 1;
    ci.mipLevels = 1;
    ci.arrayLayers = layers;
    if (layers == 6)
        ci.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(s_dev, &ci, NULL, img) != VK_SUCCESS)
        return 0;
    vkGetImageMemoryRequirements(s_dev, *img, &rq);
    *mem = alloc_mem(rq, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (!*mem || vkBindImageMemory(s_dev, *img, *mem, 0) != VK_SUCCESS) {
        vkDestroyImage(s_dev, *img, NULL);
        if (*mem) vkFreeMemory(s_dev, *mem, NULL);
        *img = VK_NULL_HANDLE; *mem = VK_NULL_HANDLE;
        return 0;
    }
    vi.image = *img;
    vi.viewType = layers == 6 ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_2D;
    vi.format = fmt;
    vi.subresourceRange.aspectMask = aspect & ~VK_IMAGE_ASPECT_STENCIL_BIT;
    vi.subresourceRange.levelCount = 1;
    vi.subresourceRange.layerCount = layers;
    if (aspect & VK_IMAGE_ASPECT_DEPTH_BIT)
        vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    if (vkCreateImageView(s_dev, &vi, NULL, view) != VK_SUCCESS) {
        vkDestroyImage(s_dev, *img, NULL);
        vkFreeMemory(s_dev, *mem, NULL);
        *img = VK_NULL_HANDLE; *mem = VK_NULL_HANDLE;
        return 0;
    }
    image_to_general(*img, aspect);
    return 1;
}

static int make_image(uint32_t w, uint32_t h, VkFormat fmt, VkImageUsageFlags usage,
                      VkImageAspectFlags aspect, VkImage *img, VkImageView *view,
                      VkDeviceMemory *mem)
{
    return make_image_n(w, h, 1, fmt, usage, aspect, img, view, mem);
}

/* ── Surfaces ─────────────────────────────────────────────────────── */

static inline uint32_t nv2a_clean_va(uint32_t va)
{
    if (va >= 0x80000000u && va < 0x80000000u + 0x04000000u)
        return va;
    if (va >= 0x04000000u) {
        uint32_t phys = va & 0x03FFFFFFu;
        extern int xbox_ContiguousIsPhysical(uint32_t phys);
        if (xbox_ContiguousIsPhysical(phys))
            return 0x80000000u + phys;
        return phys;
    }
    return va;
}

/* One address can have surfaces of several widths (surf_get); a lookup by
 * address alone takes the one drawn into last. */
static VkSurf *surf_find(uint32_t va)
{
    VkSurf *best = NULL;
    int i;
    va = nv2a_clean_va(va);
    for (i = 0; i < VK_MAX_SURF; i++)
        if (s_surf[i].image && s_surf[i].va == va
            && (!best || s_surf[i].used > best->used
                || (s_surf[i].used == best->used && s_surf[i].gen > best->gen)))
            best = &s_surf[i];
    return best;
}

static void clear_image_color(VkImage img, float r, float g, float b, float a)
{
    VkClearColorValue c = { { r, g, b, a } };
    VkImageSubresourceRange rg = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0,
                                   VK_REMAINING_ARRAY_LAYERS };
    end_rendering();
    vkCmdClearColorImage(s_cb, img, VK_IMAGE_LAYOUT_GENERAL, &c, 1, &rg);
}

static VkSurf *surf_get(uint32_t va, uint32_t w, uint32_t h, uint32_t aa_sx, uint32_t aa_sy)
{
    va = nv2a_clean_va(va);
    VkSurf *s = NULL, *victim = NULL;
    int i;

    /* Keyed by address and width. Cold Fear draws its lamps' shadow buffers
     * at one address in 1024, 512 and 256 squared, several times a frame;
     * keyed by address alone, each change of size destroyed the surface and
     * made a new one -- an eighth of the pushbuffer thread went to creating
     * and freeing images. */
    for (i = 0; i < VK_MAX_SURF; i++)
        if (s_surf[i].image && s_surf[i].va == va && s_surf[i].w == w) {
            s = &s_surf[i];
            break;
        }

    /* h is the surface clip, not the allocation (NFSU2's split screen clips
     * the back buffer to each player's half): keep the surface while the
     * clip fits, grow it when it does not. Same as nv2a_gl.c. */
    if (s && s->w == w && s->h >= h) {
        s->used = s_frame;
        return s;
    }
    if (s && s->w == w && s->h > h)
        h = s->h;
    if (!s) {
        for (i = 0; i < VK_MAX_SURF; i++) {
            if (!s_surf[i].image) { victim = &s_surf[i]; break; }
            if (!victim || s_surf[i].used < victim->used) victim = &s_surf[i];
        }
        s = victim;
    }
    if (s->image) {
        if (s_rt == s) end_rendering(), s_rt = NULL;
        if (s_last == s) s_last = NULL;
        if (s_display_surf == s) s_display_surf = NULL;
        garbage_add(s->image, s->view, s->mem);
    }
    memset(s, 0, sizeof *s);
    s->va = va; s->w = w; s->h = h; s->used = s_frame;
    s->aa_sx = aa_sx; s->aa_sy = aa_sy;
    {
        double k = s_scale, m = (double)(w > h ? w : h);
        if (m * k > (double)s_max_size)
            k = (double)s_max_size / m;
        s->pw = (uint32_t)(w * k + 0.5); s->ph = (uint32_t)(h * k + 0.5);
        if (!s->pw) s->pw = 1;
        if (!s->ph) s->ph = 1;
    }
    if (!make_image(s->pw, s->ph, VK_FORMAT_B8G8R8A8_UNORM,
                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                    VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                    VK_IMAGE_ASPECT_COLOR_BIT, &s->image, &s->view, &s->mem)) {
        LOGE("surface 0x%08X %ux%u: image creation failed\n", va, w, h);
        memset(s, 0, sizeof *s);
        return NULL;
    }
    clear_image_color(s->image, 0, 0, 0, 1);
    if (s_trace)
        LOGE("surface 0x%08X %ux%u (aa %ux%u, stored %ux%u)\n", va, w, h, aa_sx, aa_sy,
             s->pw, s->ph);
    return s;
}

static VkDepthBuf *depth_get(uint32_t va, uint32_t pw, uint32_t ph)
{
    VkDepthBuf *d, *victim = NULL;
    int i;

    {
        /* Diagnostics: a zeta address behind more than one depth buffer
         * (different sizes) splits its clears from its draws. First 24. */
        static int logged;
        int j, found = 0, other = 0;
        for (j = 0; j < VK_MAX_DEPTH; j++)
            if (s_depth[j].image && s_depth[j].va == va) {
                if (s_depth[j].pw == pw && s_depth[j].ph == ph) found = 1;
                else other = 1;
            }
        if (!found && logged < 24) {
            logged++;
            fprintf(stderr, "[DEPTHNEW] zeta %08X %ux%u frame %u%s\n", va, pw, ph, s_frame,
                    other ? "  <-- SAME ADDRESS, OTHER SIZE" : "");
        }
    }
    for (i = 0; i < VK_MAX_DEPTH; i++) {
        if (s_depth[i].image && s_depth[i].va == va && s_depth[i].pw == pw && s_depth[i].ph == ph) {
            s_depth[i].used = s_frame;
            return &s_depth[i];
        }
        if (!victim || (victim->image && (!s_depth[i].image || s_depth[i].used < victim->used)))
            victim = &s_depth[i];
    }
    d = victim;
    if (d->image) {
        if (s_rt_depth == d) end_rendering(), s_rt = NULL, s_rt_depth = NULL;
        garbage_add(d->image, d->view, d->mem);
        if (d->sview)
            garbage_add(VK_NULL_HANDLE, d->sview, VK_NULL_HANDLE);
    }
    memset(d, 0, sizeof *d);
    d->va = va; d->pw = pw; d->ph = ph; d->used = s_frame;
    if (!make_image(pw, ph, s_depth_fmt, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                    VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                    VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT,
                    &d->image, &d->view, &d->mem)) {
        LOGE("depth 0x%08X %ux%u: image creation failed\n", va, pw, ph);
        memset(d, 0, sizeof *d);
        return NULL;
    }
    {
        /* A view of the depth alone, for a title that samples this buffer
         * as a shadow map (bind_stage). */
        VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vi.image = d->image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = s_depth_fmt;
        vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        vi.subresourceRange.levelCount = 1;
        vi.subresourceRange.layerCount = 1;
        if (vkCreateImageView(s_dev, &vi, NULL, &d->sview) != VK_SUCCESS)
            d->sview = VK_NULL_HANDLE;
    }
    {
        VkClearDepthStencilValue z = { 1.0f, 0 };
        VkImageSubresourceRange rg = { VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1 };
        vkCmdClearDepthStencilImage(s_cb, d->image, VK_IMAGE_LAYOUT_GENERAL, &z, 1, &rg);
    }
    if (s_trace)
        LOGE("depth 0x%08X %ux%u\n", va, pw, ph);
    return d;
}

/* The target of a draw or clear: surface (created on first use) and the
 * depth buffer of its zeta address. */
static VkSurf *target(const Nv2aSurface *sf, uint32_t zeta_va, VkDepthBuf **dout)
{
    uint32_t bpp = sf->bytes_per_pixel ? sf->bytes_per_pixel : 4;
    uint32_t w = sf->pitch ? sf->pitch / bpp : sf->width;
    uint32_t h = sf->clip_y + sf->height;   /* the clip need not start at 0 */
    VkSurf *s;

    uint32_t color_va = sf->color_va;

    *dout = NULL;
    if (!color_va && zeta_va) {
        /* Depth only (a shadow buffer pass: no colour surface). The draw
         * still needs a colour attachment here; a throwaway one keyed by
         * the zeta address takes the writes, and the depth buffer -- the
         * part a later pass samples -- is the real one. These draws were
         * dropped, so Cold Fear's shadow buffer never existed. */
        color_va = zeta_va ^ 0x1u;
        w = sf->clip_x + sf->width;
    }
    if (w < sf->clip_x + sf->width) w = sf->clip_x + sf->width;
    if (!w || !h || !color_va || w > 4096 || h > 4096)
        return NULL;
    s = surf_get(color_va, w, h, sf->aa_sx ? sf->aa_sx : 1, sf->aa_sy ? sf->aa_sy : 1);
    if (s && zeta_va)
        *dout = depth_get(zeta_va, s->pw, s->ph);
    if (*dout) {
        (*dout)->dirty = 1;                  /* a shadow-map read needs a barrier */
        (*dout)->wseq = ++s_depth_seq;
        (*dout)->tw = sf->clip_x + sf->width;
        (*dout)->th = sf->clip_y + sf->height;
    }
    if (s) {
        s_last = s;
        if (s->w >= 640 && s->h >= 400)
            s_display_surf = s;
    }
    return s;
}

/* ── Textures ─────────────────────────────────────────────────────── */

typedef struct {
    uint32_t va, color, w, h, pitch, hash, bytes;
    uint32_t face;              /* cube map: bytes per face; 0 = 2D */
    VkImage image; VkImageView view; VkDeviceMemory mem;
    VkFormat fmt;
    uint32_t used, checked;
} VkTex;

#define VK_MAX_TEX 1024
#define TEX_BUCKETS 2048
static VkTex    s_tex[VK_MAX_TEX];
static uint16_t s_tex_head[TEX_BUCKETS], s_tex_next[VK_MAX_TEX];
static uint64_t s_tex_bytes;
static uint32_t *s_decode;
static size_t    s_decode_cap;
static uint32_t s_palette_reg, s_palette_va;
static VkImage  s_dummy_img;
static VkImageView s_dummy_view;
static VkDeviceMemory s_dummy_mem;
/* A transparent black cube, for a cube map stage with nothing to sample. */
static VkImage  s_dummy_cube_img;
static VkImageView s_dummy_cube_view;
static VkDeviceMemory s_dummy_cube_mem;

uint64_t nv2a_gl_texture_bytes(void) { return s_tex_bytes; }
uint32_t nv2a_gl_frame_count(void) { return s_frame; }

static uint32_t tex_bucket(uint32_t va, uint32_t color, uint32_t w, uint32_t h)
{
    uint32_t k = va ^ (color << 24) ^ (w << 12) ^ (h << 4);
    k ^= k >> 15; k *= 0x2C1B3C6Du; k ^= k >> 12;
    return k & (TEX_BUCKETS - 1);
}
static void tex_index_add(VkTex *t)
{
    uint32_t b = tex_bucket(t->va, t->color, t->w, t->h);
    uint16_t i = (uint16_t)(t - s_tex);
    s_tex_next[i] = s_tex_head[b];
    s_tex_head[b] = (uint16_t)(i + 1);
}
static void tex_index_del(VkTex *t)
{
    uint16_t *at = &s_tex_head[tex_bucket(t->va, t->color, t->w, t->h)];
    uint16_t i = (uint16_t)(t - s_tex);
    while (*at) {
        if (*at == i + 1) { *at = s_tex_next[i]; return; }
        at = &s_tex_next[*at - 1];
    }
}

static int tex_size_from_format(uint32_t color)
{
    if (color >= 0x0C && color <= 0x0F)
        return 1;
    switch (color) {
    case 0x10: case 0x11: case 0x12: case 0x13: case 0x14: case 0x15:
    case 0x16: case 0x17: case 0x18: case 0x1B: case 0x1C: case 0x1D:
    case 0x1E: case 0x1F: case 0x20: case 0x24: case 0x25: case 0x26:
    case 0x2E: case 0x2F: case 0x30: case 0x31: case 0x34: case 0x35:
    case 0x36: case 0x37: case 0x40: case 0x41: case 0x43: case 0x44:
    case 0x45: case 0x46:
        return 0;
    default:
        return 1;
    }
}

static uint32_t bytes_hash(uint32_t va, uint32_t bytes)
{
    va = nv2a_clean_va(va);
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset() + va;
    uint32_t h = 2166136261u ^ bytes, step, i;
    if (!bytes)
        return h;
    step = bytes / 256;
    if (step < 4) step = 4;
    step &= ~3u;
    for (i = 0; i + 4 <= bytes; i += step) {
        uint32_t w;
        memcpy(&w, mem + i, 4);
        h = (h ^ w) * 16777619u;
    }
    memcpy(&i, mem + bytes - 4, 4);
    return (h ^ i) * 16777619u;
}

static uint32_t tex_bytes(uint32_t color, uint32_t w, uint32_t h, uint32_t pitch)
{
    if (color == 0x0C) return ((w + 3) / 4) * ((h + 3) / 4) * 8;
    if (color == 0x0E || color == 0x0F) return ((w + 3) / 4) * ((h + 3) / 4) * 16;
    if (pitch) return pitch * h;
    return w * h * 4;
}

static uint32_t swizzle_index(uint32_t u, uint32_t v, uint32_t w, uint32_t h)
{
    uint32_t out = 0, bit = 0, mw = w - 1, mh = h - 1, m;
    for (m = 1; mw >= m || mh >= m; m <<= 1) {
        if (mw >= m) { if (u & m) out |= 1u << bit; bit++; }
        if (mh >= m) { if (v & m) out |= 1u << bit; bit++; }
    }
    return out;
}

/* The swizzled index splits into an x part and a y part with disjoint bits,
 * so a texture's decode looks both up instead of interleaving bits per
 * texel: the per-texel loop above was an eighth of the pushbuffer thread
 * whenever textures streamed in. Up to 4096 x 4096. */
static uint32_t s_swz_x[4096], s_swz_y[4096];

static int swizzle_tables(uint32_t w, uint32_t h)
{
    uint32_t i;
    if (w > 4096 || h > 4096)
        return 0;
    for (i = 0; i < w; i++)
        s_swz_x[i] = swizzle_index(i, 0, w, h);
    for (i = 0; i < h; i++)
        s_swz_y[i] = swizzle_index(0, i, w, h);
    return 1;
}

static int decode_indexed(uint32_t va, uint32_t w, uint32_t h, uint32_t *out)
{
    va = nv2a_clean_va(va);
    s_palette_va = nv2a_clean_va(s_palette_va);
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    uint32_t entries = 256u >> ((s_palette_reg >> 2) & 3);
    const uint8_t *idx = mem + va;
    uint32_t x, y;
    uint32_t pal[256];
    if (!(s_palette_reg & ~0x3Fu) || !swizzle_tables(w, h))
        return 0;
    memcpy(pal, mem + s_palette_va, entries * 4);
    for (x = entries; x < 256; x++)
        pal[x] = pal[0];
    for (y = 0; y < h; y++) {
        const uint32_t sy = s_swz_y[y];
        uint32_t *row = out + (size_t)y * w;
        for (x = 0; x < w; x++)
            row[x] = pal[idx[s_swz_x[x] | sy]];
    }
    return 1;
}

static void tex_free(VkTex *t)
{
    garbage_add(t->image, t->view, t->mem);
    s_tex_bytes -= t->bytes;
    tex_index_del(t);
    memset(t, 0, sizeof *t);
}

static void tex_account(VkTex *t, uint32_t bytes)
{
    static uint64_t budget;
    s_hz_texs++;
    s_hz_tex_bytes += bytes;
    if (!budget) {
        const char *e = getenv("RECOMP_GL_TEX_MB");
        budget = (uint64_t)(e && atoi(e) > 0 ? atoi(e) : 256) << 20;
    }
    s_tex_bytes += (uint64_t)bytes - t->bytes;
    t->bytes = bytes;
    while (s_tex_bytes > budget) {
        VkTex *lru = NULL;
        uint32_t i;
        for (i = 0; i < VK_MAX_TEX; i++) {
            VkTex *c = &s_tex[i];
            if (c->image && c != t && c->used != s_frame && (!lru || c->used < lru->used))
                lru = c;
        }
        if (!lru)
            break;
        tex_free(lru);
    }
}

/* Copy `bytes` at `src` (row length `row_texels`, 0 = tight) into the image. */
static void tex_upload(VkTex *t, const void *src, uint32_t bytes, uint32_t row_texels,
                       uint32_t layer)
{
    void *dst;
    VkDeviceSize at = ring_alloc(bytes, 16, &dst);
    VkBufferImageCopy rg;

    if (!dst)
        return;
    memcpy(dst, src, bytes);
    end_rendering();
    barrier_all();
    memset(&rg, 0, sizeof rg);
    rg.bufferOffset = at;
    rg.bufferRowLength = row_texels;
    rg.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    rg.imageSubresource.baseArrayLayer = layer;
    rg.imageSubresource.layerCount = 1;
    rg.imageExtent.width = t->w;
    rg.imageExtent.height = t->h;
    rg.imageExtent.depth = 1;
    vkCmdCopyBufferToImage(s_cb, s_f->ring, t->image, VK_IMAGE_LAYOUT_GENERAL, 1, &rg);
}

/* Cube map faces follow each other, each with its mip chain, padded to 128
 * bytes (xemu's NV2A_CUBEMAP_FACE_ALIGNMENT); +X -X +Y -Y +Z -Z, the same
 * order and texel orientation as Vulkan's cube layers. */
static uint32_t cube_face_bytes(uint32_t color, uint32_t w, uint32_t h, uint32_t levels)
{
    uint32_t n = 0, l;

    if (!levels)
        levels = 1;
    for (l = 0; l < levels; l++) {
        n += tex_bytes(color, w, h, 0);
        w = w > 1 ? w / 2 : 1;
        h = h > 1 ? h / 2 : 1;
    }
    return (n + 127) & ~127u;
}

/* One 2D image (layer 0) or cube face from guest memory at va. */
static void tex_fill(VkTex *t, uint32_t va, uint32_t layer)
{
    va = nv2a_clean_va(va);
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    uint32_t color = t->color, w = t->w, h = t->h, pitch = t->pitch;
    Nv2aTexture nt;

    if (t->fmt != VK_FORMAT_B8G8R8A8_UNORM) {              /* DXT as it is */
        tex_upload(t, mem + va, tex_bytes(color, w, h, 0), 0, layer);
        return;
    }
    if ((color == 0x12 || color == 0x1E) && pitch && !(pitch & 3) && pitch / 4 >= w) {
        /* Linear A8R8G8B8 / X8R8G8B8 (movie frames, HUD): the bytes are B8G8R8A8 already. */
        tex_upload(t, mem + va, pitch * (h - 1) + w * 4, pitch / 4, layer);
        return;
    }
    if ((size_t)w * h > s_decode_cap) {
        free(s_decode);
        s_decode_cap = (size_t)w * h;
        s_decode = (uint32_t *)malloc(s_decode_cap * 4);
    }
    if (!s_decode)
        return;
    if (color == 0x06 || color == 0x07) {
        const uint32_t *src = (const uint32_t *)(mem + va);
        uint32_t x, y, fill = color == 0x07 ? 0xFF000000u : 0;
        if (!swizzle_tables(w, h))
            return;
        for (y = 0; y < h; y++) {
            const uint32_t sy = s_swz_y[y];
            uint32_t *row = s_decode + (size_t)y * w;
            for (x = 0; x < w; x++)
                row[x] = src[s_swz_x[x] | sy] | fill;
        }
    } else {
        int ok;
        memset(&nt, 0, sizeof nt);
        nt.offset = va; nt.width = w; nt.height = h; nt.pitch = pitch; nt.color = color;
        nt.addr_u = nt.addr_v = 1;
        ok = color == 0x0B ? decode_indexed(va, w, h, s_decode)
                           : nv2a_backend_decode_texture(&nt, s_decode);
        if (!ok) {
            uint32_t k;
            for (k = 0; k < w * h; k++)
                s_decode[k] = 0xFFFF00FFu;
            if (s_trace)
                LOGE("texture 0x%08X format 0x%02X not decodable\n", va, color);
        }
    }
    tex_upload(t, s_decode, w * h * 4, 0, layer);
}

/* face: bytes per cube face (cube_face_bytes), 0 for a 2D texture. */
static VkImageView tex_get(uint32_t va, uint32_t color, uint32_t w, uint32_t h, uint32_t pitch,
                           uint32_t face)
{
    va = nv2a_clean_va(va);
    VkTex *t = NULL, *victim = NULL;
    uint32_t i, hash;
    VkFormat fmt = VK_FORMAT_B8G8R8A8_UNORM;
    uint32_t layers = face ? 6 : 1, f, size;

    for (i = s_tex_head[tex_bucket(va, color, w, h)]; i; i = s_tex_next[i - 1]) {
        VkTex *c = &s_tex[i - 1];
        if (c->image && c->va == va && c->color == color && c->w == w && c->h == h
            && c->face == face) {
            t = c;
            break;
        }
    }
    if (t && t->checked == s_frame) {
        t->used = s_frame;
        return t->view;
    }
    hash = bytes_hash(va, face ? face * 6 : tex_bytes(color, w, h, pitch));
    if (color == 0x0B)
        hash ^= bytes_hash(s_palette_va, 1024) * 31u;
    if (t && t->hash == hash) {
        t->used = t->checked = s_frame;
        return t->view;
    }
    if (s_tex_changes < 0) {
        const char *e = getenv("RECOMP_TEX_CHANGES");
        s_tex_changes = e && atoi(e) > 0;
    }
    if (t && s_tex_changes)
        fprintf(stderr, "[tex] frame %u: %08X format %02X %ux%u changed\n",
                s_frame, va, color, w, h);
    if (s_have_bc && color == 0x0C) fmt = VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
    if (s_have_bc && color == 0x0E) fmt = VK_FORMAT_BC2_UNORM_BLOCK;
    if (s_have_bc && color == 0x0F) fmt = VK_FORMAT_BC3_UNORM_BLOCK;
    if (t && (t->fmt != fmt || t->pitch != pitch)) {
        tex_free(t);
        t = NULL;
    }
    if (!t) {
        for (i = 0; i < VK_MAX_TEX; i++) {
            VkTex *c = &s_tex[i];
            if (!c->image) { if (!victim || victim->image) victim = c; }
            else if (!victim || (victim->image && c->used < victim->used)) victim = c;
        }
        t = victim;
        if (t->image)
            tex_free(t);
        t->va = va; t->color = color; t->w = w; t->h = h; t->pitch = pitch; t->fmt = fmt;
        t->face = face;
        VT("texture %08X fmt %02X %ux%u pitch %u%s\n", va, color, w, h, pitch,
           face ? " cube" : "");
        if (!make_image_n(w, h, layers, fmt,
                          VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                          VK_IMAGE_ASPECT_COLOR_BIT, &t->image, &t->view, &t->mem)) {
            memset(t, 0, sizeof *t);
            return face ? s_dummy_cube_view : s_dummy_view;
        }
        tex_index_add(t);
        if (s_trace)
            LOGE("texture 0x%08X format 0x%02X %ux%u pitch %u%s\n", va, color, w, h, pitch,
                 face ? " cube" : "");
    }
    t->hash = hash;
    t->used = t->checked = s_frame;

    for (f = 0; f < layers; f++)
        tex_fill(t, va + f * face, f);
    size = fmt != VK_FORMAT_B8G8R8A8_UNORM ? tex_bytes(color, w, h, 0) : w * h * 4;
    tex_account(t, size * layers);
    return t->view;
}

/* A cube map whose faces the title renders (race car reflections: six
 * 128x128 targets, one per face, every frame). The faces exist only as
 * surface images, so the cube is assembled from them on the GPU, again
 * whenever a face was drawn into since the last copy. */
typedef struct {
    uint32_t va, w, h, face, size;
    VkImage image; VkImageView view; VkDeviceMemory mem;
    VkSurf  *src[6];
    uint32_t gen[6];
    uint32_t used;
} VkCube;
#define VK_MAX_CUBE 4
static VkCube s_cube[VK_MAX_CUBE];

static VkImageView cube_from_surfaces(uint32_t va, uint32_t w, uint32_t h, uint32_t face,
                                      VkSurf *faces[6])
{
    VkCube *c = NULL;
    uint32_t f, size = 0;
    int i, copied = 0;

    for (f = 0; f < 6; f++)
        if (faces[f] && to_stored(w, faces[f]->pw, faces[f]->w) > size)
            size = to_stored(w, faces[f]->pw, faces[f]->w);
    for (i = 0; i < VK_MAX_CUBE; i++)
        if (s_cube[i].image && s_cube[i].va == va && s_cube[i].w == w && s_cube[i].h == h
            && s_cube[i].face == face && s_cube[i].size == size) {
            c = &s_cube[i];
            break;
        }
    if (!c) {
        for (i = 0; i < VK_MAX_CUBE; i++)
            if (!c || !s_cube[i].image || (c->image && s_cube[i].used < c->used))
                c = &s_cube[i];
        if (c->image)
            garbage_add(c->image, c->view, c->mem);
        memset(c, 0, sizeof *c);
        if (!make_image_n(size, size, 6, VK_FORMAT_B8G8R8A8_UNORM,
                          VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                          VK_IMAGE_ASPECT_COLOR_BIT, &c->image, &c->view, &c->mem)) {
            memset(c, 0, sizeof *c);
            return s_dummy_cube_view;
        }
        clear_image_color(c->image, 0, 0, 0, 0);
        c->va = va; c->w = w; c->h = h; c->face = face; c->size = size;
        if (s_trace)
            LOGE("cube 0x%08X %ux%u from surfaces (stored %u)\n", va, w, h, size);
    }
    c->used = s_frame;
    for (f = 0; f < 6; f++) {
        VkSurf *sf = faces[f];
        VkImageBlit bl;
        if (!sf || (c->src[f] == sf && c->gen[f] == sf->gen))
            continue;
        if (!copied) {
            end_rendering();
            barrier_all();
            copied = 1;
        }
        memset(&bl, 0, sizeof bl);
        bl.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        bl.srcSubresource.layerCount = 1;
        bl.srcOffsets[1].x = (int32_t)to_stored(w, sf->pw, sf->w);
        bl.srcOffsets[1].y = (int32_t)to_stored(h, sf->ph, sf->h);
        bl.srcOffsets[1].z = 1;
        bl.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        bl.dstSubresource.baseArrayLayer = f;
        bl.dstSubresource.layerCount = 1;
        bl.dstOffsets[1].x = (int32_t)size;
        bl.dstOffsets[1].y = (int32_t)size;
        bl.dstOffsets[1].z = 1;
        vkCmdBlitImage(s_cb, sf->image, VK_IMAGE_LAYOUT_GENERAL, c->image,
                       VK_IMAGE_LAYOUT_GENERAL, 1, &bl, VK_FILTER_LINEAR);
        c->src[f] = sf;
        c->gen[f] = sf->gen;
    }
    if (copied)
        barrier_all();
    return c->view;
}

/* Samplers: wrap u, wrap v, mag, min. */
typedef struct { uint32_t key; VkSampler s; } SamplerEnt;
static SamplerEnt s_samplers[64];
static int s_nsamplers;

static VkSamplerAddressMode wrap_mode(uint32_t m)
{
    switch (m) {
    case 2:  return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case 3:  return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    case 4:  return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    case 5:  return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    default: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    }
}

static VkSampler sampler_get(uint32_t key)
{
    VkSamplerCreateInfo ci = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    int i;
    for (i = 0; i < s_nsamplers; i++)
        if (s_samplers[i].key == key)
            return s_samplers[i].s;
    ci.addressModeU = wrap_mode(key & 0xF);
    ci.addressModeV = wrap_mode((key >> 8) & 0xF);
    /* Bits 27:24: the P (third axis) wrap of a volume, 0 = repeat. */
    ci.addressModeW = wrap_mode((key >> 24) & 0xF);
    ci.magFilter = (key & 0x10000) ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    ci.minFilter = (key & 0x20000) ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    ci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    ci.maxLod = 0.0f;
    ci.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    if (s_nsamplers == 64)
        s_nsamplers = 0;                    /* never in practice: 4 x 4 x 4 keys */
    if (vkCreateSampler(s_dev, &ci, NULL, &s_samplers[s_nsamplers].s) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    s_samplers[s_nsamplers].key = key;
    return s_samplers[s_nsamplers++].s;
}

/* A surface kept taller than its clip (surf_get) and sampled as a texture
 * of the clip's size: texcoords cover the w x h texels at the top left, not
 * the whole surface. A texture of the surface's logical or real size keeps
 * the old full-surface mapping (anti-aliased surfaces). */
static void rt_scale(uint32_t w, uint32_t h, uint32_t sw, uint32_t sh,
                     uint32_t aa_sx, uint32_t aa_sy, float scale[2])
{
    uint32_t lw = sw / (aa_sx ? aa_sx : 1), lh = sh / (aa_sy ? aa_sy : 1);

    if (w < lw) scale[0] *= (float)w / (float)lw;
    if (h < lh) scale[1] *= (float)h / (float)lh;
}

/* ── Shadow buffers and volume textures (Cold Fear's lighting) ──────────
 *
 * A PROJECT3D stage reading a depth format is a shadow buffer: the NV2A
 * compares r/q with the stored depth (gl_psh.c). The texture is a depth
 * buffer this renderer drew, so it is bound from s_depth. A stage whose
 * format says three dimensions is a volume: a 3D image, filled from the
 * guest's 3D-swizzled texels. Before either, both were sampled as plain 2D
 * images -- Cold Fear's lights and flashlight came out black. */

/* The depth buffer of the draw being recorded (vk_draw_raw). */
static VkDepthBuf *s_draw_depth;

static int is_depth_color(uint32_t color)
{
    return color >= 0x2A && color <= 0x31;
}

/* NV2A_TEX_* for stage i, as the pixel shader key records it. */
static uint32_t stage_kind(const uint32_t *r, int i)
{
    uint32_t base = (0x1B00u + (uint32_t)i * 0x40u) / 4;
    uint32_t format = r[base + 1], color = (format >> 8) & 0xFF;
    uint32_t mode = (r[0x1E70 / 4] >> (5 * i)) & 0x1F;

    if (!(r[base + 3] & 0x40000000u) || !r[base] || mode == 3)
        return NV2A_TEX_2D;
    if (mode == 2 && is_depth_color(color))
        return (color == 0x2C || color == 0x2D || color == 0x30 || color == 0x31)
               ? NV2A_TEX_SHADOW16 : NV2A_TEX_SHADOW24;
    if (((format >> 4) & 0xF) == 3)
        return NV2A_TEX_3D;
    return NV2A_TEX_2D;
}

/* The depth buffer behind a shadow texture at va, w x h. Cold Fear draws
 * every lamp's shadow buffer at the same address, at 1024, 512 or 256
 * squared, so one address has several depth buffers here: the one of the
 * texture's size, else the last one written. Picking by frame of last use
 * read another lamp's (stale) buffer, and the scene darkened in blocks. */
static VkDepthBuf *depth_find(uint32_t va, uint32_t w, uint32_t h)
{
    VkDepthBuf *best = NULL, *sized = NULL;
    int i;
    for (i = 0; i < VK_MAX_DEPTH; i++) {
        VkDepthBuf *d = &s_depth[i];
        if (!d->image || nv2a_clean_va(d->va) != va || !d->sview)
            continue;
        if (!best || d->wseq > best->wseq)
            best = d;
        if (d->tw == w && d->th == h && (!sized || d->wseq > sized->wseq))
            sized = d;
    }
    return sized ? sized : best;
}

typedef struct {
    uint32_t va, color, w, h, d, hash, used;
    VkImage image; VkImageView view; VkDeviceMemory mem;
} VkTex3;
#define VK_MAX_TEX3 16
static VkTex3 s_tex3[VK_MAX_TEX3];
static VkImage s_dummy3_img;
static VkImageView s_dummy3_view;
static VkDeviceMemory s_dummy3_mem;

static int make_image_3d(uint32_t w, uint32_t h, uint32_t d,
                         VkImage *img, VkImageView *view, VkDeviceMemory *mem)
{
    VkImageCreateInfo ci = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    VkMemoryRequirements rq;

    ci.imageType = VK_IMAGE_TYPE_3D;
    ci.format = VK_FORMAT_B8G8R8A8_UNORM;
    ci.extent.width = w;
    ci.extent.height = h;
    ci.extent.depth = d;
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(s_dev, &ci, NULL, img) != VK_SUCCESS)
        return 0;
    vkGetImageMemoryRequirements(s_dev, *img, &rq);
    *mem = alloc_mem(rq, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (!*mem || vkBindImageMemory(s_dev, *img, *mem, 0) != VK_SUCCESS) {
        vkDestroyImage(s_dev, *img, NULL);
        if (*mem) vkFreeMemory(s_dev, *mem, NULL);
        *img = VK_NULL_HANDLE; *mem = VK_NULL_HANDLE;
        return 0;
    }
    vi.image = *img;
    vi.viewType = VK_IMAGE_VIEW_TYPE_3D;
    vi.format = VK_FORMAT_B8G8R8A8_UNORM;
    vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vi.subresourceRange.levelCount = 1;
    vi.subresourceRange.layerCount = 1;
    if (vkCreateImageView(s_dev, &vi, NULL, view) != VK_SUCCESS) {
        vkDestroyImage(s_dev, *img, NULL);
        vkFreeMemory(s_dev, *mem, NULL);
        *img = VK_NULL_HANDLE; *mem = VK_NULL_HANDLE;
        return 0;
    }
    image_to_general(*img, VK_IMAGE_ASPECT_COLOR_BIT);
    return 1;
}

/* Swizzled offset of (u, v, p): the bits of the three coordinates
 * interleaved u, v, p from the lowest, each while its size has bits left. */
static uint32_t swizzle3_index(uint32_t u, uint32_t v, uint32_t p, uint32_t w, uint32_t h, uint32_t d)
{
    uint32_t out = 0, bit = 0, mw = w - 1, mh = h - 1, md = d - 1, m;
    for (m = 1; mw >= m || mh >= m || md >= m; m <<= 1) {
        if (mw >= m) { if (u & m) out |= 1u << bit; bit++; }
        if (mh >= m) { if (v & m) out |= 1u << bit; bit++; }
        if (md >= m) { if (p & m) out |= 1u << bit; bit++; }
    }
    return out;
}

/* One swizzled texel of an uncompressed format as B8G8R8A8 (memory order). */
static uint32_t texel_bgra(const uint8_t *s, uint32_t color)
{
    uint32_t x, r, g, b, a;
    switch (color) {
    case 0x00: x = s[0]; return 0xFF000000u | x << 16 | x << 8 | x;      /* Y8 */
    case 0x01: x = s[0]; return x << 24 | x << 16 | x << 8 | x;          /* AY8 */
    case 0x19: return (uint32_t)s[0] << 24 | 0x00FFFFFFu;                /* A8 */
    case 0x1A: return (uint32_t)s[1] << 24 | s[0] << 16 | s[0] << 8 | s[0]; /* A8Y8 */
    case 0x02: case 0x03:                                               /* A1R5G5B5 */
        x = s[0] | s[1] << 8;
        r = (x >> 10) & 31; g = (x >> 5) & 31; b = x & 31;
        a = color == 0x03 || (x & 0x8000) ? 255 : 0;
        return a << 24 | (r * 255 / 31) << 16 | (g * 255 / 31) << 8 | (b * 255 / 31);
    case 0x04:                                                          /* A4R4G4B4 */
        x = s[0] | s[1] << 8;
        return ((x >> 12) & 15) * 17u << 24 | ((x >> 8) & 15) * 17u << 16
             | ((x >> 4) & 15) * 17u << 8 | (x & 15) * 17u;
    case 0x05:                                                          /* R5G6B5 */
        x = s[0] | s[1] << 8;
        r = (x >> 11) & 31; g = (x >> 5) & 63; b = x & 31;
        return 0xFF000000u | (r * 255 / 31) << 16 | (g * 255 / 63) << 8 | (b * 255 / 31);
    case 0x06: return s[0] | s[1] << 8 | s[2] << 16 | (uint32_t)s[3] << 24; /* A8R8G8B8 */
    case 0x07: return s[0] | s[1] << 8 | s[2] << 16 | 0xFF000000u;          /* X8R8G8B8 */
    default:   return 0xFF808080u;
    }
}

static uint32_t texel_bytes(uint32_t color)
{
    switch (color) {
    case 0x00: case 0x01: case 0x19: return 1;
    case 0x02: case 0x03: case 0x04: case 0x05: case 0x1A: return 2;
    default: return 4;
    }
}

static VkImageView dummy3_view(void)
{
    if (!s_dummy3_view && make_image_3d(1, 1, 1, &s_dummy3_img, &s_dummy3_view, &s_dummy3_mem))
        clear_image_color(s_dummy3_img, 0, 0, 0, 0);
    return s_dummy3_view;
}

static VkImageView tex3d_get(uint32_t va, uint32_t color, uint32_t w, uint32_t h, uint32_t d)
{
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    uint32_t bpp = texel_bytes(color), bytes, hash, x, y, z, i;
    VkTex3 *t = NULL, *victim = NULL;
    uint32_t *dst;
    VkDeviceSize at;
    VkBufferImageCopy rg;

    va = nv2a_clean_va(va);
    if (!va || w > 512 || h > 512 || d > 256 || (color >= 0x0C && color <= 0x0F))
        return dummy3_view();                 /* DXT volumes: not yet */
    bytes = w * h * d * bpp;
    hash = bytes_hash(va, bytes);
    for (i = 0; i < VK_MAX_TEX3; i++) {
        VkTex3 *c = &s_tex3[i];
        if (c->image && c->va == va && c->color == color && c->w == w && c->h == h && c->d == d) {
            t = c;
            break;
        }
        if (!victim || (victim->image && (!c->image || c->used < victim->used)))
            victim = c;
    }
    if (t && t->hash == hash) {
        t->used = s_frame;
        return t->view;
    }
    if (!t) {
        t = victim;
        if (t->image)
            garbage_add(t->image, t->view, t->mem);
        memset(t, 0, sizeof *t);
        if (!make_image_3d(w, h, d, &t->image, &t->view, &t->mem)) {
            memset(t, 0, sizeof *t);
            return dummy3_view();
        }
        t->va = va; t->color = color; t->w = w; t->h = h; t->d = d;
        fprintf(stderr, "[TEX3D] volume %08X format %02X %ux%ux%u\n", va, color, w, h, d);
    }
    t->hash = hash;
    t->used = s_frame;
    at = ring_alloc((VkDeviceSize)w * h * d * 4, 16, (void **)&dst);
    if (!dst)
        return t->view;
    for (z = 0; z < d; z++)
        for (y = 0; y < h; y++)
            for (x = 0; x < w; x++)
                dst[((size_t)z * h + y) * w + x] =
                    texel_bgra(mem + va + swizzle3_index(x, y, z, w, h, d) * bpp, color);
    end_rendering();
    barrier_all();
    memset(&rg, 0, sizeof rg);
    rg.bufferOffset = at;
    rg.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    rg.imageSubresource.layerCount = 1;
    rg.imageExtent.width = w;
    rg.imageExtent.height = h;
    rg.imageExtent.depth = d;
    vkCmdCopyBufferToImage(s_cb, s_f->ring, t->image, VK_IMAGE_LAYOUT_GENERAL, 1, &rg);
    return t->view;
}

/* Diagnostics: the 2D textures of a draw that reads a shadow buffer (Cold
 * Fear's flashlight), decoded as this renderer decodes them and written once
 * each as sdmc:/switch/coldfear/flashtex_<address>.bmp -- to tell a pattern
 * the title's own texture has from one this renderer adds. First 8. */
static void flash_tex_dump(uint32_t va, uint32_t color, uint32_t w, uint32_t h, uint32_t pitch)
{
    static uint32_t done[8];
    static int ndone;
    Nv2aTexture nt;
    uint32_t *px, x, y;
    char path[128];
    FILE *f;
    int k;

    for (k = 0; k < ndone; k++)
        if (done[k] == va)
            return;
    if (ndone >= 8 || w > 1024 || h > 1024)
        return;
    done[ndone++] = va;
    px = (uint32_t *)malloc((size_t)w * h * 4);
    if (!px)
        return;
    memset(&nt, 0, sizeof nt);
    nt.offset = va; nt.width = w; nt.height = h; nt.pitch = pitch; nt.color = color;
    nt.addr_u = nt.addr_v = 1;
    if (!nv2a_backend_decode_texture(&nt, px)) {
        fprintf(stderr, "[FLASHTEX] %08X format %02X %ux%u: not decodable\n", va, color, w, h);
        free(px);
        return;
    }
#if defined(__SWITCH__)
    snprintf(path, sizeof path, "sdmc:/switch/coldfear/flashtex_%08X.bmp", va);
#else
    snprintf(path, sizeof path, "flashtex_%08X.bmp", va);
#endif
    f = fopen(path, "wb");
    if (f) {
        uint8_t hd[54];
        uint32_t size = 54 + w * h * 4;
        memset(hd, 0, sizeof hd);
        hd[0] = 'B'; hd[1] = 'M';
        memcpy(hd + 2, &size, 4);
        hd[10] = 54; hd[14] = 40;
        memcpy(hd + 18, &w, 4);
        memcpy(hd + 22, &h, 4);
        hd[26] = 1; hd[28] = 32;
        fwrite(hd, 1, sizeof hd, f);
        for (y = h; y-- > 0; )                  /* BMP rows bottom-up */
            for (x = 0; x < w; x++)
                fwrite(&px[(size_t)y * w + x], 4, 1, f);
        fclose(f);
    }
    fprintf(stderr, "[FLASHTEX] %08X format %02X %ux%u pitch %u -> %s%s\n", va, color, w, h, pitch,
            path, f ? "" : " (could not write)");
    free(px);
}

/* Stage i: its image view (a surface or a cached texture), sampler and
 * texcoord scale. Records uploads, so call it outside rendering or accept
 * that it ends the pass. */
static void bind_stage(const uint32_t *regs, int i, float scale[2], VkDescriptorImageInfo *out,
                       VkSurf **sampled)
{
    /* Texture mode CUBE_MAP: the shader declares a samplerCube (gl_psh.c),
     * so the descriptor must be a cube view whatever is bound. */
    int cube = ((regs[0x1E70 / 4] >> (5 * i)) & 0x1F) == 3;
    /* RECOMP_VK_CUBE=0: cube stages sample the black dummy (no car
     * reflections, the old look). */
    static int cube_on = -1;
    if (cube_on < 0) {
        const char *e = getenv("RECOMP_VK_CUBE");
        cube_on = !(e && *e == '0');
    }
    uint32_t base = (0x1B00u + (uint32_t)i * 0x40u) / 4;
    uint32_t control0 = regs[base + 3];
    uint32_t format = regs[base + 1];
    uint32_t color = (format >> 8) & 0xFF;
    uint32_t va, w, h, pitch = 0, addr, filter, key;
    VkSurf *rt;

    scale[0] = scale[1] = 1.0f;
    out->imageView = cube ? s_dummy_cube_view : s_dummy_view;
    out->imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    out->sampler = sampler_get(0);
    *sampled = NULL;
    if (!(control0 & 0x40000000u) || !regs[base])
        return;
    va = nv2a_clean_va(s_batch->tex_va[i]);
    {
        uint32_t kind = stage_kind(regs, i);
        if (kind == NV2A_TEX_3D) {
            /* A volume: the shader declares a sampler3D (gl_psh.c). */
            out->imageView = tex3d_get(va, color, 1u << ((format >> 20) & 0xF),
                                       1u << ((format >> 24) & 0xF), 1u << ((format >> 28) & 0xF));
            addr = regs[base + 2];
            filter = regs[base + 5];
            /* U, V and the P wrap (SET_TEXTURE_ADDRESS bits 19:16): a
             * light's attenuation volume clamps its third axis, and
             * repeating it laid bands of light and dark with straight edges
             * across Cold Fear's decks. */
            key = (addr & 0xF0F) | ((addr >> 16) & 0xF) << 24;
            if (((filter >> 24) & 0xF) == 1) key |= 0x10000;
            if (((filter >> 16) & 0xFF) == 1) key |= 0x20000;
            out->sampler = sampler_get(key);
            return;
        }
        if (kind == NV2A_TEX_SHADOW24 || kind == NV2A_TEX_SHADOW16) {
            /* A shadow buffer: one of this renderer's depth buffers, read
             * once its drawing is behind a barrier, nearest-filtered. */
            VkDepthBuf *db;
            uint32_t tw = 1u << ((format >> 20) & 0xF), th = 1u << ((format >> 24) & 0xF);
            if (!tex_size_from_format(color)) {
                uint32_t rect = regs[base + 7];
                tw = rect >> 16; th = rect & 0xFFFF;
                if (tw) scale[0] = 1.0f / (float)tw;
                if (th) scale[1] = 1.0f / (float)th;
            }
            db = depth_find(va, tw, th);
            {
                static int logged;
                if (db && logged < 12 && (db->tw != tw || db->th != th)) {
                    logged++;
                    fprintf(stderr, "[SHADOW] texture %08X %ux%u has no depth buffer of its size;"
                            " using %ux%u\n", va, tw, th, db->tw, db->th);
                }
            }
            if (db && db != s_draw_depth) {
                if (db == s_rt_depth || db->dirty)
                    end_rendering();     /* the shadow pass may still be open */
                if (db->dirty) {
                    barrier_all();
                    db->dirty = 0;
                }
                out->imageView = db->sview;
            } else {
                static int missing;
                if (missing++ < 8) {
                    int k;
                    fprintf(stderr, "[SHADOW] stage %d: no depth buffer at %08X (raw %08X)%s; have:",
                            i, va, s_batch->tex_va[i], db ? " (it is the current target)" : "");
                    for (k = 0; k < VK_MAX_DEPTH; k++)
                        if (s_depth[k].image)
                            fprintf(stderr, " %08X/%ux%u", s_depth[k].va, s_depth[k].pw, s_depth[k].ph);
                    fprintf(stderr, "\n");
                }
            }
            out->sampler = sampler_get((regs[base + 2] & 0xF0F) | 0x30000);
            return;
        }
    }
    if (tex_size_from_format(color)) {
        w = 1u << ((format >> 20) & 0xF);
        h = 1u << ((format >> 24) & 0xF);
    } else {
        uint32_t rect = regs[base + 7];
        w = rect >> 16;
        h = rect & 0xFFFF;
        pitch = regs[base + 4] >> 16;
        if (w) scale[0] = 1.0f / (float)w;
        if (h) scale[1] = 1.0f / (float)h;
    }
    if (!w || !h || w > 4096 || h > 4096)
        return;
    s_palette_reg = regs[base + 8];
    s_palette_va = nv2a_clean_va(s_batch->pal_va[i]);
    rt = cube ? NULL : surf_find(va);
    if (!cube && !rt && i > 0) {
        uint32_t k0 = stage_kind(regs, 0);
        if (k0 == NV2A_TEX_SHADOW24 || k0 == NV2A_TEX_SHADOW16)
            flash_tex_dump(va, color, w, h, pitch);
    }
    if (cube) {
        /* SET_TEXTURE_FORMAT bit 2: CUBEMAP_ENABLE; mip levels in [19:16]. */
        if (cube_on && (format & 4)) {
            uint32_t face = cube_face_bytes(color, w, h, (format >> 16) & 0xF), f;
            VkSurf *faces[6];
            int any = 0;
            for (f = 0; f < 6; f++)
                any |= (faces[f] = surf_find(va + f * face)) != NULL;
            if (any) {
                out->imageView = cube_from_surfaces(va, w, h, face, faces);
            } else {
                uint32_t texs = s_hz_texs;
                uint64_t t0 = hz_now();
                out->imageView = tex_get(va, color, w, h, 0, face);
                if (s_hz_texs != texs)
                    s_hz_tex_ns += hz_now() - t0;
            }
        }
    } else if (rt) {
        out->imageView = rt->view;
        *sampled = rt;
        rt_scale(w, h, rt->w, rt->h, rt->aa_sx, rt->aa_sy, scale);
    } else {
        uint32_t texs = s_hz_texs;
        uint64_t t0 = hz_now();
        out->imageView = tex_get(va, color, w, h, pitch, 0);
        if (s_hz_texs != texs)
            s_hz_tex_ns += hz_now() - t0;
    }
    addr = regs[base + 2];
    filter = regs[base + 5];
    key = (addr & 0xF0F);
    if (((filter >> 24) & 0xF) == 1) key |= 0x10000;
    if (((filter >> 16) & 0xFF) == 1) key |= 0x20000;
    out->sampler = sampler_get(key);
}

/* ── Shaders and pipelines ────────────────────────────────────────── */

static VkDescriptorSetLayout s_dsl;
static VkPipelineLayout      s_layout;
static VkPipelineCache       s_pcache;

typedef struct {
    uint64_t   vkey;
    Nv2aPshKey pkey;
    VkShaderModule vs, fs;
    uint32_t   used;
    uint32_t   hash;
    int        next;
} VkProg;

#define VK_MAX_PROG 1024
#define PROG_BUCKETS 1024
static VkProg s_prog[VK_MAX_PROG];
static int s_prog_head[PROG_BUCKETS];
static uint32_t s_prog_made, s_prog_live;

uint32_t nv2a_gl_program_count(void) { return s_prog_live; }
/* The GL renderer's thread statistics; the Vulkan one records on the
 * executor's thread. */
void nv2a_gl_queue_stats(double secs, int *gl_idle_pct, int *exec_wait_pct)
{
    (void)secs;
    if (gl_idle_pct) *gl_idle_pct = 0;
    if (exec_wait_pct) *exec_wait_pct = 0;
}

typedef struct {
    glslang_stage_t stage;
    const char *src;
    uint32_t *words;
    size_t nwords;
    int ok;
} CompileJob;

static void *compile_thread(void *arg)
{
    CompileJob *j = (CompileJob *)arg;
    glslang_input_t in;
    glslang_shader_t *sh;
    glslang_program_t *pr;

    memset(&in, 0, sizeof in);
    in.language = GLSLANG_SOURCE_GLSL;
    in.stage = j->stage;
    in.client = GLSLANG_CLIENT_VULKAN;
    in.client_version = GLSLANG_TARGET_VULKAN_1_1;
    in.target_language = GLSLANG_TARGET_SPV;
    in.target_language_version = GLSLANG_TARGET_SPV_1_3;
    in.code = j->src;
    in.default_version = 450;
    in.default_profile = GLSLANG_NO_PROFILE;
    in.messages = GLSLANG_MSG_DEFAULT_BIT;
    in.resource = glslang_default_resource();
    sh = glslang_shader_create(&in);
    if (!glslang_shader_preprocess(sh, &in) || !glslang_shader_parse(sh, &in)) {
        LOGE("%s shader failed:\n%s\n", j->stage == GLSLANG_STAGE_VERTEX ? "vertex" : "fragment",
             glslang_shader_get_info_log(sh));
        if (s_trace)
            fprintf(stderr, "%s\n", j->src);
        glslang_shader_delete(sh);
        return NULL;
    }
    pr = glslang_program_create();
    glslang_program_add_shader(pr, sh);
    if (!glslang_program_link(pr, GLSLANG_MSG_SPV_RULES_BIT | GLSLANG_MSG_VULKAN_RULES_BIT)) {
        LOGE("link failed: %s\n", glslang_program_get_info_log(pr));
    } else {
        glslang_program_SPIRV_generate(pr, j->stage);
        j->nwords = glslang_program_SPIRV_get_size(pr);
        j->words = (uint32_t *)malloc(j->nwords * 4);
        if (j->words) {
            glslang_program_SPIRV_get(pr, j->words);
            j->ok = 1;
        }
    }
    glslang_program_delete(pr);
    glslang_shader_delete(sh);
    return NULL;
}

/* ── Caches across runs ───────────────────────────────────────────
 *
 * A program compiled at first use stalled a race start for ~1 s (glslang,
 * then NIR and NAK inside NVK: ~50 ms each on the console). Three files, next
 * to the NRO (the current directory on Linux); RECOMP_PROG_CACHE=0 disables
 * them all, =<path> moves the program list:
 *   progcache.bin    what each program was built from -- the GL renderer's
 *                    records, shared (the format does not depend on the back end)
 *   vkspirv.bin      glslang's output by hash of the GLSL source
 *   vkpipes.bin      the pipeline variants seen (program + fixed state)
 *   vkpipecache.bin  the driver's VkPipelineCache (NAK binaries), saved while
 *                    running whenever pipelines were added
 * At start-up every recorded program and pipeline is built before the title
 * draws. */
static int s_prewarming;
static int s_cache_off = -1;

/* RECOMP_CACHE_DIR (with its trailing slash), set by the port for its own
 * folder; without it, the folder this back end was first written for. */
static const char *cache_dir(void)
{
    const char *e = getenv("RECOMP_CACHE_DIR");
    if (e && *e)
        return e;
#if defined(__SWITCH__)
    return "sdmc:/switch/nfsu2x/";
#else
    return "";
#endif
}

static int cache_path(char *out, size_t cap, const char *name)
{
    if (s_cache_off < 0) {
        const char *e = getenv("RECOMP_PROG_CACHE");
        s_cache_off = e && e[0] == '0' && !e[1];
    }
    if (s_cache_off)
        return 0;
    if (!strcmp(name, "progcache.bin")) {
        const char *e = getenv("RECOMP_PROG_CACHE");
        if (e && *e) {
            snprintf(out, cap, "%s", e);
            return 1;
        }
    }
    snprintf(out, cap, "%s%s", cache_dir(), name);
    return 1;
}

#define SPV_MAGIC 0x31565053u                   /* "SPV1" */
typedef struct { uint64_t hash; uint32_t stage, nwords; uint32_t *words; } SpvEnt;
static SpvEnt *s_spv;
static uint32_t s_nspv, s_spv_cap, s_spv_hits;

static uint64_t text_hash(const char *t, uint32_t stage)
{
    uint64_t h = 1469598103934665603ull ^ stage;
    while (*t)
        h = (h ^ (uint8_t)*t++) * 1099511628211ull;
    return h;
}

static void spv_add(uint64_t hash, uint32_t stage, const uint32_t *w, uint32_t n)
{
    if (s_nspv == s_spv_cap) {
        s_spv_cap = s_spv_cap ? s_spv_cap * 2 : 256;
        s_spv = (SpvEnt *)realloc(s_spv, s_spv_cap * sizeof *s_spv);
    }
    s_spv[s_nspv].hash = hash;
    s_spv[s_nspv].stage = stage;
    s_spv[s_nspv].nwords = n;
    s_spv[s_nspv].words = (uint32_t *)malloc((size_t)n * 4);
    memcpy(s_spv[s_nspv].words, w, (size_t)n * 4);
    s_nspv++;
}

static const SpvEnt *spv_find(uint64_t hash, uint32_t stage)
{
    uint32_t i;
    for (i = 0; i < s_nspv; i++)
        if (s_spv[i].hash == hash && s_spv[i].stage == stage)
            return &s_spv[i];
    return NULL;
}

static void spv_load(void)
{
    char path[256];
    FILE *f;
    uint32_t hdr[4];
    if (!cache_path(path, sizeof path, "vkspirv.bin") || !(f = fopen(path, "rb")))
        return;
    while (fread(hdr, sizeof hdr, 1, f) == 1) {
        uint64_t hash;
        uint32_t *w;
        if (hdr[0] != SPV_MAGIC || hdr[3] > (1u << 22))
            break;
        memcpy(&hash, &hdr[1], 8);
        w = (uint32_t *)malloc((size_t)hdr[3] * 8 + 8);
        if (!w || fread(w, 4, hdr[3], f) != hdr[3]) { free(w); break; }
        /* the stage is in the word after the header */
        spv_add(hash, w[0], w + 1, hdr[3] - 1);
        free(w);
    }
    fclose(f);
}

static void spv_append(uint64_t hash, uint32_t stage, const uint32_t *w, uint32_t n)
{
    char path[256];
    FILE *f;
    uint32_t hdr[4];
    if (!cache_path(path, sizeof path, "vkspirv.bin") || !(f = fopen(path, "ab")))
        return;
    hdr[0] = SPV_MAGIC;
    memcpy(&hdr[1], &hash, 8);
    hdr[3] = n + 1;
    fwrite(hdr, sizeof hdr, 1, f);
    fwrite(&stage, 4, 1, f);
    fwrite(w, 4, n, f);
    fclose(f);
}

/* GLSL to a shader module. glslang's parser recurses deeply: it runs on a
 * thread with a 4 MB stack (the executor's is smaller). */
static VkShaderModule compile(glslang_stage_t stage, const char *src)
{
    CompileJob j = { stage, src, NULL, 0, 0 };
    pthread_t t;
    pthread_attr_t at;
    VkShaderModuleCreateInfo ci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    VkShaderModule m = VK_NULL_HANDLE;

    uint64_t hash = text_hash(src, (uint32_t)stage);
    const SpvEnt *hit = spv_find(hash, (uint32_t)stage);

    if (hit) {
        s_spv_hits++;
        ci.codeSize = (size_t)hit->nwords * 4;
        ci.pCode = hit->words;
        vkCreateShaderModule(s_dev, &ci, NULL, &m);
        return m;
    }
    VT("compile %s (%u bytes of GLSL)\n", stage == GLSLANG_STAGE_VERTEX ? "vs" : "fs",
       (unsigned)strlen(src));
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 4u << 20);
    if (pthread_create(&t, &at, compile_thread, &j) == 0)
        pthread_join(t, NULL);
    else
        compile_thread(&j);
    pthread_attr_destroy(&at);
    if (!j.ok)
        return VK_NULL_HANDLE;
    ci.codeSize = j.nwords * 4;
    ci.pCode = j.words;
    VT("  SPIR-V %u words -> vkCreateShaderModule\n", (unsigned)j.nwords);
    vkCreateShaderModule(s_dev, &ci, NULL, &m);
    spv_add(hash, (uint32_t)stage, j.words, (uint32_t)j.nwords);
    spv_append(hash, (uint32_t)stage, j.words, (uint32_t)j.nwords);
    free(j.words);
    return m;
}

static uint64_t prog_hash(const Nv2aRawBatch *b)
{
    uint64_t h = 1469598103934665603ull;
    uint32_t pc, k;
    for (pc = b->vp_start; pc < b->vp_slots; pc++) {
        for (k = 0; k < 4; k++)
            h = (h ^ b->vp_program[pc][k]) * 1099511628211ull;
        if (b->vp_program[pc][3] & 1)
            break;
    }
    return h ? h : 1;
}

static uint32_t prog_bucket(uint64_t vkey, const Nv2aPshKey *pk)
{
    const uint32_t *w = (const uint32_t *)pk;
    uint64_t h = vkey ^ 1469598103934665603ull;
    size_t i;
    for (i = 0; i < sizeof *pk / 4; i++)
        h = (h ^ w[i]) * 1099511628211ull;
    return (uint32_t)(h ^ (h >> 32)) & (PROG_BUCKETS - 1);
}

static void pipes_forget_prog(int prog);

/* progcache.bin records: nv2a_gl.c's ProgRec, byte for byte. */
#define PROG_REC_MAGIC 0x3250564Eu              /* "NVP2": Nv2aPshKey gained tex_kind */
typedef struct {
    uint32_t   magic, xform, vp_start, slots;
    Nv2aPshKey pk;
    uint32_t   prog[136][4];
} ProgRec;

static void prog_rec_fill(ProgRec *rec, const Nv2aRawBatch *b, const Nv2aPshKey *pk)
{
    memset(rec, 0, sizeof *rec);
    rec->magic = PROG_REC_MAGIC;
    rec->xform = b->xform == 2 ? 2 : 1;
    rec->pk = *pk;
    if (rec->xform == 2 && b->vp_program && b->vp_slots <= 136) {
        rec->vp_start = b->vp_start;
        rec->slots = b->vp_slots;
        memcpy(rec->prog, b->vp_program, (size_t)b->vp_slots * 16);
    }
}

static void prog_rec_append(const Nv2aRawBatch *b, const Nv2aPshKey *pk)
{
    char path[256];
    ProgRec rec;
    FILE *f;
    if (s_prewarming || !cache_path(path, sizeof path, "progcache.bin"))
        return;
    prog_rec_fill(&rec, b, pk);
    if ((f = fopen(path, "ab"))) {
        fwrite(&rec, sizeof rec, 1, f);
        fclose(f);
    }
}

/* The batch a record describes (vertex program slots in rec). */
static void prog_rec_batch(const ProgRec *rec, Nv2aRawBatch *bb, uint32_t n)
{
    memset(bb, 0, sizeof *bb);
    bb->xform = rec->xform;
    bb->vp_program = (const uint32_t (*)[4])rec->prog;
    bb->vp_slots = rec->slots ? rec->slots : 136;
    bb->vp_start = rec->vp_start;
    bb->vp_prog_gen = 0x80000000u + n;          /* hash it: not an executor version */
}

static VkProg *prog_get(const Nv2aRawBatch *b, const Nv2aPshKey *pk)
{
    static uint32_t last_gen;
    static uint64_t last_vkey;
    static VkProg *last;
    static char vsrc[320 * 1024], fsrc[64 * 1024];
    uint64_t vkey = 0;
    uint32_t bucket;
    VkProg *p, *victim = NULL;
    int i;
    size_t n;

    if (b->xform == 2) {
        if (b->vp_prog_gen != last_gen || !last_vkey) {
            last_vkey = prog_hash(b);
            last_gen = b->vp_prog_gen;
        }
        vkey = last_vkey;
    }
    if (last && last->vs && last->vkey == vkey && !memcmp(&last->pkey, pk, sizeof *pk)) {
        last->used = s_frame;
        return last;
    }
    bucket = prog_bucket(vkey, pk);
    for (i = s_prog_head[bucket]; i; i = s_prog[i - 1].next) {
        VkProg *c = &s_prog[i - 1];
        if (c->vs && c->vkey == vkey && !memcmp(&c->pkey, pk, sizeof *pk)) {
            c->used = s_frame;
            last = c;
            return c;
        }
    }
    for (i = 0; i < VK_MAX_PROG; i++) {
        VkProg *c = &s_prog[i];
        if (!c->vs) { if (!victim || victim->vs) victim = c; }
        else if (!victim || (victim->vs && c->used < victim->used)) victim = c;
    }
    p = victim;
    last = NULL;
    if (p->vs) {
        int *at = &s_prog_head[p->hash], me = (int)(p - s_prog) + 1;
        while (*at) {
            if (*at == me) { *at = p->next; break; }
            at = &s_prog[*at - 1].next;
        }
        pipes_forget_prog((int)(p - s_prog));
        vkDestroyShaderModule(s_dev, p->vs, NULL);
        vkDestroyShaderModule(s_dev, p->fs, NULL);
        s_prog_live--;
    }
    memset(p, 0, sizeof *p);

    n = (size_t)snprintf(vsrc, sizeof vsrc, "%s", nv2a_gl_vsh_prelude());
    if (vkey) {
        int k = nv2a_gl_vsh_program(b->vp_program, b->vp_slots, b->vp_start, vsrc + n, sizeof vsrc - n);
        if (k < 0)
            return NULL;
        n += (size_t)k;
        snprintf(vsrc + n, sizeof vsrc - n, "%s", nv2a_gl_vsh_main_program());
    } else {
        snprintf(vsrc + n, sizeof vsrc - n, "%s", nv2a_gl_vsh_fixed());
    }
    if (nv2a_gl_psh(pk, fsrc, sizeof fsrc) < 0)
        return NULL;
    p->vs = compile(GLSLANG_STAGE_VERTEX, vsrc);
    p->fs = compile(GLSLANG_STAGE_FRAGMENT, fsrc);
    if (!p->vs || !p->fs) {
        if (p->vs) vkDestroyShaderModule(s_dev, p->vs, NULL);
        if (p->fs) vkDestroyShaderModule(s_dev, p->fs, NULL);
        memset(p, 0, sizeof *p);
        return NULL;
    }
    s_prog_live++;
    if ((++s_prog_made % 100) == 0)
        LOGE("%u shader programs compiled (%u live)\n", s_prog_made, s_prog_live);
    p->vkey = vkey;
    p->pkey = *pk;
    p->used = s_frame;
    p->hash = bucket;
    p->next = s_prog_head[bucket];
    s_prog_head[bucket] = (int)(p - s_prog) + 1;
    last = p;
    prog_rec_append(b, pk);
    return p;
}

typedef struct {
    int      prog;
    uint32_t blend, bsrc, bdst, beq, cmask, depth, topo;
} PipeKey;

typedef struct {
    PipeKey key;
    VkPipeline pipe;
    uint32_t hash;
    int next;
} PipeEnt;

#define VK_MAX_PIPE 4096
#define PIPE_BUCKETS 4096
static PipeEnt s_pipes[VK_MAX_PIPE];
static int s_pipe_head[PIPE_BUCKETS];
static int s_npipes;

static void pipes_forget_prog(int prog)
{
    int i;
    /* Rare (1024 live programs): drop every pipeline using it. */
    for (i = 0; i < s_npipes; i++) {
        PipeEnt *e = &s_pipes[i];
        if (e->pipe && e->key.prog == prog) {
            int *at = &s_pipe_head[e->hash], me = i + 1;
            while (*at) {
                if (*at == me) { *at = e->next; break; }
                at = &s_pipes[*at - 1].next;
            }
            garbage_add_pipe(e->pipe);               /* may still be in flight */
            e->pipe = VK_NULL_HANDLE;
        }
    }
}

static VkBlendFactor blend_factor(uint32_t v)
{
    switch (v) {
    case 0: return VK_BLEND_FACTOR_ZERO;
    case 1: return VK_BLEND_FACTOR_ONE;
    case 0x300: return VK_BLEND_FACTOR_SRC_COLOR;
    case 0x301: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case 0x302: return VK_BLEND_FACTOR_SRC_ALPHA;
    case 0x303: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 0x304: return VK_BLEND_FACTOR_DST_ALPHA;
    case 0x305: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case 0x306: return VK_BLEND_FACTOR_DST_COLOR;
    case 0x307: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case 0x308: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    case 0x8001: return VK_BLEND_FACTOR_CONSTANT_COLOR;
    case 0x8002: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case 0x8003: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
    case 0x8004: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    default: return VK_BLEND_FACTOR_ONE;
    }
}

static VkBlendOp blend_op(uint32_t v)
{
    switch (v) {
    case 0x800A: return VK_BLEND_OP_SUBTRACT;
    case 0x800B: return VK_BLEND_OP_REVERSE_SUBTRACT;
    case 0x8007: return VK_BLEND_OP_MIN;
    case 0x8008: return VK_BLEND_OP_MAX;
    default: return VK_BLEND_OP_ADD;
    }
}

/* vkpipes.bin: the program by what generated it, and the pipeline's own
 * state. */
#define PIPE_REC_MAGIC 0x32504B56u              /* "VKP2": Nv2aPshKey gained tex_kind */
typedef struct {
    uint32_t   magic, pad;
    uint64_t   vkey;
    Nv2aPshKey pk;
    uint32_t   blend, bsrc, bdst, beq, cmask, depth, topo;
} PipeRec;
static uint32_t s_pipes_unsaved;

static void pipe_rec_append(const PipeKey *k)
{
    char path[256];
    PipeRec rec;
    FILE *f;
    if (s_prewarming || !cache_path(path, sizeof path, "vkpipes.bin"))
        return;
    memset(&rec, 0, sizeof rec);
    rec.magic = PIPE_REC_MAGIC;
    rec.vkey = s_prog[k->prog].vkey;
    rec.pk = s_prog[k->prog].pkey;
    rec.blend = k->blend; rec.bsrc = k->bsrc; rec.bdst = k->bdst; rec.beq = k->beq;
    rec.cmask = k->cmask; rec.depth = k->depth; rec.topo = k->topo;
    if ((f = fopen(path, "ab"))) {
        fwrite(&rec, sizeof rec, 1, f);
        fclose(f);
    }
}

static VkPipeline pipe_get(const PipeKey *k)
{
    const uint32_t *w = (const uint32_t *)k;
    uint32_t h = 2166136261u, i;
    int e;
    PipeEnt *pe;
    VkGraphicsPipelineCreateInfo ci = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    VkPipelineShaderStageCreateInfo st[2];
    VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    VkPipelineViewportStateCreateInfo vp = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    VkPipelineDepthStencilStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    VkPipelineColorBlendAttachmentState ba;
    VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    VkPipelineDynamicStateCreateInfo dy = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    VkPipelineRenderingCreateInfo rci = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
    VkFormat cfmt = VK_FORMAT_B8G8R8A8_UNORM;
    static const VkDynamicState dyn[] = {
        VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_BLEND_CONSTANTS,
        VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,
        VK_DYNAMIC_STATE_STENCIL_REFERENCE, VK_DYNAMIC_STATE_CULL_MODE,
        VK_DYNAMIC_STATE_FRONT_FACE, VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY,
        VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE, VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE,
        VK_DYNAMIC_STATE_DEPTH_COMPARE_OP, VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE,
        VK_DYNAMIC_STATE_STENCIL_OP, VK_DYNAMIC_STATE_VERTEX_INPUT_EXT,
        VK_DYNAMIC_STATE_DEPTH_BIAS,
    };
    VkPipeline pipe = VK_NULL_HANDLE;
    VkProg *p = &s_prog[k->prog];

    for (i = 0; i < sizeof *k / 4; i++)
        h = (h ^ w[i]) * 16777619u;
    h &= PIPE_BUCKETS - 1;
    for (e = s_pipe_head[h]; e; e = s_pipes[e - 1].next)
        if (s_pipes[e - 1].pipe && !memcmp(&s_pipes[e - 1].key, k, sizeof *k))
            return s_pipes[e - 1].pipe;

    memset(st, 0, sizeof st);
    st[0].sType = st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    st[0].module = p->vs;
    st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    st[1].module = p->fs;
    st[1].pName = "main";
    ia.topology = k->topo == 0 ? VK_PRIMITIVE_TOPOLOGY_POINT_LIST
                : k->topo == 1 ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST
                               : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.lineWidth = 1.0f;
    rs.depthClampEnable = s_have_depth_clamp ? VK_TRUE : VK_FALSE;
    rs.depthBiasEnable = VK_TRUE;            /* values per draw: apply_dynamic */
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    memset(&ba, 0, sizeof ba);
    ba.blendEnable = k->blend ? VK_TRUE : VK_FALSE;
    ba.srcColorBlendFactor = ba.srcAlphaBlendFactor = blend_factor(k->bsrc);
    ba.dstColorBlendFactor = ba.dstAlphaBlendFactor = blend_factor(k->bdst);
    ba.colorBlendOp = ba.alphaBlendOp = blend_op(k->beq);
    ba.colorWriteMask = k->cmask;
    cb.attachmentCount = 1;
    cb.pAttachments = &ba;
    dy.dynamicStateCount = (uint32_t)(sizeof dyn / sizeof dyn[0]);
    dy.pDynamicStates = dyn;
    rci.colorAttachmentCount = 1;
    rci.pColorAttachmentFormats = &cfmt;
    if (k->depth) {
        rci.depthAttachmentFormat = s_depth_fmt;
        rci.stencilAttachmentFormat = s_depth_fmt;
    }
    ci.pNext = &rci;
    ci.stageCount = 2;
    ci.pStages = st;
    ci.pVertexInputState = &vi;
    ci.pInputAssemblyState = &ia;
    ci.pViewportState = &vp;
    ci.pRasterizationState = &rs;
    ci.pMultisampleState = &ms;
    ci.pDepthStencilState = &ds;
    ci.pColorBlendState = &cb;
    ci.pDynamicState = &dy;
    ci.layout = s_layout;
    VT("vkCreateGraphicsPipelines prog %d blend %u cmask %X depth %u topo %u\n", k->prog, k->blend,
       k->cmask, k->depth, k->topo);
    if (vkCreateGraphicsPipelines(s_dev, s_pcache, 1, &ci, NULL, &pipe) != VK_SUCCESS) {
        LOGE("pipeline creation failed\n");
        return VK_NULL_HANDLE;
    }
    if (s_npipes == VK_MAX_PIPE) {
        LOGE("pipeline table full\n");
        return pipe;                          /* leaks; never seen in practice */
    }
    pe = &s_pipes[s_npipes];
    pe->key = *k;
    pe->pipe = pipe;
    pe->hash = h;
    pe->next = s_pipe_head[h];
    s_pipe_head[h] = ++s_npipes;
    s_pipes_unsaved++;
    pipe_rec_append(k);
    return pipe;
}

static VkProg *prog_find(uint64_t vkey, const Nv2aPshKey *pk)
{
    int i;
    for (i = s_prog_head[prog_bucket(vkey, pk)]; i; i = s_prog[i - 1].next) {
        VkProg *c = &s_prog[i - 1];
        if (c->vs && c->vkey == vkey && !memcmp(&c->pkey, pk, sizeof *pk))
            return c;
    }
    return NULL;
}

static void *read_file(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    void *d = NULL;
    long n;
    *size = 0;
    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) == 0 && (n = ftell(f)) > 0 && fseek(f, 0, SEEK_SET) == 0) {
        d = malloc((size_t)n);
        if (d && fread(d, 1, (size_t)n, f) == (size_t)n)
            *size = (size_t)n;
        else { free(d); d = NULL; }
    }
    fclose(f);
    return d;
}

static void pcache_save(void)
{
    char path[256], tmp[272];
    size_t n = 0;
    void *d;
    FILE *f;
    if (!s_pcache || !cache_path(path, sizeof path, "vkpipecache.bin"))
        return;
    if (vkGetPipelineCacheData(s_dev, s_pcache, &n, NULL) != VK_SUCCESS || !n)
        return;
    d = malloc(n);
    if (!d || vkGetPipelineCacheData(s_dev, s_pcache, &n, d) != VK_SUCCESS) { free(d); return; }
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    if ((f = fopen(tmp, "wb"))) {
        int ok = fwrite(d, 1, n, f) == n;
        fclose(f);
        if (ok) {
            remove(path);                  /* FAT: rename does not replace */
            rename(tmp, path);
        }
    }
    free(d);
    s_pipes_unsaved = 0;
}

static void prewarm(void)
{
    char path[256];
    FILE *f;
    uint64_t t0 = hz_now();
    uint32_t nprog = 0, npipe = 0, bad = 0;
    ProgRec rec;
    PipeRec pr;

    spv_load();
    s_prewarming = 1;
    if (cache_path(path, sizeof path, "progcache.bin") && (f = fopen(path, "rb"))) {
        while (fread(&rec, sizeof rec, 1, f) == 1) {
            Nv2aRawBatch bb;
            if (rec.magic != PROG_REC_MAGIC || rec.slots > 136) { bad++; break; }
            prog_rec_batch(&rec, &bb, nprog);
            if (prog_get(&bb, &rec.pk))
                nprog++;
        }
        fclose(f);
    }
    if (cache_path(path, sizeof path, "vkpipes.bin") && (f = fopen(path, "rb"))) {
        while (fread(&pr, sizeof pr, 1, f) == 1) {
            VkProg *p;
            PipeKey k;
            if (pr.magic != PIPE_REC_MAGIC) { bad++; break; }
            if (!(p = prog_find(pr.vkey, &pr.pk)))
                continue;
            memset(&k, 0, sizeof k);
            k.prog = (int)(p - s_prog);
            k.blend = pr.blend; k.bsrc = pr.bsrc; k.bdst = pr.bdst; k.beq = pr.beq;
            k.cmask = pr.cmask; k.depth = pr.depth; k.topo = pr.topo;
            if (pipe_get(&k))
                npipe++;
        }
        fclose(f);
    }
    s_prewarming = 0;
    fprintf(stderr, "  [VK] prewarm: %u programs (%u SPIR-V from cache), %u pipelines in %.0f ms%s\n",
            nprog, s_spv_hits, npipe, (hz_now() - t0) / 1e6, bad ? " (a file ends in a bad record)" : "");
    if (s_pipes_unsaved)
        pcache_save();
}

/* ── Context ──────────────────────────────────────────────────────── */

static int create_swapchain(void)
{
    VkSurfaceCapabilitiesKHR caps;
    VkSurfaceFormatKHR fmts[16];
    VkPresentModeKHR modes[8], mode = VK_PRESENT_MODE_FIFO_KHR;
    uint32_t nf = 16, nm = 8, i;
    VkSwapchainCreateInfoKHR ci = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };

    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(s_pd, s_surface, &caps) != VK_SUCCESS)
        return 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(s_pd, s_surface, &nf, fmts);
    vkGetPhysicalDeviceSurfacePresentModesKHR(s_pd, s_surface, &nm, modes);
    s_sc_format = fmts[0].format;
    for (i = 0; i < nf; i++)
        if (fmts[i].format == VK_FORMAT_B8G8R8A8_UNORM || fmts[i].format == VK_FORMAT_R8G8B8A8_UNORM) {
            s_sc_format = fmts[i].format;
            ci.imageColorSpace = fmts[i].colorSpace;
            break;
        }
    /* By default on Nintendo Switch, lock to VK_PRESENT_MODE_FIFO_KHR (60 Hz VSync).
     * This locks the frame rate to authentic 60 FPS, prevents 2x game speedup,
     * and ensures FMV video and audio decoders are perfectly synchronized. */
    const char *pm = getenv("MKSM_PRESENT_MODE");
    if (pm && strcmp(pm, "immediate") == 0) {
        for (i = 0; i < nm; i++)
            if (modes[i] == VK_PRESENT_MODE_IMMEDIATE_KHR) mode = modes[i];
    } else if (pm && strcmp(pm, "mailbox") == 0) {
        for (i = 0; i < nm; i++)
            if (modes[i] == VK_PRESENT_MODE_MAILBOX_KHR) mode = modes[i];
    } else {
        mode = VK_PRESENT_MODE_FIFO_KHR;
    }
    s_sc_extent = caps.currentExtent;
    if (s_sc_extent.width == 0xFFFFFFFFu) {
        s_sc_extent.width = 1280;
        s_sc_extent.height = 720;
    }
    ci.surface = s_surface;
    ci.minImageCount = caps.minImageCount < 3 ? 3 : caps.minImageCount;
    if (caps.maxImageCount && ci.minImageCount > caps.maxImageCount)
        ci.minImageCount = caps.maxImageCount;
    ci.imageFormat = s_sc_format;
    ci.imageExtent = s_sc_extent;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    ci.presentMode = mode;
    ci.clipped = VK_TRUE;
    ci.oldSwapchain = s_swapchain;
    {
        VkSwapchainKHR sc;
        if (vkCreateSwapchainKHR(s_dev, &ci, NULL, &sc) != VK_SUCCESS)
            return 0;
        if (s_swapchain)
            vkDestroySwapchainKHR(s_dev, s_swapchain, NULL);
        s_swapchain = sc;
    }
    s_sc_count = 8;
    vkGetSwapchainImagesKHR(s_dev, s_swapchain, &s_sc_count, s_sc_images);
    fprintf(stderr, "  [VK] swapchain %ux%u, %u images, format %d, present mode %d\n",
            s_sc_extent.width, s_sc_extent.height, s_sc_count, s_sc_format, mode);
    return 1;
}

static int ready(void)
{
    VkApplicationInfo ai = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    const char *iext[8];
    uint32_t niext = 0, n, i;
    VkPhysicalDevice pds[8];
    VkPhysicalDeviceProperties pp;
    VkQueueFamilyProperties qf[8];
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    VkPhysicalDeviceFeatures2 f2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
    VkPhysicalDeviceVulkan13Features f13 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
    VkPhysicalDeviceVertexInputDynamicStateFeaturesEXT fvi =
        { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_INPUT_DYNAMIC_STATE_FEATURES_EXT };
    VkPhysicalDeviceExtendedDynamicStateFeaturesEXT feds =
        { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT };
    VkPhysicalDeviceFeatures have;
    const char *dext[4];
    uint32_t ndext = 0;
    const char *layers[1];
    uint32_t nlayers = 0;

    if (s_state)
        return s_state > 0;
    s_state = -1;
    s_trace = getenv("RECOMP_GL_TRACE") != NULL;
    {
        const char *e = getenv("RECOMP_VK_TRACE");
        s_vtrace = e ? atoi(e) : 0;
    }
    nv2a_shader_vk = 1;
    glslang_initialize_process();

    iext[niext++] = VK_KHR_SURFACE_EXTENSION_NAME;
#if defined(__SWITCH__)
    iext[niext++] = VK_NN_VI_SURFACE_EXTENSION_NAME;
#else
    {
        const char *e = getenv("RECOMP_VK_HEADLESS");
        const char *drv = getenv("SDL_VIDEODRIVER");
        s_headless = (e && *e == '1') || (drv && !strcmp(drv, "offscreen"));
        if (!s_headless) {
            if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
                s_headless = 1;
            } else {
                s_win = SDL_CreateWindow("NV2A (Vulkan)", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                         1280, 720, SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
                if (!s_win) {
                    s_headless = 1;
                } else {
                    unsigned int ne = 0;
                    const char *names[8];
                    SDL_Vulkan_GetInstanceExtensions(s_win, &ne, NULL);
                    if (ne > 6) ne = 6;
                    SDL_Vulkan_GetInstanceExtensions(s_win, &ne, names);
                    niext = 0;
                    for (i = 0; i < ne; i++)
                        iext[niext++] = names[i];
                }
            }
        }
        if (s_headless)
            niext = 0;
    }
    {
        const char *v = getenv("RECOMP_VK_VALIDATION");
        if (v && *v == '1')
            layers[nlayers++] = "VK_LAYER_KHRONOS_validation";
    }
#endif
    ai.pApplicationName = "nfsu2x";
    ai.pEngineName = "xboxrecomp";
    ai.apiVersion = VK_API_VERSION_1_3;
    ici.pApplicationInfo = &ai;
    ici.enabledExtensionCount = niext;
    ici.ppEnabledExtensionNames = iext;
    ici.enabledLayerCount = nlayers;
    ici.ppEnabledLayerNames = layers;
    if (vkCreateInstance(&ici, NULL, &s_inst) != VK_SUCCESS) {
        LOGE("vkCreateInstance failed\n");
        return 0;
    }
    n = 8;
    if (vkEnumeratePhysicalDevices(s_inst, &n, pds) != VK_SUCCESS || !n) {
        LOGE("no Vulkan device\n");
        return 0;
    }
    s_pd = pds[0];
    vkGetPhysicalDeviceProperties(s_pd, &pp);
    fprintf(stderr, "  [VK] %s, Vulkan %u.%u.%u\n", pp.deviceName, VK_API_VERSION_MAJOR(pp.apiVersion),
            VK_API_VERSION_MINOR(pp.apiVersion), VK_API_VERSION_PATCH(pp.apiVersion));
    s_ubo_align = pp.limits.minUniformBufferOffsetAlignment;
    s_ubo_range = pp.limits.maxUniformBufferRange;
    {
        const char *e = getenv("RECOMP_GL_SCALE");
        double k = e ? strtod(e, NULL) : 1.0;
        uint32_t m = pp.limits.maxImageDimension2D;
        if (pp.limits.maxFramebufferWidth < m) m = pp.limits.maxFramebufferWidth;
        if (pp.limits.maxFramebufferHeight < m) m = pp.limits.maxFramebufferHeight;
        s_max_size = m ? m : 4096;
        if (!(k > 0.0)) k = 1.0;                    /* unset, 0 or garbage */
        s_scale = k < 0.5 ? 0.5 : k > 4.0 ? 4.0 : k;
        if (s_scale != 1.0)
            fprintf(stderr, "  [VK] rendering at %gx (RECOMP_GL_SCALE), surfaces up to %u\n",
                    s_scale, s_max_size);
    }
    if (s_ubo_align < 16) s_ubo_align = 16;
    vkGetPhysicalDeviceMemoryProperties(s_pd, &s_memprops);
    vkGetPhysicalDeviceFeatures(s_pd, &have);
    s_have_bc = have.textureCompressionBC;
    s_have_depth_clamp = have.depthClamp;
    s_have_clip_dist = have.shaderClipDistance;
    {
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(s_pd, VK_FORMAT_D24_UNORM_S8_UINT, &fp);
        if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT))
            s_depth_fmt = VK_FORMAT_D32_SFLOAT_S8_UINT;
        {
            const char *e = getenv("RECOMP_GL_DXT");
            if (e && *e == '0')
                s_have_bc = 0;
        }
    }

    n = 8;
    vkGetPhysicalDeviceQueueFamilyProperties(s_pd, &n, qf);
    for (s_qfam = 0; s_qfam < n; s_qfam++)
        if (qf[s_qfam].queueFlags & VK_QUEUE_GRAPHICS_BIT)
            break;
    if (s_qfam == n) {
        LOGE("no graphics queue\n");
        return 0;
    }

#if defined(__SWITCH__)
    {
        NWindow *win = nwindowGetDefault();
        VkViSurfaceCreateInfoNN sci = { VK_STRUCTURE_TYPE_VI_SURFACE_CREATE_INFO_NN };
        nwindowSetDimensions(win, 1280, 720);
        sci.window = win;
        if (vkCreateViSurfaceNN(s_inst, &sci, NULL, &s_surface) != VK_SUCCESS) {
            LOGE("vkCreateViSurfaceNN failed\n");
            return 0;
        }
    }
#else
    if (!s_headless && !SDL_Vulkan_CreateSurface(s_win, s_inst, &s_surface)) {
        LOGE("SDL_Vulkan_CreateSurface: %s\n", SDL_GetError());
        s_headless = 1;
    }
#endif

    qci.queueFamilyIndex = s_qfam;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    f13.dynamicRendering = VK_TRUE;
    fvi.vertexInputDynamicState = VK_TRUE;
    feds.extendedDynamicState = VK_TRUE;
    f13.pNext = &fvi;
    f2.pNext = &f13;
    f2.features.textureCompressionBC = s_have_bc ? VK_TRUE : VK_FALSE;
    f2.features.depthClamp = s_have_depth_clamp ? VK_TRUE : VK_FALSE;
    f2.features.shaderClipDistance = s_have_clip_dist ? VK_TRUE : VK_FALSE;
    if (!s_headless)
        dext[ndext++] = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    dext[ndext++] = VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME;
    dext[ndext++] = VK_EXT_VERTEX_INPUT_DYNAMIC_STATE_EXTENSION_NAME;
    dci.pNext = &f2;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = ndext;
    dci.ppEnabledExtensionNames = dext;
    (void)feds;
    if (vkCreateDevice(s_pd, &dci, NULL, &s_dev) != VK_SUCCESS) {
        LOGE("vkCreateDevice failed (needs push descriptors, dynamic vertex input, Vulkan 1.3)\n");
        return 0;
    }
    vkGetDeviceQueue(s_dev, s_qfam, 0, &s_queue);
#define GET(p, name) p = (void *)vkGetDeviceProcAddr(s_dev, name); if (!p) { LOGE("missing %s\n", name); return 0; }
    GET(p_push_desc, "vkCmdPushDescriptorSetKHR");
    GET(p_vertex_input, "vkCmdSetVertexInputEXT");
    GET(p_begin_rendering, "vkCmdBeginRendering");
    GET(p_end_rendering, "vkCmdEndRendering");
    GET(p_cull_mode, "vkCmdSetCullMode");
    GET(p_front_face, "vkCmdSetFrontFace");
    GET(p_topology, "vkCmdSetPrimitiveTopology");
    GET(p_depth_test, "vkCmdSetDepthTestEnable");
    GET(p_depth_write, "vkCmdSetDepthWriteEnable");
    GET(p_depth_op, "vkCmdSetDepthCompareOp");
    GET(p_stencil_test, "vkCmdSetStencilTestEnable");
    GET(p_stencil_op, "vkCmdSetStencilOp");
#undef GET

    if (!s_headless && !create_swapchain()) {
        LOGE("swapchain creation failed; running headless\n");
        s_headless = 1;
    }

    {
        VkDescriptorSetLayoutBinding b[7];
        VkDescriptorSetLayoutCreateInfo lci = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        VkPipelineCacheCreateInfo pci = { VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO };
        memset(b, 0, sizeof b);
        for (i = 0; i < 7; i++) {
            b[i].binding = i;
            b[i].descriptorCount = 1;
            b[i].descriptorType = i < 2 || i == 6 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                                                  : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            b[i].stageFlags = i == 0 || i == 6 ? VK_SHADER_STAGE_VERTEX_BIT
                                               : VK_SHADER_STAGE_FRAGMENT_BIT;
        }
        lci.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
        lci.bindingCount = 7;
        lci.pBindings = b;
        if (vkCreateDescriptorSetLayout(s_dev, &lci, NULL, &s_dsl) != VK_SUCCESS)
            return 0;
        pli.setLayoutCount = 1;
        pli.pSetLayouts = &s_dsl;
        if (vkCreatePipelineLayout(s_dev, &pli, NULL, &s_layout) != VK_SUCCESS)
            return 0;
        {
            char path[256];
            size_t n = 0;
            void *d = cache_path(path, sizeof path, "vkpipecache.bin") ? read_file(path, &n) : NULL;
            pci.initialDataSize = n;
            pci.pInitialData = d;
            if (vkCreatePipelineCache(s_dev, &pci, NULL, &s_pcache) != VK_SUCCESS) {
                pci.initialDataSize = 0;       /* another driver's or a damaged file */
                pci.pInitialData = NULL;
                vkCreatePipelineCache(s_dev, &pci, NULL, &s_pcache);
            }
            if (n)
                fprintf(stderr, "  [VK] pipeline cache: %u KB from %s\n", (unsigned)(n >> 10), path);
            free(d);
        }
    }

    for (i = 0; i < VK_FRAMES; i++) {
        Frame *f = &s_fr[i];
        VkCommandPoolCreateInfo cpi = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
        VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        VkSemaphoreCreateInfo sci = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        VkMemoryRequirements rq;
        cpi.queueFamilyIndex = s_qfam;
        vkCreateCommandPool(s_dev, &cpi, NULL, &f->pool);
        cai.commandPool = f->pool;
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = 1;
        vkAllocateCommandBuffers(s_dev, &cai, &f->cb);
        vkCreateFence(s_dev, &fci, NULL, &f->fence);
        vkCreateSemaphore(s_dev, &sci, NULL, &f->acquired);
        vkCreateSemaphore(s_dev, &sci, NULL, &f->rendered);
        bci.size = RING_BYTES;
        bci.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                    VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                    VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        if (vkCreateBuffer(s_dev, &bci, NULL, &f->ring) != VK_SUCCESS)
            return 0;
        vkGetBufferMemoryRequirements(s_dev, f->ring, &rq);
        f->ring_mem = alloc_mem(rq, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (!f->ring_mem || vkBindBufferMemory(s_dev, f->ring, f->ring_mem, 0) != VK_SUCCESS ||
            vkMapMemory(s_dev, f->ring_mem, 0, RING_BYTES, 0, (void **)&f->ring_ptr) != VK_SUCCESS) {
            LOGE("frame ring allocation failed\n");
            return 0;
        }
    }
    s_fi = 0;
    frame_begin();
    if (!make_image(1, 1, VK_FORMAT_B8G8R8A8_UNORM,
                    VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                    VK_IMAGE_ASPECT_COLOR_BIT, &s_dummy_img, &s_dummy_view, &s_dummy_mem))
        return 0;
    clear_image_color(s_dummy_img, 0, 0, 0, 0);
    if (!make_image_n(1, 1, 6, VK_FORMAT_B8G8R8A8_UNORM,
                      VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                      VK_IMAGE_ASPECT_COLOR_BIT, &s_dummy_cube_img, &s_dummy_cube_view,
                      &s_dummy_cube_mem))
        return 0;
    clear_image_color(s_dummy_cube_img, 0, 0, 0, 0);
    s_state = 1;
    fprintf(stderr, "  [VK] ready%s: BC %s, depth %s, depth clamp %s, clip distance %s, UBO align %u\n",
            s_headless ? " (headless)" : "", s_have_bc ? "yes" : "no",
            s_depth_fmt == VK_FORMAT_D24_UNORM_S8_UINT ? "D24S8" : "D32S8",
            s_have_depth_clamp ? "yes" : "no", s_have_clip_dist ? "yes" : "no",
            (unsigned)s_ubo_align);
    prewarm();
    return 1;
}

/* Submit what is recorded and wait for it, then keep recording in the same
 * frame (ring full). */
static void flush_frame_wait(void)
{
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    end_rendering();
    s_rt = NULL;
    vkEndCommandBuffer(s_cb);
    si.commandBufferCount = 1;
    si.pCommandBuffers = &s_cb;
    vkQueueSubmit(s_queue, 1, &si, s_f->fence);
    vkWaitForFences(s_dev, 1, &s_f->fence, VK_TRUE, UINT64_MAX);
    vkResetFences(s_dev, 1, &s_f->fence);
    garbage_free(s_f);
    s_f->ring_off = 0;
    vkResetCommandPool(s_dev, s_f->pool, 0);
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(s_cb, &bi);
    s_cb_gen++;
    s_dyn_valid = 0;
}

/* ── Render state ─────────────────────────────────────────────────── */

static float byte_f(uint32_t v, int shift) { return (float)((v >> shift) & 0xFF) / 255.0f; }
static void argb_vec4(uint32_t v, float out[4])
{
    out[0] = byte_f(v, 16); out[1] = byte_f(v, 8);
    out[2] = byte_f(v, 0);  out[3] = byte_f(v, 24);
}

static VkCompareOp cmp_op(uint32_t v, uint32_t dflt)
{
    if (!v) v = dflt;
    if (v < 0x200 || v > 0x207) v = dflt;
    return (VkCompareOp)(v - 0x200);
}

/* NV097_SET_STENCIL_OP_V_ZERO is 0x0000 and KEEP 0x1E00; the executor starts
 * the registers at KEEP. Reading 0 as KEEP left the stencil marks of Cold
 * Fear's shadow volumes in place -- its darkening pass zeroes them for the
 * next light -- so shadows piled up into black corners, the scenery went dark
 * and the flashlight pass (drawn where the stencil is 0) never showed. */
static VkStencilOp stencil_op(uint32_t v)
{
    switch (v) {
    case 0x0000: {
        static int zero_on = -1;            /* NV2A_STENCIL_ZERO=0: the old KEEP */
        if (zero_on < 0) {
            const char *e = getenv("NV2A_STENCIL_ZERO");
            zero_on = !(e && *e == '0');
        }
        return zero_on ? VK_STENCIL_OP_ZERO : VK_STENCIL_OP_KEEP;
    }
    case 0x1E01: return VK_STENCIL_OP_REPLACE;
    case 0x1E02: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
    case 0x1E03: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
    case 0x150A: return VK_STENCIL_OP_INVERT;
    case 0x8507: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
    case 0x8508: return VK_STENCIL_OP_DECREMENT_AND_WRAP;
    default: return VK_STENCIL_OP_KEEP;         /* KEEP (0x1E00) */
    }
}

/* Dynamic state for a draw, set only when it changed. */
typedef struct {
    uint32_t cull, cullmode, front, topo;
    uint32_t dtest, dwrite, dop;
    uint32_t stest, sfail, szfail, spass, sop, smask, swrite, sref;
    uint32_t bcolor;
    uint32_t bias_on, bias_factor, bias_units, bias16;
} DynState;
static DynState s_dyn;

static uint32_t zmax_of(const uint32_t *r);

static void apply_dynamic(const uint32_t *r, int has_depth, VkPrimitiveTopology topo)
{
    DynState d;
    memset(&d, 0, sizeof d);
    d.cull = r[0x308 / 4] != 0;
    d.cullmode = r[0x39C / 4];
    d.front = r[0x3A0 / 4];
    d.topo = (uint32_t)topo;
    d.dtest = has_depth && r[0x30C / 4];
    d.dwrite = has_depth && r[0x35C / 4];
    d.dop = r[0x354 / 4];
    {
        /* NV2A_STENCIL=0: no stencil test at all (diagnostics: tells a
         * shadow-volume problem from anything else). */
        static int st_on = -1;
        if (st_on < 0) {
            const char *e = getenv("NV2A_STENCIL");
            st_on = !(e && *e == '0');
        }
        d.stest = st_on && has_depth && r[0x32C / 4];
    }
    if (d.stest) {
        d.sfail = r[0x370 / 4]; d.szfail = r[0x374 / 4]; d.spass = r[0x378 / 4];
        d.sop = r[0x364 / 4]; d.smask = r[0x36C / 4] & 0xFF; d.swrite = r[0x360 / 4] & 0xFF;
        d.sref = r[0x368 / 4] & 0xFF;
    }
    d.bcolor = r[0x34C / 4];
    d.bias_on = has_depth && r[0x338 / 4];   /* SET_POLY_OFFSET_FILL_ENABLE */
    if (d.bias_on) {
        d.bias_factor = r[0x384 / 4];
        d.bias_units = r[0x388 / 4];
        d.bias16 = zmax_of(r) == 0xFFFFu;
    }
    {
        /* Lighting diagnostics: how many draws test depth EQUAL (the
         * multi-pass light passes), use stencil, or blend additively
         * (dst factor ONE) -- every 10 s. */
        static uint32_t n_all, n_eq, n_le, n_st, n_add;
        static uint64_t t_last;
        uint64_t now = (uint64_t)time(NULL);
        n_all++;
        if (d.dtest && d.dop == 0x202) n_eq++;
        if (d.dtest && d.dop == 0x203) n_le++;
        if (d.stest) n_st++;
        if (r[0x304 / 4] && r[0x348 / 4] == 1) n_add++;
        if (d.stest) {
            /* Each stencil state the title uses (first 24): shadow volumes
             * and the pass that darkens what they mark. */
            static uint32_t seen[24][8];
            static int nseen;
            uint32_t v[8] = { d.sop, d.sref, d.smask, d.swrite, d.sfail, d.szfail, d.spass,
                              (r[0x358 / 4] ? 1u : 0u) | (r[0x304 / 4] ? 2u : 0u)
                              | (d.dwrite ? 4u : 0u) };
            int k;
            for (k = 0; k < nseen && memcmp(seen[k], v, sizeof v); k++) ;
            if (k == nseen && nseen < 24) {
                memcpy(seen[nseen++], v, sizeof v);
                fprintf(stderr, "[STENCIL] func %04X ref %02X mask %02X write %02X ops %04X/%04X/%04X"
                        " colour %s blend %s (%04X %04X %04X) zwrite %u zfunc %04X\n",
                        v[0], v[1], v[2], v[3], v[4], v[5], v[6], (v[7] & 1) ? "on" : "off",
                        (v[7] & 2) ? "on" : "off", r[0x344 / 4], r[0x348 / 4], r[0x350 / 4],
                        (v[7] >> 2) & 1, d.dop);
            }
        }
        if (!t_last)
            t_last = now;
        if (now - t_last >= 10) {
            fprintf(stderr, "[VKSTAT] 10 s: %u draws, depth EQUAL %u, LEQUAL %u, stencil %u, additive %u,"
                    " shadow passes %u (%u without a clear), clears depth %u stencil %u\n",
                    n_all, n_eq, n_le, n_st, n_add, s_zonly_passes, s_zonly_noclear, s_clr_z, s_clr_s);
            n_all = n_eq = n_le = n_st = n_add = 0;
            fprintf(stderr, "[VKSTAT]   by transform: pre %u ffp %u program %u, ffp lit %u\n",
                    s_xf_n[0], s_xf_n[1], s_xf_n[2], s_ffp_lit);
            s_xf_n[0] = s_xf_n[1] = s_xf_n[2] = s_ffp_lit = 0;
            s_clr_z = s_clr_s = 0;
            s_zonly_passes = s_zonly_noclear = 0;
            t_last = now;
        }
    }
    if (s_dyn_valid && !memcmp(&d, &s_dyn, sizeof d))
        return;
    if (!s_dyn_valid || d.cull != s_dyn.cull || d.cullmode != s_dyn.cullmode) {
        VkCullModeFlags cm = VK_CULL_MODE_NONE;
        if (d.cull)
            cm = d.cullmode == 0x404 ? VK_CULL_MODE_FRONT_BIT
               : d.cullmode == 0x408 ? VK_CULL_MODE_FRONT_AND_BACK : VK_CULL_MODE_BACK_BIT;
        p_cull_mode(s_cb, cm);
    }
    if (!s_dyn_valid || d.front != s_dyn.front)
        /* NV2A winding is as seen on screen, rows top-first -- Vulkan's
         * framebuffer orientation, so CW is CW. nv2a_gl swaps because its
         * window y runs the other way. Swapped here too, every culled draw
         * (29 a frame in the main menu: headlight glass, grille) lost its
         * front faces and kept its back ones. */
        p_front_face(s_cb, d.front == 0x901 ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE);
    if (!s_dyn_valid || d.topo != s_dyn.topo)
        p_topology(s_cb, topo);
    if (!s_dyn_valid || d.dtest != s_dyn.dtest) p_depth_test(s_cb, d.dtest);
    if (!s_dyn_valid || d.dwrite != s_dyn.dwrite) p_depth_write(s_cb, d.dwrite);
    if (!s_dyn_valid || d.dop != s_dyn.dop) p_depth_op(s_cb, cmp_op(d.dop, 0x203));
    if (!s_dyn_valid || d.stest != s_dyn.stest) p_stencil_test(s_cb, d.stest);
    if (!s_dyn_valid || memcmp(&d.sfail, &s_dyn.sfail, 7 * sizeof(uint32_t))) {
        p_stencil_op(s_cb, VK_STENCIL_FACE_FRONT_AND_BACK, stencil_op(d.sfail), stencil_op(d.spass),
                     stencil_op(d.szfail), cmp_op(d.sop, 0x207));
        vkCmdSetStencilCompareMask(s_cb, VK_STENCIL_FACE_FRONT_AND_BACK, d.smask);
        vkCmdSetStencilWriteMask(s_cb, VK_STENCIL_FACE_FRONT_AND_BACK, d.swrite);
        vkCmdSetStencilReference(s_cb, VK_STENCIL_FACE_FRONT_AND_BACK, d.sref);
    }
    if (!s_dyn_valid || d.bias_on != s_dyn.bias_on || d.bias_factor != s_dyn.bias_factor
        || d.bias_units != s_dyn.bias_units || d.bias16 != s_dyn.bias16) {
        /* Polygon offset (SET_POLY_OFFSET_FILL_ENABLE, SET_POLYGON_OFFSET_
         * SCALE_FACTOR / _BIAS), as xemu's glPolygonOffset(factor, units).
         * Shadow-buffer passes rely on it: without it lit surfaces fail
         * their own depth test against the buffer, and Cold Fear's decks
         * came out shadowed over and streaked. The units are steps of the
         * title's depth format; the buffer here is 24-bit, so a 16-bit
         * title's step is 256 of ours. */
        float f = 0.0f, u = 0.0f;
        if (d.bias_on) {
            memcpy(&f, &d.bias_factor, 4);
            memcpy(&u, &d.bias_units, 4);
            if (d.bias16)
                u *= 256.0f;
        }
        vkCmdSetDepthBias(s_cb, u, 0.0f, f);
    }
    if (!s_dyn_valid || d.bcolor != s_dyn.bcolor) {
        float bc[4];
        argb_vec4(d.bcolor, bc);
        vkCmdSetBlendConstants(s_cb, bc);
    }
    s_dyn = d;
    s_dyn_valid = 1;
}

static void psh_key(const uint32_t *r, Nv2aPshKey *k)
{
    int i;
    memset(k, 0, sizeof *k);
    for (i = 0; i < 8; i++) {
        k->color_icw[i] = r[0xAC0 / 4 + i];
        k->color_ocw[i] = r[0x1E40 / 4 + i];
        k->alpha_icw[i] = r[0x260 / 4 + i];
        k->alpha_ocw[i] = r[0xAA0 / 4 + i];
    }
    k->control = r[0x1E60 / 4];
    k->final0 = r[0x288 / 4];
    k->final1 = r[0x28C / 4];
    k->shader_program = r[0x1E70 / 4];
    k->other_input = r[0x1E78 / 4];
    for (i = 0; i < 4; i++)
        k->tex_kind |= stage_kind(r, i) << (4 * i);
    if (k->tex_kind & 0x2222u)                  /* a shadow buffer stage */
        k->tex_kind |= (r[0x1E6C / 4] & 7u) << 16;
}

static uint32_t zmax_of(const uint32_t *r)
{
    return (((r[0x208 / 4] >> 4) & 0xF) == 1) ? 0xFFFFu : 0xFFFFFFu;
}

/* NV097 primitive -> Vulkan topology and (possibly rewritten) indices. */
static uint32_t *s_prim_idx;
static size_t    s_prim_cap;

static int prim_indices(const Nv2aRawBatch *b, const uint32_t **idx, uint32_t *n,
                        VkPrimitiveTopology *topo, uint32_t *cls)
{
    uint32_t i, m = 0;
    *idx = b->indices;
    *n = b->index_count;
    switch (b->prim) {
    case 1: *topo = VK_PRIMITIVE_TOPOLOGY_POINT_LIST; *cls = 0; return 1;
    case 2: *topo = VK_PRIMITIVE_TOPOLOGY_LINE_LIST; *cls = 1; return 1;
    case 4: *topo = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP; *cls = 1; return 1;
    case 5: *topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; *cls = 2; return 1;
    case 6: case 9: *topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP; *cls = 2; return 1;
    case 7: case 10: *topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN; *cls = 2; return 1;
    case 3:                                      /* LINE_LOOP: strip + first */
    case 8: {                                    /* QUADS -> triangles */
        size_t need = b->prim == 3 ? (size_t)b->index_count + 1 : (size_t)b->index_count / 4 * 6;
        if (need > s_prim_cap) {
            free(s_prim_idx);
            s_prim_cap = need + 1024;
            s_prim_idx = (uint32_t *)malloc(s_prim_cap * 4);
        }
        if (!s_prim_idx) { *n = 0; return 0; }
        if (b->prim == 3) {
            if (!b->index_count) { *n = 0; return 0; }
            memcpy(s_prim_idx, b->indices, (size_t)b->index_count * 4);
            s_prim_idx[b->index_count] = b->indices[0];
            m = b->index_count + 1;
            *topo = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP; *cls = 1;
        } else {
            for (i = 0; i + 3 < b->index_count; i += 4) {
                const uint32_t *q = b->indices + i;
                s_prim_idx[m++] = q[0]; s_prim_idx[m++] = q[1]; s_prim_idx[m++] = q[2];
                s_prim_idx[m++] = q[0]; s_prim_idx[m++] = q[2]; s_prim_idx[m++] = q[3];
            }
            *topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; *cls = 2;
        }
        *idx = s_prim_idx;
        *n = m;
        return 1;
    }
    default:
        *n = 0;
        return 0;
    }
}

/* ── Vertices ─────────────────────────────────────────────────────── */

typedef struct { const uint8_t *lo, *hi; VkDeviceSize at; } VRun;

static uint32_t elem_bytes(uint32_t type, uint32_t size)
{
    switch (type) {
    case 0: return 4;
    case 2: return 4 * size;
    case 4: return size;
    default: return 2 * size;
    }
}

static uint32_t vertex_runs(const Nv2aRawBatch *b, uint16_t direct, VRun *run, int *of_run)
{
    uint32_t nv = b->vertex_count, nrun = 0, a, i;
    for (a = 0; a < NV2A_RAW_ATTRS; a++) {
        const uint8_t *lo, *hi;
        if (!(direct & (1u << a)))
            continue;
        lo = (const uint8_t *)((uintptr_t)b->direct[a].ptr & ~(uintptr_t)3);
        hi = b->direct[a].ptr + (size_t)(nv - 1) * b->direct[a].stride
           + elem_bytes(b->direct[a].type, b->direct[a].size);
        for (i = 0; i < nrun; i++)
            if (lo <= run[i].hi && run[i].lo <= hi)
                break;
        if (i == nrun) {
            run[nrun].lo = lo;
            run[nrun].hi = hi;
            nrun++;
        } else {
            if (lo < run[i].lo) run[i].lo = lo;
            if (hi > run[i].hi) run[i].hi = hi;
        }
        of_run[a] = (int)i;
    }
    for (i = 0; i + 1 < nrun; ) {
        uint32_t j;
        for (j = i + 1; j < nrun; j++)
            if (run[j].lo <= run[i].hi && run[i].lo <= run[j].hi)
                break;
        if (j == nrun) { i++; continue; }
        if (run[j].lo < run[i].lo) run[i].lo = run[j].lo;
        if (run[j].hi > run[i].hi) run[i].hi = run[j].hi;
        for (a = 0; a < NV2A_RAW_ATTRS; a++) {
            if (!(direct & (1u << a)))
                continue;
            if (of_run[a] == (int)j) of_run[a] = (int)i;
            else if (of_run[a] == (int)(nrun - 1)) of_run[a] = (int)j;
        }
        run[j] = run[nrun - 1];
        nrun--;
    }
    return nrun;
}

/* The Vulkan vertex format of a direct attribute, or UNDEFINED when the
 * device cannot fetch it (then it is converted to float4 here). */
static VkFormat s_vfmt_cache[8][5];
static int      s_vfmt_known[8][5];

static VkFormat vertex_format(uint32_t type, uint32_t size)
{
    static const VkFormat f_float[5] = { 0, VK_FORMAT_R32_SFLOAT, VK_FORMAT_R32G32_SFLOAT,
                                         VK_FORMAT_R32G32B32_SFLOAT, VK_FORMAT_R32G32B32A32_SFLOAT };
    static const VkFormat f_snorm16[5] = { 0, VK_FORMAT_R16_SNORM, VK_FORMAT_R16G16_SNORM,
                                           VK_FORMAT_R16G16B16_SNORM, VK_FORMAT_R16G16B16A16_SNORM };
    static const VkFormat f_unorm8[5] = { 0, VK_FORMAT_R8_UNORM, VK_FORMAT_R8G8_UNORM,
                                          VK_FORMAT_R8G8B8_UNORM, VK_FORMAT_R8G8B8A8_UNORM };
    static const VkFormat f_sscaled16[5] = { 0, VK_FORMAT_R16_SSCALED, VK_FORMAT_R16G16_SSCALED,
                                             VK_FORMAT_R16G16B16_SSCALED, VK_FORMAT_R16G16B16A16_SSCALED };
    VkFormat f;
    if (size < 1 || size > 4 || type > 7)
        return VK_FORMAT_UNDEFINED;
    if (s_vfmt_known[type][size])
        return s_vfmt_cache[type][size];
    switch (type) {
    case 0: f = VK_FORMAT_B8G8R8A8_UNORM; break;
    case 1: f = f_snorm16[size]; break;
    case 2: f = f_float[size]; break;
    case 4: f = f_unorm8[size]; break;
    case 5: f = f_sscaled16[size]; break;
    default: f = VK_FORMAT_UNDEFINED; break;
    }
    if (f != VK_FORMAT_UNDEFINED) {
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(s_pd, f, &fp);
        if (!(fp.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)) {
            if (s_trace)
                LOGE("vertex format %d (type %u size %u) not supported: converted\n", f, type, size);
            f = VK_FORMAT_UNDEFINED;
        }
    }
    s_vfmt_known[type][size] = 1;
    s_vfmt_cache[type][size] = f;
    return f;
}

/* One attribute of vertex v, as the NV2A reads it, into float4. */
static void attr_to_float4(const Nv2aRawBatch *b, uint32_t a, uint32_t v, float out[4])
{
    const uint8_t *p = b->direct[a].ptr + (size_t)v * b->direct[a].stride;
    uint32_t size = b->direct[a].size, c;
    out[0] = out[1] = out[2] = 0.0f;
    out[3] = 1.0f;
    switch (b->direct[a].type) {
    case 0:                                          /* D3DCOLOR: B,G,R,A */
        out[0] = p[2] / 255.0f; out[1] = p[1] / 255.0f; out[2] = p[0] / 255.0f; out[3] = p[3] / 255.0f;
        break;
    case 1:
        for (c = 0; c < size && c < 4; c++) {
            int16_t s; memcpy(&s, p + 2 * c, 2);
            out[c] = s / 32767.0f < -1.0f ? -1.0f : s / 32767.0f;
        }
        break;
    case 2:
        for (c = 0; c < size && c < 4; c++) memcpy(&out[c], p + 4 * c, 4);
        break;
    case 4:
        for (c = 0; c < size && c < 4; c++) out[c] = p[c] / 255.0f;
        break;
    case 6: {
        /* CMP (D3DVSDT_NORMPACKED3): one dword, signed 11:11:10 bits for
         * x, y, z. Read as 16-bit integers, every packed normal pointed
         * somewhere random, and fixed-function lighting turned the meshes
         * into dark blotches. */
        uint32_t d;
        int32_t x, y, z;
        memcpy(&d, p, 4);
        x = (int32_t)(d << 21) >> 21;
        y = (int32_t)(d << 10) >> 21;
        z = (int32_t)d >> 22;
        out[0] = x / 1023.0f; out[1] = y / 1023.0f; out[2] = z / 511.0f;
        {
            static int logged;
            if (!logged++)
                fprintf(stderr, "[VFMT] packed normals (CMP) on attribute %u\n", a);
        }
        break;
    }
    default:
        for (c = 0; c < size && c < 4; c++) { int16_t s; memcpy(&s, p + 2 * c, 2); out[c] = (float)s; }
        break;
    }
}

static int upload_vertices(const Nv2aRawBatch *b)
{
    VRun run[NV2A_RAW_ATTRS];
    int of_run[NV2A_RAW_ATTRS];
    uint32_t slot[NV2A_RAW_ATTRS], np = 0, nrun, a, i, v;
    uint32_t nv = b->vertex_count;
    uint16_t direct = 0, conv, cdirect = 0;
    VkDeviceSize bytes = 0, pack_at, const_at, at;
    VkFormat fmt[NV2A_RAW_ATTRS];
    uint8_t *dst;
    VkVertexInputBindingDescription2EXT bind[NV2A_RAW_ATTRS];
    VkVertexInputAttributeDescription2EXT attr[NV2A_RAW_ATTRS];
    VkBuffer bufs[NV2A_RAW_ATTRS];
    VkDeviceSize offs[NV2A_RAW_ATTRS];
    void *map;

    if (!nv)
        return 0;
    for (a = 0; a < NV2A_RAW_ATTRS; a++) {
        fmt[a] = VK_FORMAT_UNDEFINED;
        if (!(b->attr_direct & (1u << a)))
            continue;
        fmt[a] = vertex_format(b->direct[a].type, b->direct[a].size);
        if (fmt[a] != VK_FORMAT_UNDEFINED)
            direct |= (uint16_t)(1u << a);
        else
            cdirect |= (uint16_t)(1u << a);          /* direct data, converted here */
    }
    conv = b->attr_present & (uint16_t)~direct;
    nrun = vertex_runs(b, direct, run, of_run);
    for (i = 0; i < nrun; i++) {
        run[i].at = bytes;
        bytes += ((VkDeviceSize)(run[i].hi - run[i].lo) + 15) & ~(VkDeviceSize)15;
    }
    for (a = 0; a < NV2A_RAW_ATTRS; a++)
        if (conv & (1u << a))
            slot[np++] = a;
    pack_at = bytes;
    bytes += (VkDeviceSize)nv * np * 16;
    const_at = bytes;
    bytes += NV2A_RAW_ATTRS * 16;
    at = ring_alloc(bytes, 16, &map);
    dst = (uint8_t *)map;
    if (!dst)
        return 0;
    for (i = 0; i < nrun; i++)
        memcpy(dst + run[i].at, run[i].lo, (size_t)(run[i].hi - run[i].lo));
    if (np) {
        float *out = (float *)(dst + pack_at);
        for (v = 0; v < nv; v++)
            for (i = 0; i < np; i++) {
                float *o = out + ((size_t)v * np + i) * 4;
                if (cdirect & (1u << slot[i]))
                    attr_to_float4(b, slot[i], v, o);
                else
                    memcpy(o, b->attrs + ((size_t)v * NV2A_RAW_ATTRS + slot[i]) * 4, 16);
            }
    }
    memcpy(dst + const_at, b->attr_const, NV2A_RAW_ATTRS * 16);

    for (a = 0; a < NV2A_RAW_ATTRS; a++) {
        memset(&bind[a], 0, sizeof bind[a]);
        memset(&attr[a], 0, sizeof attr[a]);
        bind[a].sType = VK_STRUCTURE_TYPE_VERTEX_INPUT_BINDING_DESCRIPTION_2_EXT;
        bind[a].binding = a;
        bind[a].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        bind[a].divisor = 1;
        attr[a].sType = VK_STRUCTURE_TYPE_VERTEX_INPUT_ATTRIBUTE_DESCRIPTION_2_EXT;
        attr[a].location = a;
        attr[a].binding = a;
        bufs[a] = s_f->ring;
        if (direct & (1u << a)) {
            bind[a].stride = b->direct[a].stride;
            attr[a].format = fmt[a];
            offs[a] = at + run[of_run[a]].at + (VkDeviceSize)(b->direct[a].ptr - run[of_run[a]].lo);
        } else if (conv & (1u << a)) {
            for (i = 0; i < np && slot[i] != a; i++) { }
            bind[a].stride = np * 16;
            attr[a].format = VK_FORMAT_R32G32B32A32_SFLOAT;
            offs[a] = at + pack_at + i * 16;
        } else {
            /* Absent: the title's SET_VERTEX_DATA* value, stride 0. */
            bind[a].stride = 0;
            attr[a].format = VK_FORMAT_R32G32B32A32_SFLOAT;
            offs[a] = at + const_at + a * 16;
        }
    }
    p_vertex_input(s_cb, NV2A_RAW_ATTRS, bind, NV2A_RAW_ATTRS, attr);
    /* Eight at a time: Mesa's CmdBindVertexBuffers2 keeps up to 8 bindings
     * on the stack and mallocs (then frees) a larger list -- with 16 per
     * draw, malloc was 7% of the pushbuffer thread. */
    for (a = 0; a < NV2A_RAW_ATTRS; a += 8)
        vkCmdBindVertexBuffers(s_cb, a, NV2A_RAW_ATTRS - a < 8 ? NV2A_RAW_ATTRS - a : 8,
                               bufs + a, offs + a);
    return 1;
}

/* ── Back end entry points ────────────────────────────────────────── */

/* std140 blocks (gl_vsh.c / gl_psh.c, Vulkan dialect). */
typedef struct {
    float surf[4];
    float m[16];
    float vpoff[4];
    float aa[2];
    int32_t xform;
    int32_t zcull;               /* near/far culling, see nv2a_clip */
    float fog[4];                /* FOG_PARAMS 0, 1; mode; enable (nv2a_fog) */
    /* Fixed-function lighting (gl_vsh.c nv2a_light): on, light enable mask,
     * colour material, -; model-view rows; scene ambient + material alpha;
     * material emission; per light ambient + range, diffuse, local
     * position, attenuation, infinite direction, spot direction. */
    int32_t lctl[4];
    float mv[16];
    float lscene[4];
    float lemis[4];
    float light[8 * 6][4];
    /* Texture coordinate generation and texture matrices (nv2a_texgen):
     * any in use, -, -, -; matrix enable per stage; mode per stage and
     * coordinate (0 off, 1 eye linear, 2 object linear, 3 sphere map,
     * 4 normal map, 5 reflection map); matrix rows; planes S/T/R/Q. */
    int32_t tctl[4];
    int32_t tmen[4];
    int32_t tgen[4][4];
    float tmat[16][4];
    float tplane[16][4];
} VsBlock;

typedef struct {
    float tscale[4][4];          /* vec2 array: 16-byte elements */
    float c0[8][4], c1[8][4];
    float fc0[4], fc1[4];
    float fog[4];
    int32_t afunc;
    float aref;
    float out_scale;             /* gl_psh u_out_scale */
    float pad;
} FsBlock;

/* ── Instancing ───────────────────────────────────────────────────────
 *
 * A quarter of a race frame's draws repeat the previous draw with only the
 * transform constants changed (NFSU2's drag start line: ~480 of ~1870) --
 * the same mesh at another matrix. Such a run becomes one instanced draw:
 * the first draw is recorded up to its vkCmdDrawIndexed, which is held
 * back (s_pend); every repeat only appends its 192 constants right behind
 * the previous set in the ring (binding 6, which the vertex shader indexes
 * by gl_InstanceIndex * 192); anything else -- another draw, a clear, a
 * flip -- first issues the held draw with the instance count. Instances
 * are drawn in order, so blending and depth come out as separate draws.
 *
 * "Repeat" means: no register changed since the held draw except the
 * constant and program upload windows and the per-draw methods (executor
 * dirty blocks, compared to a copy), the same program, vertex pointers,
 * formats and count, the same indices, absent-attribute values and
 * texture addresses, and no semaphore release or trap in between
 * (vtx_epoch: vertex bytes the title could have rewritten). Only vertex
 * programs (xform 2) whose attributes are all uploaded as stored.
 * RECOMP_VK_INSTANCE=0 turns it off. */
#define VK_INST_MAX   16                    /* gl_vsh.c: c[192 * 16] */
#define VK_CONST_SIZE (192 * 16)

extern uint8_t *nv2a_pb_reg_dirty(void);
static uint32_t s_regs_seen[0x2000 / 4];    /* registers as of the last draw */

/* Did a register a draw depends on change since the last draw? */
static int regs_changed(const uint32_t *r)
{
    uint8_t *dirty = nv2a_pb_reg_dirty();
    uint32_t blk, w;
    int changed = 0;

    for (blk = 0; blk < 0x2000 / 64; blk++) {
        if (!dirty[blk])
            continue;
        dirty[blk] = 0;
        if (blk >= 0xB00 / 64 && blk < 0xC00 / 64)
            continue;                           /* program / constant uploads */
        for (w = blk * 16; w < blk * 16 + 16; w++) {
            uint32_t m = w * 4;
            if (r[w] == s_regs_seen[w])
                continue;
            s_regs_seen[w] = r[w];
            if (m == 0x0100 || m == 0x17FC || (m >= 0x1800 && m < 0x1820)
             || m == 0x1D6C || m == 0x1D70 || m == 0x1E9C || m == 0x1EA4)
                continue;                       /* NOP, BEGIN_END, draw data, semaphore, load slots */
            changed = 1;
        }
    }
    return changed;
}

static struct {
    int          on;                        /* a draw is held */
    uint32_t     n, count, max;             /* index count, instances so far, room */
    VkDeviceSize cat;                       /* first instance's constants */
    uint32_t     cb_gen;
    uint32_t     prim, vertex_count, index_count;
    uint16_t     attr_present, attr_direct;
    uint32_t     vp_prog_gen, attr_const_gen, vtx_epoch, color_va, zeta_va;
    uint32_t     tex_va[4], pal_va[4];
    float        aa_sx, aa_sy;
    Nv2aSurface  surface;
    struct { const uint8_t *ptr; uint32_t type, size, stride; } direct[NV2A_RAW_ATTRS];
    uint32_t    *idx;
    size_t       idx_cap;
} s_pend;
static int s_inst_on = -1, s_inst_alt;
static uint32_t s_st_draws, s_st_calls, s_st_inst;

static void pend_flush(void)
{
    if (!s_pend.on)
        return;
    s_pend.on = 0;
    if (s_pend.cb_gen != s_cb_gen)
        return;                                 /* cannot happen: flushes go first */
    vkCmdDrawIndexed(s_cb, s_pend.n, s_pend.count, 0, 0, 0);
    s_st_calls++;
}

static int pend_matches(const Nv2aRawBatch *b)
{
    uint32_t a;
    if (b->xform != 2 || b->prim != s_pend.prim || b->vertex_count != s_pend.vertex_count
     || b->index_count != s_pend.index_count || b->attr_present != s_pend.attr_present
     || b->attr_direct != s_pend.attr_direct || b->vp_prog_gen != s_pend.vp_prog_gen
     || b->attr_const_gen != s_pend.attr_const_gen || b->vtx_epoch != s_pend.vtx_epoch
     || b->color_va != s_pend.color_va || b->zeta_va != s_pend.zeta_va
     || b->aa_sx != s_pend.aa_sx || b->aa_sy != s_pend.aa_sy
     || memcmp(b->tex_va, s_pend.tex_va, sizeof s_pend.tex_va)
     || memcmp(b->pal_va, s_pend.pal_va, sizeof s_pend.pal_va)
     || memcmp(&b->surface, &s_pend.surface, sizeof s_pend.surface))
        return 0;
    for (a = 0; a < NV2A_RAW_ATTRS; a++)
        if ((b->attr_direct & (1u << a))
         && (b->direct[a].ptr != s_pend.direct[a].ptr || b->direct[a].type != s_pend.direct[a].type
          || b->direct[a].size != s_pend.direct[a].size || b->direct[a].stride != s_pend.direct[a].stride))
            return 0;
    return !memcmp(b->indices, s_pend.idx, (size_t)b->index_count * 4);
}

/* Can b be held as the first instance of a run? */
static int pend_eligible(const Nv2aRawBatch *b)
{
    uint32_t a;
    if (!s_inst_on || b->xform != 2 || (b->attr_present & ~b->attr_direct))
        return 0;
    for (a = 0; a < NV2A_RAW_ATTRS; a++)
        if ((b->attr_direct & (1u << a))
         && vertex_format(b->direct[a].type, b->direct[a].size) == VK_FORMAT_UNDEFINED)
            return 0;
    return 1;
}

static void pend_hold(const Nv2aRawBatch *b, uint32_t n, VkDeviceSize cat, uint32_t max)
{
    uint32_t a;
    s_pend.on = 1;
    s_pend.n = n;
    s_pend.count = 1;
    s_pend.max = max;
    s_pend.cat = cat;
    s_pend.cb_gen = s_cb_gen;
    s_pend.prim = b->prim;
    s_pend.vertex_count = b->vertex_count;
    s_pend.index_count = b->index_count;
    s_pend.attr_present = b->attr_present;
    s_pend.attr_direct = b->attr_direct;
    s_pend.vp_prog_gen = b->vp_prog_gen;
    s_pend.attr_const_gen = b->attr_const_gen;
    s_pend.vtx_epoch = b->vtx_epoch;
    s_pend.color_va = b->color_va;
    s_pend.zeta_va = b->zeta_va;
    s_pend.aa_sx = b->aa_sx;
    s_pend.aa_sy = b->aa_sy;
    s_pend.surface = b->surface;
    memcpy(s_pend.tex_va, b->tex_va, sizeof s_pend.tex_va);
    memcpy(s_pend.pal_va, b->pal_va, sizeof s_pend.pal_va);
    for (a = 0; a < NV2A_RAW_ATTRS; a++) {
        s_pend.direct[a].ptr = b->direct[a].ptr;
        s_pend.direct[a].type = b->direct[a].type;
        s_pend.direct[a].size = b->direct[a].size;
        s_pend.direct[a].stride = b->direct[a].stride;
    }
    if (b->index_count > s_pend.idx_cap) {
        free(s_pend.idx);
        s_pend.idx_cap = b->index_count + 1024;
        s_pend.idx = (uint32_t *)malloc(s_pend.idx_cap * 4);
        if (!s_pend.idx) {
            s_pend.idx_cap = 0;
            s_pend.on = 0;
            vkCmdDrawIndexed(s_cb, n, 1, 0, 0, 0);
            s_st_calls++;
            return;
        }
    }
    memcpy(s_pend.idx, b->indices, (size_t)b->index_count * 4);
}

/* One more instance of the held draw, when b is one; 0: draw it normally. */
static int pend_append(const Nv2aRawBatch *b, int changed)
{
    VkDeviceSize at;
    void *map;

    if (!s_pend.on)
        return 0;
    if (changed || s_pend.count >= s_pend.max || s_pend.cb_gen != s_cb_gen || !pend_matches(b))
        return 0;
    at = (s_f->ring_off + s_ubo_align - 1) & ~(s_ubo_align - 1);
    if (at != s_pend.cat + (VkDeviceSize)s_pend.count * VK_CONST_SIZE
     || at + VK_CONST_SIZE > RING_BYTES)
        return 0;
    at = ring_alloc(VK_CONST_SIZE, s_ubo_align, &map);
    if (!map)
        return 0;
    memcpy(map, b->vp_consts, VK_CONST_SIZE);
    s_pend.count++;
    s_st_inst++;
    return 1;
}

static void vk_draw_raw(const Nv2aRawBatch *b)
{
    VkSurf *s, *sampled[4];
    VkDepthBuf *d;
    VkProg *p;
    Nv2aPshKey pk;
    const uint32_t *r = b->regs;
    const uint32_t *idx;
    uint32_t n, i, cls;
    VkPrimitiveTopology topo;
    VkDescriptorImageInfo img[4];
    float tscale[4][2];
    PipeKey key;
    VkPipeline pipe;
    VkDescriptorBufferInfo ub[3];
    VkWriteDescriptorSet wr[7];
    uint32_t inst_room = 0;
    void *map;
    static VkPipeline cur_pipe;
    static uint32_t cur_pipe_gen;

    if (!ready())
        return;
    s_regs = r;
    s_batch = b;
    if (s_inst_on < 0) {
        const char *e = getenv("RECOMP_VK_INSTANCE");
        s_inst_on = (e && *e == '1') && s_ubo_range >= VK_CONST_SIZE
                 && VK_CONST_SIZE % s_ubo_align == 0;
        if (s_inst_on && e && *e == '2')
            s_inst_alt = 1;                     /* debug: every other frame */
        fprintf(stderr, "  [VK] instancing %s (uniform range %u, alignment %u)\n",
                s_inst_on ? "on" : "off", s_ubo_range, (unsigned)s_ubo_align);
    }
    s_st_draws++;
    s_frame_draws++;
    s_xf_n[b->xform < 3 ? b->xform : 0]++;
    if (b->xform == 1 && r[0x314 / 4]) {
        static int logged;
        s_ffp_lit++;
        if (logged++ < 4)
            fprintf(stderr, "[FFPLIGHT] fixed-function draw with lighting on: light mask %08X\n", r[0x3BC / 4]);
    }
    {
        int changed = regs_changed(r);
        if (pend_append(b, changed))
            return;
    }
    pend_flush();
    if (!prim_indices(b, &idx, &n, &topo, &cls) || !n)
        return;
    VT("draw: prim %u idx %u verts %u attrs %04X direct %04X xform %u surf %08X zeta %08X\n", b->prim, n,
       b->vertex_count, b->attr_present, b->attr_direct, b->xform, b->surface.color_va, b->zeta_va);
    s = target(&b->surface, b->zeta_va, &d);
    if (!s)
        return;
    s_draw_depth = d;                    /* bind_stage: not readable as a texture */
    s_drew_any = 1;
    psh_key(r, &pk);
    {
        uint32_t made = s_prog_made;
        uint64_t t0 = hz_now();
        p = prog_get(b, &pk);
        if (s_prog_made != made) {
            s_hz_progs += s_prog_made - made;
            s_hz_prog_ns += hz_now() - t0;
        }
    }
    if (!p)
        return;
    s_hz_draws++;

    /* Textures first: uploads end the rendering pass. */
    for (i = 0; i < 4; i++)
        bind_stage(r, (int)i, tscale[i], &img[i], &sampled[i]);
    for (i = 0; i < 4; i++)
        if (sampled[i] && sampled[i]->dirty && sampled[i] != s) {
            end_rendering();                 /* next begin_rendering barriers */
            break;
        }

    memset(&key, 0, sizeof key);
    key.prog = (int)(p - s_prog);
    if (r[0x304 / 4]) {
        key.blend = 1; key.bsrc = r[0x344 / 4]; key.bdst = r[0x348 / 4]; key.beq = r[0x350 / 4];
        {
            /* Diagnostics: each blend state the title uses, first 32;
             * the NV2A's signed equations (0xF005/0xF006) are not emulated. */
            static uint32_t seen[32][3];
            static int nseen;
            int k;
            for (k = 0; k < nseen && (seen[k][0] != key.bsrc || seen[k][1] != key.bdst
                                      || seen[k][2] != key.beq); k++) ;
            if (k == nseen && nseen < 32) {
                seen[nseen][0] = key.bsrc; seen[nseen][1] = key.bdst; seen[nseen][2] = key.beq;
                nseen++;
                fprintf(stderr, "[BLEND] src %04X dst %04X eq %04X%s\n", key.bsrc, key.bdst, key.beq,
                        key.beq == 0xF005 || key.beq == 0xF006 ? "  <-- SIGNED, NOT EMULATED" : "");
            }
        }
    }
    {
        /* Lighting diagnostics: leave out a class of draws to see which one
         * darkens the scene. NV2A_SKIP_REVSUB=1: reverse-subtract blends;
         * NV2A_SKIP_STENCIL_COLOR=1: stencil-tested draws that write colour
         * (the pass that darkens what shadow volumes mark);
         * NV2A_SKIP_MULT=1: multiplicative blends (dst factor SRC_COLOR or
         * src factor DST_COLOR). */
        static int srs = -1, sst, smul;
        if (srs < 0) {
            const char *e = getenv("NV2A_SKIP_REVSUB");
            srs = e && *e == '1';
            e = getenv("NV2A_SKIP_STENCIL_COLOR");
            sst = e && *e == '1';
            e = getenv("NV2A_SKIP_MULT");
            smul = e && *e == '1';
        }
        if (key.blend && key.beq == 0x800B) {
            /* [REVSUB] each distinct reverse-subtract setup, first 6: the
             * pass behind Cold Fear's dark squares. */
            static uint32_t seen[6][3];
            static int nseen;
            uint32_t sig[3] = { pk.shader_program, pk.final0 ^ pk.color_icw[0] ^ pk.color_ocw[1],
                                (uint32_t)b->xform | pk.control << 8 };
            int k, t;
            for (k = 0; k < nseen && memcmp(seen[k], sig, sizeof sig); k++) ;
            if (k == nseen && nseen < 6) {
                memcpy(seen[nseen++], sig, sizeof sig);
                fprintf(stderr, "[REVSUB] xform %u prog %08X control %08X tex_kind %08X final %08X %08X"
                        " fog %u alphatest %u/%02X zfunc %04X zwrite %u\n",
                        b->xform, pk.shader_program, pk.control, pk.tex_kind, pk.final0, pk.final1,
                        r[0x2A4 / 4], r[0x300 / 4], r[0x340 / 4], r[0x354 / 4], r[0x35C / 4]);
                for (t = 0; t < (int)(pk.control & 0xFF) && t < 8; t++)
                    fprintf(stderr, "[REVSUB]   stage %d: c icw %08X ocw %08X  a icw %08X ocw %08X"
                            "  c0 %08X c1 %08X\n", t, pk.color_icw[t], pk.color_ocw[t],
                            pk.alpha_icw[t], pk.alpha_ocw[t], r[(0xA60 + 4 * t) / 4], r[(0xA80 + 4 * t) / 4]);
                for (t = 0; t < 4; t++) {
                    uint32_t bs = (0x1B00u + (uint32_t)t * 0x40u) / 4;
                    if (!((pk.shader_program >> (5 * t)) & 0x1F))
                        continue;
                    fprintf(stderr, "[REVSUB]   tex %d: va %08X format %08X addr %08X control0 %08X"
                            " filter %08X rect %08X border %08X\n", t, r[bs], r[bs + 1], r[bs + 2],
                            r[bs + 3], r[bs + 5], r[bs + 7], r[bs + 9]);
                }
                if (b->xform == 1) {
                    uint32_t i, c;
                    for (i = 0; i < 2; i++) {
                        fprintf(stderr, "[REVSUB]   texgen %u:", i);
                        for (c = 0; c < 4; c++)
                            fprintf(stderr, " %04X", r[(0x3C0 + i * 16 + c * 4) / 4]);
                        fprintf(stderr, " matrix %u\n", r[(0x420 + i * 4) / 4]);
                    }
                }
            }
        }
        if ((srs || s_mod_shadows == 0) && key.blend && key.beq == 0x800B)
            return;
        if (sst && d && r[0x32C / 4] && r[0x358 / 4])
            return;
        if (smul && key.blend && (key.bdst == 0x300 || key.bsrc == 0x306))
            return;
        {
            /* NV2A_SKIP_ALPHA=ffp / prog: alpha-blended draws (SRC_ALPHA,
             * INV_SRC_ALPHA) of the fixed pipeline or of vertex programs --
             * smoke and other soft sprites. */
            static int sa = -1;
            if (sa < 0) {
                const char *e = getenv("NV2A_SKIP_ALPHA");
                sa = !e ? 0 : *e == 'f' ? 1 : *e == 'p' ? 2 : *e == 'z' ? 3 : 0;
            }
            /* =z: any blended draw that does not write depth (sprites,
             * smoke, decals), whichever pipeline. */
            if (sa == 3 && key.blend && !r[0x35C / 4])
                return;
            if (sa && sa < 3 && key.blend && key.bsrc == 0x302 && key.bdst == 0x303
                && (sa == 1 ? b->xform != 2 : b->xform == 2))
                return;
        }
    }
    {
        uint32_t cm = r[0x358 / 4], m = 0;
        if (cm & 0x00010000u) m |= VK_COLOR_COMPONENT_R_BIT;
        if (cm & 0x00000100u) m |= VK_COLOR_COMPONENT_G_BIT;
        if (cm & 0x00000001u) m |= VK_COLOR_COMPONENT_B_BIT;
        if (cm & 0x01000000u) m |= VK_COLOR_COMPONENT_A_BIT;
        key.cmask = m;
    }
    key.depth = d != NULL;
    key.topo = cls;
    pipe = pipe_get(&key);
    if (!pipe)
        return;

    {
        /* Everything this draw puts in the ring, reserved now: a flush
         * later in the draw would reuse the ring under its own uniforms. */
        VkDeviceSize need = 2 * 4096 + sizeof(VsBlock) + sizeof(FsBlock) + VK_CONST_SIZE + (VkDeviceSize)n * 4
                          + NV2A_RAW_ATTRS * 16 + 16 * 16;
        uint32_t a;
        for (a = 0; a < NV2A_RAW_ATTRS; a++)
            if (b->attr_present & (1u << a)) {
                uint32_t st = (b->attr_direct & (1u << a)) ? b->direct[a].stride : 0;
                need += (VkDeviceSize)b->vertex_count * (st > 16 ? st : 16) + 16;
            }
        if (s_f->ring_off + need > RING_BYTES) {
            void *tmp;
            if (need > RING_BYTES) return;
            ring_alloc(RING_BYTES, 16, &tmp);       /* forces the flush */
            s_f->ring_off = 0;
        }
    }
    begin_rendering(s, d);
    if (d && !b->surface.color_va && d->zonly_frame != s_frame + 1) {
        /* First depth-only (shadow buffer) draw of the frame. If the title
         * did not clear the buffer this frame, the depths of every earlier
         * frame stay in it and the shadows smear and pile up as the camera
         * and the player move. NV2A_SHADOW_CLEAR=0 keeps the old behaviour. */
        static int on = -1;
        if (on < 0) {
            const char *e = getenv("NV2A_SHADOW_CLEAR");
            on = !(e && *e == '0');
        }
        d->zonly_frame = s_frame + 1;
        s_zonly_passes++;
        if (d->clr_frame != s_frame + 1) {
            s_zonly_noclear++;
            if (on) {
                VkClearAttachment ca;
                VkClearRect rect;
                memset(&ca, 0, sizeof ca);
                memset(&rect, 0, sizeof rect);
                ca.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
                ca.clearValue.depthStencil.depth = 1.0f;
                rect.layerCount = 1;
                rect.rect.extent.width = d->pw < s->pw ? d->pw : s->pw;
                rect.rect.extent.height = d->ph < s->ph ? d->ph : s->ph;
                vkCmdClearAttachments(s_cb, 1, &ca, 1, &rect);
                d->clr_frame = s_frame + 1;
            }
        }
    }
    s->dirty = 1;
    s->gen++;
    if (pipe != cur_pipe || cur_pipe_gen != s_cb_gen) {
        vkCmdBindPipeline(s_cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        cur_pipe = pipe;
        cur_pipe_gen = s_cb_gen;
    }
    apply_dynamic(r, d != NULL, topo);
    set_surface_scissor(r, s);

    /* Uniforms. */
    {
        VsBlock *vb;
        FsBlock *fb;
        VkDeviceSize vat = ring_alloc(sizeof(VsBlock), s_ubo_align, &map);
        vb = (VsBlock *)map;
        if (!vb) return;
        vb->surf[0] = 2.0f / (float)s->w;
        vb->surf[1] = 2.0f / (float)s->h;
        vb->surf[2] = 1.0f / (float)zmax_of(r);
        vb->surf[3] = 0.5f - 0.5f * (float)s->w / (float)s->pw; /* see nv2a_snap */
        memcpy(vb->m, b->composite, sizeof vb->m);
        memcpy(vb->vpoff, b->vp_offset, sizeof vb->vpoff);
        vb->aa[0] = b->aa_sx > 0 ? b->aa_sx : 1.0f;
        vb->aa[1] = b->aa_sy > 0 ? b->aa_sy : 1.0f;
        vb->xform = b->xform == 1 ? 1 : 0;
        {
            /* Fixed-function lighting (SET_LIGHTING_ENABLE): a third of
             * Cold Fear's draws, its lamps and alarm lights among them. They
             * were drawn with the bare vertex colour, so the ship darkened
             * wherever the lights should have lit it. NV2A_FFP_LIGHT=0: off. */
            static int on = -1;
            if (on < 0) {
                const char *e = getenv("NV2A_FFP_LIGHT");
                on = !(e && *e == '0');
            }
            vb->lctl[0] = on && vb->xform == 1 && r[0x314 / 4] ? 1 : 0;
            memcpy(vb->mv, &r[0x480 / 4], sizeof vb->mv);
            vb->tctl[0] = 0;
            if (vb->xform == 1) {
                /* SET_TEXGEN_S/T/R/Q 0x03C0 + 16 per stage, matrix enable
                 * 0x0420, matrices 0x06C0 + 64, planes 0x0840 + 64. */
                static int tg_on = -1;
                uint32_t i, c;
                if (tg_on < 0) {
                    const char *e = getenv("NV2A_TEXGEN");
                    tg_on = !(e && *e == '0');
                }
                for (i = 0; i < 4 && tg_on; i++) {
                    vb->tmen[i] = r[(0x420 + i * 4) / 4] ? 1 : 0;
                    for (c = 0; c < 4; c++) {
                        uint32_t g = r[(0x3C0 + i * 16 + c * 4) / 4];
                        vb->tgen[i][c] = g == 0x2400 ? 1 : g == 0x2401 ? 2 : g == 0x2402 ? 3
                                       : g == 0x8511 ? 4 : g == 0x8512 ? 5 : 0;
                        if (vb->tgen[i][c]) vb->tctl[0] = 1;
                    }
                    if (vb->tmen[i]) vb->tctl[0] = 1;
                }
                if (vb->tctl[0]) {
                    memcpy(vb->tmat, &r[0x6C0 / 4], sizeof vb->tmat);
                    memcpy(vb->tplane, &r[0x840 / 4], sizeof vb->tplane);
                    {
                        /* [TEXGEN] each setup the title uses, first 16. */
                        static int32_t seen[16][20];
                        static int nseen;
                        int32_t key[20];
                        int k;
                        memcpy(key, vb->tgen, 64);
                        memcpy(key + 16, vb->tmen, 16);
                        for (k = 0; k < nseen && memcmp(seen[k], key, sizeof key); k++) ;
                        if (k == nseen && nseen < 16) {
                            memcpy(seen[nseen++], key, sizeof key);
                            fprintf(stderr, "[TEXGEN] modes %d%d%d%d %d%d%d%d %d%d%d%d %d%d%d%d matrix %d%d%d%d\n",
                                    key[0], key[1], key[2], key[3], key[4], key[5], key[6], key[7],
                                    key[8], key[9], key[10], key[11], key[12], key[13], key[14], key[15],
                                    key[16], key[17], key[18], key[19]);
                        }
                    }
                }
            }
            if (vb->lctl[0]) {
                uint32_t l, k;
                vb->lctl[1] = (int32_t)r[0x3BC / 4];
                vb->lctl[2] = (int32_t)r[0x298 / 4];
                {
                    /* NV2A_FFP_FLIP=0: light normals as stored (see
                     * nv2a_light in gl_vsh.c). */
                    static int flip = -1;
                    if (flip < 0) {
                        const char *e = getenv("NV2A_FFP_FLIP");
                        flip = !(e && *e == '0');
                    }
                    vb->lctl[3] = flip;
                }
                memcpy(vb->lscene, &r[0xA10 / 4], 12);
                memcpy(&vb->lscene[3], &r[0x3B4 / 4], 4);
                memcpy(vb->lemis, &r[0x3A8 / 4], 12);
                vb->lemis[3] = 0.0f;
                for (l = 0; l < 8; l++) {
                    const uint32_t *lr = &r[(0x1000 + l * 0x80) / 4];
                    float (*o)[4] = &vb->light[l * 6];
                    if (!((r[0x3BC / 4] >> (2 * l)) & 3)) {
                        memset(o, 0, 6 * sizeof *o);
                        continue;
                    }
                    memcpy(o[0], lr + 0x00 / 4, 12); memcpy(&o[0][3], lr + 0x24 / 4, 4);
                    memcpy(o[1], lr + 0x0C / 4, 12); o[1][3] = 0.0f;
                    memcpy(o[2], lr + 0x5C / 4, 12); o[2][3] = 0.0f;
                    memcpy(o[3], lr + 0x68 / 4, 12); o[3][3] = 0.0f;
                    memcpy(o[4], lr + 0x34 / 4, 12); o[4][3] = 0.0f;
                    memcpy(o[5], lr + 0x4C / 4, 16);
                }
                {
                    /* [FFPLIGHT] each light setup the title uses, first 16. */
                    static uint32_t seen[16][3];
                    static int nseen;
                    uint32_t key[3] = { r[0x3BC / 4], r[0x298 / 4], r[0x1000 / 4] ^ r[0x105C / 4] };
                    for (k = 0; k < (uint32_t)nseen && memcmp(seen[k], key, sizeof key); k++) ;
                    if (k == (uint32_t)nseen && nseen < 16) {
                        memcpy(seen[nseen++], key, sizeof key);
                        fprintf(stderr, "[FFPLIGHT] mask %08X colmat %02X scene %.2f %.2f %.2f emis %.2f %.2f %.2f"
                                " L0 amb %.2f %.2f %.2f dif %.2f %.2f %.2f range %.1f att %.3f %.3f %.4f\n",
                                key[0], key[1], vb->lscene[0], vb->lscene[1], vb->lscene[2],
                                vb->lemis[0], vb->lemis[1], vb->lemis[2],
                                vb->light[0][0], vb->light[0][1], vb->light[0][2],
                                vb->light[1][0], vb->light[1][1], vb->light[1][2], vb->light[0][3],
                                vb->light[3][0], vb->light[3][1], vb->light[3][2]);
                    }
                }
            }
        }
        {
            /* Fog unit (gl_vsh.c nv2a_fog): SET_FOG_MODE 0x029C as 0 linear,
             * 1 exp, 2 exp2, +4 for the _ABS forms; params at 0x09C0/4. */
            uint32_t fm = r[0x29C / 4];
            memcpy(&vb->fog[0], &r[0x9C0 / 4], 4);
            memcpy(&vb->fog[1], &r[0x9C4 / 4], 4);
            vb->fog[2] = fm == 0x800 ? 1.0f : fm == 0x801 ? 2.0f : fm == 0x802 ? 5.0f
                       : fm == 0x803 ? 6.0f : fm == 0x804 ? 4.0f : 0.0f;
            vb->fog[3] = r[0x2A4 / 4] ? 1.0f : 0.0f;
            {
                /* NV2A_FOG=0: fog off (diagnostics); [FOG] names each mode
                 * and parameter set the title uses, first 12. */
                static int fog_on = -1;
                static uint32_t seen[12][3];
                static int nseen;
                int k;
                if (fog_on < 0) {
                    const char *e = getenv("NV2A_FOG");
                    fog_on = !(e && *e == '0');
                }
                if (!fog_on || s_mod_fog == 0)
                    vb->fog[3] = 0.0f;
                for (k = 0; k < nseen && (seen[k][0] != fm || seen[k][1] != r[0x9C0 / 4]
                                          || seen[k][2] != r[0x2A4 / 4]); k++) ;
                if (k == nseen && nseen < 12) {
                    seen[nseen][0] = fm; seen[nseen][1] = r[0x9C0 / 4]; seen[nseen][2] = r[0x2A4 / 4];
                    nseen++;
                    fprintf(stderr, "[FOG] mode %04X enable %u params %g %g color %08X\n",
                            fm, r[0x2A4 / 4], vb->fog[0], vb->fog[1], r[0x2A8 / 4]);
                }
            }
        }
        {
            /* SET_ZMIN_MAX_CONTROL: CULL_NEAR_FAR_EN in bits 0-3, ZCLAMP_EN
             * in bits 4-7 (0 = cull). Never written (0) keeps clamping. */
            uint32_t zc = r[0x1D78 / 4];
            static int zcull_on = -1;           /* NV2A_ZCULL=0: clamp instead */
            if (zcull_on < 0) {
                const char *e = getenv("NV2A_ZCULL");
                zcull_on = !(e && *e == '0');
            }
            vb->zcull = zcull_on && s_have_clip_dist && (zc & 0x0Fu) && !(zc & 0xF0u);
        }
        ub[0].buffer = s_f->ring;
        ub[0].offset = vat;
        ub[0].range = sizeof(VsBlock);

        {
            VkDeviceSize fat = ring_alloc(sizeof(FsBlock), s_ubo_align, &map);
            fb = (FsBlock *)map;
            if (!fb) return;
            memset(fb, 0, sizeof *fb);
            for (i = 0; i < 4; i++) {
                fb->tscale[i][0] = tscale[i][0];
                fb->tscale[i][1] = tscale[i][1];
            }
            for (i = 0; i < 8; i++) {
                argb_vec4(r[0xA60 / 4 + i], fb->c0[i]);
                argb_vec4(r[0xA80 / 4 + i], fb->c1[i]);
            }
            argb_vec4(r[0x1E20 / 4], fb->fc0);
            argb_vec4(r[0x1E24 / 4], fb->fc1);
            {
                uint32_t fc = r[0x2A8 / 4];
                fb->fog[0] = byte_f(fc, 0); fb->fog[1] = byte_f(fc, 8);
                fb->fog[2] = byte_f(fc, 16); fb->fog[3] = byte_f(fc, 24);
            }
            uint32_t af = r[0x33C / 4];
            if (af < 0x200 || af > 0x207) af = 0x207;
            fb->afunc = r[0x300 / 4] ? (int32_t)af : 0x207;
            fb->aref = (float)(r[0x340 / 4] & 0xFF);
            /* Mod menu, lamp shadows SUAVE: the passes that subtract the
             * lamps' shadows (REVERSE_SUBTRACT) take 25 / 50 / 75%. */
            fb->out_scale = s_mod_shadows >= 1 && s_mod_shadows <= 3 && r[0x304 / 4]
                            && r[0x350 / 4] == 0x800B ? s_mod_shadows * 0.25f : 1.0f;
            ub[1].buffer = s_f->ring;
            ub[1].offset = fat;
            ub[1].range = sizeof(FsBlock);
        }
    }
    if (!upload_vertices(b))
        return;
    {
        VkDeviceSize iat;
        if (b->vertex_count <= 65536) {
            uint16_t *o;
            iat = ring_alloc((VkDeviceSize)n * 2, 4, &map);
            o = (uint16_t *)map;
            if (!o) return;
            for (i = 0; i < n; i++)
                o[i] = (uint16_t)idx[i];
            vkCmdBindIndexBuffer(s_cb, s_f->ring, iat, VK_INDEX_TYPE_UINT16);
        } else {
            iat = ring_alloc((VkDeviceSize)n * 4, 4, &map);
            if (!map) return;
            memcpy(map, idx, (size_t)n * 4);
            vkCmdBindIndexBuffer(s_cb, s_f->ring, iat, VK_INDEX_TYPE_UINT32);
        }
    }
    {
        /* Constants last: repeats append theirs right behind (pend_append). */
        VkDeviceSize cat = ring_alloc(VK_CONST_SIZE, s_ubo_align, &map), range;
        if (!map) return;
        memcpy(map, b->vp_consts, VK_CONST_SIZE);
        range = (VkDeviceSize)VK_CONST_SIZE * VK_INST_MAX;
        if (range > s_ubo_range) range = s_ubo_range;
        if (cat + range > RING_BYTES) range = RING_BYTES - cat;
        range = range / VK_CONST_SIZE * VK_CONST_SIZE;
        inst_room = (uint32_t)(range / VK_CONST_SIZE);
        ub[2].buffer = s_f->ring;
        ub[2].offset = cat;
        ub[2].range = range;
    }
    memset(wr, 0, sizeof wr);
    for (i = 0; i < 7; i++) {
        wr[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        wr[i].dstBinding = i;
        wr[i].descriptorCount = 1;
        if (i < 2 || i == 6) {
            wr[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            wr[i].pBufferInfo = &ub[i == 6 ? 2 : i];
        } else {
            wr[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            wr[i].pImageInfo = &img[i - 2];
        }
    }
    p_push_desc(s_cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s_layout, 0, 7, wr);

    VT("  vkCmdDrawIndexed %u\n", n);
    /* The constants were the ring's last allocation, so a repeat's set
     * lands right behind them (pend_append). */
    if (pend_eligible(b) && inst_room > 1) {
        pend_hold(b, n, ub[2].offset, inst_room);
    } else {
        vkCmdDrawIndexed(s_cb, n, 1, 0, 0, 0);
        s_st_calls++;
    }
    if (s_vtrace > 0)
        s_vtrace--;
}

static void vk_clear(const Nv2aSurface *sf, const Nv2aRenderState *rs,
                     uint32_t flags, uint32_t argb, uint32_t zstencil)
{
    VkSurf *s;
    VkDepthBuf *d;
    VkClearAttachment ca[2];
    VkClearRect rect;
    uint32_t nca = 0;

    if (!ready())
        return;
    pend_flush();
    s = target(sf, rs ? rs->zeta_va : 0, &d);
    if (!s)
        return;
    memset(&rect, 0, sizeof rect);
    rect.layerCount = 1;
    rect.rect.extent.width = s->pw;
    rect.rect.extent.height = s->ph;
    if (s_regs) {
        uint32_t hz = s_regs[0x1D98 / 4], vt = s_regs[0x1D9C / 4];
        uint32_t x0 = (hz & 0xFFFF) * s->aa_sx, x1 = ((hz >> 16) + 1) * s->aa_sx;
        uint32_t y0 = (vt & 0xFFFF) * s->aa_sy, y1 = ((vt >> 16) + 1) * s->aa_sy;
        if (x1 > s->w) x1 = s->w;
        if (y1 > s->h) y1 = s->h;
        if (x1 > x0 && y1 > y0) {
            uint32_t px0 = to_stored(x0, s->pw, s->w), py0 = to_stored(y0, s->ph, s->h);
            rect.rect.offset.x = (int32_t)px0;
            rect.rect.offset.y = (int32_t)py0;
            rect.rect.extent.width = to_stored(x1, s->pw, s->w) - px0;
            rect.rect.extent.height = to_stored(y1, s->ph, s->h) - py0;
        }
    }
    if (d && rect.rect.extent.width > d->pw - (uint32_t)rect.rect.offset.x)
        rect.rect.extent.width = d->pw - (uint32_t)rect.rect.offset.x;
    if (d && rect.rect.extent.height > d->ph - (uint32_t)rect.rect.offset.y)
        rect.rect.extent.height = d->ph - (uint32_t)rect.rect.offset.y;
    memset(ca, 0, sizeof ca);
    if ((flags & 0xF0) == 0xF0) {
        ca[nca].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        ca[nca].colorAttachment = 0;
        argb_vec4(argb, ca[nca].clearValue.color.float32);
        nca++;
    } else if (flags & 0xF0) {
        static int warned;
        if (!warned++)
            LOGE("clear with a partial colour mask (0x%X) not done yet\n", flags & 0xF0);
    }
    if (d && (flags & 0x3)) {
        uint32_t zmax = s_regs ? zmax_of(s_regs) : 0xFFFFFFu;
        uint32_t z = zmax == 0xFFFFu ? (zstencil & 0xFFFF) : (zstencil >> 8);
        ca[nca].aspectMask = ((flags & 1) ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) |
                             ((flags & 2) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
        ca[nca].clearValue.depthStencil.depth = (float)((double)z / (double)zmax);
        ca[nca].clearValue.depthStencil.stencil = zstencil & 0xFF;
        nca++;
        if (flags & 1) s_clr_z++;
        if (flags & 2) s_clr_s++;
        if (flags & 1) {
            d->clr_frame = s_frame + 1;
            if (!sf->color_va) {
                static int logged;
                if (logged++ < 8)
                    fprintf(stderr, "[ZONLY] clear zeta %08X flags %X z %.4f rect %d,%d %ux%u (buffer %ux%u)\n",
                            d->va, flags, ca[nca - 1].clearValue.depthStencil.depth,
                            rect.rect.offset.x, rect.rect.offset.y, rect.rect.extent.width,
                            rect.rect.extent.height, d->pw, d->ph);
            }
        }
    }
    if (!nca)
        return;
    VT("clear %08X %ux%u flags %X rect %d,%d %ux%u depth %p\n", s->va, s->pw, s->ph, flags,
       rect.rect.offset.x, rect.rect.offset.y, rect.rect.extent.width, rect.rect.extent.height, (void *)d);
    begin_rendering(s, d);
    vkCmdClearAttachments(s_cb, nca, ca, 1, &rect);
    VT("  cleared\n");
    s_drew_any = 1;
    s_frame_draws++;
    s->dirty = 1;
    s->gen++;
}

static void dump_bmp(const char *path, const uint8_t *bgra, uint32_t w, uint32_t h)
{
    FILE *f = fopen(path, "wb");
    uint8_t hdr[54] = { 'B', 'M' };
    uint32_t row = w * 3, pad = (4 - (row & 3)) & 3, size = 54 + (row + pad) * h, y, x;
    if (!f)
        return;
    memcpy(hdr + 2, &size, 4);
    hdr[10] = 54; hdr[14] = 40;
    memcpy(hdr + 18, &w, 4);
    memcpy(hdr + 22, &h, 4);
    hdr[26] = 1; hdr[28] = 24;
    fwrite(hdr, 1, 54, f);
    for (y = h; y-- > 0; ) {
        const uint8_t *p = bgra + (size_t)y * w * 4;
        for (x = 0; x < w; x++)
            fwrite(p + x * 4, 1, 3, f);
        fwrite("\0\0\0", 1, pad, f);
    }
    fclose(f);
}

/* RECOMP_GL_DUMP: read the surface back (waits for the GPU). */
static void dump_surface(VkSurf *s, const char *path)
{
    VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    VkBuffer buf;
    VkDeviceMemory mem;
    VkMemoryRequirements rq;
    VkBufferImageCopy rg;
    void *ptr;

    bci.size = (VkDeviceSize)s->pw * s->ph * 4;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (vkCreateBuffer(s_dev, &bci, NULL, &buf) != VK_SUCCESS)
        return;
    vkGetBufferMemoryRequirements(s_dev, buf, &rq);
    mem = alloc_mem(rq, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (!mem) { vkDestroyBuffer(s_dev, buf, NULL); return; }
    vkBindBufferMemory(s_dev, buf, mem, 0);
    end_rendering();
    barrier_all();
    memset(&rg, 0, sizeof rg);
    rg.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    rg.imageSubresource.layerCount = 1;
    rg.imageExtent.width = s->pw;
    rg.imageExtent.height = s->ph;
    rg.imageExtent.depth = 1;
    vkCmdCopyImageToBuffer(s_cb, s->image, VK_IMAGE_LAYOUT_GENERAL, buf, 1, &rg);
    flush_frame_wait();
    if (vkMapMemory(s_dev, mem, 0, bci.size, 0, &ptr) == VK_SUCCESS) {
        dump_bmp(path, (const uint8_t *)ptr, s->pw, s->ph);
        vkUnmapMemory(s_dev, mem);
    }
    vkDestroyBuffer(s_dev, buf, NULL);
    vkFreeMemory(s_dev, mem, NULL);
}

static inline uint32_t pvideo_resolve_va(uint32_t addr)
{
    if (addr >= 0x80000000u && addr < 0x84000000u)
        return addr;
    uint32_t phys = addr & 0x03FFFFFFu;
    extern int xbox_ContiguousIsPhysical(uint32_t phys);
    if (xbox_ContiguousIsPhysical(phys))
        return 0x80000000u + phys;
    return phys;
}

static inline uint32_t yuy2_to_bgra(int y, int u, int v)
{
    int c = y - 16;
    if (c < 0) c = 0;
    int d = u - 128;
    int e = v - 128;
    int r = (298 * c + 409 * e + 128) >> 8;
    int g = (298 * c - 100 * d - 208 * e + 128) >> 8;
    int b = (298 * c + 516 * d + 128) >> 8;
    if (r < 0) r = 0; else if (r > 255) r = 255;
    if (g < 0) g = 0; else if (g > 255) g = 255;
    if (b < 0) b = 0; else if (b > 255) b = 255;
    return 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

/* ── Display gamma ramp ──────────────────────────────────────────────
 *
 * Xbox D3D's SetGammaRamp reaches the screen through the DAC palette
 * (PRMDIO 0x6813C8 / 0x6813C9, written by its flip handler), and the title
 * draws its frames counting on it: Cold Fear brightens its dark ship that
 * way. The register aperture here is plain memory, so a title hook hands
 * the ramp over (nv2a_vk_set_gamma_ramp: 256 red, 256 green, 256 blue
 * bytes) and the frame goes through it on its way to the swapchain. An
 * identity ramp, or NV2A_GAMMA=0, skips the pass. */
static uint8_t  s_gamma_ramp[768];
static volatile uint32_t s_gamma_gen;
static volatile int s_gamma_identity = 1;
static uint32_t s_gamma_seen = 0xFFFFFFFFu;   /* nothing uploaded yet */
static VkImage s_glut_img, s_gout_img;
static VkImageView s_glut_view, s_gout_view;
static VkDeviceMemory s_glut_mem, s_gout_mem;
static uint32_t s_gout_w, s_gout_h;
static VkDescriptorSetLayout s_gdsl;
static VkPipelineLayout s_glayout;
static VkPipeline s_gpipe;
static int s_gamma_broken;

/* Player brightness, 0..10, on top of the title's ramp: each step lifts the
 * mid-tones (gamma 1 + 0.12 per step; black and white stay put). Starts
 * from brightness.txt beside the caches, else NV2A_BRIGHTNESS; changed in
 * game with nv2a_vk_brightness_step (Minus + D-pad up/down) and saved. */
static volatile int s_bright = -1;
/* Monotonic nanoseconds, for the on-screen overlays' timing. */
static uint64_t ov_now_ns(void)
{
#if defined(__SWITCH__)
    return armTicksToNs(armGetSystemTick());
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#endif
}
static volatile uint64_t s_bright_shown;  /* system tick of the last press: shows the panel */

static int bright_level(void)
{
    if (s_bright < 0) {
        char path[256];
        FILE *f;
        int v = -1;
        if (cache_path(path, sizeof path, "brightness.txt") && (f = fopen(path, "r")) != NULL) {
            if (fscanf(f, "%d", &v) != 1)
                v = -1;
            fclose(f);
        }
        if (v < 0) {
            const char *e = getenv("NV2A_BRIGHTNESS");
            v = e ? atoi(e) : 0;
        }
        s_bright = v < 0 ? 0 : v > 10 ? 10 : v;
    }
    return s_bright;
}

void nv2a_vk_brightness_step(int d)
{
    char path[256];
    FILE *f;
    int v = bright_level() + d;
    v = v < 0 ? 0 : v > 10 ? 10 : v;
    s_bright_shown = ov_now_ns();
    if (v == s_bright)
        return;
    s_bright = v;
    __atomic_add_fetch(&s_gamma_gen, 1, __ATOMIC_RELEASE);
    if (cache_path(path, sizeof path, "brightness.txt") && (f = fopen(path, "w")) != NULL) {
        fprintf(f, "%d\n", v);
        fclose(f);
    }
    fprintf(stderr, "[BRIGHT] level %d\n", v);
}

static uint32_t bright_apply(uint32_t x, int level)
{
    double g = 1.0 + 0.12 * level;
    return level ? (uint32_t)(255.0 * pow(x / 255.0, 1.0 / g) + 0.5) : x;
}

void nv2a_vk_set_gamma_ramp(const uint8_t *ramp)
{
    int i, id = 1;
    if (!memcmp(ramp, s_gamma_ramp, sizeof s_gamma_ramp) && s_gamma_gen)
        return;
    memcpy(s_gamma_ramp, ramp, sizeof s_gamma_ramp);
    for (i = 0; i < 256 && id; i++)
        id = ramp[i] == i && ramp[256 + i] == i && ramp[512 + i] == i;
    s_gamma_identity = id;
    __atomic_add_fetch(&s_gamma_gen, 1, __ATOMIC_RELEASE);
    {
        static int logged;
        if (logged++ < 4)
            fprintf(stderr, "[GAMMA] ramp %s: R 0->%u 64->%u 128->%u 192->%u 255->%u\n",
                    id ? "identity" : "set", ramp[0], ramp[64], ramp[128], ramp[192], ramp[255]);
    }
}

static int gamma_init(void)
{
    static const char vsrc[] =
        "#version 450\n"
        "layout(location = 0) out vec2 uv;\n"
        "void main() {\n"
        "    vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);\n"
        "    uv = p;\n"
        "    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
        "}\n";
    static const char fsrc[] =
        "#version 450\n"
        "layout(set = 0, binding = 0) uniform sampler2D src;\n"
        "layout(set = 0, binding = 1) uniform sampler2D lut;\n"
        "layout(location = 0) in vec2 uv;\n"
        "layout(location = 0) out vec4 o;\n"
        "float g(float c, int ch) {\n"
        "    vec4 t = texture(lut, vec2((clamp(c, 0.0, 1.0) * 255.0 + 0.5) / 256.0, 0.5));\n"
        "    return ch == 0 ? t.r : ch == 1 ? t.g : t.b;\n"
        "}\n"
        "void main() {\n"
        "    vec4 c = texture(src, uv);\n"
        "    o = vec4(g(c.r, 0), g(c.g, 1), g(c.b, 2), c.a);\n"
        "}\n";
    VkDescriptorSetLayoutBinding b[2];
    VkDescriptorSetLayoutCreateInfo lci = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    VkGraphicsPipelineCreateInfo pci = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    VkPipelineShaderStageCreateInfo st[2];
    VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    VkPipelineViewportStateCreateInfo vp = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    VkPipelineColorBlendAttachmentState ba;
    VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    VkPipelineDynamicStateCreateInfo dy = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    VkPipelineRenderingCreateInfo rci = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
    static const VkDynamicState dyn[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkFormat cfmt = VK_FORMAT_B8G8R8A8_UNORM;
    VkShaderModule vs, fs;

    memset(b, 0, sizeof b);
    b[0].binding = 0; b[1].binding = 1;
    b[0].descriptorCount = b[1].descriptorCount = 1;
    b[0].descriptorType = b[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b[0].stageFlags = b[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    lci.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    lci.bindingCount = 2;
    lci.pBindings = b;
    if (vkCreateDescriptorSetLayout(s_dev, &lci, NULL, &s_gdsl) != VK_SUCCESS)
        return 0;
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &s_gdsl;
    if (vkCreatePipelineLayout(s_dev, &pli, NULL, &s_glayout) != VK_SUCCESS)
        return 0;
    vs = compile(GLSLANG_STAGE_VERTEX, vsrc);
    fs = compile(GLSLANG_STAGE_FRAGMENT, fsrc);
    if (!vs || !fs)
        return 0;
    memset(st, 0, sizeof st);
    st[0].sType = st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;   st[0].module = vs; st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = fs; st[1].pName = "main";
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    vp.viewportCount = vp.scissorCount = 1;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1.0f;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    memset(&ba, 0, sizeof ba);
    ba.colorWriteMask = 0xF;
    cb.attachmentCount = 1;
    cb.pAttachments = &ba;
    dy.dynamicStateCount = 2;
    dy.pDynamicStates = dyn;
    rci.colorAttachmentCount = 1;
    rci.pColorAttachmentFormats = &cfmt;
    pci.pNext = &rci;
    pci.stageCount = 2;
    pci.pStages = st;
    pci.pVertexInputState = &vi;
    pci.pInputAssemblyState = &ia;
    pci.pViewportState = &vp;
    pci.pRasterizationState = &rs;
    pci.pMultisampleState = &ms;
    pci.pColorBlendState = &cb;
    pci.pDynamicState = &dy;
    pci.layout = s_glayout;
    if (vkCreateGraphicsPipelines(s_dev, VK_NULL_HANDLE, 1, &pci, NULL, &s_gpipe) != VK_SUCCESS)
        return 0;
    if (!make_image(256, 1, VK_FORMAT_B8G8R8A8_UNORM,
                    VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                    VK_IMAGE_ASPECT_COLOR_BIT, &s_glut_img, &s_glut_view, &s_glut_mem))
        return 0;
    return 1;
}

/* The image to present from: the surface itself, or its gamma-mapped copy. */
/* ── On-screen overlays: brightness panel, mod menu, FPS counter ─────
 *
 * Drawn on the CPU in "units" (one font pixel; a unit is 2-4 screen pixels
 * by the swapchain's height) into the frame ring and copied into the
 * swapchain image after the frame's blit. 5x7 font in nv2a_menu.h. */
#include "nv2a_menu.h"

typedef struct {
    uint32_t *px;
    uint32_t cw, ch, u, pw, ph;
    VkDeviceSize at;
} Ov;

static uint32_t ov_rgb(uint32_t r, uint32_t g, uint32_t b)
{
    return 0xFF000000u | (s_sc_format == VK_FORMAT_B8G8R8A8_UNORM
                          ? (r << 16 | g << 8 | b) : (b << 16 | g << 8 | r));
}

static uint32_t ov_unit(void)
{
    return s_sc_extent.height >= 1080 ? 4 : s_sc_extent.height >= 720 ? 3 : 2;
}

static int ov_begin(Ov *o, uint32_t cw, uint32_t ch, uint32_t u)
{
    o->cw = cw; o->ch = ch; o->u = u;
    o->pw = cw * u; o->ph = ch * u;
    if (o->pw > s_sc_extent.width || o->ph > s_sc_extent.height)
        return 0;
    o->at = ring_alloc((VkDeviceSize)o->pw * o->ph * 4, 16, (void **)&o->px);
    return o->px != NULL;
}

static void ov_rect(Ov *o, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t c)
{
    uint32_t r, i;
    if (x >= o->cw || y >= o->ch)
        return;
    if (x + w > o->cw) w = o->cw - x;
    if (y + h > o->ch) h = o->ch - y;
    for (r = y * o->u; r < (y + h) * o->u; r++)
        for (i = x * o->u; i < (x + w) * o->u; i++)
            o->px[(size_t)r * o->pw + i] = c;
}

/* Text at (x, y) in units; returns the width drawn. */
static uint32_t ov_text(Ov *o, uint32_t x, uint32_t y, const char *s, uint32_t c)
{
    uint32_t x0 = x, gx, gy;
    for (; *s; s++, x += 6) {
        const uint8_t *g = menu_glyph(*s);
        for (gy = 0; g && gy < 7; gy++)
            for (gx = 0; gx < 5; gx++)
                if (g[gy] & (0x10 >> gx))
                    ov_rect(o, x + gx, y + gy, 1, 1, c);
    }
    return x - x0;
}

static void ov_end(Ov *o, VkImage dst, int32_t sx, int32_t sy)
{
    VkBufferImageCopy rg;
    VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
    mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(s_cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 1, &mb, 0, NULL, 0, NULL);
    memset(&rg, 0, sizeof rg);
    rg.bufferOffset = o->at;
    rg.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    rg.imageSubresource.layerCount = 1;
    rg.imageOffset.x = sx;
    rg.imageOffset.y = sy;
    rg.imageExtent.width = o->pw;
    rg.imageExtent.height = o->ph;
    rg.imageExtent.depth = 1;
    vkCmdCopyBufferToImage(s_cb, s_f->ring, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rg);
}

/* The brightness panel: "BRILHO", the level and a bar of ten segments,
 * over the bottom of the image for 2.5 s after each Minus + D-pad press. */
static void bright_panel(VkImage dst)
{
    enum { CW = 126, CH = 30 };
    uint64_t t = s_bright_shown;
    char text[8];
    uint32_t i;
    Ov o;

    if (!t || ov_now_ns() - t > 2500000000ull)
        return;
    if (!ov_begin(&o, CW, CH, ov_unit()))
        return;
    ov_rect(&o, 0, 0, CW, CH, ov_rgb(150, 150, 150));
    ov_rect(&o, 1, 1, CW - 2, CH - 2, ov_rgb(16, 16, 20));
    for (i = 0; i < 10; i++)
        ov_rect(&o, 4 + i * 12, 16, 10, 10, (int)i < bright_level() ? ov_rgb(235, 180, 60) : ov_rgb(60, 60, 60));
    ov_text(&o, 4, 4, "BRILHO", ov_rgb(240, 240, 240));
    snprintf(text, sizeof text, "%d/10", bright_level());
    ov_text(&o, CW - 4 - 6 * (uint32_t)strlen(text) + 1, 4, text, ov_rgb(240, 240, 240));
    ov_end(&o, dst, (int32_t)((s_sc_extent.width - o.pw) / 2),
           (int32_t)(s_sc_extent.height - o.ph - s_sc_extent.height / 8));
}

/* ── Mod menu ─────────────────────────────────────────────────────── */

static NvMenuItem s_menu[NV_MENU_MAX];
static int s_menu_n, s_menu_sel;
static volatile int s_menu_open;

static int env_int(const char *key, int def)
{
    const char *e = getenv(key);
    return e && *e ? atoi(e) : def;
}

static int menu_get_bright(void) { return bright_level(); }
static void menu_set_bright(int v) { nv2a_vk_brightness_step(v - bright_level()); }
static int menu_get_fps(void) { return s_mod_fps; }
static void menu_set_fps(int v) { s_mod_fps = v; }
static int menu_get_fog(void) { return s_mod_fog; }
static void menu_set_fog(int v) { s_mod_fog = v; }
static int menu_get_shadows(void) { return s_mod_shadows; }
static void menu_set_shadows(int v) { s_mod_shadows = v; }

void nv2a_menu_add(const char *label, const char *key, int min, int max, int (*get)(void),
                   void (*set)(int), const char *const *names, const char *const *values,
                   int restart)
{
    NvMenuItem *m;
    if (s_menu_n >= NV_MENU_MAX)
        return;
    m = &s_menu[s_menu_n++];
    m->label = label; m->key = key; m->min = min; m->max = max;
    m->get = get; m->set = set; m->names = names; m->values = values; m->restart = restart;
}

/* The renderer's own items, first in the list. */
static void menu_init(void)
{
    static const char *const yesno[] = { "NAO", "SIM" };
    static const char *const shadow_names[] = { "NAO", "25%", "50%", "75%", "SIM" };
    static int done;
    if (done++)
        return;
    s_mod_fps = env_int("COLDFEAR_MOD_FPS", 0) != 0;
    s_mod_fog = env_int("COLDFEAR_MOD_FOG", 1) != 0;
    s_mod_shadows = env_int("COLDFEAR_MOD_SHADOWS", 4);
    if (s_mod_shadows < 0 || s_mod_shadows > 4)
        s_mod_shadows = 4;
    {
        /* Built-ins go in front of whatever the port registered at boot. */
        NvMenuItem port[NV_MENU_MAX];
        int n = s_menu_n, k;
        memcpy(port, s_menu, sizeof port);
        s_menu_n = 0;
        nv2a_menu_add("BRILHO", NULL, 0, 10, menu_get_bright, menu_set_bright, NULL, NULL, 0);
        for (k = 0; k < n && k < 1; k++)        /* the port's first item: resolution */
            s_menu[s_menu_n++] = port[k];
        nv2a_menu_add("CONTADOR DE FPS", "COLDFEAR_MOD_FPS", 0, 1, menu_get_fps, menu_set_fps,
                      yesno, NULL, 0);
        nv2a_menu_add("SOMBRAS DAS LAMPADAS", "COLDFEAR_MOD_SHADOWS", 0, 4, menu_get_shadows,
                      menu_set_shadows, shadow_names, NULL, 0);
        nv2a_menu_add("NEVOA", "COLDFEAR_MOD_FOG", 0, 1, menu_get_fog, menu_set_fog, yesno, NULL, 0);
        for (k = 1; k < n && s_menu_n < NV_MENU_MAX; k++)
            s_menu[s_menu_n++] = port[k];
    }
}

/* mods.txt beside the caches: KEY=value for every item that has a key.
 * The port reads it at boot, before coldfear_env.txt. */
static void menu_save(void)
{
    char path[256];
    FILE *f;
    int i;
    if (!cache_path(path, sizeof path, "mods.txt") || !(f = fopen(path, "w")))
        return;
    fprintf(f, "# Written by the in-game mod menu (Minus + R3).\n");
    for (i = 0; i < s_menu_n; i++) {
        NvMenuItem *m = &s_menu[i];
        int v = m->get();
        if (!m->key)
            continue;
        if (m->values)
            fprintf(f, "%s=%s\n", m->key, m->values[v - m->min]);
        else
            fprintf(f, "%s=%d\n", m->key, v);
    }
    fclose(f);
}

/* From the controller thread (xinput_nx.c). */
int nv2a_menu_is_open(void) { return s_menu_open; }

void nv2a_menu_key(int key)
{
    menu_init();
    if (key == NV_MENU_TOGGLE || key == NV_MENU_CLOSE) {
        if (s_menu_open)
            menu_save();
        s_menu_open = key == NV_MENU_TOGGLE ? !s_menu_open : 0;
        fprintf(stderr, "[MENU] %s\n", s_menu_open ? "open" : "closed");
        return;
    }
    if (!s_menu_open || !s_menu_n)
        return;
    if (key == NV_MENU_UP)
        s_menu_sel = (s_menu_sel + s_menu_n - 1) % s_menu_n;
    else if (key == NV_MENU_DOWN)
        s_menu_sel = (s_menu_sel + 1) % s_menu_n;
    else if (key == NV_MENU_LEFT || key == NV_MENU_RIGHT) {
        NvMenuItem *m = &s_menu[s_menu_sel];
        int v = m->get() + (key == NV_MENU_RIGHT ? 1 : -1);
        if (v < m->min) v = m->max;
        if (v > m->max) v = m->min;
        m->set(v);
        fprintf(stderr, "[MENU] %s = %d\n", m->label, v);
    }
}

static void menu_panel(VkImage dst)
{
    enum { CW = 200, ROW = 11 };
    uint32_t ch, i, white, dim, gold, sel_bg;
    int any_restart = 0;
    char val[40];
    Ov o;

    if (!s_menu_open)
        return;
    menu_init();
    ch = 24 + (uint32_t)s_menu_n * ROW + 22;
    if (!ov_begin(&o, CW, ch, ov_unit()))
        return;
    white = ov_rgb(240, 240, 240); dim = ov_rgb(150, 150, 150);
    gold = ov_rgb(235, 180, 60); sel_bg = ov_rgb(55, 45, 25);
    ov_rect(&o, 0, 0, CW, ch, ov_rgb(150, 150, 150));
    ov_rect(&o, 1, 1, CW - 2, ch - 2, ov_rgb(14, 14, 18));
    ov_text(&o, 6, 6, "COLD FEAR - MOD MENU", gold);
    ov_rect(&o, 6, 16, CW - 12, 1, dim);
    for (i = 0; i < (uint32_t)s_menu_n; i++) {
        NvMenuItem *m = &s_menu[i];
        uint32_t y = 22 + i * ROW, w;
        int v = m->get(), sel = (int)i == s_menu_sel;
        if (sel)
            ov_rect(&o, 3, y - 2, CW - 6, ROW, sel_bg);
        ov_text(&o, 6, y, m->label, sel ? gold : white);
        if (m->restart) {
            ov_text(&o, 6 + 6 * (uint32_t)strlen(m->label), y, "+", dim);
            any_restart = 1;
        }
        if (m->names)
            snprintf(val, sizeof val, "%s", m->names[v - m->min]);
        else
            snprintf(val, sizeof val, "%d", v);
        w = 6 * (uint32_t)strlen(val);
        if (sel) {
            ov_text(&o, CW - 18 - w, y, "<", gold);
            ov_text(&o, CW - 11, y, ">", gold);
        }
        ov_text(&o, CW - 12 - w, y, val, sel ? gold : white);
    }
    {
        uint32_t y = 24 + (uint32_t)s_menu_n * ROW + 2;
        ov_rect(&o, 6, y - 3, CW - 12, 1, dim);
        ov_text(&o, 6, y + 2, any_restart ? "+ AO REINICIAR    B: FECHAR" : "B: FECHAR", dim);
    }
    ov_end(&o, dst, (int32_t)((s_sc_extent.width - o.pw) / 2), (int32_t)((s_sc_extent.height - o.ph) / 2));
}

/* FPS counter (top left): presents counted over each half second. */
static void fps_counter(VkImage dst)
{
    static uint64_t t0;
    static uint32_t frames, shown;
    uint64_t now = ov_now_ns();
    char text[16];
    Ov o;

    if (s_mod_fps < 0)
        menu_init();
    frames++;
    if (!t0)
        t0 = now;
    if (now - t0 >= 500000000ull) {
        shown = (uint32_t)(frames * 1000000000ull / (now - t0));
        frames = 0;
        t0 = now;
    }
    if (s_mod_fps != 1)
        return;
    snprintf(text, sizeof text, "FPS %u", shown);
    if (!ov_begin(&o, 6 * (uint32_t)strlen(text) + 5, 11, ov_unit() > 2 ? ov_unit() - 1 : 2))
        return;
    ov_rect(&o, 0, 0, o.cw, o.ch, ov_rgb(0, 0, 0));
    ov_text(&o, 3, 2, text, shown >= 28 ? ov_rgb(120, 230, 120) : shown >= 20 ? ov_rgb(235, 200, 60)
                                                                               : ov_rgb(235, 90, 70));
    ov_end(&o, dst, 8, 8);
}

static void overlays(VkImage dst)
{
    bright_panel(dst);
    fps_counter(dst);
    menu_panel(dst);
}

static VkImage gamma_apply(VkSurf *s)
{
    static int env = -1;
    VkRenderingAttachmentInfo ca = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    VkRenderingInfo ri = { VK_STRUCTURE_TYPE_RENDERING_INFO };
    VkDescriptorImageInfo im[2];
    VkWriteDescriptorSet wr[2];
    uint32_t gen;

    if (env < 0) {
        const char *e = getenv("NV2A_GAMMA");
        env = !(e && *e == '0');
    }
    if (!env || (s_gamma_identity && !bright_level()) || s_gamma_broken || !s || !s->image)
        return s ? s->image : VK_NULL_HANDLE;
    if (!s_gpipe && !gamma_init()) {
        s_gamma_broken = 1;
        LOGE("gamma pass unavailable\n");
        return s->image;
    }
    if (!s_gout_img || s_gout_w != s->pw || s_gout_h != s->ph) {
        if (s_gout_img)
            garbage_add(s_gout_img, s_gout_view, s_gout_mem);
        s_gout_img = VK_NULL_HANDLE;
        if (!make_image(s->pw, s->ph, VK_FORMAT_B8G8R8A8_UNORM,
                        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                        VK_IMAGE_ASPECT_COLOR_BIT, &s_gout_img, &s_gout_view, &s_gout_mem))
            return s->image;
        s_gout_w = s->pw;
        s_gout_h = s->ph;
    }
    gen = __atomic_load_n(&s_gamma_gen, __ATOMIC_ACQUIRE);
    if (gen != s_gamma_seen) {
        uint32_t *dst, i;
        VkDeviceSize at = ring_alloc(1024, 16, (void **)&dst);
        VkBufferImageCopy rg;
        if (dst) {
            int lv = bright_level();
            for (i = 0; i < 256; i++) {
                /* An identity ramp may never have been written. */
                uint32_t r = s_gamma_identity ? i : s_gamma_ramp[i];
                uint32_t g = s_gamma_identity ? i : s_gamma_ramp[256 + i];
                uint32_t b = s_gamma_identity ? i : s_gamma_ramp[512 + i];
                dst[i] = 0xFF000000u | bright_apply(r, lv) << 16 | bright_apply(g, lv) << 8
                       | bright_apply(b, lv);
            }
            memset(&rg, 0, sizeof rg);
            rg.bufferOffset = at;
            rg.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            rg.imageSubresource.layerCount = 1;
            rg.imageExtent.width = 256;
            rg.imageExtent.height = 1;
            rg.imageExtent.depth = 1;
            vkCmdCopyBufferToImage(s_cb, s_f->ring, s_glut_img, VK_IMAGE_LAYOUT_GENERAL, 1, &rg);
            s_gamma_seen = gen;
        }
    }
    barrier_all();
    ca.imageView = s_gout_view;
    ca.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    ca.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    ri.renderArea.extent.width = s->pw;
    ri.renderArea.extent.height = s->ph;
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &ca;
    p_begin_rendering(s_cb, &ri);
    set_viewport(s->pw, s->ph);
    vkCmdBindPipeline(s_cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s_gpipe);
    memset(im, 0, sizeof im);
    memset(wr, 0, sizeof wr);
    im[0].imageView = s->view;
    im[0].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    im[0].sampler = sampler_get(0x30303);              /* clamp, nearest */
    im[1].imageView = s_glut_view;
    im[1].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    im[1].sampler = sampler_get(0x00303);              /* clamp, linear */
    for (gen = 0; gen < 2; gen++) {
        wr[gen].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        wr[gen].dstBinding = gen;
        wr[gen].descriptorCount = 1;
        wr[gen].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        wr[gen].pImageInfo = &im[gen];
    }
    p_push_desc(s_cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s_glayout, 0, 2, wr);
    vkCmdDraw(s_cb, 3, 1, 0, 0);
    p_end_rendering(s_cb);
    return s_gout_img;
}

static void vk_flip(uint32_t completed_color_va)
{
    static int dump_every = -1;
    static char dump_prefix[256];
    VkSurf *s;
    uint32_t idx = 0;
    int have_image = 0;
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    VkPipelineStageFlags ws = VK_PIPELINE_STAGE_TRANSFER_BIT;

    if (!ready())
        return;
    uint32_t cva = nv2a_clean_va(completed_color_va);
    extern uint32_t xbox_GetDisplayFramebuffer(uint32_t *pitch);
    uint32_t fb_pitch = 0;
    uint32_t fb_va = cva ? cva : xbox_GetDisplayFramebuffer(&fb_pitch);
    if (!fb_pitch) fb_pitch = 2560;

    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    const volatile uint32_t *pvideo = (const volatile uint32_t *)(mem + 0xFD008000u);
    uint32_t ov_stop = pvideo[0x704 / 4];
    uint32_t ov_buf  = pvideo[0x700 / 4];
    uint32_t ov_off  = pvideo[0x920 / 4];
    uint32_t ov_szin = pvideo[0x928 / 4];
    /* PVIDEO overlay is active when:
     * 1) Hardware STOP register (0x704) is NOT set
     * 2) ov_buf (0x700) has buffer active (bit 0)
     * 3) ov_off (0x920) is non-zero
     * 4) ov_szin (0x928) is valid (not 0 or 0xFFFFFFFF)
     * 5) Not in 3D rendering (video frames have 1-4 draws for clear/letterbox, 3D scenes have 200+) */
    int overlay_active = ((ov_stop & 1) == 0) && (ov_buf & 1) && (ov_off != 0) &&
                         (ov_szin != 0) && (ov_szin != 0xFFFFFFFFu) && (s_frame_draws <= 10);

    VkSurf *vs = NULL;
    if (overlay_active) {
        if (fb_va && fb_pitch >= 640 * 4) {
            vs = surf_get(fb_va, 640, 480, 1, 1);
        }
        if (!vs) {
            vs = s_last ? s_last : s_display_surf;
            if (!vs) vs = surf_get(0x80670000u, 640, 480, 1, 1);
        }
    }

    if (overlay_active && vs && vs->image) {
        /* NV2A Hardware Video Overlay (PVIDEO): Cold Fear XMV movie cutscenes
         * decode YUY2 frames into contiguous memory and activate the hardware overlay.
         * Convert YUY2 -> BGRA32 and copy into the Vulkan display surface. */
        uint32_t ov_res_va = pvideo_resolve_va(ov_off);
        const uint8_t *src_yuy2 = mem + ov_res_va;

        uint32_t size_in   = pvideo[0x928 / 4];
        uint32_t point_out = pvideo[0x948 / 4];
        uint32_t size_out  = pvideo[0x950 / 4];
        uint32_t format    = pvideo[0x958 / 4];

        uint32_t src_w = size_in & 0x7FF;
        uint32_t src_h = (size_in >> 16) & 0x7FF;
        uint32_t pitch = format & 0x1FFF;
        uint32_t dst_x = point_out & 0xFFF;
        uint32_t dst_y = (point_out >> 16) & 0xFFF;
        uint32_t dst_w = size_out & 0xFFF;
        uint32_t dst_h = (size_out >> 16) & 0xFFF;

        if (!src_w || src_w > 1920) src_w = 640;
        if (!src_h || src_h > 1080) src_h = 480;
        if (!pitch || pitch < src_w * 2) pitch = src_w * 2;
        if (!dst_w || dst_w > 1920) dst_w = 640;
        if (!dst_h || dst_h > 1080) dst_h = 480;

        uint32_t w = 640, h = 480;
        uint32_t bytes = w * h * 4;
        void *dst = NULL;
        VkDeviceSize at = ring_alloc(bytes, 16, &dst);
        if (dst) {
            uint32_t *out_pixels = (uint32_t *)dst;
            if (dst_x == 0 && dst_y == 0 && dst_w == w && dst_h == h && src_w == w && src_h == h) {
                for (uint32_t y = 0; y < h; y++) {
                    const uint8_t *line = src_yuy2 + y * pitch;
                    uint32_t *out_row = out_pixels + y * w;
                    for (uint32_t x = 0; x < w; x += 2) {
                        const uint8_t *p = line + x * 2;
                        out_row[x]     = yuy2_to_bgra(p[0], p[1], p[3]);
                        out_row[x + 1] = yuy2_to_bgra(p[2], p[1], p[3]);
                    }
                }
            } else {
                memset(out_pixels, 0, bytes);
                for (uint32_t dy = 0; dy < dst_h; dy++) {
                    uint32_t py = dst_y + dy;
                    if (py >= h) break;
                    uint32_t sy = (dy * src_h) / dst_h;
                    if (sy >= src_h) sy = src_h - 1;
                    const uint8_t *line = src_yuy2 + sy * pitch;
                    uint32_t *out_row = out_pixels + py * w;
                    for (uint32_t dx = 0; dx < dst_w; dx++) {
                        uint32_t px = dst_x + dx;
                        if (px >= w) break;
                        uint32_t sx = (dx * src_w) / dst_w;
                        if (sx >= src_w) sx = src_w - 1;
                        const uint8_t *pair = line + (sx & ~1u) * 2;
                        int y_val = (sx & 1u) ? pair[2] : pair[0];
                        out_row[px] = yuy2_to_bgra(y_val, pair[1], pair[3]);
                    }
                }
            }

            end_rendering();
            barrier_all();
            VkBufferImageCopy rg;
            memset(&rg, 0, sizeof rg);
            rg.bufferOffset = at;
            rg.bufferRowLength = w;
            rg.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            rg.imageSubresource.layerCount = 1;
            rg.imageExtent.width = w;
            rg.imageExtent.height = h;
            rg.imageExtent.depth = 1;
            if (vs->pw == w && vs->ph == h) {
                vkCmdCopyBufferToImage(s_cb, s_f->ring, vs->image, VK_IMAGE_LAYOUT_GENERAL, 1, &rg);
            } else {
                /* A scaled surface (RECOMP_GL_SCALE): the 640x480 frame goes
                 * through an image of its own and is stretched over the
                 * whole surface. Copied straight in, it filled only the
                 * top-left 640x480 of the stored pixels -- the movies
                 * stopped being full screen. */
                static VkImage ov_img;
                static VkImageView ov_view;
                static VkDeviceMemory ov_mem;
                VkImageBlit bl;
                VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
                if (!ov_img && !make_image(w, h, VK_FORMAT_B8G8R8A8_UNORM,
                                           VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                           VK_IMAGE_ASPECT_COLOR_BIT, &ov_img, &ov_view, &ov_mem))
                    ov_img = VK_NULL_HANDLE;
                if (ov_img) {
                    vkCmdCopyBufferToImage(s_cb, s_f->ring, ov_img, VK_IMAGE_LAYOUT_GENERAL, 1, &rg);
                    mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                    mb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
                    vkCmdPipelineBarrier(s_cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                         0, 1, &mb, 0, NULL, 0, NULL);
                    memset(&bl, 0, sizeof bl);
                    bl.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                    bl.srcSubresource.layerCount = 1;
                    bl.srcOffsets[1].x = (int32_t)w;
                    bl.srcOffsets[1].y = (int32_t)h;
                    bl.srcOffsets[1].z = 1;
                    bl.dstSubresource = bl.srcSubresource;
                    bl.dstOffsets[1].x = (int32_t)to_stored(w, vs->pw, vs->w);
                    bl.dstOffsets[1].y = (int32_t)to_stored(h, vs->ph, vs->h);
                    bl.dstOffsets[1].z = 1;
                    vkCmdBlitImage(s_cb, ov_img, VK_IMAGE_LAYOUT_GENERAL, vs->image,
                                   VK_IMAGE_LAYOUT_GENERAL, 1, &bl, VK_FILTER_LINEAR);
                }
            }
            vs->dirty = 1;
            s_last = vs;

            static unsigned s_ov_count = 0;
            if (s_ov_count++ < 30 || (s_ov_count % 120) == 0) {
                fprintf(stderr, "[PVIDEO-OVERLAY] frame=%u va=%08X -> %08X src=%ux%u pitch=%u dst=%ux%u+%u+%u\n",
                        s_frame, ov_off, ov_res_va, src_w, src_h, pitch, dst_w, dst_h, dst_x, dst_y);
            }
        }
    }
    s_frame_draws = 0;

    /* Present the completed surface (cva) if available, falling back to s_last / s_display_surf */
    s = NULL;
    if (overlay_active && vs && vs->image) {
        s = vs;
    } else if (cva && (s = surf_find(cva)) && s->image && s->w >= 640 && s->h >= 400) {
        /* found cva */
    } else if (s_last && s_last->image && s_last->w >= 640 && s_last->h >= 400) {
        s = s_last;
    } else if (s_display_surf && s_display_surf->image && s_display_surf->w >= 640 && s_display_surf->h >= 400) {
        s = s_display_surf;
    }
    if (s && !s->image)
        s = NULL;
    s_frame++;
    {
        static unsigned s_flip_diag_count = 0;
        if (s_flip_diag_count++ < 25 || (s_flip_diag_count % 120) == 0) {
            fprintf(stderr, "[VK-FLIP] frame=%u va=%08X surf=%p (%ux%u, s_va=%08X) img=%p\n",
                    s_frame, completed_color_va, (void*)s, s ? s->w : 0, s ? s->h : 0, s ? s->va : 0, s ? (void*)s->image : NULL);
        }
    }
    if (s_inst_alt)
        s_inst_on = s_frame & 1;
    {
        static int st = -1;
        static uint32_t st_frame;
        if (st < 0) {
            const char *e = getenv("RECOMP_DRAW_STATS");
            st = e && *e == '1';
        }
        if (st && s_frame - st_frame >= 300) {
            fprintf(stderr, "[vk] per frame: %u draws -> %u draw calls (%u instances appended)\n",
                    s_st_draws / (s_frame - st_frame), s_st_calls / (s_frame - st_frame),
                    s_st_inst / (s_frame - st_frame));
            s_st_draws = s_st_calls = s_st_inst = 0;
            st_frame = s_frame;
        }
    }
    {
        static uint64_t last_ns, window_ns;
        static int lines;
        uint64_t now = hz_now();
        if (last_ns) {
            uint64_t dt = now - last_ns;
            if (now - window_ns > 10000000000ull) { window_ns = now; lines = 0; }
            if (dt > 50000000ull && lines++ < 30)
                fprintf(stderr, "[hitch] frame %u: %.0f ms -- %u programs compiled (%.0f ms),"
                        " %u textures %.1f MB (%.0f ms), %u draws\n", s_frame, dt / 1e6,
                        s_hz_progs, s_hz_prog_ns / 1e6, s_hz_texs,
                        s_hz_tex_bytes / 1048576.0, s_hz_tex_ns / 1e6, s_hz_draws);
        }
        last_ns = now;
        s_hz_progs = s_hz_texs = s_hz_draws = 0;
        s_hz_prog_ns = s_hz_tex_ns = s_hz_tex_bytes = 0;
    }
    {
        static int on = -1;
        static uint32_t last_frame;
        static struct timespec last;
        struct timespec now;
        if (on < 0) {
            const char *e = getenv("RECOMP_FPS_LOG");
            on = e && *e == '1';
            clock_gettime(CLOCK_MONOTONIC, &last);
        }
        if (on) {
            double dt;
            clock_gettime(CLOCK_MONOTONIC, &now);
            dt = (double)(now.tv_sec - last.tv_sec) + (now.tv_nsec - last.tv_nsec) / 1e9;
            if (dt >= 10.0) {
                extern void xbox_vblank_report(double, char *, size_t);
                char vb[128];
                xbox_vblank_report(dt, vb, sizeof vb);
                fprintf(stderr, "[fps] %.1f fps (frame %u), %s\n",
                        (s_frame - last_frame) / dt, s_frame, vb);
                last_frame = s_frame;
                last = now;
            }
        }
    }
    if (dump_every < 0) {
        const char *dd = getenv("RECOMP_GL_DUMP");
        dump_every = 0;
        if (dd && *dd) {
            const char *comma = strchr(dd, ',');
            size_t nn = comma ? (size_t)(comma - dd) : strlen(dd);
            if (nn >= sizeof dump_prefix) nn = sizeof dump_prefix - 1;
            memcpy(dump_prefix, dd, nn);
            dump_prefix[nn] = 0;
            dump_every = comma ? atoi(comma + 1) : 60;
            if (dump_every <= 0) dump_every = 60;
        }
    }
    end_rendering();
    s_rt = NULL;
    s_rt_depth = NULL;
    if (s && s_drew_any && dump_every && ((s_frame % (uint32_t)dump_every) == 0
                                          || (s_inst_alt && s_frame % (uint32_t)dump_every == 1))) {
        char path[320];
        snprintf(path, sizeof path, "%s%05u.bmp", dump_prefix, s_frame);
        dump_surface(s, path);
    }
    barrier_all();

    VT("flip %u: surface %08X drew %d, acquiring\n", s_frame, s ? s->va : 0, s_drew_any);
    if (!s_headless) {
        VkResult rr = vkAcquireNextImageKHR(s_dev, s_swapchain, UINT64_MAX, s_f->acquired,
                                            VK_NULL_HANDLE, &idx);
        if (rr == VK_ERROR_OUT_OF_DATE_KHR) {
            vkDeviceWaitIdle(s_dev);
            create_swapchain();
        } else if (rr == VK_SUCCESS || rr == VK_SUBOPTIMAL_KHR) {
            VkImage dst = s_sc_images[idx];
            VT("flip %u: acquired image %u\n", s_frame, idx);
            VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
            VkImageSubresourceRange rg = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            VkClearColorValue black = { { 0, 0, 0, 1 } };
            have_image = 1;
            b.srcAccessMask = 0;
            b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = dst;
            b.subresourceRange = rg;
            vkCmdPipelineBarrier(s_cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 0, 0, NULL, 0, NULL, 1, &b);
            vkCmdClearColorImage(s_cb, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &rg);
            VkImage src_img = VK_NULL_HANDLE;
            if (s && s->image) {
                end_rendering();
                src_img = gamma_apply(s);          /* the title's gamma ramp */
            }
            if (s && s->image) {
                /* The logical aspect (16:9 for a widescreen title),
                 * letterboxed. Rows are top-first on both sides. */
                float lw = (float)s->w / (float)s->aa_sx, lh = (float)s->h / (float)s->aa_sy;
                float ww = (float)s_sc_extent.width, wh = (float)s_sc_extent.height, scale;
                VkImageBlit bl;
                int dw, dh, dx, dy;
                VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
                if (xbox_video_widescreen() && lw < lh * 1.5f)
                    lw = lh * 16.0f / 9.0f;
                scale = ww / lw < wh / lh ? ww / lw : wh / lh;
                dw = (int)(lw * scale); dh = (int)(lh * scale);
                dx = ((int)ww - dw) / 2; dy = ((int)wh - dh) / 2;
                mb.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
                mb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
                vkCmdPipelineBarrier(s_cb,
                                     VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                                     VK_PIPELINE_STAGE_TRANSFER_BIT,
                                     0, 1, &mb, 0, NULL, 0, NULL);
                memset(&bl, 0, sizeof bl);
                bl.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                bl.srcSubresource.layerCount = 1;
                bl.srcOffsets[1].x = (int32_t)s->pw;
                bl.srcOffsets[1].y = (int32_t)s->ph;
                bl.srcOffsets[1].z = 1;
                bl.dstSubresource = bl.srcSubresource;
                bl.dstOffsets[0].x = dx;
                bl.dstOffsets[0].y = dy;
                bl.dstOffsets[1].x = dx + dw;
                bl.dstOffsets[1].y = dy + dh;
                bl.dstOffsets[1].z = 1;
                vkCmdBlitImage(s_cb, src_img, VK_IMAGE_LAYOUT_GENERAL, dst,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl, VK_FILTER_LINEAR);
            }
            overlays(dst);
            b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            b.dstAccessMask = 0;
            b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            vkCmdPipelineBarrier(s_cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1, &b);
        }
    }
    VT("flip %u: submit (image %d)\n", s_frame, have_image);
    vkEndCommandBuffer(s_cb);
    si.commandBufferCount = 1;
    si.pCommandBuffers = &s_cb;
    if (have_image) {
        si.waitSemaphoreCount = 1;
        si.pWaitSemaphores = &s_f->acquired;
        si.pWaitDstStageMask = &ws;
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &s_f->rendered;
    }
    if (vkQueueSubmit(s_queue, 1, &si, s_f->fence) != VK_SUCCESS)
        LOGE("vkQueueSubmit failed\n");
    s_f->submitted = 1;
    if (have_image) {
        VkPresentInfoKHR pi = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
        VkResult pr;
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores = &s_f->rendered;
        pi.swapchainCount = 1;
        pi.pSwapchains = &s_swapchain;
        pi.pImageIndices = &idx;
        VT("flip %u: vkQueuePresentKHR\n", s_frame);
        pr = vkQueuePresentKHR(s_queue, &pi);
        if (pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_SUBOPTIMAL_KHR) {
            vkDeviceWaitIdle(s_dev);
            create_swapchain();
        }
    }
    {
        static uint64_t last_save;
        uint64_t now = hz_now();
        if (s_pipes_unsaved && now - last_save > 20000000000ull) {
            last_save = now;
            pcache_save();
        }
    }
    VT("flip %u: presented, waiting for frame slot\n", s_frame);
    s_fi = (s_fi + 1) % VK_FRAMES;
    frame_begin();
    VT("flip %u: next frame recording\n", s_frame);
    s_dyn_valid = 0;
#if !defined(__SWITCH__)
    if (s_win) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) { }
    }
#endif
}

static void vk_draw(const Nv2aSurface *s, const Nv2aBatch *b)
{
    (void)s; (void)b;                        /* draw_raw handles every batch */
}

static const Nv2aBackend s_backend = {
    vk_clear,
    vk_draw,
    vk_flip,
    vk_draw_raw,
    NV2A_BACKEND_RAW_DIRECT,
};

void nv2a_vk_install(void)
{
    nv2a_backend_register(&s_backend);
    fprintf(stderr, "[BOOT] NV2A Vulkan renderer registered\n");
}
