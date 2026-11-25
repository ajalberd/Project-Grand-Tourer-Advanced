// gt6_menu_engine.cpp - the in-game menu's engine (see gt6_menu.h): the credit lock, the SQL rules of the career and
// licence unlock, the PP lock table of the game bridge, the car names, the heartbeat, and the settings file.
// Guest memory outside the bridge's control pages is read and written through the kernel's copy (Read/Write-
// ProcessMemory on this process), so a page that is not there fails instead of faulting.
#include "gt6_menu.h"
#include "gt6_trainer.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <emmintrin.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

extern "C" {
extern uint8_t* vm_base;
extern float rsx_live_draw_present_gamma;   /* rsx_live_draw.c [GT6 0101]: the present's exponent, out = in^g */
extern int rsx_live_draw_present_max_fps;   /* rsx_live_draw.c: 0 = vsync, -1 = unlimited, 30..1000 = paced Hz */
extern volatile int g_gt6_unlock_fps;       /* gt6_frame_timing.h: 1 = clean VBlank target unlock */
}

// ---- [GT6 0087] the menu's language (see gt6_menu.h) ----------------------------------------------------------
namespace {
struct TrPair { const char* ko; const char* en; };
const TrPair kTrTable[] = {
#include "gt6_menu_l10n.inc"
    {nullptr, nullptr}
};
// "MenuLang" of the settings file as it stands on the disk (0 where there is none). GT6_MENU_LANG=ko / en: tests.
int trLangAtBoot()
{
    if (const char* e = std::getenv("GT6_MENU_LANG"); e && *e)
        return e[0] == 'k' || e[0] == 'K' ? 1 : 2;
    wchar_t path[1024] = L"";
    if (const char* e = std::getenv("GT6_MENU_SETTINGS"); e && *e)
        MultiByteToWideChar(CP_ACP, 0, e, -1, path, 1023);
    else {
        const DWORD n = GetModuleFileNameW(nullptr, path, 900);
        wchar_t* slash = n ? wcsrchr(path, L'\\') : nullptr;
        if (slash)
            wcscpy(slash + 1, L"GT6Menu.settings.json");
        else
            path[0] = 0;
    }
    int lang = 0;
    if (path[0])
        if (FILE* f = _wfopen(path, L"rb")) {
            static char text[1 << 18];
            const size_t got = fread(text, 1, sizeof text - 1, f);
            fclose(f);
            text[got] = 0;
            if (const char* k = std::strstr(text, "\"MenuLang\"")) {
                k += 10;
                while (*k == ' ' || *k == ':' || *k == '\t')
                    ++k;
                if (*k >= '0' && *k <= '2')
                    lang = *k - '0';
            }
        }
    return lang;
}
}   // namespace

bool gt6_tr_english()
{
    static const bool english = [] {
        const int lang = trLangAtBoot();
        return lang == 2 || (lang == 0 && PRIMARYLANGID(GetUserDefaultUILanguage()) != LANG_KOREAN);
    }();
    return english;
}

const char* gt6_tr(const char* ko)
{
    if (!ko || !gt6_tr_english())
        return ko;
    static const std::unordered_map<std::string_view, const char*> map = [] {
        std::unordered_map<std::string_view, const char*> m;
        for (const TrPair* p = kTrTable; p->ko; ++p)
            m.emplace(p->ko, p->en);
        return m;
    }();
    const auto it = map.find(std::string_view(ko));
    return it == map.end() ? ko : it->second;
}

namespace gt6menu {

namespace {

constexpr uint32_t P = GT6_TRAINER_PAGE, P2 = GT6_TRAINER_PAGE2;
constexpr uint32_t HeapMin = 0x10000000u;   // below this is the EMAIN image / static data, never a live car record
constexpr int MaxCarCode = 0xFFFF, MaxGarageId = 0xFFFF;

// ---- shared state ---------------------------------------------------------------------------------------------
std::mutex g_mx, g_stMx, g_cmdMx, g_toastMx, g_catMx;
Settings g_set;
Status g_status;
std::vector<Command> g_cmds;
std::atomic<bool> g_dirty{false}, g_rulesDirty{true}, g_captured{false}, g_perfPage{false};
std::atomic<unsigned> g_padPort[4];
unsigned g_toastSeq = 0;
std::atomic<unsigned> g_toastSeqA{0};
std::string g_toastText;
bool g_toastWarn = false;

std::string fmt(const char* f, ...)
{
    char b[1024];
    va_list ap;
    va_start(ap, f);
    std::vsnprintf(b, sizeof b, f, ap);
    va_end(ap);
    return b;
}

// "0.##": up to `dec` decimals, no trailing zeros.
std::string num(double v, int dec = 2)
{
    char b[64];
    std::snprintf(b, sizeof b, "%.*f", dec, v);
    std::string s = b;
    if (s.find('.') != std::string::npos) {
        while (!s.empty() && s.back() == '0')
            s.pop_back();
        if (!s.empty() && s.back() == '.')
            s.pop_back();
    }
    return s;
}

bool g_log = false;   // GT6_MENU_LOG=1: what the engine does, to stderr
void logf(const char* f, ...)
{
    char b[1024];
    va_list ap;
    va_start(ap, f);
    std::vsnprintf(b, sizeof b, f, ap);
    va_end(ap);
    std::fprintf(stderr, "[menu] %s\n", b);
}

// ---- guest memory -----------------------------------------------------------------------------------------------
bool rd(uint32_t g, void* out, size_t n)
{
    if (!vm_base || !n || (uint64_t)g + n > 0x100000000ull)
        return false;
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), vm_base + g, out, n, &got) && got == n;
}
bool wr(uint32_t g, const void* in, size_t n)
{
    if (!vm_base || !n || g < 0x10000u || (uint64_t)g + n > 0x100000000ull)
        return false;
    SIZE_T got = 0;
    return WriteProcessMemory(GetCurrentProcess(), vm_base + g, in, n, &got) && got == n;
}
std::optional<uint32_t> u32(uint32_t g)
{
    uint32_t v;
    if (!rd(g, &v, 4))
        return std::nullopt;
    return _byteswap_ulong(v);
}
uint32_t u32z(uint32_t g) { return u32(g).value_or(0); }
std::optional<uint16_t> u16(uint32_t g)
{
    uint16_t v;
    if (!rd(g, &v, 2))
        return std::nullopt;
    return _byteswap_ushort(v);
}
std::optional<float> f32(uint32_t g)
{
    auto v = u32(g);
    if (!v)
        return std::nullopt;
    float f;
    const uint32_t raw = *v;
    std::memcpy(&f, &raw, 4);
    return f;
}
bool w32(uint32_t g, uint32_t v)
{
    v = _byteswap_ulong(v);
    return wr(g, &v, 4);
}
bool w16(uint32_t g, uint16_t v)
{
    v = _byteswap_ushort(v);
    return wr(g, &v, 2);
}
bool w64(uint32_t g, uint64_t v)
{
    v = _byteswap_uint64(v);
    return wr(g, &v, 8);
}
void be32(uint8_t* p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}
uint32_t rbe32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
void bef32(uint8_t* p, float f)
{
    uint32_t u;
    std::memcpy(&u, &f, 4);
    be32(p, u);
}

// The control pages are committed for the life of the process once the bridge is ready: plain loads and stores.
uint32_t p32(uint32_t a) { return _byteswap_ulong(__atomic_load_n((const uint32_t*)(vm_base + a), __ATOMIC_ACQUIRE)); }
void wp32(uint32_t a, uint32_t v) { __atomic_store_n((uint32_t*)(vm_base + a), _byteswap_ulong(v), __ATOMIC_RELEASE); }
float pf32(uint32_t a)
{
    const uint32_t u = p32(a);
    float f;
    std::memcpy(&f, &u, 4);
    return f;
}
void wpf32(uint32_t a, float f)
{
    uint32_t u;
    std::memcpy(&u, &f, 4);
    wp32(a, u);
}
bool bridgeReady() { return vm_base && __atomic_load_n(&gt6_trainer_bridge.ready, __ATOMIC_ACQUIRE) != 0; }

// [GT6 0113] Looser Chase Camera (Settings -> Display; public issue #7): the switch and its two values for the bridge
// (gt6_trainer.cpp chase_cam, P + 0x300 / + 0x304 / + 0x308), and the camera table itself (0x016DDFF0) at once when they
// change - with it on the game's own stores are answered by the bridge; off, the game's numbers go back: the init's source
// (0x01130160, 32 bytes) and the camera parameters' + 0x54 / + 0x58 / + 0x60 (0x016E632C) as func_002F6B30 writes them.
void cameraTick(const Settings& s)
{
    if (!bridgeReady())
        return;
    const float spring = (float)std::clamp(std::isfinite(s.camSpring) ? s.camSpring : 90.0, 0.0, 300.0);
    const float angle = (float)std::clamp(std::isfinite(s.camAngle) ? s.camAngle : 35.0, 0.0, 100.0);
    wpf32(P + 0x304, spring);
    wpf32(P + 0x308, angle);
    wp32(P + 0x300, s.camLoose ? 1u : 0u);
    static int last = -1;
    static float lastSpring = -1.0f, lastAngle = -1.0f;
    const int on = s.camLoose ? 1 : 0;
    if (on == last && spring == lastSpring && angle == lastAngle)
        return;
    constexpr uint32_t T = 0x016DDFF0u, Init = 0x01130160u, Prm = 0x016E632Cu;
    if (on) {
        wpf32(T + 4, spring);
        wpf32(T + 8, 200.0f);
        wp32(T + 12, 0);
        wpf32(T + 20, angle);
        wpf32(T + 24, 20.0f);
        wp32(T + 28, 0);
    } else if (last == 1) {
        for (uint32_t i = 0; i < 32; i += 4)
            wp32(T + i, p32(Init + i));
        wp32(T, p32(Prm + 0x54));
        wp32(T + 4, p32(Prm + 0x58));
        wp32(T + 16, p32(Prm + 0x60));
        wp32(T + 20, p32(Prm + 0x60));
    }
    last = on;
    lastSpring = spring;
    lastAngle = angle;
}

// Committed, readable guest memory (Rpcs3Memory.Regions): the RSX range 0xC0000000..0xD0000000 is left out.
struct Region {
    uint64_t lo, hi;
};
std::vector<Region> regions()
{
    std::vector<Region> out;
    auto add = [&](uint64_t lo, uint64_t hi) {
        if (lo >= hi)
            return;
        if (!out.empty() && out.back().hi == lo)
            out.back().hi = hi;
        else
            out.push_back({lo, hi});
    };
    const uintptr_t base = (uintptr_t)vm_base, end = base + 0x100000000ull;
    for (uintptr_t a = base; a < end;) {
        MEMORY_BASIC_INFORMATION m;
        if (!VirtualQuery((void*)a, &m, sizeof m) || !m.RegionSize || (uintptr_t)m.BaseAddress > a)
            break;
        const uintptr_t next = (uintptr_t)m.BaseAddress + m.RegionSize;
        if (next <= a)
            break;
        const DWORD pr = m.Protect & 0xFF;
        const bool readable = m.State == MEM_COMMIT && !(m.Protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
                              (pr == 0x02 || pr == 0x04 || pr == 0x08 || pr == 0x20 || pr == 0x40 || pr == 0x80);
        if (readable) {
            const uint64_t lo = a - base, hi = std::min(next, end) - base;
            add(lo, std::min<uint64_t>(hi, 0xC0000000ull));
            add(std::max<uint64_t>(lo, 0xD0000000ull), hi);
        }
        a = std::min(next, end);
    }
    return out;
}

// One pass over guest memory for big-endian words at 4-aligned addresses >= minAddr. onHit(pattern, address,
// pointer to the word in the copy, bytes of the copy from there on). 8 MB at a time through the kernel's copy; a
// piece that cannot be read whole is skipped, as in the external trainer.
struct ScanTotals {
    double ms = 0;
    uint64_t bytes = 0;
    unsigned scans = 0;
} g_scan;

template <class F>
void scanWords(const uint32_t* words, int n, uint32_t minAddr, F&& onHit)
{
    static std::vector<uint8_t> buf;   // the engine thread only
    constexpr size_t chunk = 8u << 20, extra = 256;
    if (n <= 0 || n > 8 || !vm_base)
        return;
    buf.resize(chunk + extra);
    LARGE_INTEGER t0, t1, qf;
    QueryPerformanceCounter(&t0);
    __m128i pat[8];
    for (int k = 0; k < n; k++)
        pat[k] = _mm_set1_epi32((int)_byteswap_ulong(words[k]));
    uint64_t total = 0;
    const HANDLE self = GetCurrentProcess();
    for (const Region& r : regions()) {
        uint64_t lo = std::max<uint64_t>(r.lo, minAddr);
        lo = (lo + 3) & ~3ull;
        for (uint64_t off = lo; off < r.hi; off += chunk) {
            const size_t len = (size_t)std::min<uint64_t>(chunk + extra, r.hi - off);
            SIZE_T got = 0;
            if (!ReadProcessMemory(self, vm_base + off, buf.data(), len, &got) || got != len)
                continue;
            const size_t scan = std::min(len, chunk) & ~(size_t)3;
            total += scan;
            const uint8_t* b = buf.data();
            auto lane = [&](size_t at) {
                uint32_t raw;
                std::memcpy(&raw, b + at, 4);
                const uint32_t w = _byteswap_ulong(raw);
                for (int k = 0; k < n; k++)
                    if (w == words[k])
                        onHit(k, (uint32_t)(off + at), b + at, len - at);
            };
            size_t i = 0;
            for (; i + 16 <= scan; i += 16) {
                const __m128i v = _mm_loadu_si128((const __m128i*)(b + i));
                __m128i m = _mm_cmpeq_epi32(v, pat[0]);
                for (int k = 1; k < n; k++)
                    m = _mm_or_si128(m, _mm_cmpeq_epi32(v, pat[k]));
                const int mask = _mm_movemask_epi8(m);
                if (!mask)
                    continue;
                for (int l = 0; l < 4; l++)
                    if (mask & (0xF << (l * 4)))
                        lane(i + (size_t)l * 4);
            }
            for (; i + 4 <= scan; i += 4)
                lane(i);
        }
    }
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&qf);
    g_scan.ms = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)qf.QuadPart;
    g_scan.bytes = total;
    g_scan.scans++;
    if (g_log && (g_scan.scans <= 12 || g_scan.scans % 20 == 0))
        logf("scan %u: %.1f MB in %.1f ms (%d words from %08X)", g_scan.scans, total / 1048576.0, g_scan.ms, n, minAddr);
}

std::vector<uint32_t> findWord(uint32_t word)
{
    std::vector<uint32_t> hits;
    scanWords(&word, 1, HeapMin, [&](int, uint32_t a, const uint8_t*, size_t) { hits.push_back(a); });
    return hits;
}


// ---- files -------------------------------------------------------------------------------------------------------
std::wstring exeDir()
{
    wchar_t p[MAX_PATH * 4];
    const DWORD n = GetModuleFileNameW(nullptr, p, (DWORD)(sizeof p / sizeof p[0]));
    std::wstring s(p, n);
    const size_t k = s.find_last_of(L"\\/");
    return k == std::wstring::npos ? L"." : s.substr(0, k);
}
std::wstring widen(const std::string& s, UINT cp = CP_UTF8)
{
    if (s.empty())
        return L"";
    const int n = MultiByteToWideChar(cp, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(cp, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}
std::string narrow(const std::wstring& w)
{
    if (w.empty())
        return "";
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}
bool readFile(const std::wstring& path, std::string& out)
{
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f)
        return false;
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    out.assign((size_t)std::max(0L, n), '\0');
    const size_t got = n > 0 ? std::fread(out.data(), 1, (size_t)n, f) : 0;
    std::fclose(f);
    out.resize(got);
    if (out.size() >= 3 && (uint8_t)out[0] == 0xEF && (uint8_t)out[1] == 0xBB && (uint8_t)out[2] == 0xBF)
        out.erase(0, 3);
    return true;
}
bool writeFileAtomic(const std::wstring& path, const std::string& data, bool keepBak)
{
    const std::wstring tmp = path + L".tmp";
    FILE* f = _wfopen(tmp.c_str(), L"wb");
    if (!f)
        return false;
    const bool ok = std::fwrite(data.data(), 1, data.size(), f) == data.size();
    std::fclose(f);
    if (!ok)
        return false;
    if (keepBak && GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES)
        CopyFileW(path.c_str(), (path + L".bak").c_str(), FALSE);
    return MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}
void appendLine(const std::wstring& path, const std::string& line)
{
    FILE* f = _wfopen(path.c_str(), L"ab");
    if (!f)
        return;
    std::fwrite(line.data(), 1, line.size(), f);
    std::fwrite("\r\n", 1, 2, f);
    std::fclose(f);
}
std::wstring logsDir()
{
    const std::wstring d = exeDir() + L"\\logs";
    CreateDirectoryW(d.c_str(), nullptr);
    return d;
}
std::string nowIso()
{
    SYSTEMTIME t;
    GetLocalTime(&t);
    return fmt("%04d-%02d-%02dT%02d:%02d:%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
}
std::string nowClock()
{
    SYSTEMTIME t;
    GetLocalTime(&t);
    return fmt("%02d:%02d:%02d", t.wHour, t.wMinute, t.wSecond);
}

// ---- JSON (the profile file) ----------------------------------------------------------------------------------
struct J {
    enum T { Null, Bool, Num, Str, Arr, Obj } t = Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<J> a;
    std::vector<std::pair<std::string, J>> o;
    const J* get(const char* k) const
    {
        for (const auto& kv : o)
            if (kv.first == k)
                return &kv.second;
        return nullptr;
    }
};

struct JParse {
    const char* p;
    const char* e;
    bool ok = true;
    void ws()
    {
        while (p < e && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n'))
            p++;
    }
    static void utf8(std::string& out, uint32_t c)
    {
        if (c < 0x80)
            out += (char)c;
        else if (c < 0x800) {
            out += (char)(0xC0 | c >> 6);
            out += (char)(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            out += (char)(0xE0 | c >> 12);
            out += (char)(0x80 | ((c >> 6) & 0x3F));
            out += (char)(0x80 | (c & 0x3F));
        } else {
            out += (char)(0xF0 | c >> 18);
            out += (char)(0x80 | ((c >> 12) & 0x3F));
            out += (char)(0x80 | ((c >> 6) & 0x3F));
            out += (char)(0x80 | (c & 0x3F));
        }
    }
    bool hex4(uint32_t& v)
    {
        if (e - p < 4)
            return false;
        v = 0;
        for (int i = 0; i < 4; i++) {
            const char c = *p++;
            v <<= 4;
            if (c >= '0' && c <= '9')
                v |= (uint32_t)(c - '0');
            else if (c >= 'a' && c <= 'f')
                v |= (uint32_t)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F')
                v |= (uint32_t)(c - 'A' + 10);
            else
                return false;
        }
        return true;
    }
    bool str(std::string& out)
    {
        if (p >= e || *p != '"')
            return false;
        p++;
        while (p < e && *p != '"') {
            if (*p != '\\') {
                out += *p++;
                continue;
            }
            if (++p >= e)
                return false;
            const char c = *p++;
            switch (c) {
            case 'n': out += '\n'; break;
            case 't': out += '\t'; break;
            case 'r': out += '\r'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'u': {
                uint32_t v;
                if (!hex4(v))
                    return false;
                if (v >= 0xD800 && v < 0xDC00 && e - p >= 6 && p[0] == '\\' && p[1] == 'u') {
                    p += 2;
                    uint32_t lo;
                    if (!hex4(lo))
                        return false;
                    v = 0x10000 + ((v - 0xD800) << 10) + (lo - 0xDC00);
                }
                utf8(out, v);
                break;
            }
            default: out += c; break;
            }
        }
        if (p >= e)
            return false;
        p++;
        return true;
    }
    J value(int depth = 0)
    {
        J j;
        ws();
        if (p >= e || depth > 32) {
            ok = false;
            return j;
        }
        if (*p == '{') {
            j.t = J::Obj;
            p++;
            ws();
            if (p < e && *p == '}') {
                p++;
                return j;
            }
            while (ok) {
                ws();
                std::string k;
                if (!str(k)) {
                    ok = false;
                    break;
                }
                ws();
                if (p >= e || *p != ':') {
                    ok = false;
                    break;
                }
                p++;
                j.o.push_back({k, value(depth + 1)});
                ws();
                if (p < e && *p == ',') {
                    p++;
                    continue;
                }
                if (p < e && *p == '}') {
                    p++;
                    break;
                }
                ok = false;
            }
        } else if (*p == '[') {
            j.t = J::Arr;
            p++;
            ws();
            if (p < e && *p == ']') {
                p++;
                return j;
            }
            while (ok) {
                j.a.push_back(value(depth + 1));
                ws();
                if (p < e && *p == ',') {
                    p++;
                    continue;
                }
                if (p < e && *p == ']') {
                    p++;
                    break;
                }
                ok = false;
            }
        } else if (*p == '"') {
            j.t = J::Str;
            if (!str(j.s))
                ok = false;
        } else if (e - p >= 4 && !std::strncmp(p, "true", 4)) {
            j.t = J::Bool;
            j.b = true;
            p += 4;
        } else if (e - p >= 5 && !std::strncmp(p, "false", 5)) {
            j.t = J::Bool;
            p += 5;
        } else if (e - p >= 4 && !std::strncmp(p, "null", 4)) {
            p += 4;
        } else {
            char* end = nullptr;
            const std::string tmp(p, (size_t)std::min<ptrdiff_t>(e - p, 48));
            j.n = std::strtod(tmp.c_str(), &end);
            if (end == tmp.c_str())
                ok = false;
            else {
                j.t = J::Num;
                p += end - tmp.c_str();
            }
        }
        return j;
    }
};

bool jsonParse(const std::string& text, J& out)
{
    JParse ps{text.data(), text.data() + text.size()};
    out = ps.value();
    return ps.ok && out.t != J::Null;
}

void jsonStr(std::string& o, const std::string& s)
{
    o += '"';
    for (const char c : s) {
        switch (c) {
        case '"': o += "\\\""; break;
        case '\\': o += "\\\\"; break;
        case '\n': o += "\\n"; break;
        case '\r': o += "\\r"; break;
        case '\t': o += "\\t"; break;
        default:
            if ((unsigned char)c < 0x20)
                o += fmt("\\u%04x", c);
            else
                o += c;
        }
    }
    o += '"';
}
std::string jsonNum(double v)
{
    if (std::isfinite(v) && v == std::floor(v) && std::fabs(v) < 9e15)
        return fmt("%lld", (long long)v);
    return fmt("%.15g", std::isfinite(v) ? v : 0.0);
}

// A small writer: key/value lines of one object level.
struct JW {
    std::string o;
    int ind = 0;
    bool first = true;
    void pad() { o.append((size_t)ind * 2, ' '); }
    void sep()
    {
        if (!first)
            o += ",";
        o += "\n";
        first = false;
        pad();
    }
    void key(const char* k)
    {
        sep();
        jsonStr(o, k);
        o += ": ";
    }
    void open(char c)
    {
        o += c;
        ind++;
        first = true;
    }
    void close(char c)
    {
        ind--;
        if (!first) {
            o += "\n";
            pad();
        }
        o += c;
        first = false;
    }
    void kv(const char* k, bool v)
    {
        key(k);
        o += v ? "true" : "false";
    }
    void kv(const char* k, int v)
    {
        key(k);
        o += fmt("%d", v);
    }
    void kv(const char* k, long long v)
    {
        key(k);
        o += fmt("%lld", v);
    }
    void kv(const char* k, double v)
    {
        key(k);
        o += jsonNum(v);
    }
    void kv(const char* k, const std::string& v)
    {
        key(k);
        jsonStr(o, v);
    }
};

std::string settingsToJson(const Settings& s)
{
    JW w;
    w.open('{');
    w.kv("CreditsLockValue", s.creditsLockValue);
    w.kv("CreditsLock", s.creditsLock);
    w.kv("UnlockCareer", s.unlockCareer);
    w.key("PPProfiles");
    w.open('[');
    for (const PPProfile& p : s.ppProfiles) {
        w.sep();
        w.open('{');
        w.kv("GarageId", p.garageId);
        w.kv("CarCode", p.carCode);
        w.kv("Label", p.label);
        w.kv("LockedPP", (double)p.lockedPP);
        w.kv("Enabled", p.enabled);
        w.kv("RealPPWhenSaved", (double)p.realPPWhenSaved);
        w.kv("Saved", p.saved);
        w.close('}');
    }
    w.close(']');
    w.kv("MenuMaster", s.master);
    w.kv("MenuPadBlock", s.padBlock);
    w.kv("MenuUiScale", (double)s.uiScale);
    w.kv("MenuFullHd", s.fullHd);
    w.kv("ScreenRealRes", s.realRes);
    w.kv("ScreenBright", s.screenBright);
    w.kv("ScreenFpsLimit", s.fpsLimit);
    w.kv("CameraLooseChase", s.camLoose);
    w.kv("CameraChaseSpring", s.camSpring);
    w.kv("CameraChaseAngle", s.camAngle);
    w.kv("NetLanMode", s.lanMode);
    w.kv("MenuLang", s.menuLang);
    w.kv("MenuLastPage", s.lastPage);
    w.close('}');
    w.o += "\n";
    return w.o;
}

// Keys that are missing keep the value `s` came in with (the defaults, or what the menu has).
void settingsFromJson(const J& j, Settings& s, bool menuKeys)
{
    auto B = [&](const char* k, bool& v) {
        if (const J* x = j.get(k); x && x->t == J::Bool)
            v = x->b;
    };
    auto I = [&](const char* k, int& v) {
        if (const J* x = j.get(k); x && x->t == J::Num)
            v = (int)std::llround(x->n);
    };
    auto D = [&](const char* k, double& v) {
        if (const J* x = j.get(k); x && x->t == J::Num)
            v = x->n;
    };
    auto S = [&](const char* k, std::string& v) {
        if (const J* x = j.get(k); x && x->t == J::Str)
            v = x->s;
    };
    if (const J* x = j.get("CreditsLockValue"); x && x->t == J::Num)
        s.creditsLockValue = std::llround(x->n);
    B("CreditsLock", s.creditsLock);
    B("UnlockCareer", s.unlockCareer);
    if (const J* x = j.get("PPProfiles"); x && x->t == J::Arr) {
        s.ppProfiles.clear();
        for (const J& e : x->a) {
            if (e.t != J::Obj)
                continue;
            PPProfile p;
            if (const J* v = e.get("GarageId"); v && v->t == J::Num)
                p.garageId = (int)v->n;
            if (const J* v = e.get("CarCode"); v && v->t == J::Num)
                p.carCode = (int)v->n;
            if (const J* v = e.get("Label"); v && v->t == J::Str)
                p.label = v->s;
            if (const J* v = e.get("LockedPP"); v && v->t == J::Num)
                p.lockedPP = (float)v->n;
            if (const J* v = e.get("Enabled"); v && v->t == J::Bool)
                p.enabled = v->b;
            if (const J* v = e.get("RealPPWhenSaved"); v && v->t == J::Num)
                p.realPPWhenSaved = (float)v->n;
            if (const J* v = e.get("Saved"); v && v->t == J::Str)
                p.saved = v->s;
            s.ppProfiles.push_back(p);
        }
    }
    if (menuKeys) {
        B("MenuMaster", s.master);
        B("MenuPadBlock", s.padBlock);
        double sc = s.uiScale;
        D("MenuUiScale", sc);
        s.uiScale = (float)std::clamp(sc, 0.6, 2.0);
        B("MenuFullHd", s.fullHd);
        I("ScreenRealRes", s.realRes);
        s.realRes = std::clamp(s.realRes, 0, 3);
        D("ScreenBright", s.screenBright);
        I("ScreenFpsLimit", s.fpsLimit);
        if (s.fpsLimit < -1) s.fpsLimit = -1;
        else if (s.fpsLimit > 0 && s.fpsLimit < 30) s.fpsLimit = 0;
        else if (s.fpsLimit > 1000) s.fpsLimit = 1000;
        B("CameraLooseChase", s.camLoose);
        D("CameraChaseSpring", s.camSpring);
        s.camSpring = std::clamp(std::isfinite(s.camSpring) ? s.camSpring : 90.0, 0.0, 300.0);
        D("CameraChaseAngle", s.camAngle);
        s.camAngle = std::clamp(std::isfinite(s.camAngle) ? s.camAngle : 35.0, 0.0, 100.0);
        B("NetLanMode", s.lanMode);
        I("MenuLang", s.menuLang);
        s.menuLang = std::clamp(s.menuLang, 0, 2);
        I("MenuLastPage", s.lastPage);
    }
}

std::wstring settingsPath()
{
    if (const char* e = std::getenv("GT6_MENU_SETTINGS"); e && *e)
        return widen(e, CP_ACP);
    return exeDir() + L"\\GT6Menu.settings.json";
}
std::wstring externalProfilePath() { return exeDir() + L"\\trainer\\GT6TrainerNative.profiles.json"; }

// Resolution Full HD (2026-11-20: a higher resolution, up to Full HD). The game sizes its buffers by what the console's
// video output says when it boots, long before the menu's engine runs: main.cpp asks this at its start. It reads
// the settings file as it stands on the disk and nothing else; no file, a broken file or no such key = 720p.
extern "C" int gt6_menu_setting_full_hd(void)
{
    try {
        std::string text;
        J j;
        if (!readFile(settingsPath(), text) || !jsonParse(text, j) || j.t != J::Obj)
            return 0;
        const J* x = j.get("MenuFullHd");
        return x && x->t == J::Bool && x->b ? 1 : 0;
    } catch (...) {
        return 0;
    }
}

// [GT6 0102] Real resolution (the switch under Full HD): 0 = 1440x1080 stretched, 1 = 1920x1080, 2 = 2560x1440,
// 3 = 3840x2160. Read by main.cpp at its start like the one above; no file, a broken file or no such key = 0.
extern "C" int gt6_menu_setting_real_res(void)
{
    try {
        std::string text;
        J j;
        if (!readFile(settingsPath(), text) || !jsonParse(text, j) || j.t != J::Obj)
            return 0;
        const J* x = j.get("ScreenRealRes");
        if (!x || x->t != J::Num) return 0;
        const int v = (int)x->n;
        return v >= 0 && v <= 3 ? v : 0;
    } catch (...) {
        return 0;
    }
}

// Live FPS limit (Settings -> Screen -> FPS limit): 0 = Stock 60 FPS, -1 = Unlimited, 30..1000 = unlocked + host paced.
extern "C" int gt6_menu_setting_fps_limit(void)
{
    try {
        std::string text;
        J j;
        if (!readFile(settingsPath(), text) || !jsonParse(text, j) || j.t != J::Obj)
            return 0;
        const J* x = j.get("ScreenFpsLimit");
        if (!x || x->t != J::Num) return 0;
        const int v = (int)std::llround(x->n);
        if (v < 0) return -1;
        if (v >= 30 && v <= 1000) return v;
        return 0;
    } catch (...) {
        return 0;
    }
}

// [GT6 0114] LAN mode (Settings -> Network; Mabec's GT6 PC build brought it to RPCS3): read by main.cpp at its start like
// the two above - the game takes its hidden "lanmode" boot argument then. No file, a broken file or no such key = off.
extern "C" int gt6_menu_setting_lan_mode(void)
{
    try {
        std::string text;
        J j;
        if (!readFile(settingsPath(), text) || !jsonParse(text, j) || j.t != J::Obj)
            return 0;
        const J* x = j.get("NetLanMode");
        return x && x->t == J::Bool && x->b ? 1 : 0;
    } catch (...) {
        return 0;
    }
}

// The external trainer's profile -> the menu's settings (the menu's own keys stay).
// ---- car names (the bridge caches every name the game shows: P+0x3000 + (code & 0x1FF) * 0x60) ----------------
std::string readCarName(int carCode)
{
    if (carCode <= 0 || !bridgeReady())
        return "";
    const uint8_t* b = vm_base + P + 0x3000 + (uint32_t)(carCode & 0x1FF) * 0x60;
    if ((int)rbe32(b) != carCode)
        return "";
    const void* nul = std::memchr(b + 4, 0, 0x5C);
    std::string s((const char*)b + 4, nul ? (size_t)((const uint8_t*)nul - (b + 4)) : 0x5C);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\n' || s.back() == '\r'))
        s.pop_back();
    size_t lead = 0;
    while (lead < s.size() && s[lead] == ' ')
        lead++;
    return s.substr(lead);
}

// ---- SQL rewrite rules (Gt6.cs) -----------------------------------------------------------------------------
struct SqlRule {
    int mode;   // 0 exact, 1 prefix, 2 substring, 3 substring chained with the next rule, 5 follower only
    std::string pattern, repl;
};
constexpr int SqlMaxRules = 15, SqlRuleSize = 0x200, SqlLogEntries = 64;

bool writeSqlRules(const std::vector<SqlRule>& rules)
{
    for (const SqlRule& r : rules)
        if (r.pattern.size() > 0xFB || r.repl.size() > 0xFF)
            return false;
    wp32(P2 + 0, 0);   // entries first, count last: the hook never sees a half-written rule
    const int n = std::min((int)rules.size(), SqlMaxRules);
    for (int i = 0; i < n; i++) {
        uint8_t blob[SqlRuleSize] = {};
        be32(blob, (uint32_t)rules[i].mode);
        std::memcpy(blob + 4, rules[i].pattern.data(), rules[i].pattern.size());
        std::memcpy(blob + 0x100, rules[i].repl.data(), rules[i].repl.size());
        std::memcpy(vm_base + P2 + 0x10 + (uint32_t)i * SqlRuleSize, blob, SqlRuleSize);
    }
    wp32(P2 + 0, (uint32_t)n);
    return true;
}

const SqlRule kCareerFolderCleared = {0, "SELECT result, event_id FROM t_event_record WHERE folder_id = ?", "SELECT 0 AS result, ?1 AS event_id"};
const char kManyGold[] = "WITH RECURSIVE c(x) AS (SELECT 1 UNION ALL SELECT x+1 FROM c WHERE x<300) SELECT 0 AS result FROM c";
const SqlRule kLicenceResults = {1, "SELECT result FROM t_event_record WHERE (folder_id IN (", kManyGold};
const SqlRule kMedalResults = {1, "SELECT result FROM t_event_record WHERE 0 <= result AND", kManyGold};
// Stars: every star total the career screen asks for -> 9999, so the folders and categories that need stars open and
// the tracks show their stars. In the external trainer this rule stood in sql_rules.txt beside its exe; the package
// of the in-game trainer had no such file, and a player's stars were gone (2026-11-18). ?1..?3 keep the game's
// parameter bindings valid.
const SqlRule kStarTotals = {1, "SELECT COALESCE(SUM(star_num), 0) FROM t_event_record", "SELECT 9999+0*COALESCE(?1,?2,?3,0)"};

// ---- credits (Gt6.cs) -----------------------------------------------------------------------------------------
namespace cash {
constexpr uint32_t TagStatic = 0x016CD0C8, Off = 0x28;
constexpr long long Max = 99999999999LL;

uint32_t find()
{
    uint8_t tag[8];
    if (!rd(TagStatic, tag, 8))
        return 0;
    const uint32_t hi = rbe32(tag), lo = rbe32(tag + 4);
    if (!hi && !lo)
        return 0;
    // the word looked for is the tag's first non-zero half
    const uint32_t word = hi ? hi : lo, shift = hi ? 0 : 4;
    uint32_t found = 0;
    scanWords(&word, 1, 0x10000u, [&](int, uint32_t a, const uint8_t* at, size_t avail) {
        if (found || a < shift)
            return;
        const uint32_t g = a - shift;
        if ((g & 7) || g == TagStatic)
            return;
        uint8_t obj[0x30];
        if (shift == 0 && avail >= 0x30)
            std::memcpy(obj, at, 0x30);
        else if (!rd(g, obj, 0x30))
            return;
        if (std::memcmp(obj, tag, 8))
            return;
        const long long v = (long long)((uint64_t)rbe32(obj + Off) << 32 | rbe32(obj + Off + 4));
        if (v >= 0 && v <= Max)
            found = g;
    });
    return found;
}

std::optional<long long> read(uint32_t obj)
{
    uint8_t tag[8], hdr[8], b[8];
    if ((uint64_t)obj + Off + 8 > 0x100000000ull || !rd(TagStatic, tag, 8) || !rd(obj, hdr, 8))
        return std::nullopt;
    if ((!rbe32(tag) && !rbe32(tag + 4)) || std::memcmp(tag, hdr, 8))
        return std::nullopt;   // object moved / freed
    if (!rd(obj + Off, b, 8))
        return std::nullopt;
    const long long v = (long long)((uint64_t)rbe32(b) << 32 | rbe32(b + 4));
    return v >= 0 && v <= Max ? std::optional<long long>(v) : std::nullopt;
}

// (An earlier answer to public issue #8, [GT6 0115], also wrote a 64-bit word in the game's heap that held the old value
// between two heap pointers, taking it for the copy the shop reads. It was not: test cs1 found such a word once and no
// more of them fifteen seconds later - a freed temporary of the script engine. That write is gone; the real cause is the
// game's bank book, gt6_menu_bank_book below.)
bool write(uint32_t obj, long long v)
{
    const auto before = read(obj);
    if (v < 0 || v > Max || !before)
        return false;
    return w64(obj + Off, (uint64_t)v);
}

// The object the engine found (0 = none yet), for gt6_menu_bank_book: that one runs on the game's own thread.
std::atomic<uint32_t> g_shared{0};

// [GT6 0108] (tests, GT6_CASH_LOG=1) A player: credits set in the menu showed in the game, but the parts shop said
// "Insufficient credits", the lock did not help either. Every object with the tag (find() takes the first), and every
// place the value is held as a 64- or 32-bit word, with the words around it. GT6_CASH_TEST=<n>: the value is set once,
// and three seconds later both the old and the new value are looked for again - a copy that kept the old value is what
// the shop may read.
void scanValue(long long v, const char* label)
{
    const uint32_t lo = (uint32_t)v, hi = (uint32_t)((uint64_t)v >> 32);
    int m = 0;
    scanWords(&lo, 1, 0x10000u, [&](int, uint32_t a, const uint8_t*, size_t) {
        uint8_t w[16];
        if (a < 0x10008u || !rd(a - 8, w, 16) || m++ >= 60)
            return;
        fprintf(stderr, "[cash] %s %lld at %08X as a %s word: %08X %08X [%08X] %08X\n", label, v, a,
                rbe32(w + 4) == hi ? "64-bit" : "32-bit", rbe32(w), rbe32(w + 4), rbe32(w + 8), rbe32(w + 12));
    });
    fprintf(stderr, "[cash] %s %lld: %d place(s)\n", label, v, m);
}

void diag(uint32_t obj)
{
    uint8_t tag[8];
    const auto v = read(obj);
    if (!v || !rd(TagStatic, tag, 8))
        return;
    const uint32_t hi = rbe32(tag), lo = rbe32(tag + 4), word = hi ? hi : lo, shift = hi ? 0 : 4;
    int n = 0;
    scanWords(&word, 1, 0x10000u, [&](int, uint32_t a, const uint8_t*, size_t) {
        uint8_t o[0x30];
        if (a < shift)
            return;
        const uint32_t g = a - shift;
        if ((g & 7) || g == TagStatic || !rd(g, o, 0x30) || std::memcmp(o, tag, 8) || n++ >= 40)
            return;
        fprintf(stderr, "[cash] object with the tag at %08X: %lld%s\n", g,
                (long long)((uint64_t)rbe32(o + Off) << 32 | rbe32(o + Off + 4)), g == obj ? " (the menu's)" : "");
    });
    scanValue(*v, "value");
}

// The header of a bank book in `b` (the game's script structure BankBook: version 101, then the amount before, the
// change, the amount after and the time as 64-bit words, the place in its history and the number of times that
// history went round as 32-bit words): 1 = these bytes are one.
bool isBook(const uint8_t* b, long long* prev, long long* diff, long long* cur)
{
    auto q = [&](int o) { return (long long)((uint64_t)rbe32(b + o) << 32 | rbe32(b + o + 4)); };
    if (q(0) != 101)
        return false;
    *prev = q(8); *diff = q(16); *cur = q(24);
    const long long when = q(32);
    return *prev >= 0 && *prev <= Max && *cur >= 0 && *cur <= Max && *diff >= -Max && *diff <= Max && when > 0 &&
           rbe32(b + 40) < 256u && rbe32(b + 44) < 0x100000u;
}

// (tests, GT6_CASH_LOG) The bank book as the game holds it in memory behind the credits object: a line when it changes.
void bookLog(uint32_t obj)
{
    static std::vector<uint8_t> buf(0x10000);
    static uint8_t last[0x30];
    static uint64_t next = 0;
    if (GetTickCount64() < next)
        return;
    next = GetTickCount64() + 1000;
    for (size_t o = 0; o < buf.size(); o += 0x1000)          // page by page: a page that cannot be read stays zero
        if (!rd(obj + (uint32_t)o, buf.data() + o, 0x1000))
            std::memset(buf.data() + o, 0, 0x1000);
    for (size_t o = 0; o + 0x30 <= buf.size(); o += 8) {
        long long prev, diff, cur;
        if (!isBook(buf.data() + o, &prev, &diff, &cur))
            continue;
        if (std::memcmp(last, buf.data() + o, 0x30)) {
            std::memcpy(last, buf.data() + o, 0x30);
            const auto v = read(obj);
            fprintf(stderr, "[cash] bank book in memory at %08X: before %lld, change %lld, after %lld, place %u (credits now %lld)\n",
                    obj + (uint32_t)o, prev, diff, cur, rbe32(buf.data() + o + 40), v ? *v : -1LL);
        }
        return;
    }
}

void testStep(uint32_t obj)
{
    static const bool on = getenv("GT6_CASH_LOG") != nullptr;
    static long long before = -1, after = -1;
    static uint64_t again = 0, foundAt = 0;
    if (!on || !obj)
        return;
    bookLog(obj);
    // GT6_CASH_TEST_AFTER=<s>: the first look and the write that long after the object was found (not while a save loads)
    if (!foundAt)
        foundAt = GetTickCount64();
    if (const char* wait = getenv("GT6_CASH_TEST_AFTER"); wait && GetTickCount64() < foundAt + 1000ull * (uint64_t)std::atoi(wait))
        return;
    if (before < 0) {
        diag(obj);
        const auto v = read(obj);
        before = v ? *v : 0;
        const char* t = getenv("GT6_CASH_TEST");
        if (t && *t && v) {
            after = std::clamp(std::atoll(t), 0LL, 2147483647LL);
            fprintf(stderr, "[cash] test: %lld -> %lld: %s\n", before, after, write(obj, after) ? "written" : "NOT written");
            again = GetTickCount64() + 3000;
        }
        return;
    }
    if (again && GetTickCount64() >= again) {
        again = 0;
        scanValue(before, "old value");
        scanValue(after, "new value");
        diag(obj);
    }
}
}  // namespace cash

// [GT6 0120] THE GAME'S BANK BOOK (public issue #8: credits set in the menu showed in the game, the parts shop said
// "Insufficient credits", the lock did not help). Next to the credits the game keeps a "bank book" - the amount before,
// the change and the amount after of everything it paid out or took itself - sealed with its own cipher. Before it takes
// a payment it opens the sealed book, and when the credits are more than the book's amount it sets them back to that
// amount. What the game adds itself (a prize) goes into the book without that look, which is why a value set in the menu
// did work once a prize had been paid. The game bridge calls this where the cipher has just opened a sealed block (site
// 0x009E66B0, on the game's own thread): `plain` are the opened bytes in guest memory. When they are a bank book whose
// amount is below the credits the menu's object holds, the amount becomes the credits; the game's own look then passes,
// and it writes the payment into the book and seals it as it always does. Nothing else of the block is touched, and a
// block that is not a bank book is left alone. GT6_BANK_BOOK=0 = the book is left as it is (as before);
// GT6_CASH_LOG=1: a line for every block the cipher opens.
// Tests e6 / e7 (credits held at 48,144,100 by the lock, the book at 47,548,950, a car wash for 50 at Pit Service): with
// GT6_BANK_BOOK=0 the game's book afterwards read "before 47,548,950, change -50, after 47,548,900" - it had set the
// credits back to its own amount and charged from there; with this it read "before 48,144,100, change -50, after
// 48,144,050".
extern "C" void gt6_menu_bank_book(uint32_t plain, uint32_t size)
{
    static const bool log = getenv("GT6_CASH_LOG") != nullptr;
    static const bool off = [] { const char* e = getenv("GT6_BANK_BOOK"); return e && *e == '0'; }();
    static std::atomic<int> said{0};
    uint8_t h[0x130];
    if (off || size < 0x40u || size > 0x1000000u)
        return;
    const uint32_t look = size < sizeof h ? size : (uint32_t)sizeof h;
    if (!rd(plain, h, look))
        return;
    if (log && said.load() < 24) {
        ++said;
        fprintf(stderr, "[cash] the cipher opened %u bytes at %08X: %08X %08X %08X %08X %08X %08X %08X %08X\n", size, plain, rbe32(h),
                rbe32(h + 4), rbe32(h + 8), rbe32(h + 12), rbe32(h + 16), rbe32(h + 20), rbe32(h + 24), rbe32(h + 28));
        // (tests) GT6_CASH_DUMP=<file>: the first opened block as it is
        if (const char* dump = getenv("GT6_CASH_DUMP"); dump && *dump && said.load() == 1) {
            std::vector<uint8_t> all(size);
            if (rd(plain, all.data(), size))
                if (FILE* f = fopen(dump, "wb")) { fwrite(all.data(), 1, size, f); fclose(f); }
        }
    }
    // The sealed book is the script structure written member by member (seen in test e2, 16384 bytes): 0E <length> 09 <11
    // members>, then per member 07 <its number> <type> <value>, type 04 = a 64-bit value. Members 0 .. 3 are the amount
    // before, the version (101), the amount after and the change.
    auto q = [&](int o) { return (long long)((uint64_t)rbe32(h + o) << 32 | rbe32(h + o + 4)); };
    auto member = [&](int o, int number) { return h[o] == 0x07 && h[o + 1] == number && h[o + 2] == 0x04; };
    if (h[0] != 0x0E || h[5] != 0x09 || rbe32(h + 6) != 11u || !member(0x0A, 0) || !member(0x15, 1) || !member(0x20, 2) ||
        !member(0x2B, 3) || q(0x18) != 101)
        return;
    const long long prev = q(0x0D), cur = q(0x23), diff = q(0x2E);
    if (prev < 0 || prev > cash::Max || cur < 0 || cur > cash::Max || diff < -cash::Max || diff > cash::Max)
        return;
    const uint32_t obj = cash::g_shared.load();
    const auto have = obj ? cash::read(obj) : std::nullopt;
    if (!have) {
        if (log) fprintf(stderr, "[cash] the bank book (amount %lld) was opened, the credits object is not known: left alone\n", cur);
        return;
    }
    if (*have > cur && w64(plain + 0x23, (uint64_t)*have))
        fprintf(stderr, "[cash] the game's bank book said %lld and the credits are %lld: the book now says %lld\n", cur, *have, *have);
    else if (log)
        fprintf(stderr, "[cash] the game's bank book says %lld, the credits are %lld: nothing to do\n", cur, *have);
}


// ---- the engine: MainForm's ticks ----------------------------------------------------------------------------
struct Recent {
    int code;
    float pp, ppTire;
    uint64_t seen;
};

struct Engine {
    enum Mode { Idle, Running, Stopping, Yield } mode = Idle;
    uint32_t mine = 0, lastForeign = 0;   // the heartbeat this engine wrote last / the other trainer's
    uint64_t foreignAt = 0, stopAt = 0;
    Status st;
    int inRace = -1;
    uint32_t cashObj = 0;
    uint64_t nextCashSearch = 0, nextNote = 0, lastSave = 0;
    std::map<int, Recent> recent;
    // SQL
    std::vector<SqlRule> fileRules;
    uint64_t fileRulesStamp = ~0ull;
    int rulesOverflow = 0, rulesCount = 0;

    // ---- settings ---------------------------------------------------------------------------------------------
    template <class F>
    void withSettings(F&& f)
    {
        {
            std::lock_guard<std::mutex> lk(g_mx);
            f(g_set);
        }
        g_dirty = true;
    }

    void loadSettings()
    {
        Settings s;
        std::string text;
        J j;
        bool have = false;
        if (readFile(settingsPath(), text) && jsonParse(text, j) && j.t == J::Obj) {
            settingsFromJson(j, s, true);
            have = true;
        } else if (!text.empty()) {
            // unreadable: keep the file for the player, start from the defaults
            CopyFileW(settingsPath().c_str(), (settingsPath() + L".broken").c_str(), FALSE);
            toast(T_("설정 파일을 읽을 수 없어 따로 보관하고 기본값으로 시작합니다"), true);
        }
        {
            std::lock_guard<std::mutex> lk(g_mx);
            g_set = s;
        }
        logf("settings: %s, menu %s", have ? "read" : "defaults", s.master ? "on" : "off");
    }

    void saveIfDirty()
    {
        const uint64_t now = GetTickCount64();
        if (!g_dirty || now - lastSave < 300)
            return;
        g_dirty = false;
        lastSave = now;
        std::string text;
        {
            std::lock_guard<std::mutex> lk(g_mx);
            text = settingsToJson(g_set);
        }
        if (!writeFileAtomic(settingsPath(), text, true))
            toast(T_("설정을 저장하지 못했습니다"), true);
    }


    // ---- who holds the heartbeat --------------------------------------------------------------------------
    void resetModules()
    {
        inRace = -1;
        cashObj = 0;
        recent.clear();
    }

    // The values in my car go back (the features' own Restore), through the race gate while this engine's
    // heartbeat still stands; `direct` = at once, when another trainer has just taken the heartbeat.
    void restoreSession(bool direct)
    {
    }

    void control(uint64_t now, const Settings& s)
    {
        const uint32_t cur = p32(P + 0x44);
        if (mode == Yield) {
            if (cur != lastForeign) {
                lastForeign = cur;
                foreignAt = now;
            }
            if (now - foreignAt < (cur ? 3000u : 600u))
                return;
            mode = Idle;
            mine = 0;
            logf("the external trainer's heartbeat stopped: the built-in trainer takes over again");
        }
        if (cur != mine) {
            // the external trainer attached: it reads the car next, so my live values go back first
            if (mode == Running || mode == Stopping)
                restoreSession(true);
            resetModules();
            mode = Yield;
            lastForeign = cur;
            foreignAt = now;
            mine = 0;
            logf("another trainer's heartbeat (%08X): the built-in trainer stands back", cur);
            return;
        }
        if (mode == Idle && s.master) {
            // as the external trainer's attach: old controls dormant until this session has published its own
            wp32(P + 0x38, 0);
            g_rulesDirty = true;
            nextCashSearch = 0;
            mode = Running;
            logf("the built-in trainer is running");
        } else if (mode == Running && !s.master) {
            restoreSession(false);
            wp32(P + 0x38, 0);
            wp32(P + 0x00, 0);
            wp32(P + 0x24, 0);
            wp32(P + 0x28, 0);
            wp32(P + 0x30, 0);
            wp32(P + 0x20, 0);
            wp32(P2 + 0, 0);
            wp32(P2 + 4, 0);
            mode = Stopping;
            stopAt = now;
        } else if (mode == Stopping) {
            if (s.master)
                mode = Running;
            else if (now - stopAt >= 400) {   // the gate has served the values that went back
                wp32(P + 0x44, 0);
                mine = 0;
                resetModules();
                mode = Idle;
                logf("the built-in trainer is off");
            }
        }
    }

    void heartbeat()
    {
        uint32_t tick = (uint32_t)GetTickCount64();
        if (!tick)
            tick = 1;
        wp32(P + 0x44, tick);
        mine = tick;
    }

    // ---- SQL ------------------------------------------------------------------------------------------------

    // extra rules from sql_rules.txt beside the game (or in trainer\): "exact|pattern|replacement", "prefix|..", "sub|.."
    void loadFileRules()
    {
        std::wstring path = exeDir() + L"\\sql_rules.txt";
        if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
            path = exeDir() + L"\\trainer\\sql_rules.txt";
        WIN32_FILE_ATTRIBUTE_DATA fa;
        uint64_t stamp = 0;
        if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa))
            stamp = (uint64_t)fa.ftLastWriteTime.dwHighDateTime << 32 | fa.ftLastWriteTime.dwLowDateTime;
        if (stamp == fileRulesStamp)
            return;
        fileRulesStamp = stamp;
        fileRules.clear();
        std::string text;
        if (stamp && readFile(path, text)) {
            size_t pos = 0;
            while (pos < text.size()) {
                size_t nl = text.find('\n', pos);
                if (nl == std::string::npos)
                    nl = text.size();
                std::string line = text.substr(pos, nl - pos);
                pos = nl + 1;
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                if (line.empty() || line[0] == '#')
                    continue;
                const size_t a = line.find('|'), b = a == std::string::npos ? a : line.find('|', a + 1);
                if (b == std::string::npos || line.find('|', b + 1) != std::string::npos)
                    continue;
                const std::string kind = line.substr(0, a);
                if (kind == "exact" || kind == "prefix" || kind == "sub")
                    fileRules.push_back({kind == "exact" ? 0 : kind == "prefix" ? 1 : 2, line.substr(a + 1, b - a - 1), line.substr(b + 1)});
            }
        }
        g_rulesDirty = true;
    }

    std::vector<SqlRule> desiredSqlRules(const Settings& s)
    {
        std::vector<SqlRule> r;
        if (s.unlockCareer) {
            r.push_back(kCareerFolderCleared);
            r.push_back(kLicenceResults);
            r.push_back(kMedalResults);
            r.push_back(kStarTotals);
            // sql_rules.txt: the player's own additions; a line for a statement that has a rule above is left out
            // (the hook takes the first rule that fits, and a doubled rule only uses up one of the 15 places)
            for (const SqlRule& f : fileRules) {
                bool have = false;
                for (const SqlRule& b : r)
                    have |= b.mode == f.mode && b.pattern == f.pattern;
                if (!have)
                    r.push_back(f);
            }
        }
        rulesOverflow = std::max(0, (int)r.size() - SqlMaxRules);
        if ((int)r.size() > SqlMaxRules)
            r.resize(SqlMaxRules);
        return r;
    }

    // ---- PP -------------------------------------------------------------------------------------------------
    void writePPTable(const Settings& s)
    {
        wp32(P + 0x20, 0);   // entries first, the count last: the hook never reads half a table
        uint32_t n = 0;
        for (const PPProfile& p : s.ppProfiles) {
            if (!p.enabled || p.garageId <= 0 || n >= 512)
                continue;
            const uint32_t a = P + 0x1000 + n * 16;
            wp32(a, (uint32_t)p.garageId);
            wpf32(a + 4, p.lockedPP / 1000.0f);
            wpf32(a + 8, p.lockedPP / 1000.0f);
            wp32(a + 12, 0);
            n++;
        }
        wp32(P + 0x20, n);
    }


    // ---- commands of the menu ---------------------------------------------------------------------------------
    void runCommand(Settings& s, const Command& c)
    {
        if (mode != Running) {
            toast(mode == Yield ? T_("외장 트레이너가 연결되어 있어 내장 트레이너는 쉬는 중입니다") : T_("트레이너가 꺼져 있습니다 (왼쪽 아래 스위치)"), true);
            return;
        }
        switch (c.kind) {
        case Command::SetCashOnce:
            if (!cashObj)
                toast(T_("크레딧 위치를 아직 못 찾았습니다 (세이브를 불러온 뒤 다시)."), true);
            else if (cash::write(cashObj, c.a))
                toast(fmt(T_("크레딧을 %lld 로 바꿨습니다"), c.a));
            else
                toast(T_("크레딧을 쓰지 못했습니다"), true);
            break;
        case Command::FillNames:
            fillNames(true);
            break;
        default:
            break;
        }
    }

    // Empty / default labels of the PP locks get the name the game showed (all = typed ones too).
    void fillNames(bool all)
    {
        bool changed = false;
        std::lock_guard<std::mutex> lk(g_mx);
        for (PPProfile& p : g_set.ppProfiles) {
            const bool def = p.label.empty() || p.label == fmt(T_("차 %d"), p.carCode) || p.label == fmt("car %d", p.carCode);
            if (!all && !def)
                continue;
            const std::string n = readCarName(p.carCode);
            if (!n.empty() && n != p.label) {
                p.label = n;
                changed = true;
            }
        }
        if (changed)
            g_dirty = true;
    }

    // ---- 100 ms -----------------------------------------------------------------------------------------------
    void fastTick(const Settings& s, unsigned pad)
    {
        // credits lock
        if (cashObj) {
            const auto cur = cash::read(cashObj);
            if (!cur)
                cashObj = 0;
            else if (s.creditsLock && *cur != s.creditsLockValue)
                cash::write(cashObj, std::clamp(s.creditsLockValue, 0LL, cash::Max));
        }
        const uint32_t flags = s.unlockCareer ? 8u : 0u;
        if (p32(P) != flags)
            wp32(P, flags);
        {
            const auto rules = desiredSqlRules(s);
            if (p32(P2) != (uint32_t)rules.size() || g_rulesDirty.exchange(false)) {
                if (!writeSqlRules(rules))
                    toast(T_("SQL 규칙이 너무 깁니다"), true);
                rulesCount = (int)rules.size();
                if (g_log) {
                    std::string names;
                    for (const SqlRule& r : rules)
                        names += " [" + r.pattern.substr(0, 46) + "]";
                    logf("sql rules: %d written:%s", (int)rules.size(), names.c_str());
                }
            }
        }
        // the cars whose PP the game read (the hooks leave them in the page)
        {
            const int gid = (int)p32(P + 0x10);
            const uint32_t car = p32(P + 0x18);
            if (gid > 0 && car) {
                const int code = (int)u32z(car + 0x7C);
                if (code > 0 && code <= MaxCarCode && gid <= MaxGarageId)
                    recent[gid] = {code, pf32(P + 0x14) * 1000.0f, pf32(P + 0x1C) * 1000.0f, GetTickCount64()};
            }
        }
        heartbeat();
    }

    // ---- 500 ms -----------------------------------------------------------------------------------------------
    void slowTick(Settings& s, unsigned pad)
    {
        const uint64_t now = GetTickCount64();
        if (!cashObj && now >= nextCashSearch) {
            cashObj = cash::find();
            nextCashSearch = GetTickCount64() + 5000;
        }
        cash::g_shared.store(cashObj);   // [GT6 0120] for the bank book look on the game's thread
        cash::testStep(cashObj);   // [GT6 0108] tests only (GT6_CASH_LOG)
        loadFileRules();
        writePPTable(s);
        fillNames(false);
    }

    void publish(const Settings& s)
    {
        st.supported = true;
        st.external = mode == Yield;
        st.running = mode == Running;
        st.inRace = mode == Running ? inRace : -1;
        const auto cur = cashObj ? cash::read(cashObj) : std::nullopt;
        st.cashFound = cur.has_value();
        st.cash = cur.value_or(0);
        st.rulesOverflow = rulesOverflow;
        st.rulesCount = rulesCount;
        st.recent.clear();
        for (const auto& kv : recent)
            st.recent.push_back({kv.first, kv.second.code, kv.second.pp, kv.second.ppTire, readCarName(kv.second.code), kv.second.seen});
        std::sort(st.recent.begin(), st.recent.end(), [](const RecentCar& a, const RecentCar& b) { return a.seen > b.seen; });
        if (st.recent.size() > 15)
            st.recent.resize(15);
        st.lastScanMs = g_scan.ms;
        st.scans = g_scan.scans;
        {
            std::lock_guard<std::mutex> lk(g_toastMx);
            st.toastSeq = g_toastSeq;
            st.toast = g_toastText;
            st.toastWarn = g_toastWarn;
        }
        std::lock_guard<std::mutex> lk(g_stMx);
        g_status = st;
    }

    void publishUnsupported()
    {
        Status u;
        u.supported = false;
        {
            std::lock_guard<std::mutex> lk(g_toastMx);
            u.toastSeq = g_toastSeq;
            u.toast = g_toastText;
            u.toastWarn = g_toastWarn;
        }
        std::lock_guard<std::mutex> lk(g_stMx);
        g_status = u;
    }

    static unsigned padNow()
    {
        if (g_captured)
            return 0;   // the menu has the pad
        unsigned p = 0;
        for (auto& a : g_padPort)
            p |= a.load(std::memory_order_relaxed);
        return p;
    }


    void run()
    {
        loadSettings();
        int lastAppliedFps = -999;
        {
            const char* e1 = std::getenv("GT6_PATCH_UNLOCK_FPS");
            const char* e2 = std::getenv("LD_PRESENT_MAX_FPS");
            const char* e3 = std::getenv("GT6_RENDER_INTERVAL");
            if ((e1 && *e1) || (e2 && *e2) || (e3 && *e3)) {
                std::lock_guard<std::mutex> lk(g_mx);
                lastAppliedFps = g_set.fpsLimit;   /* keep env override until user changes it in F2 */
            }
        }
        uint64_t lastFast = 0, lastSlow = 0;
        for (;;) {
            Sleep(25);
            if (!bridgeReady()) {
                publishUnsupported();
                saveIfDirty();
                Sleep(200);
                continue;
            }
            const uint64_t now = GetTickCount64();
            if (now - lastFast < 100) {
                continue;
            }
            lastFast = now;
            Settings s;
            {
                std::lock_guard<std::mutex> lk(g_mx);
                s = g_set;
            }
            // [GT6 0101] Brightness (Settings -> Display), live: the presenter draws the picture as out = in^(1 / b)
            rsx_live_draw_present_gamma = (float)(1.0 / std::clamp(std::isfinite(s.screenBright) ? s.screenBright : 1.0, 0.5, 2.0));
            if (s.fpsLimit != lastAppliedFps) {
                lastAppliedFps = s.fpsLimit;
                g_gt6_unlock_fps = (s.fpsLimit != 0) ? 1 : 0;
                rsx_live_draw_present_max_fps = s.fpsLimit;
            }
            cameraTick(s);
            control(now, s);
            std::vector<Command> cmds;
            {
                std::lock_guard<std::mutex> lk(g_cmdMx);
                cmds.swap(g_cmds);
            }
            for (const Command& c : cmds)
                runCommand(s, c);
            if (mode == Running) {
                const unsigned pad = padNow();
                fastTick(s, pad);
                if (now - lastSlow >= 500) {
                    lastSlow = now;
                    slowTick(s, pad);
                }
            }
            publish(s);
            saveIfDirty();
        }
    }
} engine;

DWORD WINAPI engineThread(LPVOID)
{
    engine.run();
    return 0;
}

}  // namespace

// ---- the menu's side ---------------------------------------------------------------------------------------------
std::mutex& mx() { return g_mx; }
Settings& settings() { return g_set; }
void markDirty() { g_dirty = true; }
void rulesDirty() { g_rulesDirty = true; }

Status status()
{
    std::lock_guard<std::mutex> lk(g_stMx);
    return g_status;
}

void post(Command c)
{
    std::lock_guard<std::mutex> lk(g_cmdMx);
    if (g_cmds.size() < 64)
        g_cmds.push_back(std::move(c));
}


std::vector<std::pair<int, std::string>> carNames()
{
    std::vector<std::pair<int, std::string>> out;
    if (!bridgeReady())
        return out;
    for (int i = 0; i < 512; i++) {
        const int code = (int)rbe32(vm_base + P + 0x3000 + (uint32_t)i * 0x60);
        if (code <= 0 || code > 100000)
            continue;
        std::string n = readCarName(code);
        if (!n.empty())
            out.push_back({code, std::move(n)});
    }
    return out;
}

std::string carName(int code) { return readCarName(code); }

void toast(const std::string& text, bool warn)
{
    {
        std::lock_guard<std::mutex> lk(g_toastMx);
        g_toastSeq++;
        g_toastText = text;
        g_toastWarn = warn;
        g_toastSeqA = g_toastSeq;
    }
    std::fprintf(stderr, "[menu] %s%s\n", warn ? "! " : "", text.c_str());
}

unsigned toastSeq() { return g_toastSeqA.load(std::memory_order_relaxed); }


void padSample(unsigned port, unsigned buttons)
{
    if (port < 4)
        g_padPort[port].store(buttons, std::memory_order_relaxed);
}
void menuCaptures(bool on) { g_captured = on; }

void engineStart()
{
    const char* lg = std::getenv("GT6_MENU_LOG");
    g_log = lg && *lg == '1';
    const HANDLE h = CreateThread(nullptr, 0, engineThread, nullptr, 0, nullptr);
    if (h) {
        SetThreadDescription(h, L"gt6-menu-trainer");
        CloseHandle(h);
    }
}

}  // namespace gt6menu
