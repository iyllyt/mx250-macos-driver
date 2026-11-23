/*
 * include/GP108TelemetryShared.h
 *
 * Lock-free cross-process GPU telemetry page (/tmp/gp108_telemetry_v1.shm).
 * Tracks real-time GDDR5 VRAM usage, GART DMA usage, active client PIDs,
 * process names, and cumulative GPU hardware execution time with ~0% CPU overhead.
 */

#ifndef GP108_TELEMETRY_SHARED_H
#define GP108_TELEMETRY_SHARED_H

#include <errno.h>
#include <fcntl.h>
#include <libproc.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define GP108_TELEMETRY_MAGIC    0x47313038544C4D31ULL /* 'G108TLM1' */
#define GP108_TELEMETRY_SHM_PATH "/tmp/gp108_telemetry_v1.shm"
#define GP108_MAX_TELEM_PROCS    16U

struct __attribute__((aligned(8))) GP108ProcTelemetry {
    int32_t  pid;             /* 0 = empty slot */
    uint32_t engine_flags;    /* Bit 0: Graphics/Copy, Bit 1: Compute */
    uint64_t vram_bytes;      /* Live GDDR5 VRAM allocated by this process */
    uint64_t gart_bytes;      /* Live coherent GART DMA allocated by this process */
    uint64_t submit_count;    /* Command submissions kicked */
    uint64_t gpu_busy_ns;     /* Cumulative ns GPU spent executing this process's work */
    uint64_t last_active_ns;  /* CLOCK_MONOTONIC ns of last kick */
    char     comm[64];        /* Process executable / display name */
};

struct __attribute__((aligned(64))) GP108TelemetryPage {
    uint64_t                  magic;
    uint64_t                  total_submits;
    uint64_t                  total_gpu_busy_ns;
    uint64_t                  reserved[5];
    struct GP108ProcTelemetry procs[GP108_MAX_TELEM_PROCS];
};

static inline struct GP108TelemetryPage *gp108_telemetry_map(int writable) {
    int oflags = writable ? (O_CREAT | O_RDWR) : O_RDWR;
    mode_t old_umask = umask(0);
    int fd = open(GP108_TELEMETRY_SHM_PATH, oflags, 0666);
    umask(old_umask);
    if (fd < 0) {
        if (!writable) {
            return NULL;
        }
        return NULL;
    }
    struct stat st;
    if (fstat(fd, &st) == 0 && (size_t)st.st_size < sizeof(struct GP108TelemetryPage)) {
        if (writable) {
            (void)ftruncate(fd, 4096);
            (void)fchmod(fd, 0666);
        } else {
            close(fd);
            return NULL;
        }
    }
    int prot = PROT_READ | PROT_WRITE;
    void *p = mmap(NULL, 4096, prot, MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED) {
        return NULL;
    }
    struct GP108TelemetryPage *page = (struct GP108TelemetryPage *)p;
    if (writable && page->magic != GP108_TELEMETRY_MAGIC) {
        memset(page, 0, 4096);
        __sync_synchronize();
        page->magic = GP108_TELEMETRY_MAGIC;
    }
    return page;
}

static inline void gp108_telemetry_reap_dead(struct GP108TelemetryPage *page) {
    if (!page || page->magic != GP108_TELEMETRY_MAGIC) {
        return;
    }
    for (uint32_t i = 0; i < GP108_MAX_TELEM_PROCS; i++) {
        int32_t pid = page->procs[i].pid;
        if (pid > 0 && kill(pid, 0) != 0 && errno == ESRCH) {
            memset(&page->procs[i], 0, sizeof(page->procs[i]));
        }
    }
}

#endif /* GP108_TELEMETRY_SHARED_H */
