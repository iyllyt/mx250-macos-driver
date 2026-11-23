/* SPDX-License-Identifier: MIT
 * Userspace ELF64 Relocatable Loader for NVIDIA FreeBSD x86_64 nv-kernel.o
 * Maps all SHF_ALLOC sections + PLT/data stubs into [0x20000000, 0x24000000)
 * so R_X86_64_32S, R_X86_64_PC32, and R_X86_64_64 all resolve without truncation.
 */
#include "gp108_rm_host.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/ucontext.h>
#include <unistd.h>

/* Standard ELF64 definitions (avoiding missing <elf.h> on macOS) */
typedef struct {
    uint8_t  e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} Elf64_Ehdr;

typedef struct {
    uint32_t sh_name;
    uint32_t sh_type;
    uint64_t sh_flags;
    uint64_t sh_addr;
    uint64_t sh_offset;
    uint64_t sh_size;
    uint32_t sh_link;
    uint32_t sh_info;
    uint64_t sh_addralign;
    uint64_t sh_entsize;
} Elf64_Shdr;

typedef struct {
    uint32_t st_name;
    uint8_t  st_info;
    uint8_t  st_other;
    uint16_t st_shndx;
    uint64_t st_value;
    uint64_t st_size;
} Elf64_Sym;

typedef struct {
    uint64_t r_offset;
    uint64_t r_info;
    int64_t  r_addend;
} Elf64_Rela;

#define SHT_SYMTAB       2U
#define SHT_RELA         4U
#define SHT_NOBITS       8U
#define SHF_WRITE        0x1ULL
#define SHF_ALLOC        0x2ULL
#define SHF_EXECINSTR    0x4ULL
#define SHN_UNDEF        0U
#define SHN_ABS          0xfff1U
#define SHN_COMMON       0xfff2U

#define R_X86_64_NONE    0U
#define R_X86_64_64      1U
#define R_X86_64_PC32    2U
#define R_X86_64_PLT32   4U
#define R_X86_64_32      10U
#define R_X86_64_32S     11U

#define RM_ARENA_BASE    0x20000000ULL
#define RM_ARENA_SIZE    (64ULL * 1024ULL * 1024ULL) /* 64 MiB in <2 GiB VA */

static uint8_t *g_arena_base = NULL;
static uint8_t *g_text_base  = NULL;
static size_t   g_text_size  = 0;

static uint64_t align_up_u64(uint64_t v, uint64_t a)
{
    if (a <= 1) return v;
    return (v + a - 1ULL) & ~(a - 1ULL);
}

static uint64_t unimpl_stub_handler(const char *sym_name)
{
    fprintf(stderr, "[gp108-rm] WARNING: unimplemented symbol called: %s\n",
            sym_name ? sym_name : "<unknown>");
    return NV_ERR_NOT_SUPPORTED;
}

/* Ring-3 trap-and-emulate handler for privileged `mov %cr4, %reg` (0f 20 e0) */
static void rm_trap_sigaction(int sig, siginfo_t *info, void *uap)
{
    ucontext_t *uc = (ucontext_t *)uap;
    if (!uc || !uc->uc_mcontext) {
        _exit(128 + sig);
    }

    uint64_t rip = uc->uc_mcontext->__ss.__rip;
    if (g_arena_base && rip >= (uint64_t)g_arena_base &&
        rip + 3 <= (uint64_t)g_arena_base + RM_ARENA_SIZE) {
        const uint8_t *insn = (const uint8_t *)(uintptr_t)rip;
        /* 0f 20 /r = MOV r64, CRn; ModRM 11_100_reg (0xe0..0xe7) = CR4 */
        if (insn[0] == 0x0f && insn[1] == 0x20 && (insn[2] & 0xf8) == 0xe0) {
            uint64_t cr4_val = 0x000006f0ULL; /* PAE | PGE | OSFXSR | OSXMMEXCPT */
            switch (insn[2] & 0x07) {
                case 0: uc->uc_mcontext->__ss.__rax = cr4_val; break;
                case 1: uc->uc_mcontext->__ss.__rcx = cr4_val; break;
                case 2: uc->uc_mcontext->__ss.__rdx = cr4_val; break;
                case 3: uc->uc_mcontext->__ss.__rbx = cr4_val; break;
                case 6: uc->uc_mcontext->__ss.__rsi = cr4_val; break;
                case 7: uc->uc_mcontext->__ss.__rdi = cr4_val; break;
                default: break;
            }
            uc->uc_mcontext->__ss.__rip = rip + 3;
            return;
        }
    }

    int64_t text_off = g_text_base ? ((int64_t)rip - (int64_t)(uintptr_t)g_text_base) : -1;
    fprintf(stderr,
            "\n[gp108-rm] FATAL SIGNAL %d (code=%d addr=%p) at RIP=0x%llx (.text+0x%llx)\n"
            "  RAX=0x%llx RBX=0x%llx RCX=0x%llx RDX=0x%llx\n"
            "  RSI=0x%llx RDI=0x%llx RBP=0x%llx RSP=0x%llx\n",
            sig, info ? info->si_code : 0, info ? info->si_addr : NULL,
            (unsigned long long)rip, (unsigned long long)text_off,
            (unsigned long long)uc->uc_mcontext->__ss.__rax,
            (unsigned long long)uc->uc_mcontext->__ss.__rbx,
            (unsigned long long)uc->uc_mcontext->__ss.__rcx,
            (unsigned long long)uc->uc_mcontext->__ss.__rdx,
            (unsigned long long)uc->uc_mcontext->__ss.__rsi,
            (unsigned long long)uc->uc_mcontext->__ss.__rdi,
            (unsigned long long)uc->uc_mcontext->__ss.__rbp,
            (unsigned long long)uc->uc_mcontext->__ss.__rsp);
    fflush(stderr);
    _exit(128 + sig);
}

static void install_rm_trap_handlers(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = rm_trap_sigaction;
    sa.sa_flags     = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGILL,  &sa, NULL);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS,  &sa, NULL);
}

static bool emit_retpoline_if_match(const char *name, uint8_t **inout_cursor, uint64_t *out_addr)
{
    static const struct {
        const char *sym;
        uint8_t     bytes[4];
        uint8_t     len;
    } kThunks[] = {
        { "__x86_indirect_thunk_rax", { 0xff, 0xe0, 0x90, 0x90 }, 2 },
        { "__x86_indirect_thunk_rcx", { 0xff, 0xe1, 0x90, 0x90 }, 2 },
        { "__x86_indirect_thunk_rdx", { 0xff, 0xe2, 0x90, 0x90 }, 2 },
        { "__x86_indirect_thunk_rbx", { 0xff, 0xe3, 0x90, 0x90 }, 2 },
        { "__x86_indirect_thunk_rsi", { 0xff, 0xe6, 0x90, 0x90 }, 2 },
        { "__x86_indirect_thunk_rdi", { 0xff, 0xe7, 0x90, 0x90 }, 2 },
        { "__x86_indirect_thunk_r8",  { 0x41, 0xff, 0xe0, 0x90 }, 3 },
        { "__x86_indirect_thunk_r9",  { 0x41, 0xff, 0xe1, 0x90 }, 3 },
        { "__x86_indirect_thunk_r10", { 0x41, 0xff, 0xe2, 0x90 }, 3 },
        { "__x86_indirect_thunk_r11", { 0x41, 0xff, 0xe3, 0x90 }, 3 },
        { "__x86_indirect_thunk_r12", { 0x41, 0xff, 0xe4, 0x90 }, 3 },
        { "__x86_indirect_thunk_r13", { 0x41, 0xff, 0xe5, 0x90 }, 3 },
        { "__x86_indirect_thunk_r14", { 0x41, 0xff, 0xe6, 0x90 }, 3 },
        { "__x86_indirect_thunk_r15", { 0x41, 0xff, 0xe7, 0x90 }, 3 },
    };

    for (size_t i = 0; i < sizeof(kThunks) / sizeof(kThunks[0]); i++) {
        if (strcmp(name, kThunks[i].sym) == 0) {
            uint8_t *p = (uint8_t *)(uintptr_t)align_up_u64((uint64_t)(uintptr_t)*inout_cursor, 16);
            memset(p, 0x90, 16);
            memcpy(p, kThunks[i].bytes, kThunks[i].len);
            *out_addr = (uint64_t)(uintptr_t)p;
            *inout_cursor = p + 16;
            return true;
        }
    }
    return false;
}

int gp108_elf64_load_rm(const char *elf_path, struct gp108_rm_exports *out_exports)
{
    if (!elf_path || !out_exports)
        return -EINVAL;

    int fd = open(elf_path, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "[gp108-rm] Failed to open %s: %s\n", elf_path, strerror(errno));
        return -errno;
    }

    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size < (off_t)sizeof(Elf64_Ehdr)) {
        close(fd);
        return -EINVAL;
    }

    install_rm_trap_handlers();

    /* Reserve 64 MiB in low <2 GiB VA space BEFORE mapping the ELF file so they never overlap,
     * and never pass MAP_FIXED so we never clobber existing mappings! */
    if (!g_arena_base) {
        for (uint64_t hint = 0x10000000ULL; hint <= 0x60000000ULL; hint += 0x08000000ULL) {
            void *cand = mmap((void *)(uintptr_t)hint, RM_ARENA_SIZE,
                              PROT_READ | PROT_WRITE | PROT_EXEC,
                              MAP_PRIVATE | MAP_ANON, -1, 0);
            if (cand != MAP_FAILED) {
                uint64_t cva = (uint64_t)(uintptr_t)cand;
                if (cva >= 0x01000000ULL && (cva + RM_ARENA_SIZE) <= 0x7fff0000ULL) {
                    g_arena_base = (uint8_t *)cand;
                    break;
                }
                munmap(cand, RM_ARENA_SIZE);
            }
        }
    }
    if (!g_arena_base) {
        fprintf(stderr, "[gp108-rm] Failed to reserve 64 MiB in <2 GiB VA space (ensure -Wl,-pagezero_size,0x1000)\n");
        close(fd);
        return -ENOMEM;
    }

    size_t file_sz = (size_t)st.st_size;
    uint8_t *elf = mmap(NULL, file_sz, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (elf == MAP_FAILED)
        return -ENOMEM;

    const Elf64_Ehdr *eh = (const Elf64_Ehdr *)elf;
    if (memcmp(eh->e_ident, "\x7f""ELF\x02\x01\x01", 7) != 0 || eh->e_machine != 62 /* EM_X86_64 */) {
        fprintf(stderr, "[gp108-rm] Invalid ELF64 x86_64 object: %s\n", elf_path);
        munmap(elf, file_sz);
        return -EINVAL;
    }

    const Elf64_Shdr *shdrs = (const Elf64_Shdr *)(elf + eh->e_shoff);
    const char *shstr = (const char *)(elf + shdrs[eh->e_shstrndx].sh_offset);

    uint64_t *sec_addrs = calloc(eh->e_shnum, sizeof(uint64_t));
    uint8_t *cursor = g_arena_base;

    /* 1. Lay out and copy all SHF_ALLOC sections */
    for (uint16_t i = 0; i < eh->e_shnum; i++) {
        const Elf64_Shdr *sh = &shdrs[i];
        if (!(sh->sh_flags & SHF_ALLOC) || sh->sh_size == 0)
            continue;

        uint64_t align = sh->sh_addralign > 4096ULL ? sh->sh_addralign : 4096ULL;
        cursor = (uint8_t *)(uintptr_t)align_up_u64((uint64_t)(uintptr_t)cursor, align);
        if (cursor + sh->sh_size > g_arena_base + RM_ARENA_SIZE) {
            fprintf(stderr, "[gp108-rm] Arena overflow laying out section %u\n", i);
            free(sec_addrs);
            munmap(elf, file_sz);
            return -ENOMEM;
        }

        sec_addrs[i] = (uint64_t)(uintptr_t)cursor;
        if (sh->sh_type != SHT_NOBITS) {
            memcpy(cursor, elf + sh->sh_offset, sh->sh_size);
        } else {
            memset(cursor, 0, sh->sh_size);
        }

        const char *sname = shstr + sh->sh_name;
        if (strcmp(sname, ".text") == 0) {
            g_text_base = cursor;
            g_text_size = (size_t)sh->sh_size;
        }
        cursor += sh->sh_size;
    }

    /* 2. Locate SHT_SYMTAB and resolve all symbols (including SHN_UNDEF & SHN_COMMON) */
    int symtab_idx = -1;
    for (uint16_t i = 0; i < eh->e_shnum; i++) {
        if (shdrs[i].sh_type == SHT_SYMTAB) {
            symtab_idx = (int)i;
            break;
        }
    }
    if (symtab_idx < 0) {
        free(sec_addrs);
        munmap(elf, file_sz);
        return -EINVAL;
    }

    const Elf64_Shdr *sym_sh = &shdrs[symtab_idx];
    const Elf64_Sym  *syms   = (const Elf64_Sym *)(elf + sym_sh->sh_offset);
    size_t            nsyms  = (size_t)(sym_sh->sh_size / sizeof(Elf64_Sym));
    const char       *strtab = (const char *)(elf + shdrs[sym_sh->sh_link].sh_offset);

    uint64_t *sym_addrs = calloc(nsyms, sizeof(uint64_t));
    cursor = (uint8_t *)(uintptr_t)align_up_u64((uint64_t)(uintptr_t)cursor, 4096ULL);
    uint8_t *stub_start = cursor;
    size_t fallback_stubs = 0;

    for (size_t s = 1; s < nsyms; s++) {
        const Elf64_Sym *sym = &syms[s];
        const char *name = strtab + sym->st_name;

        if (sym->st_shndx == SHN_ABS) {
            sym_addrs[s] = sym->st_value;
        } else if (sym->st_shndx == SHN_COMMON) {
            uint64_t align = sym->st_value > 16ULL ? sym->st_value : 16ULL;
            cursor = (uint8_t *)(uintptr_t)align_up_u64((uint64_t)(uintptr_t)cursor, align);
            memset(cursor, 0, sym->st_size);
            sym_addrs[s] = (uint64_t)(uintptr_t)cursor;
            cursor += sym->st_size;
        } else if (sym->st_shndx != SHN_UNDEF) {
            sym_addrs[s] = sec_addrs[sym->st_shndx] + sym->st_value;
        } else {
            /* SHN_UNDEF: check retpoline first, then OS symbol table */
            if (emit_retpoline_if_match(name, &cursor, &sym_addrs[s]))
                continue;

            const struct gp108_ext_sym *ext = gp108_os_lookup_symbol(name);
            if (ext && ext->is_data) {
                cursor = (uint8_t *)(uintptr_t)align_up_u64((uint64_t)(uintptr_t)cursor, 32ULL);
                memcpy(cursor, ext->addr, ext->data_size);
                sym_addrs[s] = (uint64_t)(uintptr_t)cursor;
                cursor += align_up_u64(ext->data_size, 16ULL);
            } else if (ext && ext->addr) {
                /* 16-byte PLT jump stub: ff 25 00 00 00 00 <64-bit target> 90 90 */
                cursor = (uint8_t *)(uintptr_t)align_up_u64((uint64_t)(uintptr_t)cursor, 16ULL);
                cursor[0] = 0xff;
                cursor[1] = 0x25;
                cursor[2] = 0x00;
                cursor[3] = 0x00;
                cursor[4] = 0x00;
                cursor[5] = 0x00;
                uint64_t fn_va = (uint64_t)(uintptr_t)ext->addr;
                memcpy(cursor + 6, &fn_va, 8);
                cursor[14] = 0x90;
                cursor[15] = 0x90;
                sym_addrs[s] = (uint64_t)(uintptr_t)cursor;
                cursor += 16;
            } else {
                /* Named diagnostic fallback stub:
                 *   48 bf <imm64 name_copy>   ; movabs $name_copy, %rdi
                 *   ff 25 00 00 00 00         ; jmpq *0(%rip)
                 *   <uint64_t unimpl_stub_handler>
                 */
                char *name_dup = strdup(name);
                cursor = (uint8_t *)(uintptr_t)align_up_u64((uint64_t)(uintptr_t)cursor, 16ULL);
                cursor[0] = 0x48;
                cursor[1] = 0xbf;
                uint64_t np = (uint64_t)(uintptr_t)name_dup;
                memcpy(cursor + 2, &np, 8);
                cursor[10] = 0xff;
                cursor[11] = 0x25;
                cursor[12] = 0x00;
                cursor[13] = 0x00;
                cursor[14] = 0x00;
                cursor[15] = 0x00;
                uint64_t hva = (uint64_t)(uintptr_t)&unimpl_stub_handler;
                memcpy(cursor + 16, &hva, 8);
                sym_addrs[s] = (uint64_t)(uintptr_t)cursor;
                cursor += 32;
                fallback_stubs++;
            }
        }

        if (cursor > g_arena_base + RM_ARENA_SIZE) {
            fprintf(stderr, "[gp108-rm] Arena overflow generating stubs!\n");
            free(sym_addrs);
            free(sec_addrs);
            munmap(elf, file_sz);
            return -ENOMEM;
        }
    }

    /* 3. Apply all SHT_RELA relocations targeting SHF_ALLOC sections */
    size_t total_relocs = 0;
    for (uint16_t i = 0; i < eh->e_shnum; i++) {
        const Elf64_Shdr *rsh = &shdrs[i];
        if (rsh->sh_type != SHT_RELA)
            continue;

        uint32_t target_sec = rsh->sh_info;
        if (target_sec >= eh->e_shnum || sec_addrs[target_sec] == 0)
            continue; /* Skip .rela.eh_frame or debug sections if non-SHF_ALLOC */

        uint64_t target_base = sec_addrs[target_sec];
        const Elf64_Rela *relas = (const Elf64_Rela *)(elf + rsh->sh_offset);
        size_t nrel = (size_t)(rsh->sh_size / sizeof(Elf64_Rela));

        for (size_t r = 0; r < nrel; r++) {
            uint64_t r_off = relas[r].r_offset;
            uint32_t s_idx = (uint32_t)(relas[r].r_info >> 32);
            uint32_t rtype = (uint32_t)(relas[r].r_info & 0xffffffffU);
            int64_t  add   = relas[r].r_addend;

            uint64_t P = target_base + r_off;
            uint64_t S = sym_addrs[s_idx];

            switch (rtype) {
                case R_X86_64_NONE:
                    break;
                case R_X86_64_64: {
                    uint64_t val = (uint64_t)((int64_t)S + add);
                    memcpy((void *)(uintptr_t)P, &val, 8);
                    break;
                }
                case R_X86_64_PC32:
                case R_X86_64_PLT32: {
                    int64_t disp = (int64_t)S + add - (int64_t)P;
                    if (disp < INT32_MIN || disp > INT32_MAX) {
                        fprintf(stderr, "[gp108-rm] PC32 overflow at 0x%llx -> 0x%llx\n",
                                (unsigned long long)P, (unsigned long long)S);
                        return -ERANGE;
                    }
                    int32_t v32 = (int32_t)disp;
                    memcpy((void *)(uintptr_t)P, &v32, 4);
                    break;
                }
                case R_X86_64_32S: {
                    int64_t val = (int64_t)S + add;
                    if (val < INT32_MIN || val > INT32_MAX) {
                        fprintf(stderr, "[gp108-rm] 32S overflow at 0x%llx: val=0x%llx\n",
                                (unsigned long long)P, (unsigned long long)val);
                        return -ERANGE;
                    }
                    int32_t v32 = (int32_t)val;
                    memcpy((void *)(uintptr_t)P, &v32, 4);
                    break;
                }
                case R_X86_64_32: {
                    uint64_t val = (uint64_t)((int64_t)S + add);
                    if (val > UINT32_MAX) {
                        fprintf(stderr, "[gp108-rm] 32 overflow at 0x%llx: val=0x%llx\n",
                                (unsigned long long)P, (unsigned long long)val);
                        return -ERANGE;
                    }
                    uint32_t v32 = (uint32_t)val;
                    memcpy((void *)(uintptr_t)P, &v32, 4);
                    break;
                }
                default:
                    fprintf(stderr, "[gp108-rm] Unsupported relocation type %u\n", rtype);
                    return -ENOEXEC;
            }
            total_relocs++;
        }
    }

    /* 4. Populate exported RM entry points */
    memset(out_exports, 0, sizeof(*out_exports));
    for (size_t s = 1; s < nsyms; s++) {
        const Elf64_Sym *sym = &syms[s];
        if (sym->st_shndx == SHN_UNDEF)
            continue;
        const char *name = strtab + sym->st_name;
        void *fn = (void *)(uintptr_t)sym_addrs[s];

        #define BIND_RM(field) \
            if (strcmp(name, #field) == 0) { out_exports->field = fn; continue; }

        BIND_RM(rm_init_rm);
        BIND_RM(rm_shutdown_rm);
        BIND_RM(rm_init_private_state);
        BIND_RM(rm_free_private_state);
        BIND_RM(rm_init_adapter);
        BIND_RM(rm_disable_adapter);
        BIND_RM(rm_shutdown_adapter);
        BIND_RM(rm_is_supported_device);
        BIND_RM(rm_is_supported_pci_device);
        BIND_RM(rm_get_device_name);
        BIND_RM(rm_get_vbios_version);
        BIND_RM(rm_get_gpu_uuid);
        BIND_RM(rm_ioctl);
        BIND_RM(rm_isr);
        BIND_RM(rm_isr_bh);
        BIND_RM(rm_gpu_copy_mmu_faults);
        BIND_RM(rm_run_rc_callback);
        BIND_RM(rm_execute_work_item);
        BIND_RM(rm_init_event_locks);
        BIND_RM(rm_destroy_event_locks);
        BIND_RM(rm_cleanup_file_private);
        BIND_RM(rm_read_registry_dword);
        BIND_RM(rm_write_registry_dword);
        BIND_RM(rm_write_registry_string);
        BIND_RM(rm_write_registry_binary);
        BIND_RM(rm_perform_version_check);

        #undef BIND_RM
    }

    /* Set .text to RX (leave data RW and stubs RX/RW) */
    if (g_text_base && g_text_size > 0) {
        mprotect(g_text_base, align_up_u64(g_text_size, 4096ULL), PROT_READ | PROT_EXEC);
    }
    (void)stub_start;

    fprintf(stderr,
            "[gp108-rm] Loaded ELF64 RM core (%s): %.2f MiB mapped at 0x%llx, %zu relocations, %zu generic stubs\n",
            elf_path,
            (double)(cursor - g_arena_base) / (1024.0 * 1024.0),
            (unsigned long long)(uintptr_t)g_arena_base,
            total_relocs,
            fallback_stubs);

    free(sym_addrs);
    free(sec_addrs);
    munmap(elf, file_sz);

    if (!out_exports->rm_init_rm || !out_exports->rm_init_adapter || !out_exports->rm_ioctl) {
        fprintf(stderr, "[gp108-rm] Missing required RM entry points!\n");
        return -ENOENT;
    }
    return 0;
}
