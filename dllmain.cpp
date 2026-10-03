#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <atomic>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "gcl_tweaks.hpp"

namespace {
HMODULE g_self; am::fs::path g_dir; std::wstring g_romPrefix;
std::unordered_map<std::wstring, std::wstring> g_map;
std::once_flag g_once; bool g_ready = false; thread_local bool t_busy = false, t_init = false;
std::vector<std::wstring> g_hookPrefixes;
struct NatRange { BYTE* base; size_t size; const wchar_t* exe; };
NatRange g_nat[64]; std::atomic<int> g_natN{0}; std::deque<std::wstring> g_exeStrs; const wchar_t* g_loadingExe = nullptr;

void logmsg(const std::string& s) {
    static std::mutex m; std::lock_guard<std::mutex> g(m);
    std::ofstream f(g_dir / "mods" / "mod_loader.log", std::ios::app); f << s << "\n";
}
std::wstring lower_path(std::wstring s) {
    if (s.empty()) return s;
    CharLowerBuffW(&s[0], (DWORD)s.size());
    return s;
}
std::wstring norm(std::wstring s) {
    if (s.rfind(L"\\\\?\\", 0) == 0) s = s.substr(4);
    for (auto& c : s) if (c == L'/') c = L'\\';
    if (!s.empty()) s = lower_path(s);
    return s;
}
void hook_all();
am::fs::path run_natives(const am::Result& R, const am::fs::path& cache, const am::fs::path& gcl, am::fs::path cur);
void init() {
    t_busy = true; t_init = true;
    try {
        am::fs::path rom = g_dir / "rom", mods = g_dir / "mods";
        if (!am::fs::is_directory(rom) || !am::fs::is_directory(mods)) { t_busy = false; t_init = false; return; }
        std::error_code ec; am::fs::remove(mods / "Anymodder.log", ec);
        logmsg("AnyModder loaded.");
        am::Result R = am::build(rom, mods);
        for (auto& l : R.log) logmsg(l); for (auto& w : R.warns) logmsg("WARN " + w);
        if (R.mods == 0) { logmsg("no enabled mods - game runs untouched"); t_busy = false; t_init = false; return; }
        am::fs::path cache = mods / ".cache"; am::fs::remove_all(cache / "rom", ec);
        for (auto& kv : R.files) {
            am::fs::path p = cache / "rom" / am::fs::path(kv.first); am::fs::create_directories(p.parent_path());
            std::ofstream(p, std::ios::binary) << kv.second;
            g_map[norm((rom / am::fs::path(kv.first)).wstring())] = p.wstring();
        }
        for (auto& kv : R.assets) g_map[norm((rom / am::fs::path(kv.first)).wstring())] = kv.second.wstring();
        am::fs::path gcl = g_dir / "bin" / "game.gcl", wdir = cache / "gcl", work = wdir / "game.gcl";
        am::fs::path stage0 = wdir / "game.gcl.tweaked", keyf = wdir / "game.gcl.key";
        bool haveGcl = am::fs::exists(gcl), haveStage0 = false;
        if (!R.tweaks.empty()) {
            if (!haveGcl) logmsg("WARN tweaks.json used but bin\\game.gcl not found");
            else {
                std::string key = std::to_string(am::fs::file_size(gcl)) + "|" + std::to_string(am::fs::last_write_time(gcl).time_since_epoch().count()) + "|" + R.tweaks.dump();
                bool reuse = am::fs::exists(stage0) && am::fs::exists(keyf) && am::read_file(keyf) == key && am::fs::file_size(stage0) == am::fs::file_size(gcl);
                if (reuse) logmsg("tweaks unchanged - using cached tweaked game.gcl");
                else {
                    std::string d = am::read_file(gcl); std::vector<std::string> tl; int n = am::apply_tweaks(d, R.tweaks, tl);
                    for (auto& l : tl) logmsg(l);
                    am::fs::remove(stage0, ec);
                    if (n > 0) { am::fs::create_directories(wdir); std::ofstream(stage0, std::ios::binary) << d; std::ofstream(keyf, std::ios::binary) << key; }
                }
                haveStage0 = am::fs::exists(stage0);
            }
        }
        bool useWork = haveGcl && (haveStage0 || !R.natives.empty());
        std::uintmax_t origSize = haveGcl ? am::fs::file_size(gcl) : 0; auto origTime = haveGcl ? am::fs::last_write_time(gcl) : am::fs::file_time_type{};
        g_romPrefix = norm(rom.wstring() + L"\\");
        if (useWork) {
            am::fs::path cur = haveStage0 ? stage0 : gcl;
            if (!R.natives.empty()) { g_ready = true; t_busy = false; cur = run_natives(R, cache, gcl, cur); t_busy = true; }
            am::fs::create_directories(wdir); am::fs::copy_file(cur, work, am::fs::copy_options::overwrite_existing);
            g_map[norm(gcl.wstring())] = work.wstring();
            std::string fin = am::read_file(work), org = am::read_file(gcl);
            logmsg("final game.gcl: " + std::to_string(fin.size()) + " bytes" + (fin == org ? " (identical to original)" : " (modified)"));
            if (am::fs::file_size(gcl) != origSize || am::fs::last_write_time(gcl) != origTime) logmsg("WARN original bin\\game.gcl changed while mods loaded");
        } else if (!R.natives.empty()) logmsg("WARN bin\\game.gcl not found - native mods skipped (nothing to patch)");
        logmsg("ready: " + std::to_string(R.files.size()) + " file(s) replaced, " + std::to_string(R.assets.size()) + " asset(s) added");
        g_ready = true;
    } catch (const std::exception& e) { logmsg(std::string("ERROR (mods disabled, game runs untouched): ") + e.what()); }
    t_busy = false; t_init = false;
}
bool resolve(LPCWSTR name, std::wstring& out) {
    if (t_busy || !name || !*name) return false;
    if (!t_init) std::call_once(g_once, init);
    if (!g_ready) return false;
    wchar_t buf[2048]; DWORD n = GetFullPathNameW(name, 2048, buf, nullptr);
    if (!n || n >= 2048) return false;
    std::wstring key = norm(std::wstring(buf, n));
    auto it = g_map.find(key); if (it == g_map.end()) return false;
    out = it->second;
    { 
        static std::mutex m; static std::unordered_set<std::wstring> seen; std::lock_guard<std::mutex> g(m);
        if (seen.size() < 300 && seen.insert(key).second) {
            t_busy = true; int n = WideCharToMultiByte(CP_UTF8, 0, out.c_str(), -1, nullptr, 0, nullptr, nullptr); std::string u(n > 0 ? n - 1 : 0, '\0');
            if (n > 1) WideCharToMultiByte(CP_UTF8, 0, out.c_str(), -1, &u[0], n, nullptr, nullptr);
            int k = WideCharToMultiByte(CP_UTF8, 0, key.c_str(), -1, nullptr, 0, nullptr, nullptr); std::string kk(k > 0 ? k - 1 : 0, '\0');
            if (k > 1) WideCharToMultiByte(CP_UTF8, 0, key.c_str(), -1, &kk[0], k, nullptr, nullptr);
            logmsg("redirected: " + kk + " -> " + u); t_busy = false;
        }
    }
    return true;
}

typedef HANDLE(WINAPI* CreateFileW_t)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
typedef HANDLE(WINAPI* CreateFileA_t)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
typedef HANDLE(WINAPI* CreateFile2_t)(LPCWSTR, DWORD, DWORD, DWORD, void*);
typedef DWORD(WINAPI* GetFileAttributesW_t)(LPCWSTR);
typedef BOOL(WINAPI* GetFileAttributesExW_t)(LPCWSTR, GET_FILEEX_INFO_LEVELS, LPVOID);
CreateFileW_t real_CreateFileW; CreateFileA_t real_CreateFileA; CreateFile2_t real_CreateFile2;
GetFileAttributesW_t real_GetFileAttributesW; GetFileAttributesExW_t real_GetFileAttributesExW;

HANDLE WINAPI hk_CreateFileW(LPCWSTR n, DWORD a, DWORD s, LPSECURITY_ATTRIBUTES sa, DWORD d, DWORD f, HANDLE t) {
    std::wstring r; if (resolve(n, r)) return real_CreateFileW(r.c_str(), a, s, sa, d, f, t);
    return real_CreateFileW(n, a, s, sa, d, f, t);
}
HANDLE WINAPI hk_CreateFileA(LPCSTR n, DWORD a, DWORD s, LPSECURITY_ATTRIBUTES sa, DWORD d, DWORD f, HANDLE t) {
    if (n) { wchar_t w[2048]; int len = MultiByteToWideChar(CP_ACP, 0, n, -1, w, 2048); std::wstring r;
        if (len > 0 && resolve(w, r)) return real_CreateFileW(r.c_str(), a, s, sa, d, f, t); }
    return real_CreateFileA(n, a, s, sa, d, f, t);
}
HANDLE WINAPI hk_CreateFile2(LPCWSTR n, DWORD a, DWORD s, DWORD d, void* p) {
    std::wstring r; if (resolve(n, r)) return real_CreateFile2(r.c_str(), a, s, d, p);
    return real_CreateFile2(n, a, s, d, p);
}
DWORD WINAPI hk_GetFileAttributesW(LPCWSTR n) {
    std::wstring r; if (resolve(n, r)) return real_GetFileAttributesW(r.c_str());
    return real_GetFileAttributesW(n);
}
BOOL WINAPI hk_GetFileAttributesExW(LPCWSTR n, GET_FILEEX_INFO_LEVELS l, LPVOID p) {
    std::wstring r; if (resolve(n, r)) return real_GetFileAttributesExW(r.c_str(), l, p);
    return real_GetFileAttributesExW(n, l, p);
}

bool is_file_api_dll(const char* n) { return !_stricmp(n, "KERNEL32.dll") || !_stricmp(n, "KERNELBASE.dll") || !_strnicmp(n, "api-ms-win-core-", 16); }
bool patch_iat(HMODULE mod, const char* fn, void* hook, void** orig) {
    BYTE* base = (BYTE*)mod; auto dos = (IMAGE_DOS_HEADER*)base; if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    auto nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew); bool any = false;
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT]; if (!dir.VirtualAddress) return false;
    for (auto d = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); d->Name; ++d) {
        if (!is_file_api_dll((char*)(base + d->Name))) continue;
        auto thunk = (IMAGE_THUNK_DATA*)(base + d->FirstThunk);
        auto oft = (IMAGE_THUNK_DATA*)(base + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk));
        for (; oft->u1.AddressOfData; ++oft, ++thunk) {
            if (IMAGE_SNAP_BY_ORDINAL(oft->u1.Ordinal)) continue;
            if (strcmp((char*)((IMAGE_IMPORT_BY_NAME*)(base + oft->u1.AddressOfData))->Name, fn)) continue;
            if (thunk->u1.Function == (ULONG_PTR)hook) { any = true; break; }
            DWORD old; VirtualProtect(&thunk->u1.Function, sizeof(void*), PAGE_READWRITE, &old);
            if (orig) { *orig = (void*)thunk->u1.Function; }
            thunk->u1.Function = (ULONG_PTR)hook;
            VirtualProtect(&thunk->u1.Function, sizeof(void*), old, &old); any = true; break;
        }
    }
    return any;
}
void hook_all() {
    HMODULE exe = GetModuleHandleW(nullptr);
    patch_iat(exe, "CreateFileW", (void*)hk_CreateFileW, (void**)&real_CreateFileW);
    patch_iat(exe, "CreateFileA", (void*)hk_CreateFileA, (void**)&real_CreateFileA);
    patch_iat(exe, "CreateFile2", (void*)hk_CreateFile2, (void**)&real_CreateFile2);
    patch_iat(exe, "GetFileAttributesW", (void*)hk_GetFileAttributesW, (void**)&real_GetFileAttributesW);
    patch_iat(exe, "GetFileAttributesExW", (void*)hk_GetFileAttributesExW, (void**)&real_GetFileAttributesExW);
}
typedef DWORD(WINAPI* GetModuleFileNameW_t)(HMODULE, LPWSTR, DWORD);
typedef DWORD(WINAPI* GetModuleFileNameA_t)(HMODULE, LPSTR, DWORD);
GetModuleFileNameW_t real_GetModuleFileNameW; GetModuleFileNameA_t real_GetModuleFileNameA;
#ifdef _MSC_VER
#include <intrin.h>
#define AM_RETADDR() _ReturnAddress()
#else
#define AM_RETADDR() __builtin_return_address(0)
#endif
const wchar_t* fake_exe_for(void* ra) {
    int n = g_natN.load();
    for (int i = 0; i < n; i++) if ((BYTE*)ra >= g_nat[i].base && (BYTE*)ra < g_nat[i].base + g_nat[i].size) return g_nat[i].exe;
    return nullptr;
}
DWORD WINAPI hk_GetModuleFileNameW(HMODULE m, LPWSTR buf, DWORD n) {
    if (!m && buf && n) if (const wchar_t* e = fake_exe_for(AM_RETADDR())) {
        size_t len = wcslen(e);
        if (len >= n) { memcpy(buf, e, (n - 1) * sizeof(wchar_t)); buf[n - 1] = 0; SetLastError(ERROR_INSUFFICIENT_BUFFER); return n; }
        memcpy(buf, e, (len + 1) * sizeof(wchar_t)); SetLastError(0); return (DWORD)len;
    }
    return real_GetModuleFileNameW(m, buf, n);
}
DWORD WINAPI hk_GetModuleFileNameA(HMODULE m, LPSTR buf, DWORD n) {
    if (!m && buf && n) if (const wchar_t* e = fake_exe_for(AM_RETADDR())) {
        char tmp[2048]; int len = WideCharToMultiByte(CP_ACP, 0, e, -1, tmp, sizeof tmp, nullptr, nullptr);
        if (len > 0) {
            len--; if ((DWORD)len >= n) { memcpy(buf, tmp, n - 1); buf[n - 1] = 0; SetLastError(ERROR_INSUFFICIENT_BUFFER); return n; }
            memcpy(buf, tmp, len + 1); SetLastError(0); return (DWORD)len;
        }
    }
    return real_GetModuleFileNameA(m, buf, n);
}
void point_module_at_cache(HMODULE m) {
    patch_iat(m, "GetModuleFileNameW", (void*)hk_GetModuleFileNameW, nullptr);
    patch_iat(m, "GetModuleFileNameA", (void*)hk_GetModuleFileNameA, nullptr);
}
struct AM_USTR { USHORT Length, MaximumLength; PWSTR Buffer; };
struct AM_LDR_LOADED { ULONG Flags; const AM_USTR* FullDllName; const AM_USTR* BaseDllName; PVOID DllBase; ULONG SizeOfImage; };
typedef VOID(CALLBACK* LdrNotify_t)(ULONG, const AM_LDR_LOADED*, PVOID);
typedef LONG(NTAPI* LdrRegister_t)(ULONG, LdrNotify_t, PVOID, PVOID*);
typedef LONG(NTAPI* LdrUnregister_t)(PVOID);
VOID CALLBACK on_dll_loaded(ULONG reason, const AM_LDR_LOADED* d, PVOID) {
    if (reason != 1 || !d || !d->FullDllName || !d->FullDllName->Buffer || !g_loadingExe) return;
    size_t n = d->FullDllName->Length / sizeof(wchar_t);
    for (auto& p : g_hookPrefixes) if (n >= p.size() && _wcsnicmp(d->FullDllName->Buffer, p.c_str(), p.size()) == 0) {
        int i = g_natN.load(); if (i >= 64) return;
        g_nat[i] = { (BYTE*)d->DllBase, (size_t)d->SizeOfImage, g_loadingExe }; g_natN.store(i + 1);
        point_module_at_cache((HMODULE)d->DllBase); break;
    }
}
void* exe_iat_value(const char* fn) {
    BYTE* base = (BYTE*)GetModuleHandleW(nullptr); auto dos = (IMAGE_DOS_HEADER*)base; auto nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT]; if (!dir.VirtualAddress) return nullptr;
    for (auto d = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); d->Name; ++d) {
        if (!is_file_api_dll((char*)(base + d->Name))) continue;
        auto thunk = (IMAGE_THUNK_DATA*)(base + d->FirstThunk);
        auto oft = (IMAGE_THUNK_DATA*)(base + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk));
        for (; oft->u1.AddressOfData; ++oft, ++thunk) {
            if (IMAGE_SNAP_BY_ORDINAL(oft->u1.Ordinal)) continue;
            if (!strcmp((char*)((IMAGE_IMPORT_BY_NAME*)(base + oft->u1.AddressOfData))->Name, fn)) return (void*)thunk->u1.Function;
        }
    }
    return nullptr;
}

am::fs::path find_native_gcl_output(const am::fs::path& root,
                                    const am::fs::path& inputPath,
                                    const std::string& inputData) {
    std::error_code ec;
    am::fs::path best;
    std::uintmax_t bestSize = 0;

    if (!am::fs::is_directory(root, ec)) return {};

    for (am::fs::recursive_directory_iterator it(root, ec), end; it != end && !ec; it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec)) { ec.clear(); continue; }

        const am::fs::path p = it->path();
        if (_stricmp(p.filename().u8string().c_str(), "game.gcl") != 0) continue;

        if (norm(p.wstring()) == norm(inputPath.wstring())) continue;

        std::error_code fec;
        const auto sz = am::fs::file_size(p, fec);
        if (fec || sz == 0) continue;

        try {
            std::string data = am::read_file(p);
            if (data == inputData) continue;
            if (!best.empty() && sz < bestSize) continue;
            best = p;
            bestSize = sz;
        } catch (...) {
        }
    }

    return best;
}

am::fs::path run_natives(const am::Result& R, const am::fs::path& cache, const am::fs::path& gcl, am::fs::path cur) {
    HMODULE nt = GetModuleHandleW(L"ntdll.dll"); PVOID cookie = nullptr;
    auto reg = nt ? (LdrRegister_t)GetProcAddress(nt, "LdrRegisterDllNotification") : nullptr;
    auto unreg = nt ? (LdrUnregister_t)GetProcAddress(nt, "LdrUnregisterDllNotification") : nullptr;
    if (!reg) { logmsg("WARN LdrRegisterDllNotification unavailable - native mods skipped (they could not be pointed at the cache safely)"); return cur; }
    auto pref = [](am::fs::path p) { p = am::fs::absolute(p); p.make_preferred(); return p.wstring() + L"\\"; };
    g_hookPrefixes.clear(); for (auto& d : R.natives) g_hookPrefixes.push_back(pref(d.parent_path().parent_path() / ".cache" / "native"));
    reg(0, on_dll_loaded, nullptr, &cookie);
    std::error_code ec; am::fs::path lastMod; int idx = 0;
    for (auto& dll : R.natives) {
        am::fs::path srcDir = dll.parent_path(), modDir = srcDir.parent_path(), croot = modDir / ".cache";
        am::fs::path cdir = croot / "native", groot = croot / "game";
        am::fs::path gin = groot / "bin" / "game.gcl";
        std::string name = modDir.filename().u8string() + "/" + dll.filename().u8string(); idx++;
        if (modDir != lastMod) {
            lastMod = modDir;
            am::fs::remove_all(cdir, ec);
            am::fs::create_directories(cdir, ec);
            if (ec) { logmsg("WARN " + name + ": could not create cache/native (" + ec.message() + ") - skipped"); continue; }
            for (auto& e : am::fs::directory_iterator(srcDir, ec)) {
                if (ec) break;
                if (!e.is_regular_file(ec)) { ec.clear(); continue; }
                std::error_code ce;
                am::fs::copy_file(e.path(), cdir / e.path().filename(), am::fs::copy_options::overwrite_existing, ce);
                if (ce) logmsg("WARN " + name + ": could not cache " + e.path().filename().u8string() + " (" + ce.message() + ")");
            }
            if (ec) { logmsg("WARN " + name + ": could not read native/ (" + ec.message() + ") - skipped"); continue; }
        }
        am::fs::path cached = cdir / dll.filename();
        if (!am::fs::exists(cached)) {
            std::error_code ce;
            am::fs::copy_file(dll, cached, am::fs::copy_options::overwrite_existing, ce);
            if (ce) { logmsg("WARN " + name + ": could not cache DLL (" + ce.message() + ") - skipped"); continue; }
        }
        std::string input = am::read_file(cur), old;
        if (am::fs::exists(gin)) { try { old = am::read_file(gin); } catch (...) {} }
        if (old != input) {
            std::error_code dec;
            if (am::fs::is_directory(groot, dec)) {
                for (am::fs::recursive_directory_iterator it(groot, dec), end; it != end && !dec; it.increment(dec)) {
                    if (dec) break;
                    if (!it->is_regular_file(dec)) { dec.clear(); continue; }
                    if (_stricmp(it->path().filename().u8string().c_str(), "game.gcl") != 0) continue;
                    if (norm(it->path().wstring()) == norm(gin.wstring())) continue;
                    std::error_code rmec;
                    am::fs::remove(it->path(), rmec);
                }
            }
        }
        am::fs::create_directories(gin.parent_path(), ec); { std::ofstream f(gin, std::ios::binary); f << input; }
        g_exeStrs.push_back((groot / "game.exe").wstring()); g_loadingExe = g_exeStrs.back().c_str();
        HMODULE h = LoadLibraryExW(cached.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        g_loadingExe = nullptr;
        if (!h) { logmsg("WARN could not load native plugin " + name + " (Windows error " + std::to_string(GetLastError()) + ")"); continue; }
        logmsg("loaded native plugin " + name + " (cached copy, game folder -> " + groot.u8string() + ")");
        if (void* fn = exe_iat_value("CreateFileW")) {
            HANDLE fh = ((CreateFileW_t)fn)(gin.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (fh != INVALID_HANDLE_VALUE) CloseHandle(fh);
        }
        hook_all();
        am::fs::path gout = find_native_gcl_output(groot, gin, input);
        if (!gout.empty()) {
            std::string out = am::read_file(gout);
            logmsg("  stage " + std::to_string(idx) + " (" + name + "): detected game.gcl output at " + gout.lexically_relative(groot).u8string() +
                   " (" + std::to_string(input.size()) + " -> " + std::to_string(out.size()) + " bytes" +
                   (out == input ? " (no change)" : "") + ")");
            cur = gout;
        } else {
            logmsg("  stage " + std::to_string(idx) + " (" + name + "): no modified game.gcl output detected");
        }
    }
    if (cookie && unreg) unreg(cookie);
    return cur;
}

void install_hooks() {
    HMODULE k = GetModuleHandleW(L"kernel32.dll");
    real_CreateFileW = (CreateFileW_t)GetProcAddress(k, "CreateFileW"); real_CreateFileA = (CreateFileA_t)GetProcAddress(k, "CreateFileA");
    real_CreateFile2 = (CreateFile2_t)GetProcAddress(k, "CreateFile2");
    real_GetFileAttributesW = (GetFileAttributesW_t)GetProcAddress(k, "GetFileAttributesW");
    real_GetFileAttributesExW = (GetFileAttributesExW_t)GetProcAddress(k, "GetFileAttributesExW");
    real_GetModuleFileNameW = (GetModuleFileNameW_t)GetProcAddress(k, "GetModuleFileNameW"); real_GetModuleFileNameA = (GetModuleFileNameA_t)GetProcAddress(k, "GetModuleFileNameA");
    hook_all();
}


HMODULE g_real;
FARPROC real_fn(const char* name) {
    if (!g_real) { wchar_t p[MAX_PATH]; GetSystemDirectoryW(p, MAX_PATH); lstrcatW(p, L"\\dinput8.dll"); g_real = LoadLibraryW(p); }
    return g_real ? GetProcAddress(g_real, name) : nullptr;
}
} 

extern "C" {
__declspec(dllexport) HRESULT WINAPI DirectInput8Create(HINSTANCE h, DWORD v, const void* riid, void** out, void* unk) {
    auto f = (HRESULT(WINAPI*)(HINSTANCE, DWORD, const void*, void**, void*))real_fn("DirectInput8Create");
    return f ? f(h, v, riid, out, unk) : (HRESULT)0x80004005L;
}
__declspec(dllexport) HRESULT WINAPI DllCanUnloadNow() { return 1; }
__declspec(dllexport) HRESULT WINAPI DllGetClassObject(const void* c, const void* i, void** o) {
    auto f = (HRESULT(WINAPI*)(const void*, const void*, void**))real_fn("DllGetClassObject"); return f ? f(c, i, o) : (HRESULT)0x80004005L; }
__declspec(dllexport) HRESULT WINAPI DllRegisterServer() { return 0; }
__declspec(dllexport) HRESULT WINAPI DllUnregisterServer() { return 0; }
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = inst; DisableThreadLibraryCalls(inst);
        wchar_t p[MAX_PATH]; GetModuleFileNameW(inst, p, MAX_PATH); g_dir = am::fs::path(p).parent_path();
        install_hooks();
    }
    return TRUE;
}
