
#pragma once
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#include "json.hpp"

namespace am {
namespace fs = std::filesystem;
using json = nlohmann::ordered_json;

struct Error : std::runtime_error { using std::runtime_error::runtime_error; };

static const char* const DEF_FILES[] = {"vehicle_component_definitions", "inventory_definitions", "creature_definitions",
    "zombie_definitions", "prop_definitions", "tile_definitions", "tile_junction_definitions", "dungeon_tile_definitions"};
static const char* const ASSET_DIRS[] = {"meshes", "textures", "animations", "audio"};

struct Mod { std::string id; fs::path dir; bool enabled = true; int load_order = 0; std::vector<std::string> depends; json raw; };
struct Result {
    std::map<std::string, std::string> files;   
    std::map<std::string, fs::path> assets;     
    std::vector<std::string> log, warns;
    json tweaks = json::object();               
    std::vector<fs::path> natives;              
    int mods = 0;
};

inline std::string read_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) throw Error("cannot read " + p.u8string());
    std::ostringstream ss; ss << f.rdbuf(); return ss.str();
}
inline json parse_json(std::string s, const std::string& what) {
    if (s.size() >= 3 && (unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF) s.erase(0, 3);
    try { return json::parse(s, nullptr, true, true); }
    catch (const std::exception& e) { throw Error(what + ": invalid JSON (" + e.what() + ")"); }
}
inline json read_json(const fs::path& p) { return parse_json(read_file(p), p.filename().u8string()); }
inline bool truthy(const json& e, const char* k) { return e.contains(k) && e[k].is_boolean() && e[k].get<bool>(); }
inline bool starts_with(const std::string& s, const std::string& p) { return s.rfind(p, 0) == 0; }
inline bool has_ext(const fs::path& p, const char* ext) {
    return p.has_extension() && p.extension().u8string() == ext;
}
inline bool regular_file(const fs::path& p) {
    std::error_code ec;
    return fs::is_regular_file(p, ec);
}
inline std::string clean_rel(const fs::path& p) {
    return p.lexically_normal().generic_u8string();
}


inline void deep_merge(json& base, const json& over) {
    for (auto it = over.begin(); it != over.end(); ++it) {
        const std::string& k = it.key(); const json& v = it.value();
        if (!k.empty() && k[0] == '$') continue;
        if (k.size() > 1 && k.back() == '+') {
            std::string b = k.substr(0, k.size() - 1);
            if (!base.contains(b) || !base[b].is_array()) base[b] = json::array();
            for (const auto& x : v) base[b].push_back(x);
        } else if (v.is_object() && base.contains(k) && base[k].is_object()) deep_merge(base[k], v);
        else base[k] = v;
    }
}
inline json rewrite_assets(const json& o, const std::string& mod_id, bool strip_audio) {
    if (o.is_string()) {
        std::string s = o.get<std::string>();
        if (starts_with(s, "@mod/")) {
            std::string rest = s.substr(5); size_t sl = rest.find('/');
            std::string top = sl == std::string::npos ? rest : rest.substr(0, sl), tail = sl == std::string::npos ? "" : rest.substr(sl + 1);
            std::string out = top + "/_mods/" + mod_id + "/" + tail;
            if (strip_audio && starts_with(out, "audio/")) out = out.substr(6);
            return out;
        }
        return o;
    }
    if (o.is_array()) { json a = json::array(); for (const auto& x : o) a.push_back(rewrite_assets(x, mod_id, strip_audio)); return a; }
    if (o.is_object()) { json r = json::object(); for (auto it = o.begin(); it != o.end(); ++it) r[it.key()] = rewrite_assets(it.value(), mod_id, strip_audio); return r; }
    return o;
}
inline void apply_entries(json& defs, const json& entries, const std::string& mod, const std::string& label,
                          std::vector<std::string>& log, const char* key, bool strip_audio) {
    auto err = [&](const std::string& m) { throw Error("[" + mod + "] " + label + ": " + m); };
    for (const json& e0 : entries) {
        json e = rewrite_assets(e0, mod, strip_audio);
        if (!e.is_object() || !e.contains(key) || !e[key].is_string()) err(std::string("entry without '") + key + "'");
        std::string eid = e[key].get<std::string>();
        auto find = [&](const std::string& id) -> json* {
            for (auto& d : defs) if (d.is_object() && d.contains(key) && d[key] == id) return &d;
            return nullptr; };
        if (truthy(e, "$remove")) {
            json* t = find(eid); if (!t) err("cannot remove unknown '" + eid + "'");
            defs.erase(defs.begin() + (t - &defs[0])); log.push_back("  - " + label + ": removed " + eid);
        } else if (truthy(e, "$patch")) {
            json* t = find(eid); if (!t) err("cannot patch unknown '" + eid + "'");
            deep_merge(*t, e); log.push_back("  ~ " + label + ": patched " + eid);
        } else {
            json nu;
            if (e.contains("$inherit") && e["$inherit"].is_string() && !e["$inherit"].get<std::string>().empty()) {
                std::string base = e["$inherit"].get<std::string>(); json* b = find(base);
                if (!b) err("'" + eid + "' inherits unknown '" + base + "'");
                nu = *b; deep_merge(nu, e); nu[key] = eid;
            } else { nu = json::object(); for (auto it = e.begin(); it != e.end(); ++it) if (it.key().empty() || it.key()[0] != '$') nu[it.key()] = it.value(); }
            json* ex = find(eid);
            if (ex) { if (!truthy(e, "$replace")) err("'" + eid + "' already exists (use $patch or $replace)"); *ex = nu; }
            else defs.push_back(nu);
            log.push_back("  + " + label + ": added " + eid);
        }
    }
}


struct Tsv {
    std::vector<std::vector<std::string>> rows; std::map<std::string, int> col; bool crlf = false, tail = false; std::map<std::string, size_t> index;
    explicit Tsv(const std::string& text) {
        crlf = text.find("\r\n") != std::string::npos;
        std::string t; t.reserve(text.size());
        for (size_t i = 0; i < text.size(); i++) { if (text[i] == '\r' && i + 1 < text.size() && text[i + 1] == '\n') continue; t += text[i]; }
        std::vector<std::string> lines; size_t s = 0;
        for (size_t i = 0; i <= t.size(); i++) if (i == t.size() || t[i] == '\n') { lines.push_back(t.substr(s, i - s)); s = i + 1; }
        tail = !lines.empty() && lines.back().empty(); if (tail) lines.pop_back();
        for (auto& l : lines) { std::vector<std::string> r; size_t a = 0; for (size_t i = 0; i <= l.size(); i++) if (i == l.size() || l[i] == '\t') { r.push_back(l.substr(a, i - a)); a = i + 1; } rows.push_back(r); }
        if (rows.empty()) throw Error("empty language table");
        for (size_t i = 0; i < rows[0].size(); i++) { std::string h = rows[0][i]; col[(h == "id" && i > 0) ? "id_lang" : h] = (int)i; }
        for (size_t i = 1; i < rows.size(); i++) index[rows[i][0]] = i;
    }
    void set(const std::string& id, const std::map<std::string, std::string>& langs, const std::string& context) {
        auto it = index.find(id);
        if (it == index.end()) {
            std::vector<std::string> r(rows[0].size()); r[0] = id; r[col["context"]] = context;
            auto en = langs.find("en");
            for (auto& kv : col) if (kv.second > col["context"]) r[kv.second] = en == langs.end() ? "" : en->second;
            rows.push_back(r); it = index.emplace(id, rows.size() - 1).first;
        }
        for (auto& kv : langs) {
            auto c = col.find(kv.first);
            if (c == col.end()) { std::string v; for (auto& x : col) v += (v.empty() ? "" : ", ") + x.first; throw Error("unknown language column '" + kv.first + "' (valid: " + v + ")"); }
            rows[it->second][c->second] = kv.second;
        }
    }
    std::string dump() const {
        std::string nl = crlf ? "\r\n" : "\n", o;
        for (size_t i = 0; i < rows.size(); i++) { if (i) o += nl; for (size_t j = 0; j < rows[i].size(); j++) { if (j) o += '\t'; o += rows[i][j]; } }
        return o + (tail ? nl : "");
    }
};
inline std::string loc_file(const std::string& n) { return (n == "ui" || n.empty()) ? "languages.tsv" : "languages_" + n + ".tsv"; }


inline uint32_t crc32_of(const std::string& d) {
    static uint32_t T[256]; static bool init = false;
    if (!init) { for (uint32_t i = 0; i < 256; i++) { uint32_t c = i; for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1; T[i] = c; } init = true; }
    uint32_t c = 0xFFFFFFFFu; for (unsigned char ch : d) c = T[(c ^ ch) & 0xFF] ^ (c >> 8); return c ^ 0xFFFFFFFFu;
}
inline std::vector<std::string> zip_names(const std::string& z) {
    auto u16 = [&](size_t o) { return (uint32_t)((unsigned char)z[o] | ((unsigned char)z[o + 1] << 8)); };
    auto u32 = [&](size_t o) { return u16(o) | (u16(o + 2) << 16); };
    std::vector<std::string> names; if (z.size() < 22) return names;
    size_t e = std::string::npos; for (size_t i = z.size() - 22 + 1; i-- > 0;) if (u32(i) == 0x06054b50) { e = i; break; }
    if (e == std::string::npos) return names;
    size_t n = u16(e + 10), o = u32(e + 16);
    for (size_t i = 0; i < n && o + 46 <= z.size() && u32(o) == 0x02014b50; i++) {
        size_t nl = u16(o + 28), xl = u16(o + 30), cl = u16(o + 32);
        names.push_back(z.substr(o + 46, nl)); o += 46 + nl + xl + cl;
    }
    return names;
}
inline std::string zip_store(const std::vector<std::pair<std::string, std::string>>& files) {
    std::string out, cd; auto p16 = [](std::string& s, uint32_t v) { s += (char)(v & 255); s += (char)((v >> 8) & 255); };
    auto p32 = [&](std::string& s, uint32_t v) { p16(s, v & 0xFFFF); p16(s, v >> 16); };
    for (auto& f : files) {
        uint32_t crc = crc32_of(f.second), off = (uint32_t)out.size(), sz = (uint32_t)f.second.size();
        p32(out, 0x04034b50); p16(out, 20); p16(out, 0); p16(out, 0); p16(out, 0); p16(out, 0x21); p32(out, crc); p32(out, sz); p32(out, sz);
        p16(out, (uint32_t)f.first.size()); p16(out, 0); out += f.first; out += f.second;
        p32(cd, 0x02014b50); p16(cd, 20); p16(cd, 20); p16(cd, 0); p16(cd, 0); p16(cd, 0); p16(cd, 0x21); p32(cd, crc); p32(cd, sz); p32(cd, sz);
        p16(cd, (uint32_t)f.first.size()); p16(cd, 0); p16(cd, 0); p16(cd, 0); p16(cd, 0); p32(cd, 0); p32(cd, off); cd += f.first;
    }
    uint32_t cdo = (uint32_t)out.size(); out += cd;
    p32(out, 0x06054b50); p16(out, 0); p16(out, 0); p16(out, (uint32_t)files.size()); p16(out, (uint32_t)files.size()); p32(out, (uint32_t)cd.size()); p32(out, cdo); p16(out, 0);
    return out;
}


inline std::vector<Mod> discover(const fs::path& mods_dir) {
    std::vector<Mod> mods; std::map<std::string, std::string> seen;
    if (!fs::is_directory(mods_dir)) return mods;
    std::vector<fs::path> dirs; for (auto& d : fs::directory_iterator(mods_dir)) dirs.push_back(d.path());
    std::sort(dirs.begin(), dirs.end());
    for (auto& d : dirs) {
        fs::path mj = d / "mod.json"; std::string dn = d.filename().u8string();
        if (!fs::is_directory(d) || dn.empty() || dn[0] == '.' || !fs::exists(mj)) continue;
        json m = parse_json(read_file(mj), dn + "/mod.json"); Mod mod; mod.raw = m; mod.dir = d;
        if (!m.is_object() || !m.contains("id") || !m["id"].is_string()) throw Error(dn + "/mod.json: 'id' required (lowercase, digits, _)");
        mod.id = m["id"].get<std::string>();
        for (char c : mod.id) if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) throw Error(dn + "/mod.json: 'id' must be lowercase letters, digits, _");
        if (mod.id.empty()) throw Error(dn + "/mod.json: 'id' required");
        if (seen.count(mod.id)) throw Error("duplicate mod id '" + mod.id + "' (" + dn + " and " + seen[mod.id] + ")");
        seen[mod.id] = dn;
        if (m.contains("enabled") && m["enabled"].is_boolean()) mod.enabled = m["enabled"].get<bool>();
        if (m.contains("load_order") && m["load_order"].is_number()) mod.load_order = m["load_order"].get<int>();
        if (m.contains("depends") && m["depends"].is_array()) for (auto& x : m["depends"]) if (x.is_string()) mod.depends.push_back(x.get<std::string>());
        mods.push_back(mod);
    }
    std::stable_sort(mods.begin(), mods.end(), [](const Mod& a, const Mod& b) { return a.load_order != b.load_order ? a.load_order < b.load_order : a.id < b.id; });
    std::vector<Mod> ordered; std::set<std::string> done;
    std::function<void(const Mod&, std::vector<std::string>)> visit = [&](const Mod& m, std::vector<std::string> stack) {
        if (done.count(m.id)) return;
        if (std::find(stack.begin(), stack.end(), m.id) != stack.end()) throw Error("dependency cycle at '" + m.id + "'");
        for (auto& dep : m.depends) {
            auto it = std::find_if(mods.begin(), mods.end(), [&](const Mod& x) { return x.id == dep; });
            if (it == mods.end()) throw Error("mod '" + m.id + "' needs missing mod '" + dep + "'");
            auto s2 = stack; s2.push_back(m.id); visit(*it, s2);
        }
        done.insert(m.id); ordered.push_back(m);
    };
    for (auto& m : mods) visit(m, {});
    return ordered;
}


inline Result build(const fs::path& rom, const fs::path& mods_dir) {
    Result R; auto mods = discover(mods_dir);
    std::map<std::string, json> J; std::map<std::string, Tsv> T; std::set<std::string> touched; std::set<std::string> replaced;
    auto getj = [&](const std::string& rel) -> json& { auto it = J.find(rel); if (it == J.end()) it = J.emplace(rel, read_json(rom / fs::path(rel))).first; return it->second; };
    auto gett = [&](const std::string& rel) -> Tsv& { auto it = T.find(rel); if (it == T.end()) it = T.emplace(rel, Tsv(read_file(rom / fs::path(rel)))).first; return it->second; };
    auto sorted_files = [](const fs::path& d, const std::string& ext) { std::vector<fs::path> v;
        if (fs::is_directory(d)) for (auto& e : fs::directory_iterator(d)) if (regular_file(e.path()) && e.path().extension() == ext) v.push_back(e.path());
        std::sort(v.begin(), v.end()); return v; };
    auto safe_rel = [](const fs::path& rel) -> std::string {
        fs::path n = rel.lexically_normal();
        if (n.empty() || n.is_absolute()) throw Error("replacement path must be relative");
        for (auto& part : n) if (part == "..") throw Error("replacement path may not contain '..'");
        return n.generic_u8string();
    };
    for (auto& m : mods) {
        if (!m.enabled) continue; R.mods++;
        R.log.push_back("mod " + m.id);
        for (auto& dll : sorted_files(m.dir / "native", ".dll")) { R.natives.push_back(fs::absolute(dll)); R.log.push_back("  native plugin " + m.id + "/native/" + dll.filename().u8string()); }
        for (const char* nn : DEF_FILES) {
            std::string n = nn; fs::path p = m.dir / "data" / (n + ".json"); if (!fs::exists(p)) continue;
            std::string rel = "data/" + n + ".json";
            if (!fs::exists(rom / fs::path(rel))) throw Error("[" + m.id + "] " + rel + " is not a known game file");
            json& s = getj(rel); json entries = read_json(p).value("definitions", json::array());
            apply_entries(s["definitions"], entries, m.id, n, R.log, "id", false); touched.insert(rel);
            if (n == "vehicle_component_definitions" || n == "inventory_definitions") {
                bool veh = n[0] == 'v'; std::string t1 = veh ? "components" : "items", t2 = veh ? "component_descriptions" : "item_descriptions";
                std::set<std::string> have; fs::path lj = m.dir / "languages" / (t1 + ".json");
                if (fs::exists(lj)) { json lo = read_json(lj); for (auto it = lo.begin(); it != lo.end(); ++it) have.insert(it.key()); }
                for (const json& e : entries) {
                    if (truthy(e, "$patch") || truthy(e, "$remove")) continue;
                    std::string eid = e["id"].get<std::string>(); const json* full = nullptr;
                    for (auto& d : s["definitions"]) if (d.contains("id") && d["id"] == eid) { full = &d; break; }
                    if (!full) continue;
                    for (int k = 0; k < 2; k++) {
                        const char* fld = k == 0 ? "name" : "description";
                        if (!full->contains(fld) || !(*full)[fld].is_string() || (*full)[fld].get<std::string>().empty()) continue;
                        if (k == 0 && have.count(eid)) continue;
                        std::string tf = loc_file(k == 0 ? t1 : t2); Tsv& t = gett(tf);
                        if (!t.index.count(eid)) { t.set(eid, {{"en", (*full)[fld].get<std::string>()}}, "added by mod " + m.id); touched.insert(tf); }
                    }
                }
            }
        }
        for (auto& p : sorted_files(m.dir / "audio", ".slib")) {
            std::string rel = "audio/" + p.filename().u8string();
            if (!fs::exists(rom / fs::path(rel))) throw Error("[" + m.id + "] " + rel + " is not a known sound library");
            json& s = getj(rel); json entries = read_json(p).value("descriptors", json::array());
            apply_entries(s["descriptors"], entries, m.id, p.filename().u8string(), R.log, "name", true); touched.insert(rel);
        }
        for (auto& p : sorted_files(m.dir / "languages", ".json")) {
            std::string rel = loc_file(p.stem().u8string());
            if (!fs::exists(rom / fs::path(rel))) throw Error("[" + m.id + "] languages/" + p.filename().u8string() + ": no such table " + rel);
            Tsv& t = gett(rel); json lo = read_json(p);
            for (auto it = lo.begin(); it != lo.end(); ++it) {
                std::map<std::string, std::string> langs; for (auto l = it.value().begin(); l != it.value().end(); ++l) langs[l.key()] = l.value().get<std::string>();
                t.set(it.key(), langs, "added by mod " + m.id); R.log.push_back("  + " + rel + ": " + it.key());
            }
            touched.insert(rel);
        }
        if (fs::exists(m.dir / "tweaks.json")) {
            json tw = read_json(m.dir / "tweaks.json");
            if (!tw.is_object()) throw Error("[" + m.id + "] tweaks.json must be an object {\"name\": value}");
            for (auto it = tw.begin(); it != tw.end(); ++it) { R.tweaks[it.key()] = it.value(); R.log.push_back("  = tweaks.json: " + it.key()); }
        }
        
        
        
        
        fs::path rr = m.dir / "replace";
        if (fs::is_directory(rr)) {
            for (auto& f : fs::recursive_directory_iterator(rr)) {
                if (!f.is_regular_file()) continue;
                std::string rel = safe_rel(fs::relative(f.path(), rr));
                rel = clean_rel(fs::path(rel));
                if (!fs::exists(rom / fs::path(rel))) {
                    throw Error("[" + m.id + "] replace/ targets unknown game file " + rel);
                }
                R.files[rel] = read_file(f.path());
                replaced.insert(rel);
                touched.insert(rel);
                R.log.push_back("  ! replace: " + rel);
            }
        }
        for (const char* top : ASSET_DIRS) {
            fs::path root = m.dir / top; if (!fs::is_directory(root)) continue;
            for (auto& f : fs::recursive_directory_iterator(root))
                if (regular_file(f.path()) && f.path().extension() != ".slib")
                    R.assets[std::string(top) + "/_mods/" + m.id + "/" + fs::relative(f.path(), root).generic_u8string()] = f.path();
        }
    }
    std::function<void(const json&, const std::string&)> walk = [&](const json& o, const std::string& ctx) {
        if (o.is_string()) { std::string s = o.get<std::string>();
            if (s.size() > 5 && s.compare(s.size() - 5, 5, ".mesh") == 0 && s.find("/_mods/") != std::string::npos && !R.assets.count(s)) R.warns.push_back("missing mesh '" + s + "' used by " + ctx); }
        else if (o.is_array() || o.is_object()) for (const auto& x : o) walk(x, ctx);
    };
    for (auto& kv : J) {
        if (kv.second.contains("definitions")) for (auto& d : kv.second["definitions"]) walk(d, kv.first + ":" + (d.contains("id") && d["id"].is_string() ? d["id"].get<std::string>() : "?"));
        if (kv.second.contains("descriptors")) for (auto& d : kv.second["descriptors"]) if (d.contains("paths"))
            for (auto& pth : d["paths"]) { std::string s = pth.get<std::string>();
                if (s.find("_mods/") != std::string::npos && !R.assets.count("audio/" + s + ".ogg")) R.warns.push_back("missing sound 'audio/" + s + ".ogg' used by " + d["name"].get<std::string>()); }
    }
    for (auto& rel : touched) {
        
        
        
        if (replaced.count(rel)) continue;
        if (rel.size() > 4 && rel.compare(rel.size() - 4, 4, ".tsv") == 0) R.files[rel] = T.at(rel).dump();
        else {
            bool slib = rel.compare(rel.size() - 5, 5, ".slib") == 0;
            std::string s = J.at(rel).dump(slib ? 4 : 1, ' ', false);
            if (slib) { std::string c; for (char ch : s) { if (ch == '\n') c += '\r'; c += ch; } s = c; }
            R.files[rel] = s;
        }
    }
    bool anyData = false; for (auto& t : touched) if (starts_with(t, "data/") && t.size() > 5 && t.compare(t.size() - 5, 5, ".json") == 0) anyData = true;
    fs::path dz = rom / "data" / "data.zip";
    if (anyData && fs::exists(dz)) {   
        std::vector<std::pair<std::string, std::string>> entries; bool ok = true;
        for (auto& name : zip_names(read_file(dz))) {
            auto it = R.files.find("data/" + name);
            if (it != R.files.end()) entries.push_back({name, it->second});
            else if (fs::exists(rom / "data" / name)) entries.push_back({name, read_file(rom / "data" / name)});
            else { ok = false; R.warns.push_back("data.zip entry '" + name + "' has no loose file; data.zip left unchanged"); break; }
        }
        if (ok) R.files["data/data.zip"] = zip_store(entries);
    }
    return R;
}
} 
