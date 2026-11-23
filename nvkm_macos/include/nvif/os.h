/* SPDX-License-Identifier: MIT
 * macOS Userspace Linux-Kernel Compatibility Layer for Nouveau NVKM/NVIF (GP108)
 */
#ifndef __NOUVEAU_OS_H__
#define __NOUVEAU_OS_H__

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <assert.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>

/* ========================================================================= *
 * 1. Core Linux Integer & Pointer Types                                     *
 * ========================================================================= */
typedef uint8_t   u8;
typedef uint16_t  u16;
typedef uint32_t  u32;
typedef uint64_t  u64;
typedef int8_t    s8;
typedef int16_t   s16;
typedef int32_t   s32;
typedef int64_t   s64;

typedef uint8_t   __u8;
typedef uint16_t  __u16;
typedef uint32_t  __u32;
typedef uint64_t  __u64;
typedef int8_t    __s8;
typedef int16_t   __s16;
typedef int32_t   __s32;
typedef int64_t   __s64;

typedef uint16_t  __le16;
typedef uint32_t  __le32;
typedef uint64_t  __le64;
typedef uint16_t  __be16;
typedef uint32_t  __be32;
typedef uint64_t  __be64;

typedef uint64_t  dma_addr_t;
typedef uint64_t  phys_addr_t;
typedef uint64_t  resource_size_t;
typedef uint32_t  gfp_t;
typedef unsigned long irq_flags_t;

#ifndef __iomem
#define __iomem
#endif
#ifndef __user
#define __user
#endif
#ifndef __force
#define __force
#endif
#ifndef __acquires
#define __acquires(x)
#endif
#ifndef __releases
#define __releases(x)
#endif
#ifndef __must_hold
#define __must_hold(x)
#endif
#ifndef __must_check
#define __must_check
#endif
#ifndef __maybe_unused
#define __maybe_unused __attribute__((unused))
#endif
#ifndef __always_inline
#define __always_inline inline __attribute__((always_inline))
#endif
#ifndef __packed
#define __packed __attribute__((packed))
#endif
#ifndef __aligned
#define __aligned(x) __attribute__((aligned(x)))
#endif
#ifndef __printf
#define __printf(a, b) __attribute__((format(printf, a, b)))
#endif
#ifndef noinline
#define noinline __attribute__((noinline))
#endif
#ifndef fallthrough
#define fallthrough __attribute__((fallthrough))
#endif
#ifndef likely
#define likely(x)   __builtin_expect(!!(x), 1)
#endif
#ifndef unlikely
#define unlikely(x) __builtin_expect(!!(x), 0)
#endif

#ifndef PAGE_SHIFT
#define PAGE_SHIFT 12
#endif
#ifndef PAGE_SIZE
#define PAGE_SIZE  (1UL << PAGE_SHIFT)
#endif
#ifndef PAGE_MASK
#define PAGE_MASK  (~(PAGE_SIZE - 1))
#endif

#define HZ 1000
#define jiffies ({ struct timespec _ts; clock_gettime(CLOCK_MONOTONIC, &_ts); (unsigned long)(_ts.tv_sec * 1000UL + _ts.tv_nsec / 1000000UL); })
#define msecs_to_jiffies(m) ((unsigned long)(m))
#define usecs_to_jiffies(u) ((unsigned long)(((u) + 999) / 1000))
#define time_after(a, b)    ((long)((b) - (a)) < 0)
#define time_before(a, b)   time_after(b, a)

/* Missing errno values on Darwin */
#ifndef ENOTSUPP
#define ENOTSUPP 524
#endif
#ifndef EREMOTEIO
#define EREMOTEIO 121
#endif
#ifndef EBADRQC
#define EBADRQC 56
#endif
#ifndef ETIME
#define ETIME ETIMEDOUT
#endif

#define MAX_ERRNO 4095
#define IS_ERR_VALUE(x) unlikely((unsigned long)(void *)(x) >= (unsigned long)-MAX_ERRNO)
static inline void *ERR_PTR(long error) { return (void *)error; }
static inline long  PTR_ERR(const void *ptr) { return (long)ptr; }
static inline bool  IS_ERR(const void *ptr) { return IS_ERR_VALUE((unsigned long)ptr); }
static inline bool  IS_ERR_OR_NULL(const void *ptr) { return !ptr || IS_ERR(ptr); }
static inline int   PTR_ERR_OR_ZERO(const void *ptr) { return IS_ERR(ptr) ? (int)PTR_ERR(ptr) : 0; }
static inline void *ERR_CAST(const void *ptr) { return (void *)ptr; }

/* ========================================================================= *
 * 2. Compiler, Container, Math & Bitwise Macros                             *
 * ========================================================================= */
#define CONFIG_NOUVEAU_DEBUG         5
#define CONFIG_NOUVEAU_DEBUG_DEFAULT 3

#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))
#define BUILD_BUG_ON(cond) _Static_assert(!(cond), "BUILD_BUG_ON failed")
#define BUILD_BUG_ON_ZERO(e) (0)
#define IS_ENABLED(cfg) 0

#ifndef __struct_group
#define __struct_group(TAG, NAME, ATTRS, MEMBERS...) \
    union { \
        struct { MEMBERS } ATTRS; \
        struct TAG { MEMBERS } ATTRS NAME; \
    } ATTRS
#endif
#ifndef struct_group
#define struct_group(NAME, MEMBERS...) __struct_group(/* no tag */, NAME, /* no attrs */, MEMBERS)
#endif
#ifndef struct_group_tagged
#define struct_group_tagged(TAG, NAME, MEMBERS...) __struct_group(TAG, NAME, /* no attrs */, MEMBERS)
#endif

#define check_add_overflow(a, b, d) __builtin_add_overflow(a, b, d)
#define check_sub_overflow(a, b, d) __builtin_sub_overflow(a, b, d)
#define check_mul_overflow(a, b, d) __builtin_mul_overflow(a, b, d)
#define array_size(a, b)            ((size_t)(a) * (size_t)(b))
#define array3_size(a, b, c)        ((size_t)(a) * (size_t)(b) * (size_t)(c))

#define typeof_member(T, m) typeof(((T *)0)->m)
#ifndef container_of
#define container_of(ptr, type, member) ({                  \
    void *__mptr = (void *)(ptr);                           \
    ((type *)(__mptr - offsetof(type, member)));            \
})
#endif

#define struct_size(p, member, count) (sizeof(*(p)) + sizeof((p)->member[0]) * (size_t)(count))

#define BIT(nr)             (1UL << (nr))
#define BIT_ULL(nr)         (1ULL << (nr))
#define GENMASK(h, l)       (((~0UL) - (1UL << (l)) + 1) & (~0UL >> (64 - 1 - (h))))
#define GENMASK_ULL(h, l)   (((~0ULL) - (1ULL << (l)) + 1) & (~0ULL >> (64 - 1 - (h))))
#define DMA_BIT_MASK(n)     (((n) == 64) ? ~0ULL : ((1ULL << (n)) - 1))

#define upper_32_bits(n)    ((u32)(((n) >> 16) >> 16))
#define lower_32_bits(n)    ((u32)((n) & 0xffffffffU))

#define ALIGN(x, a)         (((x) + ((typeof(x))(a) - 1)) & ~((typeof(x))(a) - 1))
#define ALIGN_DOWN(x, a)    ((x) & ~((typeof(x))(a) - 1))
#define IS_ALIGNED(x, a)    (((x) & ((typeof(x))(a) - 1)) == 0)
#define round_up(x, y)      ((((x) - 1) | ((typeof(x))(y) - 1)) + 1)
#define round_down(x, y)    ((x) & ~((typeof(x))(y) - 1))
#define roundup(x, y)       ((((x) + ((y) - 1)) / (y)) * (y))
#define rounddown(x, y)     ((x) - ((x) % (y)))

#define DIV_ROUND_UP(n, d)       (((n) + (d) - 1) / (d))
#define DIV_ROUND_UP_ULL(ll, d)  DIV_ROUND_UP((unsigned long long)(ll), (unsigned long long)(d))
#define DIV_ROUND_CLOSEST(x, divisor) ({            \
    typeof(x) __x = (x);                            \
    typeof(divisor) __d = (divisor);                \
    (((typeof(x))-1) > 0 ||                         \
     ((typeof(divisor))-1) > 0 ||                   \
     (((__x) > 0) == ((__d) > 0))) ?                \
        (((__x) + ((__d) / 2)) / (__d)) :           \
        (((__x) - ((__d) / 2)) / (__d));            \
})
#define DIV_ROUND_CLOSEST_ULL(x, d) DIV_ROUND_CLOSEST((unsigned long long)(x), (unsigned long long)(d))

static inline u64 div_u64_rem(u64 dividend, u32 divisor, u32 *remainder) {
    *remainder = (u32)(dividend % divisor);
    return dividend / divisor;
}
static inline u64 div_u64(u64 dividend, u32 divisor) { return dividend / divisor; }
static inline u64 div64_u64(u64 dividend, u64 divisor) { return dividend / divisor; }
static inline s64 div64_s64(s64 dividend, s64 divisor) { return dividend / divisor; }
static inline u64 mul_u32_u32(u32 a, u32 b) { return (u64)a * (u64)b; }
#define do_div(n, base) ({                          \
    uint32_t __base = (base);                       \
    uint32_t __rem = (uint64_t)(n) % __base;        \
    (n) = (uint64_t)(n) / __base;                   \
    __rem;                                          \
})

#ifndef min
#define min(x, y) ({ typeof(x) _x = (x); typeof(y) _y = (y); _x < _y ? _x : _y; })
#endif
#ifndef max
#define max(x, y) ({ typeof(x) _x = (x); typeof(y) _y = (y); _x > _y ? _x : _y; })
#endif
#define min3(x, y, z) min((typeof(x))min(x, y), z)
#define max3(x, y, z) max((typeof(x))max(x, y), z)
#define min_t(type, x, y) ({ type __x = (x); type __y = (y); __x < __y ? __x : __y; })
#define max_t(type, x, y) ({ type __x = (x); type __y = (y); __x > __y ? __x : __y; })
#define clamp(val, lo, hi) min((typeof(val))max(val, lo), hi)
#define clamp_t(type, val, lo, hi) min_t(type, max_t(type, val, lo), hi)
#define swap(a, b) do { typeof(a) __tmp = (a); (a) = (b); (b) = __tmp; } while (0)

static inline unsigned long __ffs(unsigned long word) { return (unsigned long)__builtin_ctzl(word); }
static inline unsigned long __fls(unsigned long word) { return (unsigned long)(63 - __builtin_clzl(word)); }
static inline unsigned long __ffs64(u64 word)         { return (unsigned long)__builtin_ctzll(word); }
static inline int fls64(u64 x)                        { return x ? (64 - __builtin_clzll(x)) : 0; }
static inline unsigned int hweight8(unsigned int w)   { return (unsigned int)__builtin_popcount(w & 0xffU); }
static inline unsigned int hweight16(unsigned int w)  { return (unsigned int)__builtin_popcount(w & 0xffffU); }
static inline unsigned int hweight32(unsigned int w)  { return (unsigned int)__builtin_popcount(w); }
static inline unsigned long hweight64(u64 w)          { return (unsigned long)__builtin_popcountll(w); }
static inline unsigned int hweight_long(unsigned long w) { return (unsigned int)__builtin_popcountl(w); }

#define ilog2(n) ((int)(63 - __builtin_clzll((uint64_t)(n))))
static inline unsigned long roundup_pow_of_two(unsigned long n) {
    return (n <= 1) ? 1UL : (1UL << fls64(n - 1));
}
static inline unsigned long rounddown_pow_of_two(unsigned long n) {
    return (1UL << (fls64(n) - 1));
}
static inline int order_base_2(unsigned long n) {
    return (n <= 1) ? 0 : fls64(n - 1);
}
static inline bool is_power_of_2(unsigned long n) {
    return (n != 0 && ((n & (n - 1)) == 0));
}

static inline void __set_bit(unsigned long nr, volatile unsigned long *addr) {
    addr[nr / 64] |= (1UL << (nr % 64));
}
static inline void __clear_bit(unsigned long nr, volatile unsigned long *addr) {
    addr[nr / 64] &= ~(1UL << (nr % 64));
}
static inline void set_bit(unsigned long nr, volatile unsigned long *addr) {
    __atomic_fetch_or(&addr[nr / 64], 1UL << (nr % 64), __ATOMIC_SEQ_CST);
}
static inline void clear_bit(unsigned long nr, volatile unsigned long *addr) {
    __atomic_fetch_and(&addr[nr / 64], ~(1UL << (nr % 64)), __ATOMIC_SEQ_CST);
}
static inline int test_bit(unsigned long nr, const volatile unsigned long *addr) {
    return (addr[nr / 64] >> (nr % 64)) & 1UL;
}
static inline int test_and_set_bit(unsigned long nr, volatile unsigned long *addr) {
    unsigned long mask = 1UL << (nr % 64);
    unsigned long old = __atomic_fetch_or(&addr[nr / 64], mask, __ATOMIC_SEQ_CST);
    return (old & mask) != 0;
}
static inline int test_and_clear_bit(unsigned long nr, volatile unsigned long *addr) {
    unsigned long mask = 1UL << (nr % 64);
    unsigned long old = __atomic_fetch_and(&addr[nr / 64], ~mask, __ATOMIC_SEQ_CST);
    return (old & mask) != 0;
}
#define BITS_TO_LONGS(nr) DIV_ROUND_UP(nr, 64)
#define DECLARE_BITMAP(name, bits) unsigned long name[BITS_TO_LONGS(bits)]
static inline void bitmap_zero(unsigned long *dst, unsigned int nbits) {
    memset(dst, 0, DIV_ROUND_UP(nbits, 64) * sizeof(unsigned long));
}
static inline void bitmap_fill(unsigned long *dst, unsigned int nbits) {
    size_t nlongs = DIV_ROUND_UP(nbits, 64);
    memset(dst, 0xff, nlongs * sizeof(unsigned long));
    if (nbits % 64) dst[nlongs - 1] &= (1UL << (nbits % 64)) - 1;
}
static inline unsigned long find_first_zero_bit(const unsigned long *addr, unsigned long size) {
    for (unsigned long i = 0; i < size; i++) {
        if (!test_bit(i, addr)) return i;
    }
    return size;
}
static inline unsigned long find_first_bit(const unsigned long *addr, unsigned long size) {
    for (unsigned long i = 0; i < size; i++) {
        if (test_bit(i, addr)) return i;
    }
    return size;
}
static inline unsigned long find_next_bit(const unsigned long *addr, unsigned long size, unsigned long offset) {
    for (unsigned long i = offset; i < size; i++) {
        if (test_bit(i, addr)) return i;
    }
    return size;
}
static inline unsigned long find_next_zero_bit(const unsigned long *addr, unsigned long size, unsigned long offset) {
    for (unsigned long i = offset; i < size; i++) {
        if (!test_bit(i, addr)) return i;
    }
    return size;
}
static inline void bitmap_clear(unsigned long *map, unsigned int start, int len) {
    for (int i = 0; i < len; i++) __clear_bit(start + i, map);
}
static inline void bitmap_set(unsigned long *map, unsigned int start, int len) {
    for (int i = 0; i < len; i++) __set_bit(start + i, map);
}
#define for_each_set_bit(bit, addr, size) \
    for ((bit) = find_first_bit((addr), (size)); (bit) < (size); (bit) = find_next_bit((addr), (size), (bit) + 1))

/* Byteorder & Unaligned (x86_64 is little-endian) */
#define cpu_to_le16(x) ((__le16)(u16)(x))
#define cpu_to_le32(x) ((__le32)(u32)(x))
#define cpu_to_le64(x) ((__le64)(u64)(x))
#define le16_to_cpu(x) ((u16)(x))
#define le32_to_cpu(x) ((u32)(x))
#define le64_to_cpu(x) ((u64)(x))
#define cpu_to_be16(x) __builtin_bswap16(x)
#define cpu_to_be32(x) __builtin_bswap32(x)
#define cpu_to_be64(x) __builtin_bswap64(x)
#define be16_to_cpu(x) __builtin_bswap16(x)
#define be32_to_cpu(x) __builtin_bswap32(x)
#define be64_to_cpu(x) __builtin_bswap64(x)

static inline u16 get_unaligned_le16(const void *p) { u16 v; memcpy(&v, p, 2); return v; }
static inline u32 get_unaligned_le32(const void *p) { u32 v; memcpy(&v, p, 4); return v; }
static inline u64 get_unaligned_le64(const void *p) { u64 v; memcpy(&v, p, 8); return v; }
static inline u16 get_unaligned_be16(const void *p) { u16 v; memcpy(&v, p, 2); return __builtin_bswap16(v); }
static inline u32 get_unaligned_be32(const void *p) { u32 v; memcpy(&v, p, 4); return __builtin_bswap32(v); }
static inline void put_unaligned_le16(u16 v, void *p) { memcpy(p, &v, 2); }
static inline void put_unaligned_le32(u32 v, void *p) { memcpy(p, &v, 4); }
static inline void put_unaligned_le64(u64 v, void *p) { memcpy(p, &v, 8); }

static inline u32 crc32_le(u32 crc, unsigned char const *p, size_t len) {
    crc = ~crc;
    while (len--) {
        crc ^= *p++;
        for (int i = 0; i < 8; i++)
            crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320U : 0);
    }
    return ~crc;
}
#define crc32(seed, data, length) crc32_le(seed, (const unsigned char *)(data), length)

/* ========================================================================= *
 * 3. Intrusive Doubly-Linked List & Red-Black Tree                          *
 * ========================================================================= */
struct list_head {
    struct list_head *next, *prev;
};

#define LIST_HEAD_INIT(name) { &(name), &(name) }
#define LIST_HEAD(name) struct list_head name = LIST_HEAD_INIT(name)

static inline void INIT_LIST_HEAD(struct list_head *list) {
    list->next = list;
    list->prev = list;
}
static inline void __list_add(struct list_head *n, struct list_head *prev, struct list_head *next) {
    next->prev = n;
    n->next = next;
    n->prev = prev;
    prev->next = n;
}
static inline void list_add(struct list_head *n, struct list_head *head) {
    __list_add(n, head, head->next);
}
static inline void list_add_tail(struct list_head *n, struct list_head *head) {
    __list_add(n, head->prev, head);
}
static inline void __list_del(struct list_head *prev, struct list_head *next) {
    next->prev = prev;
    prev->next = next;
}
static inline void list_del(struct list_head *entry) {
    __list_del(entry->prev, entry->next);
    entry->next = NULL;
    entry->prev = NULL;
}
static inline void list_del_init(struct list_head *entry) {
    __list_del(entry->prev, entry->next);
    INIT_LIST_HEAD(entry);
}
static inline void list_move(struct list_head *list, struct list_head *head) {
    __list_del(list->prev, list->next);
    list_add(list, head);
}
static inline void list_move_tail(struct list_head *list, struct list_head *head) {
    __list_del(list->prev, list->next);
    list_add_tail(list, head);
}
static inline int list_empty(const struct list_head *head) {
    return head->next == head;
}
static inline void list_rotate_left(struct list_head *head) {
    if (!list_empty(head))
        list_move_tail(head->next, head);
}
static inline int list_is_singular(const struct list_head *head) {
    return !list_empty(head) && (head->next == head->prev);
}
static inline bool list_is_head(const struct list_head *list, const struct list_head *head) {
    return list == head;
}

#define list_entry(ptr, type, member) container_of(ptr, type, member)
#define list_first_entry(ptr, type, member) list_entry((ptr)->next, type, member)
#define list_first_entry_or_null(ptr, type, member) ({ \
    struct list_head *head__ = (ptr); \
    struct list_head *pos__ = head__->next; \
    pos__ != head__ ? list_entry(pos__, type, member) : NULL; \
})
#define list_last_entry(ptr, type, member) list_entry((ptr)->prev, type, member)
#define list_next_entry(pos, member) list_entry((pos)->member.next, typeof(*(pos)), member)
#define list_prev_entry(pos, member) list_entry((pos)->member.prev, typeof(*(pos)), member)

#define list_for_each(pos, head) \
    for (pos = (head)->next; pos != (head); pos = pos->next)
#define list_for_each_safe(pos, n, head) \
    for (pos = (head)->next, n = pos->next; pos != (head); pos = n, n = pos->next)
#define list_for_each_entry(pos, head, member) \
    for (pos = list_first_entry(head, typeof(*pos), member); \
         !list_is_head(&pos->member, (head)); \
         pos = list_next_entry(pos, member))
#define list_for_each_entry_reverse(pos, head, member) \
    for (pos = list_last_entry(head, typeof(*pos), member); \
         !list_is_head(&pos->member, (head)); \
         pos = list_prev_entry(pos, member))
#define list_for_each_entry_safe(pos, n, head, member) \
    for (pos = list_first_entry(head, typeof(*pos), member), \
         n = list_next_entry(pos, member); \
         !list_is_head(&pos->member, (head)); \
         pos = n, n = list_next_entry(n, member))
#define list_for_each_entry_safe_from(pos, n, head, member) \
    for (n = list_next_entry(pos, member); \
         !list_is_head(&pos->member, (head)); \
         pos = n, n = list_next_entry(n, member))
#define list_for_each_entry_continue(pos, head, member) \
    for (pos = list_next_entry(pos, member); \
         !list_is_head(&pos->member, (head)); \
         pos = list_next_entry(pos, member))
#define list_for_each_entry_continue_reverse(pos, head, member) \
    for (pos = list_prev_entry(pos, member); \
         !list_is_head(&pos->member, (head)); \
         pos = list_prev_entry(pos, member))
#define list_for_each_entry_safe_reverse(pos, n, head, member) \
    for (pos = list_last_entry(head, typeof(*pos), member), \
         n = list_prev_entry(pos, member); \
         !list_is_head(&pos->member, (head)); \
         pos = n, n = list_prev_entry(n, member))
#define list_for_each_entry_from(pos, head, member) \
    for (; !list_is_head(&pos->member, (head)); \
         pos = list_next_entry(pos, member))

/* Red-Black Tree */
struct rb_node {
    unsigned long   __rb_parent_color;
    struct rb_node *rb_right;
    struct rb_node *rb_left;
} __attribute__((aligned(sizeof(long))));

struct rb_root {
    struct rb_node *rb_node;
};

#define RB_ROOT (struct rb_root) { NULL, }
#define rb_entry(ptr, type, member) container_of(ptr, type, member)
#define RB_EMPTY_ROOT(root)  ((root)->rb_node == NULL)
#define RB_EMPTY_NODE(node)  ((node)->__rb_parent_color == (unsigned long)(node))
#define RB_CLEAR_NODE(node)  ((node)->__rb_parent_color = (unsigned long)(node))

static inline void rb_link_node(struct rb_node *node, struct rb_node *parent, struct rb_node **rb_link) {
    node->__rb_parent_color = (unsigned long)parent;
    node->rb_left = node->rb_right = NULL;
    *rb_link = node;
}

void rb_insert_color(struct rb_node *node, struct rb_root *root);
void rb_erase(struct rb_node *node, struct rb_root *root);
struct rb_node *rb_first(const struct rb_root *root);
struct rb_node *rb_last(const struct rb_root *root);
struct rb_node *rb_next(const struct rb_node *node);
struct rb_node *rb_prev(const struct rb_node *node);

#define rbtree_postorder_for_each_entry_safe(pos, n, root, field) \
    for (pos = rb_first(root) ? rb_entry(rb_first(root), typeof(*pos), field) : NULL, \
         n = (pos && rb_next(&pos->field)) ? rb_entry(rb_next(&pos->field), typeof(*pos), field) : NULL; \
         pos != NULL; \
         pos = n, n = (pos && rb_next(&pos->field)) ? rb_entry(rb_next(&pos->field), typeof(*pos), field) : NULL)

/* ========================================================================= *
 * 4. Atomics, Refcounts, Locks & Waitqueues                                 *
 * ========================================================================= */
typedef struct { volatile int counter; } atomic_t;
typedef struct { volatile int64_t counter; } atomic64_t;

#define ATOMIC_INIT(i) { (i) }
static inline int  atomic_read(const atomic_t *v) { return __atomic_load_n(&v->counter, __ATOMIC_ACQUIRE); }
static inline void atomic_set(atomic_t *v, int i) { __atomic_store_n(&v->counter, i, __ATOMIC_RELEASE); }
static inline void atomic_inc(atomic_t *v)        { __atomic_add_fetch(&v->counter, 1, __ATOMIC_SEQ_CST); }
static inline void atomic_dec(atomic_t *v)        { __atomic_sub_fetch(&v->counter, 1, __ATOMIC_SEQ_CST); }
static inline int  atomic_inc_return(atomic_t *v) { return __atomic_add_fetch(&v->counter, 1, __ATOMIC_SEQ_CST); }
static inline int  atomic_dec_return(atomic_t *v) { return __atomic_sub_fetch(&v->counter, 1, __ATOMIC_SEQ_CST); }
static inline bool atomic_dec_and_test(atomic_t *v) { return __atomic_sub_fetch(&v->counter, 1, __ATOMIC_SEQ_CST) == 0; }
static inline void atomic_add(int i, atomic_t *v) { __atomic_add_fetch(&v->counter, i, __ATOMIC_SEQ_CST); }
static inline int  atomic_add_return(int i, atomic_t *v) { return __atomic_add_fetch(&v->counter, i, __ATOMIC_SEQ_CST); }
static inline int  atomic_xchg(atomic_t *v, int n) { return __atomic_exchange_n(&v->counter, n, __ATOMIC_SEQ_CST); }
static inline int  atomic_cmpxchg(atomic_t *v, int old, int n) {
    __atomic_compare_exchange_n(&v->counter, &old, n, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return old;
}

typedef struct { atomic_t refs; } refcount_t;
static inline void refcount_set(refcount_t *r, int n) { atomic_set(&r->refs, n); }
static inline unsigned int refcount_read(const refcount_t *r) { return (unsigned int)atomic_read(&r->refs); }
static inline void refcount_inc(refcount_t *r) { atomic_inc(&r->refs); }
static inline bool refcount_dec_and_test(refcount_t *r) { return atomic_dec_and_test(&r->refs); }

struct kref { refcount_t refcount; };
static inline void kref_init(struct kref *kref) { refcount_set(&kref->refcount, 1); }
static inline void kref_get(struct kref *kref)  { refcount_inc(&kref->refcount); }
static inline int  kref_put(struct kref *kref, void (*release)(struct kref *kref)) {
    if (refcount_dec_and_test(&kref->refcount)) {
        release(kref);
        return 1;
    }
    return 0;
}

struct lock_class_key { int dummy; };

struct mutex { pthread_mutex_t m; bool inited; volatile int depth; };
#define DEFINE_MUTEX(name) struct mutex name = { .inited = false, .depth = 0 }
static inline void mutex_init(struct mutex *lock) {
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&lock->m, &attr);
    pthread_mutexattr_destroy(&attr);
    lock->depth = 0;
    lock->inited = true;
}
#define __mutex_init(l, n, k) mutex_init(l)
static inline void mutex_destroy(struct mutex *lock) {
    if (lock->inited) { pthread_mutex_destroy(&lock->m); lock->inited = false; lock->depth = 0; }
}
static inline void mutex_lock(struct mutex *lock) {
    if (!lock->inited) mutex_init(lock);
    pthread_mutex_lock(&lock->m);
    __sync_add_and_fetch(&lock->depth, 1);
}
#define mutex_lock_nested(l, s) mutex_lock(l)
static inline int mutex_trylock(struct mutex *lock) {
    if (!lock->inited) mutex_init(lock);
    if (pthread_mutex_trylock(&lock->m) == 0) {
        __sync_add_and_fetch(&lock->depth, 1);
        return 1;
    }
    return 0;
}
static inline void mutex_unlock(struct mutex *lock) {
    __sync_sub_and_fetch(&lock->depth, 1);
    pthread_mutex_unlock(&lock->m);
}

static inline bool refcount_inc_not_zero(refcount_t *r) {
    int old = atomic_read(&r->refs);
    while (old > 0) {
        if (atomic_cmpxchg(&r->refs, old, old + 1) == old)
            return true;
        old = atomic_read(&r->refs);
    }
    return false;
}
static inline bool refcount_dec_and_mutex_lock(refcount_t *r, struct mutex *lock) {
    int old = atomic_read(&r->refs);
    while (old > 1) {
        if (atomic_cmpxchg(&r->refs, old, old - 1) == old)
            return false;
        old = atomic_read(&r->refs);
    }
    mutex_lock(lock);
    if (atomic_dec_and_test(&r->refs))
        return true;
    mutex_unlock(lock);
    return false;
}

struct completion {
    unsigned int    done;
    pthread_mutex_t m;
    pthread_cond_t  c;
    bool            inited;
};
static inline void init_completion(struct completion *x) {
    x->done = 0;
    pthread_mutex_init(&x->m, NULL);
    pthread_cond_init(&x->c, NULL);
    x->inited = true;
}
static inline void reinit_completion(struct completion *x) {
    if (!x->inited) init_completion(x);
    pthread_mutex_lock(&x->m);
    x->done = 0;
    pthread_mutex_unlock(&x->m);
}
static inline void complete(struct completion *x) {
    if (!x->inited) init_completion(x);
    pthread_mutex_lock(&x->m);
    if (x->done != UINT_MAX) x->done++;
    pthread_cond_signal(&x->c);
    pthread_mutex_unlock(&x->m);
}
static inline void complete_all(struct completion *x) {
    if (!x->inited) init_completion(x);
    pthread_mutex_lock(&x->m);
    x->done = UINT_MAX;
    pthread_cond_broadcast(&x->c);
    pthread_mutex_unlock(&x->m);
}
static inline void wait_for_completion(struct completion *x) {
    if (!x->inited) init_completion(x);
    pthread_mutex_lock(&x->m);
    while (!x->done)
        pthread_cond_wait(&x->c, &x->m);
    if (x->done != UINT_MAX) x->done--;
    pthread_mutex_unlock(&x->m);
}
static inline unsigned long wait_for_completion_timeout(struct completion *x, unsigned long timeout_ms) {
    if (!x->inited) init_completion(x);
    struct timespec start_ts, now_ts, abs_ts;
    clock_gettime(CLOCK_REALTIME, &abs_ts);
    clock_gettime(CLOCK_MONOTONIC, &start_ts);
    abs_ts.tv_sec  += (time_t)(timeout_ms / 1000UL);
    abs_ts.tv_nsec += (long)((timeout_ms % 1000UL) * 1000000UL);
    if (abs_ts.tv_nsec >= 1000000000L) {
        abs_ts.tv_sec++;
        abs_ts.tv_nsec -= 1000000000L;
    }
    pthread_mutex_lock(&x->m);
    int rc = 0;
    while (!x->done && rc == 0)
        rc = pthread_cond_timedwait(&x->c, &x->m, &abs_ts);
    bool ok = (x->done != 0);
    if (ok && x->done != UINT_MAX) x->done--;
    pthread_mutex_unlock(&x->m);
    if (!ok) return 0UL;
    clock_gettime(CLOCK_MONOTONIC, &now_ts);
    long elapsed_ms = (now_ts.tv_sec - start_ts.tv_sec) * 1000L +
                      (now_ts.tv_nsec - start_ts.tv_nsec) / 1000000L;
    if (elapsed_ms < 0) elapsed_ms = 0;
    return ((unsigned long)elapsed_ms >= timeout_ms) ? 1UL : (timeout_ms - (unsigned long)elapsed_ms);
}
static inline bool mutex_is_locked(struct mutex *lock) {
    return lock->inited && __sync_fetch_and_add(&lock->depth, 0) > 0;
}

typedef struct { pthread_mutex_t m; bool inited; } spinlock_t;
#define DEFINE_SPINLOCK(name) spinlock_t name = { .inited = false }
static inline void spin_lock_init(spinlock_t *lock) {
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&lock->m, &attr);
    pthread_mutexattr_destroy(&attr);
    lock->inited = true;
}
static inline void spin_lock(spinlock_t *lock) {
    if (!lock->inited) spin_lock_init(lock);
    pthread_mutex_lock(&lock->m);
}
static inline void spin_unlock(spinlock_t *lock) { pthread_mutex_unlock(&lock->m); }
#define spin_lock_irq(l)              spin_lock(l)
#define spin_unlock_irq(l)            spin_unlock(l)
#define spin_lock_irqsave(l, flags)   do { (flags) = 0; (void)(flags); spin_lock(l); } while (0)
#define spin_unlock_irqrestore(l, f)  do { (void)(f); spin_unlock(l); } while (0)
#define assert_spin_locked(l)         do { (void)(l); } while (0)

typedef spinlock_t rwlock_t;
#define rwlock_init(l)                spin_lock_init(l)
#define read_lock(l)                  spin_lock(l)
#define read_unlock(l)                spin_unlock(l)
#define read_lock_irq(l)              spin_lock(l)
#define read_unlock_irq(l)            spin_unlock(l)
#define read_lock_irqsave(l, f)       spin_lock_irqsave(l, f)
#define read_unlock_irqrestore(l, f)  spin_unlock_irqrestore(l, f)
#define write_lock(l)                 spin_lock(l)
#define write_unlock(l)               spin_unlock(l)
#define write_lock_irq(l)             spin_lock(l)
#define write_unlock_irq(l)           spin_unlock(l)
#define write_lock_irqsave(l, f)      spin_lock_irqsave(l, f)
#define write_unlock_irqrestore(l, f) spin_unlock_irqrestore(l, f)

struct idr { int dummy; };
static inline void idr_init(struct idr *idp) { (void)idp; }
static inline void idr_destroy(struct idr *idp) { (void)idp; }

typedef struct {
    pthread_mutex_t m;
    pthread_cond_t  c;
    bool            inited;
} wait_queue_head_t;

static inline void init_waitqueue_head(wait_queue_head_t *q) {
    pthread_mutex_init(&q->m, NULL);
    pthread_cond_init(&q->c, NULL);
    q->inited = true;
}
static inline void wake_up(wait_queue_head_t *q) {
    if (!q->inited) init_waitqueue_head(q);
    pthread_mutex_lock(&q->m);
    pthread_cond_broadcast(&q->c);
    pthread_mutex_unlock(&q->m);
}
#define wake_up_all(q) wake_up(q)
#define wait_event(wq, condition) do { \
    while (!(condition)) usleep(200); \
} while (0)
#define wait_event_timeout(wq, condition, timeout_ms) ({ \
    long __rem = (long)(timeout_ms); \
    while (!(condition) && __rem > 0) { \
        usleep(1000); \
        __rem--; \
    } \
    (condition) ? (__rem > 0 ? __rem : 1L) : 0L; \
})

struct notifier_block {
    int (*notifier_call)(struct notifier_block *, unsigned long, void *);
    struct notifier_block *next;
    int priority;
};

struct work_struct {
    void (*func)(struct work_struct *);
};
#define INIT_WORK(w, f) do { (w)->func = (f); } while (0)
static inline bool schedule_work(struct work_struct *w) { if (w->func) w->func(w); return true; }
static inline bool flush_work(struct work_struct *w) { (void)w; return true; }
static inline bool cancel_work_sync(struct work_struct *w) { (void)w; return false; }

/* ========================================================================= *
 * 5. Memory Allocation & Strings                                            *
 * ========================================================================= */
#define GFP_KERNEL      0x01U
#define GFP_ATOMIC      0x02U
#define GFP_USER        0x04U
#define GFP_HIGHUSER    0x08U
#define GFP_DMA32       0x10U
#define __GFP_ZERO      0x100U
#define __GFP_NOWARN    0x200U

static inline void *kmalloc(size_t size, gfp_t flags) {
    if (size == 0) return malloc(8);
    void *p = (flags & __GFP_ZERO) ? calloc(1, size) : malloc(size);
    return p;
}
static inline void *kzalloc(size_t size, gfp_t flags) {
    (void)flags;
    return calloc(1, size ? size : 8);
}
static inline void *kcalloc(size_t n, size_t size, gfp_t flags) {
    (void)flags;
    return calloc(n ? n : 1, size ? size : 1);
}
static inline void *kmalloc_array(size_t n, size_t size, gfp_t flags) {
    return kmalloc(n * size, flags);
}
static inline void *krealloc(const void *p, size_t new_size, gfp_t flags) {
    (void)flags;
    if (new_size == 0) { free((void *)p); return NULL; }
    return realloc((void *)p, new_size);
}
static inline void kfree(const void *p) { free((void *)p); }
static inline void *kmemdup(const void *src, size_t len, gfp_t gfp) {
    void *p = kmalloc(len, gfp);
    if (p) memcpy(p, src, len);
    return p;
}
static inline char *kstrdup(const char *s, gfp_t gfp) {
    (void)gfp;
    return s ? strdup(s) : NULL;
}
static inline char *kstrndup(const char *s, size_t max, gfp_t gfp) {
    (void)gfp;
    return s ? strndup(s, max) : NULL;
}
static inline ssize_t strscpy(char *dest, const char *src, size_t count) {
    if (count == 0) return -E2BIG;
    size_t len = strlcpy(dest, src, count);
    return (len >= count) ? -E2BIG : (ssize_t)len;
}
static inline ssize_t strscpy_pad(char *dest, const char *src, size_t count) {
    if (count == 0) return -E2BIG;
    memset(dest, 0, count);
    size_t len = strlcpy(dest, src ? src : "", count);
    return (len >= count) ? -E2BIG : (ssize_t)len;
}
static inline int scnprintf(char *buf, size_t size, const char *fmt, ...) __printf(3, 4);
static inline int scnprintf(char *buf, size_t size, const char *fmt, ...) {
    if (size == 0) return 0;
    va_list ap;
    va_start(ap, fmt);
    int i = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    if (i <= 0) return 0;
    return ((size_t)i >= size) ? (int)(size - 1) : i;
}
static inline char *kasprintf(gfp_t gfp, const char *fmt, ...) __printf(2, 3);
static inline char *kasprintf(gfp_t gfp, const char *fmt, ...) {
    (void)gfp;
    char *p = NULL;
    va_list ap;
    va_start(ap, fmt);
    if (vasprintf(&p, fmt, ap) < 0) p = NULL;
    va_end(ap);
    return p;
}
static inline int kstrtol(const char *s, unsigned int base, long *res) {
    char *end = NULL;
    errno = 0;
    long v = strtol(s, &end, (int)base);
    if (errno || end == s) return -EINVAL;
    *res = v;
    return 0;
}
static inline int kstrtoul(const char *s, unsigned int base, unsigned long *res) {
    char *end = NULL;
    errno = 0;
    unsigned long v = strtoul(s, &end, (int)base);
    if (errno || end == s) return -EINVAL;
    *res = v;
    return 0;
}
static inline int kstrtoint(const char *s, unsigned int base, int *res) {
    long v = 0;
    int r = kstrtol(s, base, &v);
    if (!r) *res = (int)v;
    return r;
}

void *macos_vmalloc(size_t size);
void  macos_vfree(const void *addr);

#define vmalloc(s)              macos_vmalloc(s)
#define vzalloc(s)              macos_vmalloc(s)
#define kvmalloc(s, g)          kmalloc(s, g)
#define kvzalloc(s, g)          kzalloc(s, g)
#define kvmalloc_array(n, s, g) kmalloc_array(n, s, g)
#define kvcalloc(n, s, g)       kcalloc(n, s, g)
#define kvrealloc(p, os, ns, g) krealloc(p, ns, g)
#define vfree(p)                macos_vfree(p)
#define kvfree(p)               macos_vfree(p)

/* ========================================================================= *
 * 6. Logging, Debug, Timing & Module Stubs                                  *
 * ========================================================================= */
#define KERN_EMERG   ""
#define KERN_ALERT   ""
#define KERN_CRIT    ""
#define KERN_ERR     ""
#define KERN_WARNING ""
#define KERN_NOTICE  ""
#define KERN_INFO    ""
#define KERN_DEBUG   ""
#define KERN_CONT    ""

#define printk(fmt, ...)       fprintf(stderr, fmt, ##__VA_ARGS__)
#define pr_err(fmt, ...)       fprintf(stderr, "[nvkm:err] " fmt, ##__VA_ARGS__)
#define pr_warn(fmt, ...)      fprintf(stderr, "[nvkm:wrn] " fmt, ##__VA_ARGS__)
#define pr_info(fmt, ...)      fprintf(stderr, "[nvkm:inf] " fmt, ##__VA_ARGS__)
#define pr_debug(fmt, ...)     do { if (0) fprintf(stderr, fmt, ##__VA_ARGS__); } while (0)

#define dev_crit(dev, fmt, ...)   fprintf(stderr, "[GP108:CRIT] " fmt, ##__VA_ARGS__)
#define dev_err(dev, fmt, ...)    fprintf(stderr, "[GP108:ERR ] " fmt, ##__VA_ARGS__)
#define dev_warn(dev, fmt, ...)   fprintf(stderr, "[GP108:WARN] " fmt, ##__VA_ARGS__)
#define dev_notice(dev, fmt, ...) fprintf(stderr, "[GP108:NOTE] " fmt, ##__VA_ARGS__)
#define dev_info(dev, fmt, ...)   fprintf(stderr, "[GP108:INFO] " fmt, ##__VA_ARGS__)
#define dev_dbg(dev, fmt, ...)    do { if (0) fprintf(stderr, fmt, ##__VA_ARGS__); } while (0)

#define WARN_ON(condition) ({                                               \
    int __ret_warn_on = !!(condition);                                      \
    if (unlikely(__ret_warn_on))                                            \
        fprintf(stderr, "[NVKM WARN_ON] %s:%d: %s\n", __FILE__, __LINE__, #condition); \
    unlikely(__ret_warn_on);                                                \
})
#define WARN_ON_ONCE(cond) WARN_ON(cond)
#define WARN(condition, fmt, ...) ({                                        \
    int __ret_warn_on = !!(condition);                                      \
    if (unlikely(__ret_warn_on))                                            \
        fprintf(stderr, "[NVKM WARN] %s:%d: " fmt, __FILE__, __LINE__, ##__VA_ARGS__); \
    unlikely(__ret_warn_on);                                                \
})
#define WARN_ONCE(cond, fmt, ...) WARN(cond, fmt, ##__VA_ARGS__)
#define dev_WARN(dev, fmt, ...) WARN(1, fmt, ##__VA_ARGS__)
#define dev_WARN_ONCE(dev, cond, fmt, ...) WARN(cond, fmt, ##__VA_ARGS__)
#define dev_err_ratelimited(dev, fmt, ...) dev_err(dev, fmt, ##__VA_ARGS__)
#define BUG() do { fprintf(stderr, "[NVKM BUG] %s:%d\n", __FILE__, __LINE__); abort(); } while (0)
#define BUG_ON(cond) do { if (unlikely(cond)) BUG(); } while (0)

#define MODULE_FIRMWARE(x)
#define MODULE_AUTHOR(x)
#define MODULE_DESCRIPTION(x)
#define MODULE_LICENSE(x)
#define EXPORT_SYMBOL(x)
#define EXPORT_SYMBOL_GPL(x)

static inline u64 ktime_get_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u64)ts.tv_sec * 1000000000ULL + (u64)ts.tv_nsec;
}
typedef s64 ktime_t;
static inline ktime_t ktime_get(void) { return (ktime_t)ktime_get_ns(); }
static inline s64 ktime_to_ns(ktime_t kt) { return kt; }
static inline s64 ktime_to_us(ktime_t kt) { return kt / 1000; }
static inline s64 ktime_us_delta(ktime_t later, ktime_t earlier) { return (later - earlier) / 1000; }

static inline void ndelay(unsigned long nsecs) {
    struct timespec req = { 0, (long)nsecs };
    nanosleep(&req, NULL);
}
static inline void udelay(unsigned long usecs) {
    if (usecs >= 50) usleep((useconds_t)usecs);
    else {
        u64 target = ktime_get_ns() + usecs * 1000ULL;
        while (ktime_get_ns() < target) { __asm__ volatile("pause"); }
    }
}
static inline void usleep_range(unsigned long min_us, unsigned long max_us) {
    (void)max_us;
    udelay(min_us);
}
static inline void mdelay(unsigned long msecs) { usleep((useconds_t)(msecs * 1000UL)); }
static inline void msleep(unsigned int msecs)  { usleep((useconds_t)(msecs * 1000U)); }

/* ========================================================================= *
 * 7. MMIO, PCI, Firmware, Scatter-Gather & DMA Interfaces (macos_os.c)      *
 * ========================================================================= */
#define mb()    __asm__ volatile("mfence" ::: "memory")
#define rmb()   __asm__ volatile("lfence" ::: "memory")
#define wmb()   __asm__ volatile("sfence" ::: "memory")
#define smp_mb()  mb()
#define smp_rmb() rmb()
#define smp_wmb() wmb()

static inline u8   ioread8(const volatile void __iomem *addr)  { u8 v = *(const volatile u8 *)addr; rmb(); return v; }
static inline u16  ioread16(const volatile void __iomem *addr) { u16 v = *(const volatile u16 *)addr; rmb(); return v; }
static inline u32  ioread32(const volatile void __iomem *addr) { u32 v = *(const volatile u32 *)addr; rmb(); return v; }
static inline void iowrite8(u8 v, volatile void __iomem *addr)   { wmb(); *(volatile u8 *)addr = v; wmb(); }
static inline void iowrite16(u16 v, volatile void __iomem *addr) { wmb(); *(volatile u16 *)addr = v; wmb(); }
static inline void iowrite32(u32 v, volatile void __iomem *addr) { wmb(); *(volatile u32 *)addr = v; wmb(); }

#define readb(a)       ioread8(a)
#define readw(a)       ioread16(a)
#define readl(a)       ioread32(a)
#define writeb(v, a)   iowrite8(v, a)
#define writew(v, a)   iowrite16(v, a)
#define writel(v, a)   iowrite32(v, a)

#define ioread16_native  ioread16
#define iowrite16_native iowrite16
#define ioread32_native  ioread32
#define iowrite32_native iowrite32
#define ioread16be(a)    __builtin_bswap16(ioread16(a))
#define ioread32be(a)    __builtin_bswap32(ioread32(a))
#define iowrite16be(v,a) iowrite16(__builtin_bswap16(v), a)
#define iowrite32be(v,a) iowrite32(__builtin_bswap32(v), a)

#define iowrite64_native(v, p) do {                     \
    u32 __iomem *_p = (u32 __iomem *)(p);               \
    u64 _v = (v);                                       \
    iowrite32_native(lower_32_bits(_v), &_p[0]);        \
    iowrite32_native(upper_32_bits(_v), &_p[1]);        \
} while (0)

#define memset_io(a, b, c)     memset((void *)(a), (b), (c))
#define memcpy_fromio(a, b, c) memcpy((a), (const void *)(b), (c))
#define memcpy_toio(a, b, c)   memcpy((void *)(a), (b), (c))

void __iomem *macos_ioremap(phys_addr_t phys, size_t size, bool wc);
void          macos_iounmap(volatile void __iomem *addr);
#define ioremap(phys, size)     macos_ioremap((phys), (size), false)
#define ioremap_wc(phys, size)  macos_ioremap((phys), (size), true)
#define iounmap(addr)           macos_iounmap(addr)

struct io_mapping {
    void __iomem *base;
    resource_size_t size;
};
static inline struct io_mapping *io_mapping_create_wc(resource_size_t base, unsigned long size) {
    void __iomem *p = ioremap_wc(base, size);
    if (!p) return NULL;
    struct io_mapping *m = kzalloc(sizeof(*m), GFP_KERNEL);
    if (!m) return NULL;
    m->base = p;
    m->size = size;
    return m;
}
static inline void io_mapping_free(struct io_mapping *mapping) {
    if (mapping) { kfree(mapping); }
}
static inline void __iomem *io_mapping_map_wc(struct io_mapping *mapping, unsigned long offset, unsigned long size) {
    (void)size;
    return (u8 __iomem *)mapping->base + offset;
}
static inline void io_mapping_unmap(void __iomem *vaddr) { (void)vaddr; }
static inline void __iomem *io_mapping_map_atomic_wc(struct io_mapping *mapping, unsigned long offset) {
    return (u8 __iomem *)mapping->base + offset;
}
static inline void io_mapping_unmap_atomic(void __iomem *vaddr) { (void)vaddr; }

/* Device & PCI Structures */
struct device {
    struct device *parent;
    u64 dma_mask_val;
    u64 *dma_mask;
    u64 coherent_dma_mask;
    void *driver_data;
};

enum pci_bus_speed {
    PCI_SPEED_UNKNOWN = 0xff,
    PCIE_SPEED_2_5GT  = 0x14,
    PCIE_SPEED_5_0GT  = 0x15,
    PCIE_SPEED_8_0GT  = 0x16,
    PCIE_SPEED_16_0GT = 0x17,
};

struct pci_bus {
    unsigned char number;
    int domain;
    enum pci_bus_speed max_bus_speed;
    struct pci_dev *self;
};

#define IORESOURCE_MEM     0x00000200UL
#define IORESOURCE_PREFETCH 0x00002000UL
#define IORESOURCE_MEM_64  0x00100000UL

#define PCI_CAP_ID_AGP     0x02
#define PCI_CAP_ID_EXP     0x10
#define PCI_COMMAND        0x04
#define PCI_COMMAND_IO     0x01
#define PCI_COMMAND_MEMORY 0x02
#define PCI_COMMAND_MASTER 0x04
#define PCI_ROM_ADDRESS    0x30

#define PCI_EXP_DEVCTL     8
#define PCI_EXP_LNKCAP     12
#define PCI_EXP_LNKCTL     16
#define PCI_EXP_LNKSTA     18
#define PCI_EXP_LNKCTL2    48

struct pci_resource {
    resource_size_t start;
    resource_size_t end;
    unsigned long   flags;
};

struct pci_dev {
    struct device dev;
    struct pci_bus *bus;
    unsigned int devfn;
    unsigned short vendor;
    unsigned short device;
    unsigned short subsystem_vendor;
    unsigned short subsystem_device;
    unsigned int   class;
    u8             revision;
    u8             pcie_cap;
    int            irq;
    bool           is_pcie;
    bool           msi_enabled;
    phys_addr_t    rom;
    size_t         romlen;
    struct pci_resource resource[6];
};

#define PCI_SLOT(devfn) (((devfn) >> 3) & 0x1f)
#define PCI_FUNC(devfn) ((devfn) & 0x07)
#define PCI_DEVID(bus, devfn) ((((u16)(bus)) << 8) | (devfn))

static inline struct pci_dev *to_pci_dev(struct device *dev) { return container_of(dev, struct pci_dev, dev); }
static inline bool dev_is_pci(const struct device *dev) { (void)dev; return true; }
static inline int  pci_domain_nr(struct pci_bus *bus)   { return bus ? bus->domain : 0; }
static inline bool pci_is_pcie(struct pci_dev *dev)     { return dev->is_pcie; }
static inline resource_size_t pci_resource_start(struct pci_dev *dev, int bar) { return dev->resource[bar].start; }
static inline resource_size_t pci_resource_end(struct pci_dev *dev, int bar)   { return dev->resource[bar].end; }
static inline resource_size_t pci_resource_len(struct pci_dev *dev, int bar) {
    if (!dev->resource[bar].start && !dev->resource[bar].end) return 0;
    return dev->resource[bar].end - dev->resource[bar].start + 1;
}
static inline unsigned long pci_resource_flags(struct pci_dev *dev, int bar)   { return dev->resource[bar].flags; }
static inline int pci_enable_device(struct pci_dev *dev) { (void)dev; return 0; }
static inline void pci_disable_device(struct pci_dev *dev) { (void)dev; }
static inline void pci_set_master(struct pci_dev *dev) { (void)dev; }
static inline void pci_clear_master(struct pci_dev *dev) { (void)dev; }
static inline int pci_enable_msi(struct pci_dev *dev) { dev->msi_enabled = true; return 0; }
static inline void pci_disable_msi(struct pci_dev *dev) { dev->msi_enabled = false; }
static inline int pci_find_capability(struct pci_dev *dev, int cap) {
    return (cap == PCI_CAP_ID_EXP) ? dev->pcie_cap : 0;
}
static inline struct pci_dev *pci_upstream_bridge(struct pci_dev *dev) {
    return dev->bus ? dev->bus->self : NULL;
}

int macos_pci_read_config_byte(const struct pci_dev *dev, int where, u8 *val);
int macos_pci_read_config_word(const struct pci_dev *dev, int where, u16 *val);
int macos_pci_read_config_dword(const struct pci_dev *dev, int where, u32 *val);
int macos_pci_write_config_byte(const struct pci_dev *dev, int where, u8 val);
int macos_pci_write_config_word(const struct pci_dev *dev, int where, u16 val);
int macos_pci_write_config_dword(const struct pci_dev *dev, int where, u32 val);

#define pci_read_config_byte   macos_pci_read_config_byte
#define pci_read_config_word   macos_pci_read_config_word
#define pci_read_config_dword  macos_pci_read_config_dword
#define pci_write_config_byte  macos_pci_write_config_byte
#define pci_write_config_word  macos_pci_write_config_word
#define pci_write_config_dword macos_pci_write_config_dword

static inline int pcie_capability_read_word(struct pci_dev *dev, int pos, u16 *val) {
    if (!dev->pcie_cap) return -EINVAL;
    return macos_pci_read_config_word(dev, dev->pcie_cap + pos, val);
}
static inline int pcie_capability_read_dword(struct pci_dev *dev, int pos, u32 *val) {
    if (!dev->pcie_cap) return -EINVAL;
    return macos_pci_read_config_dword(dev, dev->pcie_cap + pos, val);
}
static inline int pcie_capability_write_word(struct pci_dev *dev, int pos, u16 val) {
    if (!dev->pcie_cap) return -EINVAL;
    return macos_pci_write_config_word(dev, dev->pcie_cap + pos, val);
}
static inline int pcie_capability_clear_and_set_word(struct pci_dev *dev, int pos, u16 clear, u16 set) {
    u16 v = 0;
    int r = pcie_capability_read_word(dev, pos, &v);
    if (r) return r;
    v = (v & ~clear) | set;
    return pcie_capability_write_word(dev, pos, v);
}
static inline int pcie_get_speed_cap(struct pci_dev *dev) { (void)dev; return 0; }
static inline int pcie_get_width_cap(struct pci_dev *dev) { (void)dev; return 4; }

static inline void __iomem *pci_map_rom(struct pci_dev *pdev, size_t *size) { (void)pdev; *size = 0; return NULL; }
static inline void pci_unmap_rom(struct pci_dev *pdev, void __iomem *rom) { (void)pdev; (void)rom; }
static inline int  pci_enable_rom(struct pci_dev *pdev) { (void)pdev; return -ENODEV; }
static inline void pci_disable_rom(struct pci_dev *pdev) { (void)pdev; }

/* Platform Device Stubs */
struct platform_device {
    const char *name;
    int id;
    struct device dev;
};

/* Firmware Loader */
struct firmware {
    size_t size;
    const u8 *data;
};
int  macos_request_firmware(const struct firmware **fw, const char *name, struct device *device);
void macos_release_firmware(const struct firmware *fw);
#define request_firmware(fw, name, dev)          macos_request_firmware(fw, name, dev)
#define request_firmware_direct(fw, name, dev)   macos_request_firmware(fw, name, dev)
#define firmware_request_nowarn(fw, name, dev)   macos_request_firmware(fw, name, dev)
#define release_firmware(fw)                     macos_release_firmware(fw)

/* Pages, Scatter-Gather & DMA Mapping */
struct page {
    void       *virt;
    dma_addr_t  phys;
    uint32_t    pool_idx;
    uint32_t    page_idx;
};

enum dma_data_direction {
    DMA_BIDIRECTIONAL = 0,
    DMA_TO_DEVICE     = 1,
    DMA_FROM_DEVICE   = 2,
    DMA_NONE          = 3,
};

struct scatterlist {
    struct page  *page_link_ptr;
    unsigned int  offset;
    unsigned int  length;
    dma_addr_t    dma_address;
    unsigned int  dma_length;
    bool          is_end;
};

struct sg_table {
    struct scatterlist *sgl;
    unsigned int nents;
    unsigned int orig_nents;
};

#define sg_dma_address(sg) ((sg)->dma_address)
#define sg_dma_len(sg)     ((sg)->dma_length)
static inline struct scatterlist *sg_next(struct scatterlist *sg) {
    return sg->is_end ? NULL : (sg + 1);
}
static inline void sg_init_one(struct scatterlist *sg, const void *buf, unsigned int buflen) {
    (void)buf;
    memset(sg, 0, sizeof(*sg));
    sg->length = buflen;
    sg->dma_length = buflen;
    sg->is_end = true;
}
static inline void sg_set_page(struct scatterlist *sg, struct page *page, unsigned int len, unsigned int offset) {
    sg->page_link_ptr = page;
    sg->offset = offset;
    sg->length = len;
    sg->dma_address = page ? (page->phys + offset) : 0;
    sg->dma_length = len;
}
static inline int sg_alloc_table(struct sg_table *table, unsigned int nents, gfp_t gfp) {
    table->sgl = kcalloc(nents ? nents : 1, sizeof(struct scatterlist), gfp);
    if (!table->sgl) return -ENOMEM;
    table->nents = nents;
    table->orig_nents = nents;
    if (nents > 0) table->sgl[nents - 1].is_end = true;
    return 0;
}
static inline void sg_free_table(struct sg_table *table) {
    kfree(table->sgl);
    table->sgl = NULL;
    table->nents = table->orig_nents = 0;
}
#define for_each_sgtable_sg(sgt, sg, i) \
    for ((i) = 0, (sg) = (sgt)->sgl; (i) < (int)(sgt)->orig_nents; (i)++, (sg)++)
#define for_each_sgtable_dma_sg(sgt, sg, i) \
    for ((i) = 0, (sg) = (sgt)->sgl; (i) < (int)(sgt)->nents; (i)++, (sg)++)

struct page *macos_alloc_page(gfp_t gfp);
int          macos_alloc_pages_bulk(gfp_t gfp, unsigned int npages, struct page **out_pages);
void         macos_free_page(struct page *p);
void        *macos_dma_alloc(size_t size, dma_addr_t *dma_handle, bool coherent);
void         macos_dma_free(size_t size, void *cpu_addr, dma_addr_t dma_handle);
void        *macos_vmap(struct page **pages, unsigned int count);
void         macos_vunmap(const void *addr);
struct page *macos_vmalloc_to_page(const void *vmalloc_addr);

#define alloc_page(gfp)      macos_alloc_page(gfp)
#define __free_page(page)    macos_free_page(page)
#define page_address(page)   ((page)->virt)
#define pfn_to_page(pfn)     ((struct page *)(uintptr_t)((pfn) << PAGE_SHIFT))
#define vmalloc_to_page(va)  macos_vmalloc_to_page(va)

#define VM_MAP 0x00000004
#define PAGE_KERNEL 0
#define vmap(pages, count, flags, prot) macos_vmap((pages), (count))
#define vunmap(addr)                    macos_vunmap(addr)

static inline int dma_set_mask_and_coherent(struct device *dev, u64 mask) {
    dev->dma_mask_val = mask;
    dev->coherent_dma_mask = mask;
    return 0;
}
static inline dma_addr_t dma_map_page(struct device *dev, struct page *page,
                                      size_t offset, size_t size, enum dma_data_direction dir) {
    (void)dev; (void)size; (void)dir;
    return page ? (page->phys + offset) : ~0ULL;
}
static inline void dma_unmap_page(struct device *dev, dma_addr_t addr,
                                  size_t size, enum dma_data_direction dir) {
    (void)dev; (void)addr; (void)size; (void)dir;
}
static inline int dma_mapping_error(struct device *dev, dma_addr_t dma_addr) {
    (void)dev;
    return (dma_addr == 0 || dma_addr == ~0ULL) ? -ENOMEM : 0;
}
static inline void *dma_alloc_coherent(struct device *dev, size_t size,
                                       dma_addr_t *dma_handle, gfp_t gfp) {
    (void)dev; (void)gfp;
    return macos_dma_alloc(size, dma_handle, true);
}
static inline void dma_free_coherent(struct device *dev, size_t size,
                                     void *cpu_addr, dma_addr_t dma_handle) {
    (void)dev;
    macos_dma_free(size, cpu_addr, dma_handle);
}
static inline void *dma_alloc_noncoherent(struct device *dev, size_t size,
                                          dma_addr_t *dma_handle, enum dma_data_direction dir, gfp_t gfp) {
    (void)dev; (void)dir; (void)gfp;
    return macos_dma_alloc(size, dma_handle, false);
}
static inline void dma_free_noncoherent(struct device *dev, size_t size,
                                        void *cpu_addr, dma_addr_t dma_handle, enum dma_data_direction dir) {
    (void)dev; (void)dir;
    macos_dma_free(size, cpu_addr, dma_handle);
}
static inline int dma_map_sgtable(struct device *dev, struct sg_table *sgt,
                                  enum dma_data_direction dir, unsigned long attrs) {
    (void)dev; (void)dir; (void)attrs;
    sgt->nents = sgt->orig_nents;
    return 0;
}
static inline void dma_unmap_sgtable(struct device *dev, struct sg_table *sgt,
                                     enum dma_data_direction dir, unsigned long attrs) {
    (void)dev; (void)sgt; (void)dir; (void)attrs;
}
static inline void dma_sync_single_for_device(struct device *dev, dma_addr_t addr, size_t size, enum dma_data_direction dir) {
    (void)dev; (void)addr; (void)size; (void)dir; mb();
}
static inline void dma_sync_single_for_cpu(struct device *dev, dma_addr_t addr, size_t size, enum dma_data_direction dir) {
    (void)dev; (void)addr; (void)size; (void)dir; mb();
}
static inline bool iommu_present(const void *bus) { (void)bus; return false; }

/* Interrupt Handling */
typedef enum irqreturn {
    IRQ_NONE        = 0,
    IRQ_HANDLED     = 1,
    IRQ_WAKE_THREAD = 2,
} irqreturn_t;
typedef irqreturn_t (*irq_handler_t)(int, void *);

#define IRQF_SHARED 0x00000080

int  macos_request_irq(unsigned int irq, irq_handler_t handler, unsigned long flags, const char *name, void *dev);
void macos_free_irq(unsigned int irq, void *dev_id);
void macos_synchronize_irq(unsigned int irq);
#define request_irq(irq, fn, flags, name, dev) macos_request_irq((irq), (fn), (flags), (name), (dev))
#define free_irq(irq, dev)                     macos_free_irq((irq), (dev))
#define synchronize_irq(irq)                   macos_synchronize_irq(irq)

/* ACPI & I2C Stubs (headless GP108) */
typedef void *acpi_handle;

#define I2C_NAME_SIZE 20
#define I2C_M_RD      0x0001
struct i2c_msg {
    u16 addr;
    u16 flags;
    u16 len;
    u8 *buf;
};
struct i2c_board_info {
    char type[I2C_NAME_SIZE];
    unsigned short addr;
    void *platform_data;
    int irq;
};
struct i2c_algorithm {
    int (*master_xfer)(void *adap, struct i2c_msg *msgs, int num);
    u32 (*functionality)(void *adap);
};
struct i2c_algo_bit_data {
    void *data;
    void (*setsda)(void *data, int state);
    void (*setscl)(void *data, int state);
    int  (*getsda)(void *data);
    int  (*getscl)(void *data);
    int  udelay;
    int  timeout;
};
struct i2c_adapter {
    void *owner;
    const struct i2c_algorithm *algo;
    void *algo_data;
    struct device dev;
    char name[48];
};
static inline int i2c_transfer(struct i2c_adapter *adap, struct i2c_msg *msgs, int num) {
    (void)adap; (void)msgs; (void)num;
    return -ENODEV;
}

/* String Helpers */
static inline const char *str_yes_no(bool v) { return v ? "yes" : "no"; }
static inline const char *str_enabled_disabled(bool v) { return v ? "enabled" : "disabled"; }
static inline const char *str_on_off(bool v) { return v ? "on" : "off"; }
static inline const char *str_true_false(bool v) { return v ? "true" : "false"; }

#endif /* __NOUVEAU_OS_H__ */
