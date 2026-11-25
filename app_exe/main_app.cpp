#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <commdlg.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "db_and_ipc.h"

static_assert(sizeof(void*) == 4, "statsEdps.exe MUST be compiled as 32-bit x86 (Win32) to inject statsEdps_hooks.dll into nfs.exe!");

// ============================================================================
// UI Geometry & Palette (matches our dark theme, 860 x 948 client area)
// ============================================================================
static constexpr int WIN_W   = 860;
static constexpr int WIN_H   = 948;
static constexpr float MPH_TO_KMH = 1.609344f;

namespace Col {
    static constexpr COLORREF BG          = RGB(30, 30, 30);
    static constexpr COLORREF PANEL_BG    = RGB(34, 34, 34);
    static constexpr COLORREF TAB_STRIP   = RGB(24, 24, 24);
    static constexpr COLORREF TAB_INACT   = RGB(28, 28, 28);
    static constexpr COLORREF HEADER_BG   = RGB(46, 46, 46);
    static constexpr COLORREF ROW_A       = RGB(34, 34, 34);
    static constexpr COLORREF ROW_B       = RGB(40, 40, 40);
    static constexpr COLORREF ROW_SEL     = RGB(44, 58, 74);
    static constexpr COLORREF GAP_BG      = RGB(22, 22, 22);
    static constexpr COLORREF GRID        = RGB(62, 62, 62);
    static constexpr COLORREF BORDER      = RGB(76, 76, 76);
    static constexpr COLORREF TEXT        = RGB(240, 240, 240);
    static constexpr COLORREF SYM         = RGB(215, 225, 240);
    static constexpr COLORREF NUM         = RGB(135, 215, 255);
    static constexpr COLORREF DIM         = RGB(155, 160, 170);
    static constexpr COLORREF TITLE       = RGB(230, 230, 230);
    static constexpr COLORREF BTN_BG      = RGB(50, 54, 60);
    static constexpr COLORREF BTN_HOVER   = RGB(64, 70, 78);
    static constexpr COLORREF TRACK_BLOCK = RGB(14, 14, 14);
    static constexpr COLORREF TRACK_AVAIL = RGB(72, 76, 84);
    static constexpr COLORREF TRACK_TICK  = RGB(38, 41, 46);
    static constexpr COLORREF TRACK_EDGE  = RGB(210, 216, 224);
    static constexpr COLORREF TRACK_ZERO  = RGB(185, 192, 204);
    static constexpr COLORREF CAP_BG      = RGB(58, 60, 64);
    static constexpr COLORREF CAP_ARROW   = RGB(225, 228, 232);
    static constexpr COLORREF LOCK_TRACK  = RGB(64, 66, 62);
    static constexpr COLORREF LOCK_CAP    = RGB(48, 50, 48);
    static constexpr COLORREF LOCK_ARROW  = RGB(118, 120, 114);
    static constexpr COLORREF LOCK_TEXT   = RGB(132, 134, 130);
    static constexpr COLORREF LOCK_THUMB  = RGB(172, 175, 178);
    static constexpr COLORREF SLIDER_POS  = RGB(75, 220, 115);
    static constexpr COLORREF SLIDER_NEG  = RGB(240, 95, 95);
    static constexpr COLORREF SLIDER_ACC  = RGB(245, 200, 65);
    static constexpr COLORREF SCULPT_VAL  = RGB(255, 220, 120);
}

struct HitRect {
    int id = 0;
    RECT rc{};
};

struct SliderHit {
    int sliderIdx = -1;
    uint32_t hash = 0;
    int capL1 = 0, capL2 = 0, tx1 = 0, tx2 = 0, capR1 = 0, capR2 = 0, y1 = 0, y2 = 0;
};

enum BtnId {
    BTN_MODE_TOGGLE = 1,
    BTN_TEMP_SNAP,
    BTN_TOPMOST,
    BTN_KMH,
    BTN_BP_RESCAN,
    BTN_BP_PREV,
    BTN_BP_NEXT,
    BTN_BP_AUTO,
    BTN_TAB_1,
    BTN_TAB_2,
    BTN_TAB_3,
    BTN_TAB_4,
    BTN_PART_PREV,
    BTN_PART_INSTALL,
    BTN_PART_NEXT,
    BTN_SAME_UP,
    BTN_SAME_DN,
    BTN_DIFF_UP,
    BTN_DIFF_DN,
    BTN_RESET_SLIDERS,
};

struct ModeMeta {
    const char* key;
    const char* name;
    const char* vSym;
    int vIdx; // 0-based index into curVals[21]
};

static ModeMeta GetModeMeta(uint32_t raw) {
    switch (raw) {
        case 0: return { "0", "ГРИП",     "V_grip",  7  };
        case 1: return { "1", "ДРИФТ",    "V_drift", 8  };
        case 2: return { "2", "ДРЭГ",     "V_drag",  9  };
        case 3: return { "3", "СКОРОСТЬ", "V_speed", 10 };
        default: return { "0", "РЕЖИМ",   "V",       7  };
    }
}

struct ParamRowDef {
    int valIdx; // -1 = dynamic mode rating, >=0 = index into curVals[21]
    const char* symMph;
    const char* titleMph;
    const char* fmt;
    bool isConst;
    int group;
    float minScale;
    bool invertCol;
    int altKind; // 0=none, 1=t160, 2=t100, 3=kmh
    const char* symKmh;
    const char* titleKmh;
};

static constexpr ParamRowDef PARAM_ROWS[17] = {
    {  0, "P1",      "Кривизна = 2.25",            "%.2f", true,  1, 0.1f,    false, 0, nullptr,  nullptr },
    {  1, "P2",      "Мощность + NOS",             "%.6f", false, 1, 1.0f,    false, 0, nullptr,  nullptr },
    {  2, "P3",      "Мощность без NOS",           "%.6f", false, 1, 1.0f,    false, 0, nullptr,  nullptr },
    {  3, "P4",      "Поперечный зацеп",           "%.6f", false, 1, 0.05f,   false, 0, nullptr,  nullptr },
    {  4, "P5",      "Продольный зацеп",           "%.6f", false, 1, 0.05f,   false, 0, nullptr,  nullptr },
    {  5, "P6",      "Прижим / масса",             "%.6f", false, 1, 0.0005f, false, 0, nullptr,  nullptr },
    {  6, "P7",      "Драг / масса",               "%.6f", false, 1, 0.0001f, false, 0, nullptr,  nullptr },
    { -1, "V_grip",  "Рейтинг режима (ГРИП)",      "%.6f", false, 2, 0.05f,   false, 0, nullptr,  nullptr },
    { 12, "m",       "Масса (кг)",                 "%.2f", false, 3, 50.0f,   true,  0, nullptr,  nullptr },
    { 13, "Tq",      "Пик момента (ft-lb)",        "%.2f", false, 3, 15.0f,   false, 0, nullptr,  nullptr },
    { 14, "RPM_tq",  "Обороты пика момента (RPM)", "%.1f", false, 3, 200.0f,  false, 0, nullptr,  nullptr },
    { 15, "HP",      "Мощность (HP)",              "%.2f", false, 3, 15.0f,   false, 0, nullptr,  nullptr },
    { 16, "m/HP",    "Масса / Мощность (кг/л.с.)", "%.2f", false, 3, 0.3f,    true,  0, nullptr,  nullptr },
    { 17, "t_0-100", "Разгон 0–100 mph (с)",       "%.3f", false, 4, 0.5f,    true,  1, "t_0-160","Разгон 0–160 км/ч (с)" },
    { 18, "t_0-60",  "Разгон 0–60 mph (с)",        "%.3f", false, 4, 0.25f,   true,  2, "t_0-100","Разгон 0–100 км/ч (с)" },
    { 19, "t_1/4",   "1/4 мили (с)",               "%.3f", false, 4, 0.5f,    true,  0, nullptr,  nullptr },
    { 20, "v_top",   "Макс. скорость (mph)",       "%.2f", false, 4, 10.0f,   false, 3, "v_top",  "Макс. скорость (км/ч)" },
};

// ============================================================================
// Application State
// ============================================================================
struct DetailRowView {
    int slotNum = 0;
    std::string key, name;
    uint32_t w1 = 0, w2 = 0, idx = 0, viewIdx = 0, mCount = 0;
    uint32_t mKey = 0, mLayout = 0, arr6B8 = 0, infoColl = 0, arr3EF = 0;
    std::string layoutStr, visAttrs;
    std::string stageName, brandName, fullLabel, brandHash, physHex, parName, physAttrs;
    bool isRawBrand = false, hasPhys = false, hasMeta3EF = false;
    uint32_t physCol = 0, cost = 0;
    uint32_t dws[14] = {};
    std::string strs;
    std::vector<MatchSamePhysRec> samePhys;
    std::vector<MatchDiffPhysRec> diffPhys;
};

struct AppState {
    HWND hwnd = nullptr;
    Databases dbs;
    GameBridge bridge;
    ShmSharedState shmCopy{};
    uint32_t lastFastSeq = 0;
    uint32_t lastCatSeq  = 0;

    bool useStockDB = true;
    bool stayOnTop  = false;
    bool useKmh     = false;
    bool hasTempInit = false;
    std::array<float, 21> tempInitVals{};

    std::string activeBaseHash, activeModeKey;
    std::string statusLine = "Статус: Готов к работе";
    std::string flashMsg;
    bool flashOk = true;
    uint32_t flashTick = 0;

    int activeTab = 1; // 1=Тюнинг, 2=Совпадения, 3=Расчеты, 4=Data Info
    int selectedDetailRow = 0; // 0..9
    int browseIdx = -1;        // -1 = installed idx
    int scrollSame = 0;
    int scrollDiff = 0;
    uint64_t lastDetailStateHash = 0;

    DetailRowView detailRows[NUM_INSPECT_SLOTS];

    // Interactive state
    std::vector<HitRect>   buttons;
    std::vector<SliderHit> sliderHits;
    int draggingSliderIdx = -1;
    int hoverBtnId = 0;

    // Cached GDI fonts & backbuffer
    HFONT fnt9  = nullptr, fnt9B = nullptr, fnt9I = nullptr;
    HFONT fnt8  = nullptr, fnt8B = nullptr, fnt8I = nullptr, fnt8BI = nullptr;
    HFONT fnt7  = nullptr, fnt7B = nullptr, fnt10 = nullptr, fnt10I = nullptr;
    HDC     memDC  = nullptr;
    HBITMAP memBmp = nullptr;
    int     bufW   = 0, bufH = 0;

    void initFonts() {
        auto mk = [](int px, int wt, BOOL it) -> HFONT {
            return CreateFontW(-px, 0, 0, 0, wt, it, FALSE, FALSE, DEFAULT_CHARSET,
                               OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                               DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
        };
        fnt10  = mk(14, FW_NORMAL, FALSE);
        fnt10I = mk(14, FW_NORMAL, TRUE);
        fnt9   = mk(12, FW_NORMAL, FALSE);
        fnt9B  = mk(12, FW_BOLD,   FALSE);
        fnt9I  = mk(12, FW_NORMAL, TRUE);
        fnt8   = mk(11, FW_NORMAL, FALSE);
        fnt8B  = mk(11, FW_BOLD,   FALSE);
        fnt8I  = mk(11, FW_NORMAL, TRUE);
        fnt8BI = mk(11, FW_BOLD,   TRUE);
        fnt7   = mk(10, FW_NORMAL, FALSE);
        fnt7B  = mk(10, FW_BOLD,   FALSE);
    }

    void freeGdi() {
        HFONT* fs[] = { &fnt9, &fnt9B, &fnt9I, &fnt8, &fnt8B, &fnt8I, &fnt8BI, &fnt7, &fnt7B, &fnt10, &fnt10I };
        for (HFONT* f : fs) { if (*f) { DeleteObject(*f); *f = nullptr; } }
        if (memBmp) { DeleteObject(memBmp); memBmp = nullptr; }
        if (memDC)  { DeleteDC(memDC); memDC = nullptr; }
    }

    void rebuildDetailRows() {
        std::string baseH = shmCopy.baseHash;
        std::string canonH = ProStreetMeta::canonicalCarHash(baseH);
        const ProfileCarEntry* dbCar = dbs.getProfileCar(baseH);

        for (int i = 0; i < NUM_INSPECT_SLOTS; ++i) {
            DetailRowView& rv = detailRows[i];
            rv = DetailRowView{};
            rv.slotNum = static_cast<int>(INSPECT_SLOT_DEFS[i].slot);
            rv.key     = INSPECT_SLOT_DEFS[i].key;
            rv.name    = INSPECT_SLOT_DEFS[i].nameUtf8;

            const ShmInspectSlotCatalog& sc = shmCopy.inspectSlots[i];
            bool hasLiveCat = (std::strcmp(shmCopy.catalogCarHash, canonH.c_str()) == 0) && (sc.meta.w1 != 0);

            rv.w1       = hasLiveCat ? sc.meta.w1 : 0;
            rv.w2       = shmCopy.inspectW2[i];
            rv.idx      = shmCopy.inspectIdx[i];
            rv.mKey     = sc.meta.mKey;
            rv.mLayout  = sc.meta.mLayout;
            rv.arr6B8   = sc.meta.arr6B8;
            rv.infoColl = sc.meta.infoColl;
            rv.arr3EF   = sc.meta.arr3EF;
            rv.layoutStr = sc.meta.layoutStr[0] ? sc.meta.layoutStr : "—";
            rv.visAttrs  = sc.meta.visAttrs;

            if (hasLiveCat && sc.meta.mCount > 0) {
                rv.mCount = sc.meta.mCount;
                uint32_t vIdx = rv.idx;
                if (activeTab == 2 && i == selectedDetailRow && browseIdx >= 0) {
                    vIdx = static_cast<uint32_t>(std::min<int>(browseIdx, (int)rv.mCount - 1));
                }
                if (vIdx >= rv.mCount) vIdx = 0;
                rv.viewIdx = vIdx;

                const ShmPartItem& pi = sc.items[vIdx];
                rv.hasPhys    = (pi.hasPhys != 0);
                rv.hasMeta3EF = (pi.hasMeta3EF != 0);
                rv.physCol    = pi.physCol;
                rv.cost       = pi.cost;
                rv.physAttrs  = pi.physAttrs;
                rv.strs       = pi.strs;
                std::memcpy(rv.dws, pi.dws, sizeof(rv.dws));

                if (rv.hasPhys) {
                    rv.physHex = ProStreetMeta::toHex8(pi.physKey);
                    const char* pn = getParKeyName(pi.parKey);
                    rv.parName = pn ? pn : ProStreetMeta::toHex8(pi.parKey);
                }

                uint32_t effW2 = (vIdx == rv.idx) ? rv.w2 : vIdx;
                auto dec = ProStreetMeta::decodeStageAndBrand(
                    rv.slotNum, effW2, rv.mCount,
                    rv.hasMeta3EF, pi.exactStage, pi.cfgInStage, pi.brandHash, &dbs.profBrandNames
                );
                rv.stageName  = dec.stageName;
                rv.brandName  = dec.brandName;
                rv.fullLabel  = dec.fullLabel;
                rv.brandHash  = dec.brandHash;
                rv.isRawBrand = dec.isRawBrand;

                if (rv.hasPhys) {
                    dbs.findMatches(baseH, rv.key, rv.physHex, rv.stageName, rv.brandHash, rv.samePhys, rv.diffPhys);
                }
            } else if (dbCar) {
                auto its = dbCar->slots.find(rv.key);
                if (its != dbCar->slots.end()) {
                    rv.mCount = static_cast<uint32_t>(its->second.mCount);
                    uint32_t vIdx = rv.idx;
                    if (activeTab == 2 && i == selectedDetailRow && browseIdx >= 0 && rv.mCount > 0) {
                        vIdx = static_cast<uint32_t>(std::min<int>(browseIdx, (int)rv.mCount - 1));
                    }
                    rv.viewIdx = vIdx;
                    auto iti = its->second.items.find((int)vIdx);
                    if (iti != its->second.items.end()) {
                        rv.stageName  = iti->second.stage;
                        rv.brandName  = iti->second.brand;
                        rv.fullLabel  = iti->second.label;
                        rv.brandHash  = iti->second.brandHash;
                        rv.isRawBrand = ProStreetMeta::isRawHex8(rv.brandName);
                        rv.physHex    = iti->second.phys;
                        rv.hasPhys    = !rv.physHex.empty();
                        rv.parName    = "—";
                        if (rv.hasPhys) {
                            dbs.findMatches(baseH, rv.key, rv.physHex, rv.stageName, rv.brandHash, rv.samePhys, rv.diffPhys);
                        }
                    }
                }
            }
        }
    }
};

static AppState g_app;

// ============================================================================
// GDI Drawing Helpers
// ============================================================================
static inline int TextWidth(HDC hdc, const std::wstring& ws) {
    SIZE sz{};
    if (!ws.empty()) GetTextExtentPoint32W(hdc, ws.c_str(), (int)ws.size(), &sz);
    return sz.cx;
}

static inline int TextHeight(HDC hdc) {
    TEXTMETRICW tm{};
    GetTextMetricsW(hdc, &tm);
    return tm.tmHeight;
}

static void FillRectCol(HDC hdc, int x1, int y1, int x2, int y2, COLORREF col) {
    RECT r = { x1, y1, x2, y2 };
    HBRUSH br = CreateSolidBrush(col);
    FillRect(hdc, &r, br);
    DeleteObject(br);
}

static void DrawLine(HDC hdc, int x1, int y1, int x2, int y2, COLORREF col) {
    HPEN pen = CreatePen(PS_SOLID, 1, col);
    HGDIOBJ old = SelectObject(hdc, pen);
    MoveToEx(hdc, x1, y1, nullptr);
    LineTo(hdc, x2, y2);
    SelectObject(hdc, old);
    DeleteObject(pen);
}

static void DrawBox(HDC hdc, int x1, int y1, int x2, int y2, COLORREF fill, COLORREF border) {
    FillRectCol(hdc, x1, y1, x2, y2, fill);
    DrawLine(hdc, x1, y1, x2, y1, border);
    DrawLine(hdc, x1, y2 - 1, x2, y2 - 1, border);
    DrawLine(hdc, x1, y1, x1, y2, border);
    DrawLine(hdc, x2 - 1, y1, x2 - 1, y2, border);
}

static void DrawTextUtf8(HDC hdc, int x, int y, const std::string& utf8, HFONT fnt, COLORREF col) {
    SelectObject(hdc, fnt);
    SetTextColor(hdc, col);
    SetBkMode(hdc, TRANSPARENT);
    std::wstring ws = Utf8ToWide(utf8);
    TextOutW(hdc, x, y, ws.c_str(), (int)ws.size());
}

static void DrawCenteredUtf8(HDC hdc, int x1, int x2, int y, const std::string& utf8, HFONT fnt, COLORREF col) {
    SelectObject(hdc, fnt);
    SetTextColor(hdc, col);
    SetBkMode(hdc, TRANSPARENT);
    std::wstring ws = Utf8ToWide(utf8);
    int w = TextWidth(hdc, ws);
    TextOutW(hdc, x1 + ((x2 - x1) - w) / 2, y, ws.c_str(), (int)ws.size());
}

static void DrawClippedUtf8(HDC hdc, int x1, int y1, int x2, int y2, const std::string& utf8, HFONT fnt, COLORREF col, UINT fmt = DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS) {
    SelectObject(hdc, fnt);
    SetTextColor(hdc, col);
    SetBkMode(hdc, TRANSPARENT);
    std::wstring ws = Utf8ToWide(utf8);
    RECT r = { x1, y1, x2, y2 };
    DrawTextW(hdc, ws.c_str(), (int)ws.size(), &r, fmt);
}

static void DrawButton(HDC hdc, int id, int x1, int y1, int x2, int y2, const std::string& label, COLORREF border = Col::BORDER, COLORREF txtCol = Col::TEXT, HFONT fnt = nullptr) {
    bool hov = (g_app.hoverBtnId == id);
    DrawBox(hdc, x1, y1, x2, y2, hov ? Col::BTN_HOVER : Col::BTN_BG, border);
    HFONT useF = fnt ? fnt : g_app.fnt8B;
    SelectObject(hdc, useF);
    int ty = y1 + ((y2 - y1) - TextHeight(hdc)) / 2;
    DrawCenteredUtf8(hdc, x1, x2, ty, label, useF, txtCol);
    g_app.buttons.push_back({ id, { x1, y1, x2, y2 } });
}

static void DrawCheckBox(HDC hdc, int id, int x, int y, const std::string& label, bool checked) {
    DrawBox(hdc, x, y + 1, x + 14, y + 15, Col::BG, checked ? Col::NUM : Col::BORDER);
    if (checked) {
        FillRectCol(hdc, x + 3, y + 4, x + 11, y + 12, Col::NUM);
    }
    DrawTextUtf8(hdc, x + 19, y, label, g_app.fnt8, Col::TEXT);
    int w = 22 + TextWidth(hdc, Utf8ToWide(label));
    g_app.buttons.push_back({ id, { x, y, x + w, y + 16 } });
}

static void DrawTriangleArrow(HDC hdc, int cx, int cy, int dir, COLORREF col) {
    HPEN pen = CreatePen(PS_SOLID, 1, col);
    HGDIOBJ old = SelectObject(hdc, pen);
    for (int dy = -3; dy <= 3; ++dy) {
        int span = 3 - std::abs(dy);
        if (dir < 0) { MoveToEx(hdc, cx + 1 - span, cy + dy, nullptr); LineTo(hdc, cx + 2, cy + dy); }
        else         { MoveToEx(hdc, cx - 1, cy + dy, nullptr);        LineTo(hdc, cx + span, cy + dy); }
    }
    SelectObject(hdc, old);
    DeleteObject(pen);
}

static void DrawStageBrandText(HDC hdc, int x, int y, const std::string& pre, const std::string& brand, const std::string& suf, bool italicBrand, bool bold, COLORREF col, int maxX) {
    if (!italicBrand || brand.empty()) {
        DrawClippedUtf8(hdc, x, y, maxX, y + 16, pre + brand + suf, bold ? g_app.fnt8B : g_app.fnt8, col);
        return;
    }
    int cx = x;
    HFONT fBase = bold ? g_app.fnt8B : g_app.fnt8;
    HFONT fItal = bold ? g_app.fnt8BI : g_app.fnt8I;
    auto put = [&](const std::string& s, HFONT f) {
        if (s.empty() || cx >= maxX) return;
        DrawClippedUtf8(hdc, cx, y, maxX, y + 16, s, f, col, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
        SelectObject(hdc, f);
        cx += TextWidth(hdc, Utf8ToWide(s));
    };
    put(pre, fBase);
    put(brand, fItal);
    put(suf, fBase);
}

// ============================================================================
// Math Formatting Helpers (Table & Formulas Tab)
// ============================================================================
static float CalcAltMetric(int altKind, const float* v) {
    if (!v) return 0.0f;
    if (altKind == 1) { // t160
        float t100 = v[17], t60 = v[18];
        if (t100 <= 0.0f) return 0.0f;
        return (t60 > 0.0f && t100 > t60) ? (t100 - 0.014515f * (t100 - t60)) : (t100 * (160.0f / 160.9344f));
    }
    if (altKind == 2) { // t100
        float t100 = v[17], t60 = v[18];
        if (t60 <= 0.0f) return 0.0f;
        return (t100 > t60) ? (t60 + 0.053428f * (t100 - t60)) : (t60 * (100.0f / 96.56064f));
    }
    if (altKind == 3) return v[20] * MPH_TO_KMH;
    return 0.0f;
}

static std::string FmtFloat(float v, const char* fmt) {
    if (!std::isfinite(v)) return "0";
    char buf[48];
    std::snprintf(buf, sizeof(buf), fmt, v);
    return buf;
}

static std::string FmtDiff(float d, const char* fmt) {
    if (!std::isfinite(d)) return "-";
    char buf[48];
    std::snprintf(buf, sizeof(buf), fmt, std::fabs(d));
    bool allZero = true;
    for (const char* p = buf; *p; ++p) {
        if (*p >= '1' && *p <= '9') { allZero = false; break; }
    }
    if (allZero) return "-";
    return (d > 0.0f ? "+" : "-") + std::string(buf);
}

static COLORREF DiffColor(float vDiff, float vInit, float minScale, bool invert) {
    if (!std::isfinite(vDiff)) return RGB(210, 210, 210);
    float ad = std::fabs(vDiff);
    if (ad < 1e-7f) return RGB(200, 200, 200);
    float baseScale = std::max(std::fabs(vInit) * 0.25f, minScale);
    float ratio = std::min(1.0f, ad / baseScale);
    float t = std::pow(ratio, 0.72f);
    bool good = invert ? (vDiff < 0.0f) : (vDiff > 0.0f);
    int tr = good ? 55 : 255, tg = good ? 235 : 72, tb = good ? 95 : 72;
    return RGB((int)(245 + (tr - 245) * t + 0.5f), (int)(245 + (tg - 245) * t + 0.5f), (int)(245 + (tb - 245) * t + 0.5f));
}

// Formula token renderer (supports fractions, subscripts, italics)
struct FTok {
    enum Kind { TXT, BOLD, NUM, DIM, VAR, SUB, FRAC };
    Kind kind;
    std::string a, b;
    std::vector<FTok> num, den;
    static FTok T(std::string s) { return { TXT,  std::move(s), "", {}, {} }; }
    static FTok B(std::string s) { return { BOLD, std::move(s), "", {}, {} }; }
    static FTok N(std::string s) { return { NUM,  std::move(s), "", {}, {} }; }
    static FTok D(std::string s) { return { DIM,  std::move(s), "", {}, {} }; }
    static FTok V(std::string m, std::string sub = "") { return { VAR, std::move(m), std::move(sub), {}, {} }; }
    static FTok S(std::string m, std::string sub = "") { return { SUB, std::move(m), std::move(sub), {}, {} }; }
    static FTok F(std::vector<FTok> n, std::vector<FTok> d) { return { FRAC, "", "", std::move(n), std::move(d) }; }
};

static int MeasureFormula(HDC hdc, const std::vector<FTok>& toks, bool big) {
    int total = 0;
    HFONT fN = big ? g_app.fnt10 : g_app.fnt9;
    HFONT fB = g_app.fnt9B;
    HFONT fI = big ? g_app.fnt10I : g_app.fnt9I;
    for (const auto& tk : toks) {
        if (tk.kind == FTok::TXT || tk.kind == FTok::DIM) { SelectObject(hdc, fN); total += TextWidth(hdc, Utf8ToWide(tk.a)); }
        else if (tk.kind == FTok::BOLD || tk.kind == FTok::NUM) { SelectObject(hdc, fB); total += TextWidth(hdc, Utf8ToWide(tk.a)); }
        else if (tk.kind == FTok::VAR || tk.kind == FTok::SUB) {
            SelectObject(hdc, tk.kind == FTok::VAR ? fI : fN);
            total += TextWidth(hdc, Utf8ToWide(tk.a)) + (tk.kind == FTok::VAR ? 1 : 0);
            if (!tk.b.empty()) { SelectObject(hdc, g_app.fnt7); total += TextWidth(hdc, Utf8ToWide(tk.b)) + 1; }
        } else if (tk.kind == FTok::FRAC) {
            int wn = MeasureFormula(hdc, tk.num, big);
            int wd = MeasureFormula(hdc, tk.den, big);
            total += std::max(wn, wd) + 16;
        }
    }
    return total;
}

static int DrawFormula(HDC hdc, int x, int yMid, const std::vector<FTok>& toks, bool big = false) {
    HFONT fN = big ? g_app.fnt10 : g_app.fnt9;
    HFONT fB = g_app.fnt9B;
    HFONT fI = big ? g_app.fnt10I : g_app.fnt9I;
    SelectObject(hdc, fN); int hMain = TextHeight(hdc);
    SelectObject(hdc, g_app.fnt7); int hSub = TextHeight(hdc);
    int yMain = yMid - hMain / 2;
    int ySub  = yMain + hMain - hSub + 2;

    for (const auto& tk : toks) {
        if (tk.kind == FTok::TXT)       { DrawTextUtf8(hdc, x, yMain, tk.a, fN, Col::TEXT);  x += TextWidth(hdc, Utf8ToWide(tk.a)); }
        else if (tk.kind == FTok::DIM)  { DrawTextUtf8(hdc, x, yMain, tk.a, fN, Col::DIM);   x += TextWidth(hdc, Utf8ToWide(tk.a)); }
        else if (tk.kind == FTok::BOLD) { DrawTextUtf8(hdc, x, yMain, tk.a, fB, Col::TITLE); x += TextWidth(hdc, Utf8ToWide(tk.a)); }
        else if (tk.kind == FTok::NUM)  { DrawTextUtf8(hdc, x, yMain, tk.a, fB, Col::NUM);   x += TextWidth(hdc, Utf8ToWide(tk.a)); }
        else if (tk.kind == FTok::VAR || tk.kind == FTok::SUB) {
            HFONT f = (tk.kind == FTok::VAR) ? fI : fN;
            DrawTextUtf8(hdc, x, yMain, tk.a, f, Col::TEXT);
            x += TextWidth(hdc, Utf8ToWide(tk.a)) + (tk.kind == FTok::VAR ? 1 : 0);
            if (!tk.b.empty()) {
                DrawTextUtf8(hdc, x, ySub, tk.b, g_app.fnt7, Col::TEXT);
                x += TextWidth(hdc, Utf8ToWide(tk.b)) + 1;
            }
        } else if (tk.kind == FTok::FRAC) {
            int wn = MeasureFormula(hdc, tk.num, big);
            int wd = MeasureFormula(hdc, tk.den, big);
            int wf = std::max(wn, wd) + 12;
            int xBar1 = x + 2, xBar2 = xBar1 + wf;
            int dy = hMain / 2 + 3;
            int e1 = DrawFormula(hdc, xBar1 + (wf - wn) / 2, yMid - dy, tk.num, big);
            int e2 = DrawFormula(hdc, xBar1 + (wf - wd) / 2, yMid + dy + 1, tk.den, big);
            int xEnd = std::max({ xBar2, e1 + 4, e2 + 4 });
            DrawLine(hdc, xBar1, yMid, xEnd, yMid, Col::TEXT);
            x = xEnd + 4;
        }
    }
    return x;
}

static void DrawLeftBrace(HDC hdc, int x, int yTop, int yBot) {
    int yMid = (yTop + yBot) / 2;
    DrawLine(hdc, x + 6, yTop, x + 3, yTop + 3, Col::TEXT);
    DrawLine(hdc, x + 3, yTop + 3, x + 3, yMid - 3, Col::TEXT);
    DrawLine(hdc, x + 3, yMid - 3, x, yMid, Col::TEXT);
    DrawLine(hdc, x, yMid, x + 3, yMid + 3, Col::TEXT);
    DrawLine(hdc, x + 3, yMid + 3, x + 3, yBot - 3, Col::TEXT);
    DrawLine(hdc, x + 3, yBot - 3, x + 6, yBot, Col::TEXT);
}

// ============================================================================
// Paint Sections: Top Panel, Parameter Table, 4 Bottom Tabs
// ============================================================================
static std::string GetCtxLabel(uint32_t ctx, uint32_t rdAct) {
    switch (ctx) {
        case CTX_CARLOT:       return "Автосалон";
        case CTX_SHOP:         return "Стайлинг / Магазин [Игрок]";
        case CTX_TUNING_UI:    return rdAct ? "Race Day: Тюнинг [Игрок]" : "Тюнинг [Игрок]";
        case CTX_RACEDAY_PLAY: return "Race Day [Игрок]";
        case CTX_RACEDAY_HUB:  return "Race Day: Хаб [Игрок]";
        default:               return "Гараж [Игрок]";
    }
}

static void PaintTopPanel(HDC hdc) {
    DrawBox(hdc, 8, 6, WIN_W - 8, 160, Col::PANEL_BG, Col::BORDER);

    bool conn = g_app.bridge.isConnected();
    bool hasCar  = conn && g_app.shmCopy.baseHash[0] != '\0';
    bool hasSnap = hasCar && g_app.shmCopy.hasValidSnap != 0;
    std::string baseH = hasCar ? g_app.shmCopy.baseHash : "";
    auto mm = GetModeMeta(g_app.shmCopy.carMode);

    std::string carDisp = "Открой Автосалон (Магазин) или Гараж...";
    std::string modelOnly = "—";
    if (hasCar) {
        auto itS = g_app.dbs.stockDB.find(baseH);
        const ProfileCarEntry* pe = g_app.dbs.getProfileCar(baseH);
        if (itS != g_app.dbs.stockDB.end()) {
            modelOnly = itS->second.name;
            carDisp = modelOnly + "  [VLT: " + baseH + "]";
        } else if (pe && !pe->name.empty()) {
            modelOnly = pe->name;
            carDisp = modelOnly + "  [VLT: " + baseH + "]";
        } else if (g_app.shmCopy.vltRawCarName[0]) {
            modelOnly = g_app.shmCopy.vltRawCarName;
            carDisp = modelOnly + "  [VLT: " + baseH + "]";
        } else {
            modelOnly = "Не найдена в базе [" + baseH + "]";
            carDisp = "Неизвестная модель  [VLT: " + baseH + "]";
        }
    }

    // Left column info
    DrawTextUtf8(hdc, 18, 13, "Активный автомобиль:", g_app.fnt8, Col::DIM);
    DrawClippedUtf8(hdc, 146, 13, 492, 29, carDisp, g_app.fnt9B, hasCar ? Col::NUM : Col::TEXT);

    std::string modeLine = hasCar
        ? ("Режим: " + std::string(mm.name) + " [ID=" + std::to_string(g_app.shmCopy.carMode) + "]  |  " + GetCtxLabel(g_app.shmCopy.ctxMode, g_app.shmCopy.raceDayActive))
        : ("Подключение: " + g_app.bridge.getStatus());
    DrawClippedUtf8(hdc, 18, 32, 492, 48, modeLine, g_app.fnt8, conn ? Col::TEXT : Col::SLIDER_ACC);

    std::string chainLine = std::string("VLT цепочка: ") + (g_app.shmCopy.chainStr[0] ? g_app.shmCopy.chainStr : "—");
    DrawClippedUtf8(hdc, 18, 49, 492, 65, chainLine, g_app.fnt8, Col::DIM);

    // Blueprint & Career (FECustomizationRecord) live status
    char bpLine[160];
    if (conn && g_app.shmCopy.activeBpAddr != 0) {
        if (g_app.shmCopy.custRecCount > 0) {
            std::snprintf(bpLine, sizeof(bpLine), "BP: 0x%08X [%s] | Карьера: OK (%u) | Пул: %u",
                          g_app.shmCopy.activeBpAddr, g_app.shmCopy.bpSourceLabel,
                          g_app.shmCopy.custRecCount, g_app.shmCopy.totalBpInHeap);
        } else {
            std::snprintf(bpLine, sizeof(bpLine), "BP: 0x%08X [%s] | Карьера: поиск... | Пул: %u",
                          g_app.shmCopy.activeBpAddr, g_app.shmCopy.bpSourceLabel,
                          g_app.shmCopy.totalBpInHeap);
        }
    } else if (conn) {
        std::snprintf(bpLine, sizeof(bpLine), "BP: не найден (%s) | Пул: %u | VLT: %s",
                      g_app.shmCopy.bpSourceLabel, g_app.shmCopy.totalBpInHeap,
                      g_app.shmCopy.vltReady ? "OK" : "ждём");
    } else {
        std::snprintf(bpLine, sizeof(bpLine), "BP: ожидание подключения к nfs.exe...");
    }
    DrawClippedUtf8(hdc, 18, 67, 492, 83, bpLine, g_app.fnt8B, (conn && g_app.shmCopy.activeBpAddr != 0) ? Col::SLIDER_POS : Col::SLIDER_ACC);

    // Status line + Embedded DB confirmation
    DrawClippedUtf8(hdc, 18, 86, 492, 102, g_app.statusLine, g_app.fnt8, Col::TEXT);
    DrawClippedUtf8(hdc, 18, 103, 492, 118,
                    std::string("Базы данных: Встроены в ") + APP_FULL_TITLE + " (Сток 76/76 + VLT 76/76)",
                    g_app.fnt7B, Col::SLIDER_POS);

    // Right column: Version badge + DB Stats + BP Controls (без лишних кнопок файлов баз!)
    DrawTextUtf8(hdc, 502, 13, std::string(APP_FULL_TITLE) + "  |  Встроенные базы: 76/76", g_app.fnt9B, Col::NUM);
    char s1[96], s2[96];
    std::snprintf(s1, sizeof(s1), "Сток: %d/76 (все режимы: %d/76)  |  VLT: %u/76",
                  g_app.dbs.statTotal, g_app.dbs.statFullAll, (unsigned)g_app.dbs.profCars.size());
    std::snprintf(s2, sizeof(s2), "Грип: %d/76  Дрэг: %d/76  Скор: %d/76  Дрифт: %d/47",
                  g_app.dbs.statGrip, g_app.dbs.statDrag, g_app.dbs.statSpeed, g_app.dbs.statDrift);
    DrawTextUtf8(hdc, 502, 32, s1, g_app.fnt8, Col::TEXT);
    DrawTextUtf8(hdc, 502, 48, s2, g_app.fnt8, Col::DIM);

    // Blueprint controls row
    DrawButton(hdc, BTN_BP_RESCAN, 502, 74, 610, 97, "Найти BP", conn ? Col::NUM : Col::GRID, conn ? Col::NUM : Col::DIM);
    DrawButton(hdc, BTN_BP_PREV,   616, 74, 674, 97, "◄ BP");
    DrawButton(hdc, BTN_BP_NEXT,   678, 74, 736, 97, "BP ►");
    DrawButton(hdc, BTN_BP_AUTO,   742, 74, 842, 97,
               (g_app.shmCopy.bpPinnedAddr != 0) ? "Авто (откр.)" : "Автовыбор",
               (g_app.shmCopy.bpPinnedAddr != 0) ? Col::SLIDER_ACC : Col::BORDER);

    DrawClippedUtf8(hdc, 502, 103, 842, 118,
                    hasSnap ? "Дино: LIVE (автопересчёт при правках)" : "Дино: открой авто в Гараже для расчёта",
                    g_app.fnt7, hasSnap ? Col::SLIDER_POS : Col::DIM);

    // Bottom toolbar inside Top Panel
    DrawLine(hdc, 14, 122, WIN_W - 14, 122, Col::GRID);
    DrawButton(hdc, BTN_MODE_TOGGLE, 18, 128, 250, 152,
               g_app.useStockDB ? "Режим: База СТОКА (76 авто)" : "Режим: Временный снимок",
               g_app.useStockDB ? Col::NUM : Col::SLIDER_ACC);
    DrawButton(hdc, BTN_TEMP_SNAP, 258, 128, 464, 152, "Снимок текущих (временно)");
    DrawCheckBox(hdc, BTN_TOPMOST, 490, 132, "Поверх всех окон", g_app.stayOnTop);
    DrawCheckBox(hdc, BTN_KMH,     650, 132, "Разгон в км/ч",    g_app.useKmh);
}

static void PaintParamTable(HDC hdc) {
    int x0 = 8, y0 = 166, W = WIN_W - 16, H = 351;
    DrawBox(hdc, x0, y0, x0 + W, y0 + H, Col::BG, Col::BORDER);

    const int colW[5] = { 78, 270, 165, 165, W - (78 + 270 + 165 + 165) };
    const char* colT[5] = { "Обозн.", "Название параметра", "Изначально (СТОК)", "Текущее", "Разница (Δ)" };
    int xP[6] = { x0 };
    for (int i = 0; i < 5; ++i) xP[i + 1] = xP[i] + colW[i];

    int hdrH = 22, rowH = 18, gapH = 7;
    FillRectCol(hdc, x0 + 1, y0 + 1, x0 + W - 1, y0 + hdrH, Col::HEADER_BG);
    DrawLine(hdc, x0, y0 + hdrH - 1, x0 + W, y0 + hdrH - 1, Col::BORDER);
    for (int i = 0; i < 5; ++i) {
        DrawTextUtf8(hdc, xP[i] + 6, y0 + 4, colT[i], g_app.fnt9B, Col::TITLE);
        if (i > 0) DrawLine(hdc, xP[i], y0, xP[i], y0 + hdrH, Col::BORDER);
    }

    bool hasCur = g_app.bridge.isConnected() && g_app.shmCopy.hasValidSnap != 0;
    const float* curV = hasCur ? g_app.shmCopy.curVals : nullptr;
    auto mm = GetModeMeta(g_app.shmCopy.carMode);

    const float* initV = nullptr;
    bool exactStockMode = false;
    if (hasCur) {
        if (g_app.useStockDB) {
            auto* sv = g_app.dbs.getStockValsForMode(g_app.shmCopy.baseHash, mm.key, exactStockMode);
            if (sv) initV = sv->data();
        }
        if (!initV && g_app.hasTempInit) initV = g_app.tempInitVals.data();
        if (!initV) initV = curV;
    }

    int y = y0 + hdrH;
    int prevGrp = PARAM_ROWS[0].group;
    int rowInGrp = 0;

    for (int idx = 0; idx < 17; ++idx) {
        const ParamRowDef& r = PARAM_ROWS[idx];
        if (r.group != prevGrp) {
            FillRectCol(hdc, x0 + 1, y, x0 + W - 1, y + gapH, Col::GAP_BG);
            DrawLine(hdc, x0, y, x0 + W, y, Col::BORDER);
            DrawLine(hdc, x0, y + gapH - 1, x0 + W, y + gapH - 1, Col::BORDER);
            y += gapH;
            prevGrp = r.group;
            rowInGrp = 0;
        }
        COLORREF bg = (rowInGrp++ % 2 == 0) ? Col::ROW_A : Col::ROW_B;
        FillRectCol(hdc, x0 + 1, y, x0 + W - 1, y + rowH, bg);
        DrawLine(hdc, x0, y + rowH - 1, x0 + W, y + rowH - 1, Col::GRID);
        for (int i = 1; i < 5; ++i) DrawLine(hdc, xP[i], y, xP[i], y + rowH, Col::GRID);

        std::string sym, title;
        int valIdx = r.valIdx;
        if (valIdx < 0) {
            valIdx = mm.vIdx;
            sym = mm.vSym;
            title = std::string("Рейтинг режима (") + mm.name + ")";
        } else if (r.altKind != 0 && g_app.useKmh) {
            sym   = r.symKmh ? r.symKmh : r.symMph;
            title = r.titleKmh ? r.titleKmh : r.titleMph;
        } else {
            sym   = r.symMph;
            title = r.titleMph;
        }

        int yT = y + 2;
        DrawTextUtf8(hdc, xP[0] + 6, yT, sym, g_app.fnt9B, Col::SYM);
        DrawTextUtf8(hdc, xP[1] + 6, yT, title, r.isConst ? g_app.fnt9B : g_app.fnt9, Col::TEXT);

        if (!r.isConst && curV && initV) {
            float vI = (r.altKind != 0 && g_app.useKmh) ? CalcAltMetric(r.altKind, initV) : initV[valIdx];
            float vC = (r.altKind != 0 && g_app.useKmh) ? CalcAltMetric(r.altKind, curV)  : curV[valIdx];
            float vD = vC - vI;
            float sc = (r.altKind == 3 && g_app.useKmh) ? (r.minScale * MPH_TO_KMH) : r.minScale;
            std::string sDiff = FmtDiff(vD, r.fmt);
            bool hasDiff = (sDiff != "-");
            COLORREF dCol = hasDiff ? DiffColor(vD, vI, sc, r.invertCol) : Col::DIM;

            DrawTextUtf8(hdc, xP[2] + 6, yT, FmtFloat(vI, r.fmt), g_app.fnt9, Col::TEXT);
            DrawTextUtf8(hdc, xP[3] + 6, yT, FmtFloat(vC, r.fmt), g_app.fnt9, Col::TEXT);
            DrawTextUtf8(hdc, xP[4] + 6, yT, sDiff, hasDiff ? g_app.fnt9B : g_app.fnt9, dCol);
        } else if (!r.isConst) {
            DrawTextUtf8(hdc, xP[2] + 6, yT, "0", g_app.fnt9, Col::DIM);
            DrawTextUtf8(hdc, xP[3] + 6, yT, "0", g_app.fnt9, Col::DIM);
            DrawTextUtf8(hdc, xP[4] + 6, yT, "-", g_app.fnt9, Col::DIM);
        }
        y += rowH;
    }
}

static void DrawSliderRow(HDC hdc, int x, int y, int rowH, int sIdx) {
    const SliderDef& def = SLIDER_DEFS[sIdx];
    const ShmSlider& st  = g_app.shmCopy.sliders[sIdx];
    bool locked = (st.locked != 0) || (st.hasValue == 0);
    bool editable = (!locked && st.editable != 0);

    int tx1 = x + 166, tw = 162, tx2 = tx1 + tw;
    int capL1 = tx2 + 6, capL2 = capL1 + 13;
    int valX1 = capL2 + 1, valX2 = valX1 + 32;
    int capR1 = valX2 + 1, capR2 = capR1 + 13;
    int yMid = y + rowH / 2;
    int ty1 = yMid - 5, ty2 = yMid + 5;
    int tMid = tx1 + tw / 2;
    int yT = y + (rowH - 12) / 2;

    DrawTextUtf8(hdc, x, yT, def.nameUtf8, g_app.fnt8, locked ? Col::LOCK_TEXT : Col::TEXT);

    float valF = (st.hasValue && std::isfinite(st.value)) ? st.value : 0.0f;
    float ticks = std::max(-10.0f, std::min(10.0f, valF * 10.0f));
    int rounded = (ticks >= 0.0f) ? (int)std::floor(ticks + 0.5f) : (int)std::ceil(ticks - 0.5f);
    float snapped = (std::fabs(ticks - rounded) < 0.12f) ? (float)rounded : ticks;
    int px = tMid + (int)std::lround((snapped / 10.0f) * (tw / 2.0f - 2.0f));
    px = std::max(tx1 + 3, std::min(tx2 - 4, px));

    char valStr[16] = "0";
    if (st.hasValue) {
        if (snapped > 0.05f) {
            if (snapped == (float)rounded) std::snprintf(valStr, sizeof(valStr), "+%d", rounded);
            else std::snprintf(valStr, sizeof(valStr), "+%.1f", snapped);
        } else if (snapped < -0.05f) {
            if (snapped == (float)rounded) std::snprintf(valStr, sizeof(valStr), "%d", rounded);
            else std::snprintf(valStr, sizeof(valStr), "%.1f", snapped);
        }
    }

    if (locked) {
        DrawBox(hdc, tx1, ty1, tx2, ty2, Col::LOCK_TRACK, Col::GRID);
        FillRectCol(hdc, px - 2, ty1 - 1, px + 3, ty2 + 1, Col::LOCK_THUMB);
        DrawBox(hdc, capL1, ty1, capL2, ty2, Col::LOCK_CAP, Col::GRID);
        DrawTriangleArrow(hdc, capL1 + 6, yMid, -1, Col::LOCK_ARROW);
        DrawCenteredUtf8(hdc, valX1, valX2, yT, valStr, g_app.fnt8, Col::LOCK_TEXT);
        DrawBox(hdc, capR1, ty1, capR2, ty2, Col::LOCK_CAP, Col::GRID);
        DrawTriangleArrow(hdc, capR1 + 5, yMid, 1, Col::LOCK_ARROW);
        return;
    }

    bool isDrag = (g_app.draggingSliderIdx == sIdx);
    COLORREF bCol = isDrag ? Col::SLIDER_ACC : (editable ? Col::BORDER : Col::GRID);
    DrawBox(hdc, tx1, ty1, tx2, ty2, Col::TRACK_BLOCK, bCol);

    float cMin = std::max(-1.0f, std::min(1.0f, st.minBound));
    float cMax = std::max(cMin, std::min(1.0f, st.maxBound));
    int xMin = tx1 + 1 + (int)std::lround(((cMin + 1.0f) * 0.5f) * (tw - 2));
    int xMax = tx1 + 1 + (int)std::lround(((cMax + 1.0f) * 0.5f) * (tw - 2));
    if (xMax > xMin) FillRectCol(hdc, xMin, ty1 + 1, xMax, ty2 - 1, Col::TRACK_AVAIL);

    COLORREF valCol = Col::TEXT;
    if (snapped > 0.05f) {
        FillRectCol(hdc, tMid + 1, ty1 + 1, px, ty2 - 1, Col::SLIDER_POS);
        valCol = Col::SLIDER_POS;
    } else if (snapped < -0.05f) {
        FillRectCol(hdc, px, ty1 + 1, tMid, ty2 - 1, Col::SLIDER_NEG);
        valCol = Col::SLIDER_NEG;
    }

    for (int step = -8; step <= 8; step += 2) {
        if (step == 0) continue;
        int sx = tMid + (int)std::lround((step / 10.0f) * (tw / 2.0f - 1.0f));
        if (sx > xMin && sx < xMax) DrawLine(hdc, sx, ty1 + 1, sx, ty2 - 1, Col::TRACK_TICK);
    }
    if (cMin > -0.99f) DrawLine(hdc, xMin, ty1 + 1, xMin, ty2 - 1, Col::TRACK_EDGE);
    if (cMax <  0.99f) DrawLine(hdc, xMax - 1, ty1 + 1, xMax - 1, ty2 - 1, Col::TRACK_EDGE);
    DrawLine(hdc, tMid, ty1 - 1, tMid, ty2 + 1, Col::TRACK_ZERO);
    FillRectCol(hdc, px - 2, ty1 - 1, px + 3, ty2 + 1, isDrag ? Col::SLIDER_ACC : Col::TEXT);

    DrawBox(hdc, capL1, ty1, capL2, ty2, Col::CAP_BG, bCol);
    DrawTriangleArrow(hdc, capL1 + 6, yMid, -1, Col::CAP_ARROW);
    DrawCenteredUtf8(hdc, valX1, valX2, yT, valStr, g_app.fnt8B, valCol);
    DrawBox(hdc, capR1, ty1, capR2, ty2, Col::CAP_BG, bCol);
    DrawTriangleArrow(hdc, capR1 + 5, yMid, 1, Col::CAP_ARROW);

    if (editable) {
        g_app.sliderHits.push_back({ sIdx, def.hash, capL1, capL2, tx1, tx2, capR1, capR2, y, y + rowH });
    }
}

static std::string FormatSculptSlot(int slotIdx, bool compact) {
    if (g_app.shmCopy.autosculptBasePtr == 0) return "";
    std::string out;
    for (int z = 0; z < 10; ++z) {
        int pct = (int)std::floor(g_app.shmCopy.autosculptZones[slotIdx][z] * 100.0f + 0.5f);
        if (pct != 0) {
            if (!out.empty()) out += compact ? " " : "   ";
            char b[24];
            std::snprintf(b, sizeof(b), compact ? "Z%d:%d%%" : "Z%d: %d%%", z + 1, pct);
            out += b;
        }
    }
    return out;
}

static void PaintTabTuning(HDC hdc, int x0, int y0, int W, int H) {
    int xMid = x0 + W / 2;
    DrawLine(hdc, xMid, y0 + 4, xMid, y0 + 266, Col::GRID);

    // Left: Suspension (0..14)
    DrawCenteredUtf8(hdc, x0 + 8, xMid - 6, y0 + 4, "ПОДВЕСКА", g_app.fnt8B, Col::NUM);
    int yL = y0 + 19;
    for (int i = 0; i < 15; ++i) { DrawSliderRow(hdc, x0 + 12, yL, 16, i); yL += 16; }

    // Right: Engine (15..19), Drivetrain (20..26), Brakes (27..29)
    int xR = xMid + 10, yR = y0 + 4;
    DrawCenteredUtf8(hdc, xR, x0 + W - 12, yR, "ДВИГАТЕЛЬ", g_app.fnt8B, Col::NUM); yR += 14;
    for (int i = 15; i < 20; ++i) { DrawSliderRow(hdc, xR, yR, 14, i); yR += 14; }
    yR += 3;
    DrawCenteredUtf8(hdc, xR, x0 + W - 12, yR, "ТРАНСМИССИЯ", g_app.fnt8B, Col::NUM); yR += 14;
    for (int i = 20; i < 27; ++i) { DrawSliderRow(hdc, xR, yR, 14, i); yR += 14; }
    yR += 3;
    DrawCenteredUtf8(hdc, xR, x0 + W - 12, yR, "ТОРМОЗА", g_app.fnt8B, Col::NUM); yR += 14;
    for (int i = 27; i < 30; ++i) { DrawSliderRow(hdc, xR, yR, 14, i); yR += 14; }

    // Bottom split: Autosculpt | Installed Upgrade Parts
    int ySep = y0 + 270;
    DrawLine(hdc, x0 + 10, ySep, x0 + W - 10, ySep, Col::BORDER);
    int xDiv = x0 + 436;
    DrawLine(hdc, xDiv, ySep + 4, xDiv, ySep + 86, Col::GRID);

    DrawTextUtf8(hdc, x0 + 12, ySep + 4, "Autosculpt", g_app.fnt8B, Col::NUM);
    std::string bodyVal = "— (ожидание)"; bool bodyNZ = false;
    if (g_app.shmCopy.autosculptBasePtr != 0) {
        bodyVal = "0% (сток)";
        for (int s = 6; s <= 11; ++s) {
            std::string wb = FormatSculptSlot(s, false);
            if (!wb.empty()) { bodyVal = "Обвес: " + wb; bodyNZ = true; break; }
        }
        if (!bodyNZ) {
            std::string segs;
            auto addS = [&](const char* p, int idx) {
                std::string z = FormatSculptSlot(idx, true);
                if (!z.empty()) { if (!segs.empty()) segs += " | "; segs += p + z; }
            };
            addS("Пер: ", 0); addS("Зад: ", 1); addS("Пор: ", 2);
            if (!segs.empty()) { bodyVal = segs; bodyNZ = true; }
        }
    }
    std::string hoodZ = FormatSculptSlot(12, false), spZ = FormatSculptSlot(13, false);
    std::string hoodVal = (g_app.shmCopy.autosculptBasePtr == 0) ? "— (ожидание)" : (hoodZ.empty() ? "0% (сток)" : hoodZ);
    std::string spVal   = (g_app.shmCopy.autosculptBasePtr == 0) ? "— (ожидание)" : (spZ.empty()   ? "0% (сток)" : spZ);

    struct ScRow { const char* lbl; const char* part; std::string val; bool act; };
    ScRow scRows[3] = {
        { "Кузов",   g_app.shmCopy.bodyName[0]    ? g_app.shmCopy.bodyName    : "—", bodyVal, bodyNZ },
        { "Капот",   g_app.shmCopy.hoodName[0]    ? g_app.shmCopy.hoodName    : "—", hoodVal, !hoodZ.empty() },
        { "Спойлер", g_app.shmCopy.spoilerName[0] ? g_app.shmCopy.spoilerName : "—", spVal,   !spZ.empty() },
    };
    int yS = ySep + 20;
    for (int i = 0; i < 3; ++i) {
        DrawBox(hdc, x0 + 12, yS, xDiv - 6, yS + 19, (i % 2 == 0) ? Col::ROW_A : Col::ROW_B, Col::GRID);
        DrawTextUtf8(hdc, x0 + 16, yS + 3, scRows[i].lbl, g_app.fnt8B, Col::SYM);
        DrawLine(hdc, x0 + 70, yS, x0 + 70, yS + 19, Col::GRID);
        std::string txt = std::string(scRows[i].part) + "  |  " + scRows[i].val;
        DrawClippedUtf8(hdc, x0 + 76, yS + 2, xDiv - 10, yS + 18, txt, scRows[i].act ? g_app.fnt8B : g_app.fnt8, scRows[i].act ? Col::SCULPT_VAL : Col::DIM);
        yS += 20;
    }

    // Right: Installed Parts (4x2)
    DrawTextUtf8(hdc, xDiv + 8, ySep + 4, "Тюнинг", g_app.fnt8B, Col::NUM);
    int pColW = 192, pRowH = 16;
    for (int i = 0; i < NUM_UPGRADE_PARTS; ++i) {
        int col = (i < 4) ? 0 : 1, row = i % 4;
        int px1 = xDiv + 8 + col * (pColW + 6), py1 = ySep + 20 + row * pRowH;
        int px2 = px1 + pColW, py2 = py1 + pRowH - 1;
        DrawBox(hdc, px1, py1, px2, py2, (row % 2 == 0) ? Col::ROW_A : Col::ROW_B, Col::GRID);
        DrawTextUtf8(hdc, px1 + 5, py1 + 2, INSPECT_SLOT_DEFS[i].nameUtf8, g_app.fnt8, Col::TEXT);
        int xSep = px2 - 54;
        DrawLine(hdc, xSep, py1, xSep, py2, Col::GRID);
        std::string vTxt = g_app.shmCopy.upgradeShort[i][0] ? g_app.shmCopy.upgradeShort[i] : "—";
        bool hasV = (vTxt != "—");
        DrawCenteredUtf8(hdc, xSep + 1, px2 - 1, py1 + 2, vTxt, hasV ? g_app.fnt8B : g_app.fnt8, hasV ? Col::SCULPT_VAL : Col::DIM);
    }

    // Footer status + Reset Sliders button
    const char* sName = "Ожидание чтения ползунков...";
    if (g_app.shmCopy.sliderSource == SLIDER_SRC_TUNINGDATA) sName = "TuningData [LIVE]";
    else if (g_app.shmCopy.sliderSource == SLIDER_SRC_BLUEPRINT) sName = "Blueprint [LIVE]";
    else if (g_app.shmCopy.sliderSource == SLIDER_SRC_TUNINGMAP) sName = "TuningMap [LIVE]";
    std::string foot = std::string(sName) + "  (" + GetCtxLabel(g_app.shmCopy.ctxMode, g_app.shmCopy.raceDayActive)
                     + " | " + (g_app.shmCopy.bpSourceLabel[0] ? g_app.shmCopy.bpSourceLabel : "—")
                     + ")  |  ◄ / ► или клик (ПКМ = 0)";
    DrawClippedUtf8(hdc, x0 + 12, y0 + H - 17, x0 + W - 102, y0 + H - 3, foot, g_app.fnt7, Col::DIM);
    DrawButton(hdc, BTN_RESET_SLIDERS, x0 + W - 96, y0 + H - 19, x0 + W - 10, y0 + H - 2, "Сброс в 0", Col::BORDER, Col::TEXT, g_app.fnt7B);
}

static void PaintTabMatches(HDC hdc, int x0, int y0, int W, int H) {
    uint32_t flashAge = GetTickCount() - g_app.flashTick;
    if (!g_app.flashMsg.empty() && flashAge < 4000u) {
        DrawClippedUtf8(hdc, x0 + 12, y0 + 5, x0 + W - 12, y0 + 21, g_app.flashMsg, g_app.fnt8B, g_app.flashOk ? Col::SLIDER_POS : Col::SLIDER_NEG);
    } else {
        char hdr[180];
        std::snprintf(hdr, sizeof(hdr), "Совпадения (%u/76)  |  %s  |  %s [%s]  |  ◄ / ► выбор -> [Установить]",
                      (unsigned)g_app.dbs.profCars.size(),
                      g_app.shmCopy.detailSourceLabel[0] ? g_app.shmCopy.detailSourceLabel : "—",
                      GetCtxLabel(g_app.shmCopy.ctxMode, g_app.shmCopy.raceDayActive).c_str(),
                      g_app.shmCopy.bpSourceLabel[0] ? g_app.shmCopy.bpSourceLabel : "—");
        DrawClippedUtf8(hdc, x0 + 12, y0 + 5, x0 + W - 12, y0 + 21, hdr, g_app.fnt8B, Col::NUM);
    }

    // 10-slot table
    const int colW[8] = { 46, 102, 52, 194, 76, 80, 130, W - 24 - (46 + 102 + 52 + 194 + 76 + 80 + 130) };
    const char* colT[8] = { "Слот", "Категория", "idx/cnt", "Этап — Бренд", "phys key", "brandHash", "Совп. phys key", "Только brandHash" };
    int xC[9] = { x0 + 12 };
    for (int i = 0; i < 8; ++i) xC[i + 1] = xC[i] + colW[i];

    int yHdr = y0 + 22, hdrH = 16;
    DrawBox(hdc, xC[0], yHdr, xC[8], yHdr + hdrH, Col::HEADER_BG, Col::BORDER);
    for (int i = 0; i < 8; ++i) {
        DrawTextUtf8(hdc, xC[i] + 4, yHdr + 2, colT[i], g_app.fnt8B, Col::TITLE);
        if (i > 0) DrawLine(hdc, xC[i], yHdr, xC[i], yHdr + hdrH, Col::GRID);
    }

    int yRow = yHdr + hdrH, rowH = 15;
    for (int r = 0; r < NUM_INSPECT_SLOTS; ++r) {
        const DetailRowView& it = g_app.detailRows[r];
        bool isSel = (r == g_app.selectedDetailRow);
        DrawBox(hdc, xC[0], yRow, xC[8], yRow + rowH, isSel ? Col::ROW_SEL : ((r % 2 == 0) ? Col::ROW_A : Col::ROW_B), isSel ? Col::NUM : Col::GRID);
        for (int i = 1; i < 8; ++i) DrawLine(hdc, xC[i], yRow, xC[i], yRow + rowH, Col::GRID);

        char sBuf[16]; std::snprintf(sBuf, sizeof(sBuf), "#%03d", it.slotNum);
        DrawTextUtf8(hdc, xC[0] + 4, yRow + 1, sBuf, g_app.fnt8B, isSel ? Col::NUM : Col::SYM);
        DrawTextUtf8(hdc, xC[1] + 4, yRow + 1, it.name, isSel ? g_app.fnt8B : g_app.fnt8, Col::TEXT);

        bool pending = isSel && g_app.browseIdx >= 0 && (uint32_t)g_app.browseIdx != it.idx;
        char iBuf[24]; std::snprintf(iBuf, sizeof(iBuf), pending ? "%u/%u*" : "%u/%u", it.viewIdx, it.mCount);
        DrawTextUtf8(hdc, xC[2] + 4, yRow + 1, iBuf, pending ? g_app.fnt8B : g_app.fnt8,
                     pending ? Col::SLIDER_ACC : (it.viewIdx > 0 ? Col::TEXT : Col::DIM));

        COLORREF lblCol = (it.viewIdx > 0) ? Col::SCULPT_VAL : Col::DIM;
        DrawStageBrandText(hdc, xC[3] + 4, yRow + 1, it.stageName + " — ", it.brandName, "", it.isRawBrand, it.viewIdx > 0, lblCol, xC[4] - 2);
        DrawTextUtf8(hdc, xC[4] + 4, yRow + 1, it.hasPhys ? it.physHex : "—", g_app.fnt8, it.hasPhys ? Col::NUM : Col::DIM);
        DrawTextUtf8(hdc, xC[5] + 4, yRow + 1, !it.brandHash.empty() ? it.brandHash : "—", g_app.fnt8, !it.brandHash.empty() ? Col::SYM : Col::DIM);

        size_t nSame = it.samePhys.size(), nDiff = it.diffPhys.size();
        std::string sSame = g_app.dbs.profCars.empty() ? "нет базы" : (nSame > 0 ? (std::to_string(nSame) + " др. авто") : "уникально");
        std::string sDiff = (nDiff > 0) ? (std::to_string(nDiff) + " др. авто (≠phys)") : "—";
        DrawTextUtf8(hdc, xC[6] + 4, yRow + 1, sSame, nSame > 0 ? g_app.fnt8B : g_app.fnt8, nSame > 0 ? Col::SLIDER_POS : Col::DIM);
        DrawTextUtf8(hdc, xC[7] + 4, yRow + 1, sDiff, nDiff > 0 ? g_app.fnt8B : g_app.fnt8, nDiff > 0 ? Col::SLIDER_ACC : Col::DIM);
        yRow += rowH;
    }

    // Selected bar with [◄] [Установить] [►]
    int yBar = yRow + 4;
    DrawBox(hdc, x0 + 12, yBar, x0 + W - 12, yBar + 23, Col::HEADER_BG, Col::BORDER);
    const DetailRowView& sel = g_app.detailRows[g_app.selectedDetailRow];
    bool canInst = (g_app.bridge.isConnected() && g_app.shmCopy.activeBpAddr != 0);
    bool isPend  = (g_app.browseIdx >= 0 && (uint32_t)g_app.browseIdx != sel.idx);
    COLORREF instCol = (!canInst) ? Col::DIM : (isPend ? Col::SLIDER_ACC : Col::SLIDER_POS);

    DrawButton(hdc, BTN_PART_PREV,    x0 + W - 144, yBar + 2, x0 + W - 122, yBar + 21, "◄");
    DrawButton(hdc, BTN_PART_INSTALL, x0 + W - 119, yBar + 2, x0 + W - 40,  yBar + 21, "Установить", instCol, canInst ? (isPend ? Col::SLIDER_ACC : Col::TEXT) : Col::DIM);
    DrawButton(hdc, BTN_PART_NEXT,    x0 + W - 37,  yBar + 2, x0 + W - 15,  yBar + 21, "►");

    char preH[128], sufH[128];
    std::snprintf(preH, sizeof(preH), "#%03d [%s]: %s — ", sel.slotNum, sel.name.c_str(), sel.stageName.c_str());
    if (isPend)
        std::snprintf(sufH, sizeof(sufH), " (выбор %u / уст. %u из %u) | phys=%s | brand=%s",
                      sel.viewIdx, sel.idx, sel.mCount, sel.hasPhys ? sel.physHex.c_str() : "—", !sel.brandHash.empty() ? sel.brandHash.c_str() : "—");
    else
        std::snprintf(sufH, sizeof(sufH), " (уст. %u/%u) | phys=%s | brand=%s",
                      sel.idx, sel.mCount, sel.hasPhys ? sel.physHex.c_str() : "—", !sel.brandHash.empty() ? sel.brandHash.c_str() : "—");
    DrawStageBrandText(hdc, x0 + 18, yBar + 5, preH, sel.brandName, sufH, sel.isRawBrand, true, Col::NUM, x0 + W - 150);

    // Two scrollable match panels (mouse wheel + ▲/▼)
    int yBox = yBar + 25, yEnd = y0 + H - 6, mid = x0 + W / 2;
    const int pageRows = 8;

    // Left: Same phys key
    DrawBox(hdc, x0 + 12, yBox, mid - 3, yEnd, Col::BG, Col::BORDER);
    DrawBox(hdc, x0 + 12, yBox, mid - 3, yBox + 21, Col::ROW_B, Col::BORDER);
    int nSame = (int)sel.samePhys.size();
    g_app.scrollSame = std::max(0, std::min(g_app.scrollSame, std::max(0, nSame - pageRows)));
    char hSame[96];
    if (nSame > pageRows)
        std::snprintf(hSame, sizeof(hSame), "Совпадает phys key (%d др. авто) [%d–%d]", nSame, g_app.scrollSame + 1, std::min(nSame, g_app.scrollSame + pageRows));
    else
        std::snprintf(hSame, sizeof(hSame), "Совпадает phys key (%d др. авто)", nSame);
    DrawTextUtf8(hdc, x0 + 18, yBox + 4, hSame, g_app.fnt8B, Col::SLIDER_POS);
    if (nSame > pageRows) {
        DrawButton(hdc, BTN_SAME_UP, mid - 52, yBox + 2, mid - 30, yBox + 19, "▲", Col::BORDER, g_app.scrollSame > 0 ? Col::TEXT : Col::DIM, g_app.fnt7B);
        DrawButton(hdc, BTN_SAME_DN, mid - 27, yBox + 2, mid - 6,  yBox + 19, "▼", Col::BORDER, (g_app.scrollSame + pageRows < nSame) ? Col::TEXT : Col::DIM, g_app.fnt7B);
    }
    int yL = yBox + 25;
    if (g_app.dbs.profCars.empty()) DrawTextUtf8(hdc, x0 + 18, yL, "Укажи файл VLT-базы через «Файл VLT-базы...» сверху", g_app.fnt8, Col::DIM);
    else if (nSame == 0) DrawTextUtf8(hdc, x0 + 18, yL, "У других авто такой же phys key не найден", g_app.fnt8, Col::DIM);
    else {
        for (int i = g_app.scrollSame; i < std::min(nSame, g_app.scrollSame + pageRows); ++i) {
            const auto& m = sel.samePhys[i];
            std::string pre = std::to_string(i + 1) + ". " + m.carName + " — " + m.stage + " — ";
            std::string suf = (m.cnt > 1) ? (" (" + std::to_string(m.cnt) + "x)") : "";
            DrawStageBrandText(hdc, x0 + 18, yL, pre, m.brand, suf, m.isRawBrand, false, Col::TEXT, mid - 8);
            yL += 16;
        }
    }

    // Right: Same brandHash, different phys key
    DrawBox(hdc, mid + 3, yBox, x0 + W - 12, yEnd, Col::BG, Col::BORDER);
    DrawBox(hdc, mid + 3, yBox, x0 + W - 12, yBox + 21, Col::ROW_B, Col::BORDER);
    int nDiff = (int)sel.diffPhys.size();
    g_app.scrollDiff = std::max(0, std::min(g_app.scrollDiff, std::max(0, nDiff - pageRows)));
    char hDiff[96];
    if (nDiff > pageRows)
        std::snprintf(hDiff, sizeof(hDiff), "Только brandHash, ≠ phys key (%d) [%d–%d]", nDiff, g_app.scrollDiff + 1, std::min(nDiff, g_app.scrollDiff + pageRows));
    else
        std::snprintf(hDiff, sizeof(hDiff), "Только brandHash, ≠ phys key (%d др. авто)", nDiff);
    DrawTextUtf8(hdc, mid + 9, yBox + 4, hDiff, g_app.fnt8B, Col::SLIDER_ACC);
    if (nDiff > pageRows) {
        DrawButton(hdc, BTN_DIFF_UP, x0 + W - 62, yBox + 2, x0 + W - 40, yBox + 19, "▲", Col::BORDER, g_app.scrollDiff > 0 ? Col::TEXT : Col::DIM, g_app.fnt7B);
        DrawButton(hdc, BTN_DIFF_DN, x0 + W - 37, yBox + 2, x0 + W - 16, yBox + 19, "▼", Col::BORDER, (g_app.scrollDiff + pageRows < nDiff) ? Col::TEXT : Col::DIM, g_app.fnt7B);
    }
    int yR = yBox + 25;
    if (g_app.dbs.profCars.empty()) DrawTextUtf8(hdc, mid + 9, yR, "Укажи файл VLT-базы через «Файл VLT-базы...» сверху", g_app.fnt8, Col::DIM);
    else if (nDiff == 0) DrawTextUtf8(hdc, mid + 9, yR, "Расхождений (тот же этап+бренд, другой phys) нет", g_app.fnt8, Col::DIM);
    else {
        for (int i = g_app.scrollDiff; i < std::min(nDiff, g_app.scrollDiff + pageRows); ++i) {
            const auto& m = sel.diffPhys[i];
            std::string ln = std::to_string(i + 1) + ". " + m.carName + "  [phys=" + m.phys + "]";
            DrawClippedUtf8(hdc, mid + 9, yR, x0 + W - 16, yR + 16, ln, g_app.fnt8, Col::SCULPT_VAL);
            yR += 16;
        }
    }
}

static void PaintTabFormulas(HDC hdc, int x0, int y0, int W, int /*H*/) {
    bool has = g_app.bridge.isConnected() && g_app.shmCopy.hasValidSnap != 0;
    const float* c = g_app.shmCopy.curVals;
    uint32_t rawMode = g_app.shmCopy.carMode;
    auto mm = GetModeMeta(rawMode);

    auto f6 = [](float v) { return FmtFloat(v, "%.6f"); };
    auto f4 = [](float v) { return FmtFloat(v, "%.4f"); };
    auto f2 = [](float v) { return FmtFloat(v, "%.2f"); };
    auto f1 = [](float v) { return FmtFloat(v, "%.1f"); };

    std::string boostStr = "TORQUE_BOOST";
    if (has && c[2] > 1e-6f) {
        float b = (c[1] / c[2]) - 1.0f;
        boostStr = (std::fabs(b) < 1e-4f) ? "0" : FmtFloat(b, "%.3f");
    }

    DrawFormula(hdc, x0 + 12, y0 + 25, { FTok::B("Рекомендуемое время:") });
    std::vector<FTok> numMain = (rawMode == 2)
        ? std::vector<FTok>{ FTok::N("2.0"), FTok::T(" · "), FTok::V("k", "car") }
        : std::vector<FTok>{ FTok::V("k", "track"), FTok::T(" · "), FTok::V("k", "car") };
    DrawFormula(hdc, x0 + 180, y0 + 25, {
        FTok::V("T"), FTok::T(" = "), FTok::V("t", "base"), FTok::T(" − "),
        FTok::F(numMain, { FTok::N("100") })
    }, true);

    DrawLine(hdc, x0 + 10, y0 + 52, x0 + W - 10, y0 + 52, Col::GRID);

    DrawFormula(hdc, x0 + 12, y0 + 66, {
        FTok::V("t", "base"), FTok::T(" = Sim("), FTok::V("S", "track"), FTok::T("; "), FTok::N("2.25"), FTok::T("; "),
        has ? FTok::N(f6(c[1])) : FTok::V("P", "2"), FTok::T("; "),
        has ? FTok::N(f6(c[2])) : FTok::V("P", "3"), FTok::T("; "),
        has ? FTok::N(f6(c[3])) : FTok::V("P", "4"), FTok::T("; "),
        has ? FTok::N(f6(c[4])) : FTok::V("P", "5"), FTok::T("; "),
        has ? FTok::N(f6(c[5])) : FTok::V("P", "6"), FTok::T("; "),
        has ? FTok::N(f6(c[6])) : FTok::V("P", "7"), FTok::T(")")
    });

    if (rawMode == 0) {
        int xa = DrawFormula(hdc, x0 + 12, y0 + 114, { FTok::V("k", "track"), FTok::T(" = ") });
        DrawLeftBrace(hdc, xa + 2, y0 + 80, y0 + 148);
        DrawFormula(hdc, xa + 14, y0 + 95, {
            FTok::N("25"), FTok::T(" + "), FTok::N("4"), FTok::T(" · "),
            FTok::F({ FTok::S("Dist", "engine") }, { FTok::N("1000") }),
            FTok::T(",   "), FTok::D("Grip, Sector Shootout")
        });
        DrawFormula(hdc, xa + 14, y0 + 134, {
            FTok::F({ FTok::N("25"), FTok::T(" + "), FTok::N("4"), FTok::T(" · ("), FTok::S("Dist", "engine"), FTok::T(" / "), FTok::N("1000"), FTok::T(")") }, { FTok::V("L") }),
            FTok::T(",   "), FTok::D("Time Attack (L — круги)")
        });
    } else if (rawMode == 3) {
        DrawFormula(hdc, x0 + 12, y0 + 114, {
            FTok::V("k", "track"), FTok::T(" = "), FTok::N("25"), FTok::T(" + "), FTok::N("4"), FTok::T(" · "),
            FTok::F({ FTok::S("Dist", "engine") }, { FTok::N("1000") }),
            FTok::T("   "), FTok::D("(Speed Challenge, Top Speed Run)")
        });
    } else if (rawMode == 2) {
        DrawFormula(hdc, x0 + 12, y0 + 114, { FTok::V("k", "track"), FTok::T(" = "), FTok::N("2.0"), FTok::T("   "), FTok::D("(Drag)") });
    } else {
        DrawFormula(hdc, x0 + 12, y0 + 114, { FTok::V("k", "track"), FTok::T(" = —   "), FTok::D("(в режиме ДРИФТ T не применяется — расчёт по очкам)") });
    }

    int xK = DrawFormula(hdc, x0 + 12, y0 + 174, rawMode == 0
        ? std::vector<FTok>{ FTok::V("k", "car"), FTok::T(" = "), FTok::N("−80"), FTok::T(" − "), FTok::N("20"), FTok::T(" · Ratio") }
        : std::vector<FTok>{ FTok::V("k", "car"), FTok::T(" = "), FTok::V("k", "min"), FTok::T(" + Ratio · ("), FTok::V("k", "max"), FTok::T(" − "), FTok::V("k", "min"), FTok::T(")") });

    DrawFormula(hdc, std::max(xK + 22, x0 + 305), y0 + 174, {
        FTok::T("Ratio = "),
        FTok::F({ has ? FTok::N(f6(c[mm.vIdx])) : FTok::V("V"), FTok::T(" − "), FTok::V("V", "min") },
                { FTok::V("V", "max"), FTok::T(" − "), FTok::V("V", "min") }),
        FTok::D("  [0..1]")
    });

    DrawLine(hdc, x0 + 10, y0 + 200, x0 + W - 10, y0 + 200, Col::GRID);

    DrawFormula(hdc, x0 + 12, y0 + 216, {
        FTok::V("P", "2"), FTok::T(" = "), has ? FTok::N(f6(c[2])) : FTok::V("P", "3"),
        FTok::T(" · ("), FTok::N("1"), FTok::T(" + "), has ? FTok::N(boostStr) : FTok::T("TORQUE_BOOST"),
        FTok::T(") = "), has ? FTok::N(f6(c[1])) : FTok::V("P", "2")
    });

    DrawFormula(hdc, x0 + 12, y0 + 248, {
        FTok::V("P", "3"), FTok::T(" = "),
        FTok::F({ FTok::T("("), has ? FTok::N(f2(c[13])) : FTok::V("T", "peak"), FTok::T(" · "), FTok::N("1.3558"),
                  FTok::T(") · ("), has ? FTok::N(f1(c[14])) : FTok::S("RPM", "peak"), FTok::T(" · "), FTok::N("0.104719758"), FTok::T(") · "), FTok::N("0.8") },
                { has ? FTok::N(f2(c[12])) : FTok::T("MASS") }),
        FTok::T(" = "), has ? FTok::N(f6(c[2])) : FTok::V("P", "3")
    });

    int xP4 = DrawFormula(hdc, x0 + 12, y0 + 282, {
        FTok::V("P", "4"), FTok::T(" = "), FTok::N("0.51"), FTok::T(" · "),
        has ? FTok::N(f6(c[3] / 0.51f)) : FTok::V("F", "x"), FTok::T(" = "),
        has ? FTok::N(f6(c[3])) : FTok::V("P", "4"), FTok::T(",")
    });
    DrawFormula(hdc, std::max(xP4 + 26, x0 + 310), y0 + 282, {
        FTok::V("P", "5"), FTok::T(" = "), FTok::N("0.76"), FTok::T(" · "),
        has ? FTok::N(f6(c[3] / 0.51f)) : FTok::V("F", "x"), FTok::T(" = "),
        has ? FTok::N(f6(c[4])) : FTok::V("P", "5")
    });

    int xP6 = DrawFormula(hdc, x0 + 12, y0 + 318, {
        FTok::V("P", "6"), FTok::T(" = "),
        FTok::F({ has ? FTok::N(f4(c[5] * c[12])) : FTok::S("AeroDownforce", "[+0x284]") },
                { has ? FTok::N(f2(c[12])) : FTok::T("MASS") }),
        FTok::T(" = "), has ? FTok::N(f6(c[5])) : FTok::V("P", "6"), FTok::T(",")
    });
    DrawFormula(hdc, std::max(xP6 + 26, x0 + 310), y0 + 318, {
        FTok::V("P", "7"), FTok::T(" = "),
        FTok::F({ has ? FTok::N(f4(c[6] * c[12])) : FTok::S("AeroDrag", "[+0x268]") },
                { has ? FTok::N(f2(c[12])) : FTok::T("MASS") }),
        FTok::T(" = "), has ? FTok::N(f6(c[6])) : FTok::V("P", "7")
    });
}

static void PaintTabDataInfo(HDC hdc, int x0, int y0, int W, int H) {
    char topH[160];
    std::snprintf(topH, sizeof(topH), "Data Info  |  Источник: %s  |  BP [0x%08X | Type=0x%02X]  |  Только просмотр",
                  g_app.shmCopy.detailSourceLabel[0] ? g_app.shmCopy.detailSourceLabel : "—",
                  g_app.shmCopy.activeBpAddr, g_app.shmCopy.activeBpCarType);
    DrawClippedUtf8(hdc, x0 + 12, y0 + 5, x0 + W - 12, y0 + 21, topH, g_app.fnt8B, Col::NUM);

    const int colW[8] = { 48, 108, 56, 224, 84, 110, 88, W - 24 - (48 + 108 + 56 + 224 + 84 + 110 + 88) };
    const char* colT[8] = { "Слот", "Категория", "idx/cnt", "Этап — Бренд", "phys key", "parKey", "brandHash", "w2 (hex)" };
    int xC[9] = { x0 + 12 };
    for (int i = 0; i < 8; ++i) xC[i + 1] = xC[i] + colW[i];

    int yHdr = y0 + 22, hdrH = 16;
    DrawBox(hdc, xC[0], yHdr, xC[8], yHdr + hdrH, Col::HEADER_BG, Col::BORDER);
    for (int i = 0; i < 8; ++i) {
        DrawTextUtf8(hdc, xC[i] + 4, yHdr + 2, colT[i], g_app.fnt8B, Col::TITLE);
        if (i > 0) DrawLine(hdc, xC[i], yHdr, xC[i], yHdr + hdrH, Col::GRID);
    }

    int yRow = yHdr + hdrH, rowH = 15;
    for (int r = 0; r < NUM_INSPECT_SLOTS; ++r) {
        const DetailRowView& it = g_app.detailRows[r];
        bool isSel = (r == g_app.selectedDetailRow);
        DrawBox(hdc, xC[0], yRow, xC[8], yRow + rowH, isSel ? Col::ROW_SEL : ((r % 2 == 0) ? Col::ROW_A : Col::ROW_B), isSel ? Col::NUM : Col::GRID);
        for (int i = 1; i < 8; ++i) DrawLine(hdc, xC[i], yRow, xC[i], yRow + rowH, Col::GRID);

        char sBuf[16], iBuf[24], w2Buf[16];
        std::snprintf(sBuf, sizeof(sBuf), "#%03d", it.slotNum);
        std::snprintf(iBuf, sizeof(iBuf), "%u/%u", it.idx, it.mCount);
        std::snprintf(w2Buf, sizeof(w2Buf), "%08X", it.w2);
        DrawTextUtf8(hdc, xC[0] + 4, yRow + 1, sBuf, g_app.fnt8B, isSel ? Col::NUM : Col::SYM);
        DrawTextUtf8(hdc, xC[1] + 4, yRow + 1, it.name, isSel ? g_app.fnt8B : g_app.fnt8, Col::TEXT);
        DrawTextUtf8(hdc, xC[2] + 4, yRow + 1, iBuf, g_app.fnt8, it.idx > 0 ? Col::TEXT : Col::DIM);
        DrawStageBrandText(hdc, xC[3] + 4, yRow + 1, it.stageName + " — ", it.brandName, "", it.isRawBrand, it.idx > 0, it.idx > 0 ? Col::SCULPT_VAL : Col::DIM, xC[4] - 2);
        DrawTextUtf8(hdc, xC[4] + 4, yRow + 1, it.hasPhys ? it.physHex : "—", g_app.fnt8, it.hasPhys ? Col::NUM : Col::DIM);
        DrawTextUtf8(hdc, xC[5] + 4, yRow + 1, it.hasPhys ? it.parName : "—", g_app.fnt8, it.hasPhys ? Col::TEXT : Col::DIM);
        DrawTextUtf8(hdc, xC[6] + 4, yRow + 1, !it.brandHash.empty() ? it.brandHash : "—", g_app.fnt8, !it.brandHash.empty() ? Col::SYM : Col::DIM);
        DrawTextUtf8(hdc, xC[7] + 4, yRow + 1, w2Buf, g_app.fnt8, Col::DIM);
        yRow += rowH;
    }

    int yCard = yRow + 5;
    DrawBox(hdc, x0 + 12, yCard, x0 + W - 12, y0 + H - 6, Col::BG, Col::BORDER);
    const DetailRowView& sel = g_app.detailRows[g_app.selectedDetailRow];

    char hdrPre[128], hdrSuf[128];
    std::snprintf(hdrPre, sizeof(hdrPre), "#%03d [%s]: %s — ", sel.slotNum, sel.name.c_str(), sel.stageName.c_str());
    std::snprintf(hdrSuf, sizeof(hdrSuf), "  (w2=%u / %u)  —  только отображение", sel.idx, sel.mCount);
    DrawStageBrandText(hdc, x0 + 18, yCard + 5, hdrPre, sel.brandName, hdrSuf, sel.isRawBrand, true, Col::NUM, x0 + W - 18);
    DrawLine(hdc, x0 + 16, yCard + 23, x0 + W - 16, yCard + 23, Col::GRID);

    int yD = yCard + 29;
    auto dLine = [&](const char* lbl, const std::string& val, COLORREF col) {
        if (yD > y0 + H - 22) return;
        DrawTextUtf8(hdc, x0 + 18, yD, lbl, g_app.fnt8B, Col::SYM);
        SelectObject(hdc, g_app.fnt8B);
        int xV = x0 + 22 + TextWidth(hdc, Utf8ToWide(lbl));
        DrawClippedUtf8(hdc, xV, yD, x0 + W - 18, yD + 16, val.empty() ? "—" : val, g_app.fnt8, col);
        yD += 18;
    };

    char b[256];
    if (sel.hasPhys) {
        std::snprintf(b, sizeof(b), "phys key=%s   col=%08X   par=%s   [%s]%s",
                      sel.physHex.c_str(), sel.physCol, sel.parName.c_str(), sel.fullLabel.c_str(),
                      (sel.cost > 0) ? ("  ($" + std::to_string(sel.cost) + ")").c_str() : "");
        dLine("Профиль 6B868DDF:", b, Col::SCULPT_VAL);
    } else {
        std::snprintf(b, sizeof(b), "отсутствует (arr=%08X)", sel.arr6B8);
        dLine("Профиль 6B868DDF:", b, Col::DIM);
    }

    if (sel.hasMeta3EF) {
        std::string d1, d2;
        for (int i = 0; i < 14; ++i) {
            char hx[12]; std::snprintf(hx, sizeof(hx), "%08X", sel.dws[i]);
            if (i < 7) { if (!d1.empty()) d1 += " "; d1 += hx; }
            else       { if (!d2.empty()) d2 += " "; d2 += hx; }
        }
        if (!sel.strs.empty()) d2 += "  STR: " + sel.strs;
        char l1[40], l2[40];
        std::snprintf(l1, sizeof(l1), "3EF64943[%u] +00..18:", sel.idx);
        std::snprintf(l2, sizeof(l2), "3EF64943[%u] +1C..34:", sel.idx);
        dLine(l1, d1, Col::NUM);
        dLine(l2, d2, Col::NUM);
    } else {
        dLine("3EF64943[idx]:", "нет таблицы (infoColl=00000000)", Col::DIM);
    }

    if (sel.hasPhys) dLine("part_physics attrs:", sel.physAttrs, Col::TEXT);
    std::snprintf(b, sizeof(b), "ptr=%08X  mKey=%08X  lay=[%s]  infoColl=%08X  3EF=%08X",
                  sel.w1, sel.mKey, sel.layoutStr.c_str(), sel.infoColl, sel.arr3EF);
    dLine("car_parts / info:", b, Col::DIM);
    if (!sel.visAttrs.empty()) dLine("visTab (визуал):", sel.visAttrs, Col::DIM);
    std::snprintf(b, sizeof(b), "bp=%08X   pvehicle=%08X   Car=%s   w2=%08X",
                  g_app.shmCopy.activeBpAddr, g_app.shmCopy.pvehCollPtr,
                  g_app.shmCopy.baseHash[0] ? g_app.shmCopy.baseHash : "—", sel.w2);
    dLine("Служебные адреса:", b, Col::DIM);
}

static void PaintBottomPanel(HDC hdc) {
    int x0 = 8, y0 = 523, W = WIN_W - 16, H = WIN_H - y0 - 8;
    int tabH = 26;

    FillRectCol(hdc, x0, y0, x0 + W, y0 + tabH, Col::TAB_STRIP);
    FillRectCol(hdc, x0, y0 + tabH, x0 + W, y0 + H, Col::PANEL_BG);
    DrawLine(hdc, x0, y0 + tabH, x0 + W, y0 + tabH, Col::BORDER);

    struct TabDef { int id; int btnId; const char* title; int x1, x2; };
    TabDef tabs[4] = {
        { 1, BTN_TAB_1, "Тюнинг",     x0 + 6,   x0 + 136 },
        { 2, BTN_TAB_2, "Совпадения", x0 + 140, x0 + 280 },
        { 3, BTN_TAB_3, "Расчеты",    x0 + 284, x0 + 414 },
        { 4, BTN_TAB_4, "Data Info",  x0 + 418, x0 + 548 },
    };

    for (const auto& t : tabs) {
        bool act = (g_app.activeTab == t.id);
        int ty = y0 + (act ? 2 : 5);
        FillRectCol(hdc, t.x1, ty, t.x2, y0 + tabH + (act ? 1 : 0), act ? Col::PANEL_BG : Col::TAB_INACT);
        DrawLine(hdc, t.x1, ty, t.x1, y0 + tabH, Col::BORDER);
        DrawLine(hdc, t.x1, ty, t.x2, ty, Col::BORDER);
        DrawLine(hdc, t.x2, ty, t.x2, y0 + tabH, Col::BORDER);
        if (act) {
            DrawLine(hdc, t.x1 + 1, ty + 1, t.x2 - 1, ty + 1, Col::NUM);
            DrawLine(hdc, t.x1 + 1, ty + 2, t.x2 - 1, ty + 2, Col::NUM);
            DrawLine(hdc, t.x1 + 1, y0 + tabH, t.x2 - 1, y0 + tabH, Col::PANEL_BG);
        }
        DrawCenteredUtf8(hdc, t.x1, t.x2, ty + ((y0 + tabH - ty) - 13) / 2, t.title, act ? g_app.fnt9B : g_app.fnt9, act ? Col::TEXT : Col::DIM);
        g_app.buttons.push_back({ t.btnId, { t.x1, y0, t.x2, y0 + tabH } });
    }

    int bodyY = y0 + tabH, bodyH = H - tabH;
    if (g_app.activeTab == 1)      PaintTabTuning(hdc, x0, bodyY, W, bodyH);
    else if (g_app.activeTab == 2) PaintTabMatches(hdc, x0, bodyY, W, bodyH);
    else if (g_app.activeTab == 3) PaintTabFormulas(hdc, x0, bodyY, W, bodyH);
    else                           PaintTabDataInfo(hdc, x0, bodyY, W, bodyH);

    DrawLine(hdc, x0, y0 + tabH, x0, y0 + H, Col::BORDER);
    DrawLine(hdc, x0 + W - 1, y0 + tabH, x0 + W - 1, y0 + H, Col::BORDER);
    DrawLine(hdc, x0, y0 + H - 1, x0 + W, y0 + H - 1, Col::BORDER);
}

static void RenderAll(HDC hdc, int W, int H) {
    g_app.buttons.clear();
    g_app.sliderHits.clear();
    FillRectCol(hdc, 0, 0, W, H, Col::BG);
    PaintTopPanel(hdc);
    PaintParamTable(hdc);
    PaintBottomPanel(hdc);
}

// ============================================================================
// Actions & Input Handling
// ============================================================================
static void SendSliderTicks(int sIdx, int ticksInt) {
    if (sIdx < 0 || sIdx >= NUM_SLIDERS) return;
    ShmSlider& st = g_app.shmCopy.sliders[sIdx];
    if (st.locked) return;
    int minT = (int)std::ceil(st.minBound * 10.0f - 1e-4f);
    int maxT = (int)std::floor(st.maxBound * 10.0f + 1e-4f);
    ticksInt = std::max(-10, std::min(10, std::max(minT, std::min(maxT, ticksInt))));
    float nv = ticksInt / 10.0f;
    st.value = nv; // instant visual feedback
    g_app.bridge.sendCommand(CMD_SET_SLIDER, SLIDER_DEFS[sIdx].hash, 0, nv, false);
    InvalidateRect(g_app.hwnd, nullptr, FALSE);
}

static void ApplySliderDragAtX(int x) {
    for (const auto& r : g_app.sliderHits) {
        if (r.sliderIdx == g_app.draggingSliderIdx) {
            int w = r.tx2 - r.tx1;
            if (w <= 0) return;
            float frac = std::max(0.0f, std::min(1.0f, float(x - r.tx1) / float(w)));
            int ticks = (int)std::floor(frac * 20.0f - 10.0f + 0.5f);
            SendSliderTicks(r.sliderIdx, ticks);
            return;
        }
    }
}

static void HandleButtonClick(int id) {
    switch (id) {
        case BTN_MODE_TOGGLE:
            g_app.useStockDB = !g_app.useStockDB;
            if (!g_app.useStockDB && g_app.shmCopy.hasValidSnap) {
                std::memcpy(g_app.tempInitVals.data(), g_app.shmCopy.curVals, sizeof(float) * 21);
                g_app.hasTempInit = true;
            }
            g_app.statusLine = g_app.useStockDB ? "Статус: Включено сравнение с базой СТОКА." : "Статус: Включено сравнение с временным снимком.";
            break;
        case BTN_TEMP_SNAP:
            g_app.useStockDB = false;
            if (g_app.shmCopy.hasValidSnap) {
                std::memcpy(g_app.tempInitVals.data(), g_app.shmCopy.curVals, sizeof(float) * 21);
                g_app.hasTempInit = true;
            }
            g_app.statusLine = "Статус: Текущие значения зафиксированы как временный эталон.";
            break;
        case BTN_TOPMOST:
            g_app.stayOnTop = !g_app.stayOnTop;
            SetWindowPos(g_app.hwnd, g_app.stayOnTop ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
            break;
        case BTN_KMH:
            g_app.useKmh = !g_app.useKmh;
            break;
        case BTN_BP_RESCAN: {
            std::string msg; bool ok = false;
            if (g_app.bridge.sendCommand(CMD_RESCAN_BP, 0, 0, 0.0f, true, &msg, &ok)) {
                g_app.statusLine = "Статус: " + msg;
            }
            break;
        }
        case BTN_BP_PREV:
        case BTN_BP_NEXT:
        case BTN_BP_AUTO: {
            uint32_t mode = (id == BTN_BP_NEXT) ? 1u : ((id == BTN_BP_PREV) ? 2u : 0u);
            std::string msg; bool ok = false;
            if (g_app.bridge.sendCommand(CMD_CYCLE_BP, mode, 0, 0.0f, true, &msg, &ok)) {
                g_app.statusLine = "Статус: " + msg;
            }
            break;
        }
        case BTN_TAB_1: case BTN_TAB_2: case BTN_TAB_3: case BTN_TAB_4:
            g_app.activeTab = (id - BTN_TAB_1) + 1;
            if (g_app.activeTab == 4) g_app.browseIdx = -1;
            g_app.rebuildDetailRows();
            break;
        case BTN_PART_PREV: {
            const auto& sel = g_app.detailRows[g_app.selectedDetailRow];
            if (sel.mCount > 0) {
                int curI = (g_app.browseIdx >= 0) ? g_app.browseIdx : (int)sel.idx;
                g_app.browseIdx = std::max(0, curI - 1);
                g_app.scrollSame = g_app.scrollDiff = 0;
                g_app.rebuildDetailRows();
            }
            break;
        }
        case BTN_PART_NEXT: {
            const auto& sel = g_app.detailRows[g_app.selectedDetailRow];
            if (sel.mCount > 0) {
                int curI = (g_app.browseIdx >= 0) ? g_app.browseIdx : (int)sel.idx;
                g_app.browseIdx = std::min((int)sel.mCount - 1, curI + 1);
                g_app.scrollSame = g_app.scrollDiff = 0;
                g_app.rebuildDetailRows();
            }
            break;
        }
        case BTN_PART_INSTALL: {
            const auto& sel = g_app.detailRows[g_app.selectedDetailRow];
            uint32_t targetIdx = (g_app.browseIdx >= 0) ? (uint32_t)g_app.browseIdx : sel.idx;
            std::string msg; bool ok = false;
            if (g_app.bridge.sendCommand(CMD_INSTALL_PART, (uint32_t)g_app.selectedDetailRow, targetIdx, 0.0f, true, &msg, &ok)) {
                g_app.flashOk = ok;
                g_app.flashMsg = (ok ? "✓ " : "✗ ") + msg;
                g_app.flashTick = GetTickCount();
                g_app.statusLine = "Статус: " + g_app.flashMsg;
                if (ok) g_app.browseIdx = -1;
            }
            break;
        }
        case BTN_SAME_UP: g_app.scrollSame = std::max(0, g_app.scrollSame - 4); break;
        case BTN_SAME_DN: g_app.scrollSame += 4; break;
        case BTN_DIFF_UP: g_app.scrollDiff = std::max(0, g_app.scrollDiff - 4); break;
        case BTN_DIFF_DN: g_app.scrollDiff += 4; break;
        case BTN_RESET_SLIDERS: {
            std::string msg; bool ok = false;
            if (g_app.bridge.sendCommand(CMD_RESET_SLIDERS, 0, 0, 0.0f, true, &msg, &ok)) {
                g_app.statusLine = "Статус: " + msg;
            }
            break;
        }
    }
    InvalidateRect(g_app.hwnd, nullptr, FALSE);
}

// ============================================================================
// Win32 Window Procedure & Main Loop
// ============================================================================
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE:
            SetTimer(hwnd, 1, 16, nullptr);
            return 0;

        case WM_TIMER: {
            g_app.bridge.tickConnect(g_app.dbs);
            bool fastChg = false, catChg = false;
            g_app.bridge.readFastSnapshot(g_app.shmCopy, g_app.lastFastSeq, fastChg);
            g_app.bridge.readCatalogSnapshot(g_app.shmCopy, g_app.lastCatSeq, catChg);

            bool dirty = fastChg || catChg;
            if (fastChg && g_app.shmCopy.baseHash[0] != '\0') {
                std::string bh = g_app.shmCopy.baseHash;
                std::string mk = GetModeMeta(g_app.shmCopy.carMode).key;
                if (bh != g_app.activeBaseHash || mk != g_app.activeModeKey) {
                    bool carChg = (bh != g_app.activeBaseHash);
                    g_app.activeBaseHash = bh;
                    g_app.activeModeKey  = mk;
                    g_app.browseIdx = -1;
                    g_app.scrollSame = g_app.scrollDiff = 0;
                    if (carChg && g_app.shmCopy.hasValidSnap) {
                        std::memcpy(g_app.tempInitVals.data(), g_app.shmCopy.curVals, sizeof(float) * 21);
                        g_app.hasTempInit = true;
                    }
                    bool exactM = false;
                    if (g_app.dbs.getStockValsForMode(bh, mk, exactM)) {
                        auto mm = GetModeMeta(g_app.shmCopy.carMode);
                        g_app.statusLine = exactM
                            ? (std::string("Статус: Найдена в базе! Загружен точный сток для режима ") + mm.name + ".")
                            : (std::string("Статус: Модель найдена в базе (загружен базовый сток для ") + mm.name + ").");
                    } else {
                        g_app.statusLine = "Статус: Модели нет в базе стока (используется временный снимок).";
                    }
                }
            }
            if (dirty) {
                uint64_t dh = 1469598103934665603ULL;
                auto mx = [&](uint32_t v) { dh = (dh ^ v) * 1099511628211ULL; };
                mx(g_app.lastCatSeq); mx(g_app.shmCopy.activeBpAddr);
                mx((uint32_t)g_app.activeTab); mx((uint32_t)g_app.selectedDetailRow); mx((uint32_t)(g_app.browseIdx + 2));
                for (int i = 0; i < NUM_INSPECT_SLOTS; ++i) {
                    mx(g_app.shmCopy.inspectW1[i]); mx(g_app.shmCopy.inspectW2[i]);
                }
                if (catChg || dh != g_app.lastDetailStateHash) {
                    g_app.lastDetailStateHash = dh;
                    g_app.rebuildDetailRows();
                }
                InvalidateRect(hwnd, nullptr, FALSE);
            } else if (!g_app.flashMsg.empty() && (GetTickCount() - g_app.flashTick) >= 4000u) {
                g_app.flashMsg.clear();
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }

        case WM_MOUSEMOVE: {
            int x = GET_X_LPARAM(lParam), y = GET_Y_LPARAM(lParam);
            if (g_app.draggingSliderIdx >= 0) {
                ApplySliderDragAtX(x);
                return 0;
            }
            int newHover = 0;
            POINT pt = { x, y };
            for (const auto& b : g_app.buttons) {
                if (PtInRect(&b.rc, pt)) { newHover = b.id; break; }
            }
            if (newHover != g_app.hoverBtnId) {
                g_app.hoverBtnId = newHover;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }

        case WM_LBUTTONDOWN:
        case WM_RBUTTONDOWN: {
            int x = GET_X_LPARAM(lParam), y = GET_Y_LPARAM(lParam);
            bool isRight = (msg == WM_RBUTTONDOWN);
            POINT pt = { x, y };

            // Check buttons (LMB only)
            if (!isRight) {
                for (const auto& b : g_app.buttons) {
                    if (PtInRect(&b.rc, pt)) {
                        HandleButtonClick(b.id);
                        return 0;
                    }
                }
            }

            // Tab 1 sliders
            if (g_app.activeTab == 1) {
                for (const auto& r : g_app.sliderHits) {
                    if (y >= r.y1 && y <= r.y2) {
                        if (!isRight && x >= r.capL1 && x <= r.capL2) {
                            float v = g_app.shmCopy.sliders[r.sliderIdx].value;
                            int curT = (v >= 0.0f) ? (int)std::floor(v * 10.0f + 0.5f) : (int)std::ceil(v * 10.0f - 0.5f);
                            SendSliderTicks(r.sliderIdx, curT - 1);
                            return 0;
                        }
                        if (!isRight && x >= r.capR1 && x <= r.capR2) {
                            float v = g_app.shmCopy.sliders[r.sliderIdx].value;
                            int curT = (v >= 0.0f) ? (int)std::floor(v * 10.0f + 0.5f) : (int)std::ceil(v * 10.0f - 0.5f);
                            SendSliderTicks(r.sliderIdx, curT + 1);
                            return 0;
                        }
                        if (x >= r.tx1 && x <= r.tx2) {
                            if (isRight) {
                                SendSliderTicks(r.sliderIdx, 0);
                            } else {
                                g_app.draggingSliderIdx = r.sliderIdx;
                                SetCapture(hwnd);
                                ApplySliderDragAtX(x);
                            }
                            return 0;
                        }
                    }
                }
            }

            // Tab 2 & Tab 4: 10-row slot selection (y = 523+26+22+16 .. +150 = 587..737)
            if (!isRight && (g_app.activeTab == 2 || g_app.activeTab == 4)) {
                int tableTop = 523 + 26 + 22 + 16;
                if (x >= 20 && x <= WIN_W - 20 && y >= tableTop && y < tableTop + NUM_INSPECT_SLOTS * 15) {
                    g_app.selectedDetailRow = (y - tableTop) / 15;
                    g_app.browseIdx = -1;
                    g_app.scrollSame = g_app.scrollDiff = 0;
                    g_app.rebuildDetailRows();
                    InvalidateRect(hwnd, nullptr, FALSE);
                    return 0;
                }
            }
            return 0;
        }

        case WM_LBUTTONUP:
            if (g_app.draggingSliderIdx >= 0) {
                g_app.draggingSliderIdx = -1;
                ReleaseCapture();
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;

        case WM_MOUSEWHEEL: {
            if (g_app.activeTab == 2) {
                POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
                ScreenToClient(hwnd, &pt);
                int delta = GET_WHEEL_DELTA_WPARAM(wParam);
                int step = (delta > 0) ? -2 : 2;
                if (pt.y >= 760) {
                    if (pt.x < WIN_W / 2) g_app.scrollSame = std::max(0, g_app.scrollSame + step);
                    else                  g_app.scrollDiff = std::max(0, g_app.scrollDiff + step);
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
            }
            return 0;
        }

        case WM_ERASEBKGND:
            return 1; // double-buffered, zero flicker!

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT rc; GetClientRect(hwnd, &rc);
            int W = rc.right - rc.left, H = rc.bottom - rc.top;
            if (W > 0 && H > 0) {
                if (!g_app.memDC || W != g_app.bufW || H != g_app.bufH) {
                    if (g_app.memBmp) DeleteObject(g_app.memBmp);
                    if (g_app.memDC)  DeleteDC(g_app.memDC);
                    g_app.memDC  = CreateCompatibleDC(hdc);
                    g_app.memBmp = CreateCompatibleBitmap(hdc, W, H);
                    SelectObject(g_app.memDC, g_app.memBmp);
                    g_app.bufW = W; g_app.bufH = H;
                }
                RenderAll(g_app.memDC, W, H);
                BitBlt(hdc, 0, 0, W, H, g_app.memDC, 0, 0, SRCCOPY);
            }
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_DESTROY:
            KillTimer(hwnd, 1);
            g_app.bridge.shutdownAndUnload();
            g_app.freeGdi();
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int nCmdShow) {
    // Повышаем точность системного таймера Windows (4 мс) без лишних зависимостей линковки (winmm.dll)
    using TimePeriodFn = UINT(WINAPI*)(UINT);
    HMODULE hWinmm = LoadLibraryA("winmm.dll");
    TimePeriodFn pTimeBegin = hWinmm ? reinterpret_cast<TimePeriodFn>(GetProcAddress(hWinmm, "timeBeginPeriod")) : nullptr;
    TimePeriodFn pTimeEnd   = hWinmm ? reinterpret_cast<TimePeriodFn>(GetProcAddress(hWinmm, "timeEndPeriod")) : nullptr;
    if (pTimeBegin) pTimeBegin(4);

    g_app.initFonts();
    g_app.dbs.initPaths();
    g_app.rebuildDetailRows();

    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"statsEdpsMainWnd";
    RegisterClassExW(&wc);

    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT wr = { 0, 0, WIN_W, WIN_H };
    AdjustWindowRect(&wr, style, FALSE);

    int ww = wr.right - wr.left, wh = wr.bottom - wr.top;
    int sx = (GetSystemMetrics(SM_CXSCREEN) - ww) / 2;
    int sy = std::max(10, (GetSystemMetrics(SM_CYSCREEN) - wh) / 2);

    std::wstring wndTitle = Utf8ToWide(APP_FULL_TITLE);
    g_app.hwnd = CreateWindowExW(
        0, wc.lpszClassName,
        wndTitle.c_str(),
        style, sx, sy, ww, wh, nullptr, nullptr, hInst, nullptr
    );
    if (!g_app.hwnd) return 1;

    ShowWindow(g_app.hwnd, nCmdShow);
    UpdateWindow(g_app.hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (pTimeEnd) pTimeEnd(4);
    if (hWinmm) FreeLibrary(hWinmm);
    return 0;
}
