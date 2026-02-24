/**
 * MouseJoystick.cpp — Portable single-file build
 * 
 * Compile (MinGW-w64):
 *   g++ -O2 -std=c++20 -mwindows -o MouseJoystick.exe MouseJoystick.cpp
 *       -luser32 -lgdi32 -lgdiplus -lpsapi -ldwmapi
 *
 * No installer. No runtime DLLs. Drop the .exe anywhere and run.
 *
 * Hotkeys:
 *   F8  — toggle input on/off
 *   F9  — recenter joystick to window centre
 *   ESC — exit
 *
 * Mouse buttons:
 *   Left   → Z
 *   Right  → X
 *   Middle → C
 */

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <gdiplus.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <dwmapi.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>
#include <algorithm>
#include <sstream>
#include <cstdio>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "dwmapi.lib")

using namespace Gdiplus;
using namespace std::chrono_literals;

// ═══════════════════════════════════════════════════════════════════
//  SECTION 1 — Window Targeting
// ═══════════════════════════════════════════════════════════════════

struct WindowInfo {
    HWND         hwnd        = nullptr;
    std::wstring title;
    std::wstring processName;
    RECT         rect        {};   // screen coords

    int   x()      const { return rect.left; }
    int   y()      const { return rect.top;  }
    int   width()  const { return rect.right  - rect.left; }
    int   height() const { return rect.bottom - rect.top;  }
    POINT center() const { return { x() + width()/2, y() + height()/2 }; }
};

static std::wstring ProcessNameForHwnd(HWND hwnd) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return L"?";
    PROCESSENTRY32W pe{ .dwSize = sizeof(pe) };
    std::wstring name = L"?";
    if (Process32FirstW(snap, &pe))
        do { if (pe.th32ProcessID == pid) { name = pe.szExeFile; break; } }
        while (Process32NextW(snap, &pe));
    CloseHandle(snap);
    return name;
}

static bool RectOk(const RECT& r) {
    return (r.right - r.left) > 0 && (r.bottom - r.top) > 0;
}

struct EnumCtx { std::vector<WindowInfo>* list; };

static BOOL CALLBACK EnumCb(HWND hwnd, LPARAM lp) {
    if (!IsWindowVisible(hwnd)) return TRUE;
    wchar_t buf[512]{};
    if (!GetWindowTextW(hwnd, buf, 512) || buf[0] == L'\0') return TRUE;
    RECT r{};
    if (!GetWindowRect(hwnd, &r) || !RectOk(r)) return TRUE;
    auto* ctx = reinterpret_cast<EnumCtx*>(lp);
    ctx->list->push_back({ hwnd, buf, ProcessNameForHwnd(hwnd), r });
    return TRUE;
}

static std::vector<WindowInfo> EnumerateWindows() {
    std::vector<WindowInfo> v;
    EnumCtx ctx{ &v };
    EnumWindows(EnumCb, reinterpret_cast<LPARAM>(&ctx));
    std::sort(v.begin(), v.end(),
        [](auto& a, auto& b){ return a.title < b.title; });
    return v;
}

// Background thread: polls GetWindowRect every 500 ms, fires callback on change
class WindowTracker {
public:
    using Cb = std::function<void(const WindowInfo&)>;

    WindowTracker(WindowInfo wi, Cb cb)
        : m_wi(std::move(wi)), m_cb(std::move(cb)) {}

    ~WindowTracker() { Stop(); }

    void Start() {
        m_run = true;
        m_thr = std::thread([this] {
            while (m_run) {
                RECT r{};
                if (GetWindowRect(m_wi.hwnd, &r) && RectOk(r) &&
                    memcmp(&r, &m_wi.rect, sizeof r) != 0)
                {
                    m_wi.rect = r;
                    m_cb(m_wi);
                }
                std::this_thread::sleep_for(500ms);
            }
        });
    }

    void Stop() {
        m_run = false;
        if (m_thr.joinable()) m_thr.join();
    }

    const WindowInfo& Get()       const { return m_wi; }
    bool IsForeground()           const {
        return GetForegroundWindow() == m_wi.hwnd;
    }

private:
    WindowInfo          m_wi;
    Cb                  m_cb;
    std::atomic<bool>   m_run{ false };
    std::thread         m_thr;
};

// ═══════════════════════════════════════════════════════════════════
//  SECTION 2 — Mouse Input Engine
// ═══════════════════════════════════════════════════════════════════

struct JoyVec {
    float x = 0, y = 0;      // normalised [-1, 1]
    float dist = 0;           // raw px distance
    int   dxPx = 0, dyPx = 0;
};

struct BtnState { bool left = false, right = false, middle = false; };

class MouseEngine {
public:
    MouseEngine(float dz = 30.f, float mr = 200.f)
        : m_dz(dz), m_mr(mr) { s_inst = this; }

    ~MouseEngine() { Uninstall(); s_inst = nullptr; }

    float GetDz() const { return m_dz; }
    float GetMr() const { return m_mr; }

    void Install() {
        m_hook = SetWindowsHookEx(WH_MOUSE_LL, Hook,
                                   GetModuleHandle(nullptr), 0);
    }
    void Uninstall() {
        if (m_hook) { UnhookWindowsHookEx(m_hook); m_hook = nullptr; }
    }

    void SetCenter(int x, int y) {
        std::lock_guard lk(m_mtx);
        m_cx = x; m_cy = y;
        Recompute();
    }

    JoyVec    Vec() const { std::lock_guard lk(m_mtx); return m_vec; }
    BtnState  Btn() const { std::lock_guard lk(m_mtx); return m_btn; }

private:
    static LRESULT CALLBACK Hook(int code, WPARAM wp, LPARAM lp) {
        if (code == HC_ACTION && s_inst) {
            auto* ms = reinterpret_cast<MSLLHOOKSTRUCT*>(lp);
            switch (wp) {
            case WM_MOUSEMOVE:
                { std::lock_guard lk(s_inst->m_mtx);
                  s_inst->m_mx = ms->pt.x;
                  s_inst->m_my = ms->pt.y;
                  s_inst->Recompute(); }
                break;
            case WM_LBUTTONDOWN: s_inst->SetBtn(0, true);  break;
            case WM_LBUTTONUP:   s_inst->SetBtn(0, false); break;
            case WM_RBUTTONDOWN: s_inst->SetBtn(1, true);  break;
            case WM_RBUTTONUP:   s_inst->SetBtn(1, false); break;
            case WM_MBUTTONDOWN: s_inst->SetBtn(2, true);  break;
            case WM_MBUTTONUP:   s_inst->SetBtn(2, false); break;
            }
        }
        return CallNextHookEx(nullptr, code, wp, lp);
    }

    void SetBtn(int b, bool v) {
        std::lock_guard lk(m_mtx);
        if (b == 0) m_btn.left   = v;
        if (b == 1) m_btn.right  = v;
        if (b == 2) m_btn.middle = v;
    }

    // Called under lock
    void Recompute() {
        float dx   = (float)(m_mx - m_cx);
        float dy   = (float)(m_my - m_cy);
        float dist = std::hypot(dx, dy);
        if (dist < m_dz) { m_vec = {}; return; }
        float cl   = std::min(dist, m_mr);
        m_vec = { (dx/dist)*(cl/m_mr), (dy/dist)*(cl/m_mr),
                   dist, (int)dx, (int)dy };
    }

    static MouseEngine* s_inst;

    float m_dz, m_mr;
    int   m_cx{}, m_cy{}, m_mx{}, m_my{};
    JoyVec    m_vec{};
    BtnState  m_btn{};
    mutable std::mutex m_mtx;
    HHOOK m_hook = nullptr;
};
MouseEngine* MouseEngine::s_inst = nullptr;

// ═══════════════════════════════════════════════════════════════════
//  SECTION 3 — Key Sender Engine
// ═══════════════════════════════════════════════════════════════════

static constexpr float AXIS_THR = 0.30f;

class KeySender {
public:
    explicit KeySender(HWND hwnd) : m_hwnd(hwnd) {}
    ~KeySender() { ReleaseAll(); }

    void SetEnabled(bool e) {
        std::lock_guard lk(m_mtx);
        if (!e) ReleaseAllLocked();
        m_enabled = e;
    }

    void Update(const JoyVec& v, const BtnState& b) {
        std::lock_guard lk(m_mtx);
        if (!m_enabled) return;
        Sync("up",    VK_UP,    v.y < -AXIS_THR);
        Sync("down",  VK_DOWN,  v.y >  AXIS_THR);
        Sync("left",  VK_LEFT,  v.x < -AXIS_THR);
        Sync("right", VK_RIGHT, v.x >  AXIS_THR);
        Sync("z", 'Z', b.left);
        Sync("x", 'X', b.right);
        Sync("c", 'C', b.middle);
    }

    void ReleaseAll() {
        std::lock_guard lk(m_mtx);
        ReleaseAllLocked();
    }

private:
    void Sync(const char* name, WORD vk, bool want) {
        bool have = m_pressed.count(name);
        if (want && !have) { Post(vk, true);  m_pressed.insert(name); }
        if (!want && have) { Post(vk, false); m_pressed.erase(name);  }
    }

    void ReleaseAllLocked() {
        static const std::pair<const char*, WORD> MAP[] = {
            {"up",VK_UP},{"down",VK_DOWN},{"left",VK_LEFT},
            {"right",VK_RIGHT},{"z",'Z'},{"x",'X'},{"c",'C'}
        };
        for (auto& [name, vk] : MAP)
            if (m_pressed.count(name)) Post(vk, false);
        m_pressed.clear();
    }

    void Post(WORD vk, bool down) {
        UINT sc = MapVirtualKey(vk, MAPVK_VK_TO_VSC);
        LPARAM lp = down
            ? (LPARAM)(1 | (sc << 16))
            : (LPARAM)(1 | (sc << 16) | (1 << 30) | (1u << 31));
        PostMessage(m_hwnd, down ? WM_KEYDOWN : WM_KEYUP, vk, lp);
    }

    HWND   m_hwnd;
    bool   m_enabled = true;
    std::mutex m_mtx;
    std::unordered_set<std::string> m_pressed;
};

// ═══════════════════════════════════════════════════════════════════
//  SECTION 4 — Overlay GUI
// ═══════════════════════════════════════════════════════════════════

struct OvState {
    float vecX{}, vecY{}, dist{};
    int   dxPx{}, dyPx{};
    bool  enabled = true;
    bool  up{}, dn{}, lt{}, rt{}, kz{}, kx{}, kc{};
    float dz = 30.f, mr = 200.f;
};

static Color Rgba(BYTE r, BYTE g, BYTE b, BYTE a) { return { a,r,g,b }; }

class Overlay {
public:
    Overlay() { InitializeCriticalSection(&m_cs); }
    ~Overlay() { DeleteCriticalSection(&m_cs); }

    bool Create(HINSTANCE hi) {
        m_hi = hi;
        GdiplusStartupInput gsi;
        GdiplusStartup(&m_gdi, &gsi, nullptr);

        WNDCLASSEXW wc{ .cbSize=sizeof(wc), .lpfnWndProc=Proc,
                         .hInstance=hi, .lpszClassName=L"MJOv" };
        RegisterClassExW(&wc);

        m_hwnd = CreateWindowExW(
            WS_EX_TOPMOST|WS_EX_LAYERED|WS_EX_TRANSPARENT|WS_EX_TOOLWINDOW,
            L"MJOv", L"", WS_POPUP,
            0,0,800,600, nullptr, nullptr, hi, nullptr);

        if (!m_hwnd) return false;

        // Colour-key magenta → transparent
        SetLayeredWindowAttributes(m_hwnd, RGB(255,0,255), 0, LWA_COLORKEY);

        BOOL ex = TRUE;
        DwmSetWindowAttribute(m_hwnd, DWMWA_EXCLUDED_FROM_PEEK, &ex, sizeof ex);

        s_self = this;
        return true;
    }

    void SetPos(int x, int y, int w, int h) {
        SetWindowPos(m_hwnd, HWND_TOPMOST, x, y, w, h,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }

    void Push(const OvState& st) {
        EnterCriticalSection(&m_cs);
        m_st = st;
        LeaveCriticalSection(&m_cs);
        InvalidateRect(m_hwnd, nullptr, FALSE);
    }

    void Close() { PostMessage(m_hwnd, WM_CLOSE, 0, 0); }

    void Run() {                          // blocks until WM_QUIT
        MSG msg{};
        while (GetMessage(&msg, nullptr, 0, 0)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        GdiplusShutdown(m_gdi);
    }

    HWND Hwnd() const { return m_hwnd; }

private:
    static LRESULT CALLBACK Proc(HWND hw, UINT msg, WPARAM wp, LPARAM lp) {
        if (msg == WM_PAINT) {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hw, &ps);
            if (s_self) s_self->Paint(hdc, hw);
            EndPaint(hw, &ps);
            return 0;
        }
        if (msg == WM_DESTROY) { PostQuitMessage(0); return 0; }
        return DefWindowProcW(hw, msg, wp, lp);
    }

    void Paint(HDC hdc, HWND hw) {
        RECT rc; GetClientRect(hw, &rc);
        int W = rc.right, H = rc.bottom;

        // ── double-buffer ──────────────────────────────────────
        HDC    mem  = CreateCompatibleDC(hdc);
        HBITMAP bmp = CreateCompatibleBitmap(hdc, W, H);
        SelectObject(mem, bmp);

        HBRUSH bg = CreateSolidBrush(RGB(255,0,255));  // magenta = transparent
        FillRect(mem, &rc, bg);
        DeleteObject(bg);

        EnterCriticalSection(&m_cs);
        OvState st = m_st;
        LeaveCriticalSection(&m_cs);

        Graphics g(mem);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetTextRenderingHint(TextRenderingHintAntiAlias);

        float cx = W/2.f, cy = H/2.f;

        if (!st.enabled) {
            // ── paused state ────────────────────────────────────
            SolidBrush dim(Rgba(0,0,0,140));
            g.FillRectangle(&dim, 0, 0, W, H);
            Font font(L"Consolas", 18, FontStyleBold, UnitPixel);
            SolidBrush tb(Rgba(255,80,80,220));
            StringFormat sf;
            sf.SetAlignment(StringAlignmentCenter);
            sf.SetLineAlignment(StringAlignmentCenter);
            RectF bounds(0.f, 0.f, (float)W, (float)H);
            g.DrawString(L"PAUSED  (F8 to resume)", -1, &font, bounds, &sf, &tb);
        } else {
            // ── crosshair ───────────────────────────────────────
            Pen cp(Rgba(255,255,255,80), 1.f);
            cp.SetDashStyle(DashStyleDash);
            g.DrawLine(&cp, cx-18, cy, cx+18, cy);
            g.DrawLine(&cp, cx, cy-18, cx, cy+18);

            // ── deadzone circle ─────────────────────────────────
            float dz = st.dz;
            SolidBrush dzFill(Rgba(255,255,255,20));
            Pen dzPen(Rgba(255,255,255,60), 1.f);
            dzPen.SetDashStyle(DashStyleDot);
            g.FillEllipse(&dzFill, cx-dz, cy-dz, dz*2, dz*2);
            g.DrawEllipse(&dzPen, cx-dz, cy-dz, dz*2, dz*2);

            // ── max-radius ring ─────────────────────────────────
            float mr = st.mr;
            Pen mrPen(Rgba(255,255,255,18), 1.f);
            mrPen.SetDashStyle(DashStyleDot);
            g.DrawEllipse(&mrPen, cx-mr, cy-mr, mr*2, mr*2);

            // ── vector arrow ────────────────────────────────────
            if (st.dist > 0.f) {
                float ang    = std::atan2f((float)st.dyPx, (float)st.dxPx);
                float visLen = (std::min(st.dist, mr) / mr) * mr;
                float ex = cx + visLen * std::cos(ang);
                float ey = cy + visLen * std::sin(ang);

                Pen ap(Rgba(80,200,255,220), 2.5f);
                g.DrawLine(&ap, cx, cy, ex, ey);

                // arrowhead
                float al = 11.f;
                PointF tip[3] = {
                    {ex, ey},
                    {ex - al*std::cos(ang-0.38f), ey - al*std::sin(ang-0.38f)},
                    {ex - al*std::cos(ang+0.38f), ey - al*std::sin(ang+0.38f)},
                };
                SolidBrush ab(Rgba(80,200,255,220));
                g.FillPolygon(&ab, tip, 3);

                SolidBrush dot(Rgba(80,200,255,255));
                g.FillEllipse(&dot, ex-4, ey-4, 8.f, 8.f);
            }

            // ── HUD text ────────────────────────────────────────
            Font hf(L"Consolas", 9, FontStyleRegular, UnitPixel);
            SolidBrush ht(Rgba(255,255,255,185));
            wchar_t buf[64];
            swprintf_s(buf, L"X %+.2f",  st.vecX); g.DrawString(buf,-1,&hf,PointF(6,6), &ht);
            swprintf_s(buf, L"Y %+.2f",  st.vecY); g.DrawString(buf,-1,&hf,PointF(6,22),&ht);
            swprintf_s(buf, L"D %5.1f",  st.dist); g.DrawString(buf,-1,&hf,PointF(6,38),&ht);

            // ── key badges ──────────────────────────────────────
            struct { const wchar_t* lbl; bool on; } badges[] = {
                {L"↑",st.up},{L"↓",st.dn},{L"←",st.lt},{L"→",st.rt},
                {L"Z",st.kz},{L"X",st.kx},{L"C",st.kc}
            };
            float bx = 10.f, by = (float)H - 34.f;
            Font bf(L"Consolas", 10, FontStyleBold, UnitPixel);
            StringFormat bsf;
            bsf.SetAlignment(StringAlignmentCenter);
            bsf.SetLineAlignment(StringAlignmentCenter);
            for (auto& b : badges) {
                Color fc = b.on ? Rgba(80,255,120,210) : Rgba(255,255,255,35);
                Color tc = b.on ? Rgba(0,0,0,230)      : Rgba(255,255,255,110);
                SolidBrush fb(fc), tb2(tc);
                g.FillEllipse(&fb, bx, by, 28.f, 28.f);
                RectF br(bx, by, 28.f, 28.f);
                g.DrawString(b.lbl, -1, &bf, br, &bsf, &tb2);
                bx += 34.f;
            }

            // ── hotkey hint ─────────────────────────────────────
            Font hkf(L"Consolas", 8, FontStyleRegular, UnitPixel);
            SolidBrush hkb(Rgba(255,255,255,60));
            g.DrawString(L"F8 pause  F9 recenter  ESC quit",
                         -1, &hkf, PointF(6.f, (float)H-14.f), &hkb);
        }

        BitBlt(hdc, 0,0,W,H, mem, 0,0, SRCCOPY);
        DeleteObject(bmp);
        DeleteDC(mem);
    }

    static Overlay*  s_self;
    HINSTANCE        m_hi     = nullptr;
    HWND             m_hwnd   = nullptr;
    ULONG_PTR        m_gdi    = 0;
    OvState          m_st     {};
    CRITICAL_SECTION m_cs     {};
};
Overlay* Overlay::s_self = nullptr;

// ═══════════════════════════════════════════════════════════════════
//  SECTION 5 — Window Picker (console UI)
// ═══════════════════════════════════════════════════════════════════

static WindowInfo PickWindow() {
    AllocConsole();
    FILE *fout{}, *fin{};
    freopen_s(&fout, "CONOUT$", "w", stdout);
    freopen_s(&fin,  "CONIN$",  "r", stdin);
    std::wcout.clear(); std::wcin.clear();

    auto wins = EnumerateWindows();

    std::wcout << L"\n  ╔══════════════════════════════════════════╗\n"
               << L"  ║   Mouse Joystick Overlay — Window Picker ║\n"
               << L"  ╚══════════════════════════════════════════╝\n\n";

    for (int i = 0; i < (int)wins.size(); ++i)
        std::wcout << L"  [" << i << L"] "
                   << wins[i].title.substr(0, 55)
                   << L"  (" << wins[i].processName << L")\n";

    std::wcout << L"\n  Select index: ";
    int idx = 0;
    std::wcin >> idx;

    FreeConsole();

    if (idx < 0 || idx >= (int)wins.size()) std::exit(1);
    return wins[idx];
}

// ═══════════════════════════════════════════════════════════════════
//  SECTION 6 — Main / Update Loop
// ═══════════════════════════════════════════════════════════════════

static std::atomic<bool> g_run     { true };
static std::atomic<bool> g_enabled { true };

static void UpdateLoop(WindowTracker& tracker,
                       KeySender&     sender,
                       MouseEngine&   mouse,
                       Overlay&       overlay)
{
    constexpr auto FRAME = std::chrono::milliseconds(16);  // ~60 fps

    bool prevF8 = false, prevF9 = false;

    while (g_run) {
        auto t0 = std::chrono::steady_clock::now();

        // ── hotkeys ───────────────────────────────────────────────
        if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) {
            g_run = false;
            overlay.Close();
            break;
        }
        bool f8 = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
        bool f9 = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;

        if (f8 && !prevF8) {
            bool ne = !g_enabled;
            g_enabled = ne;
            sender.SetEnabled(ne);
        }
        if (f9 && !prevF9) {
            // Recenter: nothing special needed — centre is always recomputed
            // from the live window rect on every frame.
        }
        prevF8 = f8; prevF9 = f9;

        // ── update mouse engine centre to window centre ────────────
        auto& win = tracker.Get();
        auto  ctr = win.center();
        mouse.SetCenter(ctr.x, ctr.y);

        // ── read input state ─────────────────────────────────────
        JoyVec   vec = mouse.Vec();
        BtnState btn = mouse.Btn();

        // ── foreground gate ───────────────────────────────────────
        bool fg = tracker.IsForeground();
        if (g_enabled && fg) sender.Update(vec, btn);
        else                  sender.ReleaseAll();

        // ── push overlay state ────────────────────────────────────
        bool act = g_enabled && fg;
        OvState st{};
        st.vecX = vec.x;  st.vecY = vec.y;
        st.dist = vec.dist;
        st.dxPx = vec.dxPx; st.dyPx = vec.dyPx;
        st.enabled = act;
        st.up = act && vec.y < -AXIS_THR;
        st.dn = act && vec.y >  AXIS_THR;
        st.lt = act && vec.x < -AXIS_THR;
        st.rt = act && vec.x >  AXIS_THR;
        st.kz = act && btn.left;
        st.kx = act && btn.right;
        st.kc = act && btn.middle;
        st.dz = mouse.GetDz();
        st.mr = mouse.GetMr();
        overlay.Push(st);

        // ── pace ──────────────────────────────────────────────────
        auto dt = std::chrono::steady_clock::now() - t0;
        if (dt < FRAME) std::this_thread::sleep_for(FRAME - dt);
    }
}

// ── entry point ───────────────────────────────────────────────────

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int) {
    WindowInfo target = PickWindow();

    // Build subsystems
    Overlay       overlay;
    MouseEngine   mouse(30.f, 200.f);
    KeySender     sender(target.hwnd);

    if (!overlay.Create(hInst)) return 1;

    overlay.SetPos(target.x(), target.y(), target.width(), target.height());

    WindowTracker tracker(target, [&](const WindowInfo& w) {
        overlay.SetPos(w.x(), w.y(), w.width(), w.height());
    });
    tracker.Start();

    mouse.Install();

    // Update loop on worker thread (LL hook needs its own pump on main)
    std::thread worker([&] {
        UpdateLoop(tracker, sender, mouse, overlay);
    });

    overlay.Run();      // Win32 message pump — blocks until WM_QUIT

    g_run = false;
    mouse.Uninstall();
    tracker.Stop();
    sender.ReleaseAll();
    if (worker.joinable()) worker.join();

    return 0;
}