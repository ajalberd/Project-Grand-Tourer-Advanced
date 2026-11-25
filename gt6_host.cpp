/*
 * GT6 native port -- host services Gran Turismo 6 needs on top of the ps3recomp
 * tables (installed from main.cpp after lv2_init_syscalls), and the host halves
 * of the hooks compiled into the lifted code (gt6_recomp_hooks.h).
 */

#include "ppu_recomp.h"     /* ppu_context, the lifted prototypes, gt6_recomp_hooks.h */

#include <cstdint>
#include <cstdio>
#include <cstdlib>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

/* The lifted code's cross-fragment trampoline (declared in every lifted chunk). */
extern "C" PPU_THREAD_LOCAL void (*g_trampoline_fn)(void*);
/* ppu_loader.cpp: per-thread bookkeeping of indirect calls, exposed for code that leaves frames without returning. */
extern "C" int* ps3_ppu_dispatch_depth_slot(void);
extern "C" int* ps3_ppu_vcall_sp_slot(void);
extern "C" uint8_t* vm_base;

/* Diagnostic gate is zero outside the selected recording window. */
extern "C" int g_cell_audio_flow_trace_on;
extern "C" void ps3_audio_trace_producer_begin(uint32_t destination, uint32_t slot,
                                               uint64_t sequence, uint32_t guest_tid);
extern "C" void cellAudio_trace_producer(uint32_t destination, uint32_t slot,
                                         uint64_t sequence, uint32_t guest_tid);
extern "C" uint64_t cellAudio_sgx_fifo_begin(uint32_t ring_base, uint32_t destination, uint32_t slot);
extern "C" void cellAudio_sgx_fifo_complete(uint64_t cookie, uint32_t ring_base,
                                            uint32_t destination, uint32_t slot, uint64_t sequence);

/* ---------------------------------------------------------------------------
 * Guest setjmp / longjmp  (see gt6_recomp_hooks.h)
 * -----------------------------------------------------------------------*/
namespace {

struct JmpSlot {
    uint32_t           buf;        /* guest jmp_buf address (16-aligned, as the guest code aligns it) */
    DWORD              tid;        /* host thread that did the setjmp */
    int                anchored;   /* a host setjmp was done for it (0: reached through a pointer) */
    int                depth, vsp; /* ppu_loader's indirect-call bookkeeping at the setjmp */
    unsigned long long seq;        /* last use, for eviction */
    jmp_buf            env;
};
JmpSlot            s_jmp[128];
SRWLOCK            s_jmp_lock = SRWLOCK_INIT;
unsigned long long s_jmp_seq = 0;

JmpSlot* jmp_slot(uint32_t buf, bool create)
{
    const DWORD tid = GetCurrentThreadId();
    JmpSlot* found = nullptr; JmpSlot* victim = &s_jmp[0];
    AcquireSRWLockExclusive(&s_jmp_lock);
    for (JmpSlot& s : s_jmp) {
        if (s.buf == buf && s.tid == tid) { found = &s; break; }
        if (s.seq < victim->seq) victim = &s;
    }
    if (!found && create) { found = victim; found->buf = buf; found->tid = tid; found->anchored = 0; }
    if (found) found->seq = ++s_jmp_seq;
    ReleaseSRWLockExclusive(&s_jmp_lock);
    return found;
}

inline uint32_t jmp_buf_ea(const ppu_context* ctx) { return ((uint32_t)ctx->gpr[3] + 15u) & ~15u; }

bool peek_ok(uint32_t ea, uint32_t len);     /* defined with GT6_PEEK below */
uint32_t peek32(uint32_t ea);

/* GT6_LONGJMP_LOG=<n> (diagnostic): the first n guest longjmps (the script VM's exceptions among them) with the
 * thrower's call chain, read from the ABI back chain: [frame] = caller's frame, [caller's frame + 0x10] = its LR. */
void longjmp_log(const ppu_context* ctx, uint32_t buf)
{
    static int want = -1; static LONG n = 0;
    if (want < 0) { const char* e = getenv("GT6_LONGJMP_LOG"); want = e ? atoi(e) : 0; }
    if (want <= 0 || InterlockedIncrement(&n) > want) return;
    char b[1200];
    int p = snprintf(b, sizeof b, "[gt6-longjmp] buf=0x%08X lr=0x%08X tid=%u r4=0x%08X chain:", buf, (uint32_t)ctx->lr,
                     (unsigned)ctx->thread_id, (uint32_t)ctx->gpr[4]);
    uint32_t frame = (uint32_t)ctx->gpr[1];
    for (int d = 0; d < 70 && p < 1180; d++) {
        if (!peek_ok(frame, 8)) break;
        const uint32_t up = peek32(frame + 4);
        if (up <= frame || up - frame > 0x100000u || !peek_ok(up + 0x10, 8)) break;
        const uint32_t lr = peek32(up + 0x14);
        if (lr >= 0x10200 && lr < 0x10C2000) p += snprintf(b + p, sizeof b - p, " %08X", lr);
        auto dump = [](const char* what, uint32_t ea, uint32_t len) {
            if (!ea || !peek_ok(ea, len)) { fprintf(stderr, "[gt6-longjmp]   %s 0x%08X: not readable\n", what, ea); return; }
            char line[600]; int q = snprintf(line, sizeof line, "[gt6-longjmp]   %s 0x%08X:", what, ea);
            for (uint32_t o = 0; o < len && q < 560; o += 4) q += snprintf(line + q, sizeof line - q, " %08X", peek32(ea + o));
            fprintf(stderr, "%s\n", line);
        };
        if (lr == 0x00A217B0u && peek_ok(up + 0x80, 0x20)) {   /* the raise helper's frame: the raiser's r28 (node + 4) */
            const uint32_t node = peek32(up + 0x84) - 4;
            dump("attribute node", node - 0x60, 0xC0);
            /* The statement around it: the code array that holds this node's address, and for each instruction there
             * its opcode, line and the names it carries (a name object: vtable 0x015DA148, length +0x24, text +0x30). */
            static LONG once = 0;
            if (!InterlockedExchange(&once, 1)) {
                auto insn = [](uint32_t p) {
                    if (!p || !peek_ok(p, 0x18)) return;
                    const uint32_t w = peek32(p);
                    char line[700]; int q = snprintf(line, sizeof line, "[gt6-code]   %08X: op %02X line %u |", p, w >> 24, w & 0xFFFFFF);
                    for (uint32_t o = 4; o < 0x18; o += 4) q += snprintf(line + q, sizeof line - q, " %08X", peek32(p + o));
                    const uint32_t b = peek32(p + 8), e = peek32(p + 12);
                    if (b && e > b && e - b <= 64 && peek_ok(b, e - b))
                        for (uint32_t a = b; a < e && q < 600; a += 4) {
                            const uint32_t s = peek32(a);
                            if (s && peek_ok(s, 0x34) && peek32(s) == 0x015DA148u) {
                                const uint32_t len = peek32(s + 0x24);
                                if (len < 80 && peek_ok(s + 0x30, len + 1)) q += snprintf(line + q, sizeof line - q, " '%.*s'", (int)len, (const char*)(vm_base + s + 0x30));
                            } else q += snprintf(line + q, sizeof line - q, " <%08X>", s);
                        }
                    fprintf(stderr, "%s\n", line);
                };
                int hits = 0;
                for (uint64_t a = 0x40000000ull; a < 0x50000000ull && hits < 3; ) {
                    MEMORY_BASIC_INFORMATION mbi;
                    if (!VirtualQuery(vm_base + a, &mbi, sizeof mbi)) break;
                    const uint64_t end = (uint64_t)((uint8_t*)mbi.BaseAddress + mbi.RegionSize - vm_base);
                    if (mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
                        for (uint64_t q = a & ~3ull; q + 4 <= end && q < 0x50000000ull && hits < 3; q += 4)
                            if (peek32((uint32_t)q) == node && (uint32_t)q != up + 0x84) {
                                hits++;
                                fprintf(stderr, "[gt6-code] node 0x%08X is referenced at 0x%08llX; instructions around it:\n", node, (unsigned long long)q);
                                for (int k = -26; k <= 6; k++) {
                                    const uint32_t slot = (uint32_t)q + 4 * k;
                                    if (peek_ok(slot, 4)) insn(peek32(slot));
                                }
                            }
                    a = end > a ? end : a + 0x10000;
                }
                fprintf(stderr, "[gt6-code] %d reference(s) to the node found\n", hits);
            }
        }
        if (lr == 0x00AA74A4u && peek_ok(up + 0x74, 0x40)) {   /* m_attribute_eval.cpp NilError: its message is a local there */
            const uint32_t s = peek32(up + 0x74);
            if (s && peek_ok(s, 96)) fprintf(stderr, "[gt6-longjmp] NilError message: %.96s\n", (const char*)(vm_base + s));
            /* GT6_MEM_FIND=<text>: at the first such error, where that text is in committed guest memory (the two heap
             * windows and the pager's window), with the 0x70 bytes that start there. Host reads: nothing is paged in. */
            static LONG searched = 0;
            const char* needle = getenv("GT6_MEM_FIND");
            if (needle && *needle && !InterlockedExchange(&searched, 1)) {
                const size_t nlen = strlen(needle);
                int found = 0;
                static const uint64_t ranges[3][2] = { { 0x40000000ull, 0x50000000ull }, { 0x80000000ull, 0x90000000ull },
                                                       { 0xA0000000ull, 0xB0000000ull } };
                for (const auto& rg : ranges)
                    for (uint64_t a = rg[0]; a < rg[1]; ) {
                        MEMORY_BASIC_INFORMATION mbi;
                        if (!VirtualQuery(vm_base + a, &mbi, sizeof mbi)) break;
                        uint64_t end = (uint64_t)((uint8_t*)mbi.BaseAddress + mbi.RegionSize - vm_base);
                        if (end > rg[1]) end = rg[1];
                        if (mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
                            for (uint64_t q = a; q + nlen + 0x70 <= end; q++) {
                                const uint8_t* m = vm_base + q;
                                if (m[0] != (uint8_t)needle[0] || memcmp(m, needle, nlen)) continue;
                                if (found++ < 24) {
                                    char line[400]; int n = snprintf(line, sizeof line, "[gt6-find] 0x%08llX:", (unsigned long long)q);
                                    for (int o = 0; o < 0x70; o += 4)
                                        n += snprintf(line + n, sizeof line - n, " %02X%02X%02X%02X", m[o], m[o + 1], m[o + 2], m[o + 3]);
                                    fprintf(stderr, "%s\n", line);
                                }
                            }
                        a = end > a ? end : a + 0x10000;
                    }
                fprintf(stderr, "[gt6-find] '%s': %d place(s)\n", needle, found);
            }
            for (int r = 0; r < 4; r++) {                      /* the executor's r28..r31, saved by the raiser */
                char name[24]; snprintf(name, sizeof name, "executor r%d ->", 28 + r);
                dump(name, peek32(up + 0x94 + 8 * r), 0x60);
            }
        }
        frame = up;
    }
    fprintf(stderr, "%s\n", b);
}

}   // namespace

extern "C" jmp_buf* gt6_setjmp_enter(ppu_context* ctx)
{
    const uint32_t buf = jmp_buf_ea(ctx);
    func_00C61F88_guest(ctx);                  /* guest half: registers -> guest jmp_buf, r3 = 0 */
    JmpSlot* s = jmp_slot(buf, true);
    s->anchored = 1;
    s->depth = *ps3_ppu_dispatch_depth_slot();
    s->vsp   = *ps3_ppu_vcall_sp_slot();
    return &s->env;
}

extern "C" void gt6_setjmp_landed(ppu_context* ctx)
{
    /* Back in the setjmp frame, the frames above it gone without returning: nothing of theirs is pending. */
    g_trampoline_fn = nullptr;
    if (JmpSlot* s = jmp_slot(jmp_buf_ea(ctx), false)) { (void)s; }
}

/* setjmp reached through a function pointer (the function table): there is no lifted caller frame to anchor a host
 * setjmp in, so only the guest half runs and a longjmp to this buffer behaves as it did before the hook. */
void (func_00C61F88)(ppu_context* ctx)
{
    const uint32_t buf = jmp_buf_ea(ctx);
    static int n = 0;
    if (n < 8) { n++; fprintf(stderr, "[gt6] setjmp(buf=0x%08X) called through a pointer (lr=0x%08X): guest half only\n",
                              buf, (uint32_t)ctx->lr); }
    func_00C61F88_guest(ctx);
    if (JmpSlot* s = jmp_slot(buf, false)) s->anchored = 0;
}

void func_00C620AC(ppu_context* ctx)
{
    const uint32_t buf = jmp_buf_ea(ctx);
    JmpSlot* s = jmp_slot(buf, false);
    longjmp_log(ctx, buf);
    func_00C620AC_guest(ctx);                  /* guest half: guest jmp_buf -> registers, r3 = the value (never 0) */
    if (s && s->anchored) {
        *ps3_ppu_dispatch_depth_slot() = s->depth;
        *ps3_ppu_vcall_sp_slot()       = s->vsp;
        longjmp(s->env, 1);
    }
    static int n = 0;
    if (n < 8) { n++; fprintf(stderr, "[gt6] longjmp(buf=0x%08X) has no host setjmp to return to: guest half only\n", buf); }
}

/* ---------------------------------------------------------------------------
 * GT6_DTRACE=1 [GT6_DTRACE_MAX=<lines, default 600>]   (diagnostic)
 * The functions wrapped by `tools\post_lift.py recompiled --trace <addr,...>` log every call, direct ones included:
 * thread, lr, r3..r6 on entry and r3 on return (a function that ends in a tail call "returns" before the callee runs).
 * -----------------------------------------------------------------------*/
namespace {
bool peek_ok(uint32_t ea, uint32_t len);     /* defined with GT6_PEEK below */
uint32_t peek32(uint32_t ea);
volatile LONG s_dtrace_lines = 0;
int s_dt_reg = -1, s_dt_bt = 0;          /* GT6_DTRACE_FILTER=<reg>:<min>  GT6_DTRACE_BT=<code pointers to print> */
int s_dt_tid = -1;                       /* GT6_DTRACE_TID=<guest thread id>: only that thread */
uint32_t s_dt_min = 0;
int dtrace_on(void)
{
    static int on = -1, max = 600;
    if (on < 0) {
        const char* e = getenv("GT6_DTRACE");
        if (const char* m = getenv("GT6_DTRACE_MAX")) max = atoi(m);
        if (const char* f = getenv("GT6_DTRACE_FILTER")) {     /* only calls whose gpr[reg] >= min; leave lines are dropped */
            const char* c = strchr(f, ':');
            s_dt_min = c ? (uint32_t)strtoul(c + 1, NULL, 0) : 0; s_dt_reg = atoi(f);
        }
        if (const char* b = getenv("GT6_DTRACE_BT")) s_dt_bt = atoi(b);
        if (const char* t = getenv("GT6_DTRACE_TID")) s_dt_tid = atoi(t);
        on = (e && *e && *e != '0') ? 1 : 0;
    }
    return on && s_dtrace_lines < max;
}
}   // namespace

/* GT6_DTRACE_HIST=<seconds>: for the traced heap entries func_00B0C524 (size in r5) and func_00B0A12C (size in r4),
 * no line per call: bytes and calls are summed per (size class, first three code pointers on the stack outside the
 * allocator) and the top of that table is printed every <seconds>, then cleared. */
namespace {
struct dt_hist_ent { uint32_t k[3], bucket, count, used; uint64_t bytes; };
dt_hist_ent s_dt_hist[8192];
SRWLOCK s_dt_hist_lock = SRWLOCK_INIT;
DWORD s_dt_hist_last = 0;
void dt_hist(unsigned addr, ppu_context* ctx, int period_s)
{
    /* also: func_00B0BFE8 (page runs, r5 = 8 KB pages) and func_00B0D2B4 (a mapper block: r3 = 0 -> 64 KB, else 1 MB;
     * its key keeps the heap's own code pointers, to show which heap path asks for the block) */
    const bool block = addr == 0x00B0D2B4u;
    const uint32_t size = (addr == 0x00B0A12Cu) ? (uint32_t)ctx->gpr[4]
                        : (addr == 0x00B0BFE8u) ? (uint32_t)ctx->gpr[5] << 13
                        : block ? ((uint32_t)ctx->gpr[3] ? 0x100000u : 0x10000u) : (uint32_t)ctx->gpr[5];
    uint32_t k[3] = { 0, 0, 0 }; int n = 0;
    const uint32_t sp = (uint32_t)ctx->gpr[1];
    if (block) k[n++] = (uint32_t)ctx->lr;           /* the heap path that asks for the block */
    __try {
        for (uint32_t a = sp; n < 3 && a < sp + 0x1000; a += 4) {
            if ((!(a & 0xFFF) || a == sp) && !peek_ok(a, 4)) break;
            const uint32_t v = peek32(a);
            if (v < 0x10200 || v >= 0x10C2000 || (v & 3)) continue;
            if ((v >= 0xA1EB00 && v < 0xA1F100) || (v >= 0xAEFD00 && v < 0xB0E000)) continue;   /* operator new, heap */
            k[n++] = v;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        static LONG once = 0;
        if (!InterlockedExchange(&once, 1)) fprintf(stderr, "[gt6-heap-hist] stack scan faulted (func_%08X tid=%u r1=0x%08X)\n", addr, (unsigned)ctx->thread_id, sp);
    }
    if (addr == 0x00B0BFE8u) k[2] = 0xBFE80000u | ((uint32_t)ctx->gpr[7] & 1);      /* marks the page-run rows; bit 0 = "from the end" */
    unsigned long b = 0; _BitScanReverse(&b, size | 1);
    AcquireSRWLockExclusive(&s_dt_hist_lock);
    uint32_t h = (k[0] * 2654435761u ^ k[1] * 40503u ^ k[2] * 97u ^ (uint32_t)b) & 8191;
    for (int probe = 0; probe < 8192; probe++, h = (h + 1) & 8191) {
        dt_hist_ent& e = s_dt_hist[h];
        if (!e.used) { e.used = 1; e.k[0] = k[0]; e.k[1] = k[1]; e.k[2] = k[2]; e.bucket = (uint32_t)b; }
        if (e.bucket == b && e.k[0] == k[0] && e.k[1] == k[1] && e.k[2] == k[2]) { e.bytes += size; e.count++; break; }
    }
    const DWORD now = GetTickCount();
    if (!s_dt_hist_last) s_dt_hist_last = now;
    if (now - s_dt_hist_last >= (DWORD)period_s * 1000) {
        s_dt_hist_last = now;
        uint64_t total = 0; for (auto& e : s_dt_hist) total += e.bytes;
        fprintf(stderr, "[gt6-heap-hist] last %d s: %.1f MB requested\n", period_s, total / 1048576.0);
        for (int top = 0; top < 24; top++) {
            dt_hist_ent* best = nullptr;
            for (auto& e : s_dt_hist) if (e.bytes && (!best || e.bytes > best->bytes)) best = &e;
            if (!best) break;
            fprintf(stderr, "[gt6-heap-hist]   %8.2f MB  %7u calls  size>=0x%X  from %08X %08X %08X\n", best->bytes / 1048576.0,
                    best->count, 1u << best->bucket, best->k[0], best->k[1], best->k[2]);
            best->bytes = 0;
        }
        memset(s_dt_hist, 0, sizeof s_dt_hist);
    }
    ReleaseSRWLockExclusive(&s_dt_hist_lock);
}
}   // namespace

volatile uint32_t g_gt6_flush_obj = 0, g_gt6_flush_flag = 0, g_gt6_flush_calls = 0;
extern "C" void (*g_gcm_trace_note)(const char* fmt, ...);     /* cellGcmSys.c */
extern "C" void spu_atom_ring_note(const char* fmt, ...);      /* spu_channels.c */
extern "C" void (*g_gcm_guest_write_note)(uint32_t location, uint32_t offset, uint32_t size);   /* cellGcmSys.c */
extern "C" void rsx_live_draw_note_guest_write(uint32_t location, uint32_t offset, uint32_t size);
extern "C" void rsx_live_draw_picture_log(const char* fmt, ...);

/* func_00B31A00(obj, spurs, priority[8], maxContention, 16, ...) registers one job-manager queue ([obj]) as a SPURS
 * workload (_cellSpursWorkloadAttributeInitialize + cellSpursAddWorkloadWithAttribute). Each registration is logged.
 * GT6_WKL_NOSPU0=<hex queue>: that queue's workload gets priority 0 on SPU 0 (SPU 0 never serves it). The caller's
 * table is changed only for the length of the call and put back on the way out. */
static thread_local uint32_t t_wkl_prio_ea = 0;
static thread_local uint8_t  t_wkl_prio0 = 0;
static void wkl_register_enter(ppu_context* ctx)
{
    static uint32_t nospu0 = 1;
    if (nospu0 == 1) { const char* e = getenv("GT6_WKL_NOSPU0"); nospu0 = (e && *e) ? (uint32_t)strtoul(e, nullptr, 16) : 0; }
    const uint32_t self = (uint32_t)ctx->gpr[3], prio = (uint32_t)ctx->gpr[5];
    if (!peek_ok(self, 4) || !prio || !peek_ok(prio, 8)) return;
    const uint32_t queue = peek32(self);
    uint8_t* p = vm_base + prio;
    fprintf(stderr, "[gt6-wkl] register queue 0x%08X (obj 0x%08X) priority %02X %02X %02X %02X %02X %02X %02X %02X"
            " maxContention %u arg5 %u arg6 0x%08X from 0x%08X\n", queue, self, p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7],
            (unsigned)ctx->gpr[6], (unsigned)ctx->gpr[7], (uint32_t)ctx->gpr[8], (uint32_t)ctx->lr);
    if (nospu0 && queue == nospu0 && p[0]) {
        t_wkl_prio_ea = prio; t_wkl_prio0 = p[0]; p[0] = 0;
        fprintf(stderr, "[gt6-wkl] queue 0x%08X: SPU 0 does not serve this workload (GT6_WKL_NOSPU0)\n", queue);
    }
    /* The serial render queue (0x017C1700) is registered for SPU 0 only (priority 01 00 00 00 00, one SPU at a
     * time). Its job opens the gate at the head of each frame's RSX list. The last of the frame's five parallel
     * jobs (the other queue, every SPU) meanwhile waits, spinning, for the label the RSX releases at the end of a
     * list -- and when that job happens to be the one on SPU 0 and the release it needs is the one behind the
     * closed gate, nothing can ever open it: the title stands still (see NOTES.md, 2026-11-15 19:15). On the
     * console the RSX is slow enough that the release in question is always the previous list's; here the order
     * could turn round. The fix for that is in the runtime (GCM_LABEL_BANK, cellGcmSys.c) and leaves the title's
     * registration alone. What follows is the experiment that showed it (GT6_SERIAL_ANY_SPU=1, off by default):
     * the other SPUs may run the serial queue too, at the lowest priority; it stays one job at a time and in
     * order (maxContention 1). It removes the SPU 0 case but not the frames whose list needs jobs behind the
     * wait, so it is not a fix. */
    static int any_spu = -1;      /* off unless GT6_SERIAL_ANY_SPU=1: the label fix (GCM_LABEL_BANK) made it unnecessary */
    if (any_spu < 0) { const char* e = getenv("GT6_SERIAL_ANY_SPU"); any_spu = (e && *e == '1') ? 1 : 0; }
    if (any_spu && queue == 0x017C1700u && p[0] && !p[1] && !p[2] && !p[3] && !p[4]) {
        p[1] = p[2] = p[3] = p[4] = 0x0F;
        fprintf(stderr, "[gt6-wkl] queue 0x%08X: SPUs 1-4 may run this workload too, at priority 15 (GT6_SERIAL_ANY_SPU)\n", queue);
    }
}
static void wkl_register_leave(void)
{
    if (t_wkl_prio_ea) { vm_base[t_wkl_prio_ea] = t_wkl_prio0; t_wkl_prio_ea = 0; }
}

/* Diagnostic isolation of the combined GPU DoF pass. Unlike the game's
 * NO_DOF option, this skips the entire command, including its other mode.
 * GT6_DIAG_SKIP_DOF=0 enables counters without skipping; =1 skips from boot.
 * The skipdof A/B switch may change only the flag while the game is running. */
extern "C" { int g_gt6_diag_skip_dof = 0; }
static volatile LONG s_dof_diag_enabled = 0;
extern "C" int gt6_diag_skip_dof(ppu_context* ctx)
{
    const int skip = g_gt6_diag_skip_dof != 0;
    if (skip) InterlockedExchange(&s_dof_diag_enabled, 1);
    if (!s_dof_diag_enabled) return 0;
    static volatile LONG64 calls = 0, skipped = 0, last_log = 0;
    static volatile LONG last_skip = -1;
    const LONG64 n = InterlockedIncrement64(&calls);
    const LONG64 s = skip ? InterlockedIncrement64(&skipped) : InterlockedCompareExchange64(&skipped, 0, 0);
    const LONG old_skip = InterlockedExchange(&last_skip, skip);
    const LONG64 now = (LONG64)GetTickCount64();
    const LONG64 was = InterlockedCompareExchange64(&last_log, 0, 0);
    if (n <= 3 || old_skip != skip ||
        (now - was >= 5000 && InterlockedCompareExchange64(&last_log, now, was) == was)) {
        InterlockedExchange64(&last_log, now);
        fprintf(stderr, "[gt6-dof-diag] ms=%lld calls=%lld passed=%lld skipped=%lld skip=%d tid=%u lr=%08X cmd=%08X context=%08X\n",
                (long long)now, (long long)n, (long long)(n - s), (long long)s, skip,
                (unsigned)ctx->thread_id, (uint32_t)ctx->lr, (uint32_t)ctx->gpr[3], (uint32_t)ctx->gpr[4]);
    }
    return skip;
}

/* ---------------------------------------------------------------------------
 * GT6_NO_MLAA=1: no SPU post-process.
 * The first call (0x008C2004, copy=1) stages the GPU scene for the SPU consumer;
 * the second (0x008C2060, copy=0) submits the MLAA jobs. This bypass skips the
 * staging and job wait (0x008C2080), keeps the GPU glare composite (0x008C3044),
 * and replaces the submit with the game's GPU scene copy (0x00C3E050).
 * tools\post_lift.py retains these four guarded call-site changes from the
 * BCES01893 1.22 Disable MLAA patch; the helpers below supply its copy arguments.
 * main.cpp selects this path by default for GT6_AA=gpu, or explicitly for off;
 * the GPU mode adds edge-directed AA to the completed display. GT6_AA=spu retains the original job.
 * -------------------------------------------------------------------------*/
extern "C" int gt6_no_spu_post(void)
{
    static int on = -1;
    if (on < 0) {
        const char* e = getenv("GT6_NO_MLAA");
        on = (e && *e == '1') ? 1 : 0;
        if (on) fprintf(stderr, "[gt6] GT6_NO_MLAA: no SPU post-process; the scene is copied to the display buffer on the GPU\n");
    }
    return on;
}

/* ---------------------------------------------------------------------------
 * GT6_ASPECT: ultrawide / custom 3D aspect ratio (BCES01893 1.22, 0x008A615C).
 * Mirrors AlexHaust's RPCS3 Aspect Ratio patch: when the game's aspect getter
 * returns a 16:9 ratio in [1.70, 1.85] in f1, substitute g_gt6_aspect.
 * -------------------------------------------------------------------------*/
extern "C" float g_gt6_aspect = 0.0f;

void gt6_aspect_override(ppu_context* ctx)
{
    const float target = g_gt6_aspect;
    if (target > 0.5f && ctx->fpr[1] >= 1.70 && ctx->fpr[1] <= 1.85) {
        static int logged = 0;
        if (!logged) {
            logged = 1;
            fprintf(stderr, "[gt6-aspect] 0x008A615C hook active: %.4f -> %.4f\n",
                    (double)ctx->fpr[1], (double)target);
        }
        ctx->fpr[1] = (double)target;
    }
}

/* r3 = display buffer descriptor, r4 = scene buffer descriptor: { u32 address, u32 pitch, ..., u16 width @0x14,
 * u16 height @0x16, u16 viewport x0 @0x18, y0 @0x1A, x1 @0x1C, y1 @0x1E }. Sets up
 * func_00C3E050(dst address, dst pitch, dst x, dst y, src address, src pitch, src x, src y, width, height, 4)
 * in a frame of its own, as the patch's code cave does. */
extern "C" int gt6_scene_copy_args(ppu_context* ctx)
{
    const uint32_t d = (uint32_t)ctx->gpr[3], s = (uint32_t)ctx->gpr[4];
    { static unsigned n = 0, shown = 0; static uint32_t last_d = 1, last_s = 1;   /* whenever the pair of buffers changes */
      const int changed = d != last_d || s != last_s || (n % 500u) == 0;
      last_d = d; last_s = s;
      if (changed && shown < 80 && ++shown)
          fprintf(stderr, "[gt6-nomlaa] #%u dst %08X {addr %08X pitch %u size %ux%u viewport %u,%u-%u,%u} "
                          "src %08X {addr %08X pitch %u size %ux%u viewport %u,%u-%u,%u}%s\n", n,
                  d, vm_read32(d), vm_read32(d + 4), vm_read16(d + 0x14), vm_read16(d + 0x16),
                  vm_read16(d + 0x18), vm_read16(d + 0x1A), vm_read16(d + 0x1C), vm_read16(d + 0x1E),
                  s, vm_read32(s), vm_read32(s + 4), vm_read16(s + 0x14), vm_read16(s + 0x16),
                  vm_read16(s + 0x18), vm_read16(s + 0x1A), vm_read16(s + 0x1C), vm_read16(s + 0x1E),
                  vm_read32(d) == vm_read32(s) ? "  (the same buffer: nothing copied)" : "");
      n++; }
    if (vm_read32(d) == vm_read32(s)) return 0;                 /* the same buffer */
    const uint64_t sp = ctx->gpr[1];
    vm_write64((uint32_t)sp + 0x10, ctx->lr);                   /* a callee's LR slot in the caller's frame */
    vm_write64((uint32_t)sp - 0x100, sp);                       /* stdu r1, -0x100(r1) */
    ctx->gpr[1] = sp - 0x100;
    uint32_t sx = vm_read16(s + 0x18), sy = vm_read16(s + 0x1A);
    int32_t w = (int32_t)vm_read16(s + 0x1C) - (int32_t)sx, h = (int32_t)vm_read16(s + 0x1E) - (int32_t)sy;
    uint32_t dx = vm_read16(d + 0x18), dy = vm_read16(d + 0x1A);
    if (w <= 0 || h <= 0) {                                     /* no scene viewport: the whole buffer */
        sx = sy = dx = dy = 0;
        w = (int32_t)vm_read16(s + 0x14); h = (int32_t)vm_read16(s + 0x16);
    }
    const uint32_t r1 = (uint32_t)ctx->gpr[1];
    vm_write64(r1 + 0x70, (uint64_t)(uint32_t)w);
    vm_write64(r1 + 0x78, (uint64_t)(uint32_t)h);
    vm_write64(r1 + 0x80, 4);                                   /* bytes per pixel */
    ctx->gpr[7] = vm_read32(s);     ctx->gpr[8] = vm_read32(s + 4);
    ctx->gpr[9] = sx;               ctx->gpr[10] = sy;
    ctx->gpr[5] = dx;               ctx->gpr[6] = dy;
    ctx->gpr[4] = vm_read32(d + 4); ctx->gpr[3] = vm_read32(d);
    return 1;
}
extern "C" void gt6_scene_copy_done(ppu_context* ctx)
{
    ctx->gpr[1] += 0x100;
    ctx->lr = vm_read64((uint32_t)ctx->gpr[1] + 0x10);
}

/* The race's driver-change step: tools/post_lift.py renames the lifted body to func_00482914_guest, a host hook point. */
void func_00482914_guest(ppu_context* ctx);
void func_00482914(ppu_context* ctx)
{
    func_00482914_guest(ctx);
}

extern "C" void gt6_trace_enter(unsigned addr, ppu_context* ctx)
{
    if (addr == 0x00B321F0u) {          /* the job manager's "wait until every reader has caught up": remember its queue */
        g_gt6_flush_obj = (uint32_t)ctx->gpr[3]; g_gt6_flush_flag = (uint32_t)ctx->gpr[4]; g_gt6_flush_calls++;
        if (!getenv("GT6_DTRACE")) return;
    }
    if (addr == 0x00B31A00u) {
        wkl_register_enter(ctx);
        if (!getenv("GT6_DTRACE")) return;
    }
    static int hist = -1;
    if (hist < 0) { const char* e = getenv("GT6_DTRACE_HIST"); hist = e ? atoi(e) : 0; }
    if (hist > 0) { dt_hist(addr, ctx, hist); return; }
    if (!dtrace_on()) return;
    if (s_dt_reg >= 0 && (uint32_t)ctx->gpr[s_dt_reg & 31] < s_dt_min) return;
    if (s_dt_tid >= 0 && (int)ctx->thread_id != s_dt_tid) return;
    InterlockedIncrement(&s_dtrace_lines);
    char bt[1024]; int n = 0, len = 0; bt[0] = 0;
    if (s_dt_bt > 0) {      /* code pointers found on the guest stack above r1 (not an exact call chain) */
        const uint32_t sp = (uint32_t)ctx->gpr[1];
        for (uint32_t a = sp; n < s_dt_bt && a < sp + 0x3000 && len < 1000; a += 4) {
            if (!(a & 0xFFF) || a == sp) { if (!peek_ok(a, 4)) break; }
            const uint32_t v = peek32(a);
            if (v >= 0x10200 && v < 0x10C2000 && !(v & 3) && v != 0x001FFFFF) { len += snprintf(bt + len, 12, " %08X", v); n++; }
        }
    }
    fprintf(stderr, "[gt6-dtrace] > func_%08X tid=%u lr=0x%08X r3=0x%08X r4=0x%08X r5=0x%08X r6=0x%08X"
            " r24=0x%08X r25=0x%08X r27=0x%08X r30=0x%08X%s%s\n", addr,
            (unsigned)ctx->thread_id, (uint32_t)ctx->lr, (uint32_t)ctx->gpr[3], (uint32_t)ctx->gpr[4],
            (uint32_t)ctx->gpr[5], (uint32_t)ctx->gpr[6], (uint32_t)ctx->gpr[24], (uint32_t)ctx->gpr[25],
            (uint32_t)ctx->gpr[27], (uint32_t)ctx->gpr[30], n ? " bt:" : "", bt);
}

extern "C" void gt6_trace_leave(unsigned addr, ppu_context* ctx)
{
    if (addr == 0x00B31A00u) wkl_register_leave();
    if (!dtrace_on() || s_dt_reg >= 0) return;
    if (s_dt_tid >= 0 && (int)ctx->thread_id != s_dt_tid) return;
    InterlockedIncrement(&s_dtrace_lines);
    fprintf(stderr, "[gt6-dtrace] < func_%08X tid=%u r3=0x%016llX\n", addr, (unsigned)ctx->thread_id, (unsigned long long)ctx->gpr[3]);
}

/* ---------------------------------------------------------------------------
 * Paging (sys_mmapper page-fault notification) -- not implemented yet.
 * The game enables it on two 256 MB regions and maps 64 KB pages into them from
 * its fault-handler thread. The flat VM commits a zero page on first touch,
 * which is what that handler does for a fresh page; swapping to
 * /dev_hdd1/pditsm/pagefile.sys never starts because the game sees no faults.
 * -----------------------------------------------------------------------*/
extern "C" int ps3_mmapper_page_fault(uint32_t addr, int writing);   /* runtime/syscalls/sys_memory.c */

extern "C" int gt6_page_fault(EXCEPTION_POINTERS* ep, uint32_t guest_addr)
{
    /* 1 = the title's pager has mapped the page (or is not needed): try the access again.
     * 0 = not a page the title pages itself: main.cpp commits it. */
    return ps3_mmapper_page_fault(guest_addr, ep->ExceptionRecord->ExceptionInformation[0] == 1);
}

/* ---------------------------------------------------------------------------
 * GT6_TRACE_FN=<hex addr>[,<hex addr>...]   (diagnostic, up to 16)
 * Logs every call of those guest functions that goes through the function table
 * -- calls through a pointer: thread bodies, virtual methods, callbacks -- with
 * the calling thread, lr and r3..r5, then runs the function. Direct calls from
 * lifted code do not pass here.
 * -----------------------------------------------------------------------*/
extern "C" void ppu_register_function(uint64_t addr, void (*fn)(ppu_context*));

namespace {
struct TraceSlot { uint32_t addr; void (*orig)(ppu_context*); volatile LONG calls; uint32_t last_r3; };
TraceSlot s_trace[16];
bool peek_ok(uint32_t ea, uint32_t len);

/* Every 5 s: how often each traced function has run, and the first 0x40 bytes its last r3 points at. */
void trace_summary(void)
{
    static volatile LONG64 last = 0;
    const LONG64 now = (LONG64)GetTickCount64(), was = last;
    if (now - was < 5000 || InterlockedCompareExchange64(&last, now, was) != was) return;
    for (int i = 0; i < 16 && s_trace[i].orig; i++) {
        const uint32_t p = s_trace[i].last_r3;
        char hex[0x40 * 2 + 0x10 + 1]; size_t o = 0;
        if (p >= 0x10000u && peek_ok(p, 0x40))
            for (uint32_t k = 0; k < 0x40; k++) {
                o += (size_t)snprintf(hex + o, sizeof hex - o, "%02X", vm_read8(p + k));
                if ((k & 3) == 3 && o < sizeof hex - 1) hex[o++] = ' ';
            }
        hex[o] = 0;
        fprintf(stderr, "[gt6-trace] %lld ms func_%08X calls %ld r3=%08X: %s\n", (long long)now, s_trace[i].addr,
                (long)s_trace[i].calls, p, hex);
    }
}

void trace_call(int i, ppu_context* ctx)
{
    TraceSlot& t = s_trace[i];
    const LONG n = InterlockedIncrement(&t.calls);
    t.last_r3 = (uint32_t)ctx->gpr[3];
    trace_summary();
    if (n <= 40 || (n % 1000) == 0)
        fprintf(stderr, "[gt6-trace] func_%08X #%ld tid=%u lr=0x%08X r3=0x%08X r4=0x%08X r5=0x%08X\n", t.addr, (long)n,
                (unsigned)ctx->thread_id, (uint32_t)ctx->lr, (uint32_t)ctx->gpr[3], (uint32_t)ctx->gpr[4], (uint32_t)ctx->gpr[5]);
    t.orig(ctx);
    if (n <= 40) fprintf(stderr, "[gt6-trace] func_%08X #%ld returned r3=0x%08X (tid=%u)\n", t.addr, (long)n,
                         (uint32_t)ctx->gpr[3], (unsigned)ctx->thread_id);
}
#define GT6_TRACE_THUNK(i) void trace_thunk_##i(ppu_context* c) { trace_call(i, c); }
GT6_TRACE_THUNK(0)  GT6_TRACE_THUNK(1)  GT6_TRACE_THUNK(2)  GT6_TRACE_THUNK(3)
GT6_TRACE_THUNK(4)  GT6_TRACE_THUNK(5)  GT6_TRACE_THUNK(6)  GT6_TRACE_THUNK(7)
GT6_TRACE_THUNK(8)  GT6_TRACE_THUNK(9)  GT6_TRACE_THUNK(10) GT6_TRACE_THUNK(11)
GT6_TRACE_THUNK(12) GT6_TRACE_THUNK(13) GT6_TRACE_THUNK(14) GT6_TRACE_THUNK(15)
void (*const s_trace_thunks[16])(ppu_context*) = {
    trace_thunk_0, trace_thunk_1, trace_thunk_2,  trace_thunk_3,  trace_thunk_4,  trace_thunk_5,  trace_thunk_6,  trace_thunk_7,
    trace_thunk_8, trace_thunk_9, trace_thunk_10, trace_thunk_11, trace_thunk_12, trace_thunk_13, trace_thunk_14, trace_thunk_15 };

void install_traces(void)
{
    const char* e = getenv("GT6_TRACE_FN");
    if (!e || !*e) return;
    int n = 0;
    while (*e && n < 16) {
        char* end = nullptr;
        const uint32_t addr = (uint32_t)strtoul(e, &end, 16);
        if (end == e) break;
        void (*orig)(ppu_context*) = nullptr;
        for (uint64_t k = 0; k < function_table_count; k++)
            if (function_table[k].addr == addr) { orig = function_table[k].func; break; }
        if (orig) {
            s_trace[n].addr = addr; s_trace[n].orig = orig;
            ppu_register_function(addr, s_trace_thunks[n]);
            fprintf(stderr, "[gt6-trace] tracing func_%08X\n", addr);
            n++;
        } else {
            fprintf(stderr, "[gt6-trace] 0x%08X is not a lifted function entry\n", addr);
        }
        e = (*end == ',') ? end + 1 : end;
    }
}
/* ---------------------------------------------------------------------------
 * GT6_PEEK=<spec>[;<spec>...]   (diagnostic)   spec = <hex addr>[><hex off>...]:<hex len>
 * Dumps guest memory every GT6_PEEK_MS (default 2000) ms. Each `>off` follows a pointer: addr = read32(addr) + off.
 *   GT6_PEEK=17C1690>0:80          0x80 bytes at [0x17C1690]
 *   GT6_PEEK=17C1690>30>0:40       0x40 bytes at [[0x17C1690] + 0x30]
 * Reads are guarded: an uncommitted page prints as such instead of faulting.
 * -----------------------------------------------------------------------*/
extern "C" uint8_t* vm_base;

bool peek_ok(uint32_t ea, uint32_t len)
{
    MEMORY_BASIC_INFORMATION mbi;
    for (uint64_t a = ea & ~0xFFFull; a < (uint64_t)ea + len; a += 0x1000) {
        if (!VirtualQuery(vm_base + a, &mbi, sizeof mbi)) return false;
        if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
    }
    return true;
}
uint32_t peek32(uint32_t ea)
{
    const uint8_t* p = vm_base + ea;
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

#include "gt6_bgm_diag.inc"

DWORD WINAPI peek_thread(void* arg)
{
    const char* specs = (const char*)arg;
    const char* ms = getenv("GT6_PEEK_MS");
    const DWORD period = ms ? (DWORD)atoi(ms) : 2000;
    for (unsigned pass = 0;; pass++) {
        Sleep(period);
        gt6_bgm_diag_tick(pass);
        for (const char* e = specs; *e;) {
            char* end = nullptr;
            uint32_t addr = (uint32_t)strtoul(e, &end, 16);
            char path[160]; int pl = snprintf(path, sizeof path, "%08X", addr);
            bool ok = true;
            while (*end == '>') {
                const uint32_t off = (uint32_t)strtoul(end + 1, &end, 16);
                if (!peek_ok(addr, 4)) { ok = false; break; }
                addr = peek32(addr) + off;
                if (pl < (int)sizeof path - 24) pl += snprintf(path + pl, sizeof path - pl, ">%X=%08X", off, addr);
            }
            uint32_t len = 0x40;
            if (ok && *end == ':') len = (uint32_t)strtoul(end + 1, &end, 16);
            if (len > 0x400) len = 0x400;
            if (!ok || !peek_ok(addr, len)) {
                fprintf(stderr, "[gt6-peek %u] %s: not readable\n", pass, path);
            } else {
                for (uint32_t o = 0; o < len; o += 32) {
                    char line[160]; int n = 0;
                    for (uint32_t k = o; k < o + 32 && k < len; k += 4)
                        n += snprintf(line + n, sizeof line - n, "%08X ", peek32(addr + k));
                    fprintf(stderr, "[gt6-peek %u] %s +%03X: %s\n", pass, path, o, line);
                }
            }
            while (*end && *end != ';' && *end != ',') end++;      /* ',' too: quiet_run.ps1 -Env splits on ';' */
            e = *end ? end + 1 : end;
        }
        fflush(stderr);
    }
}
/* GT6_THREADS=<seconds>[,<reports>]   (diagnostic): every guest thread's lr / sp / back chain, every n seconds
 * (default 4 reports), whether or not frames are being presented -- the freeze watchdog only speaks when they stop. */
extern "C" void ppu_report_guest_lrs(void);
extern "C" void ps3_fiber_contexts_report(void);     /* cellFiber.c: the fiber contexts, same format */
extern "C" void ps3_spu_threads_report(void);        /* lv2_register.c: the SPU threads and their SPURS instance */
DWORD WINAPI threads_thread(void* arg)
{
    const char* e = (const char*)arg;
    const int secs = atoi(e) > 0 ? atoi(e) : 5;
    const char* c = strchr(e, ',');
    const int reports = c ? atoi(c + 1) : 4;
    for (int i = 0; i < reports; i++) {
        Sleep((DWORD)secs * 1000);
        fprintf(stderr, "[gt6-threads] report %d (t+%ds); guest threads:\n", i + 1, (i + 1) * secs);
        ppu_report_guest_lrs();
        ps3_fiber_contexts_report();
        ps3_spu_threads_report();
        if (g_gt6_flush_obj && peek_ok(g_gt6_flush_obj, 4)) {      /* with post_lift --trace B321F0 */
            const uint32_t B0 = peek32(g_gt6_flush_obj);
            fprintf(stderr, "[gt6-flush] obj=0x%08X flag=%u calls=%u queue=0x%08X\n", g_gt6_flush_obj, g_gt6_flush_flag, g_gt6_flush_calls, B0);
            /* the render job queue the wait is on, then the serial queue (SPURS workload 6, SPU 0 only) */
            const uint32_t queues[2] = { B0, 0x017C1700u };
            for (int qi = 0; qi < 2; qi++) {
                const uint32_t B = queues[qi];
                if (!B || !peek_ok(B, 0x60)) continue;
                fprintf(stderr, "[gt6-flush] queue 0x%08X:\n", B);
                for (uint32_t o = 0x30; o < 0x60; o += 0x10)
                    fprintf(stderr, "[gt6-flush]   +%02X: %08X %08X %08X %08X\n", o, peek32(B + o), peek32(B + o + 4), peek32(B + o + 8), peek32(B + o + 12));
                fprintf(stderr, "[gt6-flush]   +00: %08X %08X %08X %08X  +10: %08X %08X %08X %08X  +20: %08X %08X %08X %08X\n",
                        peek32(B), peek32(B + 4), peek32(B + 8), peek32(B + 12), peek32(B + 16), peek32(B + 20), peek32(B + 24),
                        peek32(B + 28), peek32(B + 32), peek32(B + 36), peek32(B + 40), peek32(B + 44));
                for (uint32_t f = 0; f < 2; f++) {      /* every entry of both lists, and what each one points at */
                    const uint32_t E = peek32(B + 0x30 + 4 * f);
                    if (!E || !peek_ok(E, 0x100)) continue;
                    for (uint32_t k = 0; k < 20; k++) {
                        const uint32_t hi = peek32(E + 8 * k), lo = peek32(E + 8 * k + 4);
                        if (!hi && !lo) break;
                        char line[400]; int q = snprintf(line, sizeof line, "[gt6-flush]   list %u [%2u] %08X:%08X", f, k, hi, lo);
                        if (lo >= 0x10000u && peek_ok(lo, 0x40))
                            for (uint32_t o = 0; o < 0x40; o += 4) q += snprintf(line + q, sizeof line - q, "%s%08X", o ? " " : "  -> ", peek32(lo + o));
                        fprintf(stderr, "%s\n", line);
                    }
                }
            }
        }
        fflush(stderr);
    }
    return 0;
}
volatile LONG s_watch_trap_armed = 0;
volatile LONG s_watch_traps_left = 6;
uint32_t s_watch_trap_ea = 0;
/* GT6_WATCH_COMMIT=<hex addr>   (diagnostic): polls (every ms) whether the host has that guest page committed and
 * prints each change with the page's first bytes -- the log lines around it say what was going on. Finds memory
 * that appears in a paged region without the title's pager having mapped it. */
DWORD WINAPI watch_commit_thread(void* arg)
{
    const uint32_t ea = (uint32_t)strtoul((const char*)arg, nullptr, 16) & ~0xFFFFu;
    int was = -1; uint32_t last0 = 0;
    for (;;) {
        MEMORY_BASIC_INFORMATION mbi;
        int now = 0; uint32_t w0 = 0;
        if (VirtualQuery(vm_base + ea, &mbi, sizeof mbi) && mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
            now = 1; w0 = peek32(ea);
        }
        if (now != was || (now && w0 != last0)) {
            if (now) fprintf(stderr, "[gt6-watch] 0x%08X committed: %08X %08X %08X %08X\n", ea, peek32(ea), peek32(ea + 4), peek32(ea + 8), peek32(ea + 12));
            if (now && s_watch_traps_left > 0) {           /* the next access to it reports itself (gt6_watch_trap) */
                DWORD oldp;
                if (VirtualProtect(vm_base + ea, 0x10000, PAGE_NOACCESS, &oldp)) { s_watch_trap_ea = ea; InterlockedExchange(&s_watch_trap_armed, 1); }
            }
            else     fprintf(stderr, "[gt6-watch] 0x%08X not committed\n", ea);
            was = now; last0 = w0;
        }
        Sleep(1);
    }
}
}   // namespace

/* GT6_POKE=<hex addr>:<hex value>@<seconds>   (diagnostic): writes one big-endian word into guest memory that many
 * seconds after start -- to test what a waiter is waiting for before looking for who should have written it. */
static DWORD WINAPI poke_thread(void* arg)
{
    const char* e = (const char*)arg;
    const uint32_t ea = (uint32_t)strtoul(e, nullptr, 16);
    const char* c = strchr(e, ':'); const char* at = strchr(e, '@');
    const uint32_t val = c ? (uint32_t)strtoul(c + 1, nullptr, 16) : 0;
    Sleep((DWORD)((at ? atoi(at + 1) : 10) * 1000));
    uint8_t* p = vm_base + ea;
    fprintf(stderr, "[gt6-poke] 0x%08X: %02X%02X%02X%02X -> %08X\n", ea, p[0], p[1], p[2], p[3], val);
    p[0] = (uint8_t)(val >> 24); p[1] = (uint8_t)(val >> 16); p[2] = (uint8_t)(val >> 8); p[3] = (uint8_t)val;
    return 0;
}

/* The access-violation handler asks here first: an access to the page GT6_WATCH_COMMIT has trapped is reported
 * (host thread, guest thread and its call chain if the thread runs guest PPU code) and then let through. */
extern "C" PPU_THREAD_LOCAL ppu_context* g_active_ctx;
extern "C" int gt6_watch_trap(EXCEPTION_POINTERS* ep, uint32_t guest_addr)
{
    if (!s_watch_trap_armed || (guest_addr & ~0xFFFFu) != s_watch_trap_ea) return 0;
    if (InterlockedExchange(&s_watch_trap_armed, 0) == 0) { Sleep(0); return 1; }      /* another thread is reporting */
    InterlockedDecrement(&s_watch_traps_left);
    DWORD oldp;
    VirtualProtect(vm_base + s_watch_trap_ea, 0x10000, PAGE_READWRITE, &oldp);
    const ppu_context* ctx = g_active_ctx;
    const uintptr_t mod = (uintptr_t)GetModuleHandleA(NULL);
    char b[1200];
    int p = snprintf(b, sizeof b, "[gt6-watch] %s 0x%08X by host thread %lu (exe+0x%llX)", ep->ExceptionRecord->ExceptionInformation[0] == 1 ? "WRITE to" : "READ of",
                     guest_addr, GetCurrentThreadId(), (unsigned long long)((uintptr_t)ep->ExceptionRecord->ExceptionAddress - mod));
    if (ctx) {
        p += snprintf(b + p, sizeof b - p, " guest tid=%u lr=0x%08X chain:", (unsigned)ctx->thread_id, (uint32_t)ctx->lr);
        uint32_t frame = (uint32_t)ctx->gpr[1];
        for (int d = 0; d < 40 && p < 1150; d++) {
            if (!peek_ok(frame, 8)) break;
            const uint32_t up = peek32(frame + 4);
            if (up <= frame || up - frame > 0x100000u || !peek_ok(up + 0x10, 8)) break;
            const uint32_t lr = peek32(up + 0x14);
            if (lr >= 0x10200 && lr < 0x10C2000) p += snprintf(b + p, sizeof b - p, " %08X", lr);
            frame = up;
        }
    } else p += snprintf(b + p, sizeof b - p, " (no guest PPU context on this thread)");
    fprintf(stderr, "%s\n", b);
    return 1;
}

/* ---------------------------------------------------------------------------
 * GT6_PATCH_<NAME>=1: game patches of an RPCS3 launcher as switches of the native port (2026-11-16: the patches
 * that launcher's RPCS3 uses, put to use in the native port as far as they go). tools\rpcs3_patches.py reads
 * the launcher's patch file and writes gt6_patches.inc (the guards, their names, the data writes) and
 * out\EMAIN.insn_patches.json; the lifter turns each replaced instruction into `if (guard) new else original`
 * (ppu_lifter --insn-patches), so every patch is off until its variable is set and nothing changes for the others.
 * GT6_PATCHES=list prints what there is. Data-only patches are written into guest memory here, once.
 * -------------------------------------------------------------------------*/
#include "gt6_patches.inc"
extern "C" volatile int g_gt6_unlock_fps;
static void gt6_patches_init(void)
{
    const char* list = getenv("GT6_PATCHES");
    for (const auto& p : k_gt6_patches) {
        const char* e = getenv(p.env);
        if (p.guard == &g_gt6p_unlock_fps) {
            if (e && _stricmp(e, "legacy") == 0) {
                *p.guard = 1;
                fprintf(stderr, "[gt6-patch] %s=legacy  \"%s\" (%s, legacy root+0x1C=0)\n", p.env, p.title, p.author);
            } else if (e && *e == '1') {
                *p.guard = 0;
                g_gt6_unlock_fps = 1;
                fprintf(stderr, "[gt6-patch] %s=1 -> clean VBlank target unlock (root+0x1C intact)\n", p.env);
            } else {
                *p.guard = 0;
                if (list && !strcmp(list, "list"))
                    fprintf(stderr, "[gt6-patch] %s=0  \"%s\" (%s)\n", p.env, p.title, p.author);
            }
            continue;
        }
        *p.guard = (e && *e == '1') ? 1 : 0;
        if (*p.guard || (list && !strcmp(list, "list")))
            fprintf(stderr, "[gt6-patch] %s=%d  \"%s\" (%s)\n", p.env, *p.guard, p.title, p.author);
    }
    for (const auto& d : k_gt6_patch_data) {
        if (!d.guard || !*d.guard) continue;
        for (unsigned k = 0; k < d.len; k++) vm_write8(d.addr + k, d.bytes[k]);
        fprintf(stderr, "[gt6-patch] %u bytes written at 0x%08X\n", d.len, d.addr);
    }
}

/* ---------------------------------------------------------------------------
 * GT6_SPURS_SPUS=<5..8>: SPU threads of the game's own SPURS instance (5 = the game as it is, the default).
 * In a twelve-car pack the frame waits for SPU jobs, not for drawing: the eight job lists of that instance ask for
 * seventeen SPUs on average and five serve them, each busy 84% of the frame (s3, 2026-11-18). The game fixes the
 * count in two instructions - its boot code asks for five (0x008B07B0) and the helper it calls clamps the request
 * to five (0x00AF2C2C) - and adds every job list through one function (func_00B31A00, the only caller of
 * cellSpursAddWorkloadWithAttribute) with a per-SPU priority table and a maximum contention made for five.
 * tools\post_lift.py makes those three places ask here. With the switch unset the first two get 5 and the third
 * returns at once, so the game runs as before. With 6..8 a job list that all five SPUs served (or SPU 1..4, the
 * one list that keeps SPU 0 free) is also given to the added SPUs, at the priority it has on SPU 4, and may use
 * that many more at once; a list bound to one SPU stays as it is. SPURS itself has room for eight (priority[8],
 * spus[8]); the PS3 has six, so seven and eight are counts the game could never meet on a console.
 * -------------------------------------------------------------------------*/
int gt6_spurs_spus(void)
{
    static const int n = []() {
        const char* e = getenv("GT6_SPURS_SPUS");
        int v = (e && *e) ? atoi(e) : 5;
        v = v < 5 ? 5 : v > 8 ? 8 : v;
        if (v != 5) fprintf(stderr, "[gt6-spurs] GT6_SPURS_SPUS=%d: the game's SPURS instance gets %d SPU threads (the game asks for 5)\n", v, v);
        return v;
    }();
    return n;
}

/* GT6_SPURS_BG_SLOTS=<1..7> [GT6 0118] (a TEST switch; unset or 0 = the game's table): the background job list (header
 * 0x0170F180, priority 7, job module 44ed3fe39dae) keeps only its last <n> SPU slots that have a thread, so the other
 * SPUs never start one of its jobs and are free for the render jobs at once (SPURS does not take an SPU back in the
 * middle of a job). Its maximum contention becomes the smaller of the game's and <n>; below its minimum the game's
 * table stays. 60 FPS work order 5.1, Red Bull Ring, 16 cars, the 10 s after the start, <n> = 3: 37.9 -> 39.7 fps,
 * P99 66 -> 54 ms (fp3 -> fp4); lap 2 the same.
 * NOT A DEFAULT: with 3 the loading of a Free Run at Brands Hatch stood still for good in one run out of two (e10,
 * 2026-11-25; no frame from then on) with the render thread polling at 0x008EB514 for a job of this very list - the
 * game itself waits on these jobs while it loads. Why the job never came was not looked into. */
static int gt6_spurs_bg_slots(void)
{
    static const int v = []() {
        const char* e = getenv("GT6_SPURS_BG_SLOTS");
        int k = (e && *e) ? atoi(e) : 0;
        k = k < 0 ? 0 : k > 7 ? 7 : k;
        if (k) fprintf(stderr, "[gt6-spurs] GT6_SPURS_BG_SLOTS=%d: the background job list keeps %d SPU slot(s)\n", k, k);
        return k;
    }();
    return v;
}

/* After _cellSpursWorkloadAttributeInitialize in func_00B31A00: r29 = the CellSpursWorkloadAttribute on the caller's
 * stack (priority[8] at +0x18, minContention at +0x20, maxContention at +0x24, all big-endian), r3 = its result. */
void gt6_spurs_joblist(ppu_context* ctx)
{
    const int n = gt6_spurs_spus(), bgSlots = gt6_spurs_bg_slots();
    if (n <= 5 && !bgSlots) return;
    const uint32_t attr = (uint32_t)ctx->gpr[29];
    if ((int32_t)ctx->gpr[3] != 0 || !peek_ok(attr, 0x30) || peek32(attr) != 1) return;
    const bool bgList = peek32(attr + 0x14) == 0x0170F180u;
    if (n <= 5 && !bgList) return;
    uint8_t* const a = vm_base + attr;
    uint8_t* const t = a + 0x18;
    uint8_t before[8];
    memcpy(before, t, 8);
    const uint32_t maxc = peek32(attr + 0x24);
    uint32_t now = maxc;
    /* The list whose job list header is the static one at 0x0170F180 (priority 7, the lowest; its table names six
     * SPUs in the game itself) takes every SPU it is offered: with six SPUs it had 2.6 of them on average instead
     * of 1.1, its job module (44ed3fe39dae) ran seven times as long a frame and the pack went from 44 to 31 fps
     * (w6, w6c). So the added SPUs are not offered to it, unless GT6_SPURS_BG=1 asks for the game's own table. */
    static const bool bg = []() { const char* e = getenv("GT6_SPURS_BG"); return e && *e == '1'; }();
    if (n > 5) {
        if (bgList && !bg) {
            for (int i = 5; i < 8; i++) t[i] = 0;
            if (now > 5) now = 5;
        } else if (t[1] && t[2] && t[3] && t[4]) {
            for (int i = 5; i < n; i++) if (!t[i]) t[i] = t[4];
            if (t[0] && maxc == 5) now = (uint32_t)n;
            else if (!t[0] && maxc == 4) now = (uint32_t)n - 1;
        }
    }
    if (bgList && bgSlots) {
        uint8_t keep[8] = {};
        int kept = 0;
        for (int i = n - 1; i >= 0 && kept < bgSlots; i--)
            if (t[i]) { keep[i] = t[i]; kept++; }
        const uint32_t minc = peek32(attr + 0x20), want = now < (uint32_t)kept ? now : (uint32_t)kept;
        if (kept && want && want >= minc) { memcpy(t, keep, 8); now = want; }
        else fprintf(stderr, "[gt6-spurs] background list: %d slot(s), contention %u under its minimum %u - the game's table stays\n", kept, want, minc);
    }
    a[0x24] = (uint8_t)(now >> 24); a[0x25] = (uint8_t)(now >> 16); a[0x26] = (uint8_t)(now >> 8); a[0x27] = (uint8_t)now;
    static LONG said;
    if (InterlockedIncrement(&said) <= 24)
        fprintf(stderr, "[gt6-spurs] job list: priority %u %u %u %u %u %u %u %u -> %u %u %u %u %u %u %u %u, contention %u..%u -> %u\n",
                before[0], before[1], before[2], before[3], before[4], before[5], before[6], before[7],
                t[0], t[1], t[2], t[3], t[4], t[5], t[6], t[7], peek32(attr + 0x20), maxc, now);
}

/* sys_crash_dump_set_user_log_area(u8 index, {char label[16]; u32 addr; u32 size}*) [GT6, 2026-11-18]. On the console
 * it names memory for the system's crash dump. GT6 calls it in the moments before it stops itself, with the reason
 * in that memory:
 *   func_00A02410 (the script engine: an exception nobody caught) - the exception's text (up to 4096 bytes), then
 *                 "file:line", then it writes to address 0;
 *   func_00AE7268 and its two sisters (a wait that did not end within 5 s), func_00AF0920 (2500 failed thread
 *                 starts), func_008A6... - a four-byte code, then an illegal instruction.
 * The port goes on after such a stop (the write to 0 and the illegal instruction do nothing here), usually into a
 * picture that no longer moves: both freezes in a play session of 2026-11-18 began with this call, which was an
 * unresolved import then. The reason goes to the log - text as text, anything else as words. */
extern "C" void ps3_hle_register_ctx(uint32_t nid, const char* name, void (*fn)(ppu_context*));
static void gt6_fatal_log_area(ppu_context* ctx)
{
    const uint32_t index = (uint32_t)ctx->gpr[3] & 0xFFu, entry = (uint32_t)ctx->gpr[4], sp = (uint32_t)ctx->gpr[1];
    ctx->gpr[3] = 0;   /* CELL_OK */
    static LONG said;
    if (InterlockedIncrement(&said) > 48 || !peek_ok(entry, 0x18)) return;
    char label[17] = {};
    memcpy(label, vm_base + entry, 16);
    for (char& c : label) if (c && (c < 0x20 || c > 0x7E)) c = '.';
    const uint32_t addr = peek32(entry + 0x10), size = peek32(entry + 0x14);
    /* the caller of the game's wrapper func_00AEF9D4, which keeps its LR at sp + 0xD0 (64 bit) */
    const uint32_t caller = peek_ok(sp + 0xD4, 4) ? peek32(sp + 0xD4) : 0;
    fprintf(stderr, "[gt6-fatal] the game records a reason to stop itself: area %u \"%s\" at %08X, %u bytes (from %08X, guest thread %u)\n",
            index, label, addr, size, caller, (unsigned)ctx->thread_id);
    if (!size || size > 0x4000u || !peek_ok(addr, size)) return;
    const uint8_t* p = vm_base + addr;
    uint32_t len = 0, text = 0;
    while (len < size && p[len]) { text += p[len] >= 0x20 || p[len] == '\n' || p[len] == '\r' || p[len] == '\t'; len++; }
    if (len >= 3 && text == len) {
        for (uint32_t from = 0; from < len;) {       /* line by line */
            uint32_t to = from;
            while (to < len && p[to] != '\n') to++;
            uint32_t end = to;
            while (end > from && p[end - 1] == '\r') end--;
            if (end > from) fprintf(stderr, "[gt6-fatal]   | %.*s\n", (int)(end - from), (const char*)p + from);
            from = to + 1;
        }
    } else {
        char hex[3 * 64 + 1] = {};
        const uint32_t n = size < 64 ? size : 64;
        for (uint32_t i = 0; i < n; i++) snprintf(hex + 3 * i, 4, "%02X ", p[i]);
        fprintf(stderr, "[gt6-fatal]   bytes: %s\n", hex);
    }
    fflush(stderr);
}

extern "C" void gt6_install(void)
{
    install_traces();
    gt6_patches_init();
    ps3_hle_register_ctx(0xB20F87B3u, "sys_crash_dump_set_user_log_area", gt6_fatal_log_area);
    if (const char* e = getenv("GT6_DIAG_SKIP_DOF")) if (*e) {
        g_gt6_diag_skip_dof = *e == '1' ? 1 : 0;
        InterlockedExchange(&s_dof_diag_enabled, 1);
        fprintf(stderr, "[gt6-dof-diag] GT6_DIAG_SKIP_DOF=%d (whole func_008C2970 command)\n", g_gt6_diag_skip_dof);
    }
    if (const char* t = getenv("GT6_THREADS")) if (*t) CloseHandle(CreateThread(nullptr, 0, threads_thread, _strdup(t), 0, nullptr));
    if (const char* p = getenv("GT6_PEEK")) if (*p) CloseHandle(CreateThread(nullptr, 0, peek_thread, _strdup(p), 0, nullptr));
    if (const char* w = getenv("GT6_WATCH_COMMIT")) if (*w) CloseHandle(CreateThread(nullptr, 0, watch_commit_thread, _strdup(w), 0, nullptr));
    if (const char* k = getenv("GT6_POKE")) if (*k) CloseHandle(CreateThread(nullptr, 0, poke_thread, _strdup(k), 0, nullptr));
    /* SPU_ATOM_RING: the RSX walker's park and label-release events go into the same history as the SPU atomics. */
    if (const char* r = getenv("SPU_ATOM_RING")) if (*r) g_gcm_trace_note = spu_atom_ring_note;
    /* GT6's SPU post-process writes the finished 3D picture into the display buffer in RSX memory; the renderer
     * copies it into the GPU surface before the menu is drawn over it. GT6_SPU_PICTURE=0 turns this off. */
    { const char* p = getenv("GT6_SPU_PICTURE");
      if (!(p && *p == '0')) g_gcm_guest_write_note = rsx_live_draw_note_guest_write; }
    /* LD_PICTURE_LOG: label releases and the walker's stops go into the renderer's picture time line. */
    if (const char* p = getenv("LD_PICTURE_LOG")) if (*p) g_gcm_trace_note = rsx_live_draw_picture_log;
}

static uint64_t gt6_audio_mix_sequence()
{
    if (peek_ok(0x0163E270u, 4)) {
        const uint32_t state = peek32(0x0163E270u);
        if (state >= 0x10000u && state <= UINT32_MAX-0x30 && peek_ok(state + 0x28, 8))
            return ((uint64_t)peek32(state + 0x28) << 32) | peek32(state + 0x2C);
    }
    return UINT64_MAX;
}

/* One synchronous SGX submission on this guest PPU thread. The HLE cookie
 * binds the submission to a port lifetime; a late completion after stop or
 * reuse cannot publish into a new queue. Guest registers are never changed. */
struct Gt6AudioPending {
    ppu_context* context;
    uint64_t cookie;
    uint32_t base, destination, slot;
};
static thread_local Gt6AudioPending s_gt6_audio_pending{};

void gt6_audio_output_begin(ppu_context* ctx)
{
    static const bool enabled = []() {
        const char* e = getenv("GT6_AUDIO_FIFO");
        return !(e && *e && atoi(e) == 0);
    }();
    s_gt6_audio_pending = {};
    if (!ctx) return;
    const uint32_t slot = (uint32_t)ctx->gpr[24];
    const uint32_t base = (uint32_t)ctx->gpr[19];
    const uint64_t destination = base + (uint64_t)slot * 8192u;
    if (slot >= 16 || destination > UINT32_MAX || ctx->gpr[3] != destination) return;
    if (__atomic_load_n(&g_cell_audio_flow_trace_on, __ATOMIC_ACQUIRE))
        ps3_audio_trace_producer_begin((uint32_t)destination, slot, gt6_audio_mix_sequence(), (uint32_t)ctx->thread_id);
    if (!enabled) return;
    const uint64_t cookie = cellAudio_sgx_fifo_begin(base, (uint32_t)destination, slot);
    if (cookie) s_gt6_audio_pending = {ctx, cookie, base, (uint32_t)destination, slot};
}

void gt6_audio_output_complete(ppu_context* ctx)
{
    const Gt6AudioPending pending = s_gt6_audio_pending;
    s_gt6_audio_pending = {};
    if (!pending.cookie || pending.context != ctx || !ctx ||
        (uint32_t)ctx->gpr[19] != pending.base || (uint32_t)ctx->gpr[24] != pending.slot) return;
    /* CB59E8 can legitimately clear a silent block without advancing the DSP
     * sequence. The opaque submission cookie orders those completions too. */
    cellAudio_sgx_fifo_complete(pending.cookie, pending.base, pending.destination,
                               pending.slot, gt6_audio_mix_sequence());
}

/* Post-CB87D8 and DRAIN: r19 is the ring base, r24 the completed 8192-byte
 * (256 x 8 x f32) slot. This observes completion, not the earlier DMA commit. */
void gt6_audio_trace_completed(ppu_context* ctx)
{
    if (!ctx || !__atomic_load_n(&g_cell_audio_flow_trace_on, __ATOMIC_ACQUIRE)) return;
    const uint32_t slot = (uint32_t)ctx->gpr[24];
    const uint64_t destination = (uint32_t)ctx->gpr[19] + (uint64_t)slot * 8192u;
    if (slot >= 16 || destination > UINT32_MAX) return;
    cellAudio_trace_producer((uint32_t)destination, slot, gt6_audio_mix_sequence(), (uint32_t)ctx->thread_id);
}

/* Experimental render pacing stays independent of the guest's time sources. */
#include "gt6_frame_timing.h"
