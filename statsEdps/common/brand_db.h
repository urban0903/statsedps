#pragma once
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ProStreetMeta {

inline uint32_t calcBHash(const std::string& s) {
    uint32_t h = 0xFFFFFFFFu;
    for (unsigned char ch : s) {
        h = h * 33u + static_cast<uint32_t>(ch);
    }
    return h;
}

inline std::string toHex8(uint32_t v) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%08X", v);
    return std::string(buf);
}

inline std::string toUpperAscii(std::string s) {
    for (char& c : s) c = (char)std::toupper((unsigned char)c);
    return s;
}

inline std::string toLowerAscii(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

inline std::string canonicalCarHash(const std::string& h) {
    if (h == "18359DAE") return "5A7F9439"; // PORSCHE 911 TURBO (997TT)
    if (h == "C5A6CC40") return "46118CE4"; // CHEVROLET CORVETTE C6 (CORVETTE)
    return h;
}

inline std::string normalizeCarName(const std::string& raw) {
    if (raw.empty()) return "";
    std::string up = toUpperAscii(raw);
    if (up == "997TT")      return "PORSCHE 911 TURBO";
    if (up == "CORVETTE")   return "CHEVROLET CORVETTE C6";
    if (up == "CAYMANS")    return "PORSCHE CAYMAN S";
    if (up == "SKYLINER35") return "NISSAN GT-R (R35)";
    return raw;
}

inline bool isRawHex8(const std::string& s) {
    if (s.size() != 8) return false;
    for (char c : s) {
        if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'))) return false;
    }
    return true;
}

inline bool isRootVltHash(const std::string& h) {
    return h == "EEC2271A" || h == "A6ABC921" || h == "81A7A3E5" ||
           h == "E3DF4BFD" || h == "A13753EB" || h == "1744CF1A" || h == "4A97EC8F";
}

inline const std::unordered_set<std::string>& driftEligibleNames() {
    static const std::unordered_set<std::string> s = {
        "ACURA NSX", "ASTON MARTIN DB9", "ASTON MARTIN DBR9",
        "BMW M3 E46", "BMW M3 E92", "BMW Z4 M COUPE", "CADILLAC CTS-V",
        "CHEVROLET CAMARO CONCEPT", "CHEVROLET CAMARO SS", "CHEVROLET CHEVELLE SS",
        "CHEVROLET CORVETTE '67", "CHEVROLET CORVETTE C6", "CHEVROLET CORVETTE Z06",
        "DODGE CHALLENGER CONCEPT", "DODGE CHALLENGER R/T", "DODGE CHARGER R/T", "DODGE VIPER SRT10",
        "FORD GT", "FORD MUSTANG GT", "FORD MUSTANG GT '03", "HONDA S2000", "INFINITI G35 (V35)",
        "KOENIGSEGG CCX", "LEXUS IS350", "LOTUS ELISE", "MAZDA RX-7", "MAZDA RX-8", "MCLAREN F1",
        "MERCEDES-BENZ SL65 AMG", "NISSAN 240SX (S13)", "NISSAN 350Z (Z33)", "NISSAN SILVIA (S15)",
        "PAGANI ZONDA F", "PLYMOUTH HEMI CUDA", "PLYMOUTH ROAD RUNNER",
        "PONTIAC GTO", "PONTIAC GTO '65", "PONTIAC SOLSTICE GXP",
        "PORSCHE 911 GT2", "PORSCHE 911 GT3", "PORSCHE 911 GT3 RS", "PORSCHE CARRERA GT", "PORSCHE CAYMAN S",
        "SHELBY GT500", "SHELBY GT500 '67", "TOYOTA COROLLA GTS (AE86)", "TOYOTA SUPRA"
    };
    return s;
}

inline const std::unordered_map<std::string, std::string>& builtinBrandNames() {
    static std::unordered_map<std::string, std::string> db;
    if (!db.empty()) return db;

    db["DAC3D6AE"] = "Серийные";
    db["7F0BA498"] = "Серийные";

    struct BrandEntry { const char* disp; std::vector<const char*> keys; };
    static const std::vector<BrandEntry> known = {
        { "Серийные",        { "STOCK", "OEM", "FACTORY", "GENERIC" } },
        { "JE Pistons",      { "JE_PISTONS", "JEPISTONS", "JE" } },
        { "Carrillo",        { "CARRILLO" } },
        { "CP Pistons",      { "CP_PISTONS", "CPPISTONS", "CP" } },
        { "Cosworth",        { "COSWORTH" } },
        { "Crower",          { "CROWER", "BRIAN_CROWER" } },
        { "Darton",          { "DARTON", "DART" } },
        { "Edelbrock",       { "EDELBROCK" } },
        { "Ferrea",          { "FERREA" } },
        { "HKS",             { "HKS" } },
        { "JUN",             { "JUN" } },
        { "Skunk2",          { "SKUNK2", "SKUNK_2" } },
        { "Toda",            { "TODA" } },
        { "Wiseco",          { "WISECO" } },
        { "AEM",             { "AEM" } },
        { "Comp Cams",       { "COMP_CAMS", "COMPCAMS" } },
        { "Crane Cams",      { "CRANE", "CRANE_CAMS", "CRANECAMS" } },
        { "Holley",          { "HOLLEY" } },
        { "Lunati",          { "LUNATI" } },
        { "Manley",          { "MANLEY" } },
        { "MSD",             { "MSD" } },
        { "NGK",             { "NGK" } },
        { "Eagle",           { "EAGLE" } },
        { "ARP",             { "ARP" } },
        { "Scat",            { "SCAT" } },
        { "Aeromotive",      { "AEROMOTIVE" } },
        { "Castrol",         { "CASTROL" } },
        { "B&M",             { "BM", "B_M", "BANDM", "B_AND_M" } },
        { "ACT",             { "ACT" } },
        { "Centerforce",     { "CENTERFORCE", "CENTER_FORCE" } },
        { "Clutch Masters",  { "CLUTCH_MASTERS", "CLUTCHMASTERS", "CLUTCHMASTER" } },
        { "Exedy",           { "EXEDY" } },
        { "Fidanza",         { "FIDANZA" } },
        { "OS Giken",        { "OS_GIKEN", "OSGIKEN" } },
        { "Quaife",          { "QUAIFE" } },
        { "SPEC",            { "SPEC" } },
        { "Tilton",          { "TILTON" } },
        { "Tremec",          { "TREMEC" } },
        { "Hurst",           { "HURST" } },
        { "McLeod",          { "MCLEOD" } },
        { "Richmond",        { "RICHMOND" } },
        { "Eaton",           { "EATON" } },
        { "Kaaz",            { "KAAZ" } },
        { "Torsen",          { "TORSEN" } },
        { "TCI",             { "TCI" } },
        { "G-Force",         { "GFORCE", "G_FORCE" } },
        { "Moroso",          { "MOROSO" } },
        { "GReddy",          { "GREDDY", "TRUST" } },
        { "Garrett",         { "GARRETT" } },
        { "Turbonetics",     { "TURBONETICS" } },
        { "A'PEXi",          { "APEXI" } },
        { "Apex",            { "APEX" } },
        { "Blitz",           { "BLITZ" } },
        { "Borla",           { "BORLA" } },
        { "Jackson Racing",  { "JACKSON_RACING", "JACKSONRACING", "JACKSON" } },
        { "K&N",             { "KN", "K_N", "KANDN", "K_AND_N" } },
        { "MagnaFlow",       { "MAGNAFLOW", "MAGNA_FLOW" } },
        { "Paxton",          { "PAXTON" } },
        { "ProCharger",      { "PROCHARGER", "PRO_CHARGER" } },
        { "Vortech",         { "VORTECH" } },
        { "Weiand",          { "WEIAND" } },
        { "Whipple",         { "WHIPPLE" } },
        { "Injen",           { "INJEN" } },
        { "DC Sports",       { "DC_SPORTS", "DCSPORTS" } },
        { "Flowmaster",      { "FLOWMASTER", "FLOW_MASTER" } },
        { "Gibson",          { "GIBSON" } },
        { "Corsa",           { "CORSA" } },
        { "Bassani",         { "BASSANI" } },
        { "Fujitsubo",       { "FUJITSUBO" } },
        { "Tanabe",          { "TANABE" } },
        { "TiAL",            { "TIAL" } },
        { "Turbosmart",      { "TURBOSMART" } },
        { "Spearco",         { "SPEARCO" } },
        { "Koyo",            { "KOYO" } },
        { "Mishimoto",       { "MISHIMOTO" } },
        { "Cobb",            { "COBB" } },
        { "Perrin",          { "PERRIN" } },
        { "Stillen",         { "STILLEN" } },
        { "Full-Race",       { "FULLRACE", "FULL_RACE" } },
        { "XS Engineering",  { "XS", "XS_ENGINEERING" } },
        { "NOS",             { "NOS" } },
        { "Nitrous Express", { "NITROUS_EXPRESS", "NITROUSEXPRESS", "NX" } },
        { "ZEX",             { "ZEX" } },
        { "Venom",           { "VENOM" } },
        { "Cold Fusion",     { "COLD_FUSION", "COLDFUSION" } },
        { "Brembo",          { "BREMBO" } },
        { "Baer",            { "BAER" } },
        { "Wilwood",         { "WILWOOD" } },
        { "StopTech",        { "STOPTECH", "STOP_TECH" } },
        { "AP Racing",       { "AP_RACING", "APRACING" } },
        { "EBC",             { "EBC", "EBC_BRAKES" } },
        { "Hawk",            { "HAWK" } },
        { "PowerSlot",       { "POWERSLOT", "POWER_SLOT" } },
        { "Rotora",          { "ROTORA" } },
        { "Project Mu",      { "PROJECT_MU", "PROJECTMU" } },
        { "Endless",         { "ENDLESS" } },
        { "Alcon",           { "ALCON" } },
        { "SSBC",            { "SSBC" } },
        { "Bilstein",        { "BILSTEIN" } },
        { "Eibach",          { "EIBACH" } },
        { "H&R",             { "HR", "H_R", "HANDR", "H_AND_R" } },
        { "Koni",            { "KONI" } },
        { "KW",              { "KW", "K_W" } },
        { "Ohlins",          { "OHLINS" } },
        { "Progress",        { "PROGRESS" } },
        { "Tein",            { "TEIN" } },
        { "Tokico",          { "TOKICO" } },
        { "Whiteline",       { "WHITELINE", "WHITE_LINE" } },
        { "Hotchkis",        { "HOTCHKIS" } },
        { "QA1",             { "QA1" } },
        { "Cusco",           { "CUSCO" } },
        { "Neuspeed",        { "NEUSPEED" } },
        { "Ground Control",  { "GROUND_CONTROL", "GROUNDCONTROL" } },
        { "Megan Racing",    { "MEGAN_RACING", "MEGANRACING", "MEGAN" } },
        { "JIC Magic",       { "JIC", "JIC_MAGIC", "JICMAGIC" } },
        { "KYB",             { "KYB" } },
        { "Sachs",           { "SACHS" } },
        { "Vogtland",        { "VOGTLAND" } },
        { "Intrax",          { "INTRAX" } },
        { "Sprint",          { "SPRINT" } },
        { "Stance",          { "STANCE" } },
        { "BFGoodrich",      { "BFGOODRICH", "BF_GOODRICH", "BFG" } },
        { "Bridgestone",     { "BRIDGESTONE" } },
        { "Dunlop",          { "DUNLOP" } },
        { "Falken",          { "FALKEN" } },
        { "Goodyear",        { "GOODYEAR" } },
        { "Hankook",         { "HANKOOK" } },
        { "Kumho",           { "KUMHO" } },
        { "Michelin",        { "MICHELIN" } },
        { "Nitto",           { "NITTO" } },
        { "Pirelli",         { "PIRELLI" } },
        { "Toyo",            { "TOYO" } },
        { "Yokohama",        { "YOKOHAMA" } },
        { "Hoosier",         { "HOOSIER" } },
        { "Mickey Thompson", { "MTHOMPSON", "MICKEY_THOMPSON", "MICKEYTHOMPSON", "MT" } },
        { "Stratospeed",     { "STRATOSPEED", "STRATO_SPEED" } },
        { "Gentera",         { "GENTERA", "GENTERRA" } },
        { "AutoSculpt",      { "AUTOSCULPT", "AUTO_SCULPT" } },
        { "Seibon",          { "SEIBON" } },
        { "VIS Racing",      { "VIS", "VIS_RACING", "VISRACING" } },
        { "APR",             { "APR", "APR_PERFORMANCE" } },
        { "C-West",          { "CWEST", "C_WEST" } },
        { "VeilSide",        { "VEILSIDE" } },
        { "Bomex",           { "BOMEX" } },
        { "ChargeSpeed",     { "CHARGESPEED", "CHARGE_SPEED" } },
        { "Ings",            { "INGS" } },
        { "Kaminari",        { "KAMINARI" } },
        { "Wings West",      { "WINGSWEST", "WINGS_WEST" } },
        { "3D Carbon",       { "3DCARBON", "3D_CARBON" } },
        { "Sparco",          { "SPARCO" } },
        { "Recaro",          { "RECARO" } },
        { "Momo",            { "MOMO" } },
        { "Bride",           { "BRIDE" } },
    };

    static const char* prefs[] = { "AFTERMARKET_", "", "BRAND_", "BRAND_NAME_", "CUST_BRAND_", "FE_BRAND_" };
    for (const auto& e : known) {
        for (const char* k : e.keys) {
            for (const char* p : prefs) {
                std::string full = std::string(p) + k;
                db[toHex8(calcBHash(full))] = e.disp;
                db[toHex8(calcBHash(toLowerAscii(full)))] = e.disp;
            }
        }
    }
    return db;
}

inline std::string lookupBrandName(const std::string& hashHex, const std::unordered_map<std::string, std::string>* extra = nullptr) {
    if (hashHex.empty() || hashHex == "00000000" || hashHex == "FFFFFFFF") return "";
    if (extra) {
        auto it = extra->find(hashHex);
        if (it != extra->end()) return it->second;
    }
    const auto& b = builtinBrandNames();
    auto it = b.find(hashHex);
    return (it != b.end()) ? it->second : "";
}

struct DecodedStage {
    std::string shortStr;
    std::string stageName;
    std::string brandName;
    std::string fullLabel;
    std::string brandHash;
    bool        isRawBrand = false;
};

inline DecodedStage decodeStageAndBrand(
    uint32_t slot, uint32_t w2, uint32_t mCount,
    bool hasMeta3EF, uint8_t exactStage, uint8_t cfgInStage, uint32_t brandHashU32,
    const std::unordered_map<std::string, std::string>* extraBrands = nullptr
) {
    DecodedStage r;
    uint32_t idx = w2 & 0xFFFFu;
    if (hasMeta3EF && brandHashU32 != 0 && brandHashU32 != 0xFFFFFFFFu) {
        r.brandHash = toHex8(brandHashU32);
    }
    std::string bName = lookupBrandName(r.brandHash, extraBrands);

    int cfgNum = (int)idx;
    if (idx == 0) {
        r.shortStr = "Сток";
        r.stageName = "Сток";
        cfgNum = 0;
    } else if (slot == 7) {
        bool isWide = ((w2 >> 28) & 1u) != 0;
        r.shortStr  = (isWide ? "WB #" : "Кит #") + std::to_string(idx);
        r.stageName = isWide ? "Widebody" : "Обвес";
    } else if (slot == 78) {
        bool isCarb = ((w2 >> 16) & 1u) != 0;
        r.shortStr  = (isCarb ? "Карб #" : "Капот #") + std::to_string(idx);
        r.stageName = isCarb ? "Карбон" : "Капот";
    } else if (slot == 90) {
        r.shortStr  = "Сп #" + std::to_string(idx);
        r.stageName = "Спойлер";
    } else if (slot == 9) {
        if (mCount >= 15) {
            int adj = (int)idx - 3;
            if (adj < 0) {
                r.shortStr  = "0-" + std::to_string(idx);
                r.stageName = "База";
            } else {
                int st = (adj / 3) + 1;
                cfgNum = (adj % 3) + 1;
                r.shortStr  = std::to_string(st) + "-" + std::to_string(cfgNum);
                r.stageName = "Этап " + std::to_string(st);
            }
        } else if (mCount >= 4) {
            int stagesAvail = std::max(1, (int)((mCount - 1) / 3));
            int minStage = std::max(1, 5 - stagesAvail);
            int st = minStage + (int)((idx - 1) / 3);
            cfgNum = (int)(((idx - 1) % 3) + 1);
            r.shortStr  = std::to_string(st) + "-" + std::to_string(cfgNum);
            r.stageName = "Этап " + std::to_string(st);
        } else {
            r.shortStr = "#" + std::to_string(idx);
            r.stageName = r.shortStr;
        }
    } else {
        if (mCount >= 4 && ((mCount - 1) % 3 == 0)) {
            int stagesAvail = (int)((mCount - 1) / 3);
            int minStage = std::max(1, 5 - stagesAvail);
            int st = minStage + (int)((idx - 1) / 3);
            cfgNum = (int)(((idx - 1) % 3) + 1);
            r.shortStr  = std::to_string(st) + "-" + std::to_string(cfgNum);
            r.stageName = "Этап " + std::to_string(st);
        } else {
            r.shortStr = (mCount > 0) ? ("#" + std::to_string(idx) + "/" + std::to_string(mCount)) : ("#" + std::to_string(idx));
            r.stageName = r.shortStr;
        }
    }

    if (hasMeta3EF && slot != 7 && slot != 78 && slot != 90) {
        if (exactStage == 0) {
            r.stageName = "Сток";
            r.brandName = bName.empty() ? "Серийные" : bName;
            r.fullLabel = "Сток — " + r.brandName;
            r.shortStr  = "Сток";
            return r;
        } else if (exactStage >= 1 && exactStage <= 4) {
            r.stageName = "Этап " + std::to_string(exactStage);
            if (cfgInStage > 0) cfgNum = cfgInStage;
            r.shortStr = std::to_string(exactStage) + "-" + std::to_string(cfgNum);
        }
    }

    if (idx == 0) {
        r.stageName = "Сток";
        r.brandName = bName.empty() ? "Серийные" : bName;
        r.fullLabel = "Сток — " + r.brandName;
        r.shortStr  = "Сток";
        return r;
    }

    if (!bName.empty()) {
        r.brandName = bName;
    } else if (!r.brandHash.empty()) {
        r.brandName = r.brandHash;
        r.isRawBrand = true;
    } else {
        r.brandName = "Конф. " + std::to_string(cfgNum);
    }
    r.fullLabel = r.stageName + " — " + r.brandName;
    return r;
}

} // namespace ProStreetMeta
