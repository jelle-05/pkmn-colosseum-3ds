/*
 * Port override of decomp include/game/gs_log_format.h.
 *
 * The original header carries its own copy of MWCC's PowerPC va_list and
 * va_start/va_arg macros, which cannot work on ARM. This wrapper includes
 * the original for everything else (the inline number formatters), with the
 * va_list names moved out of the way, and then puts the compiler's varargs
 * back.
 */
#ifndef PORT_GS_LOG_FORMAT_H
#define PORT_GS_LOG_FORMAT_H

#define __va_list __mwcc_va_list_unused
#define va_list __mwcc_va_list_t_unused
#include "include/game/gs_log_format.h"
#undef __va_list
#undef va_list
#undef va_start
#undef va_arg

typedef __builtin_va_list va_list;
#define va_start(ap, last) __builtin_va_start(ap, last)
#define va_arg(ap, t) __builtin_va_arg(ap, t)
#ifndef va_end
#define va_end(ap) __builtin_va_end(ap)
#endif

#endif /* PORT_GS_LOG_FORMAT_H */
