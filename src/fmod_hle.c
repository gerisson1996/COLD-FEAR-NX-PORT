/*
 * fmod_hle.c -- Cold Fear's music and ambience streams, played by the host.
 *
 * The game streams its music and ambiences (data/music/*.FSB) through FMOD
 * 3 as IMA ADPCM with 2-8 channels in FMOD's own layout. Through the
 * recompiled FMOD and the emulated APU they come out as saturated static,
 * a fault not yet found. These wrappers keep FMOD's own bookkeeping (the
 * game sees every return value it always did) but take the sound away from
 * it: the levels the game sets go to zero for FMOD and to this player
 * instead, which decodes the FSB from the SD card and adds it to the host
 * output block (xa2_host_mix_hook).
 *
 * FMOD entry points (stdcall, identified from the XBE):
 *   0x7F250 FSOUND_Stream_Open(name, mode, offset, length) -> stream
 *   0x7F3B0 FSOUND_Stream_PlayEx(channel, stream, dsp, paused) -> channel
 *   0x80190 FSOUND_Stream_GetSubChannel(channel, index) -> channel
 *   0x7FB30 FSOUND_SetSpeakerMix(channel, fl, c, fr, rl, rr, lfe), 0..255
 *   0x80310 / 0x804A0 volume setters (channel, 0..255)
 *   0x7FE50 FSOUND_SetPaused(channel, paused)
 *   0x7F770 FSOUND_StopSound(channel)
 *   0x7CC40 FSOUND_Stream_Close(stream)
 * tools/fix_fmod_hle.py renames each generated sub_X to sub_X_orig.
 *
 * FSB layout (verified offline against ML_Main.FSB / AMB*.FSB): one sample,
 * blocks of 36 x nch bytes, 64 samples per channel (the predictor and 63 nibbles; the last nibble is not played, as vgmstream's FSB_IMA and the MCPX do). Each block is nch int16
 * predictors, nch int16 step indices, then the nibbles interleaved two bytes
 * per channel (FMOD's sub_00082A10 de-interleaves the same way).
 *
 * COLDFEAR_FMOD_HLE=0 turns it off: everything passes straight to FMOD.
 */
#include "recomp_types.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <strings.h>

extern void (*xa2_host_mix_hook)(int16_t *stereo, int frames);

extern void sub_0007F250_orig(void);
extern void sub_0007F3B0_orig(void);
extern void sub_00080190_orig(void);
extern void sub_0007FB30_orig(void);
extern void sub_00080310_orig(void);
extern void sub_000804A0_orig(void);
extern void sub_0007FE50_orig(void);
extern void sub_0007F770_orig(void);
extern void sub_0007CC40_orig(void);

#define HLE_MAX_STREAMS 24
#define HLE_MAX_CH      8
#define HLE_OUT_RATE    48000.0

typedef struct {
    int      used;
    uint32_t guest_stream;           /* FSOUND_STREAM * in guest memory */
    char     path[512];
    int      loop;
    FILE    *f;
    uint8_t *mem;                    /* FSOUND_LOADMEMORY: host copy */
    size_t   mem_size;
    int      nch, rate;
    long     data_off;
    uint32_t nblocks, blk;           /* blocks in the file, next one to read */
    uint32_t loop_blk;               /* block the loop restarts at */
    int16_t  pcm[HLE_MAX_CH][65];
    int      pos, have;              /* read position / samples in pcm */
    float    a[HLE_MAX_CH], b[HLE_MAX_CH];
    double   frac, step;
    int      primed;
    /* control, set by the game through the wrappers */
    int      playing, paused;
    uint32_t channel;                /* FMOD channel PlayEx returned */
    uint32_t sub[HLE_MAX_CH];        /* sub-channel per FSB channel */
    float    gl[HLE_MAX_CH], gr[HLE_MAX_CH];
    float    vol;
} HleStream;

static HleStream       s_st[HLE_MAX_STREAMS];
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static char            s_game_dir[256] = "sdmc:/switch/coldfear/game";
static int             s_on = 1;

static const int16_t s_step[89] = {
    7,8,9,10,11,12,13,14,16,17,19,21,23,25,28,31,34,37,41,45,50,55,60,66,73,80,88,97,107,118,130,143,
    157,173,190,209,230,253,279,307,337,371,408,449,494,544,598,658,724,796,876,963,1060,1166,1282,1411,
    1552,1707,1878,2066,2272,2499,2749,3024,3327,3660,4026,4428,4871,5358,5894,6484,7132,7845,8630,9493,
    10442,11487,12635,13899,15289,16818,18500,20350,22385,24623,27086,29794,32767
};
static const int8_t s_idx[8] = { -1, -1, -1, -1, 2, 4, 6, 8 };

/* ── FSB decoding (audio thread, under s_lock) ─────────────────────── */

static int read_block(HleStream *s)
{
    uint8_t buf[36 * HLE_MAX_CH];
    int n = s->nch, c, j, k;

    if (s->blk >= s->nblocks) {
        if (!s->loop)
            return 0;
        s->blk = s->loop_blk;
    }
    if (s->mem) {
        size_t at = (size_t)s->data_off + (size_t)s->blk * 36 * n;
        if (at + (size_t)36 * n > s->mem_size)
            return 0;
        memcpy(buf, s->mem + at, (size_t)36 * n);
    } else if (fseek(s->f, s->data_off + (long)s->blk * 36 * n, SEEK_SET) != 0
            || fread(buf, 1, (size_t)36 * n, s->f) != (size_t)36 * n)
        return 0;
    s->blk++;
    for (c = 0; c < n; c++) {
        int p   = (int16_t)(buf[2 * c] | (buf[2 * c + 1] << 8));
        int idx = (int16_t)(buf[2 * n + 2 * c] | (buf[2 * n + 2 * c + 1] << 8));
        int out = 0;
        if (idx < 0) idx = 0;
        if (idx > 88) idx = 88;
        s->pcm[c][out++] = (int16_t)p;
        for (j = 0; j < 16; j++) {
            const uint8_t *q = buf + 4 * n + (j * n + c) * 2;
            for (k = 0; k < 4; k++) {
                int nib = (k & 1) ? (q[k >> 1] >> 4) : (q[k >> 1] & 15);
                int st = s_step[idx], d = st >> 3;
                if (nib & 1) d += st >> 2;
                if (nib & 2) d += st >> 1;
                if (nib & 4) d += st;
                if (nib & 8) d = -d;
                p += d;
                if (p > 32767) p = 32767;
                if (p < -32768) p = -32768;
                s->pcm[c][out++] = (int16_t)p;
                idx += s_idx[nib & 7];
                if (idx < 0) idx = 0;
                if (idx > 88) idx = 88;
            }
        }
    }
    s->pos = 0;
    s->have = 64;                   /* 65th (last nibble) dropped, as the hardware does */
    return 1;
}

/* One source frame into b[] (a[] keeps the previous). 0 at the end. */
static int next_frame(HleStream *s)
{
    int c;
    if (s->pos >= s->have && !read_block(s))
        return 0;
    for (c = 0; c < s->nch; c++) {
        s->a[c] = s->b[c];
        s->b[c] = s->pcm[c][s->pos] / 32768.0f;
    }
    s->pos++;
    return 1;
}

static void mix_stream(HleStream *s, float *acc, int frames)
{
    int i, c;
    if (!s->primed) {
        if (!next_frame(s) || !next_frame(s)) { s->playing = 0; return; }
        s->frac = 0.0;
        s->primed = 1;
    }
    /* The game's speaker mix routes 4-8 channels to the speakers at up to
     * full level each; summed to stereo that is several times full scale.
     * Keep the louder side at unity. */
    float sl = 0.0f, sr = 0.0f, norm;
    for (c = 0; c < s->nch; c++) { sl += s->gl[c]; sr += s->gr[c]; }
    norm = (sl > sr ? sl : sr);
    norm = norm > 1.0f ? 1.0f / norm : 1.0f;
    for (i = 0; i < frames; i++) {
        float l = 0.0f, r = 0.0f, t = (float)s->frac;
        for (c = 0; c < s->nch; c++) {
            float v = s->a[c] + (s->b[c] - s->a[c]) * t;
            l += v * s->gl[c];
            r += v * s->gr[c];
        }
        acc[2 * i]     += l * s->vol * norm;
        acc[2 * i + 1] += r * s->vol * norm;
        s->frac += s->step;
        while (s->frac >= 1.0) {
            s->frac -= 1.0;
            if (!next_frame(s)) { s->playing = 0; return; }
        }
    }
}

static void host_mix(int16_t *stereo, int frames)
{
    static float acc[2 * 4096];
    int i, any = 0;

    if (frames > 4096) frames = 4096;
    memset(acc, 0, sizeof(float) * 2 * frames);
    pthread_mutex_lock(&s_lock);
    for (i = 0; i < HLE_MAX_STREAMS; i++)
        if (s_st[i].used && s_st[i].playing && !s_st[i].paused && (s_st[i].f || s_st[i].mem)) {
            mix_stream(&s_st[i], acc, frames);
            any = 1;
        }
    pthread_mutex_unlock(&s_lock);
    if (!any)
        return;
    for (i = 0; i < 2 * frames; i++) {
        int v = stereo[i] + (int)(acc[i] * 32767.0f);
        stereo[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
    }
}

/* ── Stream table (game thread, under s_lock) ──────────────────────── */

static HleStream *by_stream(uint32_t gs)
{
    int i;
    for (i = 0; i < HLE_MAX_STREAMS; i++)
        if (s_st[i].used && s_st[i].guest_stream == gs)
            return &s_st[i];
    return NULL;
}

/* The stream a channel handle belongs to (the PlayEx channel or one of its
 * sub-channels); *sub_index = the FSB channel, or -1 for the main channel. */
static HleStream *by_channel(uint32_t ch, int *sub_index)
{
    int i, c;
    if (ch == 0xFFFFFFFFu)
        return NULL;
    for (i = 0; i < HLE_MAX_STREAMS; i++) {
        HleStream *s = &s_st[i];
        if (!s->used || !s->channel)
            continue;
        if (s->channel == ch) { *sub_index = -1; return s; }
        for (c = 0; c < s->nch; c++)
            if (s->sub[c] == ch) { *sub_index = c; return s; }
    }
    return NULL;
}

static void release(HleStream *s)
{
    if (s->f) fclose(s->f);
    free(s->mem);
    memset(s, 0, sizeof *s);
}

static void default_gains(HleStream *s)
{
    int c;
    float g = s->nch > 2 ? 2.0f / (float)s->nch : 1.0f;
    for (c = 0; c < s->nch; c++) {
        if (s->nch == 1) { s->gl[c] = s->gr[c] = 1.0f; continue; }
        s->gl[c] = (c & 1) ? 0.0f : g;
        s->gr[c] = (c & 1) ? g : 0.0f;
    }
}

static void host_path(uint32_t name_va, char *out, size_t cap, char *guest, size_t gcap)
{
    size_t i;
    const char *p;
    for (i = 0; i + 1 < gcap; i++) {
        guest[i] = (char)MEM8(name_va + i);
        if (!guest[i]) break;
    }
    guest[i] = 0;
    p = guest;
    if (!strncasecmp(p, "\\Device\\CdRom0\\", 15)) p += 15;
    else if ((p[0] == 'd' || p[0] == 'D') && p[1] == ':') p += 2;
    while (*p == '\\' || *p == '/') p++;
    snprintf(out, cap, "%s/%s", s_game_dir, p);
    for (i = strlen(s_game_dir); out[i]; i++)
        if (out[i] == '\\') out[i] = '/';
}

static void on_open(uint32_t gstream, uint32_t name_va, uint32_t mode, uint32_t length)
{
    char path[512], guest[260];
    uint8_t h[0x18 + 80];
    HleStream *s = NULL;
    FILE *f = NULL;
    uint8_t *mem = NULL;
    size_t mem_size = 0;
    int i, nch, rate;
    uint32_t shdr, data, len, loop_start, smode;

    if (!gstream || !name_va)
        return;
    if (mode & 0x8000u) {
        /* FSOUND_LOADMEMORY: "name" is the FSB image itself, length its
         * size. Copied below, since the game may free its buffer. */
        if (length < sizeof h || length > 64u * 1024 * 1024)
            return;
        memcpy(h, (const void *)XBOX_PTR(name_va), sizeof h);
        snprintf(guest, sizeof guest, "memory@%08X", name_va);
        snprintf(path, sizeof path, "memory@%08X (%u bytes)", name_va, length);
    } else {
        host_path(name_va, path, sizeof path, guest, sizeof guest);
        f = fopen(path, "rb");
        if (!f) {
            fprintf(stderr, "[FMODHLE] open \"%s\": %s not found, left to FMOD\n", guest, path);
            return;
        }
        if (fread(h, 1, sizeof h, f) != sizeof h) { fclose(f); return; }
    }
    if (memcmp(h, "FSB3", 4) || *(int32_t *)(h + 4) != 1) {
        if (f) fclose(f);
        return;                                  /* not a one-sample FSB3 */
    }
    shdr = *(uint32_t *)(h + 8);
    data = *(uint32_t *)(h + 0x0C);
    len = *(uint32_t *)(h + 0x18 + 32);
    loop_start = *(uint32_t *)(h + 0x18 + 40);
    smode = *(uint32_t *)(h + 0x18 + 48);
    rate = *(int32_t *)(h + 0x18 + 52);
    nch = *(uint16_t *)(h + 0x18 + 62);
    if (!(smode & 0x400000u) || nch < 1 || nch > HLE_MAX_CH || rate <= 0) {
        if (f) fclose(f);
        return;                                  /* not IMA ADPCM: FMOD keeps it */
    }
    if (f) {
        setvbuf(f, NULL, _IOFBF, 64 * 1024);
    } else {
        mem_size = length;
        mem = (uint8_t *)malloc(mem_size);
        if (!mem) return;
        memcpy(mem, (const void *)XBOX_PTR(name_va), mem_size);
    }

    pthread_mutex_lock(&s_lock);
    s = by_stream(gstream);
    if (s) release(s);
    for (i = 0; i < HLE_MAX_STREAMS && !s; i++)
        if (!s_st[i].used) s = &s_st[i];
    if (s) {
        s->used = 1;
        s->guest_stream = gstream;
        snprintf(s->path, sizeof s->path, "%s", path);
        s->f = f;
        s->mem = mem;
        s->mem_size = mem_size;
        s->nch = nch;
        s->rate = rate;
        s->data_off = 0x18 + (long)shdr;
        s->nblocks = data / (36u * (uint32_t)nch);
        s->loop = (mode & 2u) != 0 || (smode & 2u) != 0;
        s->loop_blk = loop_start / 64u;
        s->step = (double)rate / HLE_OUT_RATE;
        s->vol = 1.0f;
        default_gains(s);
        f = NULL;
        mem = NULL;
    }
    pthread_mutex_unlock(&s_lock);
    if (f) fclose(f);
    free(mem);
    fprintf(stderr, "[FMODHLE] open stream %08X \"%s\" -> %s: %d ch %d Hz %u samples%s%s\n",
            gstream, guest, path, nch, rate, len, (mode & 2u) ? " loop" : "",
            s ? "" : " (table full, left to FMOD)");
}

/* ── Wrappers (guest thread; registers are the globals here) ───────── */

void sub_0007F250(void)          /* Stream_Open(name, mode, offset, length) */
{
    uint32_t name = MEM32(g_esp + 4), mode = MEM32(g_esp + 8);
    uint32_t length = MEM32(g_esp + 16);
    sub_0007F250_orig();
    if (s_on)
        on_open(g_eax, name, mode, length);
}

/* Silence an FMOD channel by calling FMOD's own volume setters with 0, as
 * a guest call (stdcall, 2 args: push args and a return slot; the callee's
 * `ret 8` pops all three). Zero speaker levels alone did not silence the
 * hardware voices. eax/ecx/edx are the caller's return state: kept. */
static void guest_mute(uint32_t ch)
{
    uint32_t eax = g_eax, ecx = g_ecx, edx = g_edx;
    void (*const setters[2])(void) = { sub_00080310_orig, sub_000804A0_orig };
    int i;
    for (i = 0; i < 2; i++) {
        PUSH32(g_esp, 0);
        PUSH32(g_esp, ch);
        PUSH32(g_esp, 0);
        setters[i]();
    }
    g_eax = eax; g_ecx = ecx; g_edx = edx;
}

void sub_0007F3B0(void)          /* Stream_PlayEx(channel, stream, dsp, paused) */
{
    uint32_t gs = MEM32(g_esp + 8);
    int paused = MEM8(g_esp + 16) != 0;
    HleStream *s;
    uint32_t mute = 0;
    sub_0007F3B0_orig();
    if (!s_on) return;
    pthread_mutex_lock(&s_lock);
    s = by_stream(gs);
    if (s && g_eax != 0xFFFFFFFFu) {
        mute = g_eax;
        s->channel = g_eax;
        memset(s->sub, 0, sizeof s->sub);
        s->paused = paused;
        s->playing = 1;
        s->blk = 0;
        s->pos = s->have = 0;
        s->primed = 0;
        fprintf(stderr, "[FMODHLE] play %s on channel %08X%s\n", s->path, g_eax,
                paused ? " (paused)" : "");
    }
    pthread_mutex_unlock(&s_lock);
    if (mute)
        guest_mute(mute);
}

void sub_00080190(void)          /* Stream_GetSubChannel(channel, index) */
{
    uint32_t ch = MEM32(g_esp + 4), idx = MEM32(g_esp + 8);
    HleStream *s;
    int sub;
    uint32_t mute = 0;
    sub_00080190_orig();
    if (!s_on) return;
    pthread_mutex_lock(&s_lock);
    s = by_channel(ch, &sub);
    if (s && sub < 0 && idx < (uint32_t)s->nch && g_eax != 0xFFFFFFFFu) {
        if (s->sub[idx] != g_eax)
            mute = g_eax;
        s->sub[idx] = g_eax;
    }
    pthread_mutex_unlock(&s_lock);
    if (mute)
        guest_mute(mute);
}

void sub_0007FB30(void)          /* SetSpeakerMix(channel, fl, c, fr, rl, rr, lfe) */
{
    uint32_t ch = MEM32(g_esp + 4);
    HleStream *s = NULL;
    int sub, k;
    if (s_on) {
        pthread_mutex_lock(&s_lock);
        s = by_channel(ch, &sub);
        if (s) {
            static int logged;
            float lv[6];
            if (logged++ < 24)
                fprintf(stderr, "[FMODHLE] speakermix ch %08X (sub %d) %d %d %d %d %d %d\n", ch, sub,
                        (int)MEM32(g_esp + 8), (int)MEM32(g_esp + 12), (int)MEM32(g_esp + 16),
                        (int)MEM32(g_esp + 20), (int)MEM32(g_esp + 24), (int)MEM32(g_esp + 28));
            for (k = 0; k < 6; k++) {
                int v = (int)MEM32(g_esp + 8 + 4 * k);
                lv[k] = (v < 0 ? 0 : v > 255 ? 255 : v) / 255.0f;
                MEM32(g_esp + 8 + 4 * k) = 0;            /* FMOD plays it silent */
            }
            float l = lv[0] + 0.707f * lv[1] + 0.707f * lv[3];
            float r = lv[2] + 0.707f * lv[1] + 0.707f * lv[4];
            if (sub >= 0) { s->gl[sub] = l; s->gr[sub] = r; }
            else for (k = 0; k < s->nch; k++) { s->gl[k] = l; s->gr[k] = r; }
        }
        pthread_mutex_unlock(&s_lock);
    }
    sub_0007FB30_orig();
}

static void volume_wrapper(void (*orig)(void))
{
    uint32_t ch = MEM32(g_esp + 4);
    HleStream *s;
    int sub;
    if (s_on) {
        pthread_mutex_lock(&s_lock);
        s = by_channel(ch, &sub);
        if (s) {
            int v = (int)MEM32(g_esp + 8);
            if (sub < 0)
                s->vol = (v < 0 ? 0 : v > 255 ? 255 : v) / 255.0f;
            MEM32(g_esp + 8) = 0;
        }
        pthread_mutex_unlock(&s_lock);
    }
    orig();
}

void sub_00080310(void) { volume_wrapper(sub_00080310_orig); }
void sub_000804A0(void) { volume_wrapper(sub_000804A0_orig); }

void sub_0007FE50(void)          /* SetPaused(channel, paused) */
{
    uint32_t ch = MEM32(g_esp + 4);
    int paused = MEM8(g_esp + 8) != 0;
    HleStream *s;
    int sub;
    if (s_on) {
        pthread_mutex_lock(&s_lock);
        s = by_channel(ch, &sub);
        if (s && sub < 0 && s->paused != paused) {
            s->paused = paused;
            fprintf(stderr, "[FMODHLE] %s %s\n", paused ? "pause" : "resume", s->path);
        }
        pthread_mutex_unlock(&s_lock);
    }
    sub_0007FE50_orig();
}

void sub_0007F770(void)          /* StopSound(channel) */
{
    uint32_t ch = MEM32(g_esp + 4);
    HleStream *s;
    int sub;
    if (s_on) {
        pthread_mutex_lock(&s_lock);
        s = by_channel(ch, &sub);
        if (s && sub < 0) {
            s->playing = 0;
            s->channel = 0;
            fprintf(stderr, "[FMODHLE] stop %s\n", s->path);
        }
        pthread_mutex_unlock(&s_lock);
    }
    sub_0007F770_orig();
}

void sub_0007CC40(void)          /* Stream_Close(stream) */
{
    uint32_t gs = MEM32(g_esp + 4);
    HleStream *s;
    if (s_on) {
        pthread_mutex_lock(&s_lock);
        s = by_stream(gs);
        if (s) {
            fprintf(stderr, "[FMODHLE] close %s\n", s->path);
            release(s);
        }
        pthread_mutex_unlock(&s_lock);
    }
    sub_0007CC40_orig();
}

void coldfear_fmod_hle_init(const char *game_dir)
{
    const char *e = getenv("COLDFEAR_FMOD_HLE");
    s_on = !(e && *e == '0');
    if (game_dir && *game_dir)
        snprintf(s_game_dir, sizeof s_game_dir, "%s", game_dir);
    if (s_on)
        xa2_host_mix_hook = host_mix;
    fprintf(stderr, "[FMODHLE] host FSB streaming %s (%s)\n", s_on ? "on" : "off", s_game_dir);
}
