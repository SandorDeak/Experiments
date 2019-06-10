#pragma once

#include <stdint.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;
typedef float f32;
typedef double f64;
typedef uintptr_t umm;
typedef intptr_t smm;
typedef s32 b32;

#define U32_MAX 0xffffffffu

#define pi32 3.14159265359f

#define global static

#ifdef _DEBUG
#define ASSERT(value) if (!(value)) { *(volatile int*)0 = 0; }
#else
#define ASSERT(value) value;
#endif

#define INVALID_CODE_PATH ASSERT(false)

#define ARRAY_SIZE(a) (sizeof(a)/sizeof(*a))

#define IS_POW2(value) (((value) != 0) && (((value) & (value - 1)) == 0))

#define ALIGN_PTR(ptr, a) ((u8*)(((umm)(ptr)+(umm)(a)-1) & (~((umm)(a)-1))))
#define ALIGN_NUM(num, a) (((umm)(num)+(umm)(a)-1) & (~((umm)(a)-1)))

#define MAX(a, b) ((a) < (b) ? (b) : (a))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define CLAMP(a, b, t) (MAX(a, MIN(b,t)))

#include <intrin.h>

#define busyWaitWhile(expr) while(expr) {_mm_pause();}