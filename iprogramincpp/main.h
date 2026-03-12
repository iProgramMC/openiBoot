/***
	The Boron Operating System
	Copyright (C) 2023 iProgramInCpp

Module name:
	main.h
	
Abstract:
	This header file contains the global definitions
	for the Boron kernel and drivers.
	
Author:
	iProgramInCpp - 20 August 2023
***/
#ifndef NS64_MAIN_H
#define NS64_MAIN_H

//#include <stdint.h>
#include <openiboot.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdbool.h>

// HACK for OpeniBoot
#define KERNEL
#define TARGET_ARM
#define TARGET_ARMV6
#define DEBUG

//this toolchain predates uintptr_t seemingly
typedef uint32_t uintptr_t;
// END HACK

#define PACKED        __attribute__((packed))
#define NO_RETURN     __attribute__((noreturn))
#define RETURNS_TWICE __attribute__((returns_twice))
#define UNUSED        __attribute__((unused))
#define ALWAYS_INLINE __attribute__((always_inline))
#define NO_DISCARD    __attribute__((warn_unused_result))
#include "rtl/assert.h"
#include "status.h"

#define FORCE_INLINE ALWAYS_INLINE static inline
#define BIT(x) (1ULL << (x))
#define ASM __asm__ __volatile__

#define LIKELY(x)   __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)

// We're using C11
#define static_assert _Static_assert

void LogMsg(const char*, ...);
#define DbgPrint LogMsg

#define ARRAY_COUNT(x) (sizeof(x) / sizeof((x)[0]))

#define IN
#define OUT
#define INOUT
#define OPTIONAL

#define CallerAddress() ((uintptr_t) __builtin_return_address(0))

#define CONTAINING_RECORD(Pointer, Type, Field) ((Type*)((uintptr_t)(Pointer) - (uintptr_t)offsetof(Type, Field)))

#include "rtl/list.h"
#include "rtl/rbtree.h"

#endif//NS64_MAIN_H
