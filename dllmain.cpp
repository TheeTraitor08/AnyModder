


#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include "gcl_tweaks.hpp"

namespace {
HMODULE g_self; am::fs::path g_dir; std::wstring g_romPrefix;
std::unordered_map<std::wstring, std::wstring> g_map;
std::once_flag g_once; bool g_ready = false; thread_local bool t_busy = false;

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
void init() {
    t_busy = true;
    try {
        am::fs::path rom = g_dir / "rom", mods = g_dir / "mods";
        if (!am::fs::is_directory(rom) || !am::fs::is_directory(mods)) { t_busy = false; return; }
        std::error_code ec; am::fs::remove(mods / "mod_loader.log", ec);
        logmsg("AnyModder loaded.");
        am::Result R = am::build(rom, mods);
        for (auto& l : R.log) logmsg(l); for (auto& w : R.warns) logmsg("WARN " + w);
        if (R.mods == 0) { logmsg("no enabled mods - game runs untouched"); t_busy = false; return; }
        am::fs::path cache = mods / ".cache"; am::fs::remove_all(cache / "rom", ec);
        for (auto& kv : R.files) {
            am::fs::path p = cache / "rom" / am::fs::path(kv.first); am::fs::create_directories(p.parent_path());
            std::ofstream(p, std::ios::binary) << kv.second;
            g_map[norm((rom / am::fs::path(kv.first)).wstring())] = p.wstring();
        }
        for (auto& kv : R.assets) g_map[norm((rom / am::fs::path(kv.first)).wstring())] = kv.second.wstring();
        if (!R.tweaks.empty()) {                       
            am::fs::path gcl = g_dir / "bin" / "game.gcl";
            if (!am::fs::exists(gcl)) logmsg("WARN tweaks.json used but bin\\game.gcl not found");
            else {
                am::fs::path out = cache / "bin" / "game.gcl", keyf = cache / "bin" / "game.gcl.key";
                std::string key = std::to_string(am::fs::file_size(gcl)) + "|" + std::to_string(am::fs::last_write_time(gcl).time_since_epoch().count()) + "|" + R.tweaks.dump();
                bool reuse = am::fs::exists(out) && am::fs::exists(keyf) && am::read_file(keyf) == key && am::fs::file_size(out) == am::fs::file_size(gcl);
                if (reuse) logmsg("tweaks unchanged - using cached patched game.gcl");
                else {
                    std::string d = am::read_file(gcl); std::vector<std::string> tl; int n = am::apply_tweaks(d, R.tweaks, tl);
                    for (auto& l : tl) logmsg(l);
                    am::fs::remove(out, ec);
                    if (n > 0) { am::fs::create_directories(out.parent_path()); std::ofstream(out, std::ios::binary) << d; std::ofstream(keyf, std::ios::binary) << key; }
                }
                if (am::fs::exists(out)) g_map[norm(gcl.wstring())] = out.wstring();
            }
        }
        g_romPrefix = norm(rom.wstring() + L"\\");
        logmsg("ready: " + std::to_string(R.files.size()) + " file(s) replaced, " + std::to_string(R.assets.size()) + " asset(s) added");
        g_ready = true;
        
        
        for (auto& d : R.natives) {
            HMODULE h = LoadLibraryExW(d.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
            if (h) logmsg("loaded native plugin " + d.u8string());
            else logmsg("WARN could not load native plugin " + d.u8string() + " (Windows error " + std::to_string(GetLastError()) + ")");
        }
        
        
        if (!R.natives.empty()) hook_all();
    } catch (const std::exception& e) { logmsg(std::string("ERROR (mods disabled, game runs untouched): ") + e.what()); }
    t_busy = false;
}
bool resolve(LPCWSTR name, std::wstring& out) {
    if (t_busy || !name || !*name) return false;
    std::call_once(g_once, init);
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

bool patch_iat(HMODULE mod, const char* dll, const char* fn, void* hook, void** orig) {
    BYTE* base = (BYTE*)mod; auto dos = (IMAGE_DOS_HEADER*)base; auto nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT]; if (!dir.VirtualAddress) return false;
    for (auto d = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); d->Name; ++d) {
        if (_stricmp((char*)(base + d->Name), dll)) continue;
        auto thunk = (IMAGE_THUNK_DATA*)(base + d->FirstThunk);
        auto oft = (IMAGE_THUNK_DATA*)(base + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk));
        for (; oft->u1.AddressOfData; ++oft, ++thunk) {
            if (IMAGE_SNAP_BY_ORDINAL(oft->u1.Ordinal)) continue;
            if (strcmp((char*)((IMAGE_IMPORT_BY_NAME*)(base + oft->u1.AddressOfData))->Name, fn)) continue;
            if (thunk->u1.Function == (ULONG_PTR)hook) return true;   
            DWORD old; VirtualProtect(&thunk->u1.Function, sizeof(void*), PAGE_READWRITE, &old);
            *orig = (void*)thunk->u1.Function; thunk->u1.Function = (ULONG_PTR)hook;
            VirtualProtect(&thunk->u1.Function, sizeof(void*), old, &old); return true;
        }
    }
    return false;
}
void hook_all() {
    HMODULE exe = GetModuleHandleW(nullptr);
    patch_iat(exe, "KERNEL32.dll", "CreateFileW", (void*)hk_CreateFileW, (void**)&real_CreateFileW);
    patch_iat(exe, "KERNEL32.dll", "CreateFileA", (void*)hk_CreateFileA, (void**)&real_CreateFileA);
    patch_iat(exe, "KERNEL32.dll", "CreateFile2", (void*)hk_CreateFile2, (void**)&real_CreateFile2);
    patch_iat(exe, "KERNEL32.dll", "GetFileAttributesW", (void*)hk_GetFileAttributesW, (void**)&real_GetFileAttributesW);
    patch_iat(exe, "KERNEL32.dll", "GetFileAttributesExW", (void*)hk_GetFileAttributesExW, (void**)&real_GetFileAttributesExW);
}
void install_hooks() {
    HMODULE k = GetModuleHandleW(L"kernel32.dll");
    real_CreateFileW = (CreateFileW_t)GetProcAddress(k, "CreateFileW"); real_CreateFileA = (CreateFileA_t)GetProcAddress(k, "CreateFileA");
    real_CreateFile2 = (CreateFile2_t)GetProcAddress(k, "CreateFile2");
    real_GetFileAttributesW = (GetFileAttributesW_t)GetProcAddress(k, "GetFileAttributesW");
    real_GetFileAttributesExW = (GetFileAttributesExW_t)GetProcAddress(k, "GetFileAttributesExW");
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
