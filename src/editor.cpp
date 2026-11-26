#include "controls.h"
#include "pluginterfaces/gui/iplugviewcontentscalesupport.h"
#include <windows.h>
#include <commctrl.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cwchar>

namespace isolator {
using namespace Steinberg;
using namespace Steinberg::Vst;
namespace {
constexpr int width = 366, height = 225;
constexpr int firstButton = 400, bypassButton = 403, latencyButton = 404, firstAdvanceButton = 405, stereoButton = 408;
constexpr COLORREF panel = RGB(25, 30, 35), button = RGB(42, 50, 57), edge = RGB(68, 80, 88);
constexpr COLORREF accent = RGB(116, 223, 194), ink = RGB(236, 240, 241), muted = RGB(179, 193, 196);

class StrengthEditor final : public EditorView, public IPlugViewContentScaleSupport {
public:
    explicit StrengthEditor(EditController* owner) : EditorView(owner) { setRect({0, 0, width, height}); }
    ~StrengthEditor() override { destroy(); }
    DEFINE_INTERFACES
        DEF_INTERFACE(IPlugViewContentScaleSupport)
    END_DEFINE_INTERFACES(EditorView)
    REFCOUNT_METHODS(EditorView)
    tresult PLUGIN_API isPlatformTypeSupported(FIDString type) override {
        return type && std::strcmp(type, kPlatformTypeHWND) == 0 ? kResultTrue : kResultFalse;
    }
    tresult PLUGIN_API attached(void* parent, FIDString type) override {
        if (isAttached() || !parent || isPlatformTypeSupported(type) != kResultTrue) return kResultFalse;
        window = CreateWindowExW(WS_EX_CONTROLPARENT, L"STATIC", L"", WS_CHILD | WS_CLIPCHILDREN,
                                 0, 0, rect.getWidth(), rect.getHeight(), static_cast<HWND>(parent),
                                 nullptr, nullptr, nullptr);
        if (!window || !SetWindowSubclass(window, windowProc, 1, reinterpret_cast<DWORD_PTR>(this))) {
            destroy();
            return kResultFalse;
        }
        title = control(L"STATIC", L"Noise reduction", SS_CENTERIMAGE);
        constexpr const wchar_t* labels[] = {L"100%", L"80%", L"50%"};
        for (int i = 0; i < 3; ++i) {
            buttons[i] = control(L"BUTTON", labels[i], BS_AUTORADIOBUTTON | BS_PUSHLIKE |
                                 (i == 0 ? WS_GROUP | WS_TABSTOP : 0), firstButton + i);
        }
        latency = control(L"BUTTON", L"Low latency", BS_AUTOCHECKBOX | BS_PUSHLIKE | WS_GROUP | WS_TABSTOP, latencyButton);
        bypass = control(L"BUTTON", L"Bypass", BS_AUTOCHECKBOX | BS_PUSHLIKE | WS_GROUP | WS_TABSTOP, bypassButton);
        advanceTitle = control(L"STATIC", L"Extra synthesis advance", SS_CENTERIMAGE);
        constexpr const wchar_t* advances[] = {L"0 ms", L"5 ms", L"10 ms"};
        for (int i = 0; i < 3; ++i) {
            advanceButtons[i] = control(L"BUTTON", advances[i], BS_AUTORADIOBUTTON | BS_PUSHLIKE |
                                        (i == 0 ? WS_GROUP | WS_TABSTOP : 0), firstAdvanceButton + i);
        }
        stereo = control(L"BUTTON", L"Dual mono", BS_AUTOCHECKBOX | BS_PUSHLIKE | WS_GROUP | WS_TABSTOP, stereoButton);
        gpu = control(L"STATIC", L"", SS_CENTERIMAGE);
        coverage = control(L"STATIC", L"", SS_CENTERIMAGE | SS_RIGHT);
        if (!title || !advanceTitle || !gpu || !coverage || !bypass || !latency || !stereo ||
            std::find(buttons.begin(), buttons.end(), nullptr) != buttons.end() ||
            std::find(advanceButtons.begin(), advanceButtons.end(), nullptr) != advanceButtons.end()) {
            destroy();
            return kResultFalse;
        }
        layout();
        sync();
        SetTimer(window, 1, 100, nullptr);
        ShowWindow(window, SW_SHOWNOACTIVATE);
        return EditorView::attached(parent, type);
    }
    tresult PLUGIN_API removed() override {
        destroy();
        return EditorView::removed();
    }
    tresult PLUGIN_API onSize(ViewRect* size) override {
        if (!size) return kInvalidArgument;
        EditorView::onSize(size);
        if (window) MoveWindow(window, 0, 0, rect.getWidth(), rect.getHeight(), TRUE);
        return kResultOk;
    }
    tresult PLUGIN_API setContentScaleFactor(ScaleFactor value) override {
        if (!std::isfinite(value) || value <= 0 || value > 8) return kInvalidArgument;
        ViewRect wanted(0, 0, std::lround(width * value), std::lround(height * value));
        if (plugFrame && isAttached()) return plugFrame->resizeView(this, &wanted);
        return onSize(&wanted);
    }
private:
    HWND window{}, title{}, advanceTitle{}, bypass{}, latency{}, stereo{}, gpu{}, coverage{};
    std::array<HWND, 3> buttons{}, advanceButtons{};
    std::array<HFONT, 3> fonts{};
    int lastGpu = -1, lastCoverage = -1;
    float scale = 1.f;

    HWND control(const wchar_t* cls, const wchar_t* label, DWORD style, int id = 0) {
        HWND child = CreateWindowExW(0, cls, label, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, window,
                                     reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), nullptr, nullptr);
        if (id && child && !SetWindowSubclass(child, buttonProc, 1, reinterpret_cast<DWORD_PTR>(this))) {
            DestroyWindow(child);
            return nullptr;
        }
        return child;
    }
    void destroy() {
        if (window) DestroyWindow(window);
        window = title = advanceTitle = bypass = latency = stereo = gpu = coverage = nullptr;
        buttons.fill(nullptr);
        advanceButtons.fill(nullptr);
        for (auto& font : fonts) { if (font) DeleteObject(font); font = nullptr; }
        lastGpu = lastCoverage = -1;
    }
    int px(int logical) const { return static_cast<int>(std::lround(logical * scale)); }
    void layout() {
        if (!coverage) return;
        RECT area{};
        GetClientRect(window, &area);
        scale = std::max(.25f, std::min(static_cast<float>(area.right) / width, static_cast<float>(area.bottom) / height));
        const auto previous = fonts;
        auto font = [&](int size, int weight) {
            return CreateFontW(-px(size), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                               OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                               DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
        };
        fonts = {font(19, FW_SEMIBOLD), font(21, FW_SEMIBOLD), font(13, FW_NORMAL)};
        auto place = [&](HWND h, int x, int y, int w, int hgt, HFONT f) {
            SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(f), FALSE);
            MoveWindow(h, px(x), px(y), px(x+w)-px(x), px(y+hgt)-px(y), TRUE);
        };
        place(title, 18, 12, 150, 29, fonts[0]);
        place(latency, 176, 14, 93, 25, fonts[2]);
        place(bypass, 277, 14, 71, 25, fonts[2]);
        place(advanceTitle, 18, 101, 229, 29, fonts[0]);
        place(stereo, 255, 103, 93, 25, fonts[2]);
        for (int i = 0; i < 3; ++i) {
            place(buttons[i], 18 + 113*i, 49, 104, 44, fonts[1]);
            place(advanceButtons[i], 18 + 113*i, 137, 104, 40, fonts[0]);
        }
        place(gpu, 18, 187, 169, 25, fonts[2]);
        place(coverage, 190, 187, 158, 25, fonts[2]);
        for (auto old : previous) if (old) DeleteObject(old);
        InvalidateRect(window, nullptr, TRUE);
    }
    static void check(HWND h, bool on) {
        if ((SendMessageW(h, BM_GETCHECK, 0, 0) == BST_CHECKED) != on) {
            SendMessageW(h, BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0);
            InvalidateRect(h, nullptr, FALSE);
        }
    }
    void sync() {
        const int selected = presetIndex(controller->getParamNormalized(Strength));
        const int advance = presetIndex(controller->getParamNormalized(SynthesisAdvance));
        for (int i = 0; i < 3; ++i) {
            check(buttons[i], selected == 2-i);
            check(advanceButtons[i], advance == i);
        }
        check(bypass, controller->getParamNormalized(Bypass) >= .5);
        check(latency, controller->getParamNormalized(Latency) >= .5);
        check(stereo, controller->getParamNormalized(StereoInput) >= .5);
        const int state = std::clamp(static_cast<int>(std::lround(controller->getParamNormalized(Engine) * 3)), 0, 3);
        if (state != lastGpu) {
            constexpr const wchar_t* states[] = {L"GPU engine   Loading", L"GPU engine   Ready",
                                                  L"GPU engine   Catching up", L"GPU engine   Unavailable"};
            SetWindowTextW(gpu, states[state]);
            lastGpu = state;
        }
        const int percent = static_cast<int>(std::lround(100. * controller->getParamNormalized(Coverage)));
        if (percent != lastCoverage) {
            wchar_t label[48]{};
            swprintf_s(label, L"Processed audio   %d%%", percent);
            SetWindowTextW(coverage, label);
            lastCoverage = percent;
        }
    }
    void click(int id) {
        ParamID param;
        double value;
        if (id == stereoButton || id == bypassButton || id == latencyButton) {
            param = id == stereoButton ? StereoInput : id == bypassButton ? Bypass : Latency;
            value = controller->getParamNormalized(param) >= .5 ? 0. : 1.;
        } else if (id >= firstAdvanceButton) {
            param = SynthesisAdvance;
            value = normalizedPreset(id - firstAdvanceButton);
        } else {
            param = Strength;
            value = normalizedPreset(2 - (id - firstButton));
        }
        if (controller->getParamNormalized(param) != value) {
            controller->beginEdit(param);
            controller->setParamNormalized(param, value);
            controller->performEdit(param, value);
            controller->endEdit(param);
        }
        sync();
    }
    void drawButton(HWND h, HDC dc) const {
        RECT bounds{};
        GetClientRect(h, &bounds);
        const bool on = SendMessageW(h, BM_GETCHECK, 0, 0) == BST_CHECKED;
        int saved = SaveDC(dc);
        const auto brush = static_cast<HBRUSH>(GetStockObject(DC_BRUSH));
        SetDCBrushColor(dc, panel);
        FillRect(dc, &bounds, brush);
        SelectObject(dc, brush);
        SelectObject(dc, GetStockObject(DC_PEN));
        SetDCBrushColor(dc, on ? accent : button);
        SetDCPenColor(dc, on ? accent : edge);
        RoundRect(dc, 0, 0, bounds.right, bounds.bottom, px(10), px(10));
        SelectObject(dc, reinterpret_cast<HFONT>(SendMessageW(h, WM_GETFONT, 0, 0)));
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, on ? panel : ink);
        wchar_t label[16]{};
        GetWindowTextW(h, label, 16);
        DrawTextW(dc, label, -1, &bounds, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        if (GetFocus() == h && !(SendMessageW(h, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS)) {
            InflateRect(&bounds, -px(5), -px(5));
            DrawFocusRect(dc, &bounds);
        }
        RestoreDC(dc, saved);
    }
    static LRESULT CALLBACK buttonProc(HWND h, UINT msg, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR data) {
        auto self = reinterpret_cast<StrengthEditor*>(data);
        if (msg == WM_ERASEBKGND) return 1;
        if (msg == WM_PRINTCLIENT) { self->drawButton(h, reinterpret_cast<HDC>(w)); return 0; }
        if (msg == WM_PAINT) {
            PAINTSTRUCT ps{};
            HDC dc = BeginPaint(h, &ps);
            self->drawButton(h, dc);
            EndPaint(h, &ps);
            return 0;
        }
        if (msg == WM_KEYDOWN && h != self->bypass && h != self->latency && h != self->stereo &&
            (w == VK_LEFT || w == VK_RIGHT || w == VK_UP || w == VK_DOWN)) {
            int delta = w == VK_LEFT || w == VK_UP ? -1 : 1;
            const int id = GetDlgCtrlID(h);
            const bool advance = id >= firstAdvanceButton;
            const auto& group = advance ? self->advanceButtons : self->buttons;
            auto next = group[(id - (advance ? firstAdvanceButton : firstButton) + delta + 3) % 3];
            SetFocus(next);
            SendMessageW(next, BM_CLICK, 0, 0);
            return 0;
        }
        if (msg == WM_NCDESTROY) RemoveWindowSubclass(h, buttonProc, 1);
        return DefSubclassProc(h, msg, w, l);
    }
    static LRESULT CALLBACK windowProc(HWND h, UINT msg, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR data) {
        auto self = reinterpret_cast<StrengthEditor*>(data);
        switch (msg) {
        case WM_COMMAND:
            if (HIWORD(w) == BN_CLICKED && LOWORD(w) >= firstButton && LOWORD(w) <= stereoButton) {
                self->click(LOWORD(w));
                return 0;
            }
            break;
        case WM_TIMER:
            if (w == 1) self->sync();
            return 0;
        case WM_SIZE:
            self->layout();
            return 0;
        case WM_CTLCOLORSTATIC: {
            auto dc = reinterpret_cast<HDC>(w);
            SetBkColor(dc, panel);
            const auto label = reinterpret_cast<HWND>(l);
            SetTextColor(dc, (label == self->title || label == self->advanceTitle) ? ink : muted);
            SetDCBrushColor(dc, panel);
            return reinterpret_cast<LRESULT>(GetStockObject(DC_BRUSH));
        }
        case WM_PAINT: case WM_PRINTCLIENT: case WM_ERASEBKGND: {
            PAINTSTRUCT ps{};
            HDC dc = msg == WM_PAINT ? BeginPaint(h, &ps) : reinterpret_cast<HDC>(w);
            RECT area{};
            GetClientRect(h, &area);
            SetDCBrushColor(dc, panel);
            FillRect(dc, &area, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
            if (msg == WM_PAINT) EndPaint(h, &ps);
            return 1;
        }
        case WM_NCDESTROY:
            self->window = nullptr;
            KillTimer(h, 1);
            RemoveWindowSubclass(h, windowProc, 1);
            break;
        }
        return DefSubclassProc(h, msg, w, l);
    }
};
}
IPlugView* makeEditor(EditController* controller) { return new StrengthEditor(controller); }
}
