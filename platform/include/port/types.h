/* types.h - fixed-size types for the platform's big-endian side, which
 * does not include <3ds.h>. */
#ifndef PORT_TYPES_H
#define PORT_TYPES_H

#include <stdint.h>

#ifndef PORT_TYPES_DEFINED
#define PORT_TYPES_DEFINED
typedef uint8_t u8;
typedef int8_t s8;
typedef uint16_t u16;
typedef int16_t s16;
typedef uint32_t u32;
typedef int32_t s32;
typedef uint64_t u64;
typedef int64_t s64;
#endif

#endif
