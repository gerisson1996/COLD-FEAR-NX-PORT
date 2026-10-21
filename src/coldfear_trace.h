#ifndef COLDFEAR_TRACE_H
#define COLDFEAR_TRACE_H
#include <stdint.h>
void coldfear_trace_call(uint32_t target, void (*function)(void));
#endif
