/* The game bridge of the in-game menu: native C++ at byte-checked sites of GT6 1.22 EU (tools/trainer_post_lift.py
 * binds them) - the PP lock, the SQL rules of the career and licence unlock, the car names, and two crash guards.
 * Guest PPC instructions are never patched or interpreted by this bridge. */
#include "recompiled/ppu_recomp.h"
#include "gt6_trainer.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cwchar>
#include <string>

extern "C" __declspec(dllexport) gt6_trainer_bridge_data gt6_trainer_bridge = {
    {'G','T','6','N','T','R','0','1'}, 1, 64, 0,
    GT6_TRAINER_PAGE, GT6_TRAINER_PAGE2, 0x7F /* bit 6: the race gate */, 0,
    0x4754363132324555ull, 0, 0
};
static_assert(sizeof(gt6_trainer_bridge_data) == 64, "trainer ABI size");
static_assert(offsetof(gt6_trainer_bridge_data, ready) == 36, "trainer ABI layout");

namespace {
constexpr uint32_t P = GT6_TRAINER_PAGE, P2 = GT6_TRAINER_PAGE2;
constexpr uint32_t SQL_BASE = P + 0x20000, SQL_CAP = 0x4000, SQL_SLOTS = 128;
std::atomic<unsigned> next_sql_slot{0};
thread_local int sql_slot = -1;
thread_local uint32_t name_code = 0;
SRWLOCK name_lock = SRWLOCK_INIT, sql_log_lock = SRWLOCK_INIT;

/* Reject memory that is not there before touching game pointers, without setting off the port's lazy commit or the
 * title's own pager: the kernel's copy routine (ReadProcessMemory on this process) fails for such memory instead of
 * faulting. Committed guest memory is always read-write, so one answer serves reads and writes.
 * [2026-11-18] This asked VirtualQuery before, which walks the whole run of pages that share a state - about a
 * millisecond a call inside the 4 GB write-watch reservation (measured on the renderer's side, run251) - and the
 * hook sites ask at every physics step of a car, the SQL site at every statement. */
bool probe(uint64_t address) {
    uint8_t b; SIZE_T n = 0;
    return ReadProcessMemory(GetCurrentProcess(), vm_base + address, &b, 1, &n) && n == 1;
}
bool range(uint64_t address, size_t size, bool writing = false) {
    (void)writing;
    if (!vm_base || address < 0x10000 || address + size > 0x100000000ull || !size) return false;
    const uint64_t last = address + size - 1;
    if (!probe(address)) return false;
    for (uint64_t a = (address | 0xFFFull) + 1; a <= last; a += 0x1000) if (!probe(a)) return false;
    return true;
}
uint32_t r32(uint32_t a) {
    uint32_t raw;
    if ((a & 3) == 0) raw = __atomic_load_n((const uint32_t*)(vm_base + a), __ATOMIC_ACQUIRE);
    else memcpy(&raw, vm_base + a, 4);
    return _byteswap_ulong(raw);
}
void w32(uint32_t a, uint32_t value) {
    const uint32_t raw = _byteswap_ulong(value);
    if ((a & 3) == 0) __atomic_store_n((uint32_t*)(vm_base + a), raw, __ATOMIC_RELEASE);
    else memcpy(vm_base + a, &raw, 4);
}
float rf32(uint32_t a) { uint32_t u = r32(a); float f; memcpy(&f, &u, 4); return f; }
void wf32(uint32_t a, float f) { uint32_t u; memcpy(&u, &f, 4); w32(a, u); }
bool ready() { return __atomic_load_n(&gt6_trainer_bridge.ready, __ATOMIC_ACQUIRE) != 0; }
bool active() {
    if (!ready()) return false;
    const uint32_t tick = r32(P + 0x44);
    return tick != 0 && (uint32_t)((uint32_t)GetTickCount64() - tick) <= 30000u;
}
bool read_cstr(uint32_t addr, size_t cap, std::string& out) {
    out.clear();
    uint64_t cur = addr;
    char page[0x1000];
    while (out.size() < cap && cur >= 0x10000 && cur < 0x100000000ull) {
        /* a page at a time through the kernel's copy (see range): no fault, no region walk */
        const size_t n = (size_t)std::min<uint64_t>(
            std::min<uint64_t>(0x1000 - (cur & 0xFFF), cap - out.size()), 0x100000000ull - cur);
        SIZE_T got = 0;
        if (!n || !ReadProcessMemory(GetCurrentProcess(), vm_base + cur, page, n, &got) || got != n) return false;
        const char* nul = (const char*)memchr(page, 0, n);
        if (nul) { out.append(page, nul - page); return true; }
        out.append(page, n); cur += n;
    }
    return false;
}
bool bounded_field(const uint8_t* p, size_t n, std::string& out) {
    const auto* end = (const uint8_t*)memchr(p, 0, n);
    if (!end) return false;
    out.assign((const char*)p, end - p); return true;
}
bool sub_replace(std::string& sql, const std::string& pat, const std::string& repl) {
    if (pat.empty()) return false;
    const auto pos = sql.find(pat);
    if (pos == std::string::npos || sql.size() - pat.size() + repl.size() >= SQL_CAP) return false;
    sql.replace(pos, pat.size(), repl); return true;
}
void sql_prepare(ppu_context* c) {
    if (!active()) return;
    const uint32_t count = r32(P2);
    const bool log = r32(P2 + 4) != 0;
    const bool lng = false;
    if ((!count && !log && !lng) || count > 15) return;
    std::string sql;
    const uint32_t input = (uint32_t)c->gpr[4];
    const int32_t input_len = (int32_t)c->gpr[5];
    if (input_len >= 0) {
        if ((uint32_t)input_len >= SQL_CAP || !input_len || !range(input, (uint32_t)input_len)) return;
        const char* p = (const char*)vm_base + input;
        const char* nul = (const char*)memchr(p, 0, (uint32_t)input_len);
        sql.assign(p, nul ? (size_t)(nul - p) : (size_t)input_len);
    } else if (!read_cstr(input, SQL_CAP, sql)) return;
    if (log) {
        AcquireSRWLockExclusive(&sql_log_lock);
        const uint32_t n = r32(P2 + 8), a = P2 + 0x2000 + ((n & 63) << 8);
        const size_t len = std::min<size_t>(255, sql.size());
        memcpy(vm_base + a, sql.data(), len); vm_base[a + len] = 0;
        w32(P2 + 8, n + 1);
        ReleaseSRWLockExclusive(&sql_log_lock);
    }
    if (!count && !lng) return;
    /* Rules are published by count-last. A second stable snapshot rejects
     * concurrent UI updates instead of observing half-written strings. */
    std::array<uint8_t, 15 * 0x200> data{}, check{};
    if (count) {
        memcpy(data.data(), vm_base + P2 + 0x10, count * 0x200);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (r32(P2) != count) return;
        memcpy(check.data(), vm_base + P2 + 0x10, count * 0x200);
        if (memcmp(data.data(), check.data(), count * 0x200) || r32(P2) != count) return;
    }
    bool changed = false;
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t* p = data.data() + i * 0x200;
        uint32_t mode; memcpy(&mode, p, 4); mode = _byteswap_ulong(mode);
        if (mode == 5 || mode > 3) continue;
        std::string pat, repl;
        if (!bounded_field(p + 4, 0xFC, pat) || !bounded_field(p + 0x100, 0x100, repl)) continue;
        if (mode == 0 || mode == 1) {
            if (mode == 0 ? sql == pat : sql.compare(0, pat.size(), pat) == 0) {
                sql = repl; changed = true; break;
            }
        } else if (sub_replace(sql, pat, repl)) {
            changed = true;
            if (mode == 3 && i + 1 < count) {
                const uint8_t* q = p + 0x200;
                if (bounded_field(q + 4, 0xFC, pat) && bounded_field(q + 0x100, 0x100, repl))
                    sub_replace(sql, pat, repl);
            }
            break;
        }
    }
    if (!changed || sql.size() >= SQL_CAP || !active()) return;
    if (sql_slot == -1) {
        const unsigned slot = next_sql_slot.fetch_add(1, std::memory_order_relaxed);
        sql_slot = slot < SQL_SLOTS ? (int)slot : -2;
    }
    if (sql_slot < 0) return; // Exhaustion leaves the original statement intact.
    const uint32_t out = SQL_BASE + (uint32_t)sql_slot * SQL_CAP;
    memcpy(vm_base + out, sql.c_str(), sql.size() + 1);
    c->gpr[4] = out; c->gpr[5] = UINT64_MAX;
}
}

extern "C" int gt6_trainer_init(void) {
    if (ready()) return gt6_trainer_bridge.guest_base == (uint64_t)(uintptr_t)vm_base;
    if (!vm_base) return 0;
    constexpr uint32_t signatures[][2] = {
        {0x003D31D4u,0xC02B1D1Cu},{0x003D3224u,0xC02B1D20u},
        {0x00C063D4u,0x7CC83378u},{0x000EDDF0u,0x38A00100u},
        {0x000EDDFCu,0x7FA4EB78u},
        {0x002F6B84u,0xD1890004u},{0x002F71BCu,0xD00B0020u},
        {0x00A0BDACu,0x7C63F038u},{0x00A0BF50u,0x7C63F038u},{0x0081715Cu,0x4807972Du},{0x00B69768u,0x807F0024u},
        {0x009E66B0u,0x5463063Eu},
    };
    for (const auto& s : signatures) if (!range(s[0], 4) || r32(s[0]) != s[1]) {
        fprintf(stderr, "[trainer] disabled: unsupported EMAIN instruction at %08X\n", s[0]); return 0;
    }
    uintptr_t cur = (uintptr_t)vm_base + P;
    const uintptr_t end = cur + GT6_TRAINER_WINDOW_SIZE;
    while (cur < end) {
        MEMORY_BASIC_INFORMATION m{};
        if (!VirtualQuery((void*)cur, &m, sizeof m) || m.State != MEM_RESERVE || m.AllocationBase != vm_base) {
            fprintf(stderr, "[trainer] disabled: dedicated guest window is already in use\n"); return 0;
        }
        const uintptr_t next = (uintptr_t)m.BaseAddress + m.RegionSize;
        if (next <= cur) return 0;
        cur = next;
    }
    if (!VirtualAlloc(vm_base + P, GT6_TRAINER_WINDOW_SIZE, MEM_COMMIT, PAGE_READWRITE)) return 0;
    memset(vm_base + P, 0, GT6_TRAINER_WINDOW_SIZE);
    gt6_trainer_bridge.guest_base = (uint64_t)(uintptr_t)vm_base;
    gt6_trainer_bridge.boot_tick = GetTickCount64();
    __atomic_store_n(&gt6_trainer_bridge.ready, 1u, __ATOMIC_RELEASE);
    fprintf(stderr, "[bridge] ready, pages %08X/%08X; inactive until the menu's heartbeat\n", P, P2);
    return 1;
}


/* ---- the course graph search that does not end [2026-11-20, a crash at 20:01; run250 of 11-16 was the same] ----
 * func_003A3D58(node, wanted, from) looks for a node of a graph of the course by walking the nodes' links, depth first
 * and by calling itself; the only node it does not go back to is the one it came from. So on a graph with a ring in it
 * the search ends only when the wanted node is found. At the start of a race - here a race started again from the menu
 * behind the finish, three seconds in - the game sometimes asks while the graph's tables are not there yet ([null-read]
 * by 0x003A3B98 / 0x003A5C3C in the same seconds: the table of nodes is still 0); the node asked for is then none of
 * the graph's, and the search goes round and round. On the console that thread's stack (64 KB, 0xA0 bytes a call)
 * would be gone after 409 calls - there the tables are simply ready in time. Here the calls went on, their frames
 * written far below the guest stack, until the HOST stack (256 MB) ran out: exception 0xC00000FD on the game's main
 * thread, about a million and a half calls deep.
 * The search is ended where the console's stack would end: from the 384th call on, the entry answers "not found" at
 * once (what the function itself answers for a node that is not in the graph), and at every level after that, until
 * the outermost call has returned. Sites: 0x003A3D58 the entry, 0x003A3E0C the one way out. Independent of the
 * trainer's switches. */
static thread_local int g_graph_depth = 0;
static thread_local bool g_graph_stop = false;
static int graph_search_enter(ppu_context* c) {
    if (g_graph_stop || g_graph_depth >= 384) {
        if (!g_graph_stop) {
            g_graph_stop = true;
            static std::atomic<int> told{0};
            if (told.fetch_add(1) < 8)
                fprintf(stderr, "[gt6] the course graph search was ended %d calls deep (node %08X, wanted %08X, from %08X): the node asked for is not "
                                "in the graph - on the console the thread's stack would end here\n",
                        g_graph_depth, (uint32_t)c->gpr[3], (uint32_t)c->gpr[4], (uint32_t)c->gpr[5]);
        }
        c->gpr[3] = 0;
        return 1;
    }
    ++g_graph_depth;
    return 0;
}
static void graph_search_leave() {
    if (g_graph_depth > 0 && --g_graph_depth == 0) g_graph_stop = false;
}
/* GT6_TEST_GRAPH=1 (test): what the game did in that crash, made on purpose - a ring of three nodes, built below the
 * stack of the first guest thread that passes one of the trainer's sites, searched for a node that is not on it. */
static void graph_search_selftest(ppu_context* c) {
    static const bool on = getenv("GT6_TEST_GRAPH") != nullptr;
    static std::atomic<int> done{0};
    if (!on || done.exchange(1)) return;
    const ppu_context saved = *c;
    const uint32_t base = ((uint32_t)c->gpr[1] - 0x400) & ~0xFu;   /* nodes at base, + 0x20, + 0x40; their links from + 0x80 */
    if (!range((uint64_t)base, 0x200, true)) { fprintf(stderr, "[gt6-test] graph: no room below the stack\n"); return; }
    const uint32_t node[3] = {base, base + 0x20, base + 0x40};
    for (uint32_t i = 0; i < 3; ++i) {
        const uint32_t links = base + 0x80 + i * 0x20;
        for (uint32_t k = 0; k < 0x20; k += 4) { w32(node[i] + k, 0); w32(links + k, 0); }
        w32(node[i] + 0xC, links);
        w32(links + 0, node[(i + 1) % 3]);
        w32(links + 8, node[(i + 2) % 3]);
    }
    c->gpr[1] = (uint64_t)(base - 0x100);
    c->gpr[3] = node[0]; c->gpr[4] = 0x7FFFFFF0u; c->gpr[5] = 0;
    func_003A3D58(c);
    const uint32_t answer = (uint32_t)c->gpr[3];
    *c = saved;
    fprintf(stderr, "[gt6-test] graph: the search for a node that is not on a ring of three answered %08X (0 = not found); depth %d and stop %d afterwards\n",
            answer, g_graph_depth, (int)g_graph_stop);
}

/* ... and the walk along that graph that does not end [GT6 0062; 2026-11-21, test drive sp2: the same crash, 0xC00000FD two
 * seconds after the AI took the car at the start of a race, the search above never 384 deep].
 * func_003A3F20(distance, graph, position, .., wanted) moves a position - two nodes and the share between them - along
 * the graph towards a node: one link a call, by calling itself with what is left of the distance, the next node asked
 * of func_003A3D58 each time. With the table of nodes not there yet ([null-read] by 0x003A5C3C in the same seconds)
 * the position never comes to rest and the calls went on, 0x100 bytes of guest stack each, until the host's 256 MB
 * were gone. Ended like the search: the entry answers 1 (what the function says where the graph has no table: "the
 * position stands") once a walk is more than 384 calls and 32 KB of guest stack deep. A walk = entries that follow each
 * other with the stack pointer going down; an entry at or above the one before starts a new count - the game's own
 * walks are a few links long. One site, the entry (0x003A3F20); independent of the trainer's switches. */
static int graph_walk_enter(ppu_context* c) {
    static thread_local struct { uint32_t top, prev, calls; } w;
    const uint32_t sp = (uint32_t)c->gpr[1];
    if (!w.prev || sp >= w.prev) { w.top = sp; w.calls = 0; }
    else ++w.calls;
    w.prev = sp;
    if (w.calls > 384 && w.top - sp > 0x8000u) {
        static std::atomic<int> told{0};
        if (told.fetch_add(1) < 8)
            fprintf(stderr, "[gt6] the walk along the course graph was ended %u calls and %u bytes of stack deep (graph %08X, position %08X, "
                            "wanted %08X): the position never came to rest - on the console the thread's stack would end here\n",
                    w.calls, w.top - sp, (uint32_t)c->gpr[3], (uint32_t)c->gpr[4], (uint32_t)c->gpr[6]);
        w.prev = 0;                                         /* (the calls above this one return now: the next entry is a new walk) */
        c->gpr[3] = 1;
        return 1;
    }
    return 0;
}
extern "C" int gt6_trainer_bypass(ppu_context* c, uint32_t site) {
    if (c && site == 0x003A3D58) return graph_search_enter(c);
    if (c && site == 0x003A3F20) return graph_walk_enter(c);
    return 0;
}
/* ---- Looser Chase Camera [GT6 0113, 2026-11-24] (public issue #7: the RPCS3 GT6 PC build has a looser chase camera) --------
 * The RPCS3 patch "Looser Chase Camera" for 1.22 (author Edo, in RPCS3's patch.yml; facts used, no code copied): the chase
 * camera's spring damper and its largest angle are two floats of the table at 0x016DDFF0 (+ 4 and + 0x14; the game's 300 and
 * 30). func_002F7170 (also inside func_002F6DBC) fills the table's 32 bytes from 0x01130160 / 0x01130170, and
 * func_002F6B30 writes + 4 and + 0x14 again from the camera parameters at 0x016E632C + 0x58 / + 0x60 each time; the patch
 * takes those stores out (nop at 0x2F71B0 / 0x2F71B8 and 0x2F6B80 / 0x2F6B84) and puts its own numbers into the table.
 * Here, with P + 0x300 bit 0 (the menu's switch, P + 0x304 / + 0x308 the two values), the table is given the patch's
 * numbers right after the game's own stores (sites 0x002F71BC, the init's last store, and 0x002F6B84). */
static void chase_cam(uint32_t site) {
    if (!(r32(P + 0x300) & 1u)) return;
    constexpr uint32_t T = 0x016DDFF0u;
    if (!range(T, 0x20)) return;
    const float spring = rf32(P + 0x304), angle = rf32(P + 0x308);
    if (!(spring >= 0.0f && spring <= 300.0f) || !(angle >= 0.0f && angle <= 100.0f)) return;
    static int said;
    if (said < 4) { ++said; fprintf(stderr, "[camera] looser chase camera: the game wrote the table (site %08X: spring %.0f, angle %.0f) - now %.0f / %.0f\n",
                                    site, rf32(T + 4), rf32(T + 20), spring, angle); }
    if (site == 0x002F71BCu) { wf32(T, 30.0f); wf32(T + 8, 200.0f); w32(T + 12, 0); wf32(T + 16, 10.0f); wf32(T + 24, 20.0f); w32(T + 28, 0); }
    wf32(T + 4, spring); wf32(T + 20, angle);
}

/* ---- LAN mode [GT6 0114, 2026-11-24] (Mabec's GT6 PC build for RPCS3 - credits in the README - and the patches it uses,
 * "LAN Room Sync Without PSN" and RPCS3's "Develop Settings"; facts used, no code copied) -------------------------------------
 * With the hidden boot argument "lanmode" (main.cpp, the menu's LAN switch) GT6 plays over the home network - but it keeps its
 * "online" flag (r31 + 0x684F) at "network up AND signed in to PSN", so a host never shares its room and the others wait at
 * "Creating room... Please wait": at 0x00A0BDAC and 0x00A0BF50 the flag is made "network up" alone (and r3,r3,r30 ->
 * mr r3,r30). The game's developer settings (R1 + SELECT at My Home) are opened too - completing the tutorials there is
 * what unlocks Open Lobby: the answer of func_00890888 at 0x0081715C is 1 (the patch's li r3,1). Only in LAN mode. */
static bool lan_on() {
    static const int on = [] { const char* a = getenv("PS3_ARGV"); return a && strstr(a, "lanmode") ? 1 : 0; }();
    return on != 0;
}
/* [GT6 0114] "LAN Host Member Info" (abowen; cosmetic): the room's host showed the members who joined as blank, nameless
 * entries. Where func_00B69488 has filled a member's node record (r29: its id at + 0, its name / address / MAC at + 0x70,
 * 0x30 bytes), the entry of the host's member table ([r31 + 0x18] + 0xBC, 64 entries of 0x70; + 0x69 in use, + 0x40 the
 * node id) gets those 0x30 bytes - what the patch's code cave at 0x00B69768 does, here after the instruction it kept
 * (lwz r3,0x24(r31)). Only in LAN mode. */
static void lan_member_info(ppu_context* c) {
    if (!lan_on()) return;
    const uint32_t r31 = (uint32_t)c->gpr[31], r29 = (uint32_t)c->gpr[29];
    if (!range((uint64_t)r31 + 0x18, 4) || !range((uint64_t)r29, 0xA0)) return;
    const uint32_t owner = r32(r31 + 0x18);
    if (!range((uint64_t)owner + 0xBC, 4)) return;
    uint32_t t = r32(owner + 0xBC);
    if (!range((uint64_t)t, 64u * 0x70u)) return;
    const uint32_t node = (uint32_t)vm_base[r29] << 8 | vm_base[r29 + 1];
    for (int i = 0; i < 64; ++i, t += 0x70) {
        if (!vm_base[t + 0x69] || r32(t + 0x40) != node) continue;
        memcpy(vm_base + t, vm_base + r29 + 0x70, 0x30);
        static int said;
        if (said < 4) { ++said; fprintf(stderr, "[lan] member %u: name and address filled in the host's list\n", node); }
        break;
    }
}
static bool dev_menu_on() {
    static const int on = [] { const char* d = getenv("GT6_DEV_MENU"); return (d && *d == '1') ? 1 : 0; }();
    return on != 0;
}
static void lan_site(ppu_context* c, uint32_t site) {
    if (site == 0x0081715Cu) {
        if (!lan_on() && !dev_menu_on()) return;
        static int said_dev;
        if (!said_dev) { said_dev = 1; fprintf(stderr, "[lan] site 0081715C: developer settings on\n"); }
        c->gpr[3] = 1;
        return;
    }
    if (!lan_on()) return;
    if (site == 0x00B69768u) { lan_member_info(c); return; }
    static int said;
    if (said < 3) { ++said; fprintf(stderr, "[lan] site %08X: the room follows 'network up'\n", site); }
    c->gpr[3] = c->gpr[30];
}

/* ---- the game's bank book [GT6 0120, 2026-11-25, public issue #8] -------------------------------------------------------------
 * Next to the credits Gran Turismo 6 keeps a "bank book" of every amount it paid out or took itself, sealed with its own cipher
 * (the script class pdistd::MCipher). Before it takes a payment it opens the book, and when the credits are more than the book
 * says it sets them back to the book's amount: credits set in the menu showed on screen, and the shop answered "Insufficient
 * credits" for anything dearer than the old balance (the lock could not help - the game looks at the moment it pays).
 * func_009E6210 is that class's "decrypt"; at 0x009E66B0 its worker (func_00AE625C) has just returned: r3 = it opened the
 * block, r28 = the plain bytes, r29 = their size. The menu's engine looks at them there (gt6_menu_bank_book): a bank book
 * whose amount is below the credits gets the credits as its amount, so the game's own check passes, and the game then writes
 * the payment into the book and seals it as it always does. */
extern "C" void gt6_menu_bank_book(uint32_t plain, uint32_t size);   /* gt6_menu_engine.cpp */

extern "C" void gt6_trainer_after(ppu_context* c, uint32_t site) {
    if (site == 0x003A3E0C) { graph_search_leave(); return; }
    if (c) graph_search_selftest(c);
    if (!c || !ready()) return;
    if (site == 0x002F6B84 || site == 0x002F71BC) { chase_cam(site); return; }   /* [GT6 0113] */
    if (site == 0x009E66B0) {                                                    /* [GT6 0120] */
        if ((c->gpr[3] & 0xFF) && range((uint64_t)(uint32_t)c->gpr[28], 0x30)) gt6_menu_bank_book((uint32_t)c->gpr[28], (uint32_t)c->gpr[29]);
        return;
    }
    if (site == 0x00A0BDAC || site == 0x00A0BF50 || site == 0x0081715C || site == 0x00B69768) { lan_site(c, site); return; }   /* [GT6 0114] */
    if (site == 0x000EDDF0) { name_code = (uint32_t)c->gpr[3]; w32(P + 0x40, name_code); return; }
    if (site == 0x000EDDFC) {
        std::string name;
        try { if (!read_cstr((uint32_t)c->gpr[29], 0x100, name)) return; } catch (...) { return; }
        const uint32_t dst = P + 0x3000 + (name_code & 0x1FF) * 0x60;
        const size_t len = std::min<size_t>(91, name.size());
        AcquireSRWLockExclusive(&name_lock);
        w32(dst, 0); memcpy(vm_base + dst + 4, name.data(), len); vm_base[dst + 4 + len] = 0; w32(dst, name_code);
        ReleaseSRWLockExclusive(&name_lock);
        return;
    }
    if (site == 0x003D31D4 || site == 0x003D3224) {
        const uint32_t car = (uint32_t)c->gpr[9];
        if (!range((uint64_t)car + 0x80, 4)) return;
        const uint32_t gid = r32(car + 0x80), off = site == 0x003D31D4 ? 4 : 8;
        if (off == 4) { w32(P + 0x10, gid); wf32(P + 0x14, (float)c->fpr[1]); w32(P + 0x18, car); }
        else wf32(P + 0x1C, (float)c->fpr[1]);
        if (!active() || !gid) return;
        const uint32_t count = r32(P + 0x20);
        if (count > 512) return;
        for (uint32_t i = 0; i < count; ++i) {
            const uint32_t entry = P + 0x1000 + i * 16;
            if (r32(entry) == gid) {
                const float value = rf32(entry + off);
                if (std::isfinite(value) && value >= 0) c->fpr[1] = value;
                break;
            }
        }
        return;
    }
    if (!active()) return;
    switch (site) {
    case 0x00C063D4: try { sql_prepare(c); } catch (...) {} break;
    default: break;
    }
}
