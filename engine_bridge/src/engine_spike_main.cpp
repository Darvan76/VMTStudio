#include "bridge/dll_loader.h"
#include "bridge/pe.h"
#include "engine_spike_engine.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

namespace {

void log_line(const char* tag, const std::string& msg) {
    std::printf("[%s] %s\n", tag, msg.c_str());
    std::fflush(stdout);
}

void log_ok(const std::string& msg)   { log_line("OK  ", msg); }
void log_fail(const std::string& msg) { log_line("FAIL", msg); }
void log_info(const std::string& msg) { log_line("----", msg); }

#if defined(_M_X64)
const char* kProcessArch = "x64";
#elif defined(_M_IX86)
const char* kProcessArch = "x86";
#else
const char* kProcessArch = "unknown";
#endif

struct RequiredDll {
    const char* name;
    bool expect_create_interface;
    bool expect_command_line;
};

const RequiredDll kRequiredDlls[] = {
    { "tier0.dll",            false, true  },
    { "vstdlib.dll",          true,  false },
    { "filesystem_stdio.dll", true,  false },
    { "materialsystem.dll",   true,  false },
    { "shaderapidx9.dll",     true,  false },
};

std::string to_utf8(const wchar_t* w) {
    return std::filesystem::path(w).string();
}

void print_usage() {
    std::printf("usage: engine_spike <game bin dir> <game dir> [material] [frames] [options]\n");
    std::printf("  <game bin dir> : folder with tier0.dll, materialsystem.dll, ...\n");
    std::printf("  <game dir>     : folder containing gameinfo.txt\n");
    std::printf("options:\n");
    std::printf("  --mount=all|no-addons|addons-only|single|single-dup\n");
    std::printf("  --path=<dir>            path used by --mount=single / single-dup\n");
    std::printf("  --steam-appid=<n>       set SteamAppId before loading DLLs (e.g. 4000)\n");
    std::printf("  --resize-test           eight window resizes with rendering\n");
    std::printf("  --size-test=WxH         resize the window, then render for capture\n");
    std::printf("  example:\n");
    std::printf("    engine_spike \"D:\\SteamLibrary\\steamapps\\common\\GarrysMod\\bin\"\n");
    std::printf("                 \"D:\\SteamLibrary\\steamapps\\common\\GarrysMod\\garrysmod\"\n");
}

} // namespace

int wmain(int argc, wchar_t* argv[]) {
    spike::install_diagnostics();

    if (argc < 3) {
        print_usage();
        return 2;
    }

    const std::filesystem::path bin(argv[1]);
    const std::filesystem::path game_dir(argv[2]);

    std::string material = "dev/dev_monotone_basetexture";
    int frames = 120;
    bool have_material = false;
    bool have_frames = false;
    bool resize_test = false;
    bool size_test = false;
    int size_w = 0;
    int size_h = 0;
    spike::MountMode mount_mode = spike::MountMode::All;
    std::string single_path;
    unsigned steam_appid = 0;

    for (int i = 3; i < argc; ++i) {
        const std::string a = to_utf8(argv[i]);
        if (a.rfind("--mount=", 0) == 0) {
            const std::string v = a.substr(8);
            if (v == "all") mount_mode = spike::MountMode::All;
            else if (v == "no-addons") mount_mode = spike::MountMode::NoAddons;
            else if (v == "addons-only") mount_mode = spike::MountMode::AddonsOnly;
            else if (v == "single") mount_mode = spike::MountMode::Single;
            else if (v == "single-dup") mount_mode = spike::MountMode::SingleDup;
            else if (v == "probe") mount_mode = spike::MountMode::Probe;
            else { log_fail("unknown --mount value: " + v); return 2; }
        } else if (a.rfind("--path=", 0) == 0) {
            single_path = a.substr(7);
        } else if (a.rfind("--steam-appid=", 0) == 0) {
            steam_appid = (unsigned)std::strtoul(a.substr(14).c_str(), nullptr, 10);
        } else if (a == "--resize-test") {
            resize_test = true;
        } else if (a.rfind("--size-test=", 0) == 0) {
            size_test = true;
            const std::string v = a.substr(12);
            const size_t x = v.find('x');
            if (x == std::string::npos) {
                log_fail("--size-test expects WxH");
                return 2;
            }
            size_w = std::atoi(v.substr(0, x).c_str());
            size_h = std::atoi(v.substr(x + 1).c_str());
        } else if (!have_material) {
            material = a;
            have_material = true;
        } else if (!have_frames) {
            frames = std::atoi(a.c_str());
            have_frames = true;
        } else {
            log_fail("unexpected argument: " + a);
            print_usage();
            return 2;
        }
    }

    log_info("engine_spike: Phase 3 spike (load + mount + material system + render)");
    log_info("this process is " + std::string(kProcessArch) + "; a 32-bit build loads x86 Source DLLs");

    if (mount_mode == spike::MountMode::Single || mount_mode == spike::MountMode::SingleDup) {
        if (single_path.empty()) {
            log_fail("--mount=single/single-dup requires --path=<dir>");
            return 2;
        }
    }
    if (steam_appid != 0) {
        spike::apply_steam_appid(steam_appid);
    }

    // ---- Step 1-2: bin validation + PE arch + DLL load + exports -----------
    if (!std::filesystem::exists(bin)) {
        log_fail("bin dir does not exist: " + bin.string());
        log_fail("remediation: pass the /bin folder of an installed 32-bit Source game");
        return 1;
    }
    if (!std::filesystem::is_directory(bin)) {
        log_fail("path is not a directory: " + bin.string());
        return 1;
    }
    log_ok("bin dir exists: " + bin.string());

    bridge::LoaderOptions options;
    options.bin_dir = bin;
    for (const auto& d : kRequiredDlls) {
        options.dll_names.push_back(d.name);
    }

    const auto result = bridge::DllLoader::load(options);
    if (!result) {
        log_fail(result.error().message);
        return 1;
    }

    bool all_ok = true;
    const auto& reports = result.value();
    for (std::size_t i = 0; i < reports.size(); ++i) {
        const auto& r = reports[i];
        const RequiredDll& req = kRequiredDlls[i];

        if (!r.present) {
            log_fail("MISSING   " + r.name + "  (" + r.error + ")");
            all_ok = false;
        } else if (r.arch == bridge::PeArch::Unknown) {
            log_fail("NOT-PE    " + r.name);
            all_ok = false;
        } else if (r.arch != bridge::PeArch::X86) {
            log_fail("ARCH-X64  " + r.name + " is 64-bit; this 32-bit build cannot load it");
            all_ok = false;
        } else if (!r.loaded) {
            log_fail("LOAD-FAIL " + r.name + ": " + r.error);
            all_ok = false;
        } else if (req.expect_create_interface && !r.create_interface) {
            log_fail("NO-CI     " + r.name + ": CreateInterface export not found");
            all_ok = false;
        } else if (req.expect_command_line && !r.command_line && !r.command_line_tier0) {
            log_fail("NO-CMDLINE " + r.name + ": neither CommandLine nor CommandLine_Tier0 exported");
            all_ok = false;
        } else {
            log_ok("loaded    " + r.name + " (" + bridge::to_string(r.arch) + ")");
        }
    }
    if (!all_ok) return 1;

    // ---- Step 3-6: filesystem mount + material system + render + shutdown --
    spike::EngineOptions opts;
    opts.bin_dir = bin.string();
    opts.game_dir = game_dir.string();
    opts.material = material;
    opts.width = 800;
    opts.height = 600;
    opts.mount_mode = mount_mode;
    opts.single_path = single_path;
    opts.steam_appid = steam_appid;

    spike::StepResult init = spike::init_engine(opts);
    if (!init.ok) {
        log_fail("init_engine: " + init.message);
        spike::shutdown_engine();
        return 1;
    }
    log_ok(init.message);

    if (mount_mode != spike::MountMode::All) {
        // Mount-mode experiments stop before the material system; the shutdown
        // path still drains the filesystem and window cleanly.
        spike::StepResult shut = spike::shutdown_engine();
        log_ok(shut.message);
        return 0;
    }

    bool render_ok = true;
    if (size_test) {
        // Window resized beyond the backbuffer, then held on screen so an
        // external capture can verify D3D presents the backbuffer scaled.
        HWND wnd = FindWindowW(L"EngineSpikeWindow", nullptr);
        if (wnd == nullptr) {
            log_fail("size-test: engine window not found");
            spike::shutdown_engine();
            return 1;
        }
        SetWindowPos(wnd, nullptr, 60, 60, size_w, size_h,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        RECT rc{};
        GetClientRect(wnd, &rc);
        log_info("size-test: window client " + std::to_string(rc.right) + "x" +
                 std::to_string(rc.bottom));
        spike::StepResult rv = spike::resize_view(rc.right, rc.bottom);
        if (!rv.ok) {
            log_fail("resize_view: " + rv.message);
            spike::shutdown_engine();
            return 1;
        }
        for (int f = 0; f < 1400; ++f) {
            spike::StepResult rf = spike::render_frame(f);
            if (!rf.ok) {
                log_fail("render_frame: " + rf.message);
                render_ok = false;
                break;
            }
            Sleep(5);
        }
        log_info("size-test: render held for capture");
    } else if (resize_test) {
        // Reproduces the UI resize path: move the host window, then call
        // resize_view with the new client size exactly like the viewer does.
        // Eight cycles catch state accumulation across device resets.
        HWND wnd = FindWindowW(L"EngineSpikeWindow", nullptr);
        if (wnd == nullptr) {
            log_fail("resize-test: engine window not found");
            spike::shutdown_engine();
            return 1;
        }
        const int sizes[][2] = { { 1024, 768 }, { 640, 480 }, { 1000, 700 }, { 800, 600 },
                                 { 1280, 720 }, { 900, 650 }, { 1100, 600 }, { 800, 600 } };
        auto render_block = [&]() {
            for (int f = 0; f < 30; ++f) {
                spike::StepResult rf = spike::render_frame(f);
                if (!rf.ok) {
                    log_fail("render_frame: " + rf.message);
                    return false;
                }
                Sleep(5);
            }
            return true;
        };
        render_ok = render_block();
        for (const auto& size : sizes) {
            if (!render_ok) break;
            SetWindowPos(wnd, nullptr, 0, 0, size[0], size[1],
                         SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            RECT rc{};
            GetClientRect(wnd, &rc);
            log_info("resize-test: resize_view(" + std::to_string(rc.right) + ", " +
                     std::to_string(rc.bottom) + ")");
            spike::StepResult rv = spike::resize_view(rc.right, rc.bottom);
            if (!rv.ok) {
                log_fail("resize_view: " + rv.message);
                render_ok = false;
                break;
            }
            render_ok = render_block();
        }
    } else {
        for (int f = 0; f < frames; ++f) {
            spike::StepResult rf = spike::render_frame(f);
            if (!rf.ok) {
                log_fail("render_frame: " + rf.message);
                render_ok = false;
                break;
            }
            Sleep(5);
        }
    }
    if (render_ok) log_ok("render loop completed");

    spike::StepResult shut = spike::shutdown_engine();
    log_ok(shut.message);

    return render_ok ? 0 : 1;
}
