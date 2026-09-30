#include "common/archive.h"
#include "common/assert.h"
#include "common/common.h"
#include "common/dateTime.h"
#include "common/debug.h"
#include "common/file.h"
#include "common/stringUtils.h"
#include "common/lowMemoryGuard.h"
#include "common/systemInfo.h"
#include "common/threads.h"
#include "common/virtualMemory.h"
#include "emulator.h"
#include "graphics/host_gpu/renderer/resolutionControl.h"
#include "graphics/host_gpu/renderer/textureQuality.h"
#include "graphics/presentation/videoOut.h"
#include "graphics/presentation/window.h"
#include "kytyGitVersion.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <thread>
#include <cstdlib>
#include <fstream>
#include <cstdio>
#include <filesystem>
#include <string_view>
#include <vector>
#include <fmt/format.h>
#include <magic_enum.hpp>

using namespace Common;
using namespace Emulator;

static std::string GetBuildString() {
	Date date = Date::FromMacros(std::string(__DATE__));

#if KYTY_BUILD == KYTY_BUILD_DEBUG
	std::string type = "Debug";
#elif KYTY_BUILD == KYTY_BUILD_RELEASE
	std::string type = "Release";
#else
	std::string type = "????";
#endif

	std::string compiler = Debug::GetCompiler() + "-" + Debug::GetLinker();

	std::string str =
	    fmt::format("{}, {}, ver = {}, git = {}, date = {}", type.c_str(), compiler.c_str(),
	                KYTY_VERSION, KYTY_GIT_VERSION, date.ToString().c_str());

	return str;
}

static void PrintUsage() {
	::printf("%s\n", GetBuildString().c_str());
	::printf("kyty_emulator --game <dir|elf|zar> [options]\n\n");
	::printf("Options:\n");
	::printf("  --game <dir|elf|zar>                 Game directory, ELF, or ZArchive to load.\n");
	::printf("  --game-patch <json>                  ETAHen cheat file.\n");
	::printf("  --screen-width <num>                 Window width. Default: 1280.\n");
	::printf("  --screen-height <num>                Window height. Default: 720.\n");
	::printf(
	    "  --user-name <name>                   Local user name (1-16 bytes). Default: Kyty.\n");
	::printf("  --user-id <num>                      Local user ID. Default: %d.\n",
	         Config::DEFAULT_USER_ID);
	::printf("  --mic <name>                        Capture from this microphone; omit for silence.\n");
	::printf("  --controller-color <#RRGGBB>        Override the controller lightbar color.\n");
	::printf(
	    "  --present-mode <value>               Fifo, Mailbox, or Immediate. Default: Mailbox.\n");
	::printf(
	    "  --gpu <index>                        Vulkan physical device index. Default: auto.\n");
	::printf("  --fullscreen                         Run in borderless desktop fullscreen.\n");
	::printf("  --vr                                 Enable the virtual VR headset.\n");
	::printf("  --amd-cpu                            Apply AMD CPU instruction patches.\n");
	::printf("  --vblank-frequency <num>             Virtual vblank frequency. Default: 60.\n");
	::printf("  --console-language <0-29>            Console language. Default: 1 (English US).\n");
	::printf("  --vulkan-validation <true|false>     Enable Vulkan validation.\n");
	::printf("  --gpu-assisted-validation <t|f>      Bounds-check shader accesses on the GPU.\n"
	         "                                       Implies --vulkan-validation; very slow.\n");
	::printf("  --shader-validation <true|false>     Enable shader validation.\n");
	::printf("  --tessellation                      Draw tessellation patches; skipped by default.\n");
	::printf("  --shader-optimization-type <value>   None, Size, or Performance.\n");
	::printf("  --shader-log-direction <value>       Silent, Console, or File.\n");
	::printf("  --shader-log-folder <path>           Shader log output folder.\n");
	::printf("  --command-buffer-dump <true|false>   Enable command buffer dumps.\n");
	::printf("  --command-buffer-dump-folder <path>  Command buffer dump folder.\n");
	::printf("  --graphics-debug-dump <true|false>   Enable graphics debug dumps.\n");
	::printf("  --printf-direction <value>           Silent, Console, or File.\n");
	::printf("  --printf-output-file <path>          Guest printf output file.\n");
	::printf("  --profile                            Enable the Tracy profiler.\n");
	::printf("  --spirv-debug-printf <true|false>    Enable SPIR-V debug printf.\n");
	::printf(
	    "  --readback-linear-images <true|false> Read back writable linear images on submit.\n");
	::printf("  --playgo-hack                       Use the supplied PlayGo stub fallback.\n");
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	::printf("  --redzone                            Protect the guest SysV red zone.\n");
#endif
	::printf("  --keymap <Control=Input>             DualSense mapping; may be repeated.\n");
	::printf("  --rd                                 Enable RenderDoc capture.\n");
}

static bool NextArg(int argc, char* argv[], int& index, std::string& out) {
	if (index + 1 >= argc) {
		return false;
	}

	index++;
	out = argv[index];
	return true;
}

static bool ParseBool(const std::string& value, bool& out) {
	if (Common::EqualNoCase(value, "true") || value == "1" || Common::EqualNoCase(value, "yes") ||
	    Common::EqualNoCase(value, "on")) {
		out = true;
		return true;
	}

	if (Common::EqualNoCase(value, "false") || value == "0" || Common::EqualNoCase(value, "no") ||
	    Common::EqualNoCase(value, "off")) {
		out = false;
		return true;
	}

	return false;
}

template <typename E>
static bool ParseEnum(const std::string& value, E& out) {
	auto enum_value = magic_enum::enum_cast<E>(value.c_str());
	if (!enum_value.has_value()) {
		return false;
	}

	out = enum_value.value();
	return true;
}

static bool ParseConsoleLanguage(const std::string& value, uint32_t& out) {
	uint32_t language = 0;
	auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), language);
	if (error != std::errc {} || end != value.data() + value.size() ||
	    language > Config::MAX_CONSOLE_LANGUAGE) {
		return false;
	}
	out = language;
	return true;
}

static bool ParseUint32(const std::string& value, uint32_t& out) {
	uint32_t number   = 0;
	auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), number);
	if (error != std::errc {} || end != value.data() + value.size()) {
		return false;
	}
	out = number;
	return true;
}

static bool ParseControllerColor(const std::string& value, Config::ControllerColor& out) {
	if (value.size() != 7 || value[0] != '#') {
		return false;
	}
	uint32_t rgb = 0;
	auto [end, error] = std::from_chars(value.data() + 1, value.data() + value.size(), rgb, 16);
	if (error != std::errc {} || end != value.data() + value.size()) {
		return false;
	}
	out = {static_cast<uint8_t>(rgb >> 16), static_cast<uint8_t>(rgb >> 8),
	       static_cast<uint8_t>(rgb)};
	return true;
}

static bool ParseInt32(const std::string& value, int32_t& out) {
	int32_t number    = 0;
	auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), number);
	if (error != std::errc {} || end != value.data() + value.size()) {
		return false;
	}
	out = number;
	return true;
}

static bool ParseUserId(const std::string& value, int32_t& out) {
	int32_t user_id   = 0;
	auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), user_id);
	if (error != std::errc {} || end != value.data() + value.size() ||
	    !Config::IsConfiguredUserIdValid(user_id)) {
		return false;
	}
	out = user_id;
	return true;
}

static bool ParseArgs(int argc, char* argv[], RunOptions& options, bool& show_help) {
	show_help = false;

	for (int i = 1; i < argc; i++) {
		std::string arg = std::string(argv[i]);
		std::string value;

		if (arg == "--help" || arg == "-h") {
			show_help = true;
			continue;
		}

		if (arg == "--rd") {
			options.config.renderdoc_enabled = true;
			continue;
		}

		if (arg == "--fullscreen") {
			options.config.fullscreen_enabled = true;
			continue;
		}

		if (arg == "--vr") {
			options.config.vr_enabled = true;
			continue;
		}

		if (arg == "--amd-cpu") {
			options.config.amd_cpu_enabled = true;
			continue;
		}

		if (arg == "--playgo-hack") {
			options.config.playgo_hack_enabled = true;
			continue;
		}

		if (arg == "--tessellation") {
			options.config.tessellation_enabled = true;
			continue;
		}

		if (arg == "--profile") {
			options.config.profiler_enabled = true;
			continue;
		}

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		if (arg == "--redzone") {
			options.config.red_zone_protection_enabled = true;
			continue;
		}
#endif

		if (!arg.starts_with("--")) {
			::printf("game input must be provided with --game\n");
			return false;
		}

		if (!NextArg(argc, argv, i, value)) {
			::printf("missing value for %s\n", arg.c_str());
			return false;
		}

		if (arg == "--game") {
			if (!options.app0_dir.empty()) {
				::printf("--game can only be specified once\n");
				return false;
			}

			value           = Common::FixFilenameSlash(value);
			const auto path = Common::PathFromUtf8(value);

			if (Common::File::IsDirectoryExisting(path)) {
				options.app0_dir = path;
				options.elf      = "/app0/eboot.bin";
			} else if (Common::IsSupportedArchive(path) && Common::File::IsFileExisting(path)) {
				const auto root = Common::MakeArchivePath(path);
				if (!Common::File::IsFileExisting(root / "eboot.bin")) {
					::printf("Archive does not contain eboot.bin: %s\n", value.c_str());
					return false;
				}
				options.app0_dir = root;
				options.elf      = "/app0/eboot.bin";
			} else if (Common::File::IsFileExisting(path)) {
				options.app0_dir = path.parent_path();

				if (options.app0_dir.empty()) {
					options.app0_dir = ".";
				}

				options.elf = std::filesystem::path("/app0") / path.filename();
			} else {
				::printf("--game must point to an existing directory, ELF, or archive: %s\n",
				         value.c_str());
				return false;
			}
		} else if (arg == "--game-patch") {
			if (!options.game_patch.empty()) {
				::printf("--game-patch can only be specified once\n");
				return false;
			}
			value = Common::FixFilenameSlash(value);
			const auto path = Common::PathFromUtf8(value);

			if (!Common::File::IsFileExisting(path)) {
				::printf("--game-patch must point to an existing file: %s\n", value.c_str());
				return false;
			}
			options.game_patch = path;
		} else if (arg == "--screen-width") {
			if (!ParseUint32(value, options.config.screen_width) ||
			    options.config.screen_width == 0) {
				::printf("invalid screen width: %s\n", value.c_str());
				return false;
			}
		} else if (arg == "--screen-height") {
			if (!ParseUint32(value, options.config.screen_height) ||
			    options.config.screen_height == 0) {
				::printf("invalid screen height: %s\n", value.c_str());
				return false;
			}
		} else if (arg == "--user-name") {
			if (value.empty() || value.size() > Config::MAX_USER_NAME_LENGTH) {
				::printf("invalid user name: must contain 1-%zu bytes\n",
				         Config::MAX_USER_NAME_LENGTH);
				return false;
			}
			options.config.user_name = value;
		} else if (arg == "--user-id") {
			if (!ParseUserId(value, options.config.user_id)) {
				::printf("invalid user ID: %s\n", value.c_str());
				return false;
			}
		} else if (arg == "--mic") {
			options.config.audio_input_device = value;
		} else if (arg == "--controller-color") {
			Config::ControllerColor color {};
			if (!ParseControllerColor(value, color)) {
				::printf("invalid controller color (expected #RRGGBB): %s\n", value.c_str());
				return false;
			}
			options.config.controller_color = color;
		} else if (arg == "--present-mode") {
			if (!ParseEnum(value, options.config.present_mode)) {
				::printf("invalid present mode: %s\n", value.c_str());
				return false;
			}
		} else if (arg == "--gpu") {
			if (!ParseInt32(value, options.config.gpu_index)) {
				::printf("invalid gpu index: %s\n", value.c_str());
				return false;
			}
		} else if (arg == "--vblank-frequency") {
			if (!ParseUint32(value, options.config.vblank_frequency)) {
				::printf("invalid vblank frequency: %s\n", value.c_str());
				return false;
			}
		} else if (arg == "--console-language") {
			if (!ParseConsoleLanguage(value, options.config.console_language)) {
				::printf("invalid console language: %s\n", value.c_str());
				return false;
			}
		} else if (arg == "--vulkan-validation") {
			if (!ParseBool(value, options.config.vulkan_validation_enabled)) {
				::printf("invalid boolean for %s: %s\n", arg.c_str(), value.c_str());
				return false;
			}
		} else if (arg == "--gpu-assisted-validation") {
			if (!ParseBool(value, options.config.gpu_assisted_validation_enabled)) {
				::printf("invalid boolean for %s: %s\n", arg.c_str(), value.c_str());
				return false;
			}
		} else if (arg == "--shader-validation") {
			if (!ParseBool(value, options.config.shader_validation_enabled)) {
				::printf("invalid boolean for %s: %s\n", arg.c_str(), value.c_str());
				return false;
			}
		} else if (arg == "--shader-optimization-type") {
			if (!ParseEnum(value, options.config.shader_optimization_type)) {
				::printf("invalid shader optimization type: %s\n", value.c_str());
				return false;
			}
		} else if (arg == "--shader-log-direction") {
			if (!ParseEnum(value, options.config.shader_log_direction)) {
				::printf("invalid shader log direction: %s\n", value.c_str());
				return false;
			}
		} else if (arg == "--shader-log-folder") {
			options.config.shader_log_folder = Common::PathFromUtf8(value);
		} else if (arg == "--command-buffer-dump") {
			if (!ParseBool(value, options.config.command_buffer_dump_enabled)) {
				::printf("invalid boolean for %s: %s\n", arg.c_str(), value.c_str());
				return false;
			}
		} else if (arg == "--command-buffer-dump-folder") {
			options.config.command_buffer_dump_folder = Common::PathFromUtf8(value);
		} else if (arg == "--graphics-debug-dump") {
			if (!ParseBool(value, options.config.graphics_debug_dump_enabled)) {
				::printf("invalid boolean for %s: %s\n", arg.c_str(), value.c_str());
				return false;
			}
		} else if (arg == "--printf-direction") {
			if (!ParseEnum(value, options.config.printf_direction)) {
				::printf("invalid printf direction: %s\n", value.c_str());
				return false;
			}
		} else if (arg == "--printf-output-file") {
			options.config.printf_output_file = Common::PathFromUtf8(value);
		} else if (arg == "--spirv-debug-printf") {
			if (!ParseBool(value, options.config.spirv_debug_printf_enabled)) {
				::printf("invalid boolean for %s: %s\n", arg.c_str(), value.c_str());
				return false;
			}
		} else if (arg == "--readback-linear-images") {
			if (!ParseBool(value, options.config.readback_linear_images)) {
				::printf("invalid boolean for %s: %s\n", arg.c_str(), value.c_str());
				return false;
			}
		} else if (arg == "--keymap") {
			const auto split = value.find('=');
			if (split == std::string::npos || split == 0 || split + 1 == value.size()) {
				::printf("invalid keymap: %s\n", value.c_str());
				return false;
			}
			options.config.keymap.push_back(value);
		} else {
			::printf("unknown option: %s\n", arg.c_str());
			return false;
		}
	}

	if (options.config.gpu_assisted_validation_enabled) {
		options.config.vulkan_validation_enabled = true;
	}

	return show_help || (!options.app0_dir.empty() && !options.elf.empty());
}


// Stops the emulator before Windows runs out of RAM. A 16 GB PC holding a game's 10-11 GB plus
// other applications froze solid when free memory ran out (ASTRO BOT, Sky Garden, 173 MB free):
// the whole machine, not just the game, had to be reset. Below 1 GB free the window title asks
// to close other applications; below `stop_mib` for 2 s the emulator ends with a message (rules
// and the opt-in fast stop: common/lowMemoryGuard.h).
static void StartLowMemoryGuard(Common::LowMemoryGuard::Settings settings) {
	if (settings.stop_mib == 0) {
		return;
	}
	if (settings.fast_stop) {
		::printf("Low-memory guard: fast stop on (below %llu MB and falling fast, or below %llu MB)\n",
		         static_cast<unsigned long long>(settings.stop_mib),
		         static_cast<unsigned long long>(settings.stop_mib / 4));
	}
	std::thread([settings] {
		Common::LowMemoryGuard guard(settings);
		for (;;) {
			std::this_thread::sleep_for(std::chrono::milliseconds(500));
			const auto available = Common::AvailablePhysicalMemoryMibIfKnown();
			switch (guard.Sample(available)) {
				case Common::LowMemoryGuard::Action::Warn:
					Libs::Graphics::WindowSetStatus(fmt::format(
					    "LOW RAM: {} MB free - close other applications", available.value_or(0)));
					break;
				case Common::LowMemoryGuard::Action::ClearWarning:
					Libs::Graphics::WindowSetStatus({});
					break;
				case Common::LowMemoryGuard::Action::Stop:
					EXIT("System RAM is almost exhausted (%llu MB free). The emulator stopped so that "
					     "Windows does not freeze. Close other applications (browsers, Discord, "
					     "launchers) and start the game again; in-game saves on disk are kept. "
					     "(emulator-settings.ini: low_memory_stop_mib=%llu, 0 disables this check)\n",
					     static_cast<unsigned long long>(available.value_or(0)),
					     static_cast<unsigned long long>(settings.stop_mib));
					break;
				case Common::LowMemoryGuard::Action::None: break;
			}
		}
	}).detach();
}

// emulator-settings.ini in the working directory (the emulator's folder), one key=value per line;
// '#' starts a comment. Environment variables override it.
static void ApplyEmulatorSettings() {
	std::string resolution = "auto";
	std::string frame_cap  = "off";
	std::string low_memory = "400";
	std::string low_memory_fast_stop = "off";
	std::string texture_quality = "full";
	std::string texture_ram     = "keep";
	if (std::ifstream file("emulator-settings.ini"); file) {
		std::string line;
		while (std::getline(file, line)) {
			if (const auto hash = line.find('#'); hash != std::string::npos) {
				line.resize(hash);
			}
			const auto equals = line.find('=');
			if (equals == std::string::npos) {
				continue;
			}
			auto trim = [](std::string text) {
				const auto begin = text.find_first_not_of(" \t\r");
				const auto end   = text.find_last_not_of(" \t\r");
				return begin == std::string::npos ? std::string {} : text.substr(begin, end - begin + 1);
			};
			const auto key = trim(line.substr(0, equals));
			if (key == "render_resolution") {
				resolution = trim(line.substr(equals + 1));
			} else if (key == "frame_cap") {
				frame_cap = trim(line.substr(equals + 1));
			} else if (key == "low_memory_stop_mib") {
				low_memory = trim(line.substr(equals + 1));
			} else if (key == "low_memory_fast_stop") {
				low_memory_fast_stop = trim(line.substr(equals + 1));
			} else if (key == "texture_quality") {
				texture_quality = trim(line.substr(equals + 1));
			} else if (key == "texture_ram") {
				texture_ram = trim(line.substr(equals + 1));
			}
		}
	}
	if (const char* value = std::getenv("KYTY_RENDER_RESOLUTION"); value != nullptr) {
		resolution = value;
	}
	if (const char* value = std::getenv("KYTY_FRAME_CAP"); value != nullptr) {
		frame_cap = value;
	}
	if (const char* value = std::getenv("KYTY_LOW_MEMORY_STOP_MIB"); value != nullptr) {
		low_memory = value;
	}
	if (const char* value = std::getenv("KYTY_LOW_MEMORY_FAST_STOP"); value != nullptr) {
		low_memory_fast_stop = value;
	}
	if (const char* value = std::getenv("KYTY_TEXTURE_QUALITY"); value != nullptr) {
		texture_quality = value;
	}
	if (const char* value = std::getenv("KYTY_TEXTURE_RAM"); value != nullptr) {
		texture_ram = value;
	}
	Libs::Graphics::TextureQuality::SetTrimRam(texture_ram == "trim");
	if (texture_quality == "reduced") {
		Libs::Graphics::TextureQuality::SetReduced(true);
		::printf("Texture quality: reduced (large sampled textures without their top mip level)\n");
	} else if (texture_quality != "full" && !texture_quality.empty()) {
		::printf("emulator-settings.ini: unknown texture_quality '%s' (full, reduced)\n",
		         texture_quality.c_str());
	}
	uint64_t low_memory_mib = 400;
	if (const auto [end, error] = std::from_chars(low_memory.data(), low_memory.data() + low_memory.size(),
	                                             low_memory_mib);
	    error != std::errc {} || end != low_memory.data() + low_memory.size()) {
		::printf("emulator-settings.ini: invalid low_memory_stop_mib '%s' (MB, 0 disables)\n",
		         low_memory.c_str());
		low_memory_mib = 400;
	}
	const bool fast_stop = low_memory_fast_stop == "on" || low_memory_fast_stop == "1";
	if (!fast_stop && low_memory_fast_stop != "off" && low_memory_fast_stop != "0" &&
	    !low_memory_fast_stop.empty()) {
		::printf("emulator-settings.ini: unknown low_memory_fast_stop '%s' (on, off)\n",
		         low_memory_fast_stop.c_str());
	}
	StartLowMemoryGuard({.stop_mib = low_memory_mib, .fast_stop = fast_stop});
	uint32_t cap = 0;
	if (frame_cap == "30" || frame_cap == "20") {
		cap = static_cast<uint32_t>(std::stoul(frame_cap));
		::printf("Frame cap: %u FPS, every frame shown for the same time\n", cap);
	} else if (frame_cap != "off" && !frame_cap.empty()) {
		::printf("emulator-settings.ini: unknown frame_cap '%s' (off, 30, 20)\n", frame_cap.c_str());
	}
	Libs::VideoOut::VideoOutSetFrameCap(cap);
	uint32_t width = 0;
	if (resolution == "1080p") {
		width = 1920;
	} else if (resolution == "1440p") {
		width = 2560;
	} else if (resolution == "2160p" || resolution == "4k") {
		width = 3840;
	} else if (resolution != "auto" && !resolution.empty()) {
		::printf("emulator-settings.ini: unknown render_resolution '%s' (auto, 1080p, 1440p, 2160p)\n",
		         resolution.c_str());
	}
	Libs::Graphics::ResolutionControl::Configure(width);
}

static int Main(int argc, char* argv[]) {
	VirtualMemory::Init();
	InitializeThreads();

	RunOptions options;
	bool       show_help = false;

	if (argc < 2) {
		PrintUsage();
		return 0;
	}

	if (!ParseArgs(argc, argv, options, show_help)) {
		PrintUsage();
		return 1;
	}

	if (show_help) {
		PrintUsage();
		return 0;
	}

	ApplyEmulatorSettings();
	Run(options);

	return 0;
}

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS

int wmain(int argc, wchar_t* argv[]) {
    std::vector<std::string> utf8_args;
    utf8_args.reserve(static_cast<size_t>(argc));

    for (int index = 0; index < argc; index++) {
        const std::wstring_view wide(argv[index]);
        const std::u16string utf16(wide.begin(), wide.end());

        utf8_args.push_back(Common::Utf16ToUtf8(utf16));
    }

    std::vector<char*> utf8_argv;
    utf8_argv.reserve(utf8_args.size());

    for (auto& argument: utf8_args) {
        utf8_argv.push_back(argument.data());
    }

    return Main(argc, utf8_argv.data());
}

#else

int main(int argc, char* argv[]) {
    return Main(argc, argv);
}

#endif
