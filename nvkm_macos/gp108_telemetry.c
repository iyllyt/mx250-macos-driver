/* SPDX-License-Identifier: MIT
 * Zero-overhead userspace telemetry publisher for libvulkan_nouveau.dylib
 */
#include "../include/GP108TelemetryShared.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <time.h>

static struct GP108TelemetryPage *g_tpage = NULL;
static struct GP108ProcTelemetry *g_tslot = NULL;
static pthread_mutex_t            g_tlock = PTHREAD_MUTEX_INITIALIZER;

static uint64_t mono_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

void gp108_telem_on_retain(void) {
    pthread_mutex_lock(&g_tlock);
    if (!g_tpage) {
        g_tpage = gp108_telemetry_map(1);
    }
    if (g_tpage && !g_tslot) {
        gp108_telemetry_reap_dead(g_tpage);
        int32_t my_pid = (int32_t)getpid();
        for (uint32_t i = 0; i < GP108_MAX_TELEM_PROCS; i++) {
            if (g_tpage->procs[i].pid == my_pid) {
                g_tslot = &g_tpage->procs[i];
                break;
            }
        }
        if (!g_tslot) {
            for (uint32_t i = 0; i < GP108_MAX_TELEM_PROCS; i++) {
                if (__sync_bool_compare_and_swap(&g_tpage->procs[i].pid, 0, my_pid)) {
                    g_tslot = &g_tpage->procs[i];
                    break;
                }
            }
        }
        if (g_tslot) {
            char namebuf[PROC_PIDPATHINFO_MAXSIZE];
            memset(namebuf, 0, sizeof(namebuf));
            const char *env_tag = getenv("GP108_APP_NAME");
            if (env_tag && env_tag[0]) {
                strlcpy(g_tslot->comm, env_tag, sizeof(g_tslot->comm));
            } else if (proc_name(my_pid, namebuf, sizeof(namebuf)) > 0 && namebuf[0]) {
                strlcpy(g_tslot->comm, namebuf, sizeof(g_tslot->comm));
            } else {
                const char *pn = getprogname();
                strlcpy(g_tslot->comm, pn ? pn : "vulkan-app", sizeof(g_tslot->comm));
            }
            /* Base 16 MiB WPR + Golden Context + MMU page tables */
            g_tslot->vram_bytes     = (16ULL << 20);
            g_tslot->gart_bytes     = 0;
            g_tslot->submit_count   = 0;
            g_tslot->gpu_busy_ns    = 0;
            g_tslot->engine_flags   = 1U;
            g_tslot->last_active_ns = mono_ns();
        }
    }
    pthread_mutex_unlock(&g_tlock);
}

void gp108_telem_on_release(void) {
    pthread_mutex_lock(&g_tlock);
    if (g_tslot) {
        memset(g_tslot, 0, sizeof(*g_tslot));
        g_tslot = NULL;
    }
    pthread_mutex_unlock(&g_tlock);
}

void gp108_telem_on_bo_alloc(uint64_t size_B, bool is_vram) {
    if (!g_tslot) return;
    if (is_vram)
        __sync_add_and_fetch(&g_tslot->vram_bytes, size_B);
    else
        __sync_add_and_fetch(&g_tslot->gart_bytes, size_B);
}

void gp108_telem_on_bo_free(uint64_t size_B, bool is_vram) {
    if (!g_tslot) return;
    if (is_vram) {
        uint64_t cur = g_tslot->vram_bytes;
        if (cur >= size_B)
            __sync_sub_and_fetch(&g_tslot->vram_bytes, size_B);
    } else {
        uint64_t cur = g_tslot->gart_bytes;
        if (cur >= size_B)
            __sync_sub_and_fetch(&g_tslot->gart_bytes, size_B);
    }
}

void gp108_telem_on_kick(uint32_t engine_flags) {
    if (g_tpage)
        __sync_add_and_fetch(&g_tpage->total_submits, 1ULL);
    if (g_tslot) {
        g_tslot->engine_flags |= engine_flags;
        __sync_add_and_fetch(&g_tslot->submit_count, 1ULL);
        g_tslot->last_active_ns = mono_ns();
    }
}

void gp108_telem_on_gpu_busy(uint64_t busy_ns) {
    if (busy_ns == 0) return;
    if (g_tpage)
        __sync_add_and_fetch(&g_tpage->total_gpu_busy_ns, busy_ns);
    if (g_tslot)
        __sync_add_and_fetch(&g_tslot->gpu_busy_ns, busy_ns);
}

__attribute__((destructor))
static void gp108_telem_dtor(void) {
    gp108_telem_on_release();
}
