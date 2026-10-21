/**
 * Manual function overrides and ICALL diagnostics
 *
 * This file provides:
 *   - recomp_lookup_manual()  : intercept specific Xbox VAs with hand-written code
 *   - recomp_icall_fail_log() : log when an indirect call target can't be resolved
 *   - ICALL trace ring buffer  : globals used by the RECOMP_ICALL macro
 *
 * The recomp pipeline generates an auto-dispatch table (recomp_lookup) that
 * resolves most function addresses. recomp_lookup_manual() is called FIRST,
 * giving you a chance to override any function with a custom implementation.
 *
 * Common reasons to add manual overrides:
 *   - Trace a function to understand call flow (wrap the generated version)
 *   - Fix a function the lifter translated incorrectly
 *   - Stub out a function that crashes (return early, set eax to a safe value)
 *   - Redirect a function to a native implementation (e.g., skip CRT init)
 *   - Intercept D3D/audio calls for custom rendering or sound
 */

#include <stdio.h>
#include <stdint.h>
#include <stddef.h>

/* ── ICALL trace ring buffer ───────────────────────────────── */

/*
 * These globals are written by the RECOMP_ICALL macro (defined in
 * recomp_types.h) every time an indirect call is dispatched. When a
 * crash occurs, the VEH handler or recomp_icall_fail_log() can dump
 * the last 16 call targets to help you trace what happened.
 *
 * The runtime owns them: xbox_kernel defines all three in
 * src/kernel/xbox_memory_layout.c, and recomp_types.h declares them extern.
 * Declare, do not define -- a definition here as well is a duplicate symbol,
 * and a project copied from this template failed to link on all three:
 *
 *   xbox_memory_layout.obj : error LNK2005: g_icall_count already defined
 *                            in recomp_manual.obj
 */
extern volatile uint32_t g_icall_trace[16];
extern volatile uint32_t g_icall_trace_idx;
extern volatile uint64_t g_icall_count;

typedef void (*recomp_func_t)(void);

/* ── Register state (defined in xbox_memory_layout.c) ──────── */


extern ptrdiff_t g_xbox_mem_offset;

/* ── Manual function overrides ─────────────────────────────── */

/*
 * Return a function pointer to override the given Xbox VA, or NULL
 * to fall through to the auto-generated dispatch table.
 *
 * This is called on every indirect call (RECOMP_ICALL) and every
 * direct call through the dispatch table, so keep it fast. A chain
 * of if-statements on uint32_t compiles to a simple comparison
 * sequence; for large override tables, consider a sorted array
 * with binary search.
 *
 * Examples of common override patterns:
 *
 *   // Trace wrapper: log entry/exit around the generated function
 *   extern void sub_00012345(void);
 *   static void traced_sub_00012345(void) {
 *       fprintf(stderr, "[TRACE] sub_00012345 entered, eax=0x%08X\n", g_eax);
 *       sub_00012345();
 *       fprintf(stderr, "[TRACE] sub_00012345 returned, eax=0x%08X\n", g_eax);
 *   }
 *
 *   // Stub: skip a function entirely (return 0 in eax)
 *   static void stub_00067890(void) {
 *       g_eax = 0;
 *   }
 *
 *   // Fix: replace a broken lifted function with correct C
 *   static void fixed_sub_000ABCDE(void) {
 *       // Read arguments from stack/registers per calling convention
 *       uint32_t arg1 = g_ecx;
 *       uint32_t arg2 = MEM32(g_esp + 4);
 *       // ... correct implementation ...
 *       g_eax = result;
 *   }
 */
#include "recomp_types.h"
#include "input/xinput_xbox.h"

void recomp_manual_XGetDevices(void)
{
    uint32_t dev_type = MEM32(g_esp + 4);
    uint32_t mask = 0;
    if (dev_type == 0x002FF530u) {
        mask = 1u; /* Player 1 connected on port 0 */
        MEM32(dev_type + 0) = mask;
        MEM32(dev_type + 4) = 0;
        MEM32(dev_type + 8) = mask;
    } else if (dev_type >= 0x10000u && dev_type < 0x04000000u) {
        MEM32(dev_type + 0) = 0;
        MEM32(dev_type + 4) = 0;
        MEM32(dev_type + 8) = 0;
    }
    g_eax = mask;
    g_esp += 8; /* ret 4 */
}

void recomp_manual_XGetDeviceChanges(void)
{
    uint32_t dev_type = MEM32(g_esp + 4);
    uint32_t p_ins = MEM32(g_esp + 8);
    uint32_t p_rem = MEM32(g_esp + 12);
    static int s_reported_gamepad = 0;
    uint32_t ins = 0;
    if (dev_type == 0x002FF530u) {
        /* If XGetDevices was already called at boot, MEM32(dev_type + 8) is 1 */
        if (MEM32(dev_type + 8) == 0 && !s_reported_gamepad) {
            ins = 1u;
            MEM32(dev_type + 0) = 1u;
            MEM32(dev_type + 4) = 0u;
            MEM32(dev_type + 8) = 1u;
        }
        s_reported_gamepad = 1;
    }
    if (p_ins) MEM32(p_ins) = ins;
    if (p_rem) MEM32(p_rem) = 0;
    g_eax = ins ? 1u : 0u;
    g_esp += 16; /* ret 12 */
}

void recomp_manual_XInputOpen(void)
{
    uint32_t dev_type = MEM32(g_esp + 4);
    uint32_t port = MEM32(g_esp + 8);
    uint32_t slot = MEM32(g_esp + 12);
    if (dev_type == 0x002FF530u && port == 0 && slot == 0) {
        static int s_logged = 0;
        if (!s_logged) {
            fprintf(stderr, "[XINPUT] XInputOpen(port=0, slot=0) -> 0x00010001\n");
            fflush(stderr);
            s_logged = 1;
        }
        g_eax = 0x00010001u;
    } else {
        g_eax = 0;
    }
    g_esp += 20; /* ret 16 */
}

void recomp_manual_XInputClose(void)
{
    g_eax = 0;
    g_esp += 8; /* ret 4 */
}

void recomp_manual_XInputGetCapabilities(void)
{
    uint32_t handle = MEM32(g_esp + 4);
    uint32_t p_caps = MEM32(g_esp + 8);
    if (handle == 0x00010001u && p_caps) {
        MEM8(p_caps + 0) = 1; /* XINPUT_DEVSUBTYPE_GC_GAMEPAD */
        MEM16(p_caps + 1) = 0;
        for (uint32_t i = 3; i < 25; i++) {
            MEM8(p_caps + i) = 0xFF;
        }
        g_eax = 0; /* ERROR_SUCCESS */
    } else {
        g_eax = 0x48Fu; /* ERROR_DEVICE_NOT_CONNECTED */
    }
    g_esp += 12; /* ret 8 */
}

void recomp_manual_XInputGetState(void)
{
    uint32_t handle = MEM32(g_esp + 4);
    uint32_t p_state = MEM32(g_esp + 8);
    if (handle == 0x00010001u && p_state) {
        XBOX_INPUT_STATE st;
        memset(&st, 0, sizeof(st));
        xbox_InputGetState(0, &st);
        MEM32(p_state + 0)  = st.dwPacketNumber;
        MEM16(p_state + 4)  = st.Gamepad.wButtons;
        for (int i = 0; i < 8; i++) {
            MEM8(p_state + 6 + i) = st.Gamepad.bAnalogButtons[i];
        }
        MEM16(p_state + 14) = (uint16_t)st.Gamepad.sThumbLX;
        MEM16(p_state + 16) = (uint16_t)st.Gamepad.sThumbLY;
        MEM16(p_state + 18) = (uint16_t)st.Gamepad.sThumbRX;
        MEM16(p_state + 20) = (uint16_t)st.Gamepad.sThumbRY;
        g_eax = 0; /* ERROR_SUCCESS */
    } else {
        g_eax = 0x48Fu; /* ERROR_DEVICE_NOT_CONNECTED */
    }
    g_esp += 12; /* ret 8 */
}

void recomp_manual_XInputSetState(void)
{
    uint32_t handle = MEM32(g_esp + 4);
    uint32_t p_fb = MEM32(g_esp + 8);
    if (handle == 0x00010001u && p_fb) {
        XBOX_VIBRATION vib;
        vib.wLeftMotorSpeed  = MEM16(p_fb + 0x42);
        vib.wRightMotorSpeed = MEM16(p_fb + 0x44);
        xbox_InputSetState(0, &vib);
        MEM32(p_fb + 0) = 0; /* Header.dwStatus = ERROR_SUCCESS */
        g_eax = 0; /* ERROR_SUCCESS */
    } else {
        if (p_fb) MEM32(p_fb + 0) = 0x48Fu;
        g_eax = 0x48Fu;
    }
    g_esp += 12; /* ret 8 */
}

void recomp_manual_XInputPoll(void)
{
    g_eax = 0; /* ERROR_SUCCESS */
    g_esp += 8; /* ret 4 */
}

extern void sub_000497B0(void);
extern void sub_00223800(void);
extern void sub_00224010(void);

/* A call from C must still push the x86 return address: the callee's `ret`
 * pops it, and its stack arguments are read from [esp+4] onward. Calling
 * sub_X() bare leaves every argument one slot off and drifts esp by 4. */
static inline void manual_call(uint32_t ret_va, void (*fn)(void))
{
    PUSH32(g_esp, ret_va);
    fn();
}

void recomp_manual_000EC960(void)
{
    /* x86: push ebx/ebp/esi/edi ... pop; callers rely on them surviving. */
    uint32_t s_ebx = g_ebx, s_ebp = g_ebp, s_esi = g_esi, s_edi = g_edi;
    uint32_t ebp = g_ecx;
    uint8_t bl = MEM8(g_esp + 4);
    uint32_t esi = MEM32(ebp + 0x174);
    MEM8(ebp + 0x1A9) = bl;
    MEM8(ebp + 0x221) = bl;

    uint32_t eax = MEM32(esi + 0xF0);
    uint32_t ecx_call = eax ? eax : MEM32(esi + 0xEC);
    uint32_t target_eax = bl ? 0x001FA380u : 0x001FA360u;

    PUSH32(g_esp, 1);
    PUSH32(g_esp, target_eax);
    PUSH32(g_esp, ecx_call);
    manual_call(0x000EC9A5u, sub_000497B0);
    g_esp += 12;

    /* sub_00223800 takes esi (reads [esi+0xDE]) as well as ecx */
    MEM8(esi + 0xDE) = bl;
    g_ecx = MEM32(esi + 0x18);
    g_esi = esi;
    manual_call(0x000EC9B6u, sub_00223800);

    target_eax = bl ? 0x001FA380u : 0x001FA360u;
    esi = MEM32(ebp + 0x174);
    uint32_t ecx2 = MEM32(esi + 0xEC);
    PUSH32(g_esp, 4);
    PUSH32(g_esp, target_eax);
    PUSH32(g_esp, ecx2);
    manual_call(0x000EC9D9u, sub_000497B0);
    g_esp += 12;

    /* sub_00224010 takes edi = [esi+0x18] in a register besides its stack arg */
    g_edi = MEM32(esi + 0x18);
    MEM8(esi + 0xDD) = bl;
    PUSH32(g_esp, esi);
    manual_call(0x000EC9EBu, sub_00224010);

    if (MEM32(ebp + 0x204) == 0x12) {
        esi = MEM32(ebp + 0x174);
        eax = MEM32(esi + 0xF0);
        if (!eax) eax = MEM32(esi + 0xEC);
        PUSH32(g_esp, 1);
        PUSH32(g_esp, 0x001FA360u);
        PUSH32(g_esp, eax);
        manual_call(0x000ECA17u, sub_000497B0);
        g_esp += 12;

        MEM8(esi + 0xDE) = 0;
        g_ecx = MEM32(esi + 0x18);
        g_esi = esi;
        manual_call(0x000ECA29u, sub_00223800);

        esi = MEM32(ebp + 0x174);
        ecx_call = MEM32(esi + 0xEC);
        PUSH32(g_esp, 4);
        PUSH32(g_esp, 0x001FA360u);
        PUSH32(g_esp, ecx_call);
        manual_call(0x000ECA42u, sub_000497B0);
        g_esp += 12;

        g_edi = MEM32(esi + 0x18);
        MEM8(esi + 0xDD) = 0;
        PUSH32(g_esp, esi);
        manual_call(0x000ECA55u, sub_00224010);
    }

    g_ebx = s_ebx; g_ebp = s_ebp; g_esi = s_esi; g_edi = s_edi;
    g_esp += 8; /* ret 4 */
}

extern void sub_001023D0(void);
extern void sub_00147800(void);
extern void sub_000BD3F0(void);
extern void sub_00103110(void);
extern void sub_00103430(void);

static inline void manual_icall_safe(uint32_t target, uint32_t ret_va)
{
    if (!target) return;
    recomp_func_t fn = recomp_lookup_manual(target);
    if (!fn) fn = recomp_lookup(target);
    if (fn) {
        manual_call(ret_va, fn);
    } else {
        recomp_icall_fail_log(target);
    }
}

void recomp_manual_00103940(void)
{
    uint32_t esi = g_ecx;
    MEM32(0x003FA3ACu) = MEM32(0x003FA3ACu) + 1;
    MEM8(esi + 0x1F8) = 1;
    MEM8(esi + 0x1F9) = 0;
    MEM8(esi + 0x1FA) = 0;
    MEM8(esi + 0x200) = 0;
    MEM32(esi + 0x1FC) = 0xFFFFFFFFu;
    MEM32(esi + 0x1F4) = 0;
    MEM32(esi + 0x1EC) = 0;
    MEM32(esi + 0x1F0) = 0;

    g_ecx = 0x003789A4u;
    manual_call(0x00103995u, sub_001023D0);

    float fov = MEMF(esi + 4);
    if (fov == -1.0f) {
        if (MEM32(esi + 0xB40) != 0) {
            MEMF(esi + 4) = 210.0f;
        } else {
            MEMF(esi + 4) = 180.0f;
        }
    }

    g_eax = esi;
    PUSH32(g_esp, MEM32(esi + 0x94C));
    manual_call(0x001039D2u, sub_00147800);

    g_ecx = esi;
    manual_call(0x001039D9u, sub_000BD3F0);

    if (MEMF(esi + 0x7E4) < 0.0f) {
        MEMF(esi + 0x7E4) = 0.0f;
    }

    uint32_t vt = MEM32(esi);
    if (vt) {
        uint32_t fn_16c = MEM32(vt + 0x16C);
        g_ecx = esi;
        manual_icall_safe(fn_16c, 0x001039F7u);
    }

    MEM8(esi + 0xA44) = 1;
    PUSH32(g_esp, esi);
    manual_call(0x00103A04u, sub_00103110);

    vt = MEM32(esi);
    if (vt) {
        uint32_t fn_28c = MEM32(vt + 0x28C);
        g_ecx = esi;
        manual_icall_safe(fn_28c, 0x00103A0Eu);
    }

    vt = MEM32(esi);
    if (vt) {
        uint32_t fn_10c = MEM32(vt + 0x10C);
        g_ecx = esi;
        manual_icall_safe(fn_10c, 0x00103A18u);
    }

    PUSH32(g_esp, esi);
    manual_call(0x00103A1Eu, sub_00103430);

    vt = MEM32(esi);
    if (vt) {
        uint32_t fn_110 = MEM32(vt + 0x110);
        g_ecx = esi;
        manual_icall_safe(fn_110, 0x00103A28u);
    }

    uint32_t actor = MEM32(esi + 0x4F0);
    if (actor != 0) {
        if ((MEM8(actor + 0xC) & 0x20) == 0) {
            uint32_t list = MEM32(actor + 8);
            if (list) {
                uint32_t count = MEM32(list + 0x14);
                MEM32(list + count * 4 + 0x18) = actor;
                MEM32(list + 0x14) = count + 1;
            }
        }
        MEM32(actor + 0xC) = 0x167;
        actor = MEM32(esi + 0x4F0);
        MEM32(actor + 0x14) = 4;
        MEMF(actor + 0x68) = 0.0f;
        MEMF(actor + 0x78) = 0.0f;
        MEM32(actor + 0x70) = 0;

        uint32_t subobj = actor + 0x9C;
        MEM32(subobj + 0) = 0;
        MEM32(subobj + 4) = 0;
        MEM32(subobj + 8) = 0;
        MEM32(subobj + 0xC) = 0;

        MEMF(actor + 0x3B8) = 1000.0f;
        MEM8(actor + 0x3BC) = 0;
    }

    g_esp += 4; /* ret */
}

recomp_func_t recomp_lookup_manual(uint32_t xbox_va)
{
    switch (xbox_va) {
    case 0x000EC960u: return recomp_manual_000EC960;
    case 0x00103940u: return recomp_manual_00103940;
    case 0x00300972u: return recomp_manual_XGetDevices;
    case 0x00300994u: return recomp_manual_XGetDeviceChanges;
    case 0x00300A01u: return recomp_manual_XInputOpen;
    case 0x00300A57u: return recomp_manual_XInputClose;
    case 0x00300A63u: return recomp_manual_XInputGetCapabilities;
    case 0x00300C3Bu: return recomp_manual_XInputGetState;
    case 0x00300CAEu: return recomp_manual_XInputSetState;
    case 0x00300CE1u: return recomp_manual_XInputPoll;
    default: break;
    }
    return (recomp_func_t)0;
}

/* ── ICALL failure logging ─────────────────────────────────── */

/*
 * Called when RECOMP_ICALL cannot resolve a target address.
 * This usually means one of:
 *   - A vtable dispatch to an address not in the dispatch table
 *   - A function pointer loaded from uninitialized or corrupt memory
 *   - A kernel thunk address that the bridge doesn't handle
 *
 * During early bring-up you will see many of these. Most are harmless
 * (the ICALL macro pops the dummy return address and continues).
 * Focus on the ones that cause crashes or incorrect behavior.
 */
void recomp_icall_fail_log(uint32_t va)
{
    fprintf(stderr, "[ICALL] Failed to resolve VA 0x%08X (total calls: %llu)\n",
            va, (unsigned long long)g_icall_count);

    /* Dump last 16 call targets from the ring buffer */
    fprintf(stderr, "  Recent ICALL targets:\n");
    for (int i = 0; i < 16; i++) {
        int idx = (g_icall_trace_idx - 16 + i) & 15;
        if (g_icall_trace[idx])
            fprintf(stderr, "    [%2d] 0x%08X\n", i, g_icall_trace[idx]);
    }
    fflush(stderr);
}

/* An indirect call whose target is not code: a null or wild function pointer.
 *
 * Skipping these is right -- calling a data address is worse -- but skipping
 * them *silently* is not. They almost always arrive inside a loop, so the
 * symptom is a hang with no output rather than a diagnosable null vtable call.
 *
 * Rate-limited per address: a spin can produce millions of these, and the
 * useful information is which addresses occur, not how often.
 */
/* The null-pointer call right before a crash is the useful clue, and the
 * target alone (0x00000000) says nothing about which object was broken. Log
 * the call site and `this` for the first few hits per run. */
void recomp_icall_not_code_log_at(uint32_t va, uint32_t ret, uint32_t this_)
{
    static int printed;
    if (printed < 32) {
        printed++;
        fprintf(stderr, "[ICALL] not-code target 0x%08X from call site ~0x%08X"
                        " (return 0x%08X), ecx/this=0x%08X\n",
                va, ret - 2u, ret, this_);
        fflush(stderr);
    }
    recomp_icall_not_code_log(va);
}

void recomp_icall_not_code_log(uint32_t va)
{
    enum { SLOTS = 16 };
    static uint32_t seen[SLOTS];
    static uint64_t hits[SLOTS];
    static int count;
    int i;

    for (i = 0; i < count; i++)
        if (seen[i] == va)
            break;
    if (i == count) {
        if (count == SLOTS)
            return;
        seen[count] = va;
        hits[count] = 0;
        count++;
    }
    hits[i]++;
    /* Report at 1, 10, 100, 1000 ... rather than once. A single line says a
     * wild pointer was skipped; the progression says it is being skipped in a
     * loop, which is the difference between a curiosity and the reason the
     * title is hung. */
    {
        uint64_t n = hits[i];
        while (n >= 10 && n % 10 == 0)
            n /= 10;
        if (n != 1)
            return;
    }
    fprintf(stderr, "[ICALL] target 0x%08X is not code -- skipped %llu time(s) "
                    "(null or wild function pointer, at call #%llu)\n",
            va, (unsigned long long)hits[i],
            (unsigned long long)g_icall_count);
    fflush(stderr);
}

#include <stdlib.h>

/* ── The title's own debug log ─────────────────────────────────────────
 *
 * tools/fix_game_log.py calls this at the entry of sub_00242B10, the game's
 * log sink (esi = level 0..3, edi = formatted text). COLDFEAR_GAME_LOG=0
 * turns it off. After 4000 lines only every 100th is kept, so a message in a
 * loop cannot drown the rest of the log. */
void coldfear_game_log(uint32_t level, uint32_t msg_va)
{
    static int enabled = -1;
    static unsigned long lines;
    char buf[512];
    size_t n;
    if (enabled < 0) {
        const char *e = getenv("COLDFEAR_GAME_LOG");
        enabled = !(e && *e == '0');
    }
    if (!enabled || level > 3u || msg_va == 0 || msg_va >= 0x04000000u)
        return;
    lines++;
    if (lines > 4000 && lines % 100 != 0)
        return;
    for (n = 0; n + 1 < sizeof(buf); n++) {
        char c = (char)MEM8(msg_va + (uint32_t)n);
        if (!c)
            break;
        buf[n] = (c == '\r') ? ' ' : c;
    }
    while (n && (buf[n - 1] == '\n' || buf[n - 1] == ' '))
        n--;
    buf[n] = 0;
    fprintf(stderr, "[GAME:%u] %s\n", (unsigned)level, buf);
}

/* tools/fix_entity_dbg.py: sub_00147800's entity-bank lookup. Every lookup
 * whose record has no clump (record[0] == 0) is logged, plus the first 40. */
void coldfear_entity_dbg(uint32_t handle, uint32_t rec, uint32_t pkey1, uint32_t pkey2,
                         uint32_t sp, uint32_t this_)
{
    /* sp is sub_00147800's esp after its 2 saves; sub_001C73B0 pops its own
     * 3 args (ret 12). So [sp] = saved esi, [sp+4] = saved-ecx slot reused
     * for the type key (0x7530), [sp+8] = return address (the caller),
     * [sp+12] = the entity number it was given. this_ = esi = the object.
     * pkey1/pkey2 are [sp]/[sp+4] from the call site: not key pointers. */
    static int printed;
    uint32_t clump = 0, vt = 0, field = 0;
    uint32_t type = MEM32(sp + 4), ret = MEM32(sp + 8), arg = MEM32(sp + 12);
    int bad = handle == 0xFFFFFFFFu || (handle >> 24) >= 8u;
    (void)pkey1; (void)pkey2;
    if (rec && rec < 0x04000000u) clump = MEM32(rec);
    if (this_ && this_ < 0x04000000u) { vt = MEM32(this_); field = MEM32(this_ + 0x94C); }
    if (clump != 0 && !bad && printed >= 40)
        return;
    printed++;
    fprintf(stderr, "[ENTITY] lookup type=0x%08X entnum=0x%08X from caller ret 0x%08X obj=0x%08X"
                    " vt=0x%08X [obj+0x94c]=0x%08X -> handle 0x%08X record 0x%08X clump 0x%08X%s\n",
            type, arg, ret, this_, vt, field, handle, rec, clump,
            bad ? "  <-- BAD HANDLE" : clump == 0 ? "  <-- NO CLUMP" : "");
    fflush(stderr);
}

/* tools/fix_bank_dbg.py: a match inside sub_001C73B0. Logged for the first
 * 60 matches and for every handle whose bank byte is not 0..7. On a bad one,
 * also dumps the slot table and the matched handle array, so the clobbered
 * address and what overwrote it are visible. */
void coldfear_bank_dbg(uint32_t slot, uint32_t slot_va, uint32_t arr,
                       uint32_t idx, uint32_t handle, uint32_t key)
{
    static int printed, dumps;
    int bad = (int32_t)handle >= 0 && (handle >> 24) >= 8u;
    int odd = bad || (handle == 0 && idx != 0);
    if (!odd && printed >= 60)
        return;
    printed++;
    fprintf(stderr, "[BANK] key=0x%08X slot %u (slot@0x%08X bank 0x%08X) array 0x%08X[%u]"
                    " @0x%08X = 0x%08X%s\n",
            key, slot, slot_va,
            slot_va < 0x04000000u ? MEM32(slot_va) : 0,
            arr, idx, arr + idx * 4u, handle,
            bad ? "  <-- BAD HANDLE" : odd ? "  <-- HANDLE 0 FOR NONZERO INDEX" : "");
    if (odd && dumps++ < 4) {
        /* slot table: 8 x {u8 active @-4, u32 bank @0}, stride 8;
         * bank: {u32 count, u32 entries}; entry: 12 bytes {key, n, array} */
        uint32_t base = slot_va - 8u * slot, i, e;
        for (i = 0; i < 8; i++) {
            uint32_t bank = MEM32(base + i * 8u);
            unsigned act = MEM8(base + i * 8u - 4u);
            uint32_t cnt = 0, ents = 0;
            if (bank && bank < 0x04000000u) { cnt = MEM32(bank); ents = MEM32(bank + 4); }
            fprintf(stderr, "[BANK]   slot %u: active=%u bank=0x%08X entries=%u @0x%08X\n",
                    i, act, bank, cnt, ents);
            for (e = 0; act && ents && ents < 0x04000000u && e < cnt && e < 16; e++)
                fprintf(stderr, "[BANK]     entry %u: key=0x%08X n=%u array=0x%08X\n", e,
                        MEM32(ents + e * 12u), MEM32(ents + e * 12u + 4u),
                        MEM32(ents + e * 12u + 8u));
        }
        if (arr < 0x04000000u) {
            uint32_t from = idx >= 8 ? idx - 8 : 0;
            fprintf(stderr, "[BANK]   array[%u..%u]:", from, idx + 4);
            for (i = from; i <= idx + 4; i++)
                fprintf(stderr, " %08X", MEM32(arr + i * 4u));
            fprintf(stderr, "\n");
        }
    }
    fflush(stderr);
}

/* tools/fix_ptank_flag.py: does this particle tank use compressed UVs?
 * Answered from the tank's own vertex writer, which its instance callback
 * stored at [plugin+0x2C] together with the global 0x37D324 -- the global
 * only describes the tank instanced last. Unknown writer: the global, as
 * before. Disagreements are logged (first 20): each one was a mis-drawn
 * tank. */
uint32_t coldfear_ptank_compressed(uint32_t obj, uint32_t global_flag)
{
    static int logged;
    uint32_t base = MEM32(0x37D31Cu), plugin, cb, mine;

    if (!obj || obj >= 0x04000000u || !base || base >= 0x04000000u)
        return global_flag;
    plugin = MEM32(base + obj);
    if (!plugin || plugin >= 0x04000000u)
        return global_flag;
    cb = MEM32(plugin + 0x2C);
    if (cb == 0x0005AC10u || cb == 0x0005ACF0u)
        mine = 1;
    else if (cb == 0x0005E070u || cb == 0x0005E260u)
        mine = 0;
    else
        return global_flag;
    if (mine != (global_flag != 0) && logged < 20) {
        logged++;
        fprintf(stderr, "[PTANK] tank %08X writer %08X: %s UVs, global said %s\n", obj, cb,
                mine ? "compressed" : "float", global_flag ? "compressed" : "float");
        fflush(stderr);
    }
    return mine;
}

/* tools/fix_entnum_dbg.py: writes/reads of an entity's number [obj+0x94c].
 * tag 1 = script cmd sub_002478C0, 2 = sub_00248870 (both write), 3 =
 * DeadBody init sub_00103940 (reads; ecx = this). Logs the first 200. */
void coldfear_entnum_dbg(int tag, uint32_t sp, uint32_t this_)
{
    static int printed;
    if (printed >= 200)
        return;
    printed++;
    if (tag == 1 || tag == 2) {
        uint32_t ctx = MEM32(sp + 4), ent = 0, obj = 0, idx = 0, val = 0;
        if (ctx && ctx < 0x04000000u) {
            ent = MEM32(ctx + 0x24);
            if (ent && ent < 0x04000000u)
                obj = MEM32(ent + 4);
            idx = MEM32(ctx + 0x20);
            if (tag == 1)
                val = MEM32(ctx + idx * 4u + 0x44u);
            else if (obj && obj < 0x04000000u)
                val = MEM32(obj + 0xD30);
        }
        fprintf(stderr, "[ENTNUM] %s obj=0x%08X vt=0x%08X <- 0x%08X (ctx 0x%08X slot %u, ret 0x%08X)\n",
                tag == 1 ? "script-set " : "set-d30   ", obj,
                obj && obj < 0x04000000u ? MEM32(obj) : 0, val, ctx, idx, MEM32(sp));
    } else {
        fprintf(stderr, "[ENTNUM] init-read  obj=0x%08X vt=0x%08X  [0x94c]=0x%08X (ret 0x%08X)\n",
                this_, this_ && this_ < 0x04000000u ? MEM32(this_) : 0,
                this_ && this_ < 0x04000000u ? MEM32(this_ + 0x94c) : 0, MEM32(sp));
    }
    fflush(stderr);
}

/* tools/fix_entity_dbg.py, at the entry of sub_00147800: [sp] is the exact
 * return address (caller), [sp+4] the entity number it was passed, eax the
 * object. Logged for DeadBody objects (vtable 0x0031F708), for numbers that
 * are not small, and for the first 20 calls. */
void coldfear_entity_entry(uint32_t sp, uint32_t this_)
{
    static int printed;
    uint32_t ret = MEM32(sp), num = MEM32(sp + 4);
    uint32_t vt = this_ && this_ < 0x04000000u ? MEM32(this_) : 0;
    if (!(printed < 20 || vt == 0x0031F708u || num > 0x1000u))
        return;
    if (printed++ > 400)
        return;
    fprintf(stderr, "[ENTITY-IN] caller ret 0x%08X entnum 0x%08X obj 0x%08X vt 0x%08X [obj+0x94c]=0x%08X\n",
            ret, num, this_, vt, vt ? MEM32(this_ + 0x94c) : 0);
    fflush(stderr);
}

/* ── Untranslated instructions ───────────────────────────────────────────
 *
 * The lifter emits RECOMP_UNIMPL(text, va) at every instruction it has no
 * translation for, in place of the bare comment it used to leave. The
 * instruction is still a no-op; this only stops the omission being silent.
 * RECOMP_UNIMPL_TRAP=1 aborts at the first hit, at the guest address of the
 * cause rather than wherever the damage surfaces. */
#include <stdlib.h>

void recomp_unimpl(const char *text, uint32_t va)
{
    static int printed;
    const char *trap = getenv("RECOMP_UNIMPL_TRAP");
    int stop = trap && *trap && *trap != '0';

    if (printed < 50 || stop) {
        printed++;
        fprintf(stderr,
                "[UNIMPL] untranslated instruction REACHED: `%s` at 0x%08X"
                " (a no-op; set RECOMP_UNIMPL_TRAP=1 to stop here)\n",
                text, va);
        fflush(stderr);
    }
    if (stop) abort();
}


/* ── DirectSound's voice-stop wait (tools/fix_dswait.py) ─────────────────
 *
 * sub_002CF73D spins until a stopping voice's 0x8000 flag clears, calling
 * sub_002CEF25 (KeQuerySystemTime + a hardware poll, 0.5 s timeout) every
 * turn: on Cold Fear's ship the main thread called it ~200,000 times a
 * second. Measured here -- waits that actually looped, their iterations,
 * total and longest time, every 2 s -- and COLDFEAR_DSWAIT=yield gives the
 * guest lock away on every turn so the APU's interrupt and DPC can run. */
#include <time.h>
#include <string.h>
extern void recomp_spin_yield(void);

static int dswait_mode(void)
{
    static int mode = -1;
    if (mode < 0) {
        const char *e = getenv("COLDFEAR_DSWAIT");
        /* yield: give the guest lock away each turn (measured: no gain);
         * report: the 2-second [DSWAIT] lines. */
        mode = e && !strcmp(e, "yield") ? 1 : e && !strcmp(e, "report") ? 2 : 0;
    }
    return mode;
}

static uint64_t dswait_now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

static uint64_t s_dsw_t0, s_dsw_report, s_dsw_total, s_dsw_max;
static uint32_t s_dsw_iters, s_dsw_waits, s_dsw_turns;

void coldfear_dswait_begin(void)
{
    s_dsw_t0 = 0;
    s_dsw_turns = 0;
}

void coldfear_dswait_iter(void)
{
    int mode = dswait_mode();
    if (mode == 1)
        recomp_spin_yield();
    if (mode != 2)
        return;
    if (!s_dsw_turns++)
        s_dsw_t0 = dswait_now_us();
    s_dsw_iters++;
}

void coldfear_dswait_end(void)
{
    uint64_t now;
    if (dswait_mode() != 2)
        return;
    now = dswait_now_us();
    if (s_dsw_turns) {
        uint64_t d = now - s_dsw_t0;
        s_dsw_waits++;
        s_dsw_total += d;
        if (d > s_dsw_max) s_dsw_max = d;
    }
    if (!s_dsw_report)
        s_dsw_report = now;
    if (now - s_dsw_report >= 2000000u) {
        fprintf(stderr, "[DSWAIT] 2 s: %u waits, %u turns, %.1f ms spent, longest %.2f ms\n",
                s_dsw_waits, s_dsw_iters, s_dsw_total / 1000.0, s_dsw_max / 1000.0);
        s_dsw_report = now;
        s_dsw_waits = s_dsw_iters = 0;
        s_dsw_total = s_dsw_max = 0;
    }
}

/* sub_002CEF25's give-up time for a releasing voice, in 100 ns units
 * (tools/fix_dswait.py). Stock is 0.5 s; this port uses 1 ms, which cut the
 * main thread's waits ~70% with the sound unchanged on the console.
 * COLDFEAR_DSWAIT_TIMEOUT_US sets another (500000 = stock). A voice whose
 * stop notification is late is then stopped by DirectSound's own forced
 * path (sub_002CEE3C) instead of being spun on. */
uint32_t coldfear_dswait_timeout(void)
{
    static uint32_t units;
    if (!units) {
        const char *e = getenv("COLDFEAR_DSWAIT_TIMEOUT_US");
        long us = e ? strtol(e, NULL, 0) : 1000;
        if (us <= 0)
            us = 1000;
        units = us < 500000 ? (uint32_t)us * 10u : 0x4C4B40u;
        fprintf(stderr, "[DSWAIT] voice-stop timeout %.3f ms\n", units / 10000.0);
    }
    return units;
}

/* tools/fix_camlog_dbg.py: every RenderWare camera that uploads the view
 * rows (sub_00265100, c[163]), each logged at most every 2 s: its frame's
 * LTM -- right, up, at, pos -- the parent frame's, and the c[163..166] rows.
 * Cold Fear's flashlight renders its shadow buffer through a camera of its
 * own; its "at" against the main camera's says which axis the light lost.
 * Diagnostics. */
void coldfear_camlog(uint32_t index, uint32_t data)
{
    static struct { uint32_t cam; uint64_t t; } seen[6];
    uint32_t cam, fr, par, k, r;
    uint64_t now;
    struct timespec ts;

    if (index != 163)
        return;
    cam = MEM32(0x416380u);
    if (!cam || cam >= 0x04000000u)
        return;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    now = (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
    for (k = 0; k < 6 && seen[k].cam && seen[k].cam != cam; k++) ;
    if (k == 6)
        return;
    if (seen[k].cam == cam && now - seen[k].t < 2000)
        return;
    seen[k].cam = cam;
    seen[k].t = now;
    fr = MEM32(cam + 4);
    fprintf(stderr, "[CAMLOG] cam %08X frame %08X\n", cam, fr);
    if (fr && fr < 0x04000000u) {
        static const char *const nm[4] = { "right", "up", "at", "pos" };
        par = MEM32(fr + 4);
        for (r = 0; r < 4; r++)
            fprintf(stderr, "[CAMLOG]   ltm %-5s %9.4f %9.4f %9.4f   model %9.4f %9.4f %9.4f\n", nm[r],
                    MEMF(fr + 0x50 + r * 16), MEMF(fr + 0x54 + r * 16), MEMF(fr + 0x58 + r * 16),
                    MEMF(fr + 0x10 + r * 16), MEMF(fr + 0x14 + r * 16), MEMF(fr + 0x18 + r * 16));
        if (par && par < 0x04000000u)
            for (r = 0; r < 4; r++)
                fprintf(stderr, "[CAMLOG]   parent %08X ltm %-5s %9.4f %9.4f %9.4f\n", par, nm[r],
                        MEMF(par + 0x50 + r * 16), MEMF(par + 0x54 + r * 16), MEMF(par + 0x58 + r * 16));
    }
    if (data && (data < 0x04000000u || data >= 0x80000000u))
        for (r = 0; r < 4; r++)
            fprintf(stderr, "[CAMLOG]   c[%u] %9.4f %9.4f %9.4f %9.4f\n", 163 + r, MEMF(data + r * 16),
                    MEMF(data + r * 16 + 4), MEMF(data + r * 16 + 8), MEMF(data + r * 16 + 12));
    fflush(stderr);
}

/* tools/fix_gamma_ramp.py: Xbox D3D's DAC writer (sub_0026BF76, the flip
 * handler's SetGammaRamp step) takes the ramp at [esp+4]: 256 red, 256
 * green, 256 blue bytes. The DAC is plain memory here, so the ramp goes to
 * the Vulkan presenter instead, which maps each frame through it. */
extern void nv2a_vk_set_gamma_ramp(const uint8_t *ramp);

void coldfear_gamma_ramp(uint32_t ramp_va)
{
    uint8_t ramp[768];
    uint32_t i;
    if (!ramp_va || ramp_va >= 0x04000000u && ramp_va < 0x80000000u)
        return;
    for (i = 0; i < 768; i++)
        ramp[i] = MEM8(ramp_va + i);
    nv2a_vk_set_gamma_ramp(ramp);
}
