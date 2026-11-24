#pragma once
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

struct LuaValue {
    enum Type { NIL, BOOL, NUMBER, STRING, TABLE };
    Type type = NIL;
    bool bval = false;
    double nval = 0.0;
    std::string sval;
    std::map<std::string, LuaValue> strMap;
    std::map<int64_t, LuaValue> intMap;

    bool isTable() const { return type == TABLE; }
    bool isString() const { return type == STRING; }
    bool isNumber() const { return type == NUMBER; }

    const LuaValue* get(const std::string& k) const {
        if (type != TABLE) return nullptr;
        auto it = strMap.find(k);
        return (it != strMap.end()) ? &it->second : nullptr;
    }
    const LuaValue* getInt(int64_t k) const {
        if (type != TABLE) return nullptr;
        auto it = intMap.find(k);
        return (it != intMap.end()) ? &it->second : nullptr;
    }
    std::string getStr(const std::string& k, const std::string& def = "") const {
        const LuaValue* v = get(k);
        return (v && v->type == STRING) ? v->sval : def;
    }
    double getNum(const std::string& k, double def = 0.0) const {
        const LuaValue* v = get(k);
        return (v && v->type == NUMBER) ? v->nval : def;
    }
    int64_t getIntVal(const std::string& k, int64_t def = 0) const {
        const LuaValue* v = get(k);
        return (v && v->type == NUMBER) ? static_cast<int64_t>(v->nval) : def;
    }
};

class LuaTableParser {
    const char* p = nullptr;
    const char* end = nullptr;

    void skipWS() {
        while (p < end) {
            unsigned char c = static_cast<unsigned char>(*p);
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                ++p;
            } else if (c == '-' && (p + 1 < end) && p[1] == '-') {
                p += 2;
                while (p < end && *p != '\n') ++p;
            } else {
                break;
            }
        }
    }

    bool matchWord(const char* w) {
        size_t len = std::strlen(w);
        if ((size_t)(end - p) >= len && std::memcmp(p, w, len) == 0) {
            if ((size_t)(end - p) == len || (!std::isalnum((unsigned char)p[len]) && p[len] != '_')) {
                p += len;
                return true;
            }
        }
        return false;
    }

    bool parseString(std::string& out) {
        skipWS();
        if (p >= end) return false;
        char q = *p;
        if (q != '"' && q != '\'') return false;
        ++p;
        out.clear();
        while (p < end && *p != q) {
            if (*p == '\\' && (p + 1 < end)) {
                ++p;
                char esc = *p++;
                switch (esc) {
                    case 'n': out.push_back('\n'); break;
                    case 'r': out.push_back('\r'); break;
                    case 't': out.push_back('\t'); break;
                    case '\\': out.push_back('\\'); break;
                    case '"': out.push_back('"'); break;
                    case '\'': out.push_back('\''); break;
                    default: out.push_back(esc); break;
                }
            } else {
                out.push_back(*p++);
            }
        }
        if (p < end && *p == q) {
            ++p;
            return true;
        }
        return false;
    }

    bool parseNumber(double& out) {
        skipWS();
        if (p >= end) return false;
        const char* start = p;
        char* stop = nullptr;
        out = std::strtod(start, &stop);
        if (stop == start) return false;
        p = stop;
        return true;
    }

    bool parseIdent(std::string& out) {
        skipWS();
        if (p >= end) return false;
        if (!std::isalpha((unsigned char)*p) && *p != '_') return false;
        const char* start = p;
        while (p < end && (std::isalnum((unsigned char)*p) || *p == '_')) ++p;
        out.assign(start, p - start);
        return true;
    }

    bool parseValue(LuaValue& out) {
        skipWS();
        if (p >= end) return false;
        char c = *p;
        if (c == '{') return parseTable(out);
        if (c == '"' || c == '\'') {
            out.type = LuaValue::STRING;
            return parseString(out.sval);
        }
        if (matchWord("nil"))   { out.type = LuaValue::NIL; return true; }
        if (matchWord("true"))  { out.type = LuaValue::BOOL; out.bval = true; return true; }
        if (matchWord("false")) { out.type = LuaValue::BOOL; out.bval = false; return true; }
        if (c == '-' || c == '+' || c == '.' || std::isdigit((unsigned char)c)) {
            out.type = LuaValue::NUMBER;
            return parseNumber(out.nval);
        }
        return false;
    }

    bool parseTable(LuaValue& out) {
        skipWS();
        if (p >= end || *p != '{') return false;
        ++p;
        out.type = LuaValue::TABLE;
        out.strMap.clear();
        out.intMap.clear();
        int64_t nextArrayIdx = 1;

        while (p < end) {
            skipWS();
            if (p >= end) return false;
            if (*p == '}') { ++p; return true; }
            if (*p == ',' || *p == ';') { ++p; continue; }

            if (*p == '[') {
                ++p;
                skipWS();
                LuaValue keyVal;
                if (!parseValue(keyVal)) return false;
                skipWS();
                if (p >= end || *p != ']') return false;
                ++p;
                skipWS();
                if (p >= end || *p != '=') return false;
                ++p;
                LuaValue val;
                if (!parseValue(val)) return false;
                if (keyVal.type == LuaValue::STRING) {
                    out.strMap[keyVal.sval] = std::move(val);
                } else if (keyVal.type == LuaValue::NUMBER) {
                    out.intMap[static_cast<int64_t>(keyVal.nval)] = std::move(val);
                }
            } else {
                const char* save = p;
                std::string ident;
                bool isKeyVal = false;
                if (parseIdent(ident)) {
                    skipWS();
                    if (p < end && *p == '=') {
                        ++p;
                        isKeyVal = true;
                        LuaValue val;
                        if (!parseValue(val)) return false;
                        out.strMap[ident] = std::move(val);
                    }
                }
                if (!isKeyVal) {
                    p = save;
                    LuaValue val;
                    if (!parseValue(val)) return false;
                    out.intMap[nextArrayIdx++] = std::move(val);
                }
            }
            skipWS();
            if (p < end && (*p == ',' || *p == ';')) ++p;
        }
        return false;
    }

public:
    bool parseBuffer(const std::string& content, LuaValue& root) {
        p = content.data();
        end = p + content.size();
        if (content.size() >= 3 && (unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF) {
            p += 3;
        }
        skipWS();
        matchWord("return");
        skipWS();
        return parseValue(root);
    }

    bool parseFile(const std::string& path, LuaValue& root) {
        std::ifstream f(path, std::ios::binary);
        if (!f.is_open()) return false;
        std::ostringstream ss;
        ss << f.rdbuf();
        return parseBuffer(ss.str(), root);
    }
};
