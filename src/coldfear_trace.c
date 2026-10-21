/* Temporary boot-03 diagnostics. Never repair guest registers or skip calls. */
#include "recomp_types.h"
#include "coldfear_trace.h"
#include <stdio.h>
#include <stdatomic.h>

static atomic_uint trace_count;

void coldfear_trace_call(uint32_t target, void (*function)(void))
{
    unsigned id = atomic_fetch_add_explicit(&trace_count, 1, memory_order_relaxed);
    if (id >= 2048) {
        function();
        return;
    }
    uint32_t stack = g_esp;
    /* Only inspect the original main-thread stack observed in boot-03. */
    int readable = stack >= 0x00F00000u && stack <= 0x00FFFFFCu;
    uint32_t ret = readable ? MEM32(stack) : 0;
    printf("[CF-CALL %u] > %08X ret=%08X esp=%08X eax=%08X ebx=%08X esi=%08X edi=%08X\n",
           id, target, ret, stack, g_eax, g_ebx, g_esi, g_edi);
    function();
    printf("[CF-CALL %u] < %08X esp=%08X delta=%d eax=%08X ebx=%08X esi=%08X edi=%08X ret-slot=%08X\n",
           id, target, g_esp, (int32_t)(g_esp - stack), g_eax, g_ebx,
           g_esi, g_edi, readable ? MEM32(stack) : 0);
}
