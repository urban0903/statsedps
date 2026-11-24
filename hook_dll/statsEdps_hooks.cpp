#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../common/shared_ipc.h"
#include "../common/brand_db.h"

static_assert(sizeof(void*) == 4, "statsEdps_hooks.dll MUST be compiled as 32-bit x86 (Win32)!");

// ============================================================================
// Global State & Hook Storage
// ============================================================================
static HMODULE  g_hSelf        = nullptr;
static uint32_t g_nfsBase      = 0x00400000u;
static HANDLE   g_hMap         = nullptr;
static HANDLE   g_hCmdEvent    = nullptr;
static ShmSharedState* g_shm   = nullptr;
static HANDLE   g_hWorker      = nullptr;
static std::atomic<bool> g_stopWorker{false};
static uint8_t* g_trampMem     = nullptr;

using CalcP1P7Fn   = void(__cdecl*)(uint32_t carPtr, uint32_t massBits, float* outBuf);
using LookupPartFn = uint32_t(__cdecl*)(uint32_t outPart, uint32_t carType0, uint32_t a, uint32_t b, uint32_t c, uint32_t slot);
using ThisCall0Fn  = uint32_t(__cdecl*)(uint32_t targetFn, void* thisPtr);
using ThisCall1Fn  = uint32_t(__cdecl*)(uint32_t targetFn, void* thisPtr, uint32_t a1);
using ThisCall2Fn  = uint32_t(__cdecl*)(uint32_t targetFn, void* thisPtr, uint32_t a1, uint32_t a2);
static CalcP1P7Fn   g_callCalcP1P7   = nullptr;
static LookupPartFn g_callLookupPart = nullptr;
static ThisCall0Fn  g_callThis0      = nullptr;
static ThisCall1Fn  g_callThis1      = nullptr;
static ThisCall2Fn  g_callThis2      = nullptr;

// Main-thread safe dispatcher (WH_GETMESSAGE on nfs.exe main window thread)
static HWND    g_gameHwnd   = nullptr;
static DWORD   g_mainTid    = 0;
static HHOOK   g_hMsgHook   = nullptr;
static HANDLE  g_hMtDoneEvt = nullptr;
static std::atomic<int> g_mtPending{0};
static volatile uint32_t g_inSelfDynoRecalc = 0;

// Captured exact pvehArg passed to 00402380 (Attrib::Instance ctor) for live Dyno recalc
static volatile uint32_t g_lastPvehArg   = 0;
static volatile uint32_t g_lastPvehArgBp = 0;

struct MainThreadJob {
    enum Type { JOB_NONE = 0, JOB_RECALC_DYNO = 1, JOB_FIND_PACKED16 = 2 };
    Type     type = JOB_NONE;
    uint32_t bpAddr = 0;
    uint32_t pvehArg = 0;
    uint32_t mode = 0;
    // For JOB_FIND_PACKED16
    uint32_t carType0 = 0;
    uint32_t feSlot = 0;
    uint32_t targetW1 = 0;
    uint32_t targetIdx = 0;
    uint16_t curPacked = 0xFFFFu;
    bool     found = false;
    uint16_t outPacked = 0xFFFFu;
    uint32_t outDbPart[4] = {};
};
static MainThreadJob g_mtJob;

// Hook-captured volatile variables
static volatile uint32_t g_monIsBot       = 0;
static volatile uint32_t g_monCtxMode     = CTX_GARAGE;
static volatile uint32_t g_raceDayActive  = 0;
static volatile uint32_t g_playerObjPtr   = 0;
static volatile uint32_t g_playerCarPtr   = 0;
static volatile uint32_t g_playerHitSeq   = 0;
static volatile uint32_t g_botHitSeq      = 0;
static volatile uint32_t g_lastBotHitTick = 0;

static volatile uint32_t g_tuningScreenPtr   = 0;
static volatile uint32_t g_tuningDataPtr     = 0;
static volatile uint32_t g_inTuningUi        = 0;
static volatile uint32_t g_tuningDataSeq     = 0;
static volatile uint32_t g_tuningSyncSeq     = 0;
static volatile uint32_t g_tuningMapPtr   = 0;
static volatile uint32_t g_tuningMapSeq   = 0;
static volatile uint32_t g_lastRbtreeRet  = 0;

// Direct O(1) Blueprint* & FECustomizationRecord* capture from BuildCarSpec (0078EBAD) & WriteToBlueprint (0078A672)
static volatile uint32_t g_directBuildBp    = 0;
static volatile uint32_t g_directBuildSeq   = 0;
static volatile uint32_t g_directCustRec    = 0;
static volatile uint32_t g_directCustBp     = 0;
static volatile uint32_t g_directCustSeq    = 0;

struct BpRingEntry {
    volatile uint32_t bp;
    volatile uint32_t custRec;
    volatile uint32_t isBot;
    volatile uint32_t seq;
};
static constexpr int BP_RING_CAP = 32;
static BpRingEntry       g_bpRing[BP_RING_CAP] = {};
static volatile uint32_t g_bpRingIdx = 0;
static volatile uint32_t g_bpRingSeq = 0;

struct UiWidgetEntry {
    volatile uint32_t hash;
    volatile uint32_t widgetPtr;
};
static constexpr int UI_WIDGET_CAP = 32;
static UiWidgetEntry g_uiWidgets[UI_WIDGET_CAP] = {};

static volatile LONG     g_inGarageHook = 0;
static volatile uint32_t g_hookSnapSeq  = 0;
struct HookGarageSnap {
    float    cur[21];
    uint32_t chainKeys[8];
    uint32_t chainSrcs[8];
    uint32_t carMode;
    uint32_t carPtr;
    uint32_t ratingPtr;
    uint32_t pvehCollPtr;
    uint32_t retAddr;
};
static HookGarageSnap g_pubSnap = {};

#pragma pack(push, 1)
struct HookRegs {
    uint32_t eflags;
    uint32_t edi;
    uint32_t esi;
    uint32_t ebp;
    uint32_t esp_orig;
    uint32_t ebx;
    uint32_t edx;
    uint32_t ecx;
    uint32_t eax;
};
#pragma pack(pop)

// ============================================================================
// Safe In-Process Memory Readers/Writers
// ============================================================================
static inline bool IsValidPtr(uint32_t p) { return p >= 0x00010000u && p <= 0x7FFF0000u; }
static inline bool IsStaticPtr(uint32_t p) { return p >= 0x00400000u && p <= 0x7FFF0000u; }
static inline bool IsFiniteFloat(float v) { return std::isfinite(v) && std::fabs(v) < 1e7f; }

static inline bool SafeRead(uint32_t addr, void* dst, size_t len) {
    if (!IsValidPtr(addr) || len == 0) return false;
    SIZE_T br = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(addr), dst, len, &br) && br == len;
}
static inline uint32_t SafeReadU32(uint32_t addr, uint32_t def = 0) {
    uint32_t v = def;
    return SafeRead(addr, &v, 4) ? v : def;
}
static inline float SafeReadF32(uint32_t addr, float def = 0.0f) {
    float v = def;
    return SafeRead(addr, &v, 4) ? v : def;
}
static inline bool SafeWrite(uint32_t addr, const void* src, size_t len) {
    if (!IsValidPtr(addr) || len == 0) return false;
    SIZE_T bw = 0;
    return WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<LPVOID>(addr), src, len, &bw) && bw == len;
}
static inline bool SafeWriteU32(uint32_t addr, uint32_t v) { return SafeWrite(addr, &v, 4); }
static inline bool SafeWriteF32(uint32_t addr, float v) { return SafeWrite(addr, &v, 4); }

static std::string TryReadAsciiSafe(uint32_t addr) {
    if (!IsValidPtr(addr)) return "";
    char buf[44] = {};
    if (!SafeRead(addr, buf, 40)) {
        if (!SafeRead(addr, buf, 16)) return "";
    }
    std::string out;
    out.reserve(40);
    for (int i = 0; i < 40; ++i) {
        unsigned char c = static_cast<unsigned char>(buf[i]);
        if (c == 0) break;
        if (c < 32 || c > 126) return "";
        out.push_back(static_cast<char>(c));
    }
    return (out.size() >= 3) ? out : "";
}

// ============================================================================
// C++ Hook Handlers
// ============================================================================
extern "C" void __cdecl OnCheckPlayerMon(HookRegs* r) {
    g_raceDayActive = 1;
    uint32_t obj = r->eax;
    uint32_t botVtbl = g_nfsBase + (0x00982D6Cu - 0x00400000u);
    if (!IsValidPtr(obj) || *reinterpret_cast<const uint32_t*>(obj) == botVtbl) {
        g_monIsBot = 1;
        g_botHitSeq = g_botHitSeq + 1;
        g_lastBotHitTick = GetTickCount();
        return;
    }
    g_playerObjPtr = obj;
    g_monIsBot = 0;
    g_monCtxMode = CTX_RACEDAY_PLAY;
    g_playerHitSeq = g_playerHitSeq + 1;
}

extern "C" void __cdecl OnTuningDataCtor(HookRegs* r) {
    uint32_t td = r->ecx;
    if (!IsValidPtr(td)) return;
    g_tuningDataPtr = td;
    g_inTuningUi = 1;
    g_monIsBot = 0;
    g_monCtxMode = CTX_TUNING_UI;
    g_tuningDataSeq = g_tuningDataSeq + 1;
}

extern "C" void __cdecl OnTuningDataDtor(HookRegs* /*r*/) {
    uint32_t td = g_tuningDataPtr;
    uint32_t tm = g_tuningMapPtr;
    if (td != 0 && tm >= td && tm <= td + 0x80u) {
        g_tuningMapPtr = 0;
    }
    g_inTuningUi = 0;
    g_tuningDataPtr = 0;
    g_tuningScreenPtr = 0;
}

// Hook at nfs.exe+1F05D6 (TuningScreen::SyncWidgetsAndRecalc): instant 0ms notification on slider change!
extern "C" void __cdecl OnTuningScreenSync(HookRegs* r) {
    uint32_t screen = r->ebx;
    if (!IsValidPtr(screen)) return;
    g_tuningScreenPtr = screen;
    uint32_t td = *reinterpret_cast<const uint32_t*>(screen + 0xDC);
    if (IsValidPtr(td)) g_tuningDataPtr = td;
    g_inTuningUi = 1;
    g_monIsBot = 0;
    g_monCtxMode = CTX_TUNING_UI;
    g_tuningSyncSeq = g_tuningSyncSeq + 1;
    if (g_hCmdEvent) SetEvent(g_hCmdEvent);
}

extern "C" void __cdecl OnUiSliderCtor(HookRegs* r) {
    uint32_t widget = r->eax;
    uint32_t edi = r->edi;
    if (!IsValidPtr(widget) || edi < 0x00010010u || edi > 0x7FFF0000u) return;
    uint32_t k0 = *reinterpret_cast<const uint32_t*>(edi - 0x10);
    if (k0 != SLIDER_CLASS_STD && k0 != SLIDER_CLASS_CAM) return;
    uint32_t k1 = *reinterpret_cast<const uint32_t*>(edi - 0x0C);
    for (int i = 0; i < UI_WIDGET_CAP; ++i) {
        if (g_uiWidgets[i].hash == k1 || g_uiWidgets[i].hash == 0) {
            g_uiWidgets[i].hash = k1;
            g_uiWidgets[i].widgetPtr = widget;
            g_monIsBot = 0;
            break;
        }
    }
}

extern "C" void __cdecl OnRbTreeSlider(uint32_t mapPtr, uint32_t retAddr) {
    if (g_inTuningUi == 1) {
        uint32_t td = g_tuningDataPtr;
        if (td != 0 && mapPtr == td + 0x10u) return;
    }
    g_tuningMapPtr = mapPtr;
    g_lastRbtreeRet = retAddr;
    g_tuningMapSeq = g_tuningMapSeq + 1;
}

static void PushBpRing(uint32_t bp, uint32_t custRec, uint32_t isBot) {
    if (!IsValidPtr(bp)) return;
    uint32_t newSeq = g_bpRingSeq + 1;
    g_bpRingSeq = newSeq;

    int slot = -1;
    for (int i = 0; i < BP_RING_CAP; ++i) {
        if (g_bpRing[i].bp == bp) { slot = i; break; }
    }
    if (slot < 0) {
        slot = static_cast<int>(g_bpRingIdx & (BP_RING_CAP - 1));
        g_bpRingIdx = (g_bpRingIdx + 1) & (BP_RING_CAP - 1);
        g_bpRing[slot].bp = bp;
        g_bpRing[slot].custRec = 0;
    }
    if (IsValidPtr(custRec)) g_bpRing[slot].custRec = custRec;
    g_bpRing[slot].isBot = isBot;
    g_bpRing[slot].seq = newSeq;
}

// Hook at nfs.exe+38EBAD (Blueprint::BuildCarSpec): ECX = exact active Blueprint*!
extern "C" void __cdecl OnBuildCarSpec(HookRegs* r) {
    uint32_t bp = r->ecx;
    if (!IsValidPtr(bp)) return;
    uint32_t isBot = (g_inTuningUi == 1) ? 0u : g_monIsBot;
    PushBpRing(bp, 0, isBot);
    if (isBot == 0) {
        g_directBuildBp = bp;
        g_directBuildSeq = g_directBuildSeq + 1;
    }
}

// Hook at nfs.exe+38A672 (FECustomizationRecord::WriteToBlueprint): ECX = FECustomizationRecord*, [esp+68] = Blueprint*!
extern "C" void __cdecl OnWriteToBlueprint(HookRegs* r) {
    uint32_t custRec = r->ecx;
    uint32_t bp = *reinterpret_cast<const uint32_t*>(r->esp_orig + 0x68);
    if (!IsValidPtr(custRec) || !IsValidPtr(bp)) return;
    uint32_t isBot = (g_inTuningUi == 1) ? 0u : g_monIsBot;
    PushBpRing(bp, custRec, isBot);
    if (isBot == 0) {
        g_directCustRec = custRec;
        g_directCustBp  = bp;
        g_directCustSeq = g_directCustSeq + 1;
    }
}

// Hook at nfs.exe+38643D (FECustomizationRecord::SyncFromBlueprint): ECX = FECustomizationRecord*, [esp+E0] = Blueprint*!
extern "C" void __cdecl OnSyncFromBlueprint(HookRegs* r) {
    uint32_t custRec = r->ecx;
    uint32_t bp = *reinterpret_cast<const uint32_t*>(r->esp_orig + 0xE0);
    if (!IsValidPtr(custRec) || !IsValidPtr(bp)) return;
    uint32_t isBot = (g_inTuningUi == 1) ? 0u : g_monIsBot;
    PushBpRing(bp, custRec, isBot);
    if (isBot == 0) {
        g_directCustRec = custRec;
        g_directCustBp  = bp;
        g_directCustSeq = g_directCustSeq + 1;
    }
}

// Hook at nfs.exe+175F70 (Garage) & nfs.exe+1A4BE1 (Tuning): capture exact arg1 passed to 00402380!
extern "C" void __cdecl OnAttribInstCtor(HookRegs* r) {
    if (g_inSelfDynoRecalc != 0) return;
    uint32_t pvehArg = *reinterpret_cast<const uint32_t*>(r->esp_orig + 0x00);
    if (pvehArg != 0) {
        g_lastPvehArg = pvehArg;
        uint32_t bp = g_directBuildBp;
        if (IsValidPtr(bp)) g_lastPvehArgBp = bp;
    }
}

// Hook at nfs.exe+133D50 (FECarCareerEntry::BuildBlueprint): ECX = careerEntry, [esp+04]=bpSlot, [esp+08]=Blueprint*!
extern "C" void __cdecl OnCareerBuildBp(HookRegs* r) {
    uint32_t entry = r->ecx;
    uint32_t bpSlot = *reinterpret_cast<const uint32_t*>(r->esp_orig + 0x04);
    uint32_t bp     = *reinterpret_cast<const uint32_t*>(r->esp_orig + 0x08);
    if (!IsValidPtr(entry) || !IsValidPtr(bp) || bpSlot > 8u) return;
    uint32_t custRec = entry + 4u + bpSlot * 0x7B4u;
    PushBpRing(bp, custRec, 0u);
    g_directCustRec = custRec;
    g_directCustBp  = bp;
    g_directCustSeq = g_directCustSeq + 1;
}

extern "C" void __cdecl OnGarageMain(HookRegs* r) {
    if (InterlockedExchange(&g_inGarageHook, 1) != 0) return;

    uint32_t retAddr   = *reinterpret_cast<const uint32_t*>(r->esp_orig + 0x08);
    uint32_t carPtr    = *reinterpret_cast<const uint32_t*>(r->esp_orig + 0x0C);
    uint32_t ratingPtr = r->edi;

    const uint32_t retGarage = g_nfsBase + (0x00575FEBu - 0x00400000u);
    const uint32_t retCarlot = g_nfsBase + (0x0072EE87u - 0x00400000u);

    bool allowed = false;
    if (g_inSelfDynoRecalc != 0) {
        g_monIsBot = 0;
        allowed = true;
    } else if (g_inTuningUi == 1) {
        g_monIsBot = 0; g_monCtxMode = CTX_TUNING_UI; allowed = true;
    } else if (retAddr == retGarage) {
        g_raceDayActive = 0; g_monIsBot = 0; g_monCtxMode = CTX_GARAGE; allowed = true;
    } else if (retAddr == retCarlot) {
        // 0x0072EE87 вызывается и в Стайлинге / Тюнинге / Магазине, и в Автосалоне — не сбрасываем BP!
        g_raceDayActive = 0; g_monIsBot = 0; g_monCtxMode = CTX_CARLOT; allowed = true;
    } else if (g_monIsBot == 0 || (carPtr != 0 && carPtr == g_playerCarPtr)) {
        g_monIsBot = 0;
        if (g_raceDayActive != 0 && g_monCtxMode != CTX_RACEDAY_HUB) g_monCtxMode = CTX_RACEDAY_PLAY;
        allowed = true;
    }

    if (!allowed || !IsValidPtr(carPtr) || !IsValidPtr(ratingPtr) || !g_callCalcP1P7) {
        InterlockedExchange(&g_inGarageHook, 0);
        return;
    }

    uint32_t pvehColl = *reinterpret_cast<const uint32_t*>(carPtr);
    if (!IsStaticPtr(pvehColl)) {
        InterlockedExchange(&g_inGarageHook, 0);
        return;
    }

    g_playerCarPtr = carPtr;

    HookGarageSnap st = {};
    uint32_t massBits = *reinterpret_cast<const uint32_t*>(ratingPtr + 0x88);
    g_callCalcP1P7(carPtr, massBits, st.cur);

    st.carMode = *reinterpret_cast<const uint32_t*>(ratingPtr + 0x178);
    st.cur[7]  = *reinterpret_cast<const float*>(ratingPtr + 0x14C);
    st.cur[8]  = *reinterpret_cast<const float*>(ratingPtr + 0x150);
    st.cur[9]  = *reinterpret_cast<const float*>(ratingPtr + 0x154);
    st.cur[10] = *reinterpret_cast<const float*>(ratingPtr + 0x158);
    st.cur[11] = *reinterpret_cast<const float*>(ratingPtr + 0x15C);

    st.cur[12] = *reinterpret_cast<const float*>(ratingPtr + 0x88);
    st.cur[13] = *reinterpret_cast<const float*>(ratingPtr + 0x80);
    st.cur[14] = *reinterpret_cast<const float*>(ratingPtr + 0x84);
    st.cur[15] = *reinterpret_cast<const float*>(ratingPtr + 0x78);
    st.cur[16] = *reinterpret_cast<const float*>(ratingPtr + 0x8C);
    st.cur[17] = *reinterpret_cast<const float*>(ratingPtr + 0x00);
    st.cur[18] = *reinterpret_cast<const float*>(ratingPtr + 0x04);
    st.cur[19] = *reinterpret_cast<const float*>(ratingPtr + 0x1C);
    st.cur[20] = *reinterpret_cast<const float*>(ratingPtr + 0x28);

    if (st.cur[12] < 100.0f || st.cur[12] > 10000.0f ||
        st.cur[15] < 10.0f  || st.cur[15] > 10000.0f ||
        st.cur[13] < 10.0f  || st.cur[13] > 10000.0f ||
        st.cur[20] < 40.0f  || st.cur[20] > 500.0f   ||
        st.carMode > 10u) {
        InterlockedExchange(&g_inGarageHook, 0);
        return;
    }

    uint32_t clsPtr = *reinterpret_cast<const uint32_t*>(pvehColl + 0x14);
    uint32_t curNode = pvehColl;
    int staticCount = 0;
    for (int i = 0; i < 8; ++i) {
        if (!IsStaticPtr(curNode)) break;
        if (*reinterpret_cast<const uint32_t*>(curNode + 0x14) != clsPtr) break;
        uint32_t src = *reinterpret_cast<const uint32_t*>(curNode + 0x1C);
        uint32_t key = *reinterpret_cast<const uint32_t*>(curNode + 0x10);
        st.chainSrcs[i] = src;
        st.chainKeys[i] = key;
        if (IsStaticPtr(src)) staticCount++;
        if (key == 0xEEC2271Au) break;
        curNode = *reinterpret_cast<const uint32_t*>(curNode + 0x0C);
    }

    if (staticCount > 0) {
        st.carPtr      = carPtr;
        st.ratingPtr   = ratingPtr;
        st.pvehCollPtr = pvehColl;
        st.retAddr     = retAddr;

        g_hookSnapSeq = g_hookSnapSeq + 1;
        std::memcpy(&g_pubSnap, &st, sizeof(st));
        g_hookSnapSeq = g_hookSnapSeq + 1;
        if (g_hCmdEvent) SetEvent(g_hCmdEvent);
    }

    InterlockedExchange(&g_inGarageHook, 0);
}

// ============================================================================
// Runtime x86-32 Trampoline Emitter (MSVC + MinGW compatible!)
// ============================================================================
class X86Emit {
    uint8_t* base = nullptr;
    size_t   pos  = 0;
public:
    explicit X86Emit(uint8_t* mem) : base(mem), pos(0) {}
    uint32_t curAddr() const { return reinterpret_cast<uint32_t>(base + pos); }
    uint8_t* curPtr()  const { return base + pos; }

    void b(uint8_t v) { base[pos++] = v; }
    void u32(uint32_t v) { std::memcpy(base + pos, &v, 4); pos += 4; }
    void raw(const uint8_t* src, size_t n) { std::memcpy(base + pos, src, n); pos += n; }

    void callRel(uint32_t target) { b(0xE8); u32(target - (curAddr() + 4)); }
    void jmpRel(uint32_t target)  { b(0xE9); u32(target - (curAddr() + 4)); }
    size_t jccPlaceholder(uint8_t op2) { b(0x0F); b(op2); size_t p = pos; u32(0); return p; }
    void patchJcc(size_t patchOff) {
        uint32_t rel = curAddr() - reinterpret_cast<uint32_t>(base + patchOff + 4);
        std::memcpy(base + patchOff, &rel, 4);
    }
    void emitCallHandler(void* fn) {
        b(0x60); b(0x9C); b(0x54);
        callRel(reinterpret_cast<uint32_t>(fn));
        b(0x83); b(0xC4); b(0x04);
        b(0x9D); b(0x61);
    }
};

struct InlineHook {
    uint32_t targetAddr = 0;
    size_t   patchLen   = 0;
    uint8_t  origBytes[16] = {};
    bool     installed  = false;
};
static constexpr int NUM_HOOKS = 13;
static InlineHook g_hooks[NUM_HOOKS] = {};

static bool InstallPatchJump(InlineHook& hk, uint32_t addr, uint32_t stubAddr, size_t len) {
    if (len < 5 || len > 16) return false;
    hk.targetAddr = addr;
    hk.patchLen   = len;
    std::memcpy(hk.origBytes, reinterpret_cast<const void*>(addr), len);

    DWORD oldProt = 0;
    if (!VirtualProtect(reinterpret_cast<LPVOID>(addr), len, PAGE_EXECUTE_READWRITE, &oldProt)) return false;
    uint8_t* dst = reinterpret_cast<uint8_t*>(addr);
    dst[0] = 0xE9;
    uint32_t rel = stubAddr - (addr + 5);
    std::memcpy(dst + 1, &rel, 4);
    for (size_t i = 5; i < len; ++i) dst[i] = 0x90;
    FlushInstructionCache(GetCurrentProcess(), dst, len);
    VirtualProtect(reinterpret_cast<LPVOID>(addr), len, oldProt, &oldProt);
    hk.installed = true;
    return true;
}

static void RestoreAllHooks() {
    for (int i = NUM_HOOKS - 1; i >= 0; --i) {
        if (!g_hooks[i].installed) continue;
        DWORD oldProt = 0;
        LPVOID addr = reinterpret_cast<LPVOID>(g_hooks[i].targetAddr);
        if (VirtualProtect(addr, g_hooks[i].patchLen, PAGE_EXECUTE_READWRITE, &oldProt)) {
            std::memcpy(addr, g_hooks[i].origBytes, g_hooks[i].patchLen);
            FlushInstructionCache(GetCurrentProcess(), addr, g_hooks[i].patchLen);
            VirtualProtect(addr, g_hooks[i].patchLen, oldProt, &oldProt);
        }
        g_hooks[i].installed = false;
    }
}

static bool InstallAllHooks() {
    g_trampMem = reinterpret_cast<uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!g_trampMem) return false;

    X86Emit e(g_trampMem);

    // 0a. Helper: __cdecl void CallCalcP1P7(uint32_t carPtr, uint32_t massBits, float* outBuf)
    g_callCalcP1P7 = reinterpret_cast<CalcP1P7Fn>(e.curPtr());
    e.b(0x53); e.b(0x55); e.b(0x56); e.b(0x57); // push ebx, ebp, esi, edi
    e.b(0x8B); e.b(0x74); e.b(0x24); e.b(0x14); // mov esi, [esp+0x14] (carPtr)
    e.b(0x8B); e.b(0x44); e.b(0x24); e.b(0x18); // mov eax, [esp+0x18] (massBits)
    e.b(0x8B); e.b(0x54); e.b(0x24); e.b(0x1C); // mov edx, [esp+0x1C] (outBuf)
    e.b(0x50); e.b(0x52);                       // push eax; push edx
    e.callRel(g_nfsBase + 0x13130u);
    e.b(0x5F); e.b(0x5E); e.b(0x5D); e.b(0x5B); // pop edi, esi, ebp, ebx
    e.b(0xC3);

    // 0b. Helper: __cdecl uint32_t CallLookupPart(outPart, carType0, a, b, c, slot) -> calls nfs.exe+37F2E0 (__thiscall on 00AED1F8)
    g_callLookupPart = reinterpret_cast<LookupPartFn>(e.curPtr());
    e.b(0x53); e.b(0x55); e.b(0x56); e.b(0x57);
    for (int k = 0; k < 6; ++k) {
        e.b(0xFF); e.b(0x74); e.b(0x24); e.b(0x28);
    }
    e.b(0xB9); e.u32(g_nfsBase + 0x6ED1F8u);
    e.callRel(g_nfsBase + 0x37F2E0u);
    e.b(0x0F); e.b(0xB6); e.b(0xC0);
    e.b(0x5F); e.b(0x5E); e.b(0x5D); e.b(0x5B);
    e.b(0xC3);

    // 0c. Helper: __cdecl uint32_t ThisCall0(targetFn, thisPtr)
    g_callThis0 = reinterpret_cast<ThisCall0Fn>(e.curPtr());
    e.b(0x53); e.b(0x55); e.b(0x56); e.b(0x57);
    e.b(0x8B); e.b(0x44); e.b(0x24); e.b(0x14); // mov eax, [esp+0x14] (targetFn)
    e.b(0x8B); e.b(0x4C); e.b(0x24); e.b(0x18); // mov ecx, [esp+0x18] (thisPtr)
    e.b(0xFF); e.b(0xD0);                       // call eax
    e.b(0x5F); e.b(0x5E); e.b(0x5D); e.b(0x5B);
    e.b(0xC3);

    // 0d. Helper: __cdecl uint32_t ThisCall1(targetFn, thisPtr, a1)
    g_callThis1 = reinterpret_cast<ThisCall1Fn>(e.curPtr());
    e.b(0x53); e.b(0x55); e.b(0x56); e.b(0x57);
    e.b(0x8B); e.b(0x44); e.b(0x24); e.b(0x14); // mov eax, [esp+0x14]
    e.b(0x8B); e.b(0x4C); e.b(0x24); e.b(0x18); // mov ecx, [esp+0x18]
    e.b(0xFF); e.b(0x74); e.b(0x24); e.b(0x1C); // push dword ptr [esp+0x1C] (a1)
    e.b(0xFF); e.b(0xD0);                       // call eax
    e.b(0x5F); e.b(0x5E); e.b(0x5D); e.b(0x5B);
    e.b(0xC3);

    // 0e. Helper: __cdecl uint32_t ThisCall2(targetFn, thisPtr, a1, a2)
    g_callThis2 = reinterpret_cast<ThisCall2Fn>(e.curPtr());
    e.b(0x53); e.b(0x55); e.b(0x56); e.b(0x57);
    e.b(0x8B); e.b(0x44); e.b(0x24); e.b(0x14); // mov eax, [esp+0x14]
    e.b(0x8B); e.b(0x4C); e.b(0x24); e.b(0x18); // mov ecx, [esp+0x18]
    e.b(0xFF); e.b(0x74); e.b(0x24); e.b(0x20); // push dword ptr [esp+0x20] (a2)
    e.b(0xFF); e.b(0x74); e.b(0x24); e.b(0x20); // push dword ptr [esp+0x20] (a1)
    e.b(0xFF); e.b(0xD0);                       // call eax
    e.b(0x5F); e.b(0x5E); e.b(0x5D); e.b(0x5B);
    e.b(0xC3);

    // 1. Hook nfs.exe+2851B0 (5B)
    uint32_t st1 = e.curAddr();
    e.callRel(g_nfsBase + 0x26C030u);
    e.emitCallHandler(reinterpret_cast<void*>(&OnCheckPlayerMon));
    e.jmpRel(g_nfsBase + 0x2851B5u);

    // 2. Hook nfs.exe+1AEB55 (6B)
    uint32_t st2 = e.curAddr();
    e.emitCallHandler(reinterpret_cast<void*>(&OnTuningDataCtor));
    const uint8_t orig2[6] = { 0x8B, 0xF1, 0x8B, 0x44, 0x24, 0x38 };
    e.raw(orig2, 6);
    e.jmpRel(g_nfsBase + 0x1AEB5Bu);

    // 3. Hook nfs.exe+1AEC76 (6B)
    uint32_t st3 = e.curAddr();
    e.emitCallHandler(reinterpret_cast<void*>(&OnTuningDataDtor));
    const uint8_t orig3[6] = { 0x8B, 0x8E, 0xDC, 0x00, 0x00, 0x00 };
    e.raw(orig3, 6);
    e.jmpRel(g_nfsBase + 0x1AEC7Cu);

    // 4. Hook nfs.exe+1F0863 (5B)
    uint32_t st4 = e.curAddr();
    e.callRel(g_nfsBase + 0x1A4D90u);
    e.emitCallHandler(reinterpret_cast<void*>(&OnUiSliderCtor));
    e.jmpRel(g_nfsBase + 0x1F0868u);

    // 5. Hook nfs.exe+31B680 (5B)
    uint32_t st5 = e.curAddr();
    e.b(0x9C);
    e.b(0x83); e.b(0x3D); e.u32(reinterpret_cast<uint32_t>(&g_monIsBot)); e.b(0x00);
    size_t j5_1 = e.jccPlaceholder(0x85);
    e.b(0x81); e.b(0xF9); e.u32(0x00010000u); size_t j5_2 = e.jccPlaceholder(0x82);
    e.b(0x81); e.b(0xF9); e.u32(0x7FFF0000u); size_t j5_3 = e.jccPlaceholder(0x87);
    e.b(0x52); e.b(0x50);
    e.b(0x8B); e.b(0x54); e.b(0x24); e.b(0x14);
    e.b(0x81); e.b(0xFA); e.u32(0x00010000u); size_t j5_4 = e.jccPlaceholder(0x82);
    e.b(0x81); e.b(0xFA); e.u32(0x7FFF0000u); size_t j5_5 = e.jccPlaceholder(0x87);
    e.b(0x8B); e.b(0x02);
    e.b(0x3D); e.u32(SLIDER_CLASS_STD); size_t j5_ok = e.jccPlaceholder(0x84);
    e.b(0x3D); e.u32(SLIDER_CLASS_CAM); size_t j5_6  = e.jccPlaceholder(0x85);
    e.patchJcc(j5_ok);
    e.b(0x60);
    e.b(0xFF); e.b(0x74); e.b(0x24); e.b(0x2C);
    e.b(0x51);
    e.callRel(reinterpret_cast<uint32_t>(&OnRbTreeSlider));
    e.b(0x83); e.b(0xC4); e.b(0x08);
    e.b(0x61);
    e.patchJcc(j5_4); e.patchJcc(j5_5); e.patchJcc(j5_6);
    e.b(0x58); e.b(0x5A);
    e.patchJcc(j5_1); e.patchJcc(j5_2); e.patchJcc(j5_3);
    e.b(0x9D);
    const uint8_t orig5[5] = { 0x8B, 0x41, 0x0C, 0x85, 0xC0 };
    e.raw(orig5, 5);
    e.jmpRel(g_nfsBase + 0x31B685u);

    // 6. Hook nfs.exe+38EBAD (9B: mov ebx, ecx; mov ebp, [esp+0x13C]) — direct Blueprint::BuildCarSpec!
    uint32_t st6 = e.curAddr();
    e.emitCallHandler(reinterpret_cast<void*>(&OnBuildCarSpec));
    const uint8_t orig6[9] = { 0x8B, 0xD9, 0x8B, 0xAC, 0x24, 0x3C, 0x01, 0x00, 0x00 };
    e.raw(orig6, 9);
    e.jmpRel(g_nfsBase + 0x38EBB6u);

    // 7. Hook nfs.exe+38A672 (8B: mov esi, [esp+68]; mov ebp, [esi]; mov ebx, ecx) — FECustomizationRecord::WriteToBlueprint!
    uint32_t st7 = e.curAddr();
    e.emitCallHandler(reinterpret_cast<void*>(&OnWriteToBlueprint));
    const uint8_t orig7[8] = { 0x8B, 0x74, 0x24, 0x68, 0x8B, 0x2E, 0x8B, 0xD9 };
    e.raw(orig7, 8);
    e.jmpRel(g_nfsBase + 0x38A67Au);

    // 8. Hook nfs.exe+32E8DD (7B: pop esi; mov al, 1; pop edi; ret 4)
    uint32_t st8 = e.curAddr();
    e.emitCallHandler(reinterpret_cast<void*>(&OnGarageMain));
    const uint8_t orig8[7] = { 0x5E, 0xB0, 0x01, 0x5F, 0xC2, 0x04, 0x00 };
    e.raw(orig8, 7);

    // 9. Hook nfs.exe+1F05D6 (6B: mov edx, [ebx+20]; mov eax, [edx+08]) — 0ms Tuning slider sync!
    uint32_t st9 = e.curAddr();
    e.emitCallHandler(reinterpret_cast<void*>(&OnTuningScreenSync));
    const uint8_t orig9[6] = { 0x8B, 0x53, 0x20, 0x8B, 0x42, 0x08 };
    e.raw(orig9, 6);
    e.jmpRel(g_nfsBase + 0x1F05DCu);

    // 10. Hook nfs.exe+38643D (6B: mov ebp, ecx; mov [esp+24], ebp) — FECustomizationRecord::SyncFromBlueprint!
    uint32_t st10 = e.curAddr();
    e.emitCallHandler(reinterpret_cast<void*>(&OnSyncFromBlueprint));
    const uint8_t orig10[6] = { 0x8B, 0xE9, 0x89, 0x6C, 0x24, 0x24 };
    e.raw(orig10, 6);
    e.jmpRel(g_nfsBase + 0x386443u);

    // 11. Hook nfs.exe+175F70 (5B: call 00402380 in Garage Dyno) — captures exact pvehArg!
    uint32_t st11 = e.curAddr();
    e.emitCallHandler(reinterpret_cast<void*>(&OnAttribInstCtor));
    e.callRel(g_nfsBase + 0x2380u);
    e.jmpRel(g_nfsBase + 0x175F75u);

    // 12. Hook nfs.exe+1A4BE1 (5B: call 00402380 in TuningData) — captures exact pvehArg!
    uint32_t st12 = e.curAddr();
    e.emitCallHandler(reinterpret_cast<void*>(&OnAttribInstCtor));
    e.callRel(g_nfsBase + 0x2380u);
    e.jmpRel(g_nfsBase + 0x1A4BE6u);

    // 13. Hook nfs.exe+133D50 (5B: push esi; mov esi, [esp+0C]) — FECarCareerEntry::BuildBlueprint!
    uint32_t st13 = e.curAddr();
    e.emitCallHandler(reinterpret_cast<void*>(&OnCareerBuildBp));
    const uint8_t orig13[5] = { 0x56, 0x8B, 0x74, 0x24, 0x0C };
    e.raw(orig13, 5);
    e.jmpRel(g_nfsBase + 0x133D55u);

    bool ok = true;
    ok = ok && InstallPatchJump(g_hooks[0],  g_nfsBase + 0x2851B0u, st1,  5);
    ok = ok && InstallPatchJump(g_hooks[1],  g_nfsBase + 0x1AEB55u, st2,  6);
    ok = ok && InstallPatchJump(g_hooks[2],  g_nfsBase + 0x1AEC76u, st3,  6);
    ok = ok && InstallPatchJump(g_hooks[3],  g_nfsBase + 0x1F0863u, st4,  5);
    ok = ok && InstallPatchJump(g_hooks[4],  g_nfsBase + 0x31B680u, st5,  5);
    ok = ok && InstallPatchJump(g_hooks[5],  g_nfsBase + 0x38EBADu, st6,  9);
    ok = ok && InstallPatchJump(g_hooks[6],  g_nfsBase + 0x38A672u, st7,  8);
    ok = ok && InstallPatchJump(g_hooks[7],  g_nfsBase + 0x32E8DDu, st8,  7);
    ok = ok && InstallPatchJump(g_hooks[8],  g_nfsBase + 0x1F05D6u, st9,  6);
    ok = ok && InstallPatchJump(g_hooks[9],  g_nfsBase + 0x38643Du, st10, 6);
    ok = ok && InstallPatchJump(g_hooks[10], g_nfsBase + 0x175F70u, st11, 5);
    ok = ok && InstallPatchJump(g_hooks[11], g_nfsBase + 0x1A4BE1u, st12, 5);
    ok = ok && InstallPatchJump(g_hooks[12], g_nfsBase + 0x133D50u, st13, 5);
    if (!ok) RestoreAllHooks();
    return ok;
}

// ============================================================================
// In-Process VLT DatabasePrivate Engine + Blueprint Heap Scanner
// ============================================================================
struct VltClassInfo { uint32_t ptr = 0, hash = 0, nColl = 0, hTab = 0, tableSz = 0, laySz = 0; };

struct VltCarInfo {
    uint32_t carKeyU32 = 0, pvKeyU32 = 0;
    std::string canonHash, rawName;
    uint32_t slotPtr[NUM_INSPECT_SLOTS] = {};
    uint32_t slotMCnt[NUM_INSPECT_SLOTS] = {};
    uint32_t rearBrakePtr = 0, rearBrakeMCnt = 0;
    std::unordered_set<uint32_t> cpPartSet, allPartSet;
};

struct BpRecord {
    uint32_t addr = 0, carType = 0;
    std::string identifiedCarHash;
    bool     playerOwned = false, seenAsBot = false, directRef = false;
    uint32_t lastRingSeq = 0;
    uint64_t lastSigHash = 0;
    uint32_t lastChangedTick = 0;
    int      cachedUpgCnt = 0;
    int      cachedNzCnt = 0;
};

struct SliderBound { float minB = -1.0f, maxB = 1.0f; bool locked = false; };

struct WorkerState {
    bool vltReady = false;
    uint32_t carPartClassPtr = 0, minPartPtr = 0xFFFFFFFFu, maxPartPtr = 0;
    std::unordered_map<uint32_t, VltClassInfo> vltClasses;
    std::unordered_map<uint32_t, uint32_t> infoCollByKey, vltCarByKey, vltCpByKey, vltPartByKey;
    std::unordered_map<uint32_t, std::unordered_set<uint32_t>> vltPkgByCar;
    std::unordered_set<uint32_t> allPartPtrs;
    std::unordered_map<uint32_t, uint32_t> partCarRefCount;
    std::unordered_map<std::string, VltCarInfo> vltCars;

    std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> physArrCache;
    std::unordered_map<uint32_t, uint32_t> info3EFArrCache;
    std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> infoCollMetaCache;

    std::unordered_map<std::string, uint32_t> knownTypeByHash;
    std::unordered_map<uint32_t, std::string> knownHashByType;

    struct CustRecInfo {
        uint32_t addr = 0;
        uint32_t carType0 = 0; // [feCust + 0x7B0] == [bp + 0x00]
        uint32_t lastBp = 0;
        uint32_t lastSeq = 0;
    };
    std::unordered_map<uint32_t, BpRecord> bpPool;
    std::unordered_map<uint32_t, CustRecInfo> custRecPool;
    std::unordered_map<uint32_t, uint32_t> pvehArgByCtHash;
    uint32_t lastRingSeqRead = 0, lastHeapScanTick = 0, lastStaticTabTick = 0, lastCustScanTick = 0;
    uint32_t lastCustScanCtHash = 0;
    uint32_t activeBpAddr = 0, pinnedBpAddr = 0, activeCustRecAddr = 0;

    std::string activeBaseHash, activeCanonHash, activeVarHash;
    uint32_t activeMode = 0, tuningMapSeqAtCarSwitch = 0, lastCatalogKeyHash = 0;

    std::unordered_map<std::string, std::unordered_map<uint32_t, SliderBound>> boundsCacheByKey;
    std::unordered_map<uint32_t, SliderBound> curBounds;
    bool hasLiveBounds = false;
    uint32_t lastBoundsTdPtr = 0;
    uint32_t boundsTdStartTick = 0;
    uint32_t lastBoundsPollTick = 0;
    std::string curPartsSig;
    // v2 performance caches & throttles
    uint32_t lastPtrProbeTick = 0;
    uint32_t lastSigPollTick = 0;
    uint32_t lastSculptPollTick = 0;
    uint32_t lastActiveActivityTick = 0;
    uint32_t lastPublishTick = 0;
    uint32_t lastCtxModeSeen = 0xFFFFFFFFu;
    bool     lastInTuningUiSeen = false;
    uint64_t lastUpgKeyHash = 0;
    std::string cachedUpgShort[NUM_UPGRADE_PARTS];
    uint64_t lastFastPubHash = 0;
    float    lastSlidersSnap[NUM_SLIDERS] = {};
    float    cachedSculptZones[AUTOSCULPT_READ_SLOTS][AUTOSCULPT_ZONES] = {};
};
static WorkerState g_ws;

static bool GetPartPhysicsArrayInfo(uint32_t collPtr, uint32_t& outArr, uint32_t& outCnt) {
    outArr = 0; outCnt = 0;
    if (!IsValidPtr(collPtr)) return false;
    auto it = g_ws.physArrCache.find(collPtr);
    if (it != g_ws.physArrCache.end()) {
        outArr = it->second.first; outCnt = it->second.second;
        return outArr != 0;
    }
    uint32_t cur = collPtr;
    for (int depth = 0; depth < 3 && IsValidPtr(cur); ++depth) {
        uint32_t mTab = SafeReadU32(cur + 0x00);
        uint32_t nEnt = SafeReadU32(cur + 0x04) & 0xFFFFu;
        if (IsValidPtr(mTab) && nEnt > 0 && nEnt <= 24) {
            for (uint32_t a = 0; a < nEnt; ++a) {
                if (SafeReadU32(mTab + a * 12) == 0x6B868DDFu) {
                    uint32_t arr = SafeReadU32(mTab + a * 12 + 4);
                    if (IsValidPtr(arr)) {
                        uint32_t cnt = SafeReadU32(arr) & 0xFFFFu;
                        g_ws.physArrCache[collPtr] = { arr, cnt };
                        outArr = arr; outCnt = cnt;
                        return true;
                    }
                }
            }
        }
        cur = SafeReadU32(cur + 0x0C);
    }
    g_ws.physArrCache[collPtr] = { 0, 0 };
    return false;
}

static void CollectVltClass(const VltClassInfo& cl, std::vector<std::pair<uint32_t, uint32_t>>& outList, std::unordered_map<uint32_t, uint32_t>& outByKey) {
    outList.clear(); outByKey.clear();
    if (!IsValidPtr(cl.hTab)) return;
    uint32_t cap = cl.tableSz;
    if (cap == 0 || cap > 8192) cap = std::min(std::max(cl.nColl * 4u + 64u, 512u), 4096u);
    std::vector<uint32_t> raw(cap * 3);
    if (!SafeRead(cl.hTab, raw.data(), cap * 12)) return;
    for (uint32_t b = 0; b < cap; ++b) {
        uint32_t k = raw[b * 3 + 0], p = raw[b * 3 + 1];
        if (k != 0 && IsValidPtr(p) && outByKey.find(k) == outByKey.end() && SafeReadU32(p + 0x14) == cl.ptr) {
            outByKey[k] = p;
            outList.push_back({ k, p });
        }
    }
}

static int ClassifyPartCategory(uint32_t partPtr) {
    if (!IsValidPtr(partPtr)) return -1;
    uint32_t pl = SafeReadU32(partPtr + 0x18);
    if (!IsValidPtr(pl)) return -1;
    std::string sC  = TryReadAsciiSafe(SafeReadU32(pl + 0x0C));
    std::string s10 = TryReadAsciiSafe(SafeReadU32(pl + 0x10));
    if (sC == "ENGINE_PACKAGE" || s10 == "ENGINE_PACKAGE")                     return 0;
    if (sC == "DRIVETRAIN_PACKAGE" || s10 == "DRIVETRAIN_PACKAGE")             return 1;
    if (sC == "FORCED_INDUCTION_PACKAGE" || s10 == "FORCED_INDUCTION_PACKAGE") return 2;
    if (sC == "SUSPENSION_PACKAGE" || s10 == "SUSPENSION_PACKAGE")             return 3;
    if (sC == "BRAKE" || s10 == "BRAKE_FRONT" || s10 == "BRAKE_REAR")          return (s10 == "BRAKE_REAR") ? 10 : 4;
    if (sC == "TIRE_PACKAGE" || s10 == "TIRE_PACKAGE")                         return 5;
    if (sC == "NITROUS_PACKAGE" || s10 == "NITROUS_PACKAGE")                   return 6;
    if (sC == "BODY" || s10 == "BODY" || s10 == "WIDEBODY")                    return (s10 == "WIDEBODY") ? 11 : 7;
    if (sC == "HOOD" || s10 == "HOOD")                                         return 8;
    if (sC == "SPOILER" || s10 == "SPOILER")                                   return 9;
    return -1;
}

static bool EnsureVltIndexed(uint32_t seedPvehColl) {
    if (g_ws.vltReady) return true;
    uint32_t seedCls = 0;
    if (IsValidPtr(seedPvehColl)) {
        uint32_t cl = SafeReadU32(seedPvehColl + 0x14);
        if (IsValidPtr(cl) && SafeReadU32(cl + 0x04) == cl) seedCls = cl;
    }
    // Фолбэк при мгновенном старте (до смены авто): берём Attrib::Class* из любого Blueprint в статической таблице!
    if (!IsValidPtr(seedCls)) {
        for (const auto& kv : g_ws.bpPool) {
            uint32_t p0 = SafeReadU32(kv.first + 0x6C);
            if (IsValidPtr(p0)) {
                uint32_t cl = SafeReadU32(p0 + 0x14);
                if (IsValidPtr(cl) && SafeReadU32(cl + 0x04) == cl) { seedCls = cl; break; }
            }
        }
    }
    if (!IsValidPtr(seedCls)) return false;
    uint32_t dbPtr = SafeReadU32(seedCls + 0x28);
    if (!IsValidPtr(dbPtr)) return false;

    uint32_t basePage  = seedCls & 0xFFFF0000u;
    uint32_t scanStart = (basePage > 0x00200000u) ? (basePage - 0x00100000u) : 0x00100000u;
    uint32_t scanEnd   = basePage + 0x00100000u;
    std::vector<uint8_t> chunk(0x1000);

    for (uint32_t ca = scanStart; ca < scanEnd; ca += 0x1000) {
        if (!SafeRead(ca, chunk.data(), 0x1000)) continue;
        for (uint32_t off = 0; off <= 0x1000 - 0x2C; off += 0x10) {
            uint32_t selfA = ca + off;
            uint32_t w04 = *reinterpret_cast<const uint32_t*>(chunk.data() + off + 0x04);
            uint32_t w28 = *reinterpret_cast<const uint32_t*>(chunk.data() + off + 0x28);
            if (w04 == selfA && w28 == dbPtr) {
                VltClassInfo ci;
                ci.ptr     = selfA;
                ci.hash    = *reinterpret_cast<const uint32_t*>(chunk.data() + off + 0x00);
                ci.nColl   = *reinterpret_cast<const uint32_t*>(chunk.data() + off + 0x10) & 0xFFFFu;
                ci.hTab    = *reinterpret_cast<const uint32_t*>(chunk.data() + off + 0x14);
                ci.tableSz = *reinterpret_cast<const uint32_t*>(chunk.data() + off + 0x18) & 0xFFFFu;
                ci.laySz   = *reinterpret_cast<const uint32_t*>(chunk.data() + off + 0x20) & 0xFFFFu;
                g_ws.vltClasses[ci.hash] = ci;
            }
        }
    }

    auto itCar  = g_ws.vltClasses.find(0xA13753EBu);
    auto itCp   = g_ws.vltClasses.find(0xCB83AEF7u);
    auto itPart = g_ws.vltClasses.find(0x201CBC7Eu);
    if (itCar == g_ws.vltClasses.end() || itCp == g_ws.vltClasses.end() || itPart == g_ws.vltClasses.end()) return false;

    g_ws.carPartClassPtr = itPart->second.ptr;

    std::vector<std::pair<uint32_t, uint32_t>> tmpList, carList, cpList, partList, pkgList;
    auto itInfo = g_ws.vltClasses.find(0x158D43BFu);
    if (itInfo != g_ws.vltClasses.end()) CollectVltClass(itInfo->second, tmpList, g_ws.infoCollByKey);
    CollectVltClass(itCar->second, carList, g_ws.vltCarByKey);
    CollectVltClass(itCp->second, cpList, g_ws.vltCpByKey);
    CollectVltClass(itPart->second, partList, g_ws.vltPartByKey);

    g_ws.allPartPtrs.clear();
    g_ws.minPartPtr = 0xFFFFFFFFu;
    g_ws.maxPartPtr = 0;
    for (const auto& kv : partList) {
        uint32_t p = kv.second;
        g_ws.allPartPtrs.insert(p);
        if (p < g_ws.minPartPtr) g_ws.minPartPtr = p;
        if (p > g_ws.maxPartPtr) g_ws.maxPartPtr = p;
    }

    auto itPkg = g_ws.vltClasses.find(0xEC2B167Au);
    if (itPkg != g_ws.vltClasses.end()) {
        std::unordered_map<uint32_t, uint32_t> pkgMap;
        CollectVltClass(itPkg->second, pkgList, pkgMap);
        for (const auto& pk : pkgList) {
            uint32_t mTab = SafeReadU32(pk.second + 0x00);
            uint32_t nEnt = SafeReadU32(pk.second + 0x04) & 0xFFFFu;
            uint32_t carsArr = 0, partsArr = 0;
            if (IsValidPtr(mTab) && nEnt > 0 && nEnt <= 16) {
                for (uint32_t a = 0; a < nEnt; ++a) {
                    uint32_t ak = SafeReadU32(mTab + a * 12), av = SafeReadU32(mTab + a * 12 + 4);
                    if (ak == 0xE3DF4BFDu) carsArr = av;
                    else if (ak == 0xB30E8B4Bu) partsArr = av;
                }
            }
            if (IsValidPtr(carsArr) && IsValidPtr(partsArr)) {
                uint32_t cCnt = SafeReadU32(carsArr) & 0xFFFFu;
                uint32_t pCnt = SafeReadU32(partsArr) & 0xFFFFu;
                uint32_t pStr = SafeReadU32(partsArr + 4) & 0xFFFFu;
                if (cCnt > 0 && cCnt <= 128 && pCnt > 0 && pCnt <= 32 && pStr >= 12 && pStr <= 32) {
                    std::vector<uint32_t> pKeys;
                    for (uint32_t j = 0; j < pCnt; ++j) {
                        uint32_t ea = partsArr + 8 + j * pStr;
                        if (SafeReadU32(ea) == 0x201CBC7Eu) pKeys.push_back(SafeReadU32(ea + 4));
                    }
                    if (!pKeys.empty()) {
                        for (uint32_t i = 0; i < cCnt; ++i) {
                            uint32_t ck = SafeReadU32(carsArr + 8 + i * 12 + 4);
                            if (ck != 0) {
                                auto& dst = g_ws.vltPkgByCar[ck];
                                for (uint32_t pkKey : pKeys) dst.insert(pkKey);
                            }
                        }
                    }
                }
            }
        }
    }

    uint32_t cpLaySz = (itCp->second.laySz > 0) ? itCp->second.laySz : 0x558u;
    uint32_t nEntries = cpLaySz / 12u;

    for (const auto& ck : carList) {
        uint32_t cKey = ck.first, carPtr = ck.second;
        uint32_t cLay = SafeReadU32(carPtr + 0x18);
        if (!IsValidPtr(cLay)) continue;

        VltCarInfo info;
        info.carKeyU32 = cKey;
        info.pvKeyU32  = SafeReadU32(cLay + 0x04);
        info.canonHash = ProStreetMeta::canonicalCarHash(ProStreetMeta::toHex8(cKey));
        info.rawName   = ProStreetMeta::normalizeCarName(TryReadAsciiSafe(SafeReadU32(cLay + 0x34)));

        uint32_t cpKey = SafeReadU32(cLay + 0x10);
        uint32_t cpPtr = SafeReadU32(cLay + 0x14);
        if (!IsValidPtr(cpPtr)) {
            auto it1 = g_ws.vltCpByKey.find(cpKey);
            if (it1 != g_ws.vltCpByKey.end()) cpPtr = it1->second;
            else {
                auto it2 = g_ws.vltCpByKey.find(cKey);
                if (it2 != g_ws.vltCpByKey.end()) cpPtr = it2->second;
            }
        }

        auto considerPart = [&](uint32_t partPtr, bool fromCarParts) {
            if (!IsValidPtr(partPtr)) return;
            info.allPartSet.insert(partPtr);
            if (fromCarParts) info.cpPartSet.insert(partPtr);
            int cat = ClassifyPartCategory(partPtr);
            if (cat < 0 || cat == 11) return;
            uint32_t arr = 0, mCnt = 0;
            if (!GetPartPhysicsArrayInfo(partPtr, arr, mCnt) || mCnt == 0 || mCnt > 32) return;
            if (cat == 10) {
                if (mCnt > info.rearBrakeMCnt) { info.rearBrakePtr = partPtr; info.rearBrakeMCnt = mCnt; }
                return;
            }
            if (mCnt > info.slotMCnt[cat]) { info.slotPtr[cat] = partPtr; info.slotMCnt[cat] = mCnt; }
        };

        if (IsValidPtr(cpPtr)) {
            uint32_t cpLay = SafeReadU32(cpPtr + 0x18);
            if (IsValidPtr(cpLay)) {
                for (uint32_t e = 0; e < nEntries; ++e) {
                    uint32_t refArr = SafeReadU32(cpLay + e * 12 + 0);
                    uint32_t refCnt = SafeReadU32(cpLay + e * 12 + 4);
                    if (IsValidPtr(refArr) && refCnt > 0 && refCnt <= 64) {
                        for (uint32_t j = 0; j < refCnt; ++j) {
                            if (SafeReadU32(refArr + j * 12 + 0) == 0x201CBC7Eu) {
                                uint32_t rKey = SafeReadU32(refArr + j * 12 + 4);
                                uint32_t rPtr = SafeReadU32(refArr + j * 12 + 8);
                                if (!IsValidPtr(rPtr)) {
                                    auto itp = g_ws.vltPartByKey.find(rKey);
                                    if (itp != g_ws.vltPartByKey.end()) rPtr = itp->second;
                                }
                                considerPart(rPtr, true);
                            }
                        }
                    }
                }
            }
        }

        auto addPkgSet = [&](uint32_t key) {
            auto it = g_ws.vltPkgByCar.find(key);
            if (it == g_ws.vltPkgByCar.end()) return;
            for (uint32_t pkKey : it->second) {
                auto itp = g_ws.vltPartByKey.find(pkKey);
                if (itp != g_ws.vltPartByKey.end()) considerPart(itp->second, false);
            }
        };
        addPkgSet(cKey);
        if (info.pvKeyU32 != cKey) addPkgSet(info.pvKeyU32);

        for (uint32_t p : info.cpPartSet) g_ws.partCarRefCount[p]++;
        g_ws.vltCars[info.canonHash] = std::move(info);
    }

    g_ws.vltReady = !g_ws.vltCars.empty();
    return g_ws.vltReady;
}

static bool IsValidBlueprint(uint32_t bp) {
    if (!IsValidPtr(bp)) return false;
    uint32_t p0   = SafeReadU32(bp + 0x68 + 0 * 0x10 + 4);
    uint32_t p1   = SafeReadU32(bp + 0x68 + 1 * 0x10 + 4);
    uint32_t p2   = SafeReadU32(bp + 0x68 + 2 * 0x10 + 4);
    uint32_t p122 = SafeReadU32(bp + 0x68 + 122 * 0x10 + 4);
    uint32_t p123 = SafeReadU32(bp + 0x68 + 123 * 0x10 + 4);
    if (!IsValidPtr(p0) || !IsValidPtr(p1) || !IsValidPtr(p2) || !IsValidPtr(p122) || !IsValidPtr(p123)) return false;

    uint32_t cls0 = SafeReadU32(p0 + 0x14);
    if (!IsValidPtr(cls0)) return false;
    if (g_ws.carPartClassPtr != 0 && cls0 != g_ws.carPartClassPtr) return false;
    if (SafeReadU32(p1 + 0x14) != cls0 || SafeReadU32(p2 + 0x14) != cls0) return false;
    if (SafeReadU32(p122 + 0x14) != cls0 || SafeReadU32(p123 + 0x14) != cls0) return false;

    uint32_t padBuf[0x184 / 4];
    if (!SafeRead(bp + 0x2054, padBuf, sizeof(padBuf))) return false;
    if (padBuf[0] != 0 || padBuf[1] != 0) return false;
    if (padBuf[(0x208C - 0x2054) / 4] != 0 || padBuf[(0x2140 - 0x2054) / 4] != 0) return false;

    for (int i = 0; i <= 30; ++i) {
        size_t baseI = ((0x205C - 0x2054) + i * 12) / 4;
        if (padBuf[baseI + 1] != 0 || padBuf[baseI + 2] != 0) return false;
    }
    return true;
}

static uint32_t GetBpCarTypeId(uint32_t bp) {
    uint32_t w0 = SafeReadU32(bp + 0x68);
    if (w0 == 0 || w0 == 0xEEEEEE00u || w0 == 0xEEEEEEEEu) return 0;
    return (w0 >> 8) & 0xFFu;
}

static bool ReadSlidersFromBp(uint32_t bp, float outVals[NUM_SLIDERS], int& outNzCount) {
    outNzCount = 0;
    uint32_t raw[32 * 3];
    if (!SafeRead(bp + 0x205C, raw, sizeof(raw))) return false;
    for (int i = 0; i < NUM_SLIDERS; ++i) {
        float fv = 0.0f;
        std::memcpy(&fv, &raw[SLIDER_DEFS[i].bpIdx * 3], 4);
        if (!IsFiniteFloat(fv) || std::fabs(fv) > 1.05f) return false;
        outVals[i] = fv;
        if (std::fabs(fv) > 1e-4f) outNzCount++;
    }
    return true;
}

static uint64_t CalcBpQuickSig(uint32_t bp, int& outUpgCnt, int& outNzCnt) {
    uint64_t h = 1469598103934665603ULL;
    outUpgCnt = 0;
    for (int i = 0; i < NUM_UPGRADE_PARTS; ++i) {
        uint32_t w2 = SafeReadU32(bp + 0x68 + INSPECT_SLOT_DEFS[i].slot * 0x10 + 8);
        uint32_t idx = w2 & 0xFFFFu;
        if (idx > 0) outUpgCnt++;
        h = (h ^ idx) * 1099511628211ULL;
    }
    float vals[NUM_SLIDERS];
    if (ReadSlidersFromBp(bp, vals, outNzCnt)) {
        for (int i = 0; i < NUM_SLIDERS; ++i) {
            int t = static_cast<int>(std::lround(vals[i] * 100.0f));
            h = (h ^ static_cast<uint32_t>(t + 1000)) * 1099511628211ULL;
        }
    }
    return h;
}

static std::string IdentifyBpCarModel(uint32_t bp, uint32_t bpCarType) {
    if (bpCarType > 0) {
        auto it = g_ws.knownHashByType.find(bpCarType);
        if (it != g_ws.knownHashByType.end()) return it->second;
    }
    if (!g_ws.vltReady || g_ws.vltCars.empty()) return "";

    uint32_t slotsRaw[150 * 4];
    if (!SafeRead(bp + 0x68, slotsRaw, sizeof(slotsRaw))) return "";

    std::vector<uint32_t> bpParts;
    bpParts.reserve(64);
    for (int s = 0; s < 150; ++s) {
        uint32_t w1 = slotsRaw[s * 4 + 1];
        if (w1 >= g_ws.minPartPtr && w1 <= g_ws.maxPartPtr && g_ws.allPartPtrs.count(w1)) {
            bpParts.push_back(w1);
        }
    }
    if (bpParts.empty()) return "";

    std::string bestCar;
    int bestScore = 0, secondScore = 0;
    for (const auto& kv : g_ws.vltCars) {
        const VltCarInfo& ci = kv.second;
        int score = 0;
        for (uint32_t p : bpParts) {
            if (ci.cpPartSet.count(p)) {
                auto itc = g_ws.partCarRefCount.find(p);
                uint32_t rc = (itc != g_ws.partCarRefCount.end()) ? itc->second : 10;
                if (rc == 1)      score += 100;
                else if (rc <= 3) score += 20;
                else              score += 5;
            } else if (ci.allPartSet.count(p)) {
                score += 1;
            }
        }
        if (score > bestScore) { secondScore = bestScore; bestScore = score; bestCar = kv.first; }
        else if (score > secondScore) { secondScore = score; }
    }

    if (!bestCar.empty() && bestScore >= 30 && bestScore > secondScore) {
        if (bpCarType > 0) {
            g_ws.knownHashByType[bpCarType] = bestCar;
            g_ws.knownTypeByHash[bestCar]   = bpCarType;
        }
        return bestCar;
    }
    return "";
}

static bool RegisterBp(uint32_t bp, int botTag, uint32_t ringSeq, bool directRef) {
    if (!IsValidBlueprint(bp)) return false;
    uint32_t bt = GetBpCarTypeId(bp);
    auto& rec = g_ws.bpPool[bp];
    bool isNew = (rec.addr == 0);
    rec.addr = bp;
    rec.carType = bt;
    if (rec.identifiedCarHash.empty()) rec.identifiedCarHash = IdentifyBpCarModel(bp, bt);
    if (directRef) { rec.directRef = true; rec.playerOwned = true; rec.seenAsBot = false; }
    else if (botTag == 0) { rec.playerOwned = true; rec.seenAsBot = false; }
    else if (botTag == 1 && !rec.playerOwned) { rec.seenAsBot = true; }
    if (ringSeq > rec.lastRingSeq) rec.lastRingSeq = ringSeq;

    int u = 0, nz = 0;
    uint64_t sig = CalcBpQuickSig(bp, u, nz);
    if (!isNew && rec.lastSigHash != 0 && rec.lastSigHash != sig) rec.lastChangedTick = GetTickCount();
    rec.lastSigHash = sig;
    rec.cachedUpgCnt = u;
    rec.cachedNzCnt = nz;
    return true;
}

static int ScanHeapForBlueprints(bool force) {
    if (!g_ws.vltReady || g_ws.allPartPtrs.empty()) return 0;
    uint32_t now = GetTickCount();
    if (!force && (now - g_ws.lastHeapScanTick) < 2000u) return 0;
    g_ws.lastHeapScanTick = now;

    int found = 0;
    bool isRd = (g_raceDayActive != 0);
    std::vector<uint8_t> buf(65536 + 64);

    uint32_t addr = 0x00400000u;
    MEMORY_BASIC_INFORMATION mbi = {};
    while (addr < 0x7FFF0000u && !g_stopWorker.load()) {
        if (!VirtualQuery(reinterpret_cast<LPCVOID>(addr), &mbi, sizeof(mbi))) { addr += 0x10000u; continue; }
        uint32_t regBase = reinterpret_cast<uint32_t>(mbi.BaseAddress);
        uint32_t regSize = static_cast<uint32_t>(mbi.RegionSize);
        uint32_t nextAddr = (regBase + regSize > addr) ? (regBase + regSize) : (addr + 0x10000u);

        bool rw = (mbi.State == MEM_COMMIT) &&
                  ((mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)) != 0) &&
                  ((mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) == 0) &&
                  (regSize >= 0x2200u) && (regSize <= 128u * 1024u * 1024u);
        if (rw) {
            for (uint32_t chunkOff = 0; chunkOff < regSize; chunkOff += 65536u) {
                uint32_t chunkAddr = regBase + chunkOff;
                uint32_t toRead = std::min(65536u + 48u, regSize - chunkOff);
                if (toRead < 0x30u) break;
                SIZE_T br = 0;
                if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(chunkAddr), buf.data(), toRead, &br) || br < 0x30u) continue;
                size_t limit = (br > 0x24u) ? (br - 0x24u) : 0;
                for (size_t off = 0; off < limit; off += 4) {
                    uint32_t w0 = *reinterpret_cast<const uint32_t*>(buf.data() + off);
                    if (w0 < g_ws.minPartPtr || w0 > g_ws.maxPartPtr || (w0 & 3u) != 0) continue;
                    uint32_t w1 = *reinterpret_cast<const uint32_t*>(buf.data() + off + 0x10);
                    if (w1 < g_ws.minPartPtr || w1 > g_ws.maxPartPtr || (w1 & 3u) != 0) continue;
                    uint32_t w2 = *reinterpret_cast<const uint32_t*>(buf.data() + off + 0x20);
                    if (w2 < g_ws.minPartPtr || w2 > g_ws.maxPartPtr || (w2 & 3u) != 0) continue;
                    if (!g_ws.allPartPtrs.count(w0) || !g_ws.allPartPtrs.count(w1) || !g_ws.allPartPtrs.count(w2)) continue;
                    uint32_t hitAddr = chunkAddr + static_cast<uint32_t>(off);
                    if (hitAddr < 0x0040006Cu) continue;
                    uint32_t candBp = hitAddr - 0x6Cu;
                    if (g_ws.bpPool.find(candBp) != g_ws.bpPool.end()) continue;
                    if (IsValidBlueprint(candBp)) {
                        RegisterBp(candBp, isRd ? -1 : 0, 0, false);
                        found++;
                    }
                }
            }
        }
        addr = nextAddr;
    }
    return found;
}

static bool IsLiveCustRec(uint32_t cr, uint32_t expectedCarType0) {
    if (!IsValidPtr(cr)) return false;
    uint32_t ct = SafeReadU32(cr + 0x7B0);
    if (ct == 0 || ct == 0xFEEEFEEEu || ct == 0xDDDDDDDDu || ct == 0xAAAAAAAAu || ct == 0xABADCAFEu) return false;
    if (expectedCarType0 != 0 && ct != expectedCarType0) return false;
    // Check that first few part slots aren't freed heap poison
    uint32_t w34 = SafeReadU32(cr + 0x34);
    if (w34 == 0xFEEEFEEEu || w34 == 0xDDDDDDDDu || w34 == 0xAAAAAAAAu) return false;
    return true;
}

static void RegisterCustRec(uint32_t cr, uint32_t bp, uint32_t seq) {
    if (!IsValidPtr(cr)) return;
    uint32_t ct = SafeReadU32(cr + 0x7B0);
    if (ct == 0 && IsValidPtr(bp)) ct = SafeReadU32(bp);
    if (!IsLiveCustRec(cr, ct)) return;
    auto& info = g_ws.custRecPool[cr];
    info.addr = cr;
    info.carType0 = ct;
    if (IsValidPtr(bp)) info.lastBp = bp;
    if (seq > info.lastSeq) info.lastSeq = seq;
}

static void IngestStaticBpTable() {
    // 1. Прямой встроенный Blueprint экрана Тюнинга: [nfs.exe+6B28A4] + 0xE0 (0 мс!)
    uint32_t ts = SafeReadU32(g_nfsBase + 0x6B28A4u);
    if (IsValidPtr(ts)) {
        g_tuningScreenPtr = ts;
        uint32_t td = SafeReadU32(ts + 0xDC);
        if (IsValidPtr(td)) { g_tuningDataPtr = td; g_inTuningUi = 1; }
        uint32_t embBp = ts + 0xE0u;
        if (IsValidBlueprint(embBp)) {
            RegisterBp(embBp, 0, g_bpRingSeq + 1, true);
        }
    }
    // 2. Статическая таблица активных RideInfo (Blueprint*) в nfs.exe+6EAA48..+6EAA94 (0 мс!)
    uint32_t base = g_nfsBase + 0x6EAA48u;
    uint32_t ptrs[20] = {};
    if (!SafeRead(base, ptrs, sizeof(ptrs))) return;
    bool isRd = (g_raceDayActive != 0);
    for (int i = 0; i < 20; ++i) {
        if (IsValidPtr(ptrs[i]) && IsValidBlueprint(ptrs[i])) {
            RegisterBp(ptrs[i], isRd ? -1 : 0, 0, (i <= 3 && !isRd));
        }
    }
}

static void IngestBpRing() {
    uint32_t now = GetTickCount();
    if ((now - g_ws.lastStaticTabTick) >= 120u || g_ws.bpPool.empty()) {
        g_ws.lastStaticTabTick = now;
        IngestStaticBpTable();
    }

    uint32_t dBp = g_directBuildBp;
    if (IsValidPtr(dBp) && IsValidBlueprint(dBp)) {
        RegisterBp(dBp, 0, g_bpRingSeq + 1, true);
    }
    uint32_t dCr = g_directCustRec, dCrBp = g_directCustBp;
    if (IsValidPtr(dCr)) {
        RegisterCustRec(dCr, dCrBp, g_directCustSeq);
    }

    uint32_t curSeq = g_bpRingSeq;
    if (curSeq == g_ws.lastRingSeqRead && !g_ws.bpPool.empty()) return;
    g_ws.lastRingSeqRead = curSeq;
    for (int i = 0; i < BP_RING_CAP; ++i) {
        uint32_t bp = g_bpRing[i].bp, cr = g_bpRing[i].custRec, seq = g_bpRing[i].seq, bot = g_bpRing[i].isBot;
        if (IsValidPtr(bp) && seq > 0) {
            RegisterBp(bp, bot ? 1 : 0, seq, (bot == 0));
            if (bot == 0 && IsValidPtr(cr)) RegisterCustRec(cr, bp, seq);
        }
    }
}


// ============================================================================
// Main-Thread Safe Execution (WH_GETMESSAGE) for 0077F2E0 & Live Dyno Recalc
// ============================================================================
static BOOL CALLBACK FindGameWindowProc(HWND hwnd, LPARAM lParam) {
    DWORD pid = 0;
    DWORD tid = GetWindowThreadProcessId(hwnd, &pid);
    if (pid == GetCurrentProcessId() && IsWindowVisible(hwnd) && GetWindow(hwnd, GW_OWNER) == nullptr) {
        RECT r{};
        if (GetClientRect(hwnd, &r) && (r.right - r.left) >= 320 && (r.bottom - r.top) >= 240) {
            g_gameHwnd = hwnd;
            g_mainTid  = tid;
            *reinterpret_cast<bool*>(lParam) = true;
            return FALSE;
        }
    }
    return TRUE;
}

static void ExecuteMainThreadJob() {
    if (g_mtJob.type == MainThreadJob::JOB_FIND_PACKED16) {
        g_mtJob.found = false;
        if (!g_callLookupPart || g_mtJob.carType0 == 0) return;

        auto tryABC = [&](uint32_t a, uint32_t b, uint32_t c, bool requireExactW1) -> bool {
            uint32_t tmp[4] = { 0, 0, 0, 0 };
            if (g_callLookupPart(reinterpret_cast<uint32_t>(tmp), g_mtJob.carType0, a, b, c, g_mtJob.feSlot) != 0) {
                if ((tmp[0] & 0xFFu) != 0 && (tmp[2] & 0xFFFFu) == g_mtJob.targetIdx) {
                    if (!requireExactW1 || !IsValidPtr(g_mtJob.targetW1) || tmp[1] == g_mtJob.targetW1) {
                        g_mtJob.outPacked = static_cast<uint16_t>(((a & 0xFFu) << 8) | ((b & 0xFu) << 4) | (c & 0xFu));
                        std::memcpy(g_mtJob.outDbPart, tmp, 16);
                        g_mtJob.found = true;
                        return true;
                    }
                }
            }
            return false;
        };

        uint32_t curA = (g_mtJob.curPacked != 0xFFFFu) ? ((g_mtJob.curPacked >> 8) & 0xFFu) : 0u;
        uint32_t curB = (g_mtJob.curPacked != 0xFFFFu) ? ((g_mtJob.curPacked >> 4) & 0xFu)  : 0u;
        uint32_t idx  = g_mtJob.targetIdx & 0xFu;

        for (int pass = 0; pass < 2; ++pass) {
            bool exactW1 = (pass == 0);
            // 1) Мгновенная проверка текущего 'a' (например 0x66 для двигателя) с новым c = targetIdx!
            if (g_mtJob.curPacked != 0xFFFFu) {
                if (tryABC(curA, curB, idx, exactW1)) return;
                if (curB != 0 && tryABC(curA, 0, idx, exactW1)) return;
                for (uint32_t b = 0; b < 16; ++b) if (tryABC(curA, b, idx, exactW1)) return;
                for (uint32_t c = 0; c < 16; ++c) if (tryABC(curA, idx, c, exactW1)) return;
            }
            // 2) Полный перебор 'a' = 0..255 с (a, 0, targetIdx) — находит любую деталь даже из пустого слота 0xFFFF!
            for (uint32_t a = 0; a < 256; ++a) {
                if (tryABC(a, 0, idx, exactW1)) return;
            }
            // 3) Запасной перебор 'a' = 0..255 с b = 1..3 (варианты вроде Widebody / Carbon)
            for (uint32_t b = 1; b <= 3; ++b) {
                for (uint32_t a = 0; a < 256; ++a) {
                    if (tryABC(a, b, idx, exactW1)) return;
                }
            }
        }
        return;
    }

    if (g_mtJob.type == MainThreadJob::JOB_RECALC_DYNO) {
        if (!g_callThis0 || !g_callThis1 || !g_callThis2) return;
        if (!IsValidPtr(g_mtJob.bpAddr) || g_mtJob.pvehArg == 0) return;

        alignas(16) uint8_t attribInst[0x40] = {};
        alignas(16) uint8_t ratingObj[0x200] = {};
        alignas(16) uint8_t carWrap[0x40]    = {};

        g_inSelfDynoRecalc = 1;
        g_callThis2(g_nfsBase + 0x2380u, attribInst, g_mtJob.pvehArg, 0u);
        if (*reinterpret_cast<uint32_t*>(attribInst) != 0) {
            uint32_t okB = g_callThis2(g_nfsBase + 0x38EB80u, reinterpret_cast<void*>(g_mtJob.bpAddr),
                                       reinterpret_cast<uint32_t>(attribInst), 1u);
            if ((okB & 0xFFu) != 0) {
                uint32_t modeVal = g_mtJob.mode;
                if (modeVal > 3u) modeVal = 0u;
                g_callThis1(g_nfsBase + 0x318C60u, ratingObj, reinterpret_cast<uint32_t>(&modeVal));
                uint32_t collPtr = g_callThis1(g_nfsBase + 0x2420u, attribInst, 0u);
                if (IsValidPtr(collPtr)) {
                    uint32_t wrapPtr = g_callThis2(g_nfsBase + 0x26F0u, carWrap, collPtr, 0u);
                    if (IsValidPtr(wrapPtr)) {
                        // Вызывает 0072E890 -> срабатывает наш OnGarageMain и мгновенно обновляет P1..P7, V и Дино!
                        g_callThis1(g_nfsBase + 0x32E890u, ratingObj, wrapPtr);
                        g_callThis0(g_nfsBase + 0x18E3B0u, carWrap);
                    }
                }
            }
            g_callThis0(g_nfsBase + 0x1301F0u, attribInst);
        }
        g_inSelfDynoRecalc = 0;
    }
}

static LRESULT CALLBACK MainThreadMsgHook(int code, WPARAM wParam, LPARAM lParam) {
    if (code >= 0 && g_mtPending.load(std::memory_order_acquire) != 0) {
        if (g_mtPending.exchange(0) != 0) {
            ExecuteMainThreadJob();
            if (g_hMtDoneEvt) SetEvent(g_hMtDoneEvt);
        }
    }
    return CallNextHookEx(g_hMsgHook, code, wParam, lParam);
}

static bool EnsureMainThreadHook() {
    if (g_hMsgHook && IsWindow(g_gameHwnd)) return true;
    bool found = false;
    EnumWindows(FindGameWindowProc, reinterpret_cast<LPARAM>(&found));
    if (!found || g_mainTid == 0) return false;
    if (!g_hMtDoneEvt) g_hMtDoneEvt = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    g_hMsgHook = SetWindowsHookExA(WH_GETMESSAGE, MainThreadMsgHook, g_hSelf, g_mainTid);
    return g_hMsgHook != nullptr;
}

static bool RunMainThreadJob(DWORD timeoutMs = 450u) {
    if (!EnsureMainThreadHook() || !g_hMtDoneEvt) return false;
    ResetEvent(g_hMtDoneEvt);
    g_mtPending.store(1, std::memory_order_release);
    PostMessageA(g_gameHwnd, WM_NULL, 0, 0);
    return WaitForSingleObject(g_hMtDoneEvt, timeoutMs) == WAIT_OBJECT_0;
}

static bool TriggerLiveDynoRecalc(uint32_t bpAddr) {
    if (!IsValidPtr(bpAddr) || !IsValidBlueprint(bpAddr)) return false;
    uint32_t ctHash = SafeReadU32(bpAddr);
    uint32_t pvehArg = 0;
    auto it = g_ws.pvehArgByCtHash.find(ctHash);
    if (it != g_ws.pvehArgByCtHash.end()) pvehArg = it->second;
    else if (g_lastPvehArg != 0 && (g_lastPvehArgBp == bpAddr || g_ws.activeBpAddr == bpAddr)) pvehArg = g_lastPvehArg;
    if (pvehArg == 0) return false;

    uint32_t modeV = SafeReadU32(bpAddr + 0x2050);
    if (modeV > 3u) modeV = g_ws.activeMode;

    g_mtJob = MainThreadJob{};
    g_mtJob.type    = MainThreadJob::JOB_RECALC_DYNO;
    g_mtJob.bpAddr  = bpAddr;
    g_mtJob.pvehArg = pvehArg;
    g_mtJob.mode    = modeV;
    return RunMainThreadJob(500u);
}

// ============================================================================
// Exact Career FECustomizationRecord Matcher & In-Process Scanner
// Находит ВСЕ постоянные записи карьеры (UserProfile + копия Гаража),
// строго соответствующие активному Blueprint (ctHash + mode + parts + sliders)!
// ============================================================================
static bool DoesCustRecMatchBp(uint32_t cr, uint32_t bp, uint32_t ctHash, uint32_t bpMode, const float bpSliders[NUM_SLIDERS], bool checkSliders) {
    if (!IsLiveCustRec(cr, ctHash)) return false;
    uint32_t crMode = SafeReadU32(cr + 0x228);
    if (crMode > 3u || (bpMode <= 3u && crMode != bpMode)) return false;

    // 1. Все 32 ползунка в [cr+0x730..0x7AF] должны быть валидными float в [-1.05..+1.05]
    float crSliders[32] = {};
    if (!SafeRead(cr + 0x730u, crSliders, sizeof(crSliders))) return false;
    for (int k = 0; k < 32; ++k) {
        if (!IsFiniteFloat(crSliders[k]) || std::fabs(crSliders[k]) > 1.05f) return false;
    }
    if (checkSliders && bpSliders) {
        for (int i = 0; i < NUM_SLIDERS; ++i) {
            if (std::fabs(crSliders[SLIDER_DEFS[i].bpIdx] - bpSliders[i]) > 0.03f) return false;
        }
    }

    // 2. Проверка 7 слотов прокачки: низшие 4 бита packed16 должны совпадать с w2 & 0xF в Blueprint!
    //    Отсеивает и чужие конфиги 2/3 той же машины, и ложные совпадения (RideInfo с мусором 4F40/174B)!
    int matchedNonZero = 0;
    for (int i = 0; i < 7; ++i) {
        uint32_t feSlot = INSPECT_SLOT_DEFS[i].feSlot;
        uint32_t bpW2   = SafeReadU32(bp + 0x58u + feSlot * 0x10u + 8u);
        uint32_t bpIdx  = bpW2 & 0xFFFFu;
        uint16_t p16    = 0xFFFFu;
        if (!SafeRead(cr + 0x34u + feSlot * 2u, &p16, 2)) return false;
        if (bpIdx > 0u) {
            if (p16 == 0xFFFFu || p16 == 0x0000u) return false;
            if ((p16 & 0xFu) != (bpIdx & 0xFu)) return false;
            if (((p16 >> 4) & 0xFu) > 3u) return false; // защита от мусора 4F40/174B!
            matchedNonZero++;
        } else {
            if (p16 != 0xFFFFu && p16 != 0x0000u) {
                if ((p16 & 0xFu) != 0u || ((p16 >> 4) & 0xFu) > 3u) return false;
            }
        }
    }
    (void)matchedNonZero;
    return true;
}

static int ScanHeapForCustRecs(uint32_t activeBp, bool force) {
    if (!IsValidPtr(activeBp) || !IsValidBlueprint(activeBp)) return 0;
    uint32_t ctHash = SafeReadU32(activeBp);
    if (ctHash == 0) return 0;

    uint32_t now = GetTickCount();
    if (!force && ctHash == g_ws.lastCustScanCtHash && (now - g_ws.lastCustScanTick) < 2500u) return 0;
    g_ws.lastCustScanTick = now;
    g_ws.lastCustScanCtHash = ctHash;

    uint32_t bpMode = SafeReadU32(activeBp + 0x2050);
    float bpSliders[NUM_SLIDERS] = {}; int dummyNz = 0;
    bool hasSl = ReadSlidersFromBp(activeBp, bpSliders, dummyNz);

    int found = 0;
    std::vector<uint8_t> buf(65536 + 16);
    uint32_t addr = 0x00400000u;
    MEMORY_BASIC_INFORMATION mbi = {};
    while (addr < 0x7FFF0000u && !g_stopWorker.load()) {
        if (!VirtualQuery(reinterpret_cast<LPCVOID>(addr), &mbi, sizeof(mbi))) { addr += 0x10000u; continue; }
        uint32_t regBase = reinterpret_cast<uint32_t>(mbi.BaseAddress);
        uint32_t regSize = static_cast<uint32_t>(mbi.RegionSize);
        uint32_t nextAddr = (regBase + regSize > addr) ? (regBase + regSize) : (addr + 0x10000u);

        bool rw = (mbi.State == MEM_COMMIT) &&
                  ((mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)) != 0) &&
                  ((mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) == 0) &&
                  (regSize >= 0x1000u) && (regSize <= 128u * 1024u * 1024u);
        if (rw) {
            for (uint32_t chunkOff = 0; chunkOff < regSize; chunkOff += 65536u) {
                uint32_t chunkAddr = regBase + chunkOff;
                uint32_t toRead = std::min(65536u, regSize - chunkOff);
                if (toRead < 4u) break;
                SIZE_T br = 0;
                if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(chunkAddr), buf.data(), toRead, &br) || br < 4u) continue;
                for (size_t off = 0; off + 4 <= br; off += 4) {
                    if (*reinterpret_cast<const uint32_t*>(buf.data() + off) != ctHash) continue;
                    uint32_t hitAddr = chunkAddr + static_cast<uint32_t>(off);
                    if (hitAddr < 0x00400800u) continue;
                    uint32_t cr = hitAddr - 0x7B0u;
                    if (g_ws.custRecPool.find(cr) != g_ws.custRecPool.end()) continue;
                    if (DoesCustRecMatchBp(cr, activeBp, ctHash, bpMode, hasSl ? bpSliders : nullptr, hasSl)) {
                        RegisterCustRec(cr, activeBp, g_directCustSeq + 1);
                        found++;
                    }
                }
            }
        }
        addr = nextAddr;
    }
    return found;
}

static void ProbeLivePointersForBp(uint32_t carPtr, uint32_t ratingPtr) {
    uint32_t roots[5] = { carPtr, g_playerCarPtr, g_playerObjPtr, g_tuningDataPtr, ratingPtr };
    std::unordered_set<uint32_t> seen;
    auto testAddr = [&](uint32_t q) {
        if (!IsValidPtr(q) || !seen.insert(q).second) return;
        if (IsValidBlueprint(q)) RegisterBp(q, 0, 0, true);
        else if (q > 0x188Cu && IsValidPtr(q - 0x188Cu) && seen.insert(q - 0x188Cu).second && IsValidBlueprint(q - 0x188Cu)) {
            RegisterBp(q - 0x188Cu, 0, 0, true);
        }
    };
    for (uint32_t r : roots) {
        if (!IsValidPtr(r)) continue;
        testAddr(r);
        uint32_t buf[0x180 / 4];
        if (SafeRead(r, buf, sizeof(buf))) {
            for (size_t i = 0; i < 0x180 / 4; ++i) if (IsValidPtr(buf[i])) testAddr(buf[i]);
        }
    }
}

static int TraverseSliderTree(uint32_t mapPtr, bool readBounds,
                              std::unordered_map<uint32_t, float>* outVals,
                              std::unordered_map<uint32_t, uint32_t>* outNodes,
                              std::unordered_map<uint32_t, SliderBound>* outBounds) {
    if (!IsValidPtr(mapPtr)) return 0;
    uint32_t anchor = mapPtr + 4;
    uint32_t root = SafeReadU32(mapPtr + 0x0C);
    if (!IsValidPtr(root)) return 0;

    const size_t nodeSize = readBounds ? 36 : 32;
    std::unordered_set<uint32_t> visited = { 0, anchor, root };
    std::vector<uint32_t> q;
    q.reserve(64);
    q.push_back(root);
    int count = 0;

    for (size_t head = 0; head < q.size() && head < 80; ++head) {
        uint32_t cur = q[head];
        uint32_t raw[9] = {};
        if (!SafeRead(cur, raw, nodeSize)) continue;
        uint32_t k0 = raw[4], k1 = raw[5];
        if ((k0 == SLIDER_CLASS_STD || k0 == SLIDER_CLASS_CAM) && findSliderIndexByHash(k1) >= 0) {
            if (readBounds && outBounds) {
                float b1 = 0.0f, b2 = 0.0f;
                std::memcpy(&b1, &raw[7], 4);
                std::memcpy(&b2, &raw[8], 4);
                if (IsFiniteFloat(b1) && IsFiniteFloat(b2) && std::fabs(b1) <= 2.0f && std::fabs(b2) <= 2.0f) {
                    SliderBound sb;
                    sb.minB = std::min(b1, b2); sb.maxB = std::max(b1, b2);
                    sb.locked = std::fabs(sb.maxB - sb.minB) < 1e-4f;
                    (*outBounds)[k1] = sb;
                    count++;
                }
            } else if (!readBounds && outVals) {
                float val = 0.0f;
                std::memcpy(&val, &raw[7], 4);
                if (IsFiniteFloat(val) && std::fabs(val) <= 2.0f) {
                    (*outVals)[k1] = val;
                    if (outNodes) (*outNodes)[k1] = cur;
                    count++;
                }
            }
        }
        uint32_t rChild = raw[0], lChild = raw[1];
        if (IsValidPtr(lChild) && visited.insert(lChild).second) q.push_back(lChild);
        if (IsValidPtr(rChild) && visited.insert(rChild).second) q.push_back(rChild);
    }
    return count;
}

static void GetInfoColl3EF(uint32_t collPtr, uint32_t& outIc, uint32_t& outIKey, uint32_t& outArr3EF) {
    outIc = 0; outIKey = 0; outArr3EF = 0;
    if (!IsValidPtr(collPtr)) return;
    auto it = g_ws.info3EFArrCache.find(collPtr);
    if (it != g_ws.info3EFArrCache.end()) {
        outArr3EF = it->second;
        auto itm = g_ws.infoCollMetaCache.find(collPtr);
        if (itm != g_ws.infoCollMetaCache.end()) { outIc = itm->second.first; outIKey = itm->second.second; }
        return;
    }
    uint32_t mLayout = SafeReadU32(collPtr + 0x18);
    if (!IsValidPtr(mLayout)) return;
    uint32_t refCls = SafeReadU32(mLayout + 0x00), refKey = SafeReadU32(mLayout + 0x04), ic = SafeReadU32(mLayout + 0x08);
    if (!IsValidPtr(ic) && refCls == 0x158D43BFu && refKey != 0) {
        auto iti = g_ws.infoCollByKey.find(refKey);
        if (iti != g_ws.infoCollByKey.end()) ic = iti->second;
    }
    if (!IsValidPtr(ic)) return;
    uint32_t iKey = SafeReadU32(ic + 0x10), iTab = SafeReadU32(ic + 0x00), iCnt = SafeReadU32(ic + 0x04) & 0xFFFFu;
    uint32_t arr3 = 0;
    if (IsValidPtr(iTab) && iCnt > 0 && iCnt <= 16) {
        for (uint32_t a = 0; a < iCnt; ++a) {
            if (SafeReadU32(iTab + a * 12) == 0x3EF64943u) { arr3 = SafeReadU32(iTab + a * 12 + 4); break; }
        }
    }
    g_ws.info3EFArrCache[collPtr] = arr3;
    g_ws.infoCollMetaCache[collPtr] = { ic, iKey };
    outIc = ic; outIKey = iKey; outArr3EF = arr3;
}

static void FillPartItem(uint32_t arr6B8, uint32_t mCnt, uint32_t arr3EF, uint32_t idx, ShmPartItem& dst) {
    std::memset(&dst, 0, sizeof(dst));
    if (IsValidPtr(arr6B8) && idx < mCnt) {
        uint32_t eAddr = arr6B8 + 8 + idx * 12;
        dst.physKey = SafeReadU32(eAddr + 4);
        dst.physCol = SafeReadU32(eAddr + 8);
        dst.hasPhys = 1;
        if (IsValidPtr(dst.physCol)) {
            uint32_t rPar = SafeReadU32(dst.physCol + 0x0C);
            if (IsValidPtr(rPar)) dst.parKey = SafeReadU32(rPar + 0x10);
            uint32_t rTab = SafeReadU32(dst.physCol + 0x00);
            uint32_t rEnt = SafeReadU32(dst.physCol + 0x04) & 0xFFFFu;
            if (IsValidPtr(rTab) && rEnt > 0 && rEnt <= 24) {
                std::string attrs;
                uint32_t lim = std::min(rEnt, 10u);
                for (uint32_t a = 0; a < lim; ++a) {
                    uint32_t ak = SafeReadU32(rTab + a * 12), av = SafeReadU32(rTab + a * 12 + 4);
                    float af = 0.0f; std::memcpy(&af, &av, 4);
                    char piece[64];
                    if (IsValidPtr(av)) {
                        std::string s = TryReadAsciiSafe(av);
                        if (!s.empty()) std::snprintf(piece, sizeof(piece), "%08X=\"%.20s\"", ak, s.c_str());
                        else {
                            float subF = SafeReadF32(av);
                            if (IsFiniteFloat(subF) && std::fabs(subF) >= 0.0001f && std::fabs(subF) <= 50000.0f)
                                std::snprintf(piece, sizeof(piece), "%08X=*{%.3f}", ak, subF);
                            else
                                std::snprintf(piece, sizeof(piece), "%08X=*[%08X]", ak, SafeReadU32(av));
                        }
                    } else if (IsFiniteFloat(af) && std::fabs(af) >= 0.001f && std::fabs(af) <= 50000.0f && av > 0x3A000000u && av < 0x50000000u) {
                        std::snprintf(piece, sizeof(piece), "%08X=%.3f", ak, af);
                    } else {
                        std::snprintf(piece, sizeof(piece), "%08X=%08X", ak, av);
                    }
                    if (!attrs.empty()) attrs += ", ";
                    attrs += piece;
                    if (attrs.size() >= sizeof(dst.physAttrs) - 16) break;
                }
                std::strncpy(dst.physAttrs, attrs.c_str(), sizeof(dst.physAttrs) - 1);
            }
        }
    }
    if (IsValidPtr(arr3EF)) {
        uint32_t cnt = SafeReadU32(arr3EF + 0) & 0xFFFFu;
        uint32_t elemSize = SafeReadU32(arr3EF + 4) & 0xFFFFu;
        if (cnt > 0 && idx < cnt && elemSize >= 28 && elemSize <= 128) {
            uint32_t eAddr = arr3EF + 8 + idx * elemSize;
            uint32_t nDw = std::min(elemSize / 4u, 14u);
            if (SafeRead(eAddr, dst.dws, nDw * 4)) {
                dst.hasMeta3EF = 1;
                dst.cost       = dst.dws[2];
                dst.exactStage = static_cast<uint8_t>(dst.dws[3] & 0xFFu);
                dst.brandHash  = dst.dws[6];
                if (dst.exactStage > 0) {
                    uint8_t cf = 0;
                    for (uint32_t j = 1; j <= idx; ++j) {
                        if ((SafeReadU32(arr3EF + 8 + j * elemSize + 0x0C) & 0xFFu) == dst.exactStage) cf++;
                    }
                    dst.cfgInStage = cf;
                }
                std::string strs;
                for (uint32_t d = 0; d < nDw; ++d) {
                    if (IsStaticPtr(dst.dws[d])) {
                        std::string s = TryReadAsciiSafe(dst.dws[d]);
                        if (!s.empty()) {
                            char tmp[48];
                            std::snprintf(tmp, sizeof(tmp), "+%02X:\"%.24s\"", d * 4, s.c_str());
                            if (!strs.empty()) strs += ", ";
                            strs += tmp;
                            if (strs.size() >= sizeof(dst.strs) - 16) break;
                        }
                    }
                }
                std::strncpy(dst.strs, strs.c_str(), sizeof(dst.strs) - 1);
            }
        }
    }
}

static std::vector<uint32_t> CollectMatchingBps(const std::string& canonHash, bool playerOnly) {
    std::vector<uint32_t> out;
    if (canonHash.empty()) return out;
    uint32_t expectedType = 0;
    auto itt = g_ws.knownTypeByHash.find(canonHash);
    if (itt != g_ws.knownTypeByHash.end()) expectedType = itt->second;

    for (auto it = g_ws.bpPool.begin(); it != g_ws.bpPool.end(); ) {
        if (!IsValidBlueprint(it->first)) { it = g_ws.bpPool.erase(it); continue; }
        BpRecord& rec = it->second;
        if (rec.identifiedCarHash.empty()) rec.identifiedCarHash = IdentifyBpCarModel(rec.addr, rec.carType);
        bool match = !rec.identifiedCarHash.empty() ? (rec.identifiedCarHash == canonHash) : (expectedType > 0 && rec.carType == expectedType);
        if (match && (!playerOnly || !rec.seenAsBot || rec.playerOwned)) out.push_back(rec.addr);
        ++it;
    }
    std::sort(out.begin(), out.end());
    return out;
}

static uint32_t ResolveActiveBp(
    const std::string& canonHash, uint32_t /*ctxMode*/, uint32_t carPtr, uint32_t ratingPtr,
    const std::unordered_map<uint32_t, float>& liveMapVals,
    uint32_t& outCandCount, uint32_t& outCandIdx, std::string& outSourceLabel,
    bool forceRefresh = false
) {
    outCandCount = 0; outCandIdx = 0;
    IngestBpRing();
    if (canonHash.empty()) { outSourceLabel = "Ожидание авто"; return 0; }

    uint32_t now = GetTickCount();
    std::vector<uint32_t> cands = CollectMatchingBps(canonHash, false);

    if (cands.empty() && (forceRefresh || (now - g_ws.lastPtrProbeTick) >= 500u)) {
        g_ws.lastPtrProbeTick = now;
        ProbeLivePointersForBp(carPtr, ratingPtr);
        cands = CollectMatchingBps(canonHash, false);
    }
    if (cands.empty()) {
        ScanHeapForBlueprints(false);
        cands = CollectMatchingBps(canonHash, false);
    }

    outCandCount = static_cast<uint32_t>(cands.size());
    if (cands.empty()) { outSourceLabel = "VLT (BP не найден)"; return 0; }

    if (g_ws.pinnedBpAddr != 0) {
        for (size_t i = 0; i < cands.size(); ++i) {
            if (cands[i] == g_ws.pinnedBpAddr) {
                outCandIdx = static_cast<uint32_t>(i);
                outSourceLabel = "Pinned (" + std::to_string(i + 1) + "/" + std::to_string(cands.size()) + ")";
                return cands[i];
            }
        }
        g_ws.pinnedBpAddr = 0;
    }

    uint32_t maxSeq = 0;
    for (uint32_t bp : cands) maxSeq = std::max(maxSeq, g_ws.bpPool[bp].lastRingSeq);

    bool doSig = forceRefresh || (now - g_ws.lastSigPollTick) >= 250u;
    if (doSig) g_ws.lastSigPollTick = now;

    uint32_t bestBp = 0; int bestScore = -999999; std::string bestTag = "HeapScan"; size_t bestIdx = 0;
    for (size_t idx = 0; idx < cands.size(); ++idx) {
        uint32_t bp = cands[idx];
        BpRecord& rec = g_ws.bpPool[bp];
        if (doSig || rec.lastSigHash == 0) {
            int u = 0, nz = 0;
            uint64_t sig = CalcBpQuickSig(bp, u, nz);
            if (rec.lastSigHash != 0 && rec.lastSigHash != sig) rec.lastChangedTick = now;
            rec.lastSigHash = sig;
            rec.cachedUpgCnt = u;
            rec.cachedNzCnt = nz;
        }
        int upgCnt = rec.cachedUpgCnt, nzCnt = rec.cachedNzCnt;

        int score = 0; std::string tag = "HeapScan";
        if (rec.seenAsBot && !rec.playerOwned) { score -= 600; tag = "Bot?"; }
        else if (rec.playerOwned)              { score += 400; tag = "PlayerBP"; }
        if (bp == g_directBuildBp && g_directBuildSeq > 0) { score += 2000; tag = "BuildSpec"; }
        else if (rec.directRef) { score += 1000; tag = "DirectPtr"; }
        if (rec.lastRingSeq > 0 && maxSeq > 0) {
            if (rec.lastRingSeq == maxSeq) { score += 350; if (!rec.seenAsBot) tag = "RingLive"; }
            else if (maxSeq - rec.lastRingSeq <= 3) score += 120;
        }
        if (rec.lastChangedTick != 0 && (now - rec.lastChangedTick) < 8000u) { score += 850; tag = "LiveEdit"; }
        if (!liveMapVals.empty()) {
            float bpVals[NUM_SLIDERS]; int dummyNz = 0;
            if (ReadSlidersFromBp(bp, bpVals, dummyNz)) {
                int matchCnt = 0, mismatchCnt = 0, nzMatch = 0;
                for (int s = 0; s < NUM_SLIDERS; ++s) {
                    auto it = liveMapVals.find(SLIDER_DEFS[s].hash);
                    if (it != liveMapVals.end()) {
                        if (std::fabs(bpVals[s] - it->second) < 0.02f) {
                            matchCnt++;
                            if (std::fabs(it->second) > 0.02f) nzMatch++;
                        } else mismatchCnt++;
                    }
                }
                if (mismatchCnt == 0 && matchCnt > 0) { score += 900 + nzMatch * 60; tag = "TuningSync"; }
                else if (mismatchCnt > 0) score -= mismatchCnt * 80;
            }
        }
        if (upgCnt > 0) score += 25 + upgCnt * 3;
        if (nzCnt > 0)  score += 20;
        if (bp == g_ws.activeBpAddr) score += 15;

        if (score > bestScore) { bestScore = score; bestBp = bp; bestTag = tag; bestIdx = idx; }
    }
    outCandIdx = static_cast<uint32_t>(bestIdx);
    outSourceLabel = bestTag + " (" + std::to_string(bestIdx + 1) + "/" + std::to_string(cands.size()) + ")";
    return bestBp;
}

static void HandleCommand(
    uint32_t cmdType, uint32_t p1, uint32_t p2, float fp,
    const std::unordered_map<uint32_t, uint32_t>& liveNodes, bool inTuningUi
) {
    g_shm->cmdResultOk = 0;
    g_shm->cmdResultMsg[0] = '\0';

    if (cmdType == CMD_SYNC_KNOWN_CAR_TYPE) {
        if (p1 != 0 && p2 > 0 && p2 < 256) {
            std::string ch = ProStreetMeta::canonicalCarHash(ProStreetMeta::toHex8(p1));
            g_ws.knownTypeByHash[ch] = p2;
            g_ws.knownHashByType[p2] = ch;
            g_shm->cmdResultOk = 1;
        }
        return;
    }
    if (cmdType == CMD_RESCAN_BP) {
        int n = ScanHeapForBlueprints(true);
        g_ws.pinnedBpAddr = 0;
        g_shm->cmdResultOk = 1;
        std::snprintf(g_shm->cmdResultMsg, sizeof(g_shm->cmdResultMsg),
                      "Скан завершён: +%d новых BP (всего в пуле: %u)", n, (unsigned)g_ws.bpPool.size());
        return;
    }
    if (cmdType == CMD_CYCLE_BP) {
        if (p1 == 0) {
            g_ws.pinnedBpAddr = 0; g_shm->cmdResultOk = 1;
            std::snprintf(g_shm->cmdResultMsg, sizeof(g_shm->cmdResultMsg), "Автовыбор Blueprint включён");
            return;
        }
        std::vector<uint32_t> cands = CollectMatchingBps(g_ws.activeCanonHash, false);
        if (cands.empty()) {
            std::snprintf(g_shm->cmdResultMsg, sizeof(g_shm->cmdResultMsg), "Нет Blueprint для переключения");
            return;
        }
        int curI = 0;
        for (size_t i = 0; i < cands.size(); ++i) if (cands[i] == g_ws.activeBpAddr) { curI = (int)i; break; }
        int nextI = (p1 == 1) ? ((curI + 1) % (int)cands.size()) : ((curI - 1 + (int)cands.size()) % (int)cands.size());
        g_ws.pinnedBpAddr = cands[nextI];
        g_shm->cmdResultOk = 1;
        std::snprintf(g_shm->cmdResultMsg, sizeof(g_shm->cmdResultMsg), "Выбран BP %d/%d (0x%08X)", nextI + 1, (int)cands.size(), g_ws.pinnedBpAddr);
        return;
    }
    auto writeOneSlider = [&](int sIdx, float newVal, float oldVal) -> bool {
        if (sIdx < 0 || sIdx >= NUM_SLIDERS) return false;
        uint32_t hash = SLIDER_DEFS[sIdx].hash;
        uint32_t bpIdx = static_cast<uint32_t>(SLIDER_DEFS[sIdx].bpIdx);
        newVal = std::max(-1.0f, std::min(1.0f, newVal));
        bool wrote = false;

        // 1. Запись напрямую во ВСЕ постоянные записи карьеры активной машины ([FECustomizationRecord + 0x730 + bpIdx * 4])!
        uint32_t curCarType0 = (IsValidPtr(g_ws.activeBpAddr) && IsValidBlueprint(g_ws.activeBpAddr)) ? SafeReadU32(g_ws.activeBpAddr) : 0u;
        if (curCarType0 != 0) {
            uint32_t bpMode = SafeReadU32(g_ws.activeBpAddr + 0x2050);
            float bpSl[NUM_SLIDERS] = {}; int dNz = 0;
            bool hasSl = ReadSlidersFromBp(g_ws.activeBpAddr, bpSl, dNz);
            std::vector<uint32_t> matchedCrs;
            for (auto it = g_ws.custRecPool.begin(); it != g_ws.custRecPool.end(); ) {
                if (!IsLiveCustRec(it->first, 0)) { it = g_ws.custRecPool.erase(it); continue; }
                if (it->second.carType0 == curCarType0 && DoesCustRecMatchBp(it->first, g_ws.activeBpAddr, curCarType0, bpMode, hasSl ? bpSl : nullptr, false)) {
                    matchedCrs.push_back(it->first);
                }
                ++it;
            }
            if (matchedCrs.empty()) {
                ScanHeapForCustRecs(g_ws.activeBpAddr, true);
                for (const auto& kv : g_ws.custRecPool) {
                    if (kv.second.carType0 == curCarType0 && DoesCustRecMatchBp(kv.first, g_ws.activeBpAddr, curCarType0, bpMode, hasSl ? bpSl : nullptr, false)) {
                        matchedCrs.push_back(kv.first);
                    }
                }
            }
            for (uint32_t cr : matchedCrs) {
                if (SafeWriteF32(cr + 0x730u + bpIdx * 4u, newVal)) wrote = true;
            }
        }

        // 2. Запись во все живые копии Blueprint (включая встроенный TuningScreen + 0xE0)
        if (IsValidPtr(g_ws.activeBpAddr) && IsValidBlueprint(g_ws.activeBpAddr)) {
            uint32_t relOff = 0x205Cu + bpIdx * 12u;
            std::vector<uint32_t> bps = CollectMatchingBps(g_ws.activeCanonHash, (g_raceDayActive != 0));
            if (g_ws.pinnedBpAddr != 0) { bps.clear(); bps.push_back(g_ws.pinnedBpAddr); }
            if (bps.empty()) bps.push_back(g_ws.activeBpAddr);
            for (uint32_t bp : bps) if (SafeWriteF32(bp + relOff, newVal)) wrote = true;
        }

        // 3. Запись в активное дерево eastl::map (TuningData + 0x2C или TuningMap)
        auto itNode = liveNodes.find(hash);
        if (itNode != liveNodes.end() && IsValidPtr(itNode->second)) {
            uint32_t k0 = SafeReadU32(itNode->second + 0x10), k1 = SafeReadU32(itNode->second + 0x14);
            if ((k0 == SLIDER_CLASS_STD || k0 == SLIDER_CLASS_CAM) && k1 == hash) {
                if (SafeWriteF32(itNode->second + 0x1C, newVal)) wrote = true;
            }
        }

        // 4. Синхронизация активного UI-виджета ползунка в меню Тюнинга ([+0xF0/+0xF4/+0xF8])
        if (inTuningUi) {
            for (int i = 0; i < UI_WIDGET_CAP; ++i) {
                if (g_uiWidgets[i].hash == hash && IsValidPtr(g_uiWidgets[i].widgetPtr)) {
                    uint32_t wPtr = g_uiWidgets[i].widgetPtr;
                    float scale = SafeReadF32(wPtr + 0x114), orig = SafeReadF32(wPtr + 0x120);
                    float curF0 = SafeReadF32(wPtr + 0xF0), curF4 = SafeReadF32(wPtr + 0xF4), curF8 = SafeReadF32(wPtr + 0xF8);
                    if (IsFiniteFloat(scale) && std::fabs(scale) > 0.01f && std::fabs(scale) < 5000.0f && IsFiniteFloat(orig) && IsFiniteFloat(curF0)) {
                        if (std::fabs(((curF0 - orig) / scale) - oldVal) < 0.30f) {
                            float newUiPos = newVal * scale + orig;
                            SafeWriteF32(wPtr + 0xF0, newUiPos);
                            if (IsFiniteFloat(curF4) && std::fabs(curF4 - curF0) < 1.0f) SafeWriteF32(wPtr + 0xF4, newUiPos);
                            if (IsFiniteFloat(curF8) && std::fabs(curF8 - curF0) < 1.0f) SafeWriteF32(wPtr + 0xF8, newUiPos);
                            wrote = true;
                        }
                    }
                    break;
                }
            }
        }
        return wrote;
    };

    if (cmdType == CMD_SET_SLIDER) {
        int sIdx = findSliderIndexByHash(p1);
        if (sIdx < 0) return;
        bool wrote = writeOneSlider(sIdx, fp, g_shm->sliders[sIdx].value);
        if (wrote) TriggerLiveDynoRecalc(g_ws.activeBpAddr);
        g_ws.lastActiveActivityTick = GetTickCount();
        g_ws.lastFastPubHash = 0;
        g_shm->cmdResultOk = wrote ? 1u : 0u;
        return;
    }
    if (cmdType == CMD_RESET_SLIDERS) {
        int resetCnt = 0;
        for (int i = 0; i < NUM_SLIDERS; ++i) {
            if (g_shm->sliders[i].locked == 0 && g_shm->sliders[i].editable != 0) {
                if (writeOneSlider(i, 0.0f, g_shm->sliders[i].value)) resetCnt++;
            }
        }
        if (resetCnt > 0) TriggerLiveDynoRecalc(g_ws.activeBpAddr);
        g_ws.lastActiveActivityTick = GetTickCount();
        g_ws.lastFastPubHash = 0;
        g_shm->cmdResultOk = (resetCnt > 0) ? 1u : 0u;
        std::snprintf(g_shm->cmdResultMsg, sizeof(g_shm->cmdResultMsg),
                      resetCnt > 0 ? "Ползунки сброшены в 0 (%d шт.)" : "Нет доступных ползунков для сброса", resetCnt);
        return;
    }
    if (cmdType == CMD_INSTALL_PART) {
        if (p1 >= NUM_INSPECT_SLOTS) return;
        uint32_t slotNum = INSPECT_SLOT_DEFS[p1].slot;
        uint32_t feSlot  = INSPECT_SLOT_DEFS[p1].feSlot; // ИСТИННЫЙ слот (0..164) в bp+0x58 и FECustomizationRecord+0x34!
        uint32_t idx = p2;

        // 1. Принудительно находим ВСЕ живые копии Blueprint и постоянные FECustomizationRecord текущей машины
        ScanHeapForBlueprints(true);
        bool filterPlayerOnly = (g_raceDayActive != 0);
        std::vector<uint32_t> bps = CollectMatchingBps(g_ws.activeCanonHash, filterPlayerOnly);
        if (g_ws.pinnedBpAddr != 0 && IsValidBlueprint(g_ws.pinnedBpAddr)) { bps.clear(); bps.push_back(g_ws.pinnedBpAddr); }
        if (bps.empty() && IsValidBlueprint(g_ws.activeBpAddr)) bps.push_back(g_ws.activeBpAddr);
        if (bps.empty()) {
            std::snprintf(g_shm->cmdResultMsg, sizeof(g_shm->cmdResultMsg), "Нет активного Blueprint — открой авто в Гараже или нажми [Найти BP]");
            return;
        }
        uint32_t primaryBp = IsValidBlueprint(g_ws.activeBpAddr) ? g_ws.activeBpAddr : bps[0];
        uint32_t bpCarType0 = SafeReadU32(primaryBp);
        uint32_t bpMode = SafeReadU32(primaryBp + 0x2050);
        float bpSl[NUM_SLIDERS] = {}; int dNz = 0;
        bool hasSl = ReadSlidersFromBp(primaryBp, bpSl, dNz);

        ScanHeapForCustRecs(primaryBp, true);

        uint32_t vltSlotPtr = 0, vltRearBrakePtr = 0;
        auto itCar = g_ws.vltCars.find(g_ws.activeCanonHash);
        if (itCar != g_ws.vltCars.end()) { vltSlotPtr = itCar->second.slotPtr[p1]; vltRearBrakePtr = itCar->second.rearBrakePtr; }

        uint32_t rearBrakeFeSlot = 0;
        if (feSlot == 10u) {
            for (uint32_t rs = 9u; rs <= 13u; ++rs) {
                if (rs == 10u) continue;
                uint32_t rW1 = SafeReadU32(primaryBp + 0x58u + rs * 0x10u + 4u);
                if (IsValidPtr(rW1)) {
                    uint32_t rPl = SafeReadU32(rW1 + 0x18);
                    if (IsValidPtr(rPl) && TryReadAsciiSafe(SafeReadU32(rPl + 0x10)) == "BRAKE_REAR") {
                        rearBrakeFeSlot = rs;
                        break;
                    }
                }
            }
        }

        // Собираем ТОЛЬКО записи карьеры, соответствующие текущему Blueprint (UserProfile + копия Гаража)
        std::vector<uint32_t> targetCrs;
        for (auto it = g_ws.custRecPool.begin(); it != g_ws.custRecPool.end(); ) {
            if (!IsLiveCustRec(it->first, 0)) { it = g_ws.custRecPool.erase(it); continue; }
            if (it->second.carType0 == bpCarType0 && DoesCustRecMatchBp(it->first, primaryBp, bpCarType0, bpMode, hasSl ? bpSl : nullptr, hasSl)) {
                targetCrs.push_back(it->first);
            }
            ++it;
        }
        // Если строгая проверка ползунков не нашла (напр., ползунки уже редактировались), смягчаем только проверку ползунков
        if (targetCrs.empty()) {
            for (const auto& kv : g_ws.custRecPool) {
                if (kv.second.carType0 == bpCarType0 && DoesCustRecMatchBp(kv.first, primaryBp, bpCarType0, bpMode, nullptr, false)) {
                    targetCrs.push_back(kv.first);
                }
            }
        }

        // 2. Ищем точный 16-битный код (a<<8 | b<<4 | c) через родную 0077F2E0 на ГЛАВНОМ потоке игры!
        auto findPacked16Safe = [&](uint32_t targetFeSlot, uint32_t targetW1, uint32_t targetIdx, uint16_t curPacked, uint16_t& outPacked, uint32_t outDbPart[4]) -> bool {
            g_mtJob = MainThreadJob{};
            g_mtJob.type      = MainThreadJob::JOB_FIND_PACKED16;
            g_mtJob.carType0  = bpCarType0;
            g_mtJob.feSlot    = targetFeSlot;
            g_mtJob.targetW1  = targetW1;
            g_mtJob.targetIdx = targetIdx;
            g_mtJob.curPacked = curPacked;
            if (RunMainThreadJob(500u) && g_mtJob.found) {
                outPacked = g_mtJob.outPacked;
                std::memcpy(outDbPart, g_mtJob.outDbPart, 16);
                return true;
            }
            return false;
        };

        uint16_t seedCurP = 0xFFFFu;
        if (!targetCrs.empty()) SafeRead(targetCrs[0] + 0x34u + feSlot * 2u, &seedCurP, 2);

        uint16_t newMainP16 = 0xFFFFu;
        uint32_t verifiedDbPart[4] = {};
        bool haveVerifiedPart = findPacked16Safe(feSlot, vltSlotPtr, idx, seedCurP, newMainP16, verifiedDbPart);

        uint16_t newRearP16 = 0xFFFFu;
        uint32_t verifiedRearPart[4] = {};
        bool haveVerifiedRear = false;
        if (feSlot == 10u && rearBrakeFeSlot != 0) {
            uint16_t seedRearP = 0xFFFFu;
            if (!targetCrs.empty()) SafeRead(targetCrs[0] + 0x34u + rearBrakeFeSlot * 2u, &seedRearP, 2);
            haveVerifiedRear = findPacked16Safe(rearBrakeFeSlot, vltRearBrakePtr, idx, seedRearP, newRearP16, verifiedRearPart);
        }

        // 3. Пишем подтверждённый 16-битный код во ВСЕ постоянные FECustomizationRecord (UserProfile + Гараж)
        int custApplied = 0;
        if (haveVerifiedPart) {
            for (uint32_t cr : targetCrs) {
                if (SafeWrite(cr + 0x34u + feSlot * 2u, &newMainP16, 2)) custApplied++;
                if (haveVerifiedRear && rearBrakeFeSlot != 0) {
                    SafeWrite(cr + 0x34u + rearBrakeFeSlot * 2u, &newRearP16, 2);
                }
            }
        }

        // 4. Обновляем ВСЕ живые копии Blueprint (bp + 0x58 + feSlot * 0x10)
        int applied = 0; bool rearSynced = false;
        for (uint32_t bp : bps) {
            uint32_t off = 0x58u + feSlot * 0x10u;
            if (haveVerifiedPart) {
                verifiedDbPart[0] = (verifiedDbPart[0] & 0xFFFFFF00u) | 1u;
                SafeWrite(bp + off, verifiedDbPart, 16);
                applied++;
            } else {
                uint32_t curW1 = SafeReadU32(bp + off + 0x04), curW2 = SafeReadU32(bp + off + 0x08);
                bool needW1 = !IsValidPtr(curW1);
                if (!needW1) {
                    uint32_t dArr = 0, bpCnt = 0; GetPartPhysicsArrayInfo(curW1, dArr, bpCnt);
                    if (bpCnt <= idx || (idx > 0 && bpCnt <= 1)) needW1 = true;
                    else if (slotNum == 7) {
                        uint32_t pl = SafeReadU32(curW1 + 0x18);
                        if (IsValidPtr(pl) && TryReadAsciiSafe(SafeReadU32(pl + 0x10)) == "WIDEBODY") needW1 = true;
                    }
                }
                if (needW1 && IsValidPtr(vltSlotPtr)) SafeWriteU32(bp + off + 0x04, vltSlotPtr);

                uint32_t newW2 = idx;
                if (slotNum == 7 || slotNum == 90) newW2 = idx;
                else if (slotNum == 78) newW2 = (idx == 0) ? 0u : ((((curW2 >> 16) & 1u) ? 0x00010000u : 0u) + idx);
                else newW2 = (curW2 & 0xFFFF0000u) + idx;
                SafeWriteU32(bp + off + 0x08, newW2);
                applied++;
            }

            if (feSlot == 10u && rearBrakeFeSlot != 0) {
                uint32_t rOff = 0x58u + rearBrakeFeSlot * 0x10u;
                if (haveVerifiedRear) {
                    verifiedRearPart[0] = (verifiedRearPart[0] & 0xFFFFFF00u) | 1u;
                    SafeWrite(bp + rOff, verifiedRearPart, 16);
                    rearSynced = true;
                } else {
                    uint32_t rW1 = SafeReadU32(bp + rOff + 0x04);
                    uint32_t dArr = 0, rCnt = 0;
                    if (IsValidPtr(rW1)) GetPartPhysicsArrayInfo(rW1, dArr, rCnt);
                    if ((rCnt <= idx || (idx > 0 && rCnt <= 1)) && IsValidPtr(vltRearBrakePtr)) {
                        SafeWriteU32(bp + rOff + 0x04, vltRearBrakePtr);
                        GetPartPhysicsArrayInfo(vltRearBrakePtr, dArr, rCnt);
                    }
                    if (rCnt == 0 || idx < rCnt) {
                        SafeWriteU32(bp + rOff + 0x08, (SafeReadU32(bp + rOff + 0x08) & 0xFFFF0000u) + idx);
                        rearSynced = true;
                    }
                }
            }
        }

        // 5. Мгновенный живой пересчёт диностенда на главном потоке (обновляет колонку «Текущее» сразу!)
        bool dynoOk = TriggerLiveDynoRecalc(primaryBp);

        if (!inTuningUi) { g_ws.curBounds.clear(); g_ws.hasLiveBounds = false; }
        g_ws.lastCatalogKeyHash = 0;
        g_ws.lastUpgKeyHash = 0;
        g_ws.lastFastPubHash = 0;
        g_ws.lastActiveActivityTick = GetTickCount();
        g_shm->cmdResultOk = 1;
        std::snprintf(g_shm->cmdResultMsg, sizeof(g_shm->cmdResultMsg),
                      "Установлено [%s]: w2=%u%s (BP:%d | Карьера:%d%s)",
                      INSPECT_SLOT_DEFS[p1].nameUtf8, idx, rearSynced ? " + зад." : "",
                      applied, custApplied, dynoOk ? " | Дино OK" : "");
    }
}

static DWORD WINAPI WorkerThreadProc(LPVOID) {
    uint32_t lastSeenHookSeq = 0;
    HookGarageSnap localSnap = {};
    bool haveSnap = false;
    uint32_t lastCmdHandled = 0;

    while (!g_stopWorker.load()) {
        if (!g_shm) { Sleep(50); continue; }
        if (g_shm->unloadRequested != 0) break;

        uint32_t now = GetTickCount();
        g_shm->dllHeartbeat = now;

        if (g_raceDayActive == 1 && g_monIsBot == 1 && (now - g_lastBotHitTick) > 600u) {
            g_monIsBot = 0;
            if (g_monCtxMode != CTX_TUNING_UI) g_monCtxMode = CTX_RACEDAY_HUB;
        }

        uint32_t s1 = g_hookSnapSeq;
        if (s1 != 0 && (s1 & 1u) == 0 && s1 != lastSeenHookSeq) {
            HookGarageSnap tmp;
            std::memcpy(&tmp, &g_pubSnap, sizeof(tmp));
            if (g_hookSnapSeq == s1) { localSnap = tmp; lastSeenHookSeq = s1; haveSnap = true; }
        }

        // Сначала читаем статическую таблицу RideInfo (nfs.exe+6EAA48) и проиндексируем VLT,
        // чтобы активное авто, BP, детали и ползунки появились МГНОВЕННО при запуске без смены авто!
        IngestBpRing();
        if (!g_ws.vltReady) {
            EnsureVltIndexed(haveSnap ? localSnap.pvehCollPtr : 0u);
        }

        // Запоминаем pvehArg для активного ctHash (для живого пересчёта колонки «Текущее»)
        if (g_lastPvehArg != 0) {
            uint32_t refBp = IsValidBlueprint(g_lastPvehArgBp) ? g_lastPvehArgBp : g_ws.activeBpAddr;
            if (IsValidBlueprint(refBp)) {
                uint32_t ct = SafeReadU32(refBp);
                if (ct != 0) g_ws.pvehArgByCtHash[ct] = g_lastPvehArg;
            }
        }

        std::string baseHash, varHash, chainStr;
        if (haveSnap) {
            std::vector<std::string> staticNodes, fullStatic;
            for (int i = 0; i < 8; ++i) {
                uint32_t k = localSnap.chainKeys[i], s = localSnap.chainSrcs[i];
                if (k == 0) break;
                if (IsStaticPtr(s)) {
                    std::string hx = ProStreetMeta::toHex8(k);
                    fullStatic.push_back(hx);
                    if (!ProStreetMeta::isRootVltHash(hx)) staticNodes.push_back(hx);
                }
            }
            if (!staticNodes.empty()) {
                baseHash = (staticNodes.size() >= 2) ? staticNodes[1] : staticNodes[0];
                varHash  = staticNodes[0];
            }
            for (size_t i = 0; i < fullStatic.size(); ++i) {
                if (i > 0) chainStr += " -> ";
                chainStr += fullStatic[i];
            }
        } else if (g_ws.vltReady) {
            // Мгновенный автодетект активного авто из статической таблицы (nfs.exe+6EAA54/50/4C) ДО первого кадра дино!
            uint32_t bootBp = 0;
            uint32_t statAddrs[3] = { g_nfsBase + 0x6EAA54u, g_nfsBase + 0x6EAA50u, g_nfsBase + 0x6EAA4Cu };
            for (uint32_t sa : statAddrs) {
                uint32_t p = SafeReadU32(sa);
                if (IsValidBlueprint(p)) { bootBp = p; break; }
            }
            if (bootBp != 0) {
                uint32_t bt = GetBpCarTypeId(bootBp);
                std::string idHash = IdentifyBpCarModel(bootBp, bt);
                if (!idHash.empty()) {
                    baseHash = idHash;
                    varHash  = idHash;
                    chainStr = idHash + " (BP-старт)";
                    uint32_t bm = SafeReadU32(bootBp + 0x2050);
                    if (bm <= 3u) localSnap.carMode = bm;
                }
            }
        }

        std::string canonHash = ProStreetMeta::canonicalCarHash(baseHash);
        bool carOrModeChanged = (!canonHash.empty() && (canonHash != g_ws.activeCanonHash || localSnap.carMode != g_ws.activeMode));
        if (carOrModeChanged) {
            g_ws.activeBaseHash = baseHash; g_ws.activeCanonHash = canonHash; g_ws.activeVarHash = varHash;
            g_ws.activeMode = localSnap.carMode; g_ws.pinnedBpAddr = 0; g_ws.activeBpAddr = 0;
            g_ws.curBounds.clear(); g_ws.hasLiveBounds = false; g_ws.lastBoundsTdPtr = 0; g_ws.curPartsSig.clear();
            g_ws.tuningMapSeqAtCarSwitch = g_tuningMapSeq;
            std::memset(g_uiWidgets, 0, sizeof(g_uiWidgets));
        } else if (!varHash.empty()) {
            g_ws.activeVarHash = varHash;
        }

        // Прямая сверка с глобальным указателем экрана Тюнинга [nfs.exe+6B28A4] (00AB28A4)
        uint32_t globTs = SafeReadU32(g_nfsBase + 0x6B28A4u);
        if (IsValidPtr(globTs)) {
            g_tuningScreenPtr = globTs;
            uint32_t gtd = SafeReadU32(globTs + 0xDC);
            if (IsValidPtr(gtd)) { g_tuningDataPtr = gtd; g_inTuningUi = 1; }
        } else if (g_tuningScreenPtr != 0) {
            g_tuningScreenPtr = 0;
            g_inTuningUi = 0;
            g_tuningDataPtr = 0;
        }
        bool inTuningUi = (g_inTuningUi == 1) && IsValidPtr(g_tuningDataPtr);
        bool ctxChanged = (g_monCtxMode != g_ws.lastCtxModeSeen) || (inTuningUi != g_ws.lastInTuningUiSeen);
        if (ctxChanged) {
            g_ws.lastCtxModeSeen = g_monCtxMode;
            g_ws.lastInTuningUiSeen = inTuningUi;
            g_ws.lastActiveActivityTick = now;
            if (!g_ws.activeCanonHash.empty()) {
                // При входе/выходе в Тюнинг или Стайлинг сразу ловим рабочую копию Blueprint!
                ScanHeapForBlueprints(false);
            }
        }

        std::unordered_map<uint32_t, float> liveMapVals;
        std::unordered_map<uint32_t, uint32_t> liveMapNodes;
        int liveValCnt = 0;

        if (inTuningUi) {
            uint32_t td = g_tuningDataPtr;
            liveValCnt = TraverseSliderTree(td + 0x2Cu, false, &liveMapVals, &liveMapNodes, nullptr);
            if (td != g_ws.lastBoundsTdPtr) {
                g_ws.lastBoundsTdPtr = td;
                g_ws.boundsTdStartTick = now;
                g_ws.lastBoundsPollTick = 0;
            }
            bool needBounds = (!g_ws.hasLiveBounds) ||
                              ((now - g_ws.boundsTdStartTick) < 1200u && (now - g_ws.lastBoundsPollTick) >= 80u) ||
                              (g_ws.curBounds.size() < NUM_SLIDERS && (now - g_ws.lastBoundsPollTick) >= 250u);
            if (needBounds) {
                g_ws.lastBoundsPollTick = now;
                std::unordered_map<uint32_t, SliderBound> bnds;
                if (TraverseSliderTree(td + 0x10u, true, nullptr, nullptr, &bnds) > 0 && bnds.size() >= g_ws.curBounds.size()) {
                    g_ws.curBounds = bnds;
                    g_ws.hasLiveBounds = true;
                }
            }
        } else if (IsValidPtr(g_tuningMapPtr) && g_tuningMapSeq != g_ws.tuningMapSeqAtCarSwitch) {
            liveValCnt = TraverseSliderTree(g_tuningMapPtr, false, &liveMapVals, &liveMapNodes, nullptr);
        }

        uint32_t candCount = 0, candIdx = 0;
        std::string bpSourceLabel;
        uint32_t activeBp = ResolveActiveBp(
            g_ws.activeCanonHash, g_monCtxMode,
            haveSnap ? localSnap.carPtr : 0, haveSnap ? localSnap.ratingPtr : 0,
            liveMapVals, candCount, candIdx, bpSourceLabel,
            carOrModeChanged || ctxChanged
        );
        g_ws.activeBpAddr = activeBp;

        uint32_t reqSeq = g_shm->cmdSeq;
        if (reqSeq != lastCmdHandled) {
            HandleCommand(g_shm->cmdType, g_shm->cmdParam1, g_shm->cmdParam2, g_shm->cmdFParam, liveMapNodes, inTuningUi);
            lastCmdHandled = reqSeq;
            g_shm->cmdAckSeq = reqSeq;
            if (g_shm->cmdType == CMD_RESCAN_BP || g_shm->cmdType == CMD_CYCLE_BP) {
                activeBp = ResolveActiveBp(
                    g_ws.activeCanonHash, g_monCtxMode,
                    haveSnap ? localSnap.carPtr : 0, haveSnap ? localSnap.ratingPtr : 0,
                    liveMapVals, candCount, candIdx, bpSourceLabel
                );
                g_ws.activeBpAddr = activeBp;
            }
        }

        float bpSliders[NUM_SLIDERS] = {}; int bpSliderNz = 0;
        bool hasBpSliders = (activeBp != 0) && ReadSlidersFromBp(activeBp, bpSliders, bpSliderNz);

        uint32_t upgIdx[NUM_UPGRADE_PARTS] = {};
        std::string upgShort[NUM_UPGRADE_PARTS], partsSig;
        uint32_t inspW1[NUM_INSPECT_SLOTS] = {}, inspW2[NUM_INSPECT_SLOTS] = {}, inspIdx[NUM_INSPECT_SLOTS] = {};

        auto itVltCar = g_ws.vltCars.find(g_ws.activeCanonHash);
        const VltCarInfo* vltCar = (itVltCar != g_ws.vltCars.end()) ? &itVltCar->second : nullptr;

        uint64_t upgHash = 1469598103934665603ULL ^ activeBp;
        for (int i = 0; i < NUM_INSPECT_SLOTS; ++i) {
            uint32_t w1 = 0, w2 = 0, idx = 0;
            if (activeBp != 0) {
                uint32_t off = 0x68u + INSPECT_SLOT_DEFS[i].slot * 0x10u;
                w1 = SafeReadU32(activeBp + off + 0x04);
                w2 = SafeReadU32(activeBp + off + 0x08);
                idx = w2 & 0xFFFFu;
            }
            inspW1[i] = w1; inspW2[i] = w2; inspIdx[i] = idx;
            if (i < NUM_UPGRADE_PARTS) {
                upgIdx[i] = idx;
                if (i > 0) partsSig += ",";
                partsSig += std::to_string(idx);
                upgHash = (upgHash ^ w1) * 1099511628211ULL;
                upgHash = (upgHash ^ w2) * 1099511628211ULL;
            }
        }
        if (upgHash != g_ws.lastUpgKeyHash) {
            g_ws.lastUpgKeyHash = upgHash;
            for (int i = 0; i < NUM_UPGRADE_PARTS; ++i) {
                if (activeBp != 0 && IsValidPtr(inspW1[i])) {
                    uint32_t dArr = 0, mCnt = 0, ic = 0, iKey = 0, arr3EF = 0;
                    GetPartPhysicsArrayInfo(inspW1[i], dArr, mCnt);
                    GetInfoColl3EF(inspW1[i], ic, iKey, arr3EF);
                    ShmPartItem tmpIt; FillPartItem(0, 0, arr3EF, inspIdx[i], tmpIt);
                    auto dec = ProStreetMeta::decodeStageAndBrand(
                        INSPECT_SLOT_DEFS[i].slot, inspW2[i], mCnt,
                        tmpIt.hasMeta3EF != 0, tmpIt.exactStage, tmpIt.cfgInStage, tmpIt.brandHash
                    );
                    g_ws.cachedUpgShort[i] = dec.shortStr;
                } else {
                    g_ws.cachedUpgShort[i] = "—";
                }
            }
        }
        for (int i = 0; i < NUM_UPGRADE_PARTS; ++i) {
            upgShort[i] = g_ws.cachedUpgShort[i].empty() ? "—" : g_ws.cachedUpgShort[i];
        }

        std::string bKey = g_ws.activeCanonHash + "|" + std::to_string(g_ws.activeMode) + "|" + partsSig;
        if (inTuningUi && g_ws.hasLiveBounds && !g_ws.curBounds.empty()) {
            g_ws.boundsCacheByKey[bKey] = g_ws.curBounds;
        } else if (partsSig != g_ws.curPartsSig) {
            g_ws.curPartsSig = partsSig;
            auto itB = g_ws.boundsCacheByKey.find(bKey);
            if (itB != g_ws.boundsCacheByKey.end()) { g_ws.curBounds = itB->second; g_ws.hasLiveBounds = true; }
            else { g_ws.curBounds.clear(); g_ws.hasLiveBounds = false; }
        }

        uint32_t sliderSource = (inTuningUi && liveValCnt > 0) ? SLIDER_SRC_TUNINGDATA
                              : (hasBpSliders ? SLIDER_SRC_BLUEPRINT : (liveValCnt > 0 ? SLIDER_SRC_TUNINGMAP : SLIDER_SRC_NONE));
        ShmSlider outSliders[NUM_SLIDERS] = {};
        for (int i = 0; i < NUM_SLIDERS; ++i) {
            ShmSlider& dst = outSliders[i];
            dst.hash = SLIDER_DEFS[i].hash; dst.minBound = -1.0f; dst.maxBound = 1.0f;
            if (sliderSource == SLIDER_SRC_TUNINGDATA) {
                auto it = liveMapVals.find(dst.hash);
                if (it != liveMapVals.end()) { dst.value = it->second; dst.hasValue = 1; }
                else if (hasBpSliders)       { dst.value = bpSliders[i]; dst.hasValue = 1; }
            } else if (sliderSource == SLIDER_SRC_BLUEPRINT) {
                dst.value = bpSliders[i]; dst.hasValue = 1;
            } else if (sliderSource == SLIDER_SRC_TUNINGMAP) {
                auto it = liveMapVals.find(dst.hash);
                dst.value = (it != liveMapVals.end()) ? it->second : 0.0f; dst.hasValue = 1;
            }

            auto itB = g_ws.curBounds.find(dst.hash);
            if (itB != g_ws.curBounds.end()) {
                dst.minBound = itB->second.minB; dst.maxBound = itB->second.maxB; dst.locked = itB->second.locked ? 1 : 0;
            } else if (g_ws.hasLiveBounds || (!hasBpSliders && !inTuningUi)) {
                dst.minBound = 0.0f; dst.maxBound = 0.0f; dst.locked = 1;
            } else {
                int pSlot = SLIDER_DEFS[i].partSlot;
                if (pSlot >= 0 && pSlot < NUM_UPGRADE_PARTS && upgIdx[pSlot] == 0 && std::fabs(dst.value) < 1e-4f) {
                    dst.minBound = 0.0f; dst.maxBound = 0.0f; dst.locked = 1;
                }
            }
            if (!dst.locked && (hasBpSliders || liveMapNodes.count(dst.hash) > 0)) dst.editable = 1;
        }

        // Track slider changes for instant 100Hz boost
        bool slidersMoved = false;
        for (int i = 0; i < NUM_SLIDERS; ++i) {
            if (std::fabs(outSliders[i].value - g_ws.lastSlidersSnap[i]) > 1e-4f) {
                g_ws.lastSlidersSnap[i] = outSliders[i].value;
                slidersMoved = true;
            }
        }
        if (slidersMoved) g_ws.lastActiveActivityTick = now;

        // Count & auto-discover live Career records (FECustomizationRecord) matching activeBp
        uint32_t curBpType0 = (activeBp != 0) ? SafeReadU32(activeBp) : 0u;
        uint32_t curBpMode  = (activeBp != 0) ? SafeReadU32(activeBp + 0x2050) : 0u;
        auto countMatchedCrs = [&](uint32_t& outBest, uint32_t& outBestSeq) -> uint32_t {
            outBest = 0; outBestSeq = 0;
            uint32_t cnt = 0;
            for (auto it = g_ws.custRecPool.begin(); it != g_ws.custRecPool.end(); ) {
                if (!IsLiveCustRec(it->first, 0)) { it = g_ws.custRecPool.erase(it); continue; }
                if (curBpType0 != 0 && it->second.carType0 == curBpType0 &&
                    DoesCustRecMatchBp(it->first, activeBp, curBpType0, curBpMode, nullptr, false)) {
                    cnt++;
                    if (it->second.lastSeq >= outBestSeq) { outBestSeq = it->second.lastSeq; outBest = it->first; }
                }
                ++it;
            }
            return cnt;
        };
        uint32_t bestCr = 0, bestCrSeq = 0;
        uint32_t matchCrCnt = (activeBp != 0) ? countMatchedCrs(bestCr, bestCrSeq) : 0u;
        if (activeBp != 0 && matchCrCnt == 0) {
            if (ScanHeapForCustRecs(activeBp, false) > 0) {
                matchCrCnt = countMatchedCrs(bestCr, bestCrSeq);
            }
        }
        g_ws.activeCustRecAddr = bestCr;

        // Read Autosculpt directly from activeBp + 0x188C (throttled 80ms)
        uint32_t asBase = (activeBp != 0) ? (activeBp + 0x188Cu) : 0u;
        if (IsValidPtr(asBase) && (carOrModeChanged || ctxChanged || (now - g_ws.lastSculptPollTick) >= 80u)) {
            g_ws.lastSculptPollTick = now;
            uint8_t rawAs[AUTOSCULPT_READ_SLOTS * 0x30];
            if (SafeRead(asBase + 4, rawAs, sizeof(rawAs))) {
                for (int s = 0; s < AUTOSCULPT_READ_SLOTS; ++s) {
                    for (int z = 0; z < AUTOSCULPT_ZONES; ++z) {
                        float fv = 0.0f; std::memcpy(&fv, rawAs + s * 0x30 + z * 4, 4);
                        if (!IsFiniteFloat(fv) || fv < -1.5f || fv > 1.5f) fv = 0.0f;
                        g_ws.cachedSculptZones[s][z] = fv;
                    }
                }
            }
        }

        // Fast fingerprint of telemetry so EXE only repaints when something actually changed!
        uint64_t fHash = 1469598103934665603ULL;
        auto mixU32 = [&](uint32_t v) { fHash = (fHash ^ v) * 1099511628211ULL; };
        mixU32(lastSeenHookSeq); mixU32(g_monCtxMode); mixU32(g_raceDayActive);
        mixU32(activeBp); mixU32(bestCr); mixU32(matchCrCnt); mixU32(candCount); mixU32(candIdx); mixU32(g_ws.pinnedBpAddr);
        mixU32(sliderSource); mixU32(g_ws.hasLiveBounds ? 1u : 0u); mixU32(inTuningUi ? 1u : 0u);
        mixU32((uint32_t)g_ws.bpPool.size()); mixU32(asBase);
        for (int i = 0; i < NUM_SLIDERS; ++i) {
            uint32_t vb = 0, nb = 0, xb = 0;
            std::memcpy(&vb, &outSliders[i].value, 4);
            std::memcpy(&nb, &outSliders[i].minBound, 4);
            std::memcpy(&xb, &outSliders[i].maxBound, 4);
            mixU32(vb); mixU32(nb); mixU32(xb);
            mixU32((uint32_t)outSliders[i].locked | ((uint32_t)outSliders[i].editable << 1) | ((uint32_t)outSliders[i].hasValue << 2));
        }
        for (int i = 0; i < NUM_INSPECT_SLOTS; ++i) { mixU32(inspW1[i]); mixU32(inspW2[i]); }
        for (int s = 0; s < AUTOSCULPT_READ_SLOTS; ++s) {
            for (int z = 0; z < 10; ++z) {
                uint32_t zb = 0; std::memcpy(&zb, &g_ws.cachedSculptZones[s][z], 4); mixU32(zb);
            }
        }

        if (fHash != g_ws.lastFastPubHash || (now - g_ws.lastPublishTick) >= 500u) {
        g_ws.lastFastPubHash = fHash;
        g_ws.lastPublishTick = now;
        // Publish Fast State
        g_shm->fastSeq = g_shm->fastSeq + 1;
        g_shm->hookSnapSeq = lastSeenHookSeq; g_shm->hasValidSnap = haveSnap ? 1u : 0u;
        uint32_t effCtxMode = g_monCtxMode;
        if (inTuningUi) {
            effCtxMode = CTX_TUNING_UI;
        } else if (effCtxMode == CTX_CARLOT && (activeBp != 0 || matchCrCnt > 0)) {
            effCtxMode = CTX_SHOP; // Стайлинг / Магазин деталей игрока (есть живой BP или CareerRecord)
        }
        g_shm->ctxMode = effCtxMode; g_shm->raceDayActive = g_raceDayActive;
        g_shm->carMode = haveSnap ? localSnap.carMode : 0u;
        if (haveSnap) {
            std::memcpy(g_shm->curVals, localSnap.cur, sizeof(g_shm->curVals));
            std::memcpy(g_shm->chainKeys, localSnap.chainKeys, sizeof(g_shm->chainKeys));
            std::memcpy(g_shm->chainSrcs, localSnap.chainSrcs, sizeof(g_shm->chainSrcs));
            g_shm->pvehCollPtr = localSnap.pvehCollPtr; g_shm->carPtr = localSnap.carPtr; g_shm->ratingPtr = localSnap.ratingPtr;
        }
        std::strncpy(g_shm->baseHash, baseHash.c_str(), sizeof(g_shm->baseHash) - 1); g_shm->baseHash[sizeof(g_shm->baseHash) - 1] = '\0';
        std::strncpy(g_shm->varHash, varHash.c_str(), sizeof(g_shm->varHash) - 1); g_shm->varHash[sizeof(g_shm->varHash) - 1] = '\0';
        std::strncpy(g_shm->chainStr, chainStr.c_str(), sizeof(g_shm->chainStr) - 1); g_shm->chainStr[sizeof(g_shm->chainStr) - 1] = '\0';

        g_shm->activeBpAddr = activeBp; g_shm->activeBpCarType = (activeBp != 0) ? GetBpCarTypeId(activeBp) : 0u;
        g_shm->activeCustRecAddr = bestCr; g_shm->custRecCount = matchCrCnt;
        g_shm->bpMatchesActive = (activeBp != 0) ? 1u : 0u; g_shm->bpCandCount = candCount; g_shm->bpCandIndex = candIdx;
        g_shm->bpPinnedAddr = g_ws.pinnedBpAddr; g_shm->vltReady = g_ws.vltReady ? 1u : 0u;
        g_shm->totalBpInHeap = static_cast<uint32_t>(g_ws.bpPool.size());
        std::strncpy(g_shm->bpSourceLabel, bpSourceLabel.c_str(), sizeof(g_shm->bpSourceLabel) - 1);
        g_shm->bpSourceLabel[sizeof(g_shm->bpSourceLabel) - 1] = '\0';

        const char* detSrc = (activeBp != 0) ? "Blueprint + VLT" : (vltCar ? "VLT напрямую (без BP)" : "Файл VLT-базы");
        std::strncpy(g_shm->detailSourceLabel, detSrc, sizeof(g_shm->detailSourceLabel) - 1);
        g_shm->detailSourceLabel[sizeof(g_shm->detailSourceLabel) - 1] = '\0';
        if (vltCar) std::strncpy(g_shm->vltRawCarName, vltCar->rawName.c_str(), sizeof(g_shm->vltRawCarName) - 1);
        else g_shm->vltRawCarName[0] = '\0';

        g_shm->sliderSource = sliderSource; g_shm->hasLiveBounds = g_ws.hasLiveBounds ? 1 : 0; g_shm->inTuningUi = inTuningUi ? 1 : 0;
        std::memcpy(g_shm->sliders, outSliders, sizeof(outSliders));

        g_shm->autosculptBasePtr = asBase;
        if (activeBp != 0) {
            uint32_t bW2 = inspW2[7], hW2 = inspW2[8], sW2 = inspW2[9];
            uint32_t bIdx = bW2 & 0xFFFFu, hIdx = hW2 & 0xFFFFu, sIdx = sW2 & 0xFFFFu;
            bool isWide = ((bW2 >> 28) & 1u) != 0;
            std::string bn = (bIdx == 0) ? "Сток" : ((isWide ? "Widebody #" : "Обвес #") + std::to_string(bIdx));
            std::string hn = (hIdx == 0) ? "Сток" : ("Капот #" + std::to_string(hIdx));
            std::string sn = (sIdx == 0) ? "Сток" : ("Спойлер #" + std::to_string(sIdx));
            std::strncpy(g_shm->bodyName, bn.c_str(), sizeof(g_shm->bodyName) - 1);
            std::strncpy(g_shm->hoodName, hn.c_str(), sizeof(g_shm->hoodName) - 1);
            std::strncpy(g_shm->spoilerName, sn.c_str(), sizeof(g_shm->spoilerName) - 1);
        } else {
            std::strncpy(g_shm->bodyName, "—", sizeof(g_shm->bodyName) - 1);
            std::strncpy(g_shm->hoodName, "—", sizeof(g_shm->hoodName) - 1);
            std::strncpy(g_shm->spoilerName, "—", sizeof(g_shm->spoilerName) - 1);
        }
        std::memcpy(g_shm->autosculptZones, g_ws.cachedSculptZones, sizeof(g_ws.cachedSculptZones));
        for (int i = 0; i < NUM_UPGRADE_PARTS; ++i) {
            g_shm->upgradeIdx[i] = upgIdx[i];
            std::strncpy(g_shm->upgradeShort[i], upgShort[i].c_str(), sizeof(g_shm->upgradeShort[i]) - 1);
            g_shm->upgradeShort[i][sizeof(g_shm->upgradeShort[i]) - 1] = '\0';
        }
        for (int i = 0; i < NUM_INSPECT_SLOTS; ++i) {
            g_shm->inspectW1[i] = inspW1[i]; g_shm->inspectW2[i] = inspW2[i]; g_shm->inspectIdx[i] = inspIdx[i];
        }
        g_shm->fastSeq = g_shm->fastSeq + 1;
        }

        // Update Catalog only when car or w1 changes
        uint32_t effW1[NUM_INSPECT_SLOTS] = {};
        uint32_t catHash = 2166136261u;
        for (char ch : g_ws.activeCanonHash) catHash = (catHash ^ (uint8_t)ch) * 16777619u;
        for (int i = 0; i < NUM_INSPECT_SLOTS; ++i) {
            uint32_t w1 = inspW1[i], vltW1 = vltCar ? vltCar->slotPtr[i] : 0u;
            if (!IsValidPtr(w1) && IsValidPtr(vltW1)) w1 = vltW1;
            else if (IsValidPtr(w1) && IsValidPtr(vltW1)) {
                uint32_t dArr = 0, bpCnt = 0; GetPartPhysicsArrayInfo(w1, dArr, bpCnt);
                if (bpCnt <= 1 && vltCar->slotMCnt[i] > bpCnt) w1 = vltW1;
            }
            effW1[i] = w1;
            catHash = (catHash ^ w1) * 16777619u;
        }
        if (!g_ws.activeCanonHash.empty() && catHash != g_ws.lastCatalogKeyHash) {
            g_ws.lastCatalogKeyHash = catHash;
            g_shm->catalogSeq = g_shm->catalogSeq + 1;
            std::strncpy(g_shm->catalogCarHash, g_ws.activeCanonHash.c_str(), sizeof(g_shm->catalogCarHash) - 1);
            g_shm->catalogCarHash[sizeof(g_shm->catalogCarHash) - 1] = '\0';
            for (int i = 0; i < NUM_INSPECT_SLOTS; ++i) {
                ShmInspectSlotCatalog& sc = g_shm->inspectSlots[i];
                std::memset(&sc, 0, sizeof(sc));
                sc.meta.slotNum = INSPECT_SLOT_DEFS[i].slot;
                sc.meta.w1 = effW1[i]; sc.meta.bpW1 = inspW1[i]; sc.meta.vltW1 = vltCar ? vltCar->slotPtr[i] : 0u;
                sc.meta.w2 = inspW2[i]; sc.meta.idx = inspIdx[i];
                if (IsValidPtr(effW1[i])) {
                    sc.meta.mKey = SafeReadU32(effW1[i] + 0x10); sc.meta.mLayout = SafeReadU32(effW1[i] + 0x18);
                    GetPartPhysicsArrayInfo(effW1[i], sc.meta.arr6B8, sc.meta.mCount);
                    if (sc.meta.mCount > MAX_PART_ITEMS) sc.meta.mCount = MAX_PART_ITEMS;
                    if (IsValidPtr(sc.meta.mLayout)) {
                        std::string sC = TryReadAsciiSafe(SafeReadU32(sc.meta.mLayout + 0x0C));
                        std::string s10 = TryReadAsciiSafe(SafeReadU32(sc.meta.mLayout + 0x10));
                        std::string lay = (!sC.empty() && !s10.empty() && sC != s10) ? (sC + " / " + s10) : (!sC.empty() ? sC : s10);
                        std::strncpy(sc.meta.layoutStr, lay.c_str(), sizeof(sc.meta.layoutStr) - 1);
                    }
                    GetInfoColl3EF(effW1[i], sc.meta.infoColl, sc.meta.infoKey, sc.meta.arr3EF);
                    for (uint32_t k = 0; k < sc.meta.mCount; ++k) FillPartItem(sc.meta.arr6B8, sc.meta.mCount, sc.meta.arr3EF, k, sc.items[k]);
                    if (sc.meta.slotNum == 7 || sc.meta.slotNum == 78 || sc.meta.slotNum == 90) {
                        uint32_t mTab = SafeReadU32(effW1[i] + 0x00), nEnt = SafeReadU32(effW1[i] + 0x04) & 0xFFFFu;
                        if (IsValidPtr(mTab) && nEnt > 0 && nEnt <= 12) {
                            std::string vis;
                            for (uint32_t a = 0; a < nEnt; ++a) {
                                uint32_t ak = SafeReadU32(mTab + a * 12), av = SafeReadU32(mTab + a * 12 + 4);
                                if (ak == 0x6B868DDFu) continue;
                                char piece[48];
                                if (IsValidPtr(av)) std::snprintf(piece, sizeof(piece), "%08X=[%08X:%08X]", ak, SafeReadU32(av), SafeReadU32(av + 4));
                                else std::snprintf(piece, sizeof(piece), "%08X=%08X", ak, av);
                                if (!vis.empty()) vis += ", ";
                                vis += piece;
                                if (vis.size() >= sizeof(sc.meta.visAttrs) - 24) break;
                            }
                            std::strncpy(sc.meta.visAttrs, vis.c_str(), sizeof(sc.meta.visAttrs) - 1);
                        }
                    }
                }
            }
            g_shm->catalogSeq = g_shm->catalogSeq + 1;
        }

        bool hotMode = inTuningUi || ((now - g_ws.lastActiveActivityTick) < 2500u);
        DWORD waitMs = hotMode ? 10u : 20u;
        if (g_shm->unloadRequested == 0 && g_shm->cmdSeq == lastCmdHandled) {
            if (g_hCmdEvent) WaitForSingleObject(g_hCmdEvent, waitMs);
            else Sleep(waitMs);
        }
    }

    if (g_hMsgHook) { UnhookWindowsHookEx(g_hMsgHook); g_hMsgHook = nullptr; }
    RestoreAllHooks();
    Sleep(60);
    if (g_hMtDoneEvt) { CloseHandle(g_hMtDoneEvt); g_hMtDoneEvt = nullptr; }
    if (g_trampMem) { VirtualFree(g_trampMem, 0, MEM_RELEASE); g_trampMem = nullptr; }
    if (g_shm) { g_shm->dllUnloaded = 1; UnmapViewOfFile(g_shm); g_shm = nullptr; }
    if (g_hCmdEvent) { CloseHandle(g_hCmdEvent); g_hCmdEvent = nullptr; }
    if (g_hMap) { CloseHandle(g_hMap); g_hMap = nullptr; }
    FreeLibraryAndExitThread(g_hSelf, 0);
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID /*lpReserved*/) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_hSelf = hModule;
        DisableThreadLibraryCalls(hModule);
        g_nfsBase = reinterpret_cast<uint32_t>(GetModuleHandleA(nullptr));
        if (g_nfsBase == 0) g_nfsBase = 0x00400000u;

        g_hMap = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(ShmSharedState), SHM_NAME);
        if (!g_hMap) return FALSE;
        g_hCmdEvent = CreateEventA(nullptr, FALSE, FALSE, SHM_CMD_EVENT_NAME);
        g_shm = reinterpret_cast<ShmSharedState*>(MapViewOfFile(g_hMap, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(ShmSharedState)));
        if (!g_shm) { CloseHandle(g_hMap); g_hMap = nullptr; return FALSE; }
        if (g_shm->magic != SHM_MAGIC || g_shm->version != SHM_VERSION) {
            std::memset(g_shm, 0, sizeof(ShmSharedState));
            g_shm->magic = SHM_MAGIC; g_shm->version = SHM_VERSION;
        }
        g_shm->dllPid = GetCurrentProcessId();
        g_shm->unloadRequested = 0;
        g_shm->dllUnloaded = 0;

        if (!InstallAllHooks()) {
            UnmapViewOfFile(g_shm); g_shm = nullptr;
            CloseHandle(g_hMap); g_hMap = nullptr;
            return FALSE;
        }
        g_hWorker = CreateThread(nullptr, 0, WorkerThreadProc, nullptr, 0, nullptr);
        if (g_hWorker) CloseHandle(g_hWorker);
    } else if (reason == DLL_PROCESS_DETACH) {
        g_stopWorker.store(true);
        RestoreAllHooks();
    }
    return TRUE;
}
