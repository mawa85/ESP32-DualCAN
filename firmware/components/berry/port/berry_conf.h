/*
 * Dorky Commander berry_conf.h — ESP32-C6 configuration
 *
 * Derived from upstream default/berry_conf.h (MIT, Guan Wenliang).
 * Tuned for:
 *   - 32-bit RISC-V with no hardware FPU (single-precision float)
 *   - ~320 KB SRAM, 4 MB flash
 *   - No filesystem (scripts loaded from memory / LittleFS via native bindings)
 *   - Minimal module set (expand as needed)
 */
#ifndef BERRY_CONF_H
#define BERRY_CONF_H

#include <assert.h>

#ifndef BE_DEBUG
#define BE_DEBUG                        0
#endif

/* 0 = int, 1 = long (32-bit on RISC-V C6), 2 = long long. */
#define BE_INTGER_TYPE                  1

/* No hardware FPU on ESP32-C6 — avoid software double arithmetic. */
#define BE_USE_SINGLE_FLOAT             1

#define BE_BYTES_MAX_SIZE               (4 * 1024)
#define BE_USE_PRECOMPILED_OBJECT       1

/* Flash / RAM savings. */
#define BE_DEBUG_SOURCE_FILE            0
#define BE_DEBUG_RUNTIME_INFO           2
#define BE_DEBUG_VAR_INFO               0

/* Observability hook for watchdog / infinite-loop detection. */
#define BE_USE_PERF_COUNTERS            1
#define BE_VM_OBSERVABILITY_SAMPLING    20

#define BE_STACK_TOTAL_MAX              2000
#define BE_STACK_FREE_MIN               10
#define BE_STACK_START                  50
#define BE_CONST_SEARCH_SIZE            50
#define BE_USE_STR_HASH_CACHE           0

/* No filesystem — Berry's be_fopen / be_filelib are unused.
 * Scripts are loaded via be_loadstring from C strings. */
#define BE_USE_FILE_SYSTEM              0

#define BE_USE_SCRIPT_COMPILER          1
#define BE_USE_BYTECODE_SAVER           0
#define BE_USE_BYTECODE_LOADER          0
#define BE_USE_SHARED_LIB               0
#define BE_USE_OVERLOAD_HASH            0

#define BE_USE_DEBUG_HOOK               0
#define BE_USE_DEBUG_GC                 0
#define BE_USE_DEBUG_STACK              0
#define BE_USE_MEM_ALIGNED              0

/* Module selection — keep the minimum for phase 0. */
#define BE_USE_STRING_MODULE            1
#define BE_USE_JSON_MODULE              0
#define BE_USE_MATH_MODULE              0
#define BE_USE_TIME_MODULE              0
#define BE_USE_OS_MODULE                0
#define BE_USE_GLOBAL_MODULE            1
#define BE_USE_SYS_MODULE               0
#define BE_USE_DEBUG_MODULE             0
#define BE_USE_GC_MODULE                1
#define BE_USE_SOLIDIFY_MODULE          0
#define BE_USE_INTROSPECT_MODULE        0
#define BE_USE_STRICT_MODULE            0

#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif

/* libc hooks — newlib-nano on ESP-IDF provides these. */
#define BE_EXPLICIT_ABORT               abort
#define BE_EXPLICIT_EXIT                exit
#if defined(ESP_PLATFORM) && defined(CONFIG_SPIRAM)
/* On PSRAM boards keep the Berry heap out of internal RAM: with
 * SPIRAM_MALLOC_ALWAYSINTERNAL every small GC object used to land in
 * internal DRAM, the same pool the BT controller allocates from at runtime.
 * be_port.c falls back to internal RAM when PSRAM is exhausted. */
#include <stddef.h>
void *be_port_malloc(size_t size);
void  be_port_free(void *ptr);
void *be_port_realloc(void *ptr, size_t size);
#define BE_EXPLICIT_MALLOC              be_port_malloc
#define BE_EXPLICIT_FREE                be_port_free
#define BE_EXPLICIT_REALLOC             be_port_realloc
#else
#define BE_EXPLICIT_MALLOC              malloc
#define BE_EXPLICIT_FREE                free
#define BE_EXPLICIT_REALLOC             realloc
#endif

#define be_assert(expr)                 assert(expr)

#endif
