/* The core runtime for the freestanding profile (cosmoos/profiles.md §4.2). */
#define NV_FREESTANDING 1
/* SPDX-License-Identifier: GPL-2.0-only WITH Classpath-exception-2.0 */
/*
 * Nova runtime core: integer formatting, output and panic reporting, heap accounting and regions, built
 * only on the platform services declared in nova/rt.h. No libc.
 *
 * With NV_FREESTANDING defined (the freestanding profile, cosmoos/profiles.md §4.2) only the first part
 * is compiled: code that needs no platform service and has no mutable global state. The environment
 * then defines nova_panic.
 */
#include "nova/rt.h"

const char *nova_panic_reason(nv_panic_kind kind) {
    switch (kind) {
    case NV_PANIC_OVERFLOW: return "integer overflow";
    case NV_PANIC_DIVIDE_BY_ZERO: return "division by zero";
    case NV_PANIC_SHIFT: return "shift amount out of range";
    case NV_PANIC_UNREACHABLE: return "entered unreachable code";
    case NV_PANIC_INDEX: return "index out of bounds";
    case NV_PANIC_SLICE: return "slice range out of bounds";
    case NV_PANIC_UNWRAP: return "unwrapped an Err value";
    case NV_PANIC_SCRATCH: return "a scratch frame is already active; nest with frame.enter()";
    case NV_PANIC_UNWRAP_NULL: return "unwrapped null";
    case NV_PANIC_STR_BOUNDARY: return "string slice not on a character boundary";
    case NV_PANIC_FFI_ENUM: return "invalid enum value from C";
    case NV_PANIC_LENGTH: return "slice lengths differ";
    case NV_PANIC_FLOAT_RANGE: return "float out of range";
    default: return "unknown panic";
    }
}

nv_str nova_io_message(uint32_t kind) {
    static const char *const messages[] = {
        "No such file or directory", "Permission denied", "File exists", "Is a directory",
        "Not a directory", "Invalid argument", "Broken pipe", "Connection refused",
        "Address already in use", "Out of memory", "Input/output error",
    };
    const char *text = messages[kind <= NV_IO_OTHER ? kind : NV_IO_OTHER];
    uint64_t length = 0;
    while (text[length] != 0) length++;
    return (nv_str){ .ptr = (const uint8_t *)text, .len = length };
}

bool nova_utf8_valid(const uint8_t *bytes, uint64_t length) {
    uint64_t i = 0;
    while (i < length) {
        uint8_t b = bytes[i];
        uint64_t extra;
        uint32_t min;
        uint32_t code;
        if (b < 0x80u) { i++; continue; }
        if ((b & 0xE0u) == 0xC0u) { extra = 1; min = 0x80u; code = b & 0x1Fu; }
        else if ((b & 0xF0u) == 0xE0u) { extra = 2; min = 0x800u; code = b & 0x0Fu; }
        else if ((b & 0xF8u) == 0xF0u) { extra = 3; min = 0x10000u; code = b & 0x07u; }
        else return false;
        if (extra > length - i - 1) return false;
        for (uint64_t k = 1; k <= extra; k++) {
            uint8_t c = bytes[i + k];
            if ((c & 0xC0u) != 0x80u) return false;
            code = (code << 6) | (c & 0x3Fu);
        }
        if (code < min || code > 0x10FFFFu || (code >= 0xD800u && code <= 0xDFFFu)) return false;
        i += extra + 1;
    }
    return true;
}

/* ---- Fixed-buffer allocator (type-system.md §19.4) ----------------------------------------- */

nv_fba nova_fba_over(uint8_t *base, uint64_t capacity) {
    nv_fba f = {base, capacity, 0u};
    return f;
}

static void *fba_alloc(nv_fba *f, uint64_t size, uint64_t align) {
    uintptr_t start = (uintptr_t)f->base + (uintptr_t)f->used;
    uintptr_t aligned = (start + (uintptr_t)(align - 1u)) & ~(uintptr_t)(align - 1u);
    uint64_t offset = (uint64_t)(aligned - (uintptr_t)f->base);
    if (aligned < start || offset > f->capacity || size > f->capacity - offset) {
        return NULL;
    }
    f->used = offset + size;
    return (void *)aligned;
}

#ifdef NV_FREESTANDING
/* Only fixed buffers exist without a heap (cosmoos/profiles.md §4.1). */
void *nova_region_alloc(void *region, uint32_t kind, uint64_t size, uint64_t align) {
    return kind == NV_REGION_FBA ? fba_alloc((nv_fba *)region, size, align) : NULL;
}
#else

static size_t format_u64(uint64_t value, uint8_t buffer[20]) {
    size_t start = 20;
    do {
        buffer[--start] = (uint8_t)('0' + (value % 10u));
        value /= 10u;
    } while (value != 0u);
    return start;
}

void nova_write_u64(uint64_t value) {
    uint8_t buffer[20];
    size_t start = format_u64(value, buffer);
    nova_platform_write_stdout(buffer + start, 20u - start);
}

void nova_write_i64(int64_t value) {
    if (value < 0) {
        static const uint8_t minus = '-';
        nova_platform_write_stdout(&minus, 1u);
        /* -(value + 1) + 1 avoids negating INT64_MIN. */
        nova_write_u64((uint64_t)(-(value + 1)) + 1u);
    } else {
        nova_write_u64((uint64_t)value);
    }
}

void nova_write_bool(bool value) {
    static const uint8_t yes[] = {'t', 'r', 'u', 'e'};
    static const uint8_t no[] = {'f', 'a', 'l', 's', 'e'};
    if (value) {
        nova_platform_write_stdout(yes, sizeof yes);
    } else {
        nova_platform_write_stdout(no, sizeof no);
    }
}

void nova_write_str(const char *text, size_t length) {
    nova_platform_write_stdout((const uint8_t *)text, length);
}

nv_max_align_t nova_vec_empty;

void *nova_vec_grow(void *ptr, uint64_t len, uint64_t *cap, uint64_t extra, uint64_t size, uint32_t allocator) {
    if (extra > UINT64_MAX - len) return NULL;
    uint64_t need = len + extra;
    if (need <= *cap) return ptr;
    uint64_t next = *cap > UINT64_MAX / 2u ? UINT64_MAX : *cap * 2u;
    if (next < 4u) next = 4u;
    if (next < need) next = need;
    if (size == 0u || next > (uint64_t)SIZE_MAX / size) return NULL;
    uint8_t *fresh = (uint8_t *)nova_alloc((size_t)(next * size), allocator);
    if (fresh == NULL) return NULL;
    /* len <= *cap, whose bytes were allocated, so len * size fits; the old and new blocks are distinct. */
    if (len != 0u) nova_mem_copy(fresh, ptr, (size_t)(len * size));
    if (*cap != 0u) nova_free(ptr, allocator);
    *cap = next;
    return fresh;
}

void *nova_svec_slot(void **chunks, uint64_t len, uint64_t size, uint32_t allocator) {
    if (len > UINT64_MAX - 8u) return NULL;
    uint64_t j = len + 8u;
    unsigned k = 60u - nv_clz64(j);
    if (k >= NV_SVEC_CHUNKS) return NULL;
    if (chunks[k] == NULL) {
        uint64_t count = (uint64_t)8u << k;
        if (size == 0u || count > (uint64_t)SIZE_MAX / size) return NULL;
        chunks[k] = nova_alloc((size_t)(count * size), allocator);
        if (chunks[k] == NULL) return NULL;
    }
    return (uint8_t *)chunks[k] + (j - ((uint64_t)8u << k)) * size;
}

void nova_svec_free(void **chunks, uint32_t allocator) {
    for (unsigned k = 0; k < NV_SVEC_CHUNKS; k++) {
        if (chunks[k] != NULL) nova_free(chunks[k], allocator);
    }
}

void nova_write_bytes(const uint8_t *bytes, uint64_t length) {
    nova_platform_write_stdout(bytes, (size_t)length);
}

void nova_write_error(nv_str text) {
    nova_platform_write_stderr(text.ptr, (size_t)text.len);
}

void nova_write_view(nv_str text) {
    nova_platform_write_stdout(text.ptr, (size_t)text.len);
}

void nova_write_newline(void) {
    static const uint8_t newline = '\n';
    nova_platform_write_stdout(&newline, 1u);
}

_Noreturn void nova_exit(int32_t code) {
    nova_platform_exit(code);
}

static void write_err_cstr(const char *text) {
    size_t length = 0;
    while (text[length] != '\0') {
        length++;
    }
    nova_platform_write_stderr((const uint8_t *)text, length);
}

static void write_err_u32(uint32_t value) {
    uint8_t buffer[20];
    size_t start = format_u64(value, buffer);
    nova_platform_write_stderr(buffer + start, 20u - start);
}

/* Message format (relied on by run-fail tests): "nova panic: <reason> at <file>:<line>:<column>\n" */
_Noreturn void nova_panic(nv_panic_kind kind, nv_loc loc) {
    const char *reason = nova_panic_reason(kind);
    write_err_cstr("nova panic: ");
    write_err_cstr(reason);
    write_err_cstr(" at ");
    write_err_cstr(loc.file);
    write_err_cstr(":");
    write_err_u32(loc.line);
    write_err_cstr(":");
    write_err_u32(loc.column);
    write_err_cstr("\n");
    nova_platform_abort();
}

/* ---- Heap: accounting and fault injection (type-system.md §18) ---------------------------- */

static uint64_t alloc_attempts;   /* every allocation attempt, failed ones included */
static uint64_t alloc_live;       /* successful allocations not yet freed */
static uint64_t alloc_fail_at;    /* NOVA_FAIL_ALLOC_AT: this attempt fails; 0 = never */
static bool alloc_report;         /* NOVA_ALLOC_REPORT: print the counters at the end of main */
static bool failing_armed;        /* FailingAllocator.failAfter has been called */
static uint64_t failing_left;     /* successes FailingAllocator still allows */

void nova_rt_start(void) {
    alloc_fail_at = nova_platform_config("NOVA_FAIL_ALLOC_AT");
    alloc_report = nova_platform_config("NOVA_ALLOC_REPORT") != 0u;
}

void *nova_alloc(size_t size, uint32_t allocator) {
    alloc_attempts++;
    if (allocator == NV_ALLOC_FAILING && failing_armed) {
        if (failing_left == 0u) {
            return NULL;
        }
        failing_left--;
    }
    if (alloc_fail_at != 0u && alloc_attempts == alloc_fail_at) {
        return NULL;
    }
    void *p = nova_platform_alloc(size == 0u ? 1u : size);
    if (p != NULL) {
        alloc_live++;
    }
    return p;
}

void nova_free(void *p, uint32_t allocator) {
    (void)allocator; /* every M3 allocator frees to the platform heap */
    alloc_live--;
    nova_platform_free(p);
}

void nova_fail_after(uint64_t n) {
    failing_armed = true;
    failing_left = n;
}

void nova_fail_reset(void) {
    failing_armed = false;
}

static void write_err_u64(uint64_t value) {
    uint8_t buffer[20];
    size_t start = format_u64(value, buffer);
    nova_platform_write_stderr(buffer + start, 20u - start);
}

/* Format (relied on by the fault-injection sweep): "nova alloc: attempts=<n> live=<m>\n" */
int32_t nova_rt_finish(int32_t code) {
    if (alloc_report) {
        write_err_cstr("nova alloc: attempts=");
        write_err_u64(alloc_attempts);
        write_err_cstr(" live=");
        write_err_u64(alloc_live);
        write_err_cstr("\n");
    }
    return code;
}

/* ---- Arena (type-system.md §19.2) ---------------------------------------------------------- */

#define NV_ARENA_HEADER 16u /* the previous-chunk link, padded so data starts 16-aligned */

nv_arena nova_arena_create(uint32_t allocator, uint64_t chunk_size) {
    nv_arena a = {NULL, NULL, NULL, chunk_size, allocator};
    return a;
}

void *nova_arena_alloc(nv_arena *a, uint64_t size, uint64_t align) {
    uintptr_t cur = (uintptr_t)a->cur;
    uintptr_t aligned = (cur + (uintptr_t)(align - 1u)) & ~(uintptr_t)(align - 1u);
    if (a->cur != NULL && aligned >= cur && size <= (uint64_t)((uintptr_t)a->end - aligned)) {
        a->cur = (uint8_t *)(aligned + (uintptr_t)size);
        return (void *)aligned;
    }
    /* A new chunk: the usual size, or larger for a large allocation. */
    uint64_t need = size + NV_ARENA_HEADER;
    if (need < size) {
        return NULL; /* overflow */
    }
    uint64_t total = need > a->chunk_size ? need : a->chunk_size;
#if SIZE_MAX < UINT64_MAX
    if (total > (uint64_t)SIZE_MAX) {
        return NULL;
    }
#endif
    uint8_t *chunk = (uint8_t *)nova_alloc((size_t)total, (uint32_t)a->allocator);
    if (chunk == NULL) {
        return NULL;
    }
    *(void **)(void *)chunk = a->chunks;
    a->chunks = chunk;
    a->end = chunk + total;
    uint8_t *data = chunk + NV_ARENA_HEADER; /* 16-aligned: malloc memory is, and align <= 16 */
    a->cur = data + size;
    return data;
}

void nova_arena_reset(nv_arena *a) {
    void *chunk = a->chunks;
    while (chunk != NULL) {
        void *previous = *(void **)chunk;
        nova_free(chunk, (uint32_t)a->allocator);
        chunk = previous;
    }
    a->chunks = NULL;
    a->cur = NULL;
    a->end = NULL;
}

void nova_arena_drop(nv_arena *a) {
    nova_arena_reset(a);
}

/* ---- Scratch frames (type-system.md §19.5; ADR 0004) -------------------------------------- */

#define NV_SCRATCH_CHUNK 65536u

static void *scratch_chunk;   /* newest chunk; each starts with the previous chunk's address */
static uint8_t *scratch_cur;
static uint8_t *scratch_end;
static uint64_t scratch_depth; /* live frames */

nv_scratch nova_scratch_enter_root(nv_loc loc) {
    if (scratch_depth != 0u) {
        nova_panic(NV_PANIC_SCRATCH, loc);
    }
    scratch_depth = 1u;
    nv_scratch frame = {scratch_chunk, scratch_cur, scratch_end, 1u};
    return frame;
}

nv_scratch nova_scratch_enter_child(nv_scratch *parent) {
    scratch_depth = parent->depth + 1u;
    nv_scratch frame = {scratch_chunk, scratch_cur, scratch_end, scratch_depth};
    return frame;
}

void nova_scratch_drop(nv_scratch *frame) {
    /* Frames are strictly LIFO (ADR 0004), so everything newer than the frame's entry state is its own. */
    while (scratch_chunk != frame->chunk) {
        void *previous = *(void **)scratch_chunk;
        nova_free(scratch_chunk, NV_ALLOC_SYSTEM);
        scratch_chunk = previous;
    }
    scratch_cur = frame->cur;
    scratch_end = frame->end;
    scratch_depth = frame->depth - 1u;
}

static void *scratch_alloc(uint64_t size, uint64_t align) {
    uintptr_t cur = (uintptr_t)scratch_cur;
    uintptr_t aligned = (cur + (uintptr_t)(align - 1u)) & ~(uintptr_t)(align - 1u);
    if (scratch_cur != NULL && aligned >= cur && size <= (uint64_t)((uintptr_t)scratch_end - aligned)) {
        scratch_cur = (uint8_t *)(aligned + (uintptr_t)size);
        return (void *)aligned;
    }
    uint64_t need = size + NV_ARENA_HEADER;
    if (need < size) {
        return NULL;
    }
    uint64_t total = need > NV_SCRATCH_CHUNK ? need : NV_SCRATCH_CHUNK;
#if SIZE_MAX < UINT64_MAX
    if (total > (uint64_t)SIZE_MAX) {
        return NULL;
    }
#endif
    uint8_t *chunk = (uint8_t *)nova_alloc((size_t)total, NV_ALLOC_SYSTEM);
    if (chunk == NULL) {
        return NULL;
    }
    *(void **)(void *)chunk = scratch_chunk;
    scratch_chunk = chunk;
    scratch_end = chunk + total;
    scratch_cur = chunk + NV_ARENA_HEADER + size;
    return chunk + NV_ARENA_HEADER;
}

void *nova_region_alloc(void *region, uint32_t kind, uint64_t size, uint64_t align) {
    switch (kind) {
    case NV_REGION_ARENA: return nova_arena_alloc((nv_arena *)region, size, align);
    case NV_REGION_FBA: return fba_alloc((nv_fba *)region, size, align);
    default: return scratch_alloc(size, align);
    }
}

/* ---- Pools (type-system.md §19.6) ---------------------------------------------------------- */

nv_pool nova_pool_create(uint32_t allocator, uint64_t capacity) {
    nv_pool p = {NULL, NULL, capacity, 0u, 0u, allocator};
    return p;
}

void *nova_pool_take(nv_pool *p, uint64_t size) {
    if (p->slots == NULL) {
        /* Slots hold either a value or the free-list link, and start 16-aligned. */
        uint64_t slot = size < sizeof(void *) ? sizeof(void *) : size;
        slot = (slot + 15u) & ~(uint64_t)15u;
        if (p->capacity == 0u || slot > UINT64_MAX / p->capacity) {
            return NULL;
        }
        uint64_t total = slot * p->capacity;
#if SIZE_MAX < UINT64_MAX
        if (total > (uint64_t)SIZE_MAX) {
            return NULL;
        }
#endif
        p->slots = (uint8_t *)nova_alloc((size_t)total, (uint32_t)p->allocator);
        if (p->slots == NULL) {
            return NULL;
        }
        p->slot_size = slot;
    }
    if (p->free != NULL) {
        void *slot = p->free;
        p->free = *(void **)slot;
        return slot;
    }
    if (p->used == p->capacity) {
        return NULL;
    }
    return p->slots + p->slot_size * p->used++;
}

void nova_pool_release(nv_pool *p, void *slot) {
    *(void **)slot = p->free;
    p->free = slot;
}

void nova_pool_drop(nv_pool *p) {
    /* Every box is gone (the borrow checker keeps the pool alive while one is live). */
    if (p->slots != NULL) {
        nova_free(p->slots, (uint32_t)p->allocator);
    }
}
#endif /* NV_FREESTANDING */
