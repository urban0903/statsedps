#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>
#include <commdlg.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../common/shared_ipc.h"
#include "../common/lua_table_parser.h"
#include "../common/brand_db.h"
#include "embedded_dbs.h"

// ============================================================================
// UTF-8 <-> UTF-16 helpers for Win32 Unicode API
// ============================================================================
inline std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    if (n <= 0) return L"";
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

inline std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return "";
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

inline std::string GetExeDirUtf8() {
    wchar_t buf[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring w(buf);
    size_t pos = w.find_last_of(L"\\/");
    return (pos != std::wstring::npos) ? WideToUtf8(w.substr(0, pos)) : ".";
}

inline std::string GetAppDataDirUtf8() {
    wchar_t buf[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableW(L"APPDATA", buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH) return WideToUtf8(std::wstring(buf, n));
    return GetExeDirUtf8();
}

inline std::string FileNameOnly(const std::string& p) {
    size_t pos = p.find_last_of("\\/");
    return (pos != std::string::npos) ? p.substr(pos + 1) : p;
}

inline std::string ShortenPath(const std::string& p, size_t maxLen) {
    if (p.size() <= maxLen) return p;
    std::string fn = FileNameOnly(p);
    if (fn.size() + 6 >= maxLen) {
        return "..." + p.substr(p.size() - (maxLen - 3));
    }
    size_t head = std::max<size_t>(8, maxLen - fn.size() - 5);
    return p.substr(0, head) + "...\\" + fn;
}

inline bool FileExistsUtf8(const std::string& p) {
    DWORD a = GetFileAttributesW(Utf8ToWide(p).c_str());
    return (a != INVALID_FILE_ATTRIBUTES) && ((a & FILE_ATTRIBUTE_DIRECTORY) == 0);
}

// ============================================================================
// Stock DB & VLT Profiles DB (loads existing .lua files directly!)
// ============================================================================
struct StockCarEntry {
    std::string name;
    std::array<float, 21> vals{};
    std::map<std::string, std::array<float, 21>> modes;
};

struct ProfilePartItem {
    std::string phys;
    std::string stage;
    std::string brand;
    std::string brandHash;
    std::string label;
};

struct ProfileSlotData {
    int slot = 0;
    std::string name;
    int mCount = 0;
    std::map<int, ProfilePartItem> items;
};

struct ProfileCarEntry {
    std::string name;
    int bpCarType = 0;
    std::map<std::string, ProfileSlotData> slots;
};

struct MatchSamePhysRec {
    std::string carHash;
    std::string carName;
    std::string stage;
    std::string brand;
    std::string brandHash;
    std::string label;
    std::string displayLabel;
    bool isRawBrand = false;
    int cnt = 1;
};

struct MatchDiffPhysRec {
    std::string carHash;
    std::string carName;
    std::string phys;
    std::string label;
};

struct Databases {
    std::string stockDbPath    = ""; // пусто = встроенная база (76/76)
    std::string profilesDbPath = ""; // пусто = встроенная VLT-база (76/76)
    bool stockIsEmbedded       = true;
    bool profilesIsEmbedded    = true;
    std::string cfgIniPath;

    std::unordered_map<std::string, StockCarEntry> stockDB;
    std::unordered_map<std::string, ProfileCarEntry> profCars;
    std::unordered_map<std::string, std::string> profBrandNames;

    // Indices for instant O(1) lookup in "Совпадения"
    struct PhysIdxRec {
        std::string carHash, carName, slotKey, stage, brand, brandHash, label;
        int idx = 0;
    };
    struct SbIdxRec {
        std::string carHash, carName, phys, label;
        int idx = 0;
    };
    std::unordered_map<std::string, std::vector<PhysIdxRec>> physIndex;
    std::unordered_map<std::string, std::vector<SbIdxRec>>   sbIndex;

    // Stats
    int statTotal = 0, statFullAll = 0, statGrip = 0, statDrift = 0, statDrag = 0, statSpeed = 0;

    void initPaths() {
        cfgIniPath = GetAppDataDirUtf8() + "\\statsEdps_paths.ini";
        // По умолчанию всегда загружаем встроенные базы (76/76); внешний .lua подхватится, только если задан в statsEdps_paths.ini
        loadConfig();
        loadStockDB(false);
        loadProfilesDB(false);
    }

    void loadConfig() {
        std::ifstream f(cfgIniPath);
        if (!f.is_open()) return;
        std::string line;
        while (std::getline(f, line)) {
            while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
            if (line.rfind("stock_db=", 0) == 0 && line.size() > 9)    stockDbPath    = line.substr(9);
            if (line.rfind("profiles_db=", 0) == 0 && line.size() > 12) profilesDbPath = line.substr(12);
        }
    }

    void saveConfig() const {
        std::ofstream f(cfgIniPath, std::ios::trunc);
        if (!f.is_open()) return;
        f << "stock_db=" << stockDbPath << "\n";
        f << "profiles_db=" << profilesDbPath << "\n";
    }

    static bool extractArray21(const LuaValue* tbl, std::array<float, 21>& out) {
        out.fill(0.0f);
        if (!tbl || !tbl->isTable()) return false;
        bool any = false;
        for (int i = 1; i <= 21; ++i) {
            const LuaValue* v = tbl->getInt(i);
            if (v && v->isNumber()) {
                out[i - 1] = static_cast<float>(v->nval);
                any = true;
            }
        }
        return any;
    }

    void loadEmbeddedStockDB() {
        stockDB.clear();
        for (uint32_t i = 0; i < EmbeddedDB::STOCK_CAR_COUNT; ++i) {
            const auto& sc = EmbeddedDB::STOCK_CARS[i];
            StockCarEntry e;
            e.name = sc.name;
            std::memcpy(e.vals.data(), sc.vals, sizeof(float) * 21);
            for (uint16_t m = 0; m < sc.modeCount; ++m) {
                const auto& sm = EmbeddedDB::STOCK_MODES[sc.modeStart + m];
                std::array<float, 21> mv{};
                std::memcpy(mv.data(), sm.vals, sizeof(float) * 21);
                e.modes[sm.modeKey] = mv;
            }
            stockDB[sc.hash] = std::move(e);
        }
        stockIsEmbedded = true;
        recalcStats();
    }

    bool loadStockDB(bool explicitOnly) {
        if (explicitOnly && !stockDbPath.empty() && FileExistsUtf8(stockDbPath)) {
            LuaTableParser parser;
            LuaValue root;
            if (parser.parseFile(stockDbPath, root) && root.isTable()) {
                stockDB.clear();
                for (const auto& kv : root.strMap) {
                    if (!kv.second.isTable()) continue;
                    StockCarEntry e;
                    e.name = kv.second.getStr("name", "CAR_" + kv.first);
                    extractArray21(kv.second.get("vals"), e.vals);
                    const LuaValue* modesTbl = kv.second.get("modes");
                    if (modesTbl && modesTbl->isTable()) {
                        for (const auto& mkv : modesTbl->strMap) {
                            std::array<float, 21> mv{};
                            if (extractArray21(&mkv.second, mv)) e.modes[mkv.first] = mv;
                        }
                        for (const auto& mkv : modesTbl->intMap) {
                            std::array<float, 21> mv{};
                            if (extractArray21(&mkv.second, mv)) e.modes[std::to_string(mkv.first)] = mv;
                        }
                    }
                    stockDB[kv.first] = std::move(e);
                }
                stockIsEmbedded = false;
                recalcStats();
                return true;
            }
            return false;
        }
        // По умолчанию используем встроенную базу (76/76), не требующую внешних файлов!
        if (!stockDbPath.empty() && FileExistsUtf8(stockDbPath)) {
            if (loadStockDB(true)) return true;
        }
        stockDbPath.clear();
        loadEmbeddedStockDB();
        return true;
    }

    void recalcStats() {
        statTotal = statFullAll = statGrip = statDrift = statDrag = statSpeed = 0;
        const auto& driftSet = ProStreetMeta::driftEligibleNames();
        for (const auto& kv : stockDB) {
            statTotal++;
            const auto& m = kv.second.modes;
            bool h0 = m.count("0") > 0, h1 = m.count("1") > 0, h2 = m.count("2") > 0, h3 = m.count("3") > 0;
            if (h0) statGrip++;
            if (h1) statDrift++;
            if (h2) statDrag++;
            if (h3) statSpeed++;
            bool needsDrift = driftSet.count(ProStreetMeta::toUpperAscii(kv.second.name)) > 0;
            if (h0 && h2 && h3 && (!needsDrift || h1)) statFullAll++;
        }
    }

    void loadEmbeddedProfilesDB() {
        profCars.clear();
        profBrandNames.clear();
        for (uint32_t i = 0; i < EmbeddedDB::PROF_CAR_COUNT; ++i) {
            const auto& pc = EmbeddedDB::PROF_CARS[i];
            ProfileCarEntry ce;
            ce.name = pc.name;
            ce.bpCarType = pc.bpCarType;
            for (uint16_t s = 0; s < pc.slotCount; ++s) {
                const auto& ps = EmbeddedDB::PROF_SLOTS[pc.slotStart + s];
                ProfileSlotData sd;
                sd.slot   = ps.slotNum;
                sd.name   = EmbeddedDB::SLOT_NAMES[ps.snameId];
                sd.mCount = ps.mCount;
                for (uint16_t k = 0; k < ps.itemCount; ++k) {
                    const auto& pi = EmbeddedDB::PROF_ITEMS[ps.itemStart + k];
                    ProfilePartItem it;
                    it.phys      = EmbeddedDB::PHYS_KEYS[pi.physId];
                    it.stage     = EmbeddedDB::STAGES[pi.stageId];
                    it.brand     = EmbeddedDB::BRANDS[pi.brandId];
                    it.brandHash = EmbeddedDB::BHASHES[pi.bhashId];
                    it.label     = it.stage + " — " + it.brand;
                    sd.items[pi.idx] = std::move(it);
                }
                ce.slots[EmbeddedDB::SLOT_KEYS[ps.skeyId]] = std::move(sd);
            }
            profCars[pc.hash] = std::move(ce);
        }
        profilesIsEmbedded = true;
        rebuildProfileIndices();
    }

    bool loadProfilesDB(bool explicitOnly) {
        if (explicitOnly && !profilesDbPath.empty() && FileExistsUtf8(profilesDbPath)) {
            LuaTableParser parser;
            LuaValue root;
            if (parser.parseFile(profilesDbPath, root) && root.isTable()) {
                profCars.clear();
                profBrandNames.clear();
                const LuaValue* bTbl = root.get("brandNames");
                if (bTbl && bTbl->isTable()) {
                    for (const auto& kv : bTbl->strMap) {
                        if (kv.second.isString()) profBrandNames[kv.first] = kv.second.sval;
                    }
                }
                const LuaValue* cTbl = root.get("cars");
                if (cTbl && cTbl->isTable()) {
                    for (const auto& ckv : cTbl->strMap) {
                        if (!ckv.second.isTable()) continue;
                        ProfileCarEntry ce;
                        ce.name = ckv.second.getStr("name", "CAR_" + ckv.first);
                        ce.bpCarType = static_cast<int>(ckv.second.getIntVal("bpCarType", 0));
                        const LuaValue* sTbl = ckv.second.get("slots");
                        if (sTbl && sTbl->isTable()) {
                            for (const auto& skv : sTbl->strMap) {
                                if (!skv.second.isTable()) continue;
                                ProfileSlotData sd;
                                sd.slot   = static_cast<int>(skv.second.getIntVal("slot", 0));
                                sd.name   = skv.second.getStr("name", skv.first);
                                sd.mCount = static_cast<int>(skv.second.getIntVal("mCount", 0));
                                const LuaValue* iTbl = skv.second.get("items");
                                if (iTbl && iTbl->isTable()) {
                                    for (const auto& ikv : iTbl->intMap) {
                                        if (!ikv.second.isTable()) continue;
                                        ProfilePartItem pi;
                                        pi.phys      = ikv.second.getStr("phys");
                                        pi.stage     = ikv.second.getStr("stage");
                                        pi.brand     = ikv.second.getStr("brand");
                                        pi.brandHash = ikv.second.getStr("brandHash");
                                        pi.label     = ikv.second.getStr("label");
                                        sd.items[static_cast<int>(ikv.first)] = std::move(pi);
                                    }
                                }
                                ce.slots[skv.first] = std::move(sd);
                            }
                        }
                        profCars[ckv.first] = std::move(ce);
                    }
                }
                profilesIsEmbedded = false;
                rebuildProfileIndices();
                return true;
            }
            return false;
        }
        // По умолчанию используем встроенную VLT-базу (76/76)!
        if (!profilesDbPath.empty() && FileExistsUtf8(profilesDbPath)) {
            if (loadProfilesDB(true)) return true;
        }
        profilesDbPath.clear();
        loadEmbeddedProfilesDB();
        return true;
    }

    void rebuildProfileIndices() {
        physIndex.clear();
        sbIndex.clear();
        for (auto& ckv : profCars) {
            const std::string& carHash = ckv.first;
            ProfileCarEntry& ce = ckv.second;
            auto itS = stockDB.find(carHash);
            if (itS != stockDB.end()) ce.name = itS->second.name;
            else ce.name = ProStreetMeta::normalizeCarName(ce.name);

            for (auto& skv : ce.slots) {
                const std::string& slotKey = skv.first;
                for (auto& ikv : skv.second.items) {
                    ProfilePartItem& it = ikv.second;
                    std::string resolvedB = ProStreetMeta::lookupBrandName(it.brandHash, &profBrandNames);
                    if (!resolvedB.empty()) {
                        it.brand = resolvedB;
                        it.label = it.stage + " — " + resolvedB;
                    }
                    if (!it.phys.empty() && it.phys != "00000000") {
                        physIndex[it.phys].push_back({ carHash, ce.name, slotKey, it.stage, it.brand, it.brandHash, it.label, ikv.first });
                        std::string bKey = (!it.brandHash.empty() && it.brandHash != "00000000" && it.brandHash != "FFFFFFFF") ? it.brandHash : it.brand;
                        if (!bKey.empty() && !it.stage.empty() && it.stage != "Сток") {
                            sbIndex[slotKey + "|" + it.stage + "|" + bKey].push_back({ carHash, ce.name, it.phys, it.label, ikv.first });
                        }
                    }
                }
            }
        }
        for (auto& kv : physIndex) {
            std::sort(kv.second.begin(), kv.second.end(), [](const PhysIdxRec& a, const PhysIdxRec& b) {
                return (a.carName != b.carName) ? (a.carName < b.carName) : (a.idx < b.idx);
            });
        }
        for (auto& kv : sbIndex) {
            std::sort(kv.second.begin(), kv.second.end(), [](const SbIdxRec& a, const SbIdxRec& b) {
                return (a.carName != b.carName) ? (a.carName < b.carName) : (a.phys < b.phys);
            });
        }
    }

    void findMatches(const std::string& activeBaseHash, const std::string& slotKey,
                     const std::string& physHex, const std::string& stageName,
                     const std::string& brandHash,
                     std::vector<MatchSamePhysRec>& outSame,
                     std::vector<MatchDiffPhysRec>& outDiff) const {
        outSame.clear();
        outDiff.clear();
        std::string activeCanon = ProStreetMeta::canonicalCarHash(activeBaseHash);

        if (!physHex.empty()) {
            auto it = physIndex.find(physHex);
            if (it != physIndex.end()) {
                std::unordered_map<std::string, size_t> byCar;
                for (const auto& m : it->second) {
                    std::string mCanon = ProStreetMeta::canonicalCarHash(m.carHash);
                    if (m.carHash == activeBaseHash || mCanon == activeCanon) continue;
                    auto itc = byCar.find(mCanon);
                    if (itc == byCar.end()) {
                        MatchSamePhysRec rec;
                        rec.carHash = m.carHash; rec.carName = m.carName;
                        rec.stage = m.stage; rec.brand = m.brand; rec.brandHash = m.brandHash;
                        rec.label = m.label; rec.isRawBrand = ProStreetMeta::isRawHex8(m.brand);
                        rec.cnt = 1;
                        byCar[mCanon] = outSame.size();
                        outSame.push_back(std::move(rec));
                    } else {
                        outSame[itc->second].cnt++;
                    }
                }
                for (auto& r : outSame) {
                    r.displayLabel = (r.cnt > 1) ? (r.label + " (" + std::to_string(r.cnt) + "x)") : r.label;
                }
            }
        }

        bool hasBH = !brandHash.empty() && brandHash != "00000000" && brandHash != "FFFFFFFF";
        if (!slotKey.empty() && !stageName.empty() && stageName != "Сток" && hasBH) {
            auto it = sbIndex.find(slotKey + "|" + stageName + "|" + brandHash);
            if (it != sbIndex.end()) {
                std::unordered_set<std::string> seen;
                for (const auto& m : it->second) {
                    std::string mCanon = ProStreetMeta::canonicalCarHash(m.carHash);
                    if (m.carHash == activeBaseHash || mCanon == activeCanon || m.phys == physHex) continue;
                    if (seen.insert(mCanon + "|" + m.phys).second) {
                        outDiff.push_back({ m.carHash, m.carName, m.phys, m.label });
                    }
                }
            }
        }
    }

    const std::array<float, 21>* getStockValsForMode(const std::string& baseHash, const std::string& modeKey, bool& outExact) const {
        outExact = false;
        auto it = stockDB.find(baseHash);
        if (it == stockDB.end()) return nullptr;
        auto itm = it->second.modes.find(modeKey);
        if (itm != it->second.modes.end()) { outExact = true; return &itm->second; }
        return &it->second.vals;
    }

    const ProfileCarEntry* getProfileCar(const std::string& baseHash) const {
        std::string canon = ProStreetMeta::canonicalCarHash(baseHash);
        auto it = profCars.find(canon);
        if (it != profCars.end()) return &it->second;
        it = profCars.find(baseHash);
        return (it != profCars.end()) ? &it->second : nullptr;
    }
};

// ============================================================================
// Process Attachment, DLL Auto-Injection & Shared Memory IPC Client
// ============================================================================
class GameBridge {
    HANDLE          hMap = nullptr;
    HANDLE          hCmdEvent = nullptr;
    ShmSharedState* shm  = nullptr;
    DWORD           attachedPid = 0;
    uint32_t        lastConnectTryTick = 0;
    bool            syncedKnownTypes = false;
    std::string     statusText = "Ожидание процесса nfs.exe...";

    static DWORD FindNfsPid() {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap == INVALID_HANDLE_VALUE) return 0;
        PROCESSENTRY32W pe = {};
        pe.dwSize = sizeof(pe);
        DWORD pid = 0;
        if (Process32FirstW(snap, &pe)) {
            do {
                if (_wcsicmp(pe.szExeFile, L"nfs.exe") == 0) {
                    pid = pe.th32ProcessID;
                    break;
                }
            } while (Process32NextW(snap, &pe));
        }
        CloseHandle(snap);
        return pid;
    }

    static bool IsDllLoaded(DWORD pid, const wchar_t* dllName) {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
        if (snap == INVALID_HANDLE_VALUE) return false;
        MODULEENTRY32W me = {};
        me.dwSize = sizeof(me);
        bool found = false;
        if (Module32FirstW(snap, &me)) {
            do {
                if (_wcsicmp(me.szModule, dllName) == 0) { found = true; break; }
            } while (Module32NextW(snap, &me));
        }
        CloseHandle(snap);
        return found;
    }

    static bool InjectDll(DWORD pid, const std::wstring& fullDllPath, std::string& outErr) {
        HANDLE hProc = OpenProcess(
            PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
            FALSE, pid
        );
        if (!hProc) {
            outErr = "Не удалось открыть nfs.exe (запусти от имени администратора)";
            return false;
        }
        size_t bytes = (fullDllPath.size() + 1) * sizeof(wchar_t);
        LPVOID remoteMem = VirtualAllocEx(hProc, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!remoteMem) {
            CloseHandle(hProc);
            outErr = "VirtualAllocEx failed";
            return false;
        }
        if (!WriteProcessMemory(hProc, remoteMem, fullDllPath.c_str(), bytes, nullptr)) {
            VirtualFreeEx(hProc, remoteMem, 0, MEM_RELEASE);
            CloseHandle(hProc);
            outErr = "WriteProcessMemory failed";
            return false;
        }
        HMODULE hK32 = GetModuleHandleW(L"kernel32.dll");
        LPTHREAD_START_ROUTINE pLoadLib = reinterpret_cast<LPTHREAD_START_ROUTINE>(GetProcAddress(hK32, "LoadLibraryW"));
        HANDLE hTh = CreateRemoteThread(hProc, nullptr, 0, pLoadLib, remoteMem, 0, nullptr);
        if (!hTh) {
            VirtualFreeEx(hProc, remoteMem, 0, MEM_RELEASE);
            CloseHandle(hProc);
            outErr = "CreateRemoteThread failed (проверь, что EXE и DLL собраны как x86 32-bit)";
            return false;
        }
        WaitForSingleObject(hTh, 3000);
        CloseHandle(hTh);
        VirtualFreeEx(hProc, remoteMem, 0, MEM_RELEASE);
        CloseHandle(hProc);
        return true;
    }

public:
    bool isConnected() const {
        if (!shm || attachedPid == 0) return false;
        return shm->magic == SHM_MAGIC && shm->dllPid == attachedPid &&
               shm->dllUnloaded == 0 && (GetTickCount() - shm->dllHeartbeat) < 3000u;
    }

    std::string getStatus() const { return statusText; }
    DWORD getPid() const { return attachedPid; }

    void ensureSharedMem() {
        if (!hCmdEvent) {
            hCmdEvent = CreateEventA(nullptr, FALSE, FALSE, SHM_CMD_EVENT_NAME);
        }
        if (shm) return;
        hMap = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(ShmSharedState), SHM_NAME);
        if (hMap) {
            shm = reinterpret_cast<ShmSharedState*>(MapViewOfFile(hMap, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(ShmSharedState)));
            if (shm && (shm->magic != SHM_MAGIC || shm->version != SHM_VERSION)) {
                std::memset(shm, 0, sizeof(ShmSharedState));
                shm->magic = SHM_MAGIC;
                shm->version = SHM_VERSION;
            }
        }
    }

    void tickConnect(const Databases& dbs) {
        ensureSharedMem();
        if (isConnected()) {
            if (!syncedKnownTypes) syncKnownCarTypes(dbs);
            return;
        }
        uint32_t now = GetTickCount();
        if (now - lastConnectTryTick < 1000u) return;
        lastConnectTryTick = now;
        syncedKnownTypes = false;

        DWORD pid = FindNfsPid();
        if (pid == 0) {
            attachedPid = 0;
            statusText = "Ожидание запуска nfs.exe...";
            return;
        }

        if (shm && shm->dllPid == pid && shm->dllUnloaded == 0 && (now - shm->dllHeartbeat) < 2500u) {
            attachedPid = pid;
            statusText = "Подключено к nfs.exe (PID " + std::to_string(pid) + ")";
            syncKnownCarTypes(dbs);
            return;
        }

        std::string dllUtf8 = GetExeDirUtf8() + "\\statsEdps_hooks.dll";
        if (!FileExistsUtf8(dllUtf8)) {
            statusText = "Не найден statsEdps_hooks.dll рядом с statsEdps.exe!";
            return;
        }

        if (!IsDllLoaded(pid, L"statsEdps_hooks.dll")) {
            if (shm) { shm->unloadRequested = 0; shm->dllUnloaded = 0; }
            std::string err;
            if (!InjectDll(pid, Utf8ToWide(dllUtf8), err)) {
                statusText = "Ошибка подключения: " + err;
                return;
            }
        }
        attachedPid = pid;
        statusText = "Хуки внедрены в nfs.exe (PID " + std::to_string(pid) + ")";
    }

    bool sendCommand(uint32_t cmdType, uint32_t p1 = 0, uint32_t p2 = 0, float fp = 0.0f,
                     bool waitAck = false, std::string* outMsg = nullptr, bool* outOk = nullptr) {
        if (!isConnected()) return false;
        uint32_t nextSeq = shm->cmdSeq + 1;
        shm->cmdType   = cmdType;
        shm->cmdParam1 = p1;
        shm->cmdParam2 = p2;
        shm->cmdFParam = fp;
        shm->cmdSeq    = nextSeq;
        if (hCmdEvent) SetEvent(hCmdEvent);
        if (!waitAck) return true;
        uint32_t t0 = GetTickCount();
        while (shm->cmdAckSeq != nextSeq && (GetTickCount() - t0) < 1500u) {
            Sleep(1);
        }
        if (shm->cmdAckSeq == nextSeq) {
            if (outOk)  *outOk  = (shm->cmdResultOk != 0);
            if (outMsg) *outMsg = shm->cmdResultMsg;
            return true;
        }
        return false;
    }

    void syncKnownCarTypes(const Databases& dbs) {
        if (!isConnected()) return;
        for (const auto& kv : dbs.profCars) {
            if (kv.second.bpCarType > 0) {
                uint32_t hVal = static_cast<uint32_t>(std::strtoul(kv.first.c_str(), nullptr, 16));
                if (hVal != 0) {
                    sendCommand(CMD_SYNC_KNOWN_CAR_TYPE, hVal, static_cast<uint32_t>(kv.second.bpCarType), 0.0f, true);
                }
            }
        }
        syncedKnownTypes = true;
    }

    bool readFastSnapshot(ShmSharedState& dst, uint32_t& ioLastFastSeq, bool& outChanged) const {
        outChanged = false;
        if (!isConnected()) return false;
        uint32_t s1 = shm->fastSeq;
        if (s1 == 0 || (s1 & 1u) != 0) return false;
        if (s1 == ioLastFastSeq) return true;
        // Copy only fast header portion (before catalogSeq)
        size_t fastBytes = offsetof(ShmSharedState, catalogSeq);
        std::memcpy(&dst, shm, fastBytes);
        if (shm->fastSeq == s1) {
            ioLastFastSeq = s1;
            outChanged = true;
            return true;
        }
        return false;
    }

    bool readCatalogSnapshot(ShmSharedState& dst, uint32_t& ioLastCatSeq, bool& outChanged) const {
        outChanged = false;
        if (!isConnected()) return false;
        uint32_t s1 = shm->catalogSeq;
        if (s1 == 0 || (s1 & 1u) != 0) return false;
        if (s1 == ioLastCatSeq) return true;
        std::memcpy(dst.catalogCarHash, shm->catalogCarHash, sizeof(dst.catalogCarHash));
        std::memcpy(dst.inspectSlots, shm->inspectSlots, sizeof(dst.inspectSlots));
        if (shm->catalogSeq == s1) {
            ioLastCatSeq = s1;
            outChanged = true;
            return true;
        }
        return false;
    }

    void shutdownAndUnload() {
        if (shm && isConnected()) {
            shm->unloadRequested = 1;
            if (hCmdEvent) SetEvent(hCmdEvent);
            uint32_t t0 = GetTickCount();
            while (shm->dllUnloaded == 0 && (GetTickCount() - t0) < 600u) {
                Sleep(10);
            }
        }
        if (hCmdEvent) { CloseHandle(hCmdEvent); hCmdEvent = nullptr; }
        if (shm) { UnmapViewOfFile(shm); shm = nullptr; }
        if (hMap) { CloseHandle(hMap); hMap = nullptr; }
    }
};
