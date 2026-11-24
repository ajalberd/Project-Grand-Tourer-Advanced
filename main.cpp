/*
 * GT6 native port -- host runner (ps3recomp project).
 *
 * Boots the lifted EMAIN (Gran Turismo 6, BCES01893 1.22) through the ps3recomp
 * PPU scaffold: flat guest VM, ELF load, lifted function table, HLE handlers,
 * frame clock, entry dispatch.
 *
 * The host parts are the generic ones of the Sakatsuku port's main.cpp
 * (PS3Recomp_StarterKit\host\MAIN_CPP_MAP.md, first table): write-watch guest
 * memory, live-draw init, present on flip, vblank on its own timer thread,
 * frame clock, freeze watchdog. Game-specific code lives in stubs.cpp and gt6_*.
 *
 *   GT6Recomp.exe <EMAIN.ELF>
 *   PS3_VFS_ROOT   host dir that holds PS3_GAME (the disc)
 *   PS3_HDD0_ROOT  host dir that /dev_hdd0 maps to
 *   PS3_DEV_FLASH  host dir that /dev_flash maps to
 *   RSX_LIVE_DRAW=1  live NV4097 -> D3D12 renderer (the working path)
 */

#include "ppu_recomp.h"
#include "gt6_trainer.h"
extern "C" void gt6_menu_install(void);

#include "win32_compat.h"
#include "ps3recomp/include/ps3emu/ps3_ftrace.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <timeapi.h>
#endif

extern "C" {
uint32_t ppu_load_elf(const char* path);      /* ELF -> guest RAM, returns entry OPD */
void     ppu_recomp_register(void);           /* generated: lifted table -> address map */
void     ppu_hle_init(void);                  /* firmware import NID -> HLE handler */
void     ppu_sysprx_register(void);           /* boot-critical CRT */
void     ppu_fs_register(void);               /* cellFs over the real game directory */
void     lv2_init_syscalls(void);             /* the lv2 syscall table */
void     rsx_live_draw_set_present_nowait(int on);
void     rsx_live_draw_set_video_active(int active);
int      ps3_vdec_has_open_decoder(void);
unsigned long long ps3_qpc_us(void);
void     rsx_d3d12_set_present_nowait(int on);
unsigned rsx_d3d12_last_frame_draws(void);
void     ppu_report_guest_lrs(void);          /* every guest thread's lr/r3/sp */
extern "C++" void ps3_sampler_start(void);    /* PS3_SAMPLE profiler; C++ linkage in ppu_loader.cpp */
int      ppu_run(uint32_t entry_opd, uint32_t stack_top);

void     ps3_load_prx_modules(void);          /* stubs.cpp */
void     gt6_install(void);                   /* gt6_host.cpp: syscalls and hooks GT6 needs */

extern const char* ppu_vfs_root;
void     ps3_host_quit(const char* why);

extern uint32_t g_ppu_main_stack_addr;
extern uint32_t g_ppu_main_stack_size;
extern int     g_sys_process_exit_called;
extern int32_t g_sys_process_exit_code;

uint8_t* vm_base = nullptr;
volatile int g_gt6_user_speed = 1;            /* game speed multiplier (overlay keys, later) */
extern uint32_t ppu_vm_size;

typedef void (*ps3_guest_caller_fn)(uint32_t, uint64_t, uint64_t, uint64_t, uint64_t,
                                    uint64_t, uint64_t, uint64_t, uint64_t);
extern ps3_guest_caller_fn g_ps3_guest_caller;
uint64_t ppu_guest_call(uint32_t, uint64_t, uint64_t, uint64_t, uint64_t,
                        uint64_t, uint64_t, uint64_t, uint64_t);

void     cellGcmTickVBlank(void);
void     cellGcmTickFlip(void);
int      cellGcm_take_flip_pending(void);
void     cellGcm_rsx_process_fifo(void);
unsigned cellGcm_flip_request_count(void);
void     cellGcm_fifo_kick_wait(unsigned ms);
extern unsigned (*g_ps3_samp_frame_fn)(void); /* runtime/ppu/ppu_loader.cpp: the frame PS3_SAMPLE_MULTI stamps a sample with */
}

/* ---------------------------------------------------------------------------
 * RSX present backend
 * -----------------------------------------------------------------------*/
extern "C" int  rsx_d3d12_backend_init(uint32_t w, uint32_t h, const char* title);
extern "C" void rsx_d3d12_backend_present(void);
extern "C" int  rsx_d3d12_backend_pump_messages(void);

extern "C" int      rsx_live_draw_enabled(void);
extern "C" int      rsx_live_draw_init(void* hwnd, uint32_t w, uint32_t h,
                                       const uint8_t* (*guest_ptr)(void*, uint32_t, uint32_t, uint32_t),
                                       void* user);
extern "C" void     rsx_live_draw_present(uint32_t buffer_id);
extern "C" uint32_t rsx_live_draw_get_last_draws(void);
extern "C" unsigned long long rsx_live_draw_fence_wait_us(unsigned* waits);
extern "C" int      rsx_null_backend_init(uint32_t w, uint32_t h, const char* title);
extern "C" int      rsx_null_backend_pump_messages(void);
extern "C" void*    rsx_null_backend_get_hwnd(void);
extern "C" void     rsx_null_backend_suppress_present(int on);
extern "C" uint32_t cellGcmResolveLocated(int local, uint32_t offset);
extern "C" uint32_t cellGcmResolveIO(uint32_t offset);
extern "C" unsigned cellGcmGetCurrentDisplayBufferId(void);
static int s_rsx_live = 0;   /* live engine selected AND up */

/* (location, offset) -> host memory for the live engine: location 0 is RSX local
 * VRAM, 1 main memory through the IO table. */
static const uint8_t* rsx_live_guest_ptr(void* user, uint32_t location,
                                         uint32_t offset, uint32_t min_bytes)
{
    (void)user;
    if (!vm_base) return nullptr;
    uint32_t ea;
    if (location == 0) {
        ea = cellGcmResolveLocated(1, offset);
    } else {
        ea = cellGcmResolveIO(offset);
        if (!ea) ea = cellGcmResolveLocated(0, offset);
    }
    if (!ea || ea == 0xFFFFFFFFu || (uint64_t)ea + min_bytes > 0x100000000ull) return nullptr;
    return (const uint8_t*)vm_base + ea;
}

static unsigned last_draws(void)
{
    return s_rsx_live ? rsx_live_draw_get_last_draws() : rsx_d3d12_last_frame_draws();
}

/* ---------------------------------------------------------------------------
 * Configuration
 * -----------------------------------------------------------------------*/
#define VM_SIZE       0x100010000ull   /* the full 32-bit guest space + a 64K guard */
#define STACK_TOP     0x0FF00000u      /* main-thread stack, below the 0x10000000 segment */
#define STACK_SIZE    0x00100000u
#define WINDOW_WIDTH  1280
#define WINDOW_HEIGHT 720

/* ---------------------------------------------------------------------------
 * Frame clock
 * -----------------------------------------------------------------------*/
static volatile LONG g_frames_presented = 0;

extern "C" unsigned ppu_boot_frames_presented(void)
{
    return (unsigned)g_frames_presented;
}

static unsigned           g_present_n = 0;
static unsigned long long g_present_sum_us = 0, g_present_max_us = 0;
static unsigned long long g_kick_wait_us = 0;
/* [perf] sema=: of the waiting between FIFO passes, the part spent while the FIFO stood at a semaphore acquire - the
 * title's label 0x53, which its SPU post-process releases - as opposed to having no commands (kick=).
 * busy=: time inside the FIFO passes themselves (drawing, texture work, and the GPU waits listed as gpu=). */
static unsigned long long g_sema_wait_us = 0, g_fifo_busy_us = 0;
extern "C" int cellGcm_last_pass_blocked(void);
extern "C" void cellfs_add_path_mapping(const char* ps3_prefix, const char* host_path);   /* libs/filesystem/cellFs.c */
extern "C" void cellVideoOut_set_resolution(unsigned char resolutionId);                  /* libs/video/cellVideoOut.c */
/* GT6_RES=1080 (2026-11-20: a higher resolution, up to Full HD): the video output is reported as 1080p, so the game
 * takes its own 1080p mode - it asks cellVideoOutGetState what the console puts out and sizes its buffers by that.
 * The window opens at 1920x1080. Unset / 720 = 720p, as before.
 * Without GT6_RES the switch of the F2 menu decides (Settings -> Display -> Resolution Full HD, kept in the menu's settings file
 * and read here before the game boots). First run 2026-11-20: the game configures resolution 11 (1440x1080, pitch
 * 6144) - its own 1080p mode -, three 1440x1080 display buffers, 59.9 fps in a Free Run. */
static int g_gt6_res_1080 = 0;
extern "C" int gt6_menu_setting_full_hd(void);      /* gt6_menu_engine.cpp */
extern "C" int rsx_live_draw_present_stretch;       /* rsx_live_draw.c: the scanout is stretched over the window */
extern "C" float rsx_live_draw_present_aspect;      /* rsx_live_draw.c: custom display aspect ratio (0 = 16:9) */
extern "C" float rsx_window_aspect;                 /* rsx_null_backend.c: custom window aspect ratio (0 = 16:9) */
extern "C" float g_gt6_aspect;                      /* gt6_host.cpp: 3D projection aspect override at 0x008A615C */
extern "C" int   g_gt6_lan;                         /* gt6_host.cpp: LAN mode patches (0x00A0BDAC/BF50, 0x00B69768) */
extern "C" int   g_gt6_dev_menu;                    /* gt6_host.cpp: Develop Settings (0x0081715C) */
extern "C" void rsx_live_draw_upscale(float ux, float uy);       /* rsx_live_draw.c [GT6 0102] */
extern "C" void rsx_live_draw_upscale_size(unsigned w, unsigned h);
extern "C" int gt6_res_1080(void) { return g_gt6_res_1080; }   /* (the menu shows which mode this run is in) */
static int g_gt6_real_res = 0;                      /* [GT6 0102] 0 = off, 1 = 1920x1080, 2 = 2560x1440, 3 = 3840x2160 */
extern "C" int gt6_real_res(void) { return g_gt6_real_res; }
extern "C" int gt6_menu_setting_real_res(void);     /* gt6_menu_engine.cpp */
extern "C" void rsx_live_draw_reports_tick(void);   /* rsx_live_draw.c: ZPASS counts the title may be waiting for [0056] */
extern "C" int g_spu_force_ch_block;        /* runtime/spu/spu_channels.c: an empty SPU channel read sleeps */
static long long s_last_pending_present = 0;

/* GCM interrupts at once [0036, 2026-11-16]. The game's frame is a relay between its main thread, its SPU jobs and
 * the RSX, and two of the hand-overs are RSX interrupts: user command 1 runs a callback that releases a GPU ring the
 * SPU jobs wait for, user command 0 wakes the thread that waits to start the next display list (the game's handler
 * at 0xC4797C signals the event at 0x17E7F40). The runtime runs guest handlers on whichever guest thread reaches an
 * HLE call or a sleep, and the threads that wait in real waits pump only when nudged - which this port did on the
 * 60 Hz beat alone. Sampled over a lap (run238), the main thread stood 1.6 to 5 ms in that wait AFTER the drawing
 * thread had reached the end of the frame. Now the drawing thread nudges the waiters whenever a pass has left a
 * handler pending. GT6_GCM_NUDGE=0 = the beat alone, as before. */
extern "C" int  cellGcm_handlers_pending(void);
extern "C" void ppu_lwcond_wake_all(void);
static volatile LONG g_gt6_gcm_nudge = -1;
static unsigned long long g_gcm_nudges = 0;

/* GT6_AB=<s>:<name>=<value>[,<name>=<value>...][/<s>:...] - switches for measuring, set when the [perf] line of that
 * second prints (entries by ascending second; "/" between them, because tools\freerun.ps1 splits -Env at ";"). One
 * test drive then compares laps: names nudge (GCM interrupts at once), wd (precise clock in the SPU transfer-repeat
 * watchdog), usleep (precise sys_timer_usleep), spin (microseconds an SPU spins before it parks on a channel). */
extern "C" int g_spu_dma_repeat_precise;    /* runtime/spu/spu_channels.c */
extern "C" int g_spu_ch_spin_us;            /* runtime/spu/spu_channels.c */
extern "C" int g_sys_usleep_precise;        /* runtime/syscalls/sys_timer.c */
extern "C" int g_spu_float_rz;              /* runtime/spu/spu_channels.c: SPU code under MXCSR round toward zero */
extern "C" int g_spu_coh_unmark_ms;         /* runtime/spu/spu_coherency.c: idle time after which a line's mark goes */
extern "C" int g_ld_vreuse_stats;           /* libs/video/rsx_live_draw.c: the [vreuse] census of vertex-cache misses */
extern "C" int g_gcm_method_run;            /* libs/video/cellGcmSys.c: runs of FIFO method words taken in one call */
extern "C" int g_ld_vslab_ab;               /* libs/video/rsx_live_draw.c: vertex arrays resident on the GPU, in use or not */
extern "C" int g_ld_gpu_aa_ab;              /* ...: completed-display GPU AA, for diagnosis */
extern "C" int g_gt6p_no_dof;              /* existing guest effect guards, for diagnosis */
extern "C" int g_gt6_diag_skip_dof;         /* skip GPU DoF executor only when explicitly diagnosing */
extern "C" int g_gt6p_no_motion_blur;
extern "C" int g_ld_vslab_mb;               /* ...: their memory limit lowered to this many MB (it can only go down) */
extern "C" int g_ld_vrun_ab;                /* ...: runs of elements converted by rsx_vertex_fetch_run (1) or through a list (0) */
extern "C" int g_ld_vdir_ab;                /* ...: the direct way chosen by span against index count alone (1) */
extern "C" int g_ld_vspr_ab;                /* ...: sparse groups pulled on the GPU (1) or fetched the old way (0) */
extern "C" int g_ld_fast_ab;                /* ...: the cheaper per-draw forms of [GT6 0051] (1) or the old ones (0) */
extern "C" int g_ld_wws_ab;                 /* ...: long-clean blocks of guest memory asked about once a frame (1) or once a pass (0) */
extern "C" int g_ld_wwsub_ab;               /* ...: the write watch harvested by 256 KB sub-block [GT6 0054] (1) or by whole 4 MB block (0) */
extern "C" int g_gcm_fast_blit;             /* libs/video/cellGcmSys.c: a scaled 2D blit by row (1) or by pixel (0) */
extern "C" int g_gcm_fifo_burst;            /* ...: consecutive state headers of the FIFO taken in one call [GT6 0055] (1) or one by one (0) */
extern "C" int g_gcm_wait_spin_us;          /* ...: the walker looks for new work before its timed wait and spins this long [GT6 0056]; 0 = the timed wait alone */
extern "C" int g_rsx_vp_gen_on_change;      /* libs/video/rsx_dispatch.c: an unchanged vertex program word leaves the program's memo valid (1) */
/* spu_jobs\jobmod_9039b3cc346b_e0030 (tools\optimize_pdi_leaves.py): six leaves of the car vertex job on their fast
 * paths (1) or on the lifted bodies (0); -1 = not read yet (GT6_NATIVE_PDI_LEAVES=1 and not GT6_PDI_LEAVES2=0).
 * selectany on both sides, so the port links whether or not the tool was applied. */
extern "C" { __declspec(selectany) int g_gt6_pdi_leaves2 = -1; }

/* The end-of-frame wait. Gran Turismo 6 ends every frame's RSX list with two user commands - 1: run the next callback
 * of the frame's list (the flip, or the release of a command ring), 0: wake the render thread - and its render thread
 * waits for that wake on the event at 0x017E7F40 (mutex id at +0, condition variable id at +4) for at most five
 * seconds (FUN_00c451f8 and FUN_00c44670, both with 5000000). On the console the RSX never takes five seconds. Here a
 * frame can: a first visit to a track built pipelines for ten seconds inside one frame. The wait then timed out,
 * FUN_00c451f8 dropped the frame's pending object (DAT_017e7df4 = 0) and the game went on; when the renderer finally
 * reached that frame's user command 1, the handler (func_00C4797C) popped a callback from whatever object was
 * pending by then - the next frame's, or none (the NULL reads at 0x00C4797C) - so a ring release or a flip ran for
 * the wrong frame and the one it belonged to never came: the main thread then waited for ring space for ever
 * (b9_as_on: no FIFO kick for the rest of the run). The wait is therefore given ten minutes: a stall of the host
 * renderer is not the dead RSX the game's five seconds are meant to catch. GT6_FRAME_WAIT=0 keeps the five. */
extern "C" uint64_t (*g_sys_cond_timeout_hook)(uint32_t id, uint32_t id_address, uint64_t timeout_us);
static uint64_t gt6_frame_wait_timeout(uint32_t id, uint32_t id_address, uint64_t timeout_us)
{
    if (timeout_us != 5000000ull) return timeout_us;
    uint32_t at_event = 0;
    if (vm_base) { memcpy(&at_event, vm_base + 0x017E7F44u, 4); at_event = _byteswap_ulong(at_event); }
    if (id_address != 0x017E7F44u && !(id && id == at_event)) return timeout_us;
    static LONG said;
    if (InterlockedIncrement(&said) == 1)
        fprintf(stderr, "[gt6] end-of-frame wait: the game's 5 s limit is 600 s here (condition variable %u; "
                        "GT6_FRAME_WAIT=0 keeps 5 s)\n", id);
    return 600000000ull;
}
/* GT6_SPURS_DUMP=<from>,<to>[,<hex ea>] (diagnostic, reads guest memory only): how the game's SPURS instance (the
 * PDI one, CellSpurs at 0x80100000 = r4 of its SPU threads) hands its workloads to the SPUs. From <from> to <to>
 * seconds after start the instance's first lines are read once a millisecond; every ten seconds one line a
 * workload says: the per-SPU priority table and min/max contention it was added with, how many SPUs were on it
 * (average, and how often each count), and how many it asked for (ready count). That is what decides whether more
 * SPU threads than the game's five could shorten the frame: a job chain capped at five SPUs, or a table with no
 * priority for SPU 5..7, would not use them. Offsets: RPCS3's CellSpurs (cellSpurs.h). */
static bool gt6_guest_readable(uint32_t ea, uint32_t n)
{
    MEMORY_BASIC_INFORMATION mi;
    if (!vm_base || !VirtualQuery(vm_base + ea, &mi, sizeof mi)) return false;
    return mi.State == MEM_COMMIT && !(mi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) &&
           (const uint8_t*)mi.BaseAddress + mi.RegionSize >= vm_base + ea + n;
}
static DWORD WINAPI spurs_dump_thread(LPVOID)
{
    int from = 80, to = 130; unsigned ea = 0x80100000u;
    sscanf(getenv("GT6_SPURS_DUMP"), "%d,%d,%x", &from, &to, &ea);
    Sleep((DWORD)from * 1000u);
    if (!gt6_guest_readable(ea, 0x1000)) { fprintf(stderr, "[spurs-dump] 0x%08X is not readable\n", ea); return 0; }
    const uint8_t* s = vm_base + ea;
    auto be32 = [](const uint8_t* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; };
    const bool two = (s[0x74] & 0x40) != 0 && gt6_guest_readable(ea + 0x1000, 0x1000);   /* SF1_32_WORKLOADS */
    fprintf(stderr, "[spurs-dump] ea=0x%08X nSpus=%u flags1=0x%02X (%d workloads) wklEnabled=0x%08X spus:", ea, s[0x76], s[0x74],
            two ? 32 : 16, be32(s + 0xB0));
    for (int i = 0; i < 8; i++) fprintf(stderr, " %X", be32(s + 0xD34 + i * 4));
    fprintf(stderr, "\n");
    struct Acc { unsigned long long cur, ready, idle, hist[9]; unsigned maxready; } acc[32];
    for (int t = from; t < to; t += 10) {
        memset(acc, 0, sizeof acc);
        unsigned n = 0;
        for (const ULONGLONG until = GetTickCount64() + 10000; GetTickCount64() < until; n++) {
            for (int w = 0; w < (two ? 32 : 16); w++) {
                const int i = w & 15, hi = w >> 4;
                const unsigned cur = hi ? s[0x20 + i] >> 4 : s[0x20 + i] & 15;
                const unsigned ready = hi ? s[0x10 + i] : s[0x00 + i];
                acc[w].cur += cur; acc[w].hist[cur > 8 ? 8 : cur]++; acc[w].ready += ready;
                if (ready > acc[w].maxready) acc[w].maxready = ready;
                if (!hi && !two) acc[w].idle += s[0x10 + i];
            }
            Sleep(1);
        }
        fprintf(stderr, "[spurs-dump] t=%d..%d s, %u samples\n", t, t + 10, n);
        for (int w = 0; w < (two ? 32 : 16) && n; w++) {
            const int i = w & 15, hi = w >> 4;
            const unsigned state = s[(hi ? 0xD0 : 0x80) + i];
            if (!state) continue;
            const uint8_t* info = s + (hi ? 0x1000 : 0xB00) + i * 0x20;
            const unsigned mn = hi ? s[0x40 + i] >> 4 : s[0x40 + i] & 15, mx = hi ? s[0x50 + i] >> 4 : s[0x50 + i] & 15;
            char name[40] = "";
            const uint32_t np = be32(s + (hi ? 0x1E00 : 0xE00) + i * 0x10 + 4);
            if (np && gt6_guest_readable(np, 32)) { memcpy(name, vm_base + np, 31); for (char* c = name; *c; c++) if (*c < 32 || *c > 126) { *c = 0; break; } }
            fprintf(stderr, "[spurs-dump]  wkl %2d st=%u prio=%u%u%u%u%u%u%u%u min=%u max=%u | on it: avg %.2f SPUs, share by count 0..8:", w, state,
                    info[0x18], info[0x19], info[0x1A], info[0x1B], info[0x1C], info[0x1D], info[0x1E], info[0x1F], mn, mx, (double)acc[w].cur / n);
            for (int k = 0; k <= 8; k++) fprintf(stderr, " %.0f", 100.0 * acc[w].hist[k] / n);
            fprintf(stderr, " | ready avg %.2f max %u | pm=0x%08X size=0x%X arg=0x%08X %s\n", (double)acc[w].ready / n, acc[w].maxready,
                    be32(info + 4), be32(info + 0x10), be32(info + 0xC), name);
        }
    }
    return 0;
}
static void gt6_ab_tick(int sec)
{
    static const char* p = (const char*)-1; static int next = -1;
    if (p == (const char*)-1) { p = getenv("GT6_AB"); if (p && !*p) p = NULL; }
    while (p) {
        if (next < 0) next = atoi(p);
        if (sec < next) return;
        const char* q = strchr(p, ':');
        const char* end = strchr(p, '/');
        if (q && (!end || q < end)) {
            for (q++; q && *q && *q != '/'; ) {
                char name[16] = ""; int v = 0, n = 0;
                if (sscanf(q, "%15[a-z]=%d%n", name, &v, &n) < 2) break;
                if (!strcmp(name, "nudge")) InterlockedExchange(&g_gt6_gcm_nudge, v);
                else if (!strcmp(name, "wd")) g_spu_dma_repeat_precise = v;
                else if (!strcmp(name, "usleep")) g_sys_usleep_precise = v;
                else if (!strcmp(name, "spin")) g_spu_ch_spin_us = v;
                else if (!strcmp(name, "rz")) g_spu_float_rz = v ? 1 : 0;
                else if (!strcmp(name, "unmark")) g_spu_coh_unmark_ms = v < 0 ? 0 : v;
                else if (!strcmp(name, "vreuse")) g_ld_vreuse_stats = v ? 1 : 0;
                else if (!strcmp(name, "frun")) g_gcm_method_run = v ? 1 : 0;
                else if (!strcmp(name, "vslab")) g_ld_vslab_ab = v ? 1 : 0;
                else if (!strcmp(name, "gpuab")) g_ld_gpu_aa_ab = v ? 1 : 0;
                else if (!strcmp(name, "nodof")) g_gt6p_no_dof = v ? 1 : 0;
                else if (!strcmp(name, "skipdof")) g_gt6_diag_skip_dof = v ? 1 : 0;
                else if (!strcmp(name, "noblur")) g_gt6p_no_motion_blur = v ? 1 : 0;
                else if (!strcmp(name, "vsmb")) g_ld_vslab_mb = v;
                else if (!strcmp(name, "vrun")) g_ld_vrun_ab = v ? 1 : 0;
                else if (!strcmp(name, "vdir")) g_ld_vdir_ab = v ? 1 : 0;
                else if (!strcmp(name, "vspr")) g_ld_vspr_ab = v ? 1 : 0;
                else if (!strcmp(name, "fast")) { g_ld_fast_ab = v ? 1 : 0; g_gcm_fast_blit = v ? 1 : 0; }
                else if (!strcmp(name, "wws")) g_ld_wws_ab = v ? 1 : 0;
                else if (!strcmp(name, "wwsub")) g_ld_wwsub_ab = v < 0 ? 0 : v > 3 ? 3 : v;
                else if (!strcmp(name, "vleaves")) g_gt6_pdi_leaves2 = v ? 1 : 0;
                else if (!strcmp(name, "burst")) g_gcm_fifo_burst = v ? 1 : 0;
                else if (!strcmp(name, "spin")) g_gcm_wait_spin_us = v < 0 ? 0 : v > 20000 ? 20000 : v;
                else if (!strcmp(name, "vpgen")) g_rsx_vp_gen_on_change = v ? 1 : 0;
                fprintf(stderr, "[gt6] GT6_AB at %d s: %s=%d\n", sec, name, v);
                if (PS3_FT_ON()) {              /* a mark in a PS3_FTRACE timeline (kind 99, a = the value): tools\ftrace_walker.py */
                    const uint64_t t = ps3_ft_now();
                    ps3_ft_span(99, 0, (uint32_t)v, 0, t, t);
                }
                q += n;
                if (*q == ',') q++; else break;
            }
        }
        p = end ? end + 1 : NULL;
        next = -1;
    }
}

static void present_guest_frame(void)
{
    if (s_rsx_live) {
        const unsigned long long p0 = ps3_qpc_us();
        rsx_live_draw_present(cellGcmGetCurrentDisplayBufferId());
        const unsigned long long pd = ps3_qpc_us() - p0;
        g_present_n++; g_present_sum_us += pd; if (pd > g_present_max_us) g_present_max_us = pd;
        static unsigned slow_presents = 0;
        if (pd > 80000ull && ++slow_presents <= 200)
            fprintf(stderr, "[slow] present %.0f ms (draws %u, t=%.3f)\n", pd / 1000.0,
                    rsx_live_draw_get_last_draws(), ps3_qpc_us() / 1e6);
    } else {
        rsx_d3d12_backend_present();
    }
    InterlockedIncrement(&g_frames_presented);
}

/* [freeze] watchdog: no guest frame for 1.2 s (after the first one) prints where
 * every guest thread is, and whether the frame clock itself still turns. */
static volatile LONG g_frame_clock_beats = 0;
static DWORD WINAPI freeze_watchdog(LPVOID)
{
    unsigned flips = 0; LONG beats = 0;
    ULONGLONG since = GetTickCount64(), next = 0, beat_t = since;
    int reports = 0;
    for (;;) {
        Sleep(200);
        const ULONGLONG now = GetTickCount64();
        const LONG b = g_frame_clock_beats;
        if (b != beats) { beats = b; beat_t = now; }
        const unsigned fc = cellGcm_flip_request_count();
        if (fc != flips) {
            if (next) fprintf(stderr, "[freeze] over after %.1f s\n", (now - since) / 1000.0);
            flips = fc; since = now; next = 0;
            continue;
        }
        if (fc && now - since > 1200 && (!next || now >= next) && reports < 40) {
            fprintf(stderr, "[freeze] no frame for %.1f s (t=%.3f, last draws %u, frame clock %s); guest threads:\n",
                    (now - since) / 1000.0, ps3_qpc_us() / 1e6, last_draws(),
                    now - beat_t > 1000 ? "STUCK (GPU wait / Present?)" : "running");
            ppu_report_guest_lrs();
            fflush(stderr);
            next = now + 3000; reports++;
        }
    }
    return 0;
}

/* GT6_BOOT_WATCH=<seconds>: before the first frame exists the freeze watchdog is
 * silent, so a boot that stalls says nothing. This prints every guest thread's
 * lr every n seconds until the first flip. */
static DWORD WINAPI boot_watch(LPVOID p)
{
    const unsigned secs = (unsigned)(uintptr_t)p;
    for (int i = 0; i < 20; i++) {
        Sleep(secs * 1000u);
        if (cellGcm_flip_request_count()) return 0;
        fprintf(stderr, "[bootwatch] t=%.1f no flip yet; guest threads:\n", ps3_qpc_us() / 1e6);
        ppu_report_guest_lrs();
        fflush(stderr);
    }
    return 0;
}

/* GT6_THREADS_AT=<seconds>[,<repeat seconds>]: every guest thread's lr and callers at that time, and again every
 * <repeat> seconds (twelve times at most). For a thread that spins or waits while the frames go on - the freeze
 * watchdog only speaks when the frames stop (the game's own installation of a new copy, [GT6 0089]). */
static DWORD WINAPI threads_at(LPVOID p)
{
    const char* e = (const char*)p;
    const double at = atof(e);
    const char* c = strchr(e, ',');
    const double every = c ? atof(c + 1) : 0.0;
    Sleep((DWORD)(at * 1000.0));
    for (int i = 0; i < 12; i++) {
        fprintf(stderr, "[threads] t=%.3f guest threads:\n", ps3_qpc_us() / 1e6);
        ppu_report_guest_lrs();
        fflush(stderr);
        if (every <= 0.0) break;
        Sleep((DWORD)(every * 1000.0));
    }
    return 0;
}

/* vblank on its own timer thread (a busy drain must not delay the 60 Hz beat). */
extern "C" volatile uint32_t g_step_census_frame;
extern "C" void ppu_lwcond_wake_all(void);
static volatile long long s_vbt_period = 0;
static volatile LONG s_vbt_resync = 0;
static volatile LONG s_vbt_ticks = 0;
static volatile LONG s_vbt_maxgap_us = 0;
static int s_vbt_on = 0;

static void vbt_note_beat(long long now, long long qpf)
{
    static long long last = 0;
    if (last) {
        const LONG gap = (LONG)(((now - last) * 1000000) / qpf);
        if (gap > s_vbt_maxgap_us) s_vbt_maxgap_us = gap;
    }
    last = now;
    InterlockedIncrement(&s_vbt_ticks);
}

static DWORD WINAPI vblank_thread(LPVOID)
{
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    HANDLE tmr = CreateWaitableTimerExW(NULL, NULL, 0x00000002, TIMER_ALL_ACCESS);   /* high resolution */
    if (!tmr) tmr = CreateWaitableTimerExW(NULL, NULL, 0, TIMER_ALL_ACCESS);
    LARGE_INTEGER qpf, q;
    QueryPerformanceFrequency(&qpf);
    QueryPerformanceCounter(&q);
    long long next = q.QuadPart;
    for (;;) {
        QueryPerformanceCounter(&q);
        if (InterlockedExchange(&s_vbt_resync, 0)) next = q.QuadPart;
        const long long left = next - q.QuadPart;
        if (left > 0) {
            if (tmr) {
                LARGE_INTEGER due;
                due.QuadPart = -(LONGLONG)((left * 10000000LL) / qpf.QuadPart);
                if (due.QuadPart > -1) due.QuadPart = -1;
                SetWaitableTimerEx(tmr, &due, 0, NULL, NULL, NULL, 0);
                WaitForSingleObject(tmr, 100);
            } else {
                Sleep(1);
            }
            continue;
        }
        cellGcmTickVBlank();
        cellGcmTickFlip();
        ppu_lwcond_wake_all();
        vbt_note_beat(q.QuadPart, qpf.QuadPart);
        const long long p = s_vbt_period;
        next += p > 0 ? p : qpf.QuadPart / 60;
        if (q.QuadPart - next > qpf.QuadPart / 4) next = q.QuadPart;   /* far behind: resync, no burst */
    }
    return 0;
}

static DWORD WINAPI frame_clock(LPVOID)
{
    const char* title = getenv("PS3_TITLE");
    if (!title || !*title) title = "Gran Turismo 6";

    int rsx_ok;
    const int win_w = g_gt6_res_1080 ? 1920 : WINDOW_WIDTH, win_h = g_gt6_res_1080 ? 1080 : WINDOW_HEIGHT;   /* GT6_RES */
    if (rsx_live_draw_enabled()) {
        rsx_ok = (rsx_null_backend_init(win_w, win_h, title) == 0);
        if (rsx_ok && rsx_live_draw_init(rsx_null_backend_get_hwnd(), win_w, win_h,
                                         rsx_live_guest_ptr, nullptr) == 0) {
            s_rsx_live = 1;
            rsx_null_backend_suppress_present(1);
            fprintf(stderr, "[rsx] live-draw engine up (D3D12)\n");
        } else {
            fprintf(stderr, "[rsx] live-draw init FAILED -- falling back to the legacy D3D12 backend\n");
            rsx_ok = (rsx_d3d12_backend_init(win_w, win_h, title) == 0);
        }
    } else {
        rsx_ok = (rsx_d3d12_backend_init(win_w, win_h, title) == 0);
    }
    fprintf(stderr, "[rsx] %s backend init %s\n", s_rsx_live ? "live-draw" : "D3D12 (legacy)",
            rsx_ok ? "OK -- window open" : "FAILED");

    unsigned  last_flip = 0;
    LARGE_INTEGER qpf, qnow;
    QueryPerformanceFrequency(&qpf);
    double vblank_hz = 60.0;   /* PAL disc, but the game runs its 60 Hz video mode */
    { const char* e = getenv("GT6_VBLANK_HZ"); if (e && atof(e) > 0.0) vblank_hz = atof(e); }
    const long long tick_period = (long long)((double)qpf.QuadPart / vblank_hz + 0.5);
    QueryPerformanceCounter(&qnow);
    long long next_tick = qnow.QuadPart;
    const long long clock_started = qnow.QuadPart;
    const char* block_text = getenv("GT6_SPU_CH_BLOCK_AT");
    const int spu_block_after = block_text ? atoi(block_text) : 20;
    int spu_block_enabled = 0;
    { const char* e = getenv("GT6_VBLANK_THREAD");
      if (rsx_ok && !(e && *e == '0')) {
          s_vbt_period = tick_period;
          HANDLE h = CreateThread(NULL, 256u * 1024, vblank_thread, NULL, 0, NULL);
          if (h) { s_vbt_on = 1; CloseHandle(h); }
      }
      fprintf(stderr, "[gt6] vblank %.1f Hz, beat: %s\n", vblank_hz, s_vbt_on ? "own timer thread" : "drain loop"); }

    /* A name for this thread - the one that walks the RSX FIFO and draws - so tools\threadcpu.ps1 lists it and
     * PS3_SAMPLE_NAME=gt6-draw can sample it with the stacks of its waits. */
    SetThreadDescription(GetCurrentThread(), L"gt6-draw");
    int video_active = -1;
    for (;;) {
        { const unsigned long long kw0 = ps3_qpc_us();
          const int blocked = cellGcm_last_pass_blocked();   /* 1: the FIFO stands at a label the title has to release */
          cellGcm_fifo_kick_wait(1);
          const unsigned long long dt = ps3_qpc_us() - kw0;
          if (blocked == 1) g_sema_wait_us += dt; else g_kick_wait_us += dt; }
        if (s_rsx_live) {
            const int movie_open = ps3_vdec_has_open_decoder();
            if (movie_open != video_active) {
                rsx_live_draw_set_video_active(movie_open);
                video_active = movie_open;
            }
        }
        QueryPerformanceCounter(&qnow);
        long long now = qnow.QuadPart;
        /* Idle SPU channel waits must not depend on whether performance logs
         * are enabled or on how many slow shader-loading reports occurred. */
        if (!spu_block_enabled && spu_block_after > 0 &&
            (now - clock_started) / qpf.QuadPart >= spu_block_after) {
            g_spu_force_ch_block = 1;
            spu_block_enabled = 1;
            fprintf(stderr, "[gt6] SPU channel blocking enabled after %d wall-clock seconds\n", spu_block_after);
        }

        /* Game speed (overlay keys, later): vblank that much faster, Present without vsync. */
        const int user_speed = g_gt6_user_speed > 1 ? (g_gt6_user_speed > 16 ? 16 : g_gt6_user_speed) : 1;
        { static int user_prev = 1;
          if (user_speed != user_prev) {
              user_prev = user_speed;
              if (s_rsx_live) rsx_live_draw_set_present_nowait(user_speed > 1);
              else rsx_d3d12_set_present_nowait(user_speed > 1);
              next_tick = now; InterlockedExchange(&s_vbt_resync, 1);
              fprintf(stderr, "[gt6] game speed x%d\n", user_speed);
          } }
        const long long period = tick_period / user_speed;
        s_vbt_period = period;

        InterlockedIncrement(&g_frame_clock_beats);
        g_step_census_frame = cellGcm_flip_request_count();

        /* F12 under the live engine: open GT6_CAPTURE_GATE (a file) for 0.6 s; the engine's
         * LD_SURF_DUMP_GATE / RSX_DRAW_CSV_GATE watch that file. */
        if (s_rsx_live) {
            static const char* gate = (const char*)-1;
            static ULONGLONG gate_until = 0;
            if (gate == (const char*)-1) gate = getenv("GT6_CAPTURE_GATE");
            if (gate && *gate) {
                const ULONGLONG ms = GetTickCount64();
                HWND fg = GetForegroundWindow(); DWORD fpid = 0;
                if (fg) GetWindowThreadProcessId(fg, &fpid);
                if (!gate_until && fpid == GetCurrentProcessId() && (GetAsyncKeyState(VK_F12) & 0x8000)) {
                    FILE* gf = fopen(gate, "w");
                    if (gf) { fputs("1\n", gf); fclose(gf); }
                    gate_until = ms + 600;
                } else if (gate_until && ms >= gate_until) {
                    DeleteFileA(gate);
                    gate_until = 0;
                }
            }
        }

        /* [perf] once a second. GT6_PERF_LOG=0 turns it off. */
        { static long long perf_next = 0, perf_last = 0; static unsigned perf_f0 = 0;
          static int perf_on = -1;
          if (perf_on < 0) { const char* e = getenv("GT6_PERF_LOG"); perf_on = !(e && *e == '0'); }
          if (perf_on) {
              if (!perf_next) { perf_next = now + qpf.QuadPart; perf_last = now; perf_f0 = cellGcm_flip_request_count(); }
              else if (now >= perf_next) {
                  const unsigned f = cellGcm_flip_request_count();
                  static unsigned long long fw0 = 0; static unsigned fn0 = 0;
                  unsigned fn = 0; const unsigned long long fw = s_rsx_live ? rsx_live_draw_fence_wait_us(&fn) : 0;
                  static LONG vbt_t0 = 0;
                  const LONG vbt_n = s_vbt_ticks, vbt_gap = InterlockedExchange(&s_vbt_maxgap_us, 0);
                  fprintf(stderr, "[perf] t=%.1f fps=%.2f draws=%u present=%.1f/%.1fms wait: gpu=%.0fms/%u kick=%.0fms vbl=%ld/%.1fms sema=%.0fms busy=%.0fms\n",
                          ps3_qpc_us() / 1e6, (double)(f - perf_f0) * qpf.QuadPart / (now - perf_last), last_draws(),
                          g_present_n ? g_present_sum_us / 1000.0 / g_present_n : 0.0, g_present_max_us / 1000.0,
                          (fw - fw0) / 1000.0, fn - fn0, g_kick_wait_us / 1000.0,
                          (long)(vbt_n - vbt_t0), vbt_gap / 1000.0,
                          g_sema_wait_us / 1000.0, g_fifo_busy_us / 1000.0);
                  g_sema_wait_us = 0; g_fifo_busy_us = 0;
                  /* Diagnostic A/B scheduling retains report-count semantics.
                   * Production channel blocking uses elapsed time above. */
                  { static int secs = 0;
                    ++secs;
                    gt6_ab_tick(secs);
                    { static unsigned long long n0 = 0;          /* every 5th report: how often the nudge was given */
                      if (secs % 5 == 0) { fprintf(stderr, "[gt6] gcm nudges in the last 5 s: %llu\n", g_gcm_nudges - n0); n0 = g_gcm_nudges; } } }
                  vbt_t0 = vbt_n;
                  g_present_n = 0; g_present_sum_us = 0; g_present_max_us = 0;
                  fw0 = fw; fn0 = fn; g_kick_wait_us = 0;
                  perf_f0 = f; perf_last = now; perf_next += qpf.QuadPart;
                  if (now >= perf_next) perf_next = now + qpf.QuadPart;
              }
          } }

        int fired = 0;
        while (!s_vbt_on && (long long)(now - next_tick) >= 0 && fired < 240) {
            cellGcmTickVBlank();
            cellGcmTickFlip();
            vbt_note_beat(now, qpf.QuadPart);
            if (rsx_ok && cellGcm_take_flip_pending()) {
                s_last_pending_present = now;
                present_guest_frame();
                last_flip = cellGcm_flip_request_count();
            }
            if (rsx_ok) cellGcm_rsx_process_fifo();
            next_tick += period;
            fired++;
        }
        if (fired >= 240) next_tick = now;

        if (rsx_ok) {
            if (cellGcm_take_flip_pending()) {
                s_last_pending_present = now;
                present_guest_frame();
                last_flip = cellGcm_flip_request_count();
            }
            { const unsigned long long pf0 = ps3_qpc_us();
              cellGcm_rsx_process_fifo();
              if (s_rsx_live) rsx_live_draw_reports_tick();   /* ZPASS counts the title may be waiting for [0056] */
              const unsigned long long pf1 = ps3_qpc_us();
              g_fifo_busy_us += pf1 - pf0;
              /* a handler this pass (or an earlier one) left pending: see g_gt6_gcm_nudge */
              if (g_gt6_gcm_nudge < 0) { const char* e = getenv("GT6_GCM_NUDGE"); InterlockedExchange(&g_gt6_gcm_nudge, (e && *e == '0') ? 0 : 1); }
              static unsigned long long nudge_last = 0;
              if (g_gt6_gcm_nudge > 0 && cellGcm_handlers_pending() && pf1 - nudge_last >= 100) {
                  nudge_last = pf1;
                  g_gcm_nudges++;
                  ppu_lwcond_wake_all();
              } }

            /* Window messages once a millisecond, not once a pass. This loop turns tens of thousands of times a
             * second while the game kicks its command buffer in small pieces, and PeekMessage on every turn was
             * 12.8% of this thread (win32u.dll in the sampling of run210) - the thread that limits the frame rate
             * since the SPU job manager is lifted. GT6_PUMP_EVERY_PASS=1 = as before. */
            int pump_rc = 0;
            { static unsigned long long pump_next = 0; static int every = -1;
              if (every < 0) { const char* e = getenv("GT6_PUMP_EVERY_PASS"); every = (e && *e == '1') ? 1 : 0; }
              const unsigned long long t_us = ps3_qpc_us();
              if (every || t_us >= pump_next) {
                  pump_next = t_us + 1000;
                  pump_rc = s_rsx_live ? rsx_null_backend_pump_messages() : rsx_d3d12_backend_pump_messages();
              } }
            if (pump_rc != 0) {
                rsx_ok = 0;
                ps3_host_quit("window closed");
                continue;
            }
            /* Fallback present on a flip REQUEST: only when flips do not arrive through the FIFO
             * (boot, or a flip that never reaches it). See the Sakatsuku port's notes. */
            unsigned fc = cellGcm_flip_request_count();
            const int fifo_flips_live = s_last_pending_present &&
                                        (now - s_last_pending_present) < qpf.QuadPart / 10;
            if (fc != last_flip && !fifo_flips_live) {
                present_guest_frame();
                last_flip = fc;
            } else if (fc == 0) {
                if (!s_rsx_live) rsx_d3d12_backend_present();
            }
        }
    }
    return 0;
}

/* ---------------------------------------------------------------------------
 * VFS root
 * -----------------------------------------------------------------------*/
static char s_vfs_root[1024];

static void derive_vfs_root(const char* elf)
{
    const char* env = getenv("PS3_VFS_ROOT");
    if (env && *env) { ppu_vfs_root = env; return; }
    /* <root>/EMAIN.ELF -> <root>/dev_bdvd */
    strncpy(s_vfs_root, elf, sizeof s_vfs_root - 16);
    for (char* p = s_vfs_root; *p; p++) if (*p == '\\') *p = '/';
    char* s = strrchr(s_vfs_root, '/');
    if (s) *s = 0; else strcpy(s_vfs_root, ".");
    strcat(s_vfs_root, "/dev_bdvd");
    ppu_vfs_root = s_vfs_root;
}

/* ---------------------------------------------------------------------------
 * Guest VM
 * -----------------------------------------------------------------------*/
/* gt6_host.cpp: a fault inside a region the game pages itself (sys_mmapper page
 * fault notification). Returns 1 when it handled the fault. */
extern "C" int gt6_page_fault(EXCEPTION_POINTERS* ep, uint32_t guest_addr);
extern "C" int gt6_watch_trap(EXCEPTION_POINTERS* ep, uint32_t guest_addr);   /* GT6_WATCH_COMMIT (diagnostic) */

/* Demand-paging for the flat VM: reserve the whole 4 GB guest space and commit
 * each 64 KB page on first access. */
static LONG WINAPI vm_commit_veh(EXCEPTION_POINTERS* ep)
{
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        ULONG_PTR fault = ep->ExceptionRecord->ExceptionInformation[1];
        uintptr_t base  = (uintptr_t)vm_base;
        if (vm_base && fault >= base && fault < base + VM_SIZE) {
            if (gt6_watch_trap(ep, (uint32_t)(fault - base))) return EXCEPTION_CONTINUE_EXECUTION;
            if (gt6_page_fault(ep, (uint32_t)(fault - base))) return EXCEPTION_CONTINUE_EXECUTION;
            void* page = (void*)(fault & ~(uintptr_t)0xFFFF);
            if (VirtualAlloc(page, 0x10000, MEM_COMMIT, PAGE_READWRITE))
                return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

/* A stack that ran out (0xC00000FD, a crash of 2026-11-20 20:01 on the game's main thread): a guest loop made of
 * tail calls that the lift turned into host calls, or a wait that never ends - on the console it would stand still,
 * here every turn of it costs host stack. The functions that call each other are named by the stack itself: the
 * return addresses nearest its top are printed as offsets in the executable (tools\stack_cycle.py names them from
 * the .map and shows what repeats). This runs on the thread that overflowed; the runtime gives every guest thread a
 * 256 KB guarantee for exactly this (sys_ppu_thread.c). */
static void crash_report_stack(const EXCEPTION_POINTERS* ep, uintptr_t mod)
{
    const IMAGE_NT_HEADERS* nt = (const IMAGE_NT_HEADERS*)(mod + ((const IMAGE_DOS_HEADER*)mod)->e_lfanew);
    const uintptr_t end = mod + nt->OptionalHeader.SizeOfImage;
    const NT_TIB* tib = (const NT_TIB*)NtCurrentTeb();
    const uintptr_t sp = (uintptr_t)ep->ContextRecord->Rsp & ~(uintptr_t)7, base = (uintptr_t)tib->StackBase;
    fprintf(stderr, "[crash] stack overflow: %.1f MB of stack in use (base %p, sp %p); return addresses from the top, exe+:\n",
            sp < base ? (double)(base - sp) / 1048576.0 : 0.0, (void*)base, (void*)sp);
    if (sp >= base || base - sp > ((uintptr_t)1 << 32))
        return;   /* not this thread's own stack (a fiber's): nothing to walk by these bounds */
    int n = 0;
    uintptr_t page = 0;
    for (const uintptr_t* p = (const uintptr_t*)sp; (uintptr_t)p < base && n < 384; p++) {
        if (((uintptr_t)p & ~(uintptr_t)0xFFF) != page) {   /* each page once: committed and no guard page */
            MEMORY_BASIC_INFORMATION mbi;
            page = (uintptr_t)p & ~(uintptr_t)0xFFF;
            if (!VirtualQuery((void*)page, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))) {
                p = (const uintptr_t*)(page + 0x1000) - 1;
                continue;
            }
        }
        const uintptr_t v = *p;
        if (v > mod + 0x1000 && v < end) {
            fprintf(stderr, "%s%llX", n % 16 ? " " : (n ? "\n[crash]  " : "[crash]  "), (unsigned long long)(v - mod));
            n++;
        }
    }
    fprintf(stderr, "\n");
}

/* An exception nobody handled: say where, so that a silent exit code 0xC0000005 can be found in the .map file. */
static LONG WINAPI crash_report(EXCEPTION_POINTERS* ep)
{
    const EXCEPTION_RECORD* er = ep->ExceptionRecord;
    const uintptr_t mod = (uintptr_t)GetModuleHandleA(NULL), ip = (uintptr_t)er->ExceptionAddress;
    fprintf(stderr, "[crash] code=0x%08lX at %p (exe+0x%llX) access=%llu addr=0x%llX guest=0x%llX tid=%lu\n",
            er->ExceptionCode, er->ExceptionAddress, (unsigned long long)(ip - mod),
            er->NumberParameters >= 2 ? (unsigned long long)er->ExceptionInformation[0] : 0ull,
            er->NumberParameters >= 2 ? (unsigned long long)er->ExceptionInformation[1] : 0ull,
            er->NumberParameters >= 2 ? (unsigned long long)(er->ExceptionInformation[1] - (uintptr_t)vm_base) : 0ull,
            GetCurrentThreadId());
    if (er->ExceptionCode == EXCEPTION_STACK_OVERFLOW)
        crash_report_stack(ep, mod);
    fflush(stderr); fflush(stdout);
    return EXCEPTION_CONTINUE_SEARCH;
}

static bool alloc_guest_vm(void)
{
    SetUnhandledExceptionFilter(crash_report);
    AddVectoredExceptionHandler(1, vm_commit_veh);
    /* MEM_WRITE_WATCH: the live renderer re-hashes a cached texture / vertex range only when its
     * pages were written. PS3_VM_WRITE_WATCH=0 reserves without it. */
    { const char* e = getenv("PS3_VM_WRITE_WATCH");
      if (!(e && *e == '0'))
          vm_base = (uint8_t*)VirtualAlloc(NULL, VM_SIZE, MEM_RESERVE | MEM_WRITE_WATCH, PAGE_READWRITE); }
    if (!vm_base)
        vm_base = (uint8_t*)VirtualAlloc(NULL, VM_SIZE, MEM_RESERVE, PAGE_READWRITE);
    ppu_vm_size = 0;              /* the whole space is backed; no OOB guard needed */
    return vm_base != nullptr;
}

static void harness_guest_caller(uint32_t opd, uint64_t a0, uint64_t a1,
                                 uint64_t a2, uint64_t a3, uint64_t a4,
                                 uint64_t a5, uint64_t a6, uint64_t a7)
{
    ppu_guest_call(opd, a0, a1, a2, a3, a4, a5, a6, a7);
}

/* ---------------------------------------------------------------------------
 * The port's own folders
 * -----------------------------------------------------------------------*/
/* The port is one self-contained folder; nothing outside it is read or written:
 *     <root>\build-release\GT6Recomp.exe      (or <root>\GT6Recomp.exe in a release package)
 *     <root>\game\EMAIN.ELF                   the game's main executable
 *     <root>\game\dev_bdvd  dev_hdd0  dev_hdd1  dev_flash      the console's mount points
 * Started with no argument the exe finds <root> from its own place, so a double click runs the game from any
 * working directory and with no environment. An ELF given as argument makes that file's folder the game folder
 * (the working directory is then left alone), and each PS3_* variable still overrides its own folder. */
static char s_game_elf[1024];
static char s_game_dir[1024];

/* The checkout uses game.path; the standalone package keeps its own game/. */
static bool locate_routed_game(const char* root)
{
    char route_file[1100], relative[1024];
    snprintf(route_file, sizeof route_file, "%s\\game.path", root);
    FILE* f = fopen(route_file, "rb");
    if (!f) return false;
    const bool read = fgets(relative, sizeof relative, f) != NULL;
    fclose(f);
    if (!read) return false;
    relative[strcspn(relative, "\r\n")] = 0;
    if (!*relative || relative[0] == '/' || relative[0] == '\\' || strchr(relative, ':')) return false;
    const int n = snprintf(s_game_elf, sizeof s_game_elf, "%s\\%s\\EMAIN.ELF", root, relative);
    return n > 0 && n < (int)sizeof s_game_elf && GetFileAttributesA(s_game_elf) != INVALID_FILE_ATTRIBUTES;
}

static void default_dir(const char* var, const char* sub)
{
    const char* e = getenv(var);
    if (e && *e) return;
    char p[1100];
    snprintf(p, sizeof p, "%s\\%s", s_game_dir, sub);
    _putenv_s(var, p);
}

static const char* locate_game(const char* elf_arg)
{
    if (elf_arg && *elf_arg) {
        if (!GetFullPathNameA(elf_arg, (DWORD)sizeof s_game_elf, s_game_elf, NULL))
            snprintf(s_game_elf, sizeof s_game_elf, "%s", elf_arg);
    } else {
        char root[1024];
        DWORD n = GetModuleFileNameA(NULL, root, (DWORD)sizeof root);
        if (!n || n >= sizeof root) strcpy(root, ".\\GT6Recomp.exe");
        char* s = strrchr(root, '\\');
        if (s) *s = 0;
        if (!locate_routed_game(root))
            snprintf(s_game_elf, sizeof s_game_elf, "%s\\game\\EMAIN.ELF", root);
        if (GetFileAttributesA(s_game_elf) == INVALID_FILE_ATTRIBUTES && (s = strrchr(root, '\\')) != NULL) {
            *s = 0;                                    /* the exe sits in <root>\build-release */
            if (!locate_routed_game(root))
                snprintf(s_game_elf, sizeof s_game_elf, "%s\\game\\EMAIN.ELF", root);
        }
        SetCurrentDirectoryA(root);    /* files with a relative name stay inside the port's folder */
    }
    snprintf(s_game_dir, sizeof s_game_dir, "%s", s_game_elf);
    for (char* p = s_game_dir; *p; p++) if (*p == '/') *p = '\\';
    char* s = strrchr(s_game_dir, '\\');
    if (s) *s = 0; else strcpy(s_game_dir, ".");
    default_dir("PS3_VFS_ROOT", "dev_bdvd");
    default_dir("PS3_HDD0_ROOT", "dev_hdd0");
    default_dir("PS3_HDD1_ROOT", "dev_hdd1");
    default_dir("PS3_DEV_FLASH", "dev_flash");
    default_dir("GT6_PRX_DIR", "firmware");
    return s_game_elf;
}

int main(int argc, char** argv)
{
    const char* elf = locate_game(argc > 1 ? argv[1] : NULL);

#pragma comment(lib, "winmm.lib")
    timeBeginPeriod(1);
    setvbuf(stdout, NULL, _IONBF, 0);
    /* Opt out of Windows 11 power throttling (EcoQoS ignores the 1 ms timer resolution for a
     * hidden or occluded window). GT6_POWER_THROTTLING=1 keeps Windows' default. */
    {
        const char* pt = getenv("GT6_POWER_THROTTLING");
        if (!(pt && *pt == '1')) {
            typedef struct { ULONG Version, ControlMask, StateMask; } power_state;
            typedef BOOL (WINAPI *spi_fn)(HANDLE, int, LPVOID, DWORD);
            spi_fn spi = (spi_fn)(void (*)(void))GetProcAddress(
                GetModuleHandleA("kernel32.dll"), "SetProcessInformation");
            power_state s = { 1, 0x1 | 0x4, 0 };
            if (spi) spi(GetCurrentProcess(), 4, &s, (DWORD)sizeof s);
        }
    }

    /* Defaults a plain start needs (the environment still overrides each):
     * argv[0] as the loader EBOOT hands it to EMAIN, and the live renderer. */
    if (!getenv("PS3_BOOT_PATH")) _putenv_s("PS3_BOOT_PATH", "/dev_hdd0/game/BCES01893/USRDIR/EMAIN.SELF");

    /* GT6_DEV_MENU=1: Develop Settings (R1+SELECT at My Home -> Tutorial -> Complete All).
     * GT6_LAN=1: hidden LAN mode ("lanmode" argv, room-sync + member-info patches, Winsock2 sockets, no PSN). */
    { const char* dm = getenv("GT6_DEV_MENU");
      if (dm && *dm == '1') {
          g_gt6_dev_menu = 1;
          fprintf(stderr, "[gt6] Develop Settings enabled (GT6_DEV_MENU=1)\n");
      }
      const char* lan = getenv("GT6_LAN");
      if (lan && *lan == '1') {
          g_gt6_lan = 1;
          g_gt6_dev_menu = 1;
          _putenv_s("PS3_NET_ONLINE", "1");
          const char* cur_argv = getenv("PS3_ARGV");
          if (!cur_argv || !*cur_argv) {
              _putenv_s("PS3_ARGV", "lanmode");
          } else if (!strstr(cur_argv, "lanmode")) {
              char buf[512];
              snprintf(buf, sizeof(buf), "%s;lanmode", cur_argv);
              _putenv_s("PS3_ARGV", buf);
          }
          const char* bind_ip = getenv("GT6_LAN_BIND");
          if (bind_ip && *bind_ip && !getenv("PS3_NET_BIND_IP"))
              _putenv_s("PS3_NET_BIND_IP", bind_ip);
          const char* lan_name = getenv("GT6_LAN_NAME");
          if (lan_name && *lan_name && !getenv("PS3_NP_ONLINE_ID"))
              _putenv_s("PS3_NP_ONLINE_ID", lan_name);
          if (!getenv("PS3_NP_ONLINE_ID") || !*getenv("PS3_NP_ONLINE_ID")) {
              const char* host = getenv("COMPUTERNAME");
              if (host && *host) {
                  char clean[17]; size_t n = 0;
                  for (const char* p = host; *p && n < 16; p++) {
                      unsigned char c = (unsigned char)*p;
                      if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-')
                          clean[n++] = (char)c;
                  }
                  clean[n] = '\0';
                  if (n > 0) _putenv_s("PS3_NP_ONLINE_ID", clean);
              }
          }
          fprintf(stderr, "[gt6-lan] LAN mode enabled (argv=%s, bind=%s, user=%s)\n",
                  getenv("PS3_ARGV") ? getenv("PS3_ARGV") : "",
                  getenv("PS3_NET_BIND_IP") ? getenv("PS3_NET_BIND_IP") : "auto",
                  getenv("PS3_NP_ONLINE_ID") ? getenv("PS3_NP_ONLINE_ID") : "default");
      } }
    if (!getenv("RSX_LIVE_DRAW")) _putenv_s("RSX_LIVE_DRAW", "1");
    /* Retain unchanged vertex arrays on the GPU. LD_VTX_SLAB=0 keeps the
     * ordinary upload path available for diagnosis and comparison. */
    if (!getenv("LD_VTX_SLAB")) _putenv_s("LD_VTX_SLAB", "1");
    /* Pipelines for depth-tested geometry are built off the drawing thread and the draw waits the few frames that
     * takes (rsx_live_draw.c, "Asynchronous pipelines"): a first visit to a track used to build over a hundred of
     * them inside single frames - a minute at 7-30 frames a second with stops of several seconds (b9_as_off);
     * with this it is about ten seconds at 25 and no stop (b10_on1), the scene filling in during the first of them.
     * LD_ASYNC_PSO=0 = the synchronous path. Six pipeline workers (and six compile workers) at lower priority. */
    if (!getenv("LD_ASYNC_PSO")) _putenv_s("LD_ASYNC_PSO", "1");
    if (!getenv("LD_PSO_PREWARM_THREADS")) _putenv_s("LD_PSO_PREWARM_THREADS", "6");
    /* PC default: edge-directed AA on the completed
     * GPU display image. The old SPU path remains GT6_AA=spu. A legacy
     * GT6_NO_MLAA override keeps its old meaning unless GT6_AA is explicit. */
    {
        const char* aa = getenv("GT6_AA");
        const char* legacy = getenv("GT6_NO_MLAA");
        if (!aa && legacy) aa = *legacy == '1' ? "off" : "spu";
        const int spu = aa && !_stricmp(aa, "spu");
        const int off = aa && !_stricmp(aa, "off");
        _putenv_s("GT6_NO_MLAA", spu ? "0" : "1");
        if (!getenv("LD_GPU_AA")) _putenv_s("LD_GPU_AA", (!spu && !off) ? "1" : "0");
        fprintf(stderr, "[gt6] anti-aliasing: %s (GT6_AA=gpu|spu|off)\n",
                spu ? "original SPU MLAA" : off ? "off" : "GPU display filter");
    }
    /* The game reserves 512 MB of address space for its heap (sys_mmapper_allocate_address, alignment 256 MB) and
     * its page map names a page by (address >> 13) & 0xFFFF, kept sorted: right only when that window starts on a
     * 512 MB boundary, as it does on the console (0x40000000). The runtime's mapper steps over its
     * own windows and returned 0x70000000: blocks in the lower half were not found any more, realloc() returned
     * NULL, and libc's atexit (which leaves its lock held on that path) blocked the next thread that used it. */
    if (!getenv("PS3_MAPPER_SELF_ALIGN")) _putenv_s("PS3_MAPPER_SELF_ALIGN", "1");
    /* Paths outside every mount point fail as on the console. The game probes "/specdb" and "/database" for
     * development copies of its databases and, unless told they are not mounted, creates and uses them. */
    if (!getenv("PS3_FS_STRICT_MOUNTS")) _putenv_s("PS3_FS_STRICT_MOUNTS", "1");
    /* The render job queue. Each frame's RSX list begins with a jump to itself that the serial SPU job (SPURS
     * workload 6, SPU 0 only) opens when the frame's five parallel jobs have produced the list: the walker stands
     * there for most of a frame, which is not a lost FIFO, so the 50 ms jump-park resync must not skip the list
     * (it would skip the label release at its end too). And the release of label 0x53 at the end of a list, which
     * the last parallel job of the next frame waits for, must not be lost when the walker gets there before the
     * previous one was taken (GCM_LABEL_BANK in cellGcmSys.c): otherwise the first frame whose last job runs on
     * SPU 0 waits for a list that only SPU 0 can open. */
    /* Every MFC channel access of an SPU looks up its MFC engine (mfc_for, spu_channels.c): a shared lock and a
     * walk of the list of engines, unless the runtime's per-thread memo of the last answer is on. GT6's five
     * SPURS threads make millions of such accesses a second (each idle-loop pass alone is a GETLLAR = seven), and a
     * sample of one of them (run235) had 15-21% of its time in that lock. The memo checks a generation that every
     * removal of an engine moves on, so a stale answer is never used. */
    if (!getenv("SPU_MFC_LOOKUP_CACHE")) _putenv_s("SPU_MFC_LOOKUP_CACHE", "1");
    if (!getenv("GCM_PARK_GRACE_MS")) _putenv_s("GCM_PARK_GRACE_MS", "600000");
    if (!getenv("GCM_LABEL_BANK"))    _putenv_s("GCM_LABEL_BANK", "530");
    /* GT6 copies its finished 3D scene into main memory with a 1:1 blit; SPU jobs post-process that copy and
     * write the picture into the display buffer. The pixels have to exist in guest memory for them. */
    if (!getenv("LD_BLIT_DOWNLOAD"))  _putenv_s("LD_BLIT_DOWNLOAD", "1");
    /* With GT6_NO_MLAA the scene goes to the display buffer by the game's own GPU copy (two NV3089 tiles) and
     * nobody reads either buffer's guest memory: the renderer does that copy on the GPU and the 3.7 MB copy of
     * stale guest bytes after it is left out (NV3089_GPU_ONLY, cellGcmSys.c). GT6_AA=spu instead keeps the
     * original post-process, which writes the display buffer in guest memory. */
    { const char* nm = getenv("GT6_NO_MLAA");
      if (nm && *nm == '1' && !getenv("NV3089_GPU_ONLY")) _putenv_s("NV3089_GPU_ONLY", "1"); }
    /* The RSX stream waits for label 0x53 == 0 (the SPUs have written the picture) before it draws the menu over
     * the display buffer, and releases it (:= 1) right after the scene copy. The wait has to be a real wait: without
     * it the menu was drawn first and the stream released the label twice for one take (the old menu freeze). */
    if (!getenv("GCM_SEMA_ACQUIRE"))    _putenv_s("GCM_SEMA_ACQUIRE", "1");
    if (!getenv("GCM_SEMA_ACQUIRE_MS")) _putenv_s("GCM_SEMA_ACQUIRE_MS", "1000");
    /* The game's five-second end-of-frame wait must not expire because the host renderer stalled (see above). */
    { const char* fw = getenv("GT6_FRAME_WAIT");
      if (!(fw && *fw == '0')) g_sys_cond_timeout_hook = gt6_frame_wait_timeout; }
    /* [GT6 0051] The cheaper forms of what the drawing thread does for every draw: on unless LD_FAST=0. The
     * once-a-frame look at guest memory that has long been clean: off unless LD_WW_STATIC=1 (no gain measured).
     * GT6_AB "fast" / "wws" switch them inside a run. */
    { const char* lf = getenv("LD_FAST");
      if (lf && *lf == '0') { g_ld_fast_ab = 0; g_gcm_fast_blit = 0; }
      const char* ws = getenv("LD_WW_STATIC");
      if (ws && *ws == '1') g_ld_wws_ab = 1; }
    /* The car's reflection cube map is rendered face by face into small targets and then sampled as a cube
     * texture with border texels: LD_SURFACE_MIRROR keeps those targets current in guest memory, where the cube
     * decoder reads them (rsx_live_draw.c). Without it the car body is black. It costs several GPU round trips
     * a frame. */
    if (!getenv("LD_SURFACE_MIRROR"))   _putenv_s("LD_SURFACE_MIRROR", "850000");
    /* GT6 uploads data inside RSX memory with the memory-to-memory engine on FIFO subchannel 1 (NV0039,
     * cellGcmSetTransferData) -- among it the instruction words of fragment programs whose constants it then patches
     * every frame. The walker gives subchannel 1 to the 3D engine unless told otherwise, and the copies never
     * happened: those programs read as "empty, constant, ..." and a new nonsense pixel shader was compiled for
     * their draws nearly every frame (cellGcmSys.c, gcm_2d_method). GCM_SUBCH1_2D=0 = as before. */
    if (!getenv("GCM_SUBCH1_2D"))       _putenv_s("GCM_SUBCH1_2D", "1");
    /* cellSaveData keeps saves under <PS3_GAMEDATA_ROOT>/dev_hdd0/home/00000001/savedata and falls back to
     * ./gamedata. Left unset it never found the saves that sit beside the installed game: the game was told there
     * was no save (BCJS37016-GAME6) and went down the first-start path instead of loading it. So follow dev_hdd0. */
    if (!getenv("PS3_GAMEDATA_ROOT")) {
        if (const char* h = getenv("PS3_HDD0_ROOT")) {
            char root[1024];
            snprintf(root, sizeof root, "%s", h);
            size_t n = strlen(root);
            while (n && (root[n - 1] == '\\' || root[n - 1] == '/')) root[--n] = 0;
            if (n > 8 && !_stricmp(root + n - 8, "dev_hdd0") && (root[n - 9] == '\\' || root[n - 9] == '/')) {
                root[n - 9] = 0;
                _putenv_s("PS3_GAMEDATA_ROOT", root);
            }
        }
    }

    /* cellFs's own entry points that take a path (Rename, Unlink ...) translate with a table of their own, whose
     * defaults are ./gamedata/<device> under the working directory - not the roots the sys_fs syscalls use
     * (PS3_HDD0_ROOT ...). Gran Turismo 6 opens and writes through the syscalls and renames through cellFs: what it
     * reads from the disc it copies to <file>.fdupi on the hard disk and then renames to <file>. Every such rename
     * failed ("No such file or directory" - a play log of 2026-11-20: 717 orphaned .fdupi files, 1.9 GB, the
     * same files written again at every race load), and an empty gamedata folder appeared beside the package. The
     * same roots for both now. GT6_CELLFS_ROOTS=0 = as before. */
    { const char* e = getenv("GT6_CELLFS_ROOTS");
      if (!(e && *e == '0')) {
          static const char* const dev[][2] = { {"PS3_HDD0_ROOT", "/dev_hdd0/"}, {"PS3_HDD1_ROOT", "/dev_hdd1/"} };
          for (const auto& d : dev)
              if (const char* h = getenv(d[0]))
                  if (*h) cellfs_add_path_mapping(d[1], h);
      } }

    /* GT6_ASPECT: ultrawide / custom display aspect ratio ("auto", "W:H", "WxH", or float e.g. "2.3888889").
     * Widens 3D projection at 0x008A615C, updates D3D12 present & window shape, and widens real-res width. */
    { const char* ae = getenv("GT6_ASPECT");
      if (ae && *ae && strcmp(ae, "0") != 0 && _stricmp(ae, "16:9") != 0 && _stricmp(ae, "off") != 0) {
          float asp = 0.0f;
          if (_stricmp(ae, "auto") == 0) {
              DEVMODEA dm; memset(&dm, 0, sizeof(dm)); dm.dmSize = sizeof(dm);
              if (EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &dm) && dm.dmPelsWidth > 0 && dm.dmPelsHeight > 0) {
                  const float ma = (float)dm.dmPelsWidth / (float)dm.dmPelsHeight;
                  if (fabsf(ma / (16.0f / 9.0f) - 1.0f) > 0.01f)
                      asp = ma;
                  fprintf(stderr, "[gt6] GT6_ASPECT=auto: monitor %lux%lu (%.4f)%s\n",
                          (unsigned long)dm.dmPelsWidth, (unsigned long)dm.dmPelsHeight, (double)ma,
                          asp > 0.5f ? " -> ultrawide/custom active" : " -> stock 16:9");
              }
          } else {
              float a = 0.0f, b = 0.0f;
              if (sscanf(ae, "%f:%f", &a, &b) == 2 && a > 0.0f && b > 0.0f) asp = a / b;
              else if (sscanf(ae, "%fx%f", &a, &b) == 2 && a > 0.0f && b > 0.0f) asp = a / b;
              else asp = (float)atof(ae);
          }
          if (asp >= 1.0f && asp <= 6.0f) {
              g_gt6_aspect = asp;
              rsx_live_draw_present_aspect = asp;
              rsx_window_aspect = asp;
              rsx_live_draw_present_stretch = 1;
              fprintf(stderr, "[gt6] aspect ratio override: %.4f (GT6_ASPECT=%s)\n", (double)asp, ae);
          }
      } }

    { const char* e = getenv("GT6_RES");
      const int by_env = e && *e;
      if (by_env ? atoi(e) == 1080 : gt6_menu_setting_full_hd() != 0) {
          g_gt6_res_1080 = 1;
          rsx_live_draw_present_stretch = 1;  /* its 1080p buffer is 1440x1080 for a 16:9 picture */
          cellVideoOut_set_resolution(1);     /* CELL_VIDEO_OUT_RESOLUTION_1080 */
          fprintf(stderr, "[gt6] video output reported as 1080p (%s)\n", by_env ? "GT6_RES=1080" : "the F2 menu's switch");
          /* [GT6 0102] GT6_UPSCALE=1 (= 1920x1080) or <W>x<H>: a real resolution, not a stretch - the game's
           * 1440x1080 scene and display buffers are made W x H on the host and drawn at that size (rsx_live_draw.c,
           * rsx_live_draw_upscale). Everything else (shadow maps, reflection faces, the brightness samples) keeps its
           * size. Not with GT6_AA=spu: the SPU anti-aliasing reads the guest-size picture from guest memory.
           * Without GT6_UPSCALE the F2 menu's switch decides (Settings -> Screen -> Real resolution). A picture larger
           * than the 1920x1080 window is drawn down into it (the present's stretch, linear). */
          { const char* up = getenv("GT6_UPSCALE");
            unsigned uw = 0, uh = 0;
            if (up && *up) {
                if (*up != '0' && sscanf(up, "%ux%u", &uw, &uh) != 2) { uw = 1920; uh = 1080; }
            } else {
                static const unsigned sizes[4][2] = { {0, 0}, {1920, 1080}, {2560, 1440}, {3840, 2160} };
                const int r = gt6_menu_setting_real_res();
                if (r > 0 && r < 4) {
                    uw = sizes[r][0];
                    uh = sizes[r][1];
                    if (g_gt6_aspect > 0.5f) {
                        unsigned aw = (unsigned)((double)uh * (double)g_gt6_aspect + 0.5);
                        aw = (aw + 1u) & ~1u;
                        if (aw < 1440u) aw = 1440u;
                        if (aw > 7680u) aw = 7680u;
                        uw = aw;
                    }
                }
            }
            const char* nm = getenv("GT6_NO_MLAA");
            if (uw && nm && *nm == '0') {
                fprintf(stderr, "[gt6] GT6_UPSCALE left off: GT6_AA=spu reads the guest-size picture\n");
                uw = uh = 0;
            }
            if (uw >= 1440 && uh >= 1080 && uw <= 7680 && uh <= 4320) {
                rsx_live_draw_upscale_size(1440, 1080);
                rsx_live_draw_upscale_size(1536, 1408);
                rsx_live_draw_upscale((float)uw / 1440.0f, (float)uh / 1080.0f);
                g_gt6_real_res = uw >= 3840 ? 3 : uw >= 2560 ? 2 : 1;
                fprintf(stderr, "[gt6] real resolution %ux%u (%s): the game's 1440x1080 buffers are that large on the host\n", uw, uh,
                        up && *up ? "GT6_UPSCALE" : "the F2 menu's switch");
            } }
      } }

    printf("=== GT6 native port (ps3recomp) ===\n");

    if (!alloc_guest_vm()) {
        fprintf(stderr, "ERROR: could not allocate the guest address space\n");
        return 1;
    }

    uint32_t entry = ppu_load_elf(elf);
    if (!entry) {
        fprintf(stderr, "ERROR: could not load %s\n"
                        "       (expected beside the exe: game\\EMAIN.ELF with game\\dev_bdvd and game\\dev_hdd0;\n"
                        "        or give the ELF as first argument)\n", elf);
        return 1;
    }

    derive_vfs_root(elf);
    printf("[boot] game folder: %s\n", s_game_dir);
    printf("[boot] VFS root: %s\n", ppu_vfs_root);

    ppu_recomp_register();   /* lifted function table -> address map */
    ps3_load_prx_modules();  /* lifted system PRX (libsre) + cellGame patch boot */
    ppu_hle_init();          /* firmware import NID -> HLE handlers */
    ppu_sysprx_register();   /* boot-critical CRT */
    ppu_fs_register();       /* cellFs over the game directory */
    lv2_init_syscalls();     /* the lv2 syscall table */
    gt6_install();           /* GT6: syscalls and hooks on top of the tables above */
    gt6_trainer_init();      /* Optional trainer bridge; incompatible images fail closed. */
    gt6_menu_install();      /* The trainer inside the game: F2 menu + its engine (gt6_menu.h). GT6_TRAINER_MENU=0 = off. */
    g_ps3_samp_frame_fn = cellGcm_flip_request_count;        /* each sample names the frame the game is building */
    /* SPU code under MXCSR round toward zero + flush-to-zero + denormals-are-zero [0036, 2026-11-16]: the lifted
     * job code then does fa/fs/fm/fma with one native op a vector instead of a widening to double (a third of the
     * SPU threads' busy time in the heavy part of a lap, run239). Same bits either way - the kit's
     * tests/test_spu_float_exact.c (447 M lanes in each mode) and test_spu_float_rz_x86.c. SPU_FLOAT_RZ=0 = off. */
    { const char* e = getenv("SPU_FLOAT_RZ"); g_spu_float_rz = (e && *e == '0') ? 0 : 1; }
    ps3_sampler_start();     /* no-op unless PS3_SAMPLE=<ms> or PS3_SAMPLE_MULTI=<names> */

    g_ps3_guest_caller = harness_guest_caller;
    HANDLE fc = CreateThread(NULL, 4u * 1024 * 1024, frame_clock, NULL, 0, NULL);
    if (fc) CloseHandle(fc);
    CreateThread(NULL, 0, freeze_watchdog, NULL, 0, NULL);
    if (getenv("GT6_SPURS_DUMP")) CreateThread(NULL, 0, spurs_dump_thread, NULL, 0, NULL);
    if (const char* bw = getenv("GT6_BOOT_WATCH"))
        if (atoi(bw) > 0) CreateThread(NULL, 0, boot_watch, (LPVOID)(uintptr_t)atoi(bw), 0, NULL);
    if (const char* ta = getenv("GT6_THREADS_AT"))
        if (atof(ta) > 0.0) CreateThread(NULL, 0, threads_at, (LPVOID)ta, 0, NULL);

    printf("\n[boot] dispatching entry OPD 0x%08X (stack top 0x%08X)\n\n", entry, STACK_TOP);

    g_ppu_main_stack_addr = STACK_TOP - STACK_SIZE;
    g_ppu_main_stack_size = STACK_SIZE;

    int rc = ppu_run(entry, STACK_TOP);
    printf("\n[boot] ppu_run returned %d (entry function unwound)\n", rc);
    return g_sys_process_exit_called ? (int)g_sys_process_exit_code : 0;
}
