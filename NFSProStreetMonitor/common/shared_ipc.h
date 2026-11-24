#pragma once
#include <cstdint>
#include <cstring>

// ============================================================================
// Shared IPC Contract between NFSProStreetMonitor.exe (Win32) and prostreet_hooks.dll (Win32)
// Both binaries MUST be compiled as 32-bit x86 (Win32) to match nfs.exe!
// ============================================================================

static constexpr uint32_t SHM_MAGIC   = 0x4E465350u; // 'NFSP'
static constexpr uint32_t SHM_VERSION = 1u;
static constexpr const char* SHM_NAME = "Local\\NFSProStreetMonitorSHM_v1";

static constexpr int NUM_SLIDERS       = 30;
static constexpr int NUM_UPGRADE_PARTS = 8;
static constexpr int NUM_INSPECT_SLOTS = 10;
static constexpr int MAX_PART_ITEMS    = 32;
static constexpr int AUTOSCULPT_READ_SLOTS = 14;
static constexpr int AUTOSCULPT_ZONES  = 11;

static constexpr uint32_t SLIDER_CLASS_STD = 0xC36CD979u; // physics_tuning_slider
static constexpr uint32_t SLIDER_CLASS_CAM = 0x9F961BDCu; // cam_timing

enum SliderSource : uint32_t {
    SLIDER_SRC_NONE       = 0,
    SLIDER_SRC_TUNINGDATA = 1,
    SLIDER_SRC_BLUEPRINT  = 2,
    SLIDER_SRC_TUNINGMAP  = 3,
};

enum ContextMode : uint32_t {
    CTX_GARAGE       = 0,
    CTX_CARLOT       = 1,
    CTX_TUNING_UI    = 2,
    CTX_RACEDAY_PLAY = 3,
    CTX_RACEDAY_HUB  = 4,
};

enum IpcCommandType : uint32_t {
    CMD_NONE         = 0,
    CMD_SET_SLIDER   = 1, // param1 = sliderHash, fparam = newVal (-1..+1)
    CMD_INSTALL_PART = 2, // param1 = inspectRowIdx (0..9), param2 = targetIdx
    CMD_RESCAN_BP    = 3, // force immediate in-process Blueprint heap scan
    CMD_CYCLE_BP     = 4, // param1 = 1 (next), 2 (prev), 0 (auto)
    CMD_SYNC_KNOWN_CAR_TYPE = 5, // param1 = canonHashU32, param2 = bpCarType
};

#pragma pack(push, 1)

struct ShmSlider {
    uint32_t hash;
    float    value;
    float    minBound;
    float    maxBound;
    uint8_t  locked;
    uint8_t  editable;
    uint8_t  hasValue;
    uint8_t  pad;
};

struct ShmPartItem {
    uint32_t physKey;
    uint32_t physCol;
    uint32_t parKey;
    uint32_t cost;
    uint32_t brandHash;
    uint8_t  exactStage;
    uint8_t  cfgInStage;
    uint8_t  hasMeta3EF;
    uint8_t  hasPhys;
    uint32_t dws[14];
    char     strs[80];
    char     physAttrs[176];
};

struct ShmInspectSlotMeta {
    uint32_t slotNum;
    uint32_t w1;
    uint32_t bpW1;
    uint32_t vltW1;
    uint32_t w2;
    uint32_t idx;
    uint32_t mCount;
    uint32_t mKey;
    uint32_t mLayout;
    uint32_t arr6B8;
    uint32_t infoColl;
    uint32_t infoKey;
    uint32_t arr3EF;
    char     layoutStr[48];
    char     visAttrs[128];
};

struct ShmInspectSlotCatalog {
    ShmInspectSlotMeta meta;
    ShmPartItem        items[MAX_PART_ITEMS];
};

struct ShmSharedState {
    uint32_t magic;
    uint32_t version;
    volatile uint32_t dllPid;
    volatile uint32_t dllHeartbeat;
    volatile uint32_t unloadRequested;
    volatile uint32_t dllUnloaded;

    // Seqlock for fast state (odd = DLL writing, even = consistent)
    volatile uint32_t fastSeq;

    // --- Fast telemetry (updated ~60ms) ---
    uint32_t hookSnapSeq;
    uint32_t hasValidSnap;
    uint32_t ctxMode;
    uint32_t raceDayActive;
    uint32_t carMode;
    float    curVals[21];
    uint32_t chainKeys[8];
    uint32_t chainSrcs[8];
    char     baseHash[12];
    char     varHash[12];
    char     chainStr[96];

    uint32_t pvehCollPtr;
    uint32_t carPtr;
    uint32_t ratingPtr;

    // Blueprint status
    uint32_t activeBpAddr;
    uint32_t activeBpCarType;
    uint32_t bpMatchesActive;
    uint32_t bpCandCount;
    uint32_t bpCandIndex;
    uint32_t bpPinnedAddr;
    uint32_t vltReady;
    uint32_t totalBpInHeap;
    char     bpSourceLabel[64];
    char     detailSourceLabel[64];
    char     vltRawCarName[64];

    // 30 Sliders
    uint32_t sliderSource;
    uint8_t  hasLiveBounds;
    uint8_t  inTuningUi;
    uint8_t  padS[2];
    ShmSlider sliders[NUM_SLIDERS];

    // Autosculpt
    uint32_t autosculptBasePtr;
    char     bodyName[32];
    char     hoodName[32];
    char     spoilerName[32];
    float    autosculptZones[AUTOSCULPT_READ_SLOTS][AUTOSCULPT_ZONES];

    // 8 Upgrade parts summary
    uint32_t upgradeIdx[NUM_UPGRADE_PARTS];
    char     upgradeShort[NUM_UPGRADE_PARTS][24];

    // Fast slot w1/w2 mirror (so EXE immediately sees installed w2 even between catalog updates)
    uint32_t inspectW1[NUM_INSPECT_SLOTS];
    uint32_t inspectW2[NUM_INSPECT_SLOTS];
    uint32_t inspectIdx[NUM_INSPECT_SLOTS];

    // --- Catalog state (seqlock, updated only when car or w1 changes) ---
    volatile uint32_t catalogSeq;
    char     catalogCarHash[12];
    ShmInspectSlotCatalog inspectSlots[NUM_INSPECT_SLOTS];

    // --- Command Channel (EXE -> DLL) ---
    volatile uint32_t cmdSeq;
    volatile uint32_t cmdAckSeq;
    uint32_t cmdType;
    uint32_t cmdParam1;
    uint32_t cmdParam2;
    float    cmdFParam;
    uint32_t cmdResultOk;
    char     cmdResultMsg[160];
};

#pragma pack(pop)

// Static metadata tables shared by both DLL and EXE
struct SliderDef {
    uint32_t    hash;
    int         bpIdx;     // 0..31 inside Blueprint (+0x205C + bpIdx * 0x0C)
    int         groupIdx;  // 0=susp, 1=engine, 2=drive, 3=brakes
    int         partSlot;  // upgradePartIdx index (0=eng,1=drv,2=ind,3=susp,4=brk,5=tires,6=n2o)
    const char* nameUtf8;
};

static constexpr SliderDef SLIDER_DEFS[NUM_SLIDERS] = {
    // Suspension (left column: 0..14)
    { 0xEA25F3C1u, 12, 0, 3, "Сжатие перед. аморт." },
    { 0x60E59FC8u, 21, 0, 3, "Сжатие зад. аморт." },
    { 0x29AAE9F3u, 13, 0, 3, "Отбой перед. аморт." },
    { 0x13E40342u, 22, 0, 3, "Отбой зад. аморт." },
    { 0x86A690B0u, 14, 0, 3, "Жёстк. перед. пружин" },
    { 0x6040A459u, 23, 0, 3, "Жёстк. зад. пружин" },
    { 0xE5853435u, 10, 0, 3, "Клиренс" },
    { 0xEE0C7CF9u, 11, 0, 3, "Перед. стабилизатор" },
    { 0x699B5223u, 20, 0, 3, "Зад. стабилизатор" },
    { 0x2EA305BFu, 15, 0, 5, "Давл. перед. шин" },
    { 0x6CE2FFF8u, 24, 0, 5, "Давл. зад. шин" },
    { 0x108C5F95u, 30, 0, 3, "Угол схода" },
    { 0x18E535F2u,  2, 0, 3, "Угол развала" },
    { 0xF0D0712Au,  3, 0, 3, "Угол продол. накл." },
    { 0x95537CC5u, 28, 0, 3, "Чуств. руля" },
    // Engine (right top: 15..19)
    { 0xF0F8DE03u, 31, 1, 0, "Фаза газораспределения" },
    { 0xAA53F439u, 27, 1, 2, "Начальная фаза" },
    { 0x3E131D27u,  5, 1, 2, "Конечная фаза" },
    { 0x5E49553Au, 18, 1, 6, "Давление N2O" },
    { 0x12ACE656u, 17, 1, 6, "Скорость N2O" },
    // Drivetrain (right mid: 20..26)
    { 0xCE5B38F3u,  8, 2, 1, "1-я передача" },
    { 0xE70E7A91u, 25, 2, 1, "2-я передача" },
    { 0x42EE6910u, 29, 2, 1, "3-я передача" },
    { 0xF06671BBu,  9, 2, 1, "4-я передача" },
    { 0xAAF35F7Fu,  6, 2, 1, "5-я передача" },
    { 0x76FC7A1Au, 26, 2, 1, "6-я передача" },
    { 0x25CEB125u,  7, 2, 1, "Главная пара" },
    // Brakes (right bot: 27..29)
    { 0x46CF952Bu,  0, 3, 4, "Баланс тормозов" },
    { 0xEF495559u,  1, 3, 4, "Давление тормозов" },
    { 0xB933C561u, 16, 3, 4, "Ручной тормоз" },
};

struct InspectSlotDef {
    uint32_t    slot;
    const char* key;
    const char* nameUtf8;
};

static constexpr InspectSlotDef INSPECT_SLOT_DEFS[NUM_INSPECT_SLOTS] = {
    { 123, "engine",     "Двигатель" },
    { 122, "drivetrain", "Трансмиссия" },
    { 124, "induction",  "Наддув" },
    { 146, "suspension", "Подвеска" },
    {   9, "brakes",     "Тормоза" },
    { 148, "tires",      "Шины" },
    { 130, "nitrous",    "Закись азота" },
    {   7, "body",       "Кузов" },
    {  78, "hood",       "Капот" },
    {  90, "spoiler",    "Спойлер" },
};

static inline int findSliderIndexByHash(uint32_t hash) {
    for (int i = 0; i < NUM_SLIDERS; ++i) {
        if (SLIDER_DEFS[i].hash == hash) return i;
    }
    return -1;
}

static inline const char* getParKeyName(uint32_t pk) {
    switch (pk) {
        case 0xC0DF5AC1u: return "visual";
        case 0x36350867u: return "brakes";
        case 0x028CD6C8u: return "drivetrain";
        case 0xF1F5FBC7u: return "engine";
        case 0xFFA02101u: return "forced_induction";
        case 0x94759B10u: return "nitrous";
        case 0xB3ED1642u: return "suspension";
        case 0xBD38D1CAu: return "tires";
        default: return nullptr;
    }
}
