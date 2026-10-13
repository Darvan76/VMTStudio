#include "engine_spike_engine.h"

// ---- Valve / Source SDK 2013 headers (the ONLY TU that includes them). ----
// Compiled with /W0 /permissive; see engine_bridge/CMakeLists.txt.
#pragma warning(push, 0)
#include <windows.h>
#include <crtdbg.h>
#include <rtcapi.h>
#include "tier1/interface.h"
#include "tier0/icommandline.h"
#include "filesystem.h"
#include "icvar.h"
#include "vstdlib/cvar.h"
#include "vstdlib/iprocessutils.h"
#include "tier1/convar.h"
#include "materialsystem/imaterialsystem.h"
#include "materialsystem/imaterial.h"
#include "materialsystem/imaterialvar.h"
#include "materialsystem/itexture.h"
#include "materialsystem/imesh.h"
#include "materialsystem/materialsystem_config.h"
#include "mathlib/lightdesc.h"
#pragma warning(pop)

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace spike {
namespace {

// ---------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------
static std::mutex g_engine_log_mutex;

FILE* engine_log_file() {
    static FILE* file = [] () -> FILE* {
        wchar_t temp[MAX_PATH] = {};
        const DWORD n = GetTempPathW(MAX_PATH, temp);
        if (n == 0 || n >= MAX_PATH) return nullptr;
        const std::filesystem::path dir = std::filesystem::path(temp) / L"VMTViewer";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        const std::filesystem::path path = dir / L"engine.log";
        FILE* f = _wfsopen(path.c_str(), L"a", _SH_DENYNO);
        return f;
    }();
    return file;
}

std::string engine_now_stamp() {
    SYSTEMTIME time{};
    GetLocalTime(&time);
    char buffer[64] = {};
    std::snprintf(buffer, sizeof(buffer), "%04u-%02u-%02u %02u:%02u:%02u.%03u",
                  time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute,
                  time.wSecond, time.wMilliseconds);
    return buffer;
}

bool engine_ansi_enabled() {
    static bool checked = false;
    static bool enabled = false;
    if (!checked) {
        checked = true;
        HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
        if (hOut != INVALID_HANDLE_VALUE && hOut != nullptr) {
            DWORD mode = 0;
            if (GetConsoleMode(hOut, &mode)) {
                if (mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING) {
                    enabled = true;
                }
            }
        }
    }
    return enabled;
}

void log_line(const char* tag, const std::string& msg) {
    std::lock_guard<std::mutex> lock(g_engine_log_mutex);
    const std::string stamp = engine_now_stamp();
    const std::string stag = tag ? tag : "ENG ";

    if (engine_ansi_enabled()) {
        const char* color = "\033[95;1m"; // bright magenta for ENG
        if (stag == "OK  ") color = "\033[92;1m";
        else if (stag == "FAIL" || stag == "CRSH") color = "\033[91;1m";
        else if (stag == "WARN") color = "\033[93;1m";
        else if (stag == "DIAG") color = "\033[96m";
        else if (stag == "SRC ") color = "\033[35m";
        else if (stag == "THMB") color = "\033[33m";

        std::printf("\033[90m[%s]\033[0m %s[%s]\033[0m %s\n",
                    stamp.c_str(), color, stag.c_str(), msg.c_str());
    } else {
        std::printf("[%s] [%s] %s\n", stamp.c_str(), stag.c_str(), msg.c_str());
    }
    std::fflush(stdout);

    // A WIN32 GUI host has no console, so the same log goes to a file; the
    // crash handler also writes here, which makes engine faults diagnosable.
    if (FILE* file = engine_log_file()) {
        std::fprintf(file, "[%s] [%s] %s\n", stamp.c_str(), stag.c_str(), msg.c_str());
        std::fflush(file);
    }
}

StepResult ok(const std::string& msg) { return StepResult{ true, msg }; }
StepResult fail(const std::string& msg) { return StepResult{ false, msg }; }

std::string win32_error(DWORD code) {
    if (code == 0) return {};
    wchar_t* buf = nullptr;
    const DWORD n = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, 0, reinterpret_cast<wchar_t*>(&buf), 0, nullptr);
    std::string out;
    if (n != 0 && buf != nullptr) {
        const int len = WideCharToMultiByte(CP_UTF8, 0, buf, (int)n, nullptr, 0, nullptr, nullptr);
        if (len > 0) {
            out.resize((size_t)len);
            WideCharToMultiByte(CP_UTF8, 0, buf, (int)n, out.data(), len, nullptr, nullptr);
        }
    }
    if (buf) LocalFree(buf);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    return out;
}

std::string hex64(unsigned long long v) {
    char b[32] = {};
    std::snprintf(b, sizeof(b), "%llX", v);
    return b;
}

std::string module_of(const void* addr, unsigned long long* out_rva) {
    HMODULE mod = nullptr;
    if (addr != nullptr &&
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(addr), &mod) && mod != nullptr) {
        wchar_t path[MAX_PATH] = {};
        GetModuleFileNameW(mod, path, MAX_PATH);
        if (out_rva) {
            *out_rva = static_cast<unsigned long long>(
                reinterpret_cast<const unsigned char*>(addr) - reinterpret_cast<const unsigned char*>(mod));
        }
        return std::filesystem::path(path).filename().string();
    }
    if (out_rva) *out_rva = 0;
    return std::string("?");
}

// ---------------------------------------------------------------------------
// SEH capture: report fault address, module and faulting instruction bytes.
// ---------------------------------------------------------------------------
LONG CALLBACK veh_handler(EXCEPTION_POINTERS* info) {
    const DWORD code = info->ExceptionRecord->ExceptionCode;
    const bool fatal = (code == EXCEPTION_ACCESS_VIOLATION ||
                        code == EXCEPTION_ILLEGAL_INSTRUCTION ||
                        code == EXCEPTION_PRIV_INSTRUCTION ||
                        code == EXCEPTION_STACK_OVERFLOW ||
                        code == EXCEPTION_INT_DIVIDE_BY_ZERO ||
                        code == EXCEPTION_IN_PAGE_ERROR);

    static LONG fatal_reported = 0;
    static int ignored_logged = 0;
    if (!fatal) {
        if (ignored_logged < 8) {
            ++ignored_logged;
            log_line("CRSH", "ignoring non-fatal exception 0x" + hex64(code));
        }
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (fatal_reported != 0) return EXCEPTION_CONTINUE_SEARCH;
    fatal_reported = 1;

    const EXCEPTION_RECORD* er = info->ExceptionRecord;
    unsigned long long rva = 0;
    const std::string mod = module_of(er->ExceptionAddress, &rva);
    log_line("CRSH", "EXCEPTION code=0x" + hex64(er->ExceptionCode) +
                         "  address=0x" + hex64((unsigned long long)(std::uintptr_t)er->ExceptionAddress) +
                         "  (" + mod + "+0x" + hex64(rva) + ")");

    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2) {
        const char* kind = er->ExceptionInformation[0] == 0 ? "READ"
                          : er->ExceptionInformation[0] == 1 ? "WRITE"
                          : er->ExceptionInformation[0] == 8 ? "EXEC" : "?";
        log_line("CRSH", std::string("  AV ") + kind + " at 0x" + hex64(er->ExceptionInformation[1]));
    }

    unsigned char bytes[16] = {};
    SIZE_T got = 0;
    if (er->ExceptionAddress != nullptr &&
        ReadProcessMemory(GetCurrentProcess(), er->ExceptionAddress, bytes, sizeof(bytes), &got) != FALSE && got > 0) {
        std::string hex;
        for (SIZE_T i = 0; i < got; ++i) {
            char b[4] = {};
            std::snprintf(b, sizeof(b), "%02X ", bytes[i]);
            hex += b;
        }
        log_line("CRSH", "  bytes at fault: " + hex);
    }

    void* frames[16] = {};
    const USHORT n = RtlCaptureStackBackTrace(1, 16, frames, nullptr);
    for (USHORT i = 0; i < n; ++i) {
        unsigned long long frva = 0;
        const std::string fmod = module_of(frames[i], &frva);
        log_line("CRSH", "  #" + std::to_string((int)i) + " " + fmod + "+0x" + hex64(frva));
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

#ifdef _DEBUG
// /RTC1 handler. Error 0 (_RTC_CHKSTK) is "ESP was not properly saved across a
// function call"; error 2 is a corrupted stack. Both are fatal for our purposes
// (we terminate with a distinct code after logging). Other RTC errors are
// logged and execution continues.
int __cdecl rtc_handler(int errorNumber, const char* filename, int linenumber,
                        const char* moduleName, const char* format, ...) {
    char buf[512] = {};
    if (format != nullptr) {
        va_list ap;
        va_start(ap, format);
        _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, format, ap);
        va_end(ap);
    }
    log_line("RTC ", "Run-Time Check Failure #" + std::to_string(errorNumber) + "  at " +
                         (filename ? filename : "?") + ":" + std::to_string(linenumber) +
                         "  (" + (moduleName ? moduleName : "?") + ")  : " + buf);
    if (errorNumber == _RTC_CHKSTK || errorNumber == _RTC_CORRUPT_STACK) {
        std::fflush(stdout);
        TerminateProcess(GetCurrentProcess(), 71);
    }
    return 0;
}
#endif

// ---------------------------------------------------------------------------
// Engine state
// ---------------------------------------------------------------------------
struct State {
    HMODULE tier0 = nullptr;
    HMODULE vstdlib = nullptr;
    HMODULE fs_mod = nullptr;
    HMODULE mats_mod = nullptr;
    HMODULE shader_mod = nullptr;

    CreateInterfaceFn fs_factory = nullptr;
    CreateInterfaceFn mats_factory = nullptr;

    IFileSystem* fs = nullptr;
    IMaterialSystem* matsys = nullptr;
    ICvar* cvar = nullptr;        // for mat_dxlevel diagnostics
    bool matsys_inited = false;   // IMaterialSystem::Init succeeded
    bool view_added = false;      // IMaterialSystem::AddView succeeded

    IMaterial* material = nullptr;
    bool material_is_error = false;

    HWND hwnd = nullptr;
    void* child_hwnd = nullptr;   // caller-provided host window (UI integration)
    bool owns_window = true;      // false when rendering into a child window
    bool pump_messages = true;    // the standalone spike pumps; the UI does not
    int width = 800;              // current host (window) size: projection aspect
    int height = 600;
    int buffer_width = 800;       // device backbuffer size (fixed after SetMode)
    int buffer_height = 600;
    bool initialized = false;

    // Phase 9 preview options.
    PreviewShape shape = PreviewShape::Sphere;
    Lighting lighting;

    CreateInterfaceFn connect_factories[2] = { nullptr, nullptr };

    static void* ConnectFactory(const char* name, int* return_code);
};

State g;

// --- Material ownership -------------------------------------------------------
//
// IMaterialSystem::FindMaterial hands back a material at reference count 0:
// nothing in the engine owns it until a surface binds it. Binding one whose
// count is not positive makes the engine log
//
//     Material <name> has bad reference count 0 when being bound
//
// which is why the preview is written the way it is.
//
// The obvious fix is IncrementReferenceCount/DecrementReferenceCount, which
// the SDK does declare on IMaterial. DO NOT call them here. This host runs the
// retail Half-Life 2 binaries (the configured game folder is ep2, a 2004
// release), and that runtime's IMaterial vtable is not the one in the 2013 SDK
// header this translation unit is compiled against. Calling IncrementReferenceCount
// at the SDK's slot reads a different virtual entirely and faults: verified
// empirically, it turns the engine probe into exit code 0xC0000005.
//
// The warning is cosmetic. Binding still works - the engine logs the complaint
// and renders the material correctly - so the correct trade is to leave the
// refcount alone and accept the console noise. If a warning-free path is ever
// needed, the options are to obtain the matching retail interface header, or
// to have the engine create and own the material (CreateMaterial +
// DeleteMaterial), which is a larger change to how the preview is built.

// Takes ownership of the current preview material, releasing whatever it held
// before. The name is kept because the ownership is real even though the
// engine-side reference count is deliberately not manipulated - see above.
void adopt_material(IMaterial* material) {
    g.material = material;
}

// TEMP diagnostics: reports the DX level the material system believes it runs
// at, plus the load state of every texture parameter of the material. This is
// how a "missing textures" render is attributed: an ITexture that reports
// IsError or a 32x32 "error" mapping means the VTF never loaded.
void log_texture_state(IMaterial* material, const char* when) {
    if (material == nullptr) return;
    const int dxlevel = g.cvar != nullptr
        ? (g.cvar->FindVar("mat_dxlevel") != nullptr ? g.cvar->FindVar("mat_dxlevel")->GetInt() : -1)
        : -2;
    log_line("DIAG", std::string(when) + ": mat_dxlevel=" + std::to_string(dxlevel) +
                         " shader=" + material->GetShaderName());

    const int count = material->ShaderParamCount();
    IMaterialVar** params = material->GetShaderParams();
    if (params == nullptr || count <= 0) {
        // Retail IMaterial ABI may drift from the SDK header (see the refcount
        // note above); fall back to a by-name lookup for the common params.
        log_line("DIAG", "  GetShaderParams empty (count=" + std::to_string(count) +
                             ", params=" + (params != nullptr ? "set" : "null") + "), probing by name");
        static const char* kTexVars[] = { "$basetexture", "$detail", "$bumpmap", "$envmap",
                                          "$texture1", "$texture2" };
        for (const char* name : kTexVars) {
            bool found = false;
            IMaterialVar* var = material->FindVar(name, &found, false);
            if (!found || var == nullptr) continue;
            if (!var->IsTexture()) continue;
            ITexture* tex = var->GetTextureValue();
            if (tex == nullptr) {
                log_line("DIAG", std::string("  $") + name + " = (null texture)");
                continue;
            }
            log_line("DIAG", std::string("  $") + name + " -> '" + tex->GetName() +
                                 "' mapping=" + std::to_string(tex->GetMappingWidth()) + "x" +
                                 std::to_string(tex->GetMappingHeight()) +
                                 " actual=" + std::to_string(tex->GetActualWidth()) + "x" +
                                 std::to_string(tex->GetActualHeight()) +
                                 "  isError=" + (tex->IsError() ? "YES" : "no"));
        }
        return;
    }
    for (int i = 0; i < count; ++i) {
        IMaterialVar* var = params[i];
        if (var == nullptr || !var->IsTexture()) continue;
        ITexture* tex = var->GetTextureValue();
        if (tex == nullptr) {
            log_line("DIAG", std::string("  $") + var->GetName() + " = (null texture)");
            continue;
        }
        log_line("DIAG", std::string("  $") + var->GetName() + " -> '" + tex->GetName() +
                             "' actual=" + std::to_string(tex->GetActualWidth()) + "x" +
                             std::to_string(tex->GetActualHeight()) +
                             "  isError=" + (tex->IsError() ? "YES" : "no"));
    }
}

void release_current_material() {
    g.material = nullptr;
    g.material_is_error = true;
}

// Composite factory for IMaterialSystem::Connect. The engine's launcher passes
// a factory that covers every engine interface; a tool provides only what it
// has and answers null for the rest. Forwarding arbitrary engine-only names to
// the game DLLs is not safe (verified: filesystem_stdio/shaderapidx9 fault when
// asked for QueuedLoaderVersion004 by the current engine builds), so only the
// interfaces a tool can actually provide are answered here.
void* State::ConnectFactory(const char* name, int* return_code) {
    if (name == nullptr) {
        return nullptr;
    }
    // Hardware config comes from the shader DLL. The vstdlib names are served
    // from the cvar factory because matsys reuses this factory to connect the
    // shader DLL, whose IAppSystem::Connect pulls ICvar through it. The cvar
    // interface version differs per engine branch (SDK 2013 uses VEngineCvar004,
    // Garry's Mod asks for VEngineCvar007), so any VEngineCvar0xx name is
    // delegated and the factory answers with whatever version it provides.
    if (std::strcmp(name, MATERIALSYSTEM_HARDWARECONFIG_INTERFACE_VERSION) == 0 &&
        g.connect_factories[0] != nullptr) {
        return g.connect_factories[0](name, return_code);
    }
    if ((std::strncmp(name, "VEngineCvar0", 12) == 0 ||
         std::strcmp(name, PROCESS_UTILS_INTERFACE_VERSION) == 0) &&
        g.connect_factories[1] != nullptr) {
        return g.connect_factories[1](name, return_code);
    }
    return nullptr;
}

// Every AddSearchPath goes through this wrapper so the log pinpoints the call.
int g_call_no = 0;
const int kMaxCalls = 200;

void heap_check(const std::string& where) {
#ifdef _DEBUG
    const BOOL healthy = _CrtCheckMemory();
    log_line("HEAP", where + ": " + (healthy != FALSE ? "OK" : "*** CORRUPTED ***"));
#else
    (void)where;
#endif
}

// ---------------------------------------------------------------------------
// IFileSystem primary-vtable layout compatibility
// ---------------------------------------------------------------------------
// Garry's Mod's filesystem_stdio (VFileSystem022) inserts four extra virtual
// methods before IsSteam, so every IFileSystem method shifts by +4 slots
// relative to the Source SDK 2013 layout (AddSearchPath 7 -> 11,
// RemoveAllSearchPaths 9 -> 13, FindFirst..FindClose 27..30 -> 31..34).
// Calling the wrong slot corrupts the stack because the real function pops a
// different number of argument bytes. Every engine call that is affected goes
// through the shims below; the layout delta is detected at runtime
// (detect_fs_layout). FileExists/Open/Read/Close/Size live in the separate
// IBaseFileSystem secondary vtable (this+4), which is unchanged in GMod, so
// those keep using the normal compiled calls.
struct FsLayout {
    int delta = 0;      // 0 = SDK 2013, 4 = Garry's Mod VFileSystem022
    std::string name;
};
FsLayout g_fs_layout;

void** fs_primary_vtable() {
    return *reinterpret_cast<void***>(g.fs);
}

void* fs_primary_slot(int sdk_slot) {
    return fs_primary_vtable()[sdk_slot + g_fs_layout.delta];
}

// Calls `fn` as `self->fn(path, path_id, add_type)` and returns the ESP drift:
// 0 bytes means the callee popped all three argument slots (a real 3-argument
// __thiscall); -4/-8/-12 mean it popped two/one/none, i.e. the candidate has a
// shorter signature. The stack pointer is restored before returning, so a
// wrong-arity candidate can never corrupt the caller's frame.
long long call_3arg_thunk(void* fn, const char* path, const char* path_id, int add_type) {
    static unsigned long esp_before = 0;
    static unsigned long esp_after = 0;
    typedef void(__fastcall * Fn)(void* self, void* edx, const char* pPath, const char* pPathID, int addType);
    Fn call = reinterpret_cast<Fn>(fn);
    __asm { mov esp_before, esp }
    call(g.fs, nullptr, path, path_id, add_type);
    __asm { mov esp_after, esp }
    const long long drift = (long long)esp_after - (long long)esp_before;
    if (drift != 0) {
        __asm { mov esp, esp_before }
    }
    return drift;
}

void fs_remove_all_search_paths() {
    typedef void(__fastcall * Fn)(void* self, void* edx);
    reinterpret_cast<Fn>(fs_primary_slot(9))(g.fs, nullptr);
}

const char* fs_find_first(const char* wildcard, FileFindHandle_t* handle) {
    typedef const char*(__fastcall * Fn)(void* self, void* edx, const char* pWildCard, FileFindHandle_t* pHandle);
    return reinterpret_cast<Fn>(fs_primary_slot(27))(g.fs, nullptr, wildcard, handle);
}
const char* fs_find_next(FileFindHandle_t handle) {
    typedef const char*(__fastcall * Fn)(void* self, void* edx, FileFindHandle_t h);
    return reinterpret_cast<Fn>(fs_primary_slot(28))(g.fs, nullptr, handle);
}
bool fs_find_is_directory(FileFindHandle_t handle) {
    typedef bool(__fastcall * Fn)(void* self, void* edx, FileFindHandle_t h);
    return reinterpret_cast<Fn>(fs_primary_slot(29))(g.fs, nullptr, handle);
}
void fs_find_close(FileFindHandle_t handle) {
    typedef void(__fastcall * Fn)(void* self, void* edx, FileFindHandle_t h);
    reinterpret_cast<Fn>(fs_primary_slot(30))(g.fs, nullptr, handle);
}

bool call_add_search_path(const std::string& path_id, const std::string& full_path, const char* origin,
                          bool to_head = false) {
    if (g_call_no >= kMaxCalls) {
        log_line("FAIL", "AddSearchPath call cap reached; aborting to avoid an infinite loop");
        return false;
    }
    ++g_call_no;
    log_line("CALL", "#" + std::to_string(g_call_no) + " BEFORE  id='" + path_id +
                         "'  path='" + full_path + "'  origin=" + origin +
                         (to_head ? "  [head]" : ""));
    const long long drift = call_3arg_thunk(fs_primary_slot(7), full_path.c_str(), path_id.c_str(),
                                            to_head ? PATH_ADD_TO_HEAD : PATH_ADD_TO_TAIL);
    if (drift != 0) {
        log_line("FAIL", "AddSearchPath ESP drift = " + std::to_string(drift) +
                             " bytes (wrong vtable slot?)");
        return false;
    }
    log_line("CALL", "#" + std::to_string(g_call_no) + " AFTER");
    heap_check("after call #" + std::to_string(g_call_no));
    return true;
}

CreateInterfaceFn load_factory(HMODULE mod) {
    return reinterpret_cast<CreateInterfaceFn>(GetProcAddress(mod, CREATEINTERFACE_PROCNAME));
}

HMODULE load_dll(const std::string& bin_dir, const std::string& name) {
    const std::wstring full = std::filesystem::path(bin_dir).append(name).wstring();
    // LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR makes the DLL's own directory
    // participate in dependency resolution (bin-local dependencies such as
    // steam_api.dll), and it keeps working when the process runs under
    // SetDefaultDllDirectories, where LOAD_WITH_ALTERED_SEARCH_PATH is ignored.
    return LoadLibraryExW(full.c_str(), nullptr,
                          LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
                              LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
}

// Scan a DLL's bytes for interface version strings like "VFileSystem022".
std::vector<std::string> discover_versions(const std::vector<unsigned char>& bytes,
                                           const std::string& prefix) {
    std::set<std::string> found;
    for (size_t i = 0; i + prefix.size() <= bytes.size(); ++i) {
        if (std::memcmp(&bytes[i], prefix.data(), prefix.size()) != 0) continue;
        size_t j = i + prefix.size();
        std::string ver;
        while (j < bytes.size() && std::isdigit((unsigned char)bytes[j])) {
            ver += (char)bytes[j];
            ++j;
        }
        if (!ver.empty()) found.insert(prefix + ver);
    }
    return std::vector<std::string>(found.begin(), found.end());
}

std::vector<unsigned char> read_file_bytes(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<unsigned char>((std::istreambuf_iterator<char>(in)),
                                      std::istreambuf_iterator<char>());
}

std::string read_file_bytes_as_string(const std::string& path) {
    const auto bytes = read_file_bytes(path);
    return std::string(bytes.begin(), bytes.end());
}

// Try each discovered version string; return the first interface that resolves.
void* find_interface(CreateInterfaceFn factory, const std::vector<std::string>& candidates,
                     std::string* out_version) {
    for (const std::string& ver : candidates) {
        int code = 0;
        void* iface = factory(ver.c_str(), &code);
        if (iface) {
            if (out_version) *out_version = ver;
            return iface;
        }
    }
    return nullptr;
}

// Logs the detected layout and the resolved slot addresses (diagnostics only).
void log_fs_layout() {
    log_line("VTBL", "IFileSystem layout: " + g_fs_layout.name + " (delta +" +
                         std::to_string(g_fs_layout.delta) + ")");
    const int sdk_slots[] = { 7, 9, 27, 28, 29, 30 };
    for (int s : sdk_slots) {
        void* fn = fs_primary_slot(s);
        unsigned long long rva = 0;
        const std::string mod = module_of(fn, &rva); // sequence explicitly: rva is an out-param
        log_line("VTBL", "  sdk slot " + std::to_string(s) + " -> vtable[" +
                             std::to_string(s + g_fs_layout.delta) + "]  " + mod + "+0x" + hex64(rva));
    }
    // The IBaseFileSystem base sits right after IAppSystem (this+4); its vtable
    // serves FileExists/Open/Read/Close/Size, which the spike calls directly.
    void** secondary = *reinterpret_cast<void***>(reinterpret_cast<char*>(g.fs) + 4);
    unsigned long long rva = 0;
    const std::string mod = module_of(secondary, &rva); // sequence explicitly: rva is an out-param
    log_line("VTBL", "  IBaseFileSystem vtable (this+4): " + mod + "+0x" + hex64(rva));
}

// ---------------------------------------------------------------------------
// gameinfo.txt search-path mounting (follows filesystem_init.cpp)
// ---------------------------------------------------------------------------
std::string fix_slashes(std::string s) {
    for (auto& c : s) if (c == '/') c = '\\';
    return s;
}

std::string strip_trailing_slash(std::string s) {
    while (!s.empty() && (s.back() == '\\' || s.back() == '/')) s.pop_back();
    return s;
}

std::string strip_filename(std::string s) {
    const size_t pos = s.find_last_of("\\/");
    if (pos == std::string::npos) return s;
    return s.substr(0, pos);
}

std::string lower_copy(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

bool contains_ci(const std::string& haystack, const std::string& needle) {
    return lower_copy(haystack).find(lower_copy(needle)) != std::string::npos;
}

bool wildcard_match(const std::string& pattern, const std::string& name) {
    size_t pi = 0, ni = 0;
    size_t star = std::string::npos, mark = 0;
    while (ni < name.size()) {
        if (pi < pattern.size() && (pattern[pi] == '?' || pattern[pi] == name[ni])) {
            ++pi; ++ni;
        } else if (pi < pattern.size() && pattern[pi] == '*') {
            star = pi++;
            mark = ni;
        } else if (star != std::string::npos) {
            pi = star + 1;
            ni = ++mark;
        } else {
            return false;
        }
    }
    while (pi < pattern.size() && pattern[pi] == '*') ++pi;
    return pi == pattern.size();
}

bool ends_with(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

void mount_wildcard(const std::string& base_dir, const std::string& pattern,
                    const std::vector<std::string>& path_ids, bool first_only) {
    const std::string full = fix_slashes((std::filesystem::path(base_dir) / pattern).string());
    const size_t slash = full.find_last_of("\\/");
    if (slash == std::string::npos) return;
    const std::string dir = full.substr(0, slash);
    const std::string wc = full.substr(slash + 1);

    std::error_code ec;
    std::vector<std::string> found;
    std::filesystem::directory_iterator it(dir, ec);
    for (const auto& e : it) {
        const std::string name = e.path().filename().string();
        if (!wildcard_match(wc, name)) continue;
        if (e.is_directory(ec) || ends_with(name, ".vpk")) {
            found.push_back(fix_slashes(e.path().string()));
        }
    }

    std::sort(found.begin(), found.end());

    // Drop _nnn.vpk chunks when the matching _dir.vpk is present.
    std::vector<std::string> keep;
    for (const auto& f : found) {
        const std::string base = [&] {
            std::string s = f;
            const size_t d = s.find("_dir.vpk");
            if (d != std::string::npos) s = s.substr(0, d);
            return s;
        }();
        const bool has_dir = std::any_of(found.begin(), found.end(), [&](const std::string& o) {
            return o.find(base) == 0 && o.find("_dir.vpk") != std::string::npos;
        });
        const bool is_chunk = ends_with(f, ".vpk") && f.find("_dir.vpk") == std::string::npos;
        if (is_chunk && has_dir) continue;
        keep.push_back(f);
    }
    found = std::move(keep);

    if (first_only) {
        if (!found.empty() && !path_ids.empty()) {
            call_add_search_path(path_ids[0], found[0], "wildcard-first-only");
        }
        return;
    }

    for (const auto& p : found) {
        for (const auto& id : path_ids) {
            call_add_search_path(id, p, "wildcard");
        }
    }
}

std::string trim_ws(std::string s) {
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n')) --e;
    return s.substr(b, e - b);
}

struct SearchPathEntry {
    std::string path_ids;
    std::string location;
};

// gameinfo.txt uses '//' line comments (not '/* */'), and search-path values
// may contain '*' wildcards and '/*' substrings, so the VMT parser (which treats
// '/*' as a block comment) can't be reused here. Parse it line-by-line instead.
std::vector<SearchPathEntry> parse_search_paths(const std::string& text) {
    std::vector<std::string> lines;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        const size_t c = line.find("//");
        if (c != std::string::npos) line = line.substr(0, c);
        lines.push_back(trim_ws(line));
    }

    std::vector<SearchPathEntry> out;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (lines[i] != "SearchPaths") continue;
        ++i;
        while (i < lines.size() && (lines[i].empty() || lines[i] == "{")) ++i;
        for (; i < lines.size(); ++i) {
            const std::string& l = lines[i];
            if (l == "}") break;
            if (l.empty()) continue;
            const size_t sp = l.find_first_of(" \t");
            const std::string ids = (sp == std::string::npos) ? l : l.substr(0, sp);
            const std::string loc = (sp == std::string::npos) ? std::string() : trim_ws(l.substr(sp));
            if (!ids.empty() && !loc.empty()) out.push_back(SearchPathEntry{ ids, loc });
        }
        break;
    }
    return out;
}

std::string mount_mode_name(MountMode m) {
    switch (m) {
        case MountMode::All: return "all";
        case MountMode::NoAddons: return "no-addons";
        case MountMode::AddonsOnly: return "addons-only";
        case MountMode::Single: return "single";
        case MountMode::SingleDup: return "single-dup";
        case MountMode::Probe: return "probe";
    }
    return "?";
}

// Identifies the runtime IFileSystem vtable layout by probing AddSearchPath
// candidates with a 3-argument __thiscall and verifying that the call really
// added the path (FileExists with the probe path ID). Slot 7 is AddSearchPath
// in the SDK 2013 layout; Garry's Mod shifts it to slot 11. The probe leaves
// the filesystem clean: the caller removes all search paths afterwards.
StepResult detect_fs_layout(const EngineOptions& opts) {
    const std::string probe_path = strip_trailing_slash(fix_slashes(opts.game_dir));
    const char* probe_id = "FS_LAYOUT_PROBE";

    struct Candidate {
        int delta;
        int slot;
        const char* name;
    };
    const Candidate candidates[] = {
        { 0, 7,  "SDK 2013 / VFileSystem0xx (AddSearchPath = slot 7)" },
        { 4, 11, "Garry's Mod VFileSystem022 (AddSearchPath = slot 11)" },
    };

    for (const Candidate& candidate : candidates) {
        void* fn = fs_primary_vtable()[candidate.slot];
        unsigned long long rva = 0;
        const std::string mod = module_of(fn, &rva); // sequence explicitly: rva is an out-param
        const long long drift = call_3arg_thunk(fn, probe_path.c_str(), probe_id, PATH_ADD_TO_TAIL);
        log_line("PROBE", "candidate slot " + std::to_string(candidate.slot) + " (" + mod + "+0x" +
                              hex64(rva) + "): 3-arg call ESP drift = " + std::to_string(drift) +
                              " bytes");
        if (drift != 0) {
            continue;
        }
        const bool added = g.fs->FileExists("gameinfo.txt", probe_id);
        log_line("PROBE", std::string("  FileExists(gameinfo.txt, \"") + probe_id +
                              "\") after call = " + (added ? "true" : "false"));
        if (!added) {
            continue;
        }
        g_fs_layout.delta = candidate.delta;
        g_fs_layout.name = candidate.name;
        log_line("OK  ", std::string("IFileSystem layout detected: ") + candidate.name);
        return ok(std::string("layout detected: ") + candidate.name);
    }
    return fail("could not detect the IFileSystem vtable layout "
                "(neither SDK slot 7 nor GMod slot 11 is a working AddSearchPath)");
}

// Mount-mode probe: confirms the detected layout end-to-end by mounting the
// game folder through the resolved slots, then reading gameinfo.txt back
// (FileExists + Open/Size/Read/Close) and running a bounded FindFirst/FindNext
// scan, which exercises every shimmed slot plus the IBaseFileSystem vtable.
StepResult probe_fs_layout(const EngineOptions& opts) {
    log_line("PROBE", "clean FS (RemoveAllSearchPaths already ran)");
    const bool before_any = g.fs->FileExists("gameinfo.txt", nullptr);
    log_line("PROBE", std::string("FileExists(gameinfo.txt, null) before mount = ") +
                          (before_any ? "true" : "false") + "  (expect false)");

    const std::string game_dir = strip_trailing_slash(fix_slashes(opts.game_dir));
    if (!call_add_search_path("GAME", game_dir, "probe")) return fail("probe mount stuck");

    const bool game_exists = g.fs->FileExists("gameinfo.txt", "GAME");
    log_line("PROBE", std::string("FileExists(gameinfo.txt, \"GAME\") = ") +
                          (game_exists ? "true" : "false"));

    const TextFile text = read_text_file("gameinfo.txt");
    log_line("PROBE", "read_text_file(gameinfo.txt): found=" +
                          std::string(text.found ? "true" : "false") + "  bytes=" +
                          std::to_string(text.text.size()));
    if (!text.found || text.text.empty()) {
        return fail("probe: gameinfo.txt could not be read back through the engine file system");
    }
    const size_t eol = text.text.find('\n');
    log_line("PROBE", "first line: " + text.text.substr(0, eol == std::string::npos ? text.text.size() : eol));

    const MaterialList materials = list_materials();
    log_line("PROBE", "list_materials: " + std::to_string(materials.names.size()) +
                          " .vmt file(s) (bounded scan through the Find shims)");

    if (!game_exists) {
        return fail("probe: FileExists did not see gameinfo.txt after mounting the game folder");
    }
    return ok("probe: layout works end-to-end (mount + FileExists + read + find)");
}

StepResult mount_search_paths(const EngineOptions& opts) {
    const std::string gameinfo_path =
        fix_slashes((std::filesystem::path(opts.game_dir) / "gameinfo.txt").string());
    const std::string base_dir = strip_filename(opts.bin_dir); // game root: contains bin/ and the mod dir

    log_line("----", "gameinfo.txt: " + gameinfo_path);
    log_line("----", "base_dir     : " + base_dir);
    log_line("----", "mod_dir      : " + opts.game_dir);
    log_line("----", "mount mode   : " + mount_mode_name(opts.mount_mode));

    if (opts.mount_mode == MountMode::Probe) {
        return probe_fs_layout(opts);
    }

    heap_check("before any AddSearchPath");
    if (!call_add_search_path("EXECUTABLE_PATH", opts.bin_dir, "base")) return fail("stuck");
    if (!call_add_search_path("BASE_PATH", base_dir, "base")) return fail("stuck");

    if (opts.mount_mode == MountMode::Single || opts.mount_mode == MountMode::SingleDup) {
        if (opts.single_path.empty()) return fail("--mount=single requires --path=<dir>");
        const std::string p = strip_trailing_slash(fix_slashes(opts.single_path));
        if (!call_add_search_path("GAME", p, "single")) return fail("stuck");
        if (opts.mount_mode == MountMode::SingleDup) {
            if (!call_add_search_path("MOD", p, "single-dup")) return fail("stuck");
        }
        return ok("single-path mount complete");
    }

    const auto entries = parse_search_paths(read_file_bytes_as_string(gameinfo_path));
    if (entries.empty()) return fail("no SearchPaths entries found in gameinfo.txt");
    log_line("----", "found " + std::to_string(entries.size()) + " search path entries");

    for (const auto& entry : entries) {
        std::string location = entry.location;
        const bool is_addons = contains_ci(entry.location, "addons");
        log_line("----", "entry: [" + entry.path_ids + "] <- " + location +
                             (is_addons ? "   (addons)" : ""));

        if (opts.mount_mode == MountMode::NoAddons && is_addons) {
            log_line("SKIP", "addons entry skipped (mount mode no-addons)");
            continue;
        }
        if (opts.mount_mode == MountMode::AddonsOnly && !is_addons) {
            continue;
        }

        // Skip appid mounts (need the Steam engine DLL; out of scope for the spike).
        if (location.rfind("|appid_", 0) == 0) {
            log_line("SKIP", "appid mount not supported in spike: " + location);
            continue;
        }

        std::string loc_base = base_dir;
        if (location.rfind("|gameinfo_path|", 0) == 0) {
            location = location.substr(strlen("|gameinfo_path|"));
            loc_base = opts.game_dir;
        } else if (location.rfind("|all_source_engine_paths|", 0) == 0) {
            location = location.substr(strlen("|all_source_engine_paths|"));
        }

        // Split path IDs on '+'.
        std::vector<std::string> path_ids;
        {
            const std::string& raw = entry.path_ids;
            size_t start = 0;
            while (start <= raw.size()) {
                const size_t plus = raw.find('+', start);
                const std::string id =
                    raw.substr(start, plus == std::string::npos ? std::string::npos : plus - start);
                if (!id.empty()) path_ids.push_back(id);
                if (plus == std::string::npos) break;
                start = plus + 1;
            }
        }

        if (location.find('*') != std::string::npos || location.find('?') != std::string::npos) {
            mount_wildcard(loc_base, location, path_ids, opts.mount_mode == MountMode::AddonsOnly);
        } else {
            std::string full = fix_slashes((std::filesystem::path(loc_base) / location).string());
            full = strip_trailing_slash(full);
            if (opts.mount_mode == MountMode::AddonsOnly) {
                if (!path_ids.empty() && !call_add_search_path(path_ids[0], full, "addons-first-only")) {
                    return fail("stuck");
                }
            } else {
                for (const auto& id : path_ids) {
                    if (!call_add_search_path(id, full, "entry")) return fail("stuck");
                }
            }
        }

        if (opts.mount_mode == MountMode::AddonsOnly) break;
    }
    return ok("search paths mounted");
}

// ---------------------------------------------------------------------------
// Win32 window (top-level for the spike; child-HWND integration is a UI-phase task)
// ---------------------------------------------------------------------------
LRESULT CALLBACK spike_wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_CLOSE: DestroyWindow(h); return 0;
        case WM_DESTROY: PostQuitMessage(0); return 0;
        default: return DefWindowProcW(h, m, w, l);
    }
}

StepResult create_window() {
    if (g.child_hwnd != nullptr) {
        g.hwnd = reinterpret_cast<HWND>(g.child_hwnd);
        g.owns_window = false;
        return ok("using caller-provided child window");
    }

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = spike_wndproc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
    wc.lpszClassName = L"EngineSpikeWindow";
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return fail("RegisterClassExW failed: " + win32_error(GetLastError()));
    }
    g.hwnd = CreateWindowExW(0, wc.lpszClassName, L"VMT Viewer - Engine Spike", WS_OVERLAPPEDWINDOW,
                             CW_USEDEFAULT, CW_USEDEFAULT, g.width, g.height,
                             nullptr, nullptr, wc.hInstance, nullptr);
    if (!g.hwnd) return fail("CreateWindowExW failed: " + win32_error(GetLastError()));
    g.owns_window = true;
    ShowWindow(g.hwnd, SW_SHOW);
    UpdateWindow(g.hwnd);
    return ok("window created");
}

// ---------------------------------------------------------------------------
// Steps 3-4: filesystem + material system + view
// ---------------------------------------------------------------------------
StepResult init_filesystem(const EngineOptions& opts) {
    g.tier0 = load_dll(opts.bin_dir, "tier0.dll");
    g.vstdlib = load_dll(opts.bin_dir, "vstdlib.dll");
    g.fs_mod = load_dll(opts.bin_dir, "filesystem_stdio.dll");

    if (!g.tier0 || !g.vstdlib || !g.fs_mod) {
        log_line("FAIL", std::string("load: tier0=") + (g.tier0 ? "ok" : "FAIL") +
                              " vstdlib=" + (g.vstdlib ? "ok" : "FAIL") +
                              " filesystem_stdio=" + (g.fs_mod ? "ok" : "FAIL") +
                              " win32='" + win32_error(GetLastError()) + "'");
        return fail("failed to load tier0/vstdlib/filesystem_stdio from " + opts.bin_dir);
    }

    // Initialize the command line (material system init reads it). The export is
    // "CommandLine" in GMod's tier0 and "CommandLine_Tier0" in HL2/TF2 tier0.
    typedef ICommandLine* (*PFNCommandLine)();
    const char* cmdline_export = "CommandLine";
    PFNCommandLine pfn_cmdline = reinterpret_cast<PFNCommandLine>(GetProcAddress(g.tier0, cmdline_export));
    if (!pfn_cmdline) {
        cmdline_export = "CommandLine_Tier0";
        pfn_cmdline = reinterpret_cast<PFNCommandLine>(GetProcAddress(g.tier0, cmdline_export));
    }
    if (!pfn_cmdline) return fail("tier0.dll exports neither CommandLine nor CommandLine_Tier0");
    ICommandLine* cmdline = pfn_cmdline();
    if (!cmdline) return fail("CommandLine() returned null");
    const std::string cmdline_str = "engine_spike -game \"" + opts.game_dir + "\"";
    cmdline->CreateCmdLine(cmdline_str.c_str());
    log_line("OK  ", std::string("command line initialized via ") + cmdline_export + ": " +
                         std::string(cmdline->GetCmdLine()));

    typedef void (*PFNSpewOutputFunc)(SpewOutputFunc_t);
    PFNSpewOutputFunc pfn_spew = reinterpret_cast<PFNSpewOutputFunc>(
        GetProcAddress(g.tier0, "SpewOutputFunc"));
    if (pfn_spew) {
        pfn_spew([](SpewType_t type, const tchar* msg) -> SpewRetval_t {
            if (!msg || !*msg) return SPEW_CONTINUE;
            std::string text(msg);
            while (!text.empty() && (text.back() == '\r' || text.back() == '\n')) {
                text.pop_back();
            }
            if (text.empty()) return SPEW_CONTINUE;
            const char* tag = "SRC ";
            if (type == SPEW_WARNING) tag = "WARN";
            else if (type == SPEW_ERROR || type == SPEW_ASSERT) tag = "FAIL";
            log_line(tag, text);
            return SPEW_CONTINUE;
        });
        log_line("OK  ", "installed Source engine spew output hook");
    }

    g.fs_factory = load_factory(g.fs_mod);
    if (!g.fs_factory) return fail("filesystem_stdio.dll has no CreateInterface");

    const std::string fs_dll_path = (std::filesystem::path(opts.bin_dir) / "filesystem_stdio.dll").string();
    std::vector<std::string> fs_versions = discover_versions(read_file_bytes(fs_dll_path), "VFileSystem0");
    if (fs_versions.empty()) return fail("no VFileSystem0xx version string found in filesystem_stdio.dll");
    std::string fs_ver;
    g.fs = static_cast<IFileSystem*>(find_interface(g.fs_factory, fs_versions, &fs_ver));
    if (!g.fs) {
        return fail("CreateInterface returned no IFileSystem (tried " +
                    std::to_string(fs_versions.size()) + " versions)");
    }
    log_line("OK  ", "IFileSystem resolved with " + fs_ver);

    // Init() without Connect(), matching filesystem_tools.cpp; Connect(fs_factory) corrupts AddSearchPath.
    if (g.fs->Init() != INIT_OK) return fail("IFileSystem::Init failed");
    log_line("OK  ", "IFileSystem init");

    // The SDK 2013 and Garry's Mod vtable layouts differ (see FsLayout); all
    // affected calls go through the shims, so the layout must be known first.
    StepResult layout = detect_fs_layout(opts);
    if (!layout.ok) return layout;
    log_fs_layout();

    fs_remove_all_search_paths();
    return mount_search_paths(opts);
}

void patch_vtf_version_check(HMODULE mats_mod) {
    if (mats_mod == nullptr) return;

    const unsigned char* base = reinterpret_cast<const unsigned char*>(mats_mod);
    const IMAGE_DOS_HEADER* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
    const IMAGE_NT_HEADERS* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return;
    const size_t size = nt->OptionalHeader.SizeOfImage;

    // Pattern in materialsystem.dll CVTFTexture::UnserializeHeader:
    // 8B 40 08 83 F8 04 75 04 6A 40 EB 28 83 F8 03
    static const unsigned char pattern[] = {
        0x8B, 0x40, 0x08, 0x83, 0xF8, 0x04, 0x75, 0x04, 0x6A, 0x40, 0xEB, 0x28, 0x83, 0xF8, 0x03
    };

    for (size_t i = 0; i + sizeof(pattern) <= size; ++i) {
        if (std::memcmp(base + i, pattern, sizeof(pattern)) == 0) {
            unsigned char* target = const_cast<unsigned char*>(base + i + 3); // points to 83 F8 04...
            DWORD old_protect = 0;
            if (VirtualProtect(target, 32, PAGE_EXECUTE_READWRITE, &old_protect)) {
                // 83 F8 02       cmp eax, 2
                // 72 2A          jb +0x2A (to push 30h at +0x32)
                // 6A 40          push 40h
                // EB 28          jmp +0x28 (to do_call at +0x34)
                target[0] = 0x83; target[1] = 0xF8; target[2] = 0x02; // cmp eax, 2
                target[3] = 0x72; target[4] = 0x2A;                   // jb +0x2A
                target[5] = 0x6A; target[6] = 0x40;                   // push 40h
                target[7] = 0xEB; target[8] = 0x28;                   // jmp +0x28
                for (int n = 9; n < 26; ++n) {
                    target[n] = 0x90; // NOP
                }
                VirtualProtect(target, 32, old_protect, &old_protect);
                FlushInstructionCache(GetCurrentProcess(), target, 32);
                log_line("OK  ", "patched materialsystem.dll VTF version check to support VTF 7.5+");
            }
            break;
        }
    }
}

StepResult init_material_system(const EngineOptions& opts) {
    // Garry's Mod's newer engine branch ships a material system whose Connect()
    // hard-requires engine-only interfaces (QueuedLoader, texture streaming,
    // ...; verified: it exits the process through tier0 Error -> TerminateProcess)
    // and whose vtable layout drifted from SDK 2013. A tool process cannot
    // provide those interfaces, so engine-rendered preview is unavailable
    // there. Fail cleanly before loading anything: the file system side
    // (material listing and reading files from VPKs) is fully supported.
    if (g_fs_layout.delta != 0) {
        return fail("this game's engine branch (Garry's Mod) requires engine-only "
                    "interfaces to initialize its material system, so the engine "
                    "renderer is unavailable for this install; the file system "
                    "itself works (material lists and VPK reads are supported)");
    }

    g.mats_mod = load_dll(opts.bin_dir, "materialsystem.dll");
    if (!g.mats_mod) return fail("failed to load materialsystem.dll");
    patch_vtf_version_check(g.mats_mod);

    g.mats_factory = load_factory(g.mats_mod);
    if (!g.mats_factory) return fail("materialsystem.dll has no CreateInterface");

    const std::string mats_dll_path = (std::filesystem::path(opts.bin_dir) / "materialsystem.dll").string();
    std::vector<std::string> mats_versions = discover_versions(read_file_bytes(mats_dll_path), "VMaterialSystem0");
    if (mats_versions.empty()) return fail("no VMaterialSystem0xx version string found in materialsystem.dll");
    std::string mats_ver;
    g.matsys = static_cast<IMaterialSystem*>(find_interface(g.mats_factory, mats_versions, &mats_ver));
    if (!g.matsys) return fail("CreateInterface returned no IMaterialSystem");
    log_line("OK  ", "IMaterialSystem resolved with " + mats_ver);

    // Load the shader DLL first and connect the material system with its
    // factory: shaderapidx9.dll provides MaterialSystemHardwareConfig012 (and
    // ShaderDevice001), which IMaterialSystem::Connect queries. The engine's
    // launcher does this before Init; without it SetMode crashes calling a
    // null hardware-config interface.
    g.shader_mod = load_dll(opts.bin_dir, "shaderapidx9.dll");
    if (!g.shader_mod) return fail("failed to load shaderapidx9.dll");
    CreateInterfaceFn shader_dll_factory = load_factory(g.shader_mod);
    if (!shader_dll_factory) return fail("shaderapidx9.dll has no CreateInterface");

    // IMaterialSystem::Connect queries a list of interfaces (hardware config,
    // filesystem, and several engine-only ones: InputSystem, NetworkSystem,
    // QueuedLoader, ...). The engine's launcher passes a factory that covers
    // all of them; a tool can only provide the filesystem and the shader DLL
    // ones. The SDK 2013 material system tolerates null for the engine-only
    // ones, and its return value only reflects the last (engine-only) query, so
    // it is ignored here; the hardware config is verified directly instead.
    // The real DX9 shader API registers convars during Init, so it needs the
    // cvar factory from vstdlib (NULL works only for shaderapiempty). Tools get
    // it through the documented entry point in public/vstdlib/cvar.h.
    CreateInterfaceFn cvar_factory = nullptr;
    if (g.vstdlib != nullptr) {
        typedef CreateInterfaceFn (*PFNGetICVarFactory)();
        PFNGetICVarFactory get_icvar_factory = reinterpret_cast<PFNGetICVarFactory>(
            GetProcAddress(g.vstdlib, "VStdLib_GetICVarFactory"));
        cvar_factory = get_icvar_factory ? get_icvar_factory() : load_factory(g.vstdlib);
    }
    log_line("OK  ", std::string("cvar factory: ") +
                         (cvar_factory ? "VStdLib_GetICVarFactory" : "NULL"));

    // The engine connects the cvar system before anything else uses it:
    // ICvar::Connect (IAppSystem slot 0) initializes vstdlib's internal
    // ICvarQuery (VCvarQuery001) with the engine's query, or with its built-in
    // default when none exists. materialsystem registers its console variables
    // during Init and calls through that query; without this connection
    // vstdlib hits a null query and crashes (verified against HL2 and Source
    // SDK Base 2013 binaries).
    // The interface version differs per engine branch (the SDK 2013 header asks
    // for VEngineCvar004, GMod's vstdlib only provides VEngineCvar007), so the
    // version advertised by the DLL is used instead of the header constant.
    if (cvar_factory != nullptr) {
        const std::string vstdlib_path = (std::filesystem::path(opts.bin_dir) / "vstdlib.dll").string();
        std::vector<std::string> cvar_versions =
            discover_versions(read_file_bytes(vstdlib_path), "VEngineCvar0");
        if (cvar_versions.empty()) cvar_versions.push_back(CVAR_INTERFACE_VERSION);
        std::string cvar_ver;
        ICvar* cvar_system =
            reinterpret_cast<ICvar*>(find_interface(cvar_factory, cvar_versions, &cvar_ver));
        if (cvar_system != nullptr) {
            g.cvar = cvar_system;
            log_line("OK  ", "ICvar resolved with " + cvar_ver);
            if (cvar_system->Connect(cvar_factory)) {
                log_line("OK  ", "ICvar connected (ICvarQuery initialized)");
            } else {
                log_line("WARN", "ICvar::Connect returned false");
            }
        } else {
            log_line("WARN", "no ICvar from the cvar factory");
        }
    }

    g.connect_factories[0] = shader_dll_factory;
    g.connect_factories[1] = cvar_factory;
    log_line("----", "calling IMaterialSystem::Connect (composite factory)");
    (void)g.matsys->Connect(&State::ConnectFactory);
    log_line("----", "IMaterialSystem::Connect returned");
    if (shader_dll_factory(MATERIALSYSTEM_HARDWARECONFIG_INTERFACE_VERSION, nullptr) == nullptr) {
        return fail("shaderapidx9.dll does not provide the hardware config interface");
    }
    log_line("OK  ", "matsys connected; hardware config present (engine-only interfaces null)");

    // Init directly (utilmatlib.cpp pattern): Connect(fs_factory) is not safe here.
    CreateInterfaceFn shader_factory = g.matsys->Init("shaderapidx9.dll", nullptr, g.fs_factory, cvar_factory);
    if (!shader_factory) return fail("IMaterialSystem::Init(shaderapidx9.dll) failed");
    g.matsys_inited = true;
    log_line("OK  ", "IMaterialSystem::Init (shaderapidx9) ok");

    // TEMP diagnostics: material threading state.
    log_line("THRD", "thread mode = " + std::to_string((int)g.matsys->GetThreadMode()) +
                         "  renderThreadSafe = " +
                         (g.matsys->IsRenderThreadSafe() ? "true" : "false"));
    g.matsys->SetThreadMode(MATERIAL_SINGLE_THREADED);
    log_line("THRD", "thread mode after SetThreadMode(SINGLETHREADED) = " +
                         std::to_string((int)g.matsys->GetThreadMode()));

    // The device backbuffer is created once, here. Resizing the host window
    // later never calls SetMode again: the 2013 binaries fault inside
    // shaderapidx9 when the device is reset after shaders have been used
    // (stdshader restore reads a "current vertex shader" of -1). The host
    // window size only drives the projection aspect; D3D presents the fixed
    // backbuffer scaled to the window.
    MaterialSystem_Config_t config = g.matsys->GetCurrentConfigForVideoCard();
    log_line("DIAG", "config from GetCurrentConfigForVideoCard: dxSupportLevel=" +
                         std::to_string(config.dxSupportLevel));
    config.m_VideoMode.m_Width = g.width;
    config.m_VideoMode.m_Height = g.height;
    config.SetFlag(MATSYS_VIDCFG_FLAGS_WINDOWED, true);

    if (!g.matsys->SetMode(reinterpret_cast<void*>(g.hwnd), config)) {
        return fail("IMaterialSystem::SetMode failed (device creation)");
    }
    g.buffer_width = g.width;
    g.buffer_height = g.height;
    log_line("OK  ", "device created via SetMode (" + std::to_string(g.width) + "x" +
                         std::to_string(g.height) + ")");

    if (g.cvar != nullptr) {
        ConVar* dx = g.cvar->FindVar("mat_dxlevel");
        log_line("DIAG", "after SetMode: mat_dxlevel=" +
                             (dx != nullptr ? std::to_string(dx->GetInt()) : std::string("<not registered>")));
        // TEMP EXPERIMENT: force DX9 shader selection.
        if (dx != nullptr) {
            dx->SetValue(95);
            log_line("DIAG", "forced mat_dxlevel=95, now=" + std::to_string(dx->GetInt()));
        }
    }

    if (!g.matsys->AddView(reinterpret_cast<void*>(g.hwnd))) {
        return fail("IMaterialSystem::AddView failed");
    }
    g.view_added = true;
    g.matsys->SetView(reinterpret_cast<void*>(g.hwnd));
    log_line("OK  ", "view added + set on HWND");

    static const char* kFallbackMaterials[] = {
        "dev/dev_monotone_basetexture",
        "dev/dev_measuregeneric01",
        "brick/brickwall001a",
        "metal/metalwall001a",
        "concrete/concretefloor001a",
        "wood/woodwall001a",
        "tile/tilefloor001a",
    };

    std::vector<std::string> candidates;
    candidates.push_back(opts.material);
    for (const char* m : kFallbackMaterials) {
        if (opts.material != m) candidates.push_back(m);
    }

    release_current_material();
    std::string chosen;
    for (const auto& name : candidates) {
        IMaterial* m = g.matsys->FindMaterial(name.c_str(), TEXTURE_GROUP_OTHER, false);
        if (m == nullptr) continue;
        if (!m->IsErrorMaterial()) {
            adopt_material(m);
            g.material_is_error = false;
            chosen = name;
            break;
        }
        if (g.material == nullptr) adopt_material(m);
    }

    if (!g.material_is_error) {
        log_line("OK  ", "found material: " + chosen + " (shader " + g.material->GetShaderName() + ")");
        log_texture_state(g.material, "init fallback material");
    } else {
        log_line("WARN", "no real material found from candidates; rendering error (checkerboard) material");
    }
    return ok("material system initialized");
}

} // namespace

void install_diagnostics() {
#ifdef _DEBUG
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
    _CrtSetDbgFlag(_CrtSetDbgFlag(_CRTDBG_REPORT_FLAG) | _CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
    _RTC_SetErrorFunc(rtc_handler);
    log_line("DIAG", "debug build: CRT heap checks + RTC handler installed (/RTC1)");
#endif
    AddVectoredExceptionHandler(1, veh_handler);
    log_line("DIAG", "vectored exception handler installed");
}

void apply_steam_appid(unsigned appid) {
    if (appid == 0) return;
    const std::string v = std::to_string(appid);
    SetEnvironmentVariableA("SteamAppId", v.c_str());
    SetEnvironmentVariableA("steam_appid", v.c_str());
    log_line("DIAG", "SteamAppId environment variable set to " + v);
}

StepResult init_filesystem_only(const EngineOptions& opts) {
    g = State{};
    g.width = opts.width;
    g.height = opts.height;
    g.child_hwnd = opts.child_hwnd;
    g.pump_messages = false;

    StepResult r = init_filesystem(opts);
    if (!r.ok) return r;
    log_line("OK  ", r.message);

    g.initialized = true;
    return ok("filesystem initialized");
}

StepResult init_engine(const EngineOptions& opts) {
    g = State{};
    g.width = opts.width;
    g.height = opts.height;
    g.child_hwnd = opts.child_hwnd;
    g.pump_messages = opts.pump_messages;

    StepResult r = create_window();
    if (!r.ok) return r;
    log_line("OK  ", r.message);

    r = init_filesystem(opts);
    if (!r.ok) return r;
    log_line("OK  ", r.message);

    if (opts.mount_mode != MountMode::All) {
        log_line("DIAG", "mount-mode test finished; stopping before material system");
        return ok("mount test completed without fault");
    }

    r = init_material_system(opts);
    if (!r.ok) return r;
    log_line("OK  ", r.message);

    g.initialized = true;
    return ok("engine initialized (filesystem mounted + material system + view)");
}

namespace {

struct VertexExtra {
    bool has_color = false;
    bool has_lightmap_uv = false;
    bool has_texcoord2 = false;

    void emit(CMeshBuilder& mb) const {
        if (has_color) {
            mb.Color4ub(255, 255, 255, 255);
        }
        if (has_lightmap_uv) {
            mb.TexCoord2f(1, 0.5f, 0.5f);
        }
        if (has_texcoord2) {
            mb.TexCoord2f(2, 0.0f, 0.0f);
        }
    }
};

void build_sphere(CMeshBuilder& mb, int rings, int segments, const VertexExtra& extra) {
    for (int i = 0; i <= rings; ++i) {
        const float v = (float)i / rings;
        const float phi = v * 3.14159265f;
        const float y = std::cos(phi);
        const float r = std::sin(phi);
        for (int j = 0; j <= segments; ++j) {
            const float u = (float)j / segments;
            const float theta = u * 2.0f * 3.14159265f;
            const float x = r * std::cos(theta);
            const float z = r * std::sin(theta);
            mb.Position3f(x, y, z);
            mb.Normal3f(x, y, z);
            mb.TexCoord2f(0, u, v);
            extra.emit(mb);
            mb.AdvanceVertex();
        }
    }
    for (int i = 0; i < rings; ++i) {
        for (int j = 0; j < segments; ++j) {
            const int a = i * (segments + 1) + j;
            const int b = a + 1;
            const int c = a + (segments + 1);
            const int d = c + 1;
            // Source front faces are clockwise as seen from outside; the
            // engine's default cull mode removes counter-clockwise windings.
            mb.Index((unsigned short)a); mb.AdvanceIndex();
            mb.Index((unsigned short)b); mb.AdvanceIndex();
            mb.Index((unsigned short)c); mb.AdvanceIndex();
            mb.Index((unsigned short)b); mb.AdvanceIndex();
            mb.Index((unsigned short)d); mb.AdvanceIndex();
            mb.Index((unsigned short)c); mb.AdvanceIndex();
        }
    }
}

void build_cube(CMeshBuilder& mb, const VertexExtra& extra) {
    const float h = 0.9f;
    // normal, U tangent, V tangent per face; U x V == normal so the quad
    // corner order below is front-facing (clockwise) from outside.
    const float faces[6][9] = {
        {  1,  0,  0,   0,  0, -1,   0,  1,  0 },
        { -1,  0,  0,   0,  0,  1,   0,  1,  0 },
        {  0,  1,  0,   1,  0,  0,   0,  0, -1 },
        {  0, -1,  0,   1,  0,  0,   0,  0,  1 },
        {  0,  0,  1,   1,  0,  0,   0,  1,  0 },
        {  0,  0, -1,  -1,  0,  0,   0,  1,  0 },
    };
    const float su[4] = { -1, 1, 1, -1 };
    const float sv[4] = { -1, -1, 1, 1 };
    for (int face = 0; face < 6; ++face) {
        const float* f = faces[face];
        for (int i = 0; i < 4; ++i) {
            mb.Position3f((f[0] + f[3] * su[i] + f[6] * sv[i]) * h,
                          (f[1] + f[4] * su[i] + f[7] * sv[i]) * h,
                          (f[2] + f[5] * su[i] + f[8] * sv[i]) * h);
            mb.Normal3f(f[0], f[1], f[2]);
            mb.TexCoord2f(0, (su[i] + 1.0f) * 0.5f, (1.0f - sv[i]) * 0.5f);
            extra.emit(mb);
            mb.AdvanceVertex();
        }
        const unsigned short base = (unsigned short)(face * 4);
        mb.Index(base); mb.AdvanceIndex();
        mb.Index((unsigned short)(base + 1)); mb.AdvanceIndex();
        mb.Index((unsigned short)(base + 2)); mb.AdvanceIndex();
        mb.Index(base); mb.AdvanceIndex();
        mb.Index((unsigned short)(base + 2)); mb.AdvanceIndex();
        mb.Index((unsigned short)(base + 3)); mb.AdvanceIndex();
    }
}

void build_cylinder(CMeshBuilder& mb, int segments, const VertexExtra& extra) {
    const float radius = 0.7f;
    const float half = 0.9f;
    for (int j = 0; j <= segments; ++j) {
        const float u = (float)j / segments;
        const float theta = u * 2.0f * 3.14159265f;
        const float x = std::cos(theta);
        const float z = std::sin(theta);
        mb.Position3f(x * radius, half, z * radius);
        mb.Normal3f(x, 0.0f, z);
        mb.TexCoord2f(0, u, 0.0f);
        extra.emit(mb);
        mb.AdvanceVertex();
        mb.Position3f(x * radius, -half, z * radius);
        mb.Normal3f(x, 0.0f, z);
        mb.TexCoord2f(0, u, 1.0f);
        extra.emit(mb);
        mb.AdvanceVertex();
    }
    for (int j = 0; j < segments; ++j) {
        const unsigned short a = (unsigned short)(j * 2);
        const unsigned short b = (unsigned short)(a + 1);
        const unsigned short c = (unsigned short)(a + 2);
        const unsigned short d = (unsigned short)(a + 3);
        mb.Index(a); mb.AdvanceIndex();
        mb.Index(b); mb.AdvanceIndex();
        mb.Index(c); mb.AdvanceIndex();
        mb.Index(c); mb.AdvanceIndex();
        mb.Index(b); mb.AdvanceIndex();
        mb.Index(d); mb.AdvanceIndex();
    }
    // Caps: a center vertex and a fan per side. Both windings are emitted so a
    // cap is visible regardless of the engine's culling orientation.
    for (int cap = 0; cap < 2; ++cap) {
        const float y = cap == 0 ? half : -half;
        const float ny = cap == 0 ? 1.0f : -1.0f;
        const unsigned short center = (unsigned short)((segments + 1) * 2 + cap * (segments + 2));
        mb.Position3f(0.0f, y, 0.0f);
        mb.Normal3f(0.0f, ny, 0.0f);
        mb.TexCoord2f(0, 0.5f, 0.5f);
        extra.emit(mb);
        mb.AdvanceVertex();
        for (int j = 0; j <= segments; ++j) {
            const float theta = (float)j / segments * 2.0f * 3.14159265f;
            const float x = std::cos(theta);
            const float z = std::sin(theta);
            mb.Position3f(x * radius, y, z * radius);
            mb.Normal3f(0.0f, ny, 0.0f);
            mb.TexCoord2f(0, 0.5f + x * 0.5f, 0.5f + z * 0.5f);
            extra.emit(mb);
            mb.AdvanceVertex();
        }
        for (int j = 0; j < segments; ++j) {
            const unsigned short a = (unsigned short)(center + 1 + j);
            const unsigned short b = (unsigned short)(a + 1);
            mb.Index(center); mb.AdvanceIndex();
            mb.Index(a); mb.AdvanceIndex();
            mb.Index(b); mb.AdvanceIndex();
            mb.Index(center); mb.AdvanceIndex();
            mb.Index(b); mb.AdvanceIndex();
            mb.Index(a); mb.AdvanceIndex();
        }
    }
}

void build_plane(CMeshBuilder& mb, int grid, const VertexExtra& extra) {
    for (int i = 0; i <= grid; ++i) {
        const float v = (float)i / grid;
        for (int j = 0; j <= grid; ++j) {
            const float u = (float)j / grid;
            mb.Position3f((u - 0.5f) * 1.8f, (0.5f - v) * 1.8f, 0.0f);
            mb.Normal3f(0.0f, 0.0f, 1.0f);
            mb.TexCoord2f(0, u, v);
            extra.emit(mb);
            mb.AdvanceVertex();
        }
    }
    // Both windings: a flat plane stays visible from either side.
    for (int i = 0; i < grid; ++i) {
        for (int j = 0; j < grid; ++j) {
            const unsigned short a = (unsigned short)(i * (grid + 1) + j);
            const unsigned short b = (unsigned short)(a + 1);
            const unsigned short c = (unsigned short)(a + (grid + 1));
            const unsigned short d = (unsigned short)(c + 1);
            mb.Index(a); mb.AdvanceIndex();
            mb.Index(c); mb.AdvanceIndex();
            mb.Index(b); mb.AdvanceIndex();
            mb.Index(b); mb.AdvanceIndex();
            mb.Index(c); mb.AdvanceIndex();
            mb.Index(d); mb.AdvanceIndex();
            mb.Index(a); mb.AdvanceIndex();
            mb.Index(b); mb.AdvanceIndex();
            mb.Index(c); mb.AdvanceIndex();
            mb.Index(b); mb.AdvanceIndex();
            mb.Index(d); mb.AdvanceIndex();
            mb.Index(c); mb.AdvanceIndex();
        }
    }
}

constexpr int kSphereRings = 16;
constexpr int kSphereSegments = 32;
constexpr int kCylinderSegments = 32;
constexpr int kPlaneGrid = 8;

int shape_vertex_count(PreviewShape shape) {
    switch (shape) {
    case PreviewShape::Sphere:
        return (kSphereRings + 1) * (kSphereSegments + 1);
    case PreviewShape::Cube:
        return 6 * 4;
    case PreviewShape::Cylinder:
        return (kCylinderSegments + 1) * 2 + 2 * (kCylinderSegments + 2);
    case PreviewShape::Plane:
        break;
    }
    return (kPlaneGrid + 1) * (kPlaneGrid + 1);
}

int shape_index_count(PreviewShape shape) {
    switch (shape) {
    case PreviewShape::Sphere:
        return kSphereRings * kSphereSegments * 6;
    case PreviewShape::Cube:
        return 6 * 6;
    case PreviewShape::Cylinder:
        // Sides (one winding) + two caps with both windings.
        return kCylinderSegments * 6 + 2 * kCylinderSegments * 6;
    case PreviewShape::Plane:
        break;
    }
    // Grid with both windings.
    return kPlaneGrid * kPlaneGrid * 12;
}

void build_shape(CMeshBuilder& mb, PreviewShape shape, const VertexExtra& extra) {
    switch (shape) {
    case PreviewShape::Sphere:
        build_sphere(mb, kSphereRings, kSphereSegments, extra);
        break;
    case PreviewShape::Cube:
        build_cube(mb, extra);
        break;
    case PreviewShape::Cylinder:
        build_cylinder(mb, kCylinderSegments, extra);
        break;
    case PreviewShape::Plane:
        build_plane(mb, kPlaneGrid, extra);
        break;
    }
}

unsigned char to_byte(float value) {
    if (!(value > 0.0f)) return 0; // also catches NaN
    if (value >= 1.0f) return 255;
    return (unsigned char)(value * 255.0f + 0.5f);
}

void apply_lighting(IMatRenderContext* ctx) {
    ctx->SetAmbientLight(g.lighting.ambient[0], g.lighting.ambient[1], g.lighting.ambient[2]);

    LightDesc_t key;
    std::memset(&key, 0, sizeof(key));
    key.m_Type = MATERIAL_LIGHT_DIRECTIONAL;
    key.m_Direction.Init(g.lighting.key_dir[0], g.lighting.key_dir[1], g.lighting.key_dir[2]);
    key.m_Direction.NormalizeInPlace();
    key.m_Color.Init(g.lighting.key_color[0], g.lighting.key_color[1], g.lighting.key_color[2]);
    key.m_Attenuation0 = 1.0f;
    key.m_Falloff = 1.0f;
    ctx->SetLight(0, key);

    LightDesc_t fill;
    std::memset(&fill, 0, sizeof(fill));
    fill.m_Type = MATERIAL_LIGHT_DIRECTIONAL;
    fill.m_Direction.Init(g.lighting.fill_dir[0], g.lighting.fill_dir[1], g.lighting.fill_dir[2]);
    fill.m_Direction.NormalizeInPlace();
    fill.m_Color.Init(g.lighting.fill_color[0], g.lighting.fill_color[1], g.lighting.fill_color[2]);
    fill.m_Attenuation0 = 1.0f;
    fill.m_Falloff = 1.0f;
    ctx->SetLight(1, fill);
}

// Draws a material on the given shape. Uses the caller's render context inside
// an open BeginFrame/EndFrame block.
bool draw_mesh(IMatRenderContext* ctx, IMaterial* material, PreviewShape shape) {
    if (material == nullptr) {
        return false;
    }
    ctx->Bind(material);
    ctx->BindLightmapPage(MATERIAL_SYSTEM_LIGHTMAP_PAGE_WHITE);
    IMesh* mesh = ctx->GetDynamicMesh(true, nullptr, nullptr, material);
    if (mesh == nullptr) {
        return false;
    }
    const VertexFormat_t fmt = material->GetVertexFormat();
    VertexExtra extra;
    extra.has_color = (fmt & VERTEX_COLOR) != 0;
    extra.has_lightmap_uv = TexCoordSize(1, fmt) > 0;
    extra.has_texcoord2 = TexCoordSize(2, fmt) > 0;

    CMeshBuilder mb;
    mb.Begin(mesh, MATERIAL_TRIANGLES, shape_vertex_count(shape), shape_index_count(shape));
    build_shape(mb, shape, extra);
    mb.End();
    mesh->Draw();
    return true;
}

} // namespace

StepResult render_frame(int frame) {
    if (!g.initialized || !g.matsys) return fail("engine not initialized");

    g.matsys->BeginFrame(0.0f);
    IMatRenderContext* ctx = g.matsys->GetRenderContext();
    if (!ctx) {
        g.matsys->EndFrame();
        return fail("GetRenderContext returned null");
    }

    // The backbuffer size is fixed after SetMode; the host window may be a
    // different size, so clear/draw across the whole backbuffer and let D3D
    // present it scaled to the window. Only the projection aspect follows the
    // host size, which keeps circles round under that scaling.
    ctx->Viewport(0, 0, g.buffer_width, g.buffer_height);
    ctx->ClearColor3ub(to_byte(g.lighting.background[0]), to_byte(g.lighting.background[1]),
                       to_byte(g.lighting.background[2]));
    ctx->ClearBuffers(true, true, true);

    ctx->MatrixMode(MATERIAL_PROJECTION);
    ctx->LoadIdentity();
    ctx->PerspectiveX(60.0, (double)g.width / (double)g.height, 0.1, 1000.0);

    ctx->MatrixMode(MATERIAL_VIEW);
    ctx->LoadIdentity();
    ctx->Translate(0.0f, 0.0f, -4.0f);

    apply_lighting(ctx);

    if (!draw_mesh(ctx, g.material, g.shape)) {
        g.matsys->EndFrame();
        return fail("GetDynamicMesh returned null");
    }

    g.matsys->EndFrame();
    g.matsys->SwapBuffers();

    // Pump window messages so the window stays responsive and can be closed.
    // The UI host owns its message loop, so it disables this.
    if (g.pump_messages) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    if (frame % 60 == 0) log_line("OK  ", "rendered frame " + std::to_string(frame));
    return ok("frame " + std::to_string(frame) + " rendered");
}

StepResult shutdown_engine() {
    if (!g.initialized && !g.matsys && !g.fs) return ok("nothing to shut down");

    if (g.matsys) {
        // Only call into the material system methods that are known to be safe:
        // RemoveView after a successful AddView, Shutdown after a successful
        // Init. Calling them on a partially initialized matsys (e.g. its
        // shader DLL failed to init, as on GMod) dereferences null interfaces
        // inside materialsystem.dll and crashes.
        release_current_material();
        if (g.hwnd && g.view_added) g.matsys->RemoveView(reinterpret_cast<void*>(g.hwnd));
        if (g.matsys_inited) g.matsys->Shutdown();
        g.matsys = nullptr;
        log_line("OK  ", "material system shutdown");
    }
    if (g.fs) {
        g.fs->Shutdown();
        g.fs = nullptr;
        log_line("OK  ", "file system shutdown");
    }
    if (g.hwnd && g.owns_window) {
        DestroyWindow(g.hwnd);
        log_line("OK  ", "window destroyed");
    }
    g.hwnd = nullptr;
    if (g.fs_mod) { FreeLibrary(g.fs_mod); g.fs_mod = nullptr; }
    if (g.mats_mod) { FreeLibrary(g.mats_mod); g.mats_mod = nullptr; }
    if (g.shader_mod) { FreeLibrary(g.shader_mod); g.shader_mod = nullptr; }
    if (g.vstdlib) { FreeLibrary(g.vstdlib); g.vstdlib = nullptr; }
    if (g.tier0) {
        typedef void (*PFNSpewOutputFunc)(SpewOutputFunc_t);
        PFNSpewOutputFunc pfn_spew = reinterpret_cast<PFNSpewOutputFunc>(
            GetProcAddress(g.tier0, "SpewOutputFunc"));
        if (pfn_spew) {
            pfn_spew(nullptr);
        }
        FreeLibrary(g.tier0);
        g.tier0 = nullptr;
    }

    g.initialized = false;
    return ok("engine shutdown complete");
}

// ---------------------------------------------------------------------------
// UI-phase extensions
// ---------------------------------------------------------------------------
StepResult mount_custom_path(const std::string& path_id, const std::string& full_path, bool to_head) {
    if (!g.fs) return fail("file system not initialized");
    if (path_id.empty() || full_path.empty()) return fail("mount_custom_path: empty argument");
    if (!call_add_search_path(path_id, fix_slashes(full_path), "custom", to_head)) return fail("stuck");
    return ok("mounted '" + full_path + "' as '" + path_id +
              (to_head ? "' (highest priority)" : "'"));
}

StepResult set_material(const std::string& material_name) {
    if (!g.matsys) return fail("material system not initialized");
    IMaterial* material = g.matsys->FindMaterial(material_name.c_str(), TEXTURE_GROUP_OTHER, false);
    if (material == nullptr) return fail("material not found: " + material_name);
    if (material->IsErrorMaterial()) {
        return fail("material resolved to the error material: " + material_name);
    }
    // Hand the previous material back before adopting the new one.
    adopt_material(material);
    g.material_is_error = false;
    log_line("OK  ", "preview material: " + material_name + " (shader " +
                         material->GetShaderName() + ")");
    log_texture_state(material, "set_material");
    return ok("material set: " + material_name);
}

StepResult reload_material(const std::string& material_name) {
    if (!g.matsys) return fail("material system not initialized");
    g.matsys->ReloadMaterials(material_name.c_str());
    log_line("OK  ", "reloaded materials matching '" + material_name + "'");
    // Re-resolve: the reload may have replaced the IMaterial instance.
    return set_material(material_name);
}

StepResult resize_view(int width, int height) {
    if (!g.initialized || !g.matsys || !g.hwnd) return fail("engine not initialized");
    if (width <= 0 || height <= 0) return fail("resize_view: invalid size");
    // Never call SetMode here: device resets fault inside shaderapidx9 after
    // shaders have been used (see init_material_system). The backbuffer stays
    // fixed and D3D presents it scaled to the window; tracking the host size
    // is all that is needed to keep the projection aspect correct.
    g.width = width;
    g.height = height;
    return ok("host size tracked (" + std::to_string(width) + "x" + std::to_string(height) +
              ", backbuffer " + std::to_string(g.buffer_width) + "x" +
              std::to_string(g.buffer_height) + ")");
}

ExistsResult file_exists(const std::string& game_path) {
    ExistsResult result;
    if (!g.fs) {
        return result;
    }
    result.ok = true;
    // IFileSystem::FileExists walks every mounted search path, which includes
    // the VPKs mounted from gameinfo.txt (see filesystem.h).
    result.exists = g.fs->FileExists(fix_slashes(game_path).c_str(), nullptr);
    return result;
}

// ---------------------------------------------------------------------------
// Phase 9: preview options and material browser
// ---------------------------------------------------------------------------
StepResult set_preview_shape(PreviewShape shape) {
    if (!g.matsys) return fail("material system not initialized");
    g.shape = shape;
    return ok("preview shape updated");
}

StepResult set_lighting(const Lighting& lighting) {
    if (!g.matsys) return fail("material system not initialized");
    g.lighting = lighting;
    return ok("preview lighting updated");
}

namespace {

bool ends_with_ci(const std::string& s, const std::string& suffix) {
    if (s.size() < suffix.size()) return false;
    for (size_t i = 0; i < suffix.size(); ++i) {
        const char a = (char)std::tolower((unsigned char)s[s.size() - suffix.size() + i]);
        const char b = (char)std::tolower((unsigned char)suffix[i]);
        if (a != b) return false;
    }
    return true;
}

std::string material_name_from_path(std::string path) {
    for (auto& c : path) {
        if (c == '\\') c = '/';
    }
    const std::string prefix = "materials/";
    // Case-insensitive prefix: loose game folders usually spell it lowercase,
    // but "Materials/" exists in the wild and must not be dropped.
    bool has_prefix = path.size() > prefix.size();
    for (size_t i = 0; has_prefix && i < prefix.size(); ++i) {
        if (std::tolower((unsigned char)path[i]) != prefix[i]) {
            has_prefix = false;
        }
    }
    if (!has_prefix) {
        return std::string();
    }
    std::string name = path.substr(prefix.size());
    if (!ends_with_ci(name, ".vmt")) {
        return std::string();
    }
    name.resize(name.size() - 4);
    return name;
}

} // namespace

MaterialList list_materials() {
    MaterialList result;
    if (!g.fs) {
        return result;
    }
    result.ok = true;
    constexpr size_t kMax = 50000;

    std::set<std::string> found;

    const auto add_path = [&found](const char* entry) {
        if (entry == nullptr) return;
        const std::string name = material_name_from_path(entry);
        if (!name.empty()) {
            found.insert(name);
        }
    };

    // 1) Flat wildcard scan. Depending on the engine build the wildcard matcher
    //    either stops at '/' (loose directories) or spans the whole path (VPK
    //    indices), so this catches whichever side the walk below misses.
    {
        FileFindHandle_t handle = FILESYSTEM_INVALID_FIND_HANDLE;
        const char* entry = fs_find_first("materials/*.vmt", &handle);
        size_t guard = 0;
        while (entry != nullptr && guard < 400000) {
            ++guard;
            if (!fs_find_is_directory(handle)) {
                add_path(entry);
            }
            entry = fs_find_next(handle);
        }
        if (handle != FILESYSTEM_INVALID_FIND_HANDLE) {
            fs_find_close(handle);
        }
    }

    // 2) Recursive directory walk (loose game and addon folders, and VPKs whose
    //    FindFirst reports directory entries).
    {
        std::vector<std::string> pending;
        pending.push_back("materials");
        size_t guard = 0;
        while (!pending.empty() && guard < 400000 && found.size() < kMax) {
            const std::string dir = pending.back();
            pending.pop_back();
            FileFindHandle_t handle = FILESYSTEM_INVALID_FIND_HANDLE;
            const char* entry = fs_find_first((dir + "/*").c_str(), &handle);
            while (entry != nullptr && guard < 400000) {
                ++guard;
                const std::string name = entry;
                if (name != "." && name != "..") {
                    if (fs_find_is_directory(handle)) {
                        pending.push_back(dir + "/" + name);
                    } else {
                        add_path((dir + "/" + name).c_str());
                    }
                }
                entry = fs_find_next(handle);
            }
            if (handle != FILESYSTEM_INVALID_FIND_HANDLE) {
                fs_find_close(handle);
            }
        }
    }

    result.names.assign(found.begin(), found.end());
    if (result.names.size() > kMax) {
        result.names.resize(kMax);
        result.truncated = true;
    }
    log_line("OK  ", "material scan found " + std::to_string(result.names.size()) +
                         " .vmt file(s)" + (result.truncated ? " (truncated)" : ""));
    return result;
}

TextFile read_text_file(const std::string& game_path) {
    TextFile result;
    if (!g.fs) {
        return result;
    }
    result.ok = true;
    const std::string fixed = fix_slashes(game_path);
    FileHandle_t file = g.fs->Open(fixed.c_str(), "rb", nullptr);
    if (file == FILESYSTEM_INVALID_HANDLE) {
        return result;
    }
    const unsigned int size = g.fs->Size(file);
    if (size >= (64u << 20)) {
        // Not a VMT (or a corrupt header): report not-found instead of opening
        // an empty document, so the caller surfaces a real error.
        g.fs->Close(file);
        return result;
    }
    if (size > 0) {
        result.text.resize(size);
        const int read = g.fs->Read(result.text.data(), (int)size, file);
        if (read < 0) {
            result.text.clear();
            g.fs->Close(file);
            return result;
        }
        result.text.resize((size_t)read);
    }
    g.fs->Close(file);
    result.found = true;
    return result;
}

ThumbnailResult render_material_thumbnail(const std::string& material_name, int size,
                                          bool flip_vertical) {
    ThumbnailResult result;
    if (!g.initialized || !g.matsys) {
        result.message = "engine not initialized";
        return result;
    }
    if (size < 16 || size > 512) {
        result.message = "invalid thumbnail size";
        return result;
    }
    if (size > g.buffer_width || size > g.buffer_height) {
        result.message = "the thumbnail does not fit the device backbuffer";
        return result;
    }
    IMaterial* material = g.matsys->FindMaterial(material_name.c_str(), TEXTURE_GROUP_OTHER, false);
    if (material == nullptr || material->IsErrorMaterial()) {
        result.message = "material not found: " + material_name;
        return result;
    }

    // Draw into the top-left corner of the backbuffer and read it back, like
    // the engine's own screenshot code. No render target is allocated: the
    // 2013 binaries fault inside shaderapidx9 when resources are reallocated
    // after shaders have been used (see SetMode notes above). The frame is
    // never presented, so the viewport only loses its content until the next
    // rendered frame paints over it.
    g.matsys->BeginFrame(0.0f);
    IMatRenderContext* ctx = g.matsys->GetRenderContext();
    if (ctx == nullptr) {
        g.matsys->EndFrame();
        result.message = "GetRenderContext returned null";
        return result;
    }

    ctx->Viewport(0, 0, size, size);
    ctx->ClearColor3ub(to_byte(g.lighting.background[0]), to_byte(g.lighting.background[1]),
                       to_byte(g.lighting.background[2]));
    ctx->ClearBuffers(true, true, true);

    // TEMP diagnostics: does ReadPixels see the clear at all?
    {
        unsigned char probe[4] = { 111, 112, 113, 114 };
        ctx->ReadPixels(0, 0, 1, 1, probe, IMAGE_FORMAT_BGRA8888);
        log_line("THMB", "after clear, readback = " + std::to_string(probe[0]) + "," +
                             std::to_string(probe[1]) + "," + std::to_string(probe[2]));
        ctx->Flush(true);
        unsigned char probe2[4] = { 121, 122, 123, 124 };
        ctx->ReadPixels(0, 0, 1, 1, probe2, IMAGE_FORMAT_BGRA8888);
        log_line("THMB", "after clear+flush, readback = " + std::to_string(probe2[0]) + "," +
                             std::to_string(probe2[1]) + "," + std::to_string(probe2[2]));
    }

    ctx->MatrixMode(MATERIAL_PROJECTION);
    ctx->LoadIdentity();
    ctx->PerspectiveX(60.0, 1.0, 0.1, 1000.0);
    ctx->MatrixMode(MATERIAL_VIEW);
    ctx->LoadIdentity();
    ctx->Translate(0.0f, 0.0f, -4.0f);
    apply_lighting(ctx);

    result.ok = draw_mesh(ctx, material, PreviewShape::Sphere);

    if (result.ok) {
        ctx->Flush(true);
        std::vector<unsigned char> rgb((size_t)size * (size_t)size * 3);
        ctx->ReadPixels(0, 0, size, size, rgb.data(), IMAGE_FORMAT_RGB888);
        result.bgra.resize((size_t)size * (size_t)size * 4);
        for (int i = 0; i < size * size; ++i) {
            result.bgra[(size_t)i * 4 + 0] = rgb[(size_t)i * 3 + 2]; // B
            result.bgra[(size_t)i * 4 + 1] = rgb[(size_t)i * 3 + 1]; // G
            result.bgra[(size_t)i * 4 + 2] = rgb[(size_t)i * 3 + 0]; // R
            result.bgra[(size_t)i * 4 + 3] = 255;
        }
        result.width = size;
        result.height = size;

        if (flip_vertical) {
            std::vector<unsigned char> flipped(result.bgra.size());
            const size_t row = (size_t)size * 4;
            for (int y = 0; y < size; ++y) {
                std::memcpy(flipped.data() + (size_t)y * row,
                            result.bgra.data() + (size_t)(size - 1 - y) * row, row);
            }
            result.bgra.swap(flipped);
        }
    } else {
        result.message = "the thumbnail mesh could not be drawn";
    }

    g.matsys->EndFrame(); // no SwapBuffers: the on-screen image is untouched
    return result;
}

BinaryFile read_binary_file(const std::string& game_path) {
    BinaryFile result;
    if (!g.fs) {
        result.message = "file system not initialized";
        return result;
    }
    result.ok = true;
    const std::string fixed = fix_slashes(game_path);
    FileHandle_t file = g.fs->Open(fixed.c_str(), "rb", nullptr);
    if (file == FILESYSTEM_INVALID_HANDLE) {
        result.message = "not found: " + game_path;
        return result;
    }

    const unsigned int size = g.fs->Size(file);
    // Textures are far below this; the cap only stops a corrupt Size() from
    // triggering an unbounded allocation.
    if (size > (256u << 20)) {
        g.fs->Close(file);
        result.message = "the file is too large to read: " + game_path;
        return result;
    }

    if (size > 0) {
        result.bytes.resize(size);
        const int read = g.fs->Read(result.bytes.data(), (int)size, file);
        if (read < 0) {
            result.bytes.clear();
            g.fs->Close(file);
            result.message = "the file could not be read: " + game_path;
            return result;
        }
        result.bytes.resize((size_t)read);
    }

    g.fs->Close(file);
    result.found = true;
    return result;
}

StepResult set_model(const std::string& model_path) {
    if (model_path.empty()) return fail("set_model: empty path");
    // Model preview needs the engine's StudioRender interfaces (IStudioRender
    // from studiorender.dll plus IModelInfo), which are not wired in this
    // build yet. The UI exposes the entry point so the feature can land
    // without reshaping the app; until then it reports the limitation.
    return fail("model preview is not wired yet: the StudioRender headers for "
                "this engine are pending (VMT Viewer reads VMT materials only)");
}

bool is_initialized() {
    return g.initialized;
}

} // namespace spike

// ---------------------------------------------------------------------------
// tier2/meshutils.h declares these helpers but their bodies ship only in the
// closed tier2.lib, which we don't link. Provide the standard implementations.
// The spike never exercises them (it writes explicit indices), but CMeshBuilder
// references them via the auto-index path, so they must link.
// ---------------------------------------------------------------------------
void GenerateSequentialIndexBuffer(unsigned short* p, int n, int first) {
    for (int i = 0; i < n; ++i) p[i] = (unsigned short)(first + i);
}
void GenerateQuadIndexBuffer(unsigned short* p, int n, int first) {
    for (int i = 0, quad = 0; i < n; i += 6, ++quad) {
        const unsigned short b = (unsigned short)(first + quad * 4);
        p[i + 0] = b; p[i + 1] = (unsigned short)(b + 1); p[i + 2] = (unsigned short)(b + 2);
        p[i + 3] = b; p[i + 4] = (unsigned short)(b + 2); p[i + 5] = (unsigned short)(b + 3);
    }
}
void GeneratePolygonIndexBuffer(unsigned short* p, int n, int first) {
    for (int i = 0, v = 2; i < n; i += 3, ++v) {
        p[i + 0] = (unsigned short)first;
        p[i + 1] = (unsigned short)(first + v - 1);
        p[i + 2] = (unsigned short)(first + v);
    }
}
void GenerateLineStripIndexBuffer(unsigned short* p, int n, int first) {
    for (int i = 0; i < n; i += 2) {
        const int v = i / 2;
        p[i + 0] = (unsigned short)(first + v);
        p[i + 1] = (unsigned short)(first + v + 1);
    }
}
void GenerateLineLoopIndexBuffer(unsigned short* p, int n, int first) {
    const int nVerts = n / 2;
    for (int i = 0; i < nVerts; ++i) {
        p[i * 2 + 0] = (unsigned short)(first + i);
        p[i * 2 + 1] = (unsigned short)(first + ((i + 1) % nVerts));
    }
}

// tier0 debug API as local IAT stubs (no tier0.lib linked): log, never dialog/abort.
void _SpewInfo(SpewType_t type, const tchar* pFile, int line) {
    (void)type; (void)pFile; (void)line;
}
SpewRetval_t _SpewMessage(const tchar* pMsg, ...) {
    std::printf("[ASSERT] %s\n", pMsg ? pMsg : "(null)");
    std::fflush(stdout);
    return SPEW_CONTINUE;
}
void _ExitOnFatalAssert(const tchar* pFile, int line) {
    std::printf("[ASSERT] fatal assert at %s:%d\n", pFile ? pFile : "?", line);
    std::fflush(stdout);
}
bool ShouldUseNewAssertDialog() { return false; }
bool DoNewAssertDialog(const tchar* pFile, int line, const tchar* pExpression) {
    (void)pFile; (void)line; (void)pExpression;
    return false;
}
void CallAssertFailedNotifyFunc(const char* pchFile, int nLine, const char* pchMessage) {
    (void)pchFile; (void)nLine; (void)pchMessage;
}
void Warning(const tchar* pMsg, ...) {
    std::printf("[WARNING] %s\n", pMsg ? pMsg : "(null)");
    std::fflush(stdout);
}

void (*__imp___SpewInfo)(SpewType_t, const tchar*, int) = &_SpewInfo;
SpewRetval_t (*__imp___SpewMessage)(const tchar*, ...) = &_SpewMessage;
void (*__imp___ExitOnFatalAssert)(const tchar*, int) = &_ExitOnFatalAssert;
bool (*__imp__ShouldUseNewAssertDialog)() = &ShouldUseNewAssertDialog;
bool (*__imp__DoNewAssertDialog)(const tchar*, int, const tchar*) = &DoNewAssertDialog;
void (*__imp__CallAssertFailedNotifyFunc)(const char*, int, const char*) = &CallAssertFailedNotifyFunc;
void (*__imp__Warning)(const tchar*, ...) = &Warning;
