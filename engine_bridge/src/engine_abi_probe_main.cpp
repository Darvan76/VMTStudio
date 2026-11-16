// Isolated x86 factory probe. This deliberately includes no Valve headers and
// never calls methods on the returned interface objects. Factory availability
// alone does not establish compatible vtables, structures, or rendering.
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0602
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <array>
#include <cstdio>
#include <filesystem>
#include <system_error>

static_assert(sizeof(void*) == 4, "engine_abi_probe must be built for x86");

namespace {

class DllSearchDirectory final {
public:
    explicit DllSearchDirectory(const wchar_t* path) noexcept
        : cookie_(AddDllDirectory(path)) {}

    ~DllSearchDirectory() {
        if (cookie_ != nullptr) {
            RemoveDllDirectory(cookie_);
        }
    }

    DllSearchDirectory(const DllSearchDirectory&) = delete;
    DllSearchDirectory& operator=(const DllSearchDirectory&) = delete;
    DllSearchDirectory(DllSearchDirectory&&) = delete;
    DllSearchDirectory& operator=(DllSearchDirectory&&) = delete;

    bool valid() const noexcept { return cookie_ != nullptr; }

private:
    DLL_DIRECTORY_COOKIE cookie_ = nullptr;
};

using CreateInterfaceFn = void* (__cdecl*)(const char*, int*);

struct ModuleProbe {
    const wchar_t* file_name;
    const char* label;
    std::array<const char*, 3> versions;
};

// Dependency order matters: tier0 and vstdlib must be loaded from the selected
// installation before the other modules import them.
constexpr std::array<ModuleProbe, 6> kModules{{
    {L"tier0.dll", "tier0.dll", {nullptr, nullptr, nullptr}},
    {L"vstdlib.dll", "vstdlib.dll", {"VEngineCvar004", "VEngineCvar007", nullptr}},
    {L"filesystem_stdio.dll", "filesystem_stdio.dll", {"VFileSystem018", "VFileSystem022", "VFileSystem017"}},
    {L"materialsystem.dll", "materialsystem.dll", {"VMaterialSystem080", "VMaterialSystem082", nullptr}},
    {L"studiorender.dll", "studiorender.dll", {"VStudioRender025", "VStudioRender026", nullptr}},
    {L"datacache.dll", "datacache.dll", {"MDLCache004", "VDataCache003", nullptr}},
}};

int probe_modules(const std::filesystem::path& bin_dir) {
    std::printf("process_arch=x86\nrendering_verified=false\n");
    std::printf("evidence=factory_presence_only; no interface methods are called\n");
    std::printf("module\trequested\tprovided\treturn_code\n");
    std::fflush(stdout);

    for (const auto& module : kModules) {
        const auto path = bin_dir / module.file_name;
        const HMODULE loaded = LoadLibraryExW(
            path.c_str(), nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (loaded == nullptr) {
            const DWORD error = GetLastError();
            std::fprintf(stderr, "load_failed module=%s win32_error=%lu\n", module.label, error);
            return 1;
        }

        // Keep every module loaded until process termination. Explicit unload
        // can run engine destructors after dependencies have been torn down.
        std::fprintf(stderr, "loaded module=%s\n", module.label);
        if (module.versions.front() == nullptr) {
            continue;
        }

        const auto factory = reinterpret_cast<CreateInterfaceFn>(GetProcAddress(loaded, "CreateInterface"));
        if (factory == nullptr) {
            std::fprintf(stderr, "factory_missing module=%s\n", module.label);
            return 1;
        }
        for (const char* const version : module.versions) {
            if (version == nullptr) {
                continue;
            }
            int return_code = -1;
            const void* const object = factory(version, &return_code);
            std::printf("%s\t%s\t%s\t%d\n", module.label, version,
                        object != nullptr ? "true" : "false", return_code);
            std::fflush(stdout);
        }
    }
    return 0;
}

} // namespace

int wmain(int argc, wchar_t* argv[]) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    if (argc != 2) {
        std::fprintf(stderr, "usage: engine_abi_probe <bin-dir>\n");
        return 2;
    }

    std::error_code path_error;
    const auto bin_dir = std::filesystem::absolute(argv[1], path_error);
    if (path_error || !std::filesystem::is_directory(bin_dir, path_error) || path_error) {
        std::fprintf(stderr, "bin-dir must identify an existing directory\n");
        return 2;
    }
    if (!SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS)) {
        const DWORD error = GetLastError();
        std::fprintf(stderr, "SetDefaultDllDirectories failed win32_error=%lu\n", error);
        return 1;
    }
    const DllSearchDirectory dll_directory(bin_dir.c_str());
    if (!dll_directory.valid()) {
        const DWORD error = GetLastError();
        std::fprintf(stderr, "AddDllDirectory failed win32_error=%lu\n", error);
        return 1;
    }
    return probe_modules(bin_dir);
}
