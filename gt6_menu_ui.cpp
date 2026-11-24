// gt6_menu_ui.cpp - the F2 menu of the trainer inside the game (see gt6_menu.h).
//
// F2 (or L3+R3 on a pad) opens and closes it; Dear ImGui draws it over the game's back buffer right before the
// present (rsx_live_draw_overlay_draw), as the Sakatsuku port's menu does. While it is open the window's keyboard
// and mouse are the menu's (Esc closes the menu, not the game) and, unless that is switched off in Settings, the game
// gets a neutral pad: d-pad moves, X chooses, O closes, L1/R1 change the page.
// GT6_TRAINER_MENU=0: nothing of this is installed. Tests: GT6_MENU_OPEN=1, GT6_MENU_PAGE=<n>,
// GT6_MENU_SHOT=<prefix> + GT6_MENU_SHOT_SEC=<s> (every page to <prefix>_p<n><a|b|c>.bmp at that time).
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d12.h>

#include "imgui.h"
#include "imgui_internal.h"
#include "backends/imgui_impl_dx12.h"

#include "gt6_menu.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

extern "C" {
extern int (*rsx_window_message_hook)(void* hwnd, unsigned msg, uintptr_t wp, intptr_t lp);
extern int (*rsx_live_draw_overlay_on)(void);
extern void (*rsx_live_draw_overlay_draw)(void* dev, void* queue, void* list, void* backbuffer, size_t rtv, unsigned width,
                                          unsigned height);
extern int (*cellPad_overlay_hook)(unsigned port, unsigned buttons);
extern float g_gt6_aspect;
extern float rsx_live_draw_present_aspect;
extern float rsx_window_aspect;
extern int rsx_live_draw_present_stretch;
int gt6_res_1080(void);     /* main.cpp: 1 = this run is in the game's 1080p mode */
int gt6_real_res(void);     /* main.cpp [GT6 0102]: this run's real resolution (Settings::realRes, 0 = off) */
}

using namespace gt6menu;

namespace {

// ---- state shared between the window thread, the game's pad reads and the render thread ---------------------
std::atomic<bool> g_open{false}, g_padBlock{true}, g_typing{false};
std::atomic<unsigned> g_padPorts[4];
std::atomic<int> g_clientW{0}, g_clientH{0};
std::atomic<int> g_bind{-1};   // the order that waits for its key (Key Bindings)
struct Msg {
    unsigned msg;
    uintptr_t wp;
    intptr_t lp;
};
std::mutex g_msgMutex;
std::vector<Msg> g_msgs;

std::string sfmt(const char* f, ...)
{
    char b[1024];
    va_list ap;
    va_start(ap, f);
    std::vsnprintf(b, sizeof b, f, ap);
    va_end(ap);
    return b;
}

void setOpen(bool open)
{
    if (g_open.exchange(open) != open)
        std::fprintf(stderr, "[menu] %s\n", open ? "opened" : "closed");
    if (!open)
        g_bind = -1;
    menuCaptures(open && g_padBlock);
}


int windowHook(void* hwnd, unsigned msg, uintptr_t wp, intptr_t lp)
{
    const bool down = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN, up = msg == WM_KEYUP || msg == WM_SYSKEYUP;
    const bool repeat = (lp & (1 << 30)) != 0;
    if (down && wp == VK_F2) {
        if (!repeat)
            setOpen(!g_open);
        return 1;
    }
    // Esc closes the menu - and, without the menu, the game (the window's own handler). A second Esc right after
    // the one that closed the menu, and that key's repeats and release, stay with the menu.
    static ULONGLONG escGuardUntil = 0;
    if ((down || up) && wp == VK_ESCAPE && !g_open && GetTickCount64() < escGuardUntil)
        return 1;
    if (!g_open)
        return 0;
    switch (msg) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if (wp == VK_ESCAPE) {
            setOpen(false);
            escGuardUntil = GetTickCount64() + 1500;
            return 1;
        }
        break;
    case WM_KEYUP: case WM_SYSKEYUP: case WM_CHAR: case WM_IME_CHAR:
    case WM_MOUSEMOVE: case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MOUSEWHEEL:
        break;
    default:
        return 0;
    }
    RECT rc;
    if (GetClientRect((HWND)hwnd, &rc)) {
        g_clientW = rc.right - rc.left;
        g_clientH = rc.bottom - rc.top;
    }
    std::lock_guard<std::mutex> lock(g_msgMutex);
    if (g_msgs.size() < 512)
        g_msgs.push_back({msg, wp, lp});
    return 1;
}

int padHook(unsigned port, unsigned buttons)
{
    padSample(port, buttons);
    if (port < 4) {
        g_padPorts[port] = buttons;
        static bool held[4];   // L3 (0x02) + R3 (0x04) together: open / close
        const bool now = (buttons & 0x6u) == 0x6u;
        if (now && !held[port])
            setOpen(!g_open);
        held[port] = now;
    }
    return g_open && g_padBlock ? 1 : 0;
}

// ---- look ---------------------------------------------------------------------------------------------------------
const ImVec4 kText(0.91f, 0.93f, 0.96f, 1.0f), kMuted(0.58f, 0.62f, 0.69f, 1.0f), kFaint(0.40f, 0.44f, 0.51f, 1.0f);
const ImVec4 kAccent(0.31f, 0.62f, 1.00f, 1.0f), kGood(0.25f, 0.80f, 0.57f, 1.0f), kWarn(0.96f, 0.72f, 0.28f, 1.0f), kBad(1.00f, 0.43f, 0.43f, 1.0f);
const ImVec4 kWin(0.060f, 0.071f, 0.092f, 0.975f), kSide(0.040f, 0.048f, 0.064f, 0.985f), kCard(0.094f, 0.110f, 0.139f, 1.0f);
const ImVec4 kLine(1.0f, 1.0f, 1.0f, 0.075f), kTrack(0.170f, 0.194f, 0.236f, 1.0f), kInset(0.050f, 0.060f, 0.079f, 1.0f);

struct Ui {
    float S = 0.0f;   // pixels per design unit (the back buffer's height / 1080 x the player's zoom)
    ImFont* reg = nullptr;
    ImFont* bold = nullptr;
    int page = 0, wantPage = -1;
    bool dirty = false;
    Status st;
    float scrollMax = 0.0f, scrollView = 0.0f, forceScroll = -1.0f;
    unsigned seenToast = 0;
    bool wasOpen = false;
    std::chrono::steady_clock::time_point openedAt{};
    std::chrono::steady_clock::time_point toastAt{};
    std::string toastText;
    bool toastWarn = false;
    long long cashEdit = -1;
} U;

float px(float v) { return std::floor(v * U.S + 0.5f); }
ImU32 col(const ImVec4& c, float a = 1.0f) { return ImGui::ColorConvertFloat4ToU32(ImVec4(c.x, c.y, c.z, c.w * a)); }

constexpr float kBody = 19.0f, kSmall = 16.0f, kTitle = 28.0f, kCardTitle = 20.5f;

void applyStyle(float S)
{
    ImGuiStyle st;   // defaults
    ImGui::StyleColorsDark(&st);
    st.WindowPadding = ImVec2(0, 0);
    st.FramePadding = ImVec2(12, 7);
    st.ItemSpacing = ImVec2(10, 9);
    st.ItemInnerSpacing = ImVec2(8, 6);
    st.CellPadding = ImVec2(6, 9);
    st.ScrollbarSize = 11;
    st.ScrollbarRounding = 8;
    st.GrabMinSize = 16;
    st.GrabRounding = 9;
    st.FrameRounding = 9;
    st.ChildRounding = 13;
    st.WindowRounding = 17;
    st.PopupRounding = 10;
    st.WindowBorderSize = 1;
    st.ChildBorderSize = 1;
    st.FrameBorderSize = 0;
    st.PopupBorderSize = 1;
    st.DisabledAlpha = 0.45f;
    ImVec4* c = st.Colors;
    c[ImGuiCol_Text] = kText;
    c[ImGuiCol_TextDisabled] = kMuted;
    c[ImGuiCol_WindowBg] = kWin;
    c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PopupBg] = ImVec4(0.075f, 0.088f, 0.112f, 0.99f);
    c[ImGuiCol_Border] = ImVec4(1, 1, 1, 0.09f);
    c[ImGuiCol_FrameBg] = kTrack;
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.21f, 0.24f, 0.29f, 1.0f);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.23f, 0.27f, 0.33f, 1.0f);
    c[ImGuiCol_Button] = ImVec4(0.17f, 0.195f, 0.24f, 1.0f);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.22f, 0.26f, 0.32f, 1.0f);
    c[ImGuiCol_ButtonActive] = ImVec4(0.26f, 0.31f, 0.38f, 1.0f);
    c[ImGuiCol_Header] = ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.22f);
    c[ImGuiCol_HeaderHovered] = ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.30f);
    c[ImGuiCol_HeaderActive] = ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.40f);
    c[ImGuiCol_SliderGrab] = kAccent;
    c[ImGuiCol_SliderGrabActive] = ImVec4(0.50f, 0.74f, 1.0f, 1.0f);
    c[ImGuiCol_CheckMark] = kAccent;
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab] = ImVec4(1, 1, 1, 0.14f);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(1, 1, 1, 0.22f);
    c[ImGuiCol_ScrollbarGrabActive] = ImVec4(1, 1, 1, 0.30f);
    c[ImGuiCol_Separator] = kLine;
    c[ImGuiCol_TableBorderLight] = kLine;
    c[ImGuiCol_TableBorderStrong] = kLine;
    c[ImGuiCol_TableHeaderBg] = ImVec4(1, 1, 1, 0.035f);
    c[ImGuiCol_TableRowBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TableRowBgAlt] = ImVec4(1, 1, 1, 0.018f);
    c[ImGuiCol_NavCursor] = ImVec4(0.55f, 0.78f, 1.0f, 0.95f);
    c[ImGuiCol_TextSelectedBg] = ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.40f);
    st.ScaleAllSizes(S);
    // ScaleAllSizes truncates: below 1.0 the 1-pixel borders would go, and a child without a border has no padding
    st.WindowBorderSize = st.ChildBorderSize = st.PopupBorderSize = std::max(1.0f, std::floor(S + 0.5f));
    st.FontSizeBase = kBody;
    st.FontScaleMain = S;
    ImGui::GetStyle() = st;
}

void textCol(const ImVec4& c, const char* s)
{
    ImGui::PushStyleColor(ImGuiCol_Text, c);
    ImGui::TextUnformatted(s);
    ImGui::PopStyleColor();
}

// Small wrapped text (notes, descriptions, live state).
void note(const std::string& s, const ImVec4& c = kMuted)
{
    if (s.empty())
        return;
    ImGui::PushFont(nullptr, kSmall);
    ImGui::PushStyleColor(ImGuiCol_Text, c);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(s.c_str());
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

void chip(const char* text, const ImVec4& c)
{
    ImGui::PushFont(nullptr, kSmall);
    const ImVec2 ts = ImGui::CalcTextSize(text), pad(px(10), px(3)), p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + ts.x + pad.x * 2, p.y + ts.y + pad.y * 2), col(c, 0.16f), 99.0f);
    dl->AddText(ImVec2(p.x + pad.x, p.y + pad.y), col(c), text);
    ImGui::Dummy(ImVec2(ts.x + pad.x * 2, ts.y + pad.y * 2));
    ImGui::PopFont();
}
float chipW(const char* text)
{
    ImGui::PushFont(nullptr, kSmall);
    const float w = ImGui::CalcTextSize(text).x + px(20);
    ImGui::PopFont();
    return w;
}

// ---- widgets ------------------------------------------------------------------------------------------------------
float toggleW() { return std::floor(std::floor(ImGui::GetFrameHeight() * 0.86f) * 1.85f); }

bool toggle(const char* id, bool* v)
{
    const float fh = ImGui::GetFrameHeight(), h = std::floor(fh * 0.86f), w = std::floor(h * 1.85f);
    ImVec2 p = ImGui::GetCursorScreenPos();
    p.y += std::floor((fh - h) * 0.5f);
    ImGui::SetCursorScreenPos(p);
    const bool pressed = ImGui::InvisibleButton(id, ImVec2(w, h));
    if (pressed)
        *v = !*v;
    const bool hov = ImGui::IsItemHovered();
    const ImGuiID gid = ImGui::GetItemID();
    ImGuiStorage* stg = ImGui::GetStateStorage();
    const float target = *v ? 1.0f : 0.0f;
    float t = stg->GetFloat(gid, target);
    t += (target - t) * std::min(1.0f, ImGui::GetIO().DeltaTime * 16.0f);
    if (std::fabs(target - t) < 0.01f)
        t = target;
    stg->SetFloat(gid, t);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec4 off = kTrack;
    if (hov) {
        off.x += 0.05f;
        off.y += 0.05f;
        off.z += 0.06f;
    }
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), ImGui::ColorConvertFloat4ToU32(ImLerp(off, kAccent, t)), h * 0.5f);
    const float r = h * 0.5f - px(3);
    dl->AddCircleFilled(ImVec2(p.x + h * 0.5f + t * (w - h), p.y + h * 0.5f), r, IM_COL32(255, 255, 255, 255), 24);
    return pressed;
}

bool button(const char* label, int kind = 0, float w = 0.0f)   // 0 plain, 1 primary, 2 quiet
{
    if (kind == 1) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.47f, 0.86f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.26f, 0.55f, 0.95f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.17f, 0.41f, 0.78f, 1.0f));
    } else if (kind == 2) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(1, 1, 1, 0.05f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1, 1, 1, 0.10f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1, 1, 1, 0.15f));
    }
    const bool r = ImGui::Button(label, ImVec2(w, 0));
    if (kind)
        ImGui::PopStyleColor(3);
    return r;
}

// A button with the order's keyboard key at its right end.
bool cmdButton(const char* label, const std::string& key, float w)
{
    const float h = std::floor(ImGui::GetFrameHeight() * 1.12f);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::PushID(label);
    const bool pressed = ImGui::InvisibleButton("##cmd", ImVec2(w, h));
    const bool hov = ImGui::IsItemHovered(), act = ImGui::IsItemActive();
    ImGui::PopID();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImGuiStyle& st = ImGui::GetStyle();
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), ImGui::GetColorU32(act ? ImGuiCol_ButtonActive : hov ? ImGuiCol_ButtonHovered : ImGuiCol_Button),
                      st.FrameRounding);
    const ImVec2 ts = ImGui::CalcTextSize(label);
    dl->AddText(ImVec2(p.x + px(13), p.y + (h - ts.y) * 0.5f), col(kText), label);
    if (!key.empty()) {
        ImGui::PushFont(nullptr, kSmall - 1.0f);
        const ImVec2 ks = ImGui::CalcTextSize(key.c_str());
        const ImVec2 a(p.x + w - ks.x - px(20), p.y + (h - ks.y) * 0.5f - px(2)), b(p.x + w - px(8), p.y + (h + ks.y) * 0.5f + px(2));
        dl->AddRectFilled(a, b, IM_COL32(0, 0, 0, 70), px(5));
        dl->AddRect(a, b, IM_COL32(255, 255, 255, 46), px(5));
        dl->AddText(ImVec2(a.x + px(6), p.y + (h - ks.y) * 0.5f), col(kMuted), key.c_str());
        ImGui::PopFont();
    }
    return pressed;
}

bool beginCard(const char* id, const char* title, const char* badge = nullptr, const ImVec4& badgeCol = kAccent)
{
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kCard);
    ImGui::PushStyleColor(ImGuiCol_Border, kLine);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(px(20), px(16)));
    // AlwaysAutoResize: a card below the view is measured too, so the page's length does not change while scrolling
    ImGui::BeginChild(id, ImVec2(0, 0),
                      ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysAutoResize |
                          ImGuiChildFlags_NavFlattened,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    if (title && *title) {
        const float rightX = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
        ImGui::PushFont(U.bold, kCardTitle);
        ImGui::TextUnformatted(title);
        ImGui::PopFont();
        if (badge) {
            ImGui::SameLine(rightX - chipW(badge));
            chip(badge, badgeCol);
        }
        ImGui::Dummy(ImVec2(0, px(2)));
    }
    return true;
}
void endCard()
{
    ImGui::EndChild();
    ImGui::PopStyleColor(2);
    ImGui::Dummy(ImVec2(0, px(4)));
}

// A dark inset box with small text: what the engine reports from the car.
void infoBox(const char* id, const std::vector<std::string>& lines, const ImVec4& c = kMuted)
{
    bool any = false;
    for (const auto& l : lines)
        any |= !l.empty();
    if (!any)
        return;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kInset);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(px(14), px(10)));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, px(9));
    ImGui::BeginChild(id, ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysAutoResize,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoNav);
    for (const auto& l : lines)
        note(l, c);
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
}

// Rows of a card: the name and what it does at the left, the controls at the right.
bool beginRows(const char* id, float ctrlW = 400.0f)
{
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(px(2), px(10)));
    if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_NoSavedSettings)) {
        ImGui::PopStyleVar();
        return false;
    }
    ImGui::TableSetupColumn("l", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("r", ImGuiTableColumnFlags_WidthFixed, px(ctrlW));
    return true;
}
void endRows()
{
    ImGui::EndTable();
    ImGui::PopStyleVar();
}
void rowLabel(const char* label, const char* desc)
{
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    if (desc && *desc)
        note(desc);
    ImGui::TableSetColumnIndex(1);
}
void rightAlign(float w) { ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, ImGui::GetContentRegionAvail().x - w)); }

bool rowToggle(const char* label, const char* desc, bool* v)
{
    rowLabel(label, desc);
    rightAlign(toggleW());
    ImGui::PushID(label);
    const bool ch = toggle("##t", v);
    ImGui::PopID();
    return ch;
}

// A slider (Ctrl+click: type the number, also beyond the slider's ends up to the real limits) and, with `on`, the
// feature's switch at the right end.
bool rowSlider(const char* label, const char* desc, bool* on, double* v, double lo, double hi, double realLo, double realHi, const char* format,
               bool keepSwitchPlace = false)
{
    rowLabel(label, desc);
    ImGui::PushID(label);
    const float tw = on || keepSwitchPlace ? toggleW() + ImGui::GetStyle().ItemSpacing.x : 0.0f;
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - tw);
    if (on && !*on)
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.55f);
    bool ch = ImGui::SliderScalar("##s", ImGuiDataType_Double, v, &lo, &hi, format);
    if (on && !*on)
        ImGui::PopStyleVar();
    if (*v < realLo || *v > realHi || std::isnan(*v))
        *v = std::isnan(*v) ? realLo : std::clamp(*v, realLo, realHi);
    if (on) {
        ImGui::SameLine();
        ch |= toggle("##t", on);
    }
    ImGui::PopID();
    return ch;
}

bool rowSliderInt(const char* label, const char* desc, bool* on, int* v, int lo, int hi, int realLo, int realHi, const char* format)
{
    rowLabel(label, desc);
    ImGui::PushID(label);
    const float tw = on ? toggleW() + ImGui::GetStyle().ItemSpacing.x : 0.0f;
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - tw);
    if (on && !*on)
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.55f);
    bool ch = ImGui::SliderInt("##s", v, lo, hi, format);
    if (on && !*on)
        ImGui::PopStyleVar();
    *v = std::clamp(*v, realLo, realHi);
    if (on) {
        ImGui::SameLine();
        ch |= toggle("##t", on);
    }
    ImGui::PopID();
    return ch;
}

std::string commas(long long v)
{
    std::string s = std::to_string(v < 0 ? -v : v);
    for (int i = (int)s.size() - 3; i > 0; i -= 3)
        s.insert((size_t)i, ",");
    return v < 0 ? "-" + s : s;
}

// ---- the pages ----------------------------------------------------------------------------------------------------
struct PageDef {
    const char* name;
    const char* sub;
    const char* chip;
    int icon;
};
const PageDef kPages[] = {
    {T_("PP 고정"), T_("차고의 차마다 표시 PP를 원하는 값으로 고정합니다."), T_("차고"), 4},
    {T_("게임 · 해제"), T_("언제든 쓰는 기능: 크레딧, 커리어·라이선스 해제."), T_("언제든"), 5},
    {T_("설정"), T_("메뉴 조작과 글자 크기."), nullptr, 6},
};
constexpr int kPageCount = (int)(sizeof kPages / sizeof kPages[0]);


std::string nowIso()
{
    SYSTEMTIME t;
    GetLocalTime(&t);
    return sfmt("%04d-%02d-%02dT%02d:%02d:%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
}


// ---- PP Lock ---------------------------------------------------------------------------------------------------
struct PPUi {
    int sel = 0;          // garage ID chosen in the recent list
    int value = 500;
    char label[96] = "";
    int deleteArmed = -1;
} PPU;

void pagePP(Settings& s)
{
    const Status& st = U.st;
    {
        bool have = false;   // nothing chosen (or the chosen car is gone): the car opened last
        for (const RecentCar& r : st.recent)
            have |= r.garageId == PPU.sel;
        if (!have)
            PPU.sel = st.recent.empty() ? 0 : st.recent[0].garageId;
    }
    beginCard("recent", T_("최근 게임이 PP를 읽은 차"), st.recent.empty() ? nullptr : sfmt(T_("%zu대"), st.recent.size()).c_str(), kAccent);
    note(T_("차고나 튜닝 숍에서 차를 열면 맨 위에 나옵니다. 줄을 골라 아래에서 고정할 PP를 정하세요."));
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(px(8), px(6)));
    if (ImGui::BeginTable("rec", 6, ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg | ImGuiTableFlags_NoSavedSettings)) {
        ImGui::TableSetupColumn(T_("차고 ID"), ImGuiTableColumnFlags_WidthFixed, px(80));
        ImGui::TableSetupColumn(T_("차 이름"), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn(T_("코드"), ImGuiTableColumnFlags_WidthFixed, px(70));
        ImGui::TableSetupColumn(T_("실제 PP"), ImGuiTableColumnFlags_WidthFixed, px(90));
        ImGui::TableSetupColumn(T_("PP (타이어)"), ImGuiTableColumnFlags_WidthFixed, px(110));
        ImGui::TableSetupColumn(T_("고정값"), ImGuiTableColumnFlags_WidthFixed, px(80));
        ImGui::PushFont(nullptr, kSmall);
        ImGui::PushStyleColor(ImGuiCol_Text, kFaint);
        ImGui::TableHeadersRow();
        ImGui::PopStyleColor();
        ImGui::PopFont();
        for (const RecentCar& r : st.recent) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::PushID(r.garageId);
            if (ImGui::Selectable(std::to_string(r.garageId).c_str(), PPU.sel == r.garageId, ImGuiSelectableFlags_SpanAllColumns))
                PPU.sel = r.garageId;
            ImGui::PopID();
            ImGui::TableSetColumnIndex(1);
            if (r.name.empty())
                textCol(kFaint, T_("(이름 확인 전 - 게임에서 차 이름이 표시되면 채워짐)"));
            else
                ImGui::TextUnformatted(r.name.c_str());
            ImGui::TableSetColumnIndex(2);
            ImGui::Text("%d", r.carCode);
            ImGui::TableSetColumnIndex(3);
            ImGui::Text("%.1f", r.pp);
            ImGui::TableSetColumnIndex(4);
            ImGui::Text("%.1f", r.ppTire);
            ImGui::TableSetColumnIndex(5);
            float locked = 0;
            for (const PPProfile& p : s.ppProfiles)
                if (p.garageId == r.garageId && p.enabled)
                    locked = p.lockedPP;
            if (locked > 0)
                textCol(kGood, sfmt("%.0f", locked).c_str());
            else
                textCol(kFaint, "-");
        }
        ImGui::EndTable();
    }
    ImGui::PopStyleVar();
    if (st.recent.empty())
        note(T_("아직 확인된 차가 없습니다."), kFaint);
    ImGui::Dummy(ImVec2(0, px(2)));
    {
        const RecentCar* sel = nullptr;
        for (const RecentCar& r : st.recent)
            if (r.garageId == PPU.sel)
                sel = &r;
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(T_("고정할 PP"));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(px(170));
        ImGui::InputInt("##pp", &PPU.value, 1, 10);
        PPU.value = std::clamp(PPU.value, 1, 2000);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(px(260));
        ImGui::InputTextWithHint("##label", T_("이름 (비우면 게임의 차 이름)"), PPU.label, sizeof PPU.label);
        ImGui::SameLine();
        ImGui::BeginDisabled(!sel);
        if (button(T_("선택한 차를 이 PP로 고정"), 1) && sel) {
            PPProfile* pr = nullptr;
            for (PPProfile& p : s.ppProfiles)
                if (p.garageId == sel->garageId)
                    pr = &p;
            if (!pr) {
                s.ppProfiles.push_back({});
                pr = &s.ppProfiles.back();
                pr->garageId = sel->garageId;
            }
            pr->carCode = sel->carCode;
            pr->lockedPP = (float)PPU.value;
            pr->enabled = true;
            pr->realPPWhenSaved = sel->ppTire > 0 ? sel->ppTire : sel->pp;
            const bool def = pr->label.empty() || pr->label == sfmt(T_("차 %d"), pr->carCode) || pr->label == sfmt("car %d", pr->carCode);
            std::string typed = PPU.label;
            while (!typed.empty() && typed.back() == ' ')
                typed.pop_back();
            if (!typed.empty())
                pr->label = typed;
            else if (def)
                pr->label = sel->name.empty() ? sfmt(T_("차 %d"), sel->carCode) : sel->name;
            pr->saved = nowIso();
            U.dirty = true;
            toast(sfmt(T_("차고 ID %d (%s)의 PP를 %d 로 고정했습니다"), pr->garageId, pr->label.c_str(), PPU.value));
        }
        ImGui::EndDisabled();
    }
    endCard();

    beginCard("locks", T_("저장된 PP 고정"), s.ppProfiles.empty() ? nullptr : sfmt(T_("%zu개"), s.ppProfiles.size()).c_str(), kAccent);
    if (s.ppProfiles.empty())
        note(T_("아직 없습니다. 위에서 차를 골라 고정하세요."), kFaint);
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(px(8), px(5)));
    if (!s.ppProfiles.empty() && ImGui::BeginTable("locks", 7, ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_NoSavedSettings)) {
        ImGui::TableSetupColumn(T_("사용"), ImGuiTableColumnFlags_WidthFixed, px(64));
        ImGui::TableSetupColumn(T_("차고 ID"), ImGuiTableColumnFlags_WidthFixed, px(74));
        ImGui::TableSetupColumn(T_("코드"), ImGuiTableColumnFlags_WidthFixed, px(64));
        ImGui::TableSetupColumn(T_("이름"), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn(T_("고정 PP"), ImGuiTableColumnFlags_WidthFixed, px(150));
        ImGui::TableSetupColumn(T_("저장 당시 PP"), ImGuiTableColumnFlags_WidthFixed, px(110));
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, px(96));
        ImGui::PushFont(nullptr, kSmall);
        ImGui::PushStyleColor(ImGuiCol_Text, kFaint);
        ImGui::TableHeadersRow();
        ImGui::PopStyleColor();
        ImGui::PopFont();
        int erase = -1;
        for (size_t i = 0; i < s.ppProfiles.size(); i++) {
            PPProfile& p = s.ppProfiles[i];
            ImGui::TableNextRow();
            ImGui::PushID((int)i);
            ImGui::TableSetColumnIndex(0);
            U.dirty |= toggle("##on", &p.enabled);
            ImGui::TableSetColumnIndex(1);
            ImGui::AlignTextToFramePadding();
            ImGui::Text("%d", p.garageId);
            ImGui::TableSetColumnIndex(2);
            ImGui::AlignTextToFramePadding();
            ImGui::Text("%d", p.carCode);
            ImGui::TableSetColumnIndex(3);
            char buf[96];
            std::snprintf(buf, sizeof buf, "%s", p.label.c_str());
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
            if (ImGui::InputText("##name", buf, sizeof buf)) {
                p.label = buf;
                U.dirty = true;
            }
            ImGui::TableSetColumnIndex(4);
            int pp = (int)std::lround(p.lockedPP);
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
            if (ImGui::InputInt("##pp", &pp, 1, 10)) {
                p.lockedPP = (float)std::clamp(pp, 1, 2000);
                U.dirty = true;
            }
            ImGui::TableSetColumnIndex(5);
            ImGui::AlignTextToFramePadding();
            textCol(kMuted, sfmt("%.1f", p.realPPWhenSaved).c_str());
            ImGui::TableSetColumnIndex(6);
            const bool armed = PPU.deleteArmed == (int)i;
            if (button(armed ? T_("정말 삭제") : T_("삭제"), armed ? 1 : 2, ImGui::GetContentRegionAvail().x)) {
                if (armed)
                    erase = (int)i;
                else
                    PPU.deleteArmed = (int)i;
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
        if (erase >= 0) {
            s.ppProfiles.erase(s.ppProfiles.begin() + erase);
            PPU.deleteArmed = -1;
            U.dirty = true;
            toast(T_("PP 고정을 지웠습니다"));
        }
    }
    ImGui::PopStyleVar();
    if (!s.ppProfiles.empty()) {
        if (button(T_("모든 이름을 게임 표시 이름으로 바꾸기"), 2)) {
            Command c;
            c.kind = Command::FillNames;
            post(c);
            toast(T_("게임에서 한 번이라도 이름이 표시된 차의 이름을 바꿨습니다"));
        }
    }
    note(T_("고정한 차는 게임이 PP를 물을 때 이 값으로 답합니다 (이벤트의 PP 제한을 통과). 스위치를 끄면 그 차만 잠시 해제됩니다."), kFaint);
    endCard();
}

void pageGame(Settings& s)
{
    const Status& st = U.st;
    beginCard("cash", T_("크레딧"));
    {
        ImGui::PushFont(U.bold, 30.0f);
        if (st.cashFound)
            textCol(kAccent, (commas(st.cash) + " Cr").c_str());
        else
            textCol(kMuted, T_("아직 못 찾음"));
        ImGui::PopFont();
        if (st.cashFound && s.creditsLock) {
            ImGui::SameLine();
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + px(8));
            chip(T_("고정 중"), kGood);
        }
        if (!st.cashFound)
            note(T_("세이브를 불러오면 찾습니다."));
    }
    if (beginRows("r")) {
        rowLabel(T_("값"), sfmt("%s Cr", commas(s.creditsLockValue).c_str()).c_str());
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
        long long v = s.creditsLockValue;
        const long long step = 1000000, fast = 10000000;
        if (ImGui::InputScalar("##cash", ImGuiDataType_S64, &v, &step, &fast, "%lld")) {
            s.creditsLockValue = std::clamp(v, 0LL, 2147483647LL);
            U.dirty = true;
        }
        rowLabel(T_("적용"), nullptr);
        {
            const float w = std::floor((ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) / 2.0f);
            if (button(T_("지금 적용"), 1, w)) {
                Command c;
                c.kind = Command::SetCashOnce;
                c.a = s.creditsLockValue;
                post(c);
            }
            ImGui::SameLine();
            ImGui::BeginDisabled(!st.cashFound);
            if (button(T_("현재 값 가져오기"), 0, w)) {
                s.creditsLockValue = std::clamp(st.cash, 0LL, 2147483647LL);
                U.dirty = true;
            }
            ImGui::EndDisabled();
        }
        U.dirty |= rowToggle(T_("이 값으로 크레딧 고정"), T_("쓰거나 벌어도 이 값으로 돌아옵니다."), &s.creditsLock);
        endRows();
    }
    endCard();

    beginCard("career", T_("커리어 · 라이선스"));
    if (beginRows("r")) {
        U.dirty |= rowToggle(T_("커리어 전체 해제"), T_("모든 라이선스 / 이벤트 폴더를 클리어한 것으로 취급하고, 스타 합계를 9999로 보여 스타가 필요한 폴더·트랙도 엽니다. 세이브를 바꾸지 않고, 켜져 있는 동안만 풀립니다."), &s.unlockCareer);
        endRows();
    }
    note(T_("게임이 '클리어'로 보고 엔딩 영상 · 보너스 음악 · 선물 차량 · 트로피를 한 번 줄 수 있습니다."), kFaint);
    endCard();

}

void pageSettings(Settings& s)
{
    const Status& st = U.st;
    beginCard("menu", T_("메뉴"));
    if (beginRows("r")) {
        double z = s.uiScale * 100.0;
        if (rowSlider(T_("글자 · 칸 크기"), T_("화면 해상도에 맞춰 자동으로 커지고, 여기서 더 키우거나 줄입니다."), nullptr, &z, 70.0, 160.0, 60.0, 200.0, "%.0f %%")) {
            s.uiScale = (float)(std::round(z / 5.0) * 5.0 / 100.0);
            U.dirty = true;
        }
        if (rowToggle(T_("메뉴가 열려 있는 동안 게임 패드 입력 차단"), T_("켜면 패드로 메뉴를 조작합니다 (십자키 이동 · × 선택 · ○ 닫기 · L1/R1 페이지). "
                                                                 "끄면 패드는 계속 게임을 조작하고 메뉴는 마우스·키보드로만 씁니다."),
                      &s.padBlock))
            U.dirty = true;
        /* [GT6 0087] the menu's language */
        rowLabel("언어 · Language", T_("다음에 게임을 켤 때부터 적용됩니다."));
        {
            const float w = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * 2) / 3.0f;
            const char* names[] = {T_("자동 (Windows)"), "한국어", "English"};
            for (int i = 0; i < 3; i++) {
                if (i)
                    ImGui::SameLine();
                ImGui::PushID(900 + i);
                if (button(names[i], s.menuLang == i ? 1 : 2, std::floor(w))) {
                    s.menuLang = i;
                    U.dirty = true;
                }
                ImGui::PopID();
            }
        }
        endRows();
    }
    endCard();

    // Resolution (2026-11-20: a higher resolution, up to Full HD). The game takes its mode when it boots: from the next start.
    beginCard("screen", T_("화면"), gt6_res_1080() ? T_("지금 1080p") : T_("지금 720p"), kMuted);
    if (beginRows("r")) {
        if (rowToggle(T_("해상도 Full HD (1080p)"), T_("게임 자체의 1080p 모드로 그립니다 (내부 1440x1080, 창 1920x1080). 끄면 720p. "
                                                 "게임이 켜질 때 정해지므로 다음 실행부터 적용됩니다."),
                      &s.fullHd))
            U.dirty = true;
        // [GT6 0102] Real resolution: the game's 1440x1080 buffers drawn that large on the host (from the next start)
        if (s.fullHd) {
            rowLabel(T_("실제 해상도"), T_("게임의 1440x1080 화면을 PC에서 이 크기로 실제로 그립니다 (늘이기가 아님). 창보다 크면 창에 "
                                         "맞게 줄여 보여 줘서 더 매끄럽습니다. 높을수록 GPU 부담이 큽니다. 다음 실행부터 적용됩니다."));
            const float w = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * 3) / 4.0f;
            const char* names[] = {T_("끄기 (1440x1080)"), "1920x1080", "2560x1440", "3840x2160"};
            for (int i = 0; i < 4; i++) {
                if (i)
                    ImGui::SameLine();
                ImGui::PushID(950 + i);
                if (button(names[i], s.realRes == i ? 1 : 2, std::floor(w))) {
                    s.realRes = i;
                    U.dirty = true;
                }
                ImGui::PopID();
            }
        }
        // [GT6 0101] Brightness (2026-11-24, a player's report: "image is very dim"): applied at the present, live
        if (rowSlider(T_("밝기"), T_("화면이 어둡거나 밝게 보일 때 조절합니다. 1.00이 게임이 그린 그대로이고, 올리면 중간 밝기가 "
                                   "밝아집니다(검정과 흰색은 그대로). 바로 적용됩니다."),
                      nullptr, &s.screenBright, 0.6, 1.6, 0.5, 2.0, "%.2f", true))
            U.dirty = true;
        // Live 3D aspect ratio override (0x008A615C + D3D12 present/window shape)
        {
            rowLabel(gt6_tr_english() ? "Aspect ratio (live)" : "화면 비율 (바로 적용)",
                     gt6_tr_english() ? "Widens the 3D view and removes pillarbox bars on ultrawide monitors. Menus/HUD stretch horizontally. Set in the launcher too so real resolution matches (e.g. 3440x1440)."
                                      : "울트라와이드 모니터에서 3D 시야를 넓히고 좌우 검은 여백을 없앱니다. 메뉴/HUD는 가로로 늘어납니다. 런처에서도 설정하면 실제 해상도 폭도 맞춥니다 (예: 3440x1440).");
            static const float kAspects[4] = { 0.0f, 3440.0f / 1440.0f, 2560.0f / 1080.0f, 32.0f / 9.0f };
            static const char* kAspectNames[4] = { "16:9", "21:9 (3440)", "21:9 (2560)", "32:9" };
            int cur = 0;
            for (int i = 1; i < 4; i++)
                if (g_gt6_aspect > 0.5f && std::fabs(g_gt6_aspect - kAspects[i]) < 0.02f) cur = i;
            const float w = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * 3) / 4.0f;
            for (int i = 0; i < 4; i++) {
                if (i) ImGui::SameLine();
                ImGui::PushID(960 + i);
                if (button(kAspectNames[i], cur == i ? 1 : 2, std::floor(w))) {
                    g_gt6_aspect = kAspects[i];
                    rsx_live_draw_present_aspect = kAspects[i];
                    rsx_window_aspect = kAspects[i];
                    if (kAspects[i] > 0.5f) rsx_live_draw_present_stretch = 1;
                }
                ImGui::PopID();
            }
        }
        endRows();
    }
    if (s.fullHd != (gt6_res_1080() != 0))
        note(s.fullHd ? T_("게임을 다시 실행하면 1080p로 켜집니다.") : T_("게임을 다시 실행하면 720p로 켜집니다."), kFaint);
    else if (s.fullHd && s.realRes != gt6_real_res())
        note(T_("게임을 다시 실행하면 새 실제 해상도로 그립니다."), kFaint);
    endCard();


    beginCard("keys", T_("조작"));
    note(T_("F2 — 메뉴 열기 / 닫기 (패드: L3 + R3)\nEsc — 메뉴 닫기\nCtrl + 클릭 — 슬라이더에 숫자 직접 입력\n"
         "패드: 십자키 이동 · × 선택 / 슬라이더 잡기 · ○ 닫기 · L1 / R1 페이지 이동"));
    endCard();

}

// ---- the frame ----------------------------------------------------------------------------------------------------
void icon(ImDrawList* dl, int kind, ImVec2 c, float r, ImU32 color)
{
    const float t = std::max(1.0f, px(1.6f));
    switch (kind) {
    case 0:   // steering wheel
        dl->AddCircle(c, r, color, 28, t);
        dl->AddCircleFilled(c, r * 0.24f, color, 12);
        dl->AddLine(ImVec2(c.x - r, c.y), ImVec2(c.x - r * 0.24f, c.y), color, t);
        dl->AddLine(ImVec2(c.x + r * 0.24f, c.y), ImVec2(c.x + r, c.y), color, t);
        dl->AddLine(ImVec2(c.x, c.y + r * 0.24f), ImVec2(c.x, c.y + r), color, t);
        break;
    case 1:   // flag
        dl->AddLine(ImVec2(c.x - r * 0.8f, c.y - r), ImVec2(c.x - r * 0.8f, c.y + r), color, t);
        dl->AddRect(ImVec2(c.x - r * 0.8f, c.y - r), ImVec2(c.x + r, c.y + r * 0.15f), color, 0.0f, 0, t);
        dl->AddRectFilled(ImVec2(c.x - r * 0.8f, c.y - r), ImVec2(c.x + r * 0.1f, c.y - r * 0.42f), color);
        dl->AddRectFilled(ImVec2(c.x + r * 0.1f, c.y - r * 0.42f), ImVec2(c.x + r, c.y + r * 0.15f), color);
        break;
    case 2:   // sliders
        for (int i = -1; i <= 1; i++) {
            const float y = c.y + i * r * 0.72f, kx = c.x + (i == 0 ? r * 0.35f : -r * 0.35f);
            dl->AddLine(ImVec2(c.x - r, y), ImVec2(c.x + r, y), color, t);
            dl->AddCircleFilled(ImVec2(kx, y), r * 0.26f, color, 12);
        }
        break;
    case 3:   // swap
        dl->AddLine(ImVec2(c.x - r, c.y - r * 0.45f), ImVec2(c.x + r, c.y - r * 0.45f), color, t);
        dl->AddTriangleFilled(ImVec2(c.x + r, c.y - r * 0.45f), ImVec2(c.x + r * 0.4f, c.y - r * 0.95f), ImVec2(c.x + r * 0.4f, c.y + r * 0.05f), color);
        dl->AddLine(ImVec2(c.x - r, c.y + r * 0.45f), ImVec2(c.x + r, c.y + r * 0.45f), color, t);
        dl->AddTriangleFilled(ImVec2(c.x - r, c.y + r * 0.45f), ImVec2(c.x - r * 0.4f, c.y - r * 0.05f), ImVec2(c.x - r * 0.4f, c.y + r * 0.95f), color);
        break;
    case 4:   // lock
        dl->AddRectFilled(ImVec2(c.x - r * 0.78f, c.y - r * 0.1f), ImVec2(c.x + r * 0.78f, c.y + r), color, r * 0.2f);
        dl->PathArcTo(ImVec2(c.x, c.y - r * 0.1f), r * 0.5f, 3.14159f, 6.28318f, 14);
        dl->PathStroke(color, 0, t);
        break;
    case 5:   // coin
        dl->AddCircle(c, r, color, 28, t);
        dl->AddCircle(c, r * 0.5f, color, 16, t);
        break;
    default:   // gear
        dl->AddCircle(c, r * 0.62f, color, 20, t);
        for (int i = 0; i < 8; i++) {
            const float a = i * 0.785398f;
            dl->AddLine(ImVec2(c.x + std::cos(a) * r * 0.62f, c.y + std::sin(a) * r * 0.62f), ImVec2(c.x + std::cos(a) * r, c.y + std::sin(a) * r), color, t * 1.5f);
        }
        break;
    }
}

bool navItem(int idx, bool active)
{
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x, h = px(46);
    ImGui::PushID(idx);
    const bool pressed = ImGui::InvisibleButton("##nav", ImVec2(w, h));
    const bool hov = ImGui::IsItemHovered();
    ImGui::PopID();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (active) {
        dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), col(kAccent, 0.16f), px(10));
        dl->AddRectFilled(ImVec2(p.x, p.y + h * 0.24f), ImVec2(p.x + px(3.5f), p.y + h * 0.76f), col(kAccent), px(2));
    } else if (hov)
        dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), IM_COL32(255, 255, 255, 13), px(10));
    const ImVec4 c = active ? kText : hov ? kText : kMuted;
    icon(dl, kPages[idx].icon, ImVec2(p.x + px(27), p.y + h * 0.5f), px(8.5f), col(active ? kAccent : c));
    ImGui::PushFont(active ? U.bold : U.reg, kBody);
    const ImVec2 ts = ImGui::CalcTextSize(kPages[idx].name);
    dl->AddText(ImVec2(p.x + px(50), p.y + (h - ts.y) * 0.5f), col(c), kPages[idx].name);
    ImGui::PopFont();
    return pressed;
}

void drawMenu(unsigned w, unsigned h)
{
    const Status& st = U.st;
    std::lock_guard<std::mutex> lk(mx());   // the settings are edited under it for this frame
    Settings& s = settings();
    U.dirty = false;
    if (U.wantPage >= 0) {
        U.page = std::clamp(U.wantPage, 0, kPageCount - 1);
        U.wantPage = -1;
        U.forceScroll = 0.0f;
    }

    // the game behind goes dim; the menu comes in over a tenth of a second
    const float fade = std::clamp(std::chrono::duration<float>(std::chrono::steady_clock::now() - U.openedAt).count() / 0.11f, 0.0f, 1.0f);
    ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), ImVec2((float)w, (float)h), IM_COL32(4, 6, 10, (int)(120 * fade)));
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.25f + 0.75f * fade);

    const ImVec2 size(std::min(px(1260), w * 0.955f), std::min(px(810), h * 0.935f));
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    ImGui::SetNextWindowPos(ImVec2(std::floor((w - size.x) * 0.5f), std::floor((h - size.y) * 0.5f)), ImGuiCond_Always);
    ImGui::Begin("##gt6menu", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    const ImVec2 wp = ImGui::GetWindowPos();
    const float sideW = px(244);
    ImGui::GetWindowDrawList()->AddRectFilled(wp, ImVec2(wp.x + sideW, wp.y + size.y), col(kSide), ImGui::GetStyle().WindowRounding, ImDrawFlags_RoundCornersLeft);
    ImGui::GetWindowDrawList()->AddLine(ImVec2(wp.x + sideW, wp.y), ImVec2(wp.x + sideW, wp.y + size.y), col(kLine));

    // pages by L1 / R1 and by 1..7 when nothing is being typed or held
    if (!ImGui::IsAnyItemActive() && !ImGui::GetIO().WantTextInput) {
        int go = 0;
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false))
            go = -1;
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false))
            go = 1;
        if (go) {
            U.page = (U.page + go + kPageCount) % kPageCount;
            U.forceScroll = 0.0f;
            s.lastPage = U.page;
            U.dirty = true;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false))
            setOpen(false);
    }

    // ---- the side bar ----
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(px(14), px(20)));
    ImGui::BeginChild("##side", ImVec2(sideW, 0), ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_NavFlattened, ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleVar();
    {
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + px(8));
        ImGui::PushFont(U.bold, 24.0f);
        textCol(kText, "GT6");
        ImGui::SameLine(0, px(7));
        textCol(kAccent, "TRAINER");
        ImGui::PopFont();
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + px(8));
        note("Gran Turismo 6 · 1.22", kFaint);
        ImGui::Dummy(ImVec2(0, px(10)));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, px(3)));
        for (int i = 0; i < kPageCount; i++)
            if (navItem(i, U.page == i) && U.page != i) {
                U.page = i;
                U.forceScroll = 0.0f;
                s.lastPage = i;
                U.dirty = true;
                std::fprintf(stderr, "[menu] page %d (%s)\n", i, kPages[i].name);
            }
        ImGui::PopStyleVar();

        // the state and the master switch at the bottom
        const float blockH = px(132);
        ImGui::SetCursorPosY(std::max(ImGui::GetCursorPosY() + px(8), ImGui::GetWindowHeight() - blockH - px(18)));
        const ImVec2 bp = ImGui::GetCursorScreenPos();
        const float bw = ImGui::GetContentRegionAvail().x;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(bp, ImVec2(bp.x + bw, bp.y + blockH), col(kCard), px(12));
        dl->AddRect(bp, ImVec2(bp.x + bw, bp.y + blockH), col(kLine), px(12));
        const char* state;
        ImVec4 sc;
        if (!st.supported) {
            state = T_("지원 안 되는 게임");
            sc = kBad;
        } else if (st.external) {
            state = T_("외장 트레이너 사용 중");
            sc = kWarn;
        } else if (!s.master) {
            state = T_("트레이너 꺼짐");
            sc = kMuted;
        } else if (st.inRace == 1) {
            state = T_("레이스 중");
            sc = kGood;
        } else if (st.inRace == 0) {
            state = T_("메뉴 · 차고");
            sc = kAccent;
        } else {
            state = T_("시작하는 중");
            sc = kMuted;
        }
        dl->AddCircleFilled(ImVec2(bp.x + px(20), bp.y + px(26)), px(5), col(sc), 16);
        ImGui::PushFont(U.bold, kBody);
        dl->AddText(ImVec2(bp.x + px(34), bp.y + px(26) - ImGui::GetFontSize() * 0.5f), col(kText), state);
        ImGui::PopFont();
        ImGui::PushFont(nullptr, kSmall);
        const std::string sub = st.supported && s.master && !st.external ? std::string(T_("켜짐"))
                                : st.external                              ? std::string(T_("내장 트레이너는 쉬는 중"))
                                                                           : std::string(T_("게임은 트레이너 없이 실행 중"));
        dl->AddText(ImVec2(bp.x + px(20), bp.y + px(46)), col(kMuted), sub.c_str());
        ImGui::PopFont();
        dl->AddLine(ImVec2(bp.x + px(14), bp.y + px(76)), ImVec2(bp.x + bw - px(14), bp.y + px(76)), col(kLine));
        ImGui::SetCursorScreenPos(ImVec2(bp.x + px(20), bp.y + px(88)));
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(T_("트레이너"));
        ImGui::SameLine();
        ImGui::SetCursorScreenPos(ImVec2(bp.x + bw - px(16) - toggleW(), bp.y + px(88)));
        if (toggle("##master", &s.master)) {
            U.dirty = true;
            toast(s.master ? T_("트레이너를 켰습니다") : T_("트레이너를 껐습니다: 게임이 트레이너 없이 실행됩니다 (부품 해제 포함 모두 꺼짐)"));
        }
    }
    ImGui::EndChild();
    ImGui::SameLine(0, 0);

    // ---- the page ----
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(px(30), px(24)));
    ImGui::BeginChild("##main", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_NavFlattened, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    {
        const PageDef& pg = kPages[U.page];
        const float headRight = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
        ImGui::PushFont(U.bold, kTitle);
        ImGui::TextUnformatted(pg.name);
        ImGui::PopFont();
        if (pg.chip) {
            ImGui::SameLine(0, px(12));
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + px(7));
            chip(pg.chip, kAccent);
        }
        {
            const char* close = T_("닫기  F2");
            ImGui::SameLine(headRight - ImGui::CalcTextSize(close).x - ImGui::GetStyle().FramePadding.x * 2);
            if (button(close, 2))
                setOpen(false);
        }
        note(pg.sub);
        if (st.supported && st.external)
            note(T_("외장 트레이너(GT6TrainerNative)가 연결되어 있어 내장 트레이너는 쉬는 중입니다. 외장 트레이너를 닫으면 자동으로 이어받습니다."), kWarn);
        else if (!st.supported)
            note(T_("이 게임 이미지는 트레이너를 지원하지 않습니다 (네이티브 브리지가 준비되지 않았습니다)."), kBad);
        else if (!s.master)
            note(T_("트레이너가 꺼져 있습니다. 왼쪽 아래의 스위치로 켜면 아래 설정이 적용됩니다."), kWarn);
        ImGui::Dummy(ImVec2(0, px(4)));

        const float footerH = ImGui::GetFrameHeight() + px(12);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::BeginChild("##scroll", ImVec2(0, -footerH), ImGuiChildFlags_NavFlattened, 0);
        ImGui::PopStyleVar();
        if (U.forceScroll >= 0.0f) {
            ImGui::SetScrollY(std::min(U.forceScroll, ImGui::GetScrollMaxY()));
            U.forceScroll = -1.0f;
        }
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(px(10), px(9)));
        // the cards stay clear of the scroll bar
        const float cardW = ImGui::GetContentRegionAvail().x - px(14);
        ImGui::BeginChild("##cards", ImVec2(cardW, 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysAutoResize | ImGuiChildFlags_NavFlattened,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        switch (U.page) {
        case 0: pagePP(s); break;
        case 1: pageGame(s); break;
        default: pageSettings(s); break;
        }
        ImGui::EndChild();
        ImGui::PopStyleVar();
        U.scrollMax = ImGui::GetScrollMaxY();
        U.scrollView = ImGui::GetWindowHeight();
        ImGui::EndChild();

        // the status line: the last result stays
        ImGui::Dummy(ImVec2(0, px(2)));
        ImGui::PushFont(nullptr, kSmall);
        if (!U.toastText.empty()) {
            const long long age = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - U.toastAt).count();
            textCol(U.toastWarn ? kWarn : kGood, (std::string(U.toastWarn ? "! " : "● ") + U.toastText).c_str());
            ImGui::SameLine();
            textCol(kFaint, sfmt(T_("· %lld초 전"), age).c_str());
        } else
            textCol(kFaint, T_("F2 닫기 · Ctrl+클릭: 숫자 입력 · 패드: × 선택 · ○ 닫기 · L1/R1 페이지"));
        ImGui::PopFont();
    }
    ImGui::EndChild();
    ImGui::End();
    ImGui::PopStyleVar();

    g_typing = ImGui::GetIO().WantTextInput;
    if (g_padBlock != s.padBlock) {
        g_padBlock = s.padBlock;
        menuCaptures(g_open && g_padBlock);
    }
    if (U.dirty) {
        markDirty();
        rulesDirty();
    }
}

// The closed menu: a notice at the top right for a few seconds (the start, an order's result).
std::chrono::steady_clock::time_point g_noticeUntil{}, g_bootUntil{};
bool g_bootSet = false;

void drawNotice(unsigned w)
{
    const auto now = std::chrono::steady_clock::now();
    std::string text;
    ImVec4 c = kText;
    if (now < g_noticeUntil && !U.toastText.empty()) {
        text = U.toastText;
        c = U.toastWarn ? kWarn : kText;
    } else if (now < g_bootUntil)
        text = U.st.supported ? (U.st.external ? T_("F2 · 트레이너 메뉴  (외장 트레이너 사용 중)") : U.st.running ? T_("F2 · 트레이너 메뉴  (트레이너 켜짐)") : T_("F2 · 트레이너 메뉴  (트레이너 꺼짐)"))
                              : T_("F2 · 트레이너 메뉴");
    if (text.empty())
        return;
    ImGui::PushFont(U.reg, kBody);
    const ImVec2 ts = ImGui::CalcTextSize(text.c_str()), pad(px(18), px(11));
    const ImVec2 a((float)w - ts.x - pad.x * 2 - px(28), px(26)), b((float)w - px(28), px(26) + ts.y + pad.y * 2);
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    dl->AddRectFilled(a, b, IM_COL32(12, 15, 21, 225), px(12));
    dl->AddRect(a, b, IM_COL32(255, 255, 255, 28), px(12));
    dl->AddRectFilled(ImVec2(a.x, a.y + px(10)), ImVec2(a.x + px(3.5f), b.y - px(10)), col(U.toastWarn && now < g_noticeUntil ? kWarn : kAccent), px(2));
    dl->AddText(ImVec2(a.x + pad.x, a.y + pad.y), col(c), text.c_str());
    ImGui::PopFont();
}

// ---- ImGui on the live renderer ----------------------------------------------------------------------------
bool g_inited = false, g_failed = false;
ID3D12DescriptorHeap* g_srvHeap = nullptr;
UINT g_srvStep = 0;
std::vector<int> g_srvFree;

void srvAlloc(ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE* cpu, D3D12_GPU_DESCRIPTOR_HANDLE* gpu)
{
    const int i = g_srvFree.back();
    g_srvFree.pop_back();
    *cpu = g_srvHeap->GetCPUDescriptorHandleForHeapStart();
    *gpu = g_srvHeap->GetGPUDescriptorHandleForHeapStart();
    cpu->ptr += (SIZE_T)i * g_srvStep;
    gpu->ptr += (UINT64)i * g_srvStep;
}
void srvFree(ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE cpu, D3D12_GPU_DESCRIPTOR_HANDLE)
{
    g_srvFree.push_back((int)((cpu.ptr - g_srvHeap->GetCPUDescriptorHandleForHeapStart().ptr) / g_srvStep));
}

bool init(ID3D12Device* dev, ID3D12CommandQueue* queue)
{
    D3D12_DESCRIPTOR_HEAP_DESC hd = {};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = 64;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_srvHeap))))
        return false;
    g_srvStep = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    for (int i = 63; i >= 0; i--)
        g_srvFree.push_back(i);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;   // the layout is fixed; the zoom is in the trainer's settings
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    io.ConfigNavMoveSetMousePos = false;
    ImGui::GetCurrentContext()->ConfigNavWindowingKeyNext = ImGuiKey_None;   // no Ctrl+Tab window list
    ImGui::GetCurrentContext()->ConfigNavWindowingKeyPrev = ImGuiKey_None;

    // Korean text: Malgun Gothic, regular and bold (glyphs are rasterized on demand by ImGui 1.92).
    char win[MAX_PATH];
    GetWindowsDirectoryA(win, MAX_PATH);
    const std::string fonts = std::string(win) + "\\Fonts\\";
    auto have = [&](const char* f) { return GetFileAttributesA((fonts + f).c_str()) != INVALID_FILE_ATTRIBUTES; };
    for (const char* f : {"malgun.ttf", "gulim.ttc"})
        if (!U.reg && have(f))
            U.reg = io.Fonts->AddFontFromFileTTF((fonts + f).c_str(), kBody);
    if (!U.reg)
        U.reg = io.Fonts->AddFontDefault();
    for (const char* f : {"malgunbd.ttf"})
        if (!U.bold && have(f))
            U.bold = io.Fonts->AddFontFromFileTTF((fonts + f).c_str(), kBody);
    if (!U.bold)
        U.bold = U.reg;

    ImGui_ImplDX12_InitInfo info;
    info.Device = dev;
    info.CommandQueue = queue;
    info.NumFramesInFlight = 4;
    info.RTVFormat = DXGI_FORMAT_R8G8B8A8_UNORM;   // the swap chain's format (rsx_live_draw.c)
    info.DSVFormat = DXGI_FORMAT_UNKNOWN;
    info.SrvDescriptorHeap = g_srvHeap;
    info.SrvDescriptorAllocFn = srvAlloc;
    info.SrvDescriptorFreeFn = srvFree;
    if (!ImGui_ImplDX12_Init(&info))
        return false;
    std::fprintf(stderr, "[menu] ImGui %s on the live renderer\n", IMGUI_VERSION);
    return true;
}

ImGuiKey vkToKey(uintptr_t vk)
{
    switch (vk) {
    case VK_TAB: return ImGuiKey_Tab;
    case VK_LEFT: return ImGuiKey_LeftArrow;
    case VK_RIGHT: return ImGuiKey_RightArrow;
    case VK_UP: return ImGuiKey_UpArrow;
    case VK_DOWN: return ImGuiKey_DownArrow;
    case VK_PRIOR: return ImGuiKey_PageUp;
    case VK_NEXT: return ImGuiKey_PageDown;
    case VK_HOME: return ImGuiKey_Home;
    case VK_END: return ImGuiKey_End;
    case VK_DELETE: return ImGuiKey_Delete;
    case VK_BACK: return ImGuiKey_Backspace;
    case VK_SPACE: return ImGuiKey_Space;
    case VK_RETURN: return ImGuiKey_Enter;
    case VK_CONTROL: return ImGuiKey_LeftCtrl;
    case VK_SHIFT: return ImGuiKey_LeftShift;
    case VK_OEM_MINUS: case VK_SUBTRACT: return ImGuiKey_Minus;
    case VK_OEM_PERIOD: case VK_DECIMAL: return ImGuiKey_Period;
    case 'A': return ImGuiKey_A;
    case 'C': return ImGuiKey_C;
    case 'V': return ImGuiKey_V;
    case 'X': return ImGuiKey_X;
    case 'Z': return ImGuiKey_Z;
    default: return ImGuiKey_None;
    }
}

void feedInput(unsigned w, unsigned h)
{
    ImGuiIO& io = ImGui::GetIO();
    std::vector<Msg> msgs;
    {
        std::lock_guard<std::mutex> lock(g_msgMutex);
        msgs.swap(g_msgs);
    }
    const float sx = g_clientW > 0 ? (float)w / g_clientW : 1.0f, sy = g_clientH > 0 ? (float)h / g_clientH : 1.0f;
    for (const Msg& m : msgs) {
        switch (m.msg) {
        case WM_MOUSEMOVE:
            io.AddMousePosEvent((float)(short)LOWORD(m.lp) * sx, (float)(short)HIWORD(m.lp) * sy);
            break;
        case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: io.AddMouseButtonEvent(0, true); break;
        case WM_LBUTTONUP: io.AddMouseButtonEvent(0, false); break;
        case WM_RBUTTONDOWN: io.AddMouseButtonEvent(1, true); break;
        case WM_RBUTTONUP: io.AddMouseButtonEvent(1, false); break;
        case WM_MBUTTONDOWN: io.AddMouseButtonEvent(2, true); break;
        case WM_MBUTTONUP: io.AddMouseButtonEvent(2, false); break;
        case WM_MOUSEWHEEL: io.AddMouseWheelEvent(0.0f, (float)(short)HIWORD(m.wp) / WHEEL_DELTA); break;
        case WM_KEYDOWN: case WM_SYSKEYDOWN: case WM_KEYUP: case WM_SYSKEYUP: {
            const bool down = m.msg == WM_KEYDOWN || m.msg == WM_SYSKEYDOWN;
            const ImGuiKey key = vkToKey(m.wp);
            if (key != ImGuiKey_None)
                io.AddKeyEvent(key, down);
            if (m.wp == VK_CONTROL)
                io.AddKeyEvent(ImGuiMod_Ctrl, down);
            if (m.wp == VK_SHIFT)
                io.AddKeyEvent(ImGuiMod_Shift, down);
            break;
        }
        case WM_CHAR:
            if (m.wp >= 0x20 && m.wp < 0x80)
                io.AddInputCharacter((unsigned)m.wp);
            break;
        case WM_IME_CHAR: {   // the window is ANSI: a double-byte character of the system code page
            char mb[2];
            int n = 0;
            if (m.wp > 0xFF)
                mb[n++] = (char)(m.wp >> 8);
            mb[n++] = (char)(m.wp & 0xFF);
            wchar_t wc[2];
            const int k = MultiByteToWideChar(CP_ACP, 0, mb, n, wc, 2);
            for (int i = 0; i < k; i++)
                io.AddInputCharacterUTF16(wc[i]);
            break;
        }
        }
    }
    // The pad while it is the menu's (CELL_PAD_CTRL_*: UP 0x10, RIGHT 0x20, DOWN 0x40, LEFT 0x80, L1 0x400,
    // R1 0x800, CIRCLE 0x2000, CROSS 0x4000): X chooses, O goes back, as in this game.
    unsigned p = 0;
    if (g_open && g_padBlock)
        for (auto& a : g_padPorts)
            p |= a.load(std::memory_order_relaxed);
    // The port's keyboard pad (cellPad.c: arrows = d-pad, Z = X, X = O, Q = L1, W = R1) is part of those buttons.
    // In the menu a key is a key: an arrow moves once, and typing x into a box does not close the menu.
    static const struct {
        int vk;
        unsigned btn;
    } kb[] = {{VK_UP, 0x10u}, {VK_RIGHT, 0x20u}, {VK_DOWN, 0x40u}, {VK_LEFT, 0x80u}, {'Z', 0x4000u}, {'X', 0x2000u}, {'Q', 0x400u}, {'W', 0x800u}};
    if (p)
        for (const auto& k : kb)
            if ((p & k.btn) && (GetAsyncKeyState(k.vk) & 0x8000))
                p &= ~k.btn;
    io.AddKeyEvent(ImGuiKey_GamepadDpadUp, (p & 0x10u) != 0);
    io.AddKeyEvent(ImGuiKey_GamepadDpadRight, (p & 0x20u) != 0);
    io.AddKeyEvent(ImGuiKey_GamepadDpadDown, (p & 0x40u) != 0);
    io.AddKeyEvent(ImGuiKey_GamepadDpadLeft, (p & 0x80u) != 0);
    io.AddKeyEvent(ImGuiKey_GamepadFaceDown, (p & 0x4000u) != 0);
    io.AddKeyEvent(ImGuiKey_GamepadFaceRight, (p & 0x2000u) != 0);
    io.AddKeyEvent(ImGuiKey_GamepadL1, (p & 0x400u) != 0);
    io.AddKeyEvent(ImGuiKey_GamepadR1, (p & 0x800u) != 0);
}

bool shotDue();

int overlayOn()
{
    if (g_open || shotDue())
        return 1;
    const auto now = std::chrono::steady_clock::now();
    if (!g_bootSet) {
        g_bootSet = true;
        g_bootUntil = now + std::chrono::seconds(9);
    }
    if (toastSeq() != U.seenToast)
        return 1;   // a new message: the frame takes it over
    return now < g_bootUntil || now < g_noticeUntil ? 1 : 0;
}

// GT6_MENU_SHOT=<prefix> (tests): back buffers (game + menu) written as BMP once the GPU is past them.
struct Capture {
    std::string path;
    int stage = 0;   // 0 idle, 1 wanted, 2 copy recorded, 3 fence signalled
    ID3D12Resource* readback = nullptr;
    ID3D12Fence* fence = nullptr;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {};
    unsigned w = 0, h = 0;
} g_cap;

void captureStep(ID3D12Device* dev, ID3D12CommandQueue* queue, ID3D12GraphicsCommandList* cl, ID3D12Resource* bb, unsigned w, unsigned h)
{
    if (g_cap.stage == 1) {
        D3D12_RESOURCE_DESC desc = bb->GetDesc();
        UINT64 total = 0;
        dev->GetCopyableFootprints(&desc, 0, 1, 0, &g_cap.fp, nullptr, nullptr, &total);
        D3D12_HEAP_PROPERTIES hp = {};
        hp.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC rd = {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = total;
        rd.Height = 1;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&g_cap.readback))) ||
            FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_cap.fence)))) {
            g_cap.stage = 0;
            return;
        }
        D3D12_RESOURCE_BARRIER b = {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = bb;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        cl->ResourceBarrier(1, &b);
        D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
        dst.pResource = g_cap.readback;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = g_cap.fp;
        src.pResource = bb;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = 0;
        cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        cl->ResourceBarrier(1, &b);
        g_cap.w = w;
        g_cap.h = h;
        g_cap.stage = 2;
    } else if (g_cap.stage == 2) {
        queue->Signal(g_cap.fence, 1);   // after the previous frame's list, which holds the copy
        g_cap.stage = 3;
    } else if (g_cap.stage == 3 && g_cap.fence->GetCompletedValue() >= 1) {
        uint8_t* p = nullptr;
        D3D12_RANGE r = {0, (SIZE_T)g_cap.fp.Footprint.RowPitch * g_cap.h};
        if (SUCCEEDED(g_cap.readback->Map(0, &r, (void**)&p))) {
            std::ofstream f(g_cap.path, std::ios::binary);
            const uint32_t row = g_cap.w * 4, size = 54 + row * g_cap.h;
            uint8_t hdr[54] = {'B', 'M'};
            std::memcpy(hdr + 2, &size, 4);
            const uint32_t off = 54, dib = 40, planes_bpp = 1 | (32 << 16);
            std::memcpy(hdr + 10, &off, 4);
            std::memcpy(hdr + 14, &dib, 4);
            const int32_t bw = (int32_t)g_cap.w, bh = -(int32_t)g_cap.h;   // top-down
            std::memcpy(hdr + 18, &bw, 4);
            std::memcpy(hdr + 22, &bh, 4);
            std::memcpy(hdr + 26, &planes_bpp, 4);
            f.write((const char*)hdr, 54);
            std::vector<uint8_t> line(row);
            for (unsigned y = 0; y < g_cap.h; y++) {
                const uint8_t* s = p + (size_t)y * g_cap.fp.Footprint.RowPitch;
                for (unsigned x = 0; x < g_cap.w; x++) {   // RGBA -> BGRA
                    line[x * 4 + 0] = s[x * 4 + 2];
                    line[x * 4 + 1] = s[x * 4 + 1];
                    line[x * 4 + 2] = s[x * 4 + 0];
                    line[x * 4 + 3] = 255;
                }
                f.write((const char*)line.data(), row);
            }
            g_cap.readback->Unmap(0, nullptr);
            std::fprintf(stderr, "[menu] shot %s (%ux%u)\n", g_cap.path.c_str(), g_cap.w, g_cap.h);
        }
        g_cap.readback->Release();
        g_cap.fence->Release();
        g_cap.readback = nullptr;
        g_cap.fence = nullptr;
        g_cap.stage = 0;
    }
}

// The pages one after the other, each at the top and, where it is longer than the view, further down.
struct ShotRun {
    const char* prefix = nullptr;
    double atSec = 0;
    bool read = false, done = false, started = false;
    int page = 0, part = 0, parts = 1, wait = 0, phase = 0;
    ULONGLONG t0 = 0;
} g_shots;

// True from the moment the pages are due until they are all written (the frame has to be drawn for it).
bool shotDue()
{
    ShotRun& r = g_shots;
    if (!r.read) {
        r.read = true;
        r.prefix = std::getenv("GT6_MENU_SHOT");
        const char* at = std::getenv("GT6_MENU_SHOT_SEC");
        r.atSec = at ? std::atof(at) : 20.0;
        r.t0 = GetTickCount64();   // the first present
    }
    return r.prefix && !r.done && (r.started || (GetTickCount64() - r.t0) / 1000.0 >= r.atSec);
}

void shotDrive()
{
    ShotRun& r = g_shots;
    if (!shotDue())
        return;
    if (!r.started) {
        r.started = true;
        setOpen(true);
        r.page = 0;
        r.part = 0;
        r.phase = 0;
    }
    switch (r.phase) {
    case 0:   // show the page part
        U.wantPage = r.page;
        r.wait = 14;
        r.phase = 1;
        break;
    case 1:
        if (--r.wait <= 0) {
            g_cap.path = sfmt("%s_p%d%c.bmp", r.prefix, r.page, 'a' + r.part);
            g_cap.stage = 1;
            r.phase = 2;
        }
        break;
    case 2:
        if (g_cap.stage != 0)
            break;
        if (r.part == 0)   // as many parts as the page is long (each a little less than one view further), six at most
            r.parts = U.scrollMax <= 4.0f ? 1 : std::clamp(1 + (int)std::ceil(U.scrollMax / std::max(1.0f, U.scrollView * 0.85f)), 2, 6);
        if (r.part + 1 < r.parts) {
            r.part++;
            r.wait = 14;
            r.phase = 3;
        } else if (r.page + 1 < kPageCount) {
            r.page++;
            r.part = 0;
            r.phase = 0;
        } else {
            r.done = true;
            setOpen(false);
        }
        break;
    case 3:   // scroll, then shoot
        if (r.wait == 14)
            U.forceScroll = U.scrollMax * (float)r.part / (float)std::max(1, r.parts - 1);
        if (--r.wait <= 0) {
            g_cap.path = sfmt("%s_p%d%c.bmp", r.prefix, r.page, 'a' + r.part);
            g_cap.stage = 1;
            r.phase = 2;
        }
        break;
    }
}

void overlayDraw(void* dev, void* queue, void* list, void* backbuffer, size_t rtv, unsigned w, unsigned h)
{
    if (g_failed || !w || !h)
        return;
    if (!g_inited) {
        g_inited = true;
        if (!init((ID3D12Device*)dev, (ID3D12CommandQueue*)queue)) {
            g_failed = true;
            std::fprintf(stderr, "[menu] ImGui init failed: menu off\n");
            return;
        }
        std::lock_guard<std::mutex> lk(mx());
        U.page = std::clamp(settings().lastPage, 0, kPageCount - 1);
        if (const char* pg = std::getenv("GT6_MENU_PAGE"))
            U.page = std::clamp(std::atoi(pg), 0, kPageCount - 1);
    }
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)w, (float)h);
    static auto last = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    io.DeltaTime = std::max(1.0f / 1000.0f, std::chrono::duration<float>(now - last).count());
    last = now;

    U.st = status();
    if (U.st.toastSeq != U.seenToast) {
        U.seenToast = U.st.toastSeq;
        U.toastText = U.st.toast;
        U.toastWarn = U.st.toastWarn;
        U.toastAt = now;
        g_noticeUntil = now + std::chrono::milliseconds(3800);
    }
    float zoom;
    {
        std::lock_guard<std::mutex> lk(mx());
        zoom = settings().uiScale;
        g_padBlock = settings().padBlock;
    }
    // The back buffer is the game's 1280x720 today (stretched to the window): body text of 17 pixels there, and
    // growing slower than the height for a larger one.
    const float S = 0.9f * std::pow(std::clamp((float)h / 720.0f, 0.5f, 4.0f), 0.75f) * std::clamp(zoom, 0.6f, 2.0f);
    if (std::fabs(S - U.S) > 0.001f) {
        U.S = S;
        applyStyle(S);
    }

    shotDrive();
    feedInput(w, h);
    ImGui_ImplDX12_NewFrame();
    ImGui::NewFrame();
    ImGui::PushFont(U.reg, kBody);
    const bool open = g_open;
    if (open && !U.wasOpen)
        U.openedAt = now;
    U.wasOpen = open;
    if (open)
        drawMenu(w, h);
    else
        drawNotice(w);
    ImGui::PopFont();
    ImGui::Render();
    auto* cl = (ID3D12GraphicsCommandList*)list;
    D3D12_CPU_DESCRIPTOR_HANDLE target;
    target.ptr = rtv;
    cl->OMSetRenderTargets(1, &target, FALSE, nullptr);
    cl->SetDescriptorHeaps(1, &g_srvHeap);
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), cl);
    if (g_cap.stage)
        captureStep((ID3D12Device*)dev, (ID3D12CommandQueue*)queue, cl, (ID3D12Resource*)backbuffer, w, h);
}

}  // namespace

extern "C" void gt6_menu_install(void)
{
    const char* off = std::getenv("GT6_TRAINER_MENU");
    if (off && *off == '0') {
        std::fprintf(stderr, "[menu] GT6_TRAINER_MENU=0: the built-in trainer and its F2 menu are not installed\n");
        return;
    }
    engineStart();
    rsx_window_message_hook = windowHook;
    rsx_live_draw_overlay_on = overlayOn;
    rsx_live_draw_overlay_draw = overlayDraw;
    cellPad_overlay_hook = padHook;
    const char* open = std::getenv("GT6_MENU_OPEN");
    if (open && *open == '1')
        setOpen(true);
    std::fprintf(stderr, "[menu] the built-in trainer is installed: F2 (pad L3+R3) opens the menu\n");
}

#else
extern "C" void gt6_menu_install(void) {}
#endif
