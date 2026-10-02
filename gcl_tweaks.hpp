


#pragma once
#include <cmath>
#include <cstring>
#include "modcore.hpp"

namespace am {
struct GclGlobal { std::string name, type; size_t off = 0; };   

inline uint32_t gu32(const std::string& d, size_t o) { uint32_t v; std::memcpy(&v, d.data() + o, 4); return v; }
inline int32_t gi32(const std::string& d, size_t o) { int32_t v; std::memcpy(&v, d.data() + o, 4); return v; }
inline bool bytes_ok(const std::string& d, size_t at, size_t count) {
    return at <= d.size() && count <= d.size() - at;
}


inline std::vector<GclGlobal> scan_gcl(const std::string& d, size_t* total_ctors = nullptr) {
    std::vector<GclGlobal> out; size_t total = 0, pos = 0; const std::string tag = ".$ctor ()";
    while ((pos = d.find(tag, pos)) != std::string::npos) {
        size_t endstr = pos + tag.size(); pos = endstr;
        size_t s = std::string::npos;
        for (size_t back = 6; back < 400 && back < endstr; back++) {                  
            size_t c = endstr - back; if (d.compare(c, 3, "() ") != 0 || c < 4) continue;
            if (gu32(d, c - 4) == endstr - c) { s = c; break; }
        }
        if (s == std::string::npos) continue;
        total++;
        try {
            std::string name = d.substr(s + 3, pos - 9 - (s + 3) + 9 - 9 + 0);        
            name = d.substr(s + 3, (endstr - tag.size()) - (s + 3));
            size_t p = endstr; if (!bytes_ok(d, p, 12)) continue;
            uint32_t nref = gu32(d, p + 8); if (nref != 1) continue;                  
            size_t q = p + 12; if (gu32(d, q) != 3) continue;                         
            uint32_t ln = gu32(d, q + 4); q += 8 + ln; q += 4;                         
            uint32_t size = gu32(d, q); q += 4; size_t c = q; if (!bytes_ok(d, c, size) || size < 24) continue;
            auto b = [&](size_t i) { return (unsigned char)d[c + i]; };
            GclGlobal g; g.name = name;
            if (b(0) == 0x48 && b(1) == 0x8b && b(2) == 0x0d && b(7) == 0x8b && b(8) == 0x05 && b(13) == 0x89 && b(14) == 0x01 && b(15) == 0xc3) {
                g.type = "s32"; g.off = c + 13 + gi32(d, c + 9);
            } else if (b(0) == 0x48 && b(1) == 0x8b && b(2) == 0x0d && b(7) == 0x8a && b(8) == 0x05 && b(13) == 0x88 && b(14) == 0x01 && b(15) == 0xc3) {
                g.type = "bool"; g.off = c + 13 + gi32(d, c + 9);
            } else if (b(0) == 0x48 && b(1) == 0x8b && b(2) == 0x05 && b(7) == 0xf2 && b(8) == 0x0f && b(9) == 0x10 && b(10) == 0x05 &&
                       b(15) == 0xf2 && b(16) == 0x0f && b(17) == 0x11 && b(18) == 0x00 && b(19) == 0xc3) {
                g.type = "f64"; g.off = c + 15 + gi32(d, c + 11);
            } else continue;
            size_t width = g.type == "f64" ? 8 : g.type == "s32" ? 4 : 1;
            if (g.off < c || g.off + width > c + size) continue;                       
            out.push_back(g);
        } catch (...) {}
    }
    if (total_ctors) *total_ctors = total;
    return out;
}
inline json gcl_value(const std::string& d, const GclGlobal& g) {
    if (g.type == "f64") { double v; std::memcpy(&v, d.data() + g.off, 8); return v; }
    if (g.type == "s32") return gi32(d, g.off);
    return d[g.off] != 0;
}
inline const GclGlobal* find_global(const std::vector<GclGlobal>& gs, const std::string& name, std::string& why) {
    for (auto& g : gs) if (g.name == name) return &g;
    const GclGlobal* hit = nullptr; int n = 0;
    for (auto& g : gs) if (g.name.size() > name.size() && g.name.compare(g.name.size() - name.size(), name.size(), name) == 0 && g.name[g.name.size() - name.size() - 1] == '.') { hit = &g; n++; }
    if (n == 1) return hit;
    why = n > 1 ? "ambiguous name, use the full one (e.g. " + hit->name + ")" : "no tweakable global with that name (run: modtool tweaks <game> " + name + ")";
    return nullptr;
}

inline bool apply_tweak(std::string& d, const GclGlobal& g, const json& spec, std::string& msg) {
    json cur = gcl_value(d, g); double base = cur.is_boolean() ? (cur.get<bool>() ? 1 : 0) : cur.get<double>(), nv;
    if (spec.is_boolean()) nv = spec.get<bool>() ? 1 : 0;
    else if (spec.is_number()) nv = spec.get<double>();
    else if (spec.is_object() && spec.contains("scale") && spec["scale"].is_number()) nv = base * spec["scale"].get<double>();
    else if (spec.is_object() && spec.contains("add") && spec["add"].is_number()) nv = base + spec["add"].get<double>();
    else { msg = "value must be a number, true/false, {\"scale\":x} or {\"add\":x}"; return false; }
    if (!std::isfinite(nv)) { msg = "value is not finite"; return false; }
    if (g.type == "s32") {
        if (nv != std::floor(nv) && !(spec.is_object())) { msg = "this is a whole-number (s32) setting"; return false; }
        nv = std::round(nv); if (nv < -2147483648.0 || nv > 2147483647.0) { msg = "out of range for s32"; return false; }
        int32_t v = (int32_t)nv; std::memcpy(&d[g.off], &v, 4);
    } else if (g.type == "f64") std::memcpy(&d[g.off], &nv, 8);
    else d[g.off] = nv != 0 ? 1 : 0;
    msg = cur.dump() + " -> " + gcl_value(d, g).dump(); return true;
}

inline int apply_tweaks(std::string& d, const json& tweaks, std::vector<std::string>& log) {
    if (!tweaks.is_object() || tweaks.empty()) return 0;
    auto gs = scan_gcl(d); int n = 0;
    for (auto it = tweaks.begin(); it != tweaks.end(); ++it) {
        std::string why, msg; const GclGlobal* g = find_global(gs, it.key(), why);
        if (!g) { log.push_back("WARN tweak '" + it.key() + "': " + why); continue; }
        if (apply_tweak(d, *g, it.value(), msg)) { log.push_back("  = tweak " + g->name + ": " + msg); n++; }
        else log.push_back("WARN tweak '" + it.key() + "': " + msg);
    }
    return n;
}
} 
