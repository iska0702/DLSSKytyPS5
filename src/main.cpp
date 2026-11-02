#include "common/archive.h"
#include "common/common.h"
#include "common/dateTime.h"
#include "common/debug.h"
#include "common/file.h"
#include "common/stringUtils.h"
#include "common/threads.h"
#include "common/virtualMemory.h"
#include "emulator.h"
#include "kytyGitVersion.h"
#include "loader/x64InstructionEmulator.h"

#include <algorithm>
#include <charconv>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string_view>
#include <vector>
#include <fmt/format.h>
#include <magic_enum.hpp>

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#if defined(KYTY_PGO_GENERATE)
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>
#include <thread>
#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

// compiler-rt profile runtime (instrumented builds, CMake KYTY_PGO_GENERATE).
extern "C" void __llvm_profile_set_filename(const char*);
extern "C" int  __llvm_profile_write_file(void);
#endif

using namespace Common;
using namespace Emulator;

#if defined(KYTY_PGO_GENERATE)
// The profile runtime writes its counters only at a normal exit, but the timing harness stops the
// emulator with TerminateProcess. With KYTY_PGO_FILE=<file.profraw>, a thread writes them every
// KYTY_PGO_DUMP_SECONDS (default 20) to <file>.tmp and renames that over <file>, so the file always
// holds the last complete dump.
static void StartPgoDumps() {
	const char* file = std::getenv("KYTY_PGO_FILE");
	if (file == nullptr || *file == '\0') {
		::printf("PGO: instrumented build without KYTY_PGO_FILE: no profile is written\n");
		return;
	}
	const char* seconds_text = std::getenv("KYTY_PGO_DUMP_SECONDS");
	const int   seconds      = (seconds_text != nullptr && std::atoi(seconds_text) > 0) ? std::atoi(seconds_text) : 20;
	// "%p" in the name becomes the process id, so repeated launches (a player's own training
	// sessions) each keep their own profile instead of overwriting the previous one.
	std::string name = file;
	if (const auto at = name.find("%p"); at != std::string::npos) {
#if defined(_WIN32)
		name.replace(at, 2, std::to_string(_getpid()));
#else
		name.replace(at, 2, std::to_string(getpid()));
#endif
	}
	static const std::string target = name;
	static const std::string temp   = target + ".tmp";
	::printf("PGO: writing the profile to %s every %d s\n", target.c_str(), seconds);
	std::thread([seconds]() {
		for (uint32_t dump = 1;; dump++) {
			std::this_thread::sleep_for(std::chrono::seconds(seconds));
			__llvm_profile_set_filename(temp.c_str());
			const int       written = __llvm_profile_write_file();
			std::error_code error;
			if (written == 0) {
				std::filesystem::rename(temp, target, error);
			}
			::printf("PGO: dump %u %s\n", dump, written != 0 ? "failed to write" : (error ? "failed to rename" : "written"));
			::fflush(stdout);
		}
	}).detach();
}
#endif

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
	::printf("  --addcont-dir <dir>                  Add-ons (DLC) folder (default: none).\n");
	::printf("  --screen-width <num>                 Window width. Default: 1280.\n");
	::printf("  --screen-height <num>                Window height. Default: 720.\n");
	::printf(
	    "  --user-name <name>                   Local user name (1-16 bytes). Default: Kyty.\n");
	::printf("  --user-id <num>                      Local user ID. Default: %d.\n",
	         Config::DEFAULT_USER_ID);
	::printf("  --mic <name>                        Capture from this microphone; omit for silence.\n");
	::printf("  --controller-color <#RRGGBB>        Override the controller lightbar color.\n");
	::printf("  --controller-volume <0-100>         DualSense speaker volume. Default: 50.\n");
	::printf("  --controller-vibration <0-100>      DualSense vibration intensity. Default: 100.\n");
	::printf("  --keyboard-player <0-4>             Keyboard player; 0 shares player 1 with the gamepad. Default: 0.\n");
	::printf("  --audio-master-volume <0-200>       Volume of everything on the main output. Default: 100.\n");
	::printf("  --audio-main-volume <0-200>         Game sound (main ports). Default: 100.\n");
	::printf("  --audio-music-volume <0-200>        Music on BGM ports. Default: 100.\n");
	::printf("  --audio-pad-speaker-volume <0-200>  Controller-speaker sounds played on the main output\n"
	         "                                      when no DualSense takes them. Default: %u.\n",
	         Config::DEFAULT_AUDIO_PAD_SPEAKER_MAIN_VOLUME);
	::printf("  --audio-objects-volume <0-200>      3D audio objects (AudioOut2 object ports), mixed into\n"
	         "                                      the game sound. Default: 100.\n");
	::printf("  --audio-objects <on|off>            Play 3D audio objects (off: dropped). Default: on.\n");
	::printf(
	    "  --present-mode <value>               Fifo, Mailbox, or Immediate. Default: Mailbox.\n");
	::printf(
	    "  --gpu <index>                        Vulkan physical device index. Default: auto.\n");
	::printf("  --fullscreen                         Run in borderless desktop fullscreen.\n");
	::printf("  --hdr                                Enable HDR10 output when supported.\n");
	::printf(
	    "  --hide-cursor                        Hide the cursor after 2 s idle. Default: off.\n");
	::printf("  --no-splash                          Disable the startup splash screen.\n");
	::printf("  --vr                                 Enable the virtual VR headset.\n");
	::printf("  --amd-cpu                            Intel CPU compatibility: emulate the PS5's AMD-only\n"
	         "                                       instructions (EXTRQ/INSERTQ, AMD VRSQRTPS results).\n"
	         "                                       Default: on unless the CPU is an AMD (KYTY_AMD_CPU=\n"
	         "                                       1|0|auto overrides).\n");
	::printf("  --no-amd-cpu                         Turn Intel CPU compatibility off on a non-AMD CPU.\n");
	::printf("  --vblank-frequency <num>             Virtual vblank frequency. Default: 60.\n");
	::printf("  --console-language <0-29>            Console language. Default: 1 (English US).\n");
	::printf("  --vulkan-validation <true|false>     Enable Vulkan validation.\n");
	::printf("  --network-connected <true|false>     Use the host local network (default: false).\n");
	::printf("  --gpu-assisted-validation <t|f>      Bounds-check shader accesses on the GPU.\n"
	         "                                       Implies --vulkan-validation; very slow.\n");
	::printf("  --shader-validation <true|false>     Enable shader validation.\n");
	::printf("  --tessellation                      Draw tessellation patches; skipped by default.\n");
	::printf("  --gpu-occlusion <on|off>             Accurate GPU occlusion queries (on) or always visible\n"
	         "                                       (off, faster). Overrides KYTY_GPU_OCCLUSION (the\n"
	         "                                       bundled preset: off).\n");
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
	::printf(
	    "  --readback-tiled-images <true|false>  Read back small writable tiled images on submit.\n");
	::printf(
	    "  --sync-raw-image-buffers <true|false> Synchronize raw reads of GPU images. Default: false.\n");
	::printf(
	    "  --trophy-notifications <true|false>   Show trophy unlock toasts and play their sound.\n");
	::printf("  --perf-overlay                       Show the performance panel (F2 or Cmd+P).\n");
	::printf(
	    "  --trophy-duration <seconds>          Trophy toast duration, 5-10. Default: 6.\n");
	::printf("  --playgo-hack                       Use the supplied PlayGo stub fallback.\n");
	::printf(
	    "  --skip-notice-screen <true|false>    Skip startup logos and notices in supported games.
"
	    "                                      Default: false.
");
	::printf("  --fsr                               Enable AMD FSR 3.1 Vulkan upscaling (takes precedence over DLSS).
");
	::printf("  --fsr-fg                            Enable FSR and 2x frame generation.
");
	::printf("  --fsr-sharpness <0.0-1.0>           Set FSR sharpening strength.
");
	::printf("  --no-fsr                            Disable FSR and its frame generation.
");
	::printf("  --dlss                              Enable NVIDIA DLSS Super Resolution.
");
	::printf("  --dlss-mode <mode>                  UltraPerformance, Performance, Balanced, Quality, DLAA.
");
	::printf("  --dlss-sharpness <0.0-1.0>          Presentation CAS sharpness. Default: 0.35.
");
	::printf("  --dlss-preset <preset>              Default, PresetJ, PresetK, PresetL, PresetM.
");
	::printf("  --dlss-rr                           Request RR lighting backend (requires certified pre-denoise HDR inputs).
");
	::printf("  --dlss-diagnostics                  Enable verbose DLSS & Streamline diagnostics.
");
	::printf("  --dlss-fg                           Enable DLSS Frame Generation (2x frame multiplier).
");
	::printf("  --dlss-mfg <3x|4x|dynamic>          Enable Multi-Frame Generation (3x, 4x, or dynamic).
");
	::printf("  --reflex <on|boost|off>             NVIDIA Reflex Low Latency mode. Default: on.
");
	::printf("  --directstorage                     Enable asynchronous asset/texture streaming (Experimental).
");
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

// --amd-cpu forces the AMD instruction patch on, --no-amd-cpu off; without either it follows the
// CPU vendor: off on an AMD CPU, which runs the instructions natively. On any other CPU the game's
// EXTRQ would otherwise trap at run time, where on Windows the exception frame can overwrite the
// guest's red zone (amd_cpu_emulation_tests), and VRSQRTPS would give the host's approximation.
enum class AmdCpuChoice { Automatic, On, Off };

static void ResolveAmdCpu(AmdCpuChoice choice, const std::string& source, Config::ConfigOptions& config) {
	if (choice != AmdCpuChoice::Automatic) {
		config.amd_cpu_enabled  = choice == AmdCpuChoice::On;
		config.amd_cpu_decision = fmt::format("{} ({})", config.amd_cpu_enabled ? "on" : "off", source);
		return;
	}
	const auto vendor = Loader::X64InstructionEmulator::HostCpuVendor();
#if defined(__APPLE__)
	// The launcher does not offer the patch on macOS, and Darwin's signal frames keep the red zone.
	config.amd_cpu_enabled  = false;
	config.amd_cpu_decision = fmt::format("off (automatic on macOS, CPU vendor {})", vendor);
#else
	config.amd_cpu_enabled  = !Loader::X64InstructionEmulator::IsAmdCpuVendor(vendor);
	config.amd_cpu_decision = fmt::format("{} (automatic: CPU vendor {}{})", config.amd_cpu_enabled ? "on" : "off",
	                                      vendor, config.amd_cpu_enabled ? " is not AMD" : "");
#endif
}

static bool ParseArgs(int argc, char* argv[], RunOptions& options, bool& show_help) {
	show_help = false;
	AmdCpuChoice amd_cpu        = AmdCpuChoice::Automatic;
	std::string  amd_cpu_source = "automatic";

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

		if (arg == "--hdr") {
			options.config.hdr_enabled = true;
			continue;
		}

		if (arg == "--hide-cursor") {
			options.config.hide_cursor_enabled = true;
			continue;
		}

		if (arg == "--no-splash") {
			options.config.splash_enabled = false;
			continue;
		}

		if (arg == "--vr") {
			options.config.vr_enabled = true;
			continue;
		}

		if (arg == "--perf-overlay") {
			options.config.perf_overlay_enabled = true;
			continue;
		}

		if (arg == "--amd-cpu" || arg == "--no-amd-cpu") {
			amd_cpu        = arg == "--amd-cpu" ? AmdCpuChoice::On : AmdCpuChoice::Off;
			amd_cpu_source = arg;
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

		if (arg == "--no-dlss" || arg == "--dlss=off" || arg == "--dlss=false") {
			options.config.dlss_enabled = false;
			options.config.dlss_mode = 0;
			continue;
		}

		if (arg == "--fsr" || arg == "--fsr-fg") {
			options.config.fsr_enabled = true;
			options.config.fsr_fg_enabled |= arg == "--fsr-fg";
			continue;
		}
		if (arg == "--no-fsr") {
			options.config.fsr_enabled = false;
			options.config.fsr_fg_enabled = false;
			continue;
		}

		if (arg == "--dlss") {
			options.config.dlss_enabled = true;
			if (i + 1 < argc) {
				std::string next_val = argv[i + 1];
				std::string lower_val = next_val;
				std::transform(lower_val.begin(), lower_val.end(), lower_val.begin(), ::tolower);
				if (lower_val == "ultraperformance" || lower_val == "ultra_performance" || lower_val == "ultra-performance") {
					options.config.dlss_mode = 1;
					i++;
				} else if (lower_val == "performance") {
					options.config.dlss_mode = 2;
					i++;
				} else if (lower_val == "balanced") {
					options.config.dlss_mode = 3;
					i++;
				} else if (lower_val == "quality") {
					options.config.dlss_mode = 4;
					i++;
				} else if (lower_val == "dlaa") {
					options.config.dlss_mode = 5;
					i++;
				} else if (lower_val == "off" || lower_val == "disabled" || lower_val == "false" || lower_val == "0") {
					options.config.dlss_enabled = false;
					options.config.dlss_mode = 0;
					i++;
				}
			}
			continue;
		}

		if (arg == "--dlss-rr") {
			options.config.dlss_enabled = true;
			options.config.dlss_rr_enabled = true;
			continue;
		}

		if (arg == "--dlss-diagnostics") {
			options.config.dlss_diagnostics = true;
			continue;
		}

		if (arg == "--dlss-fg") {
			options.config.dlss_enabled = true;
			options.config.dlss_fg_enabled = true;
			if (i + 1 < argc) {
				std::string next_val = argv[i + 1];
				std::string lower_val = next_val;
				std::transform(lower_val.begin(), lower_val.end(), lower_val.begin(), ::tolower);
				if (lower_val == "2x" || lower_val == "on" || lower_val == "2") {
					options.config.dlss_fg_mode = 1;
					i++;
				} else if (lower_val == "3x" || lower_val == "3") {
					options.config.dlss_fg_mode = 2;
					i++;
				} else if (lower_val == "4x" || lower_val == "4") {
					options.config.dlss_fg_mode = 3;
					i++;
				} else if (lower_val == "dynamic") {
					options.config.dlss_fg_mode = 4;
					i++;
				}
			}
			continue;
		}

		if (arg == "--directstorage") {
			options.config.directstorage_enabled = true;
			continue;
		}

		if (arg == "--no-directstorage" || arg == "--directstorage=off" || arg == "--directstorage=false") {
			options.config.directstorage_enabled = false;
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
		} else if (arg == "--guest-exec") {
			options.elf = Common::PathFromUtf8(Common::FixFilenameSlash(value));
			options.config.guest_arguments.assign(argv + i + 1, argv + argc);
			break;
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
		} else if (arg == "--addcont-dir") {
			if (!options.addcont_dir.empty()) {
				::printf("--addcont-dir can only be specified once\n");
				return false;
			}
			options.addcont_dir = Common::PathFromUtf8(Common::FixFilenameSlash(value));
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
		} else if (arg == "--controller-volume") {
			if (!ParseUint32(value, options.config.controller_speaker_volume) ||
			    options.config.controller_speaker_volume > 100) {
				::printf("invalid controller volume: %s\n", value.c_str());
				return false;
			}
		} else if (arg == "--controller-vibration") {
			if (!ParseUint32(value, options.config.controller_vibration_intensity) ||
			    options.config.controller_vibration_intensity > 100) {
				::printf("invalid controller vibration intensity: %s\n", value.c_str());
				return false;
			}
		} else if (arg == "--keyboard-player") {
			if (!ParseUint32(value, options.config.keyboard_player) ||
			    options.config.keyboard_player > static_cast<uint32_t>(Config::MAX_PLAYERS)) {
				::printf("invalid keyboard player: %s\n", value.c_str());
				return false;
			}
		} else if (arg == "--audio-master-volume" || arg == "--audio-main-volume" ||
		           arg == "--audio-music-volume" || arg == "--audio-pad-speaker-volume" ||
		           arg == "--audio-objects-volume") {
			auto& target = arg == "--audio-master-volume" ? options.config.audio_master_volume
			               : arg == "--audio-main-volume" ? options.config.audio_main_volume
			               : arg == "--audio-music-volume"
			                   ? options.config.audio_music_volume
			               : arg == "--audio-objects-volume"
			                   ? options.config.audio_objects_volume
			                   : options.config.audio_pad_speaker_main_volume;
			if (!ParseUint32(value, target) || target > Config::MAX_AUDIO_VOLUME) {
				::printf("invalid %s (expected 0-%u): %s\n", arg.c_str(), Config::MAX_AUDIO_VOLUME,
				         value.c_str());
				return false;
			}
		} else if (arg == "--audio-objects") {
			if (!ParseBool(value, options.config.audio_objects_enabled)) {
				::printf("invalid boolean for %s: %s\n", arg.c_str(), value.c_str());
				return false;
			}
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
		} else if (arg == "--trophy-duration") {
			if (!ParseUint32(value, options.config.trophy_duration_seconds)) {
				::printf("invalid trophy duration: %s\n", value.c_str());
				return false;
			}
		} else if (arg == "--console-language") {
			if (!ParseConsoleLanguage(value, options.config.console_language)) {
				::printf("invalid console language: %s\n", value.c_str());
				return false;
			}
		} else if (arg == "--gpu-occlusion") {
			bool on = false;
			if (!ParseBool(value, on)) {
				::printf("invalid boolean for %s: %s\n", arg.c_str(), value.c_str());
				return false;
			}
			// The renderer reads KYTY_GPU_OCCLUSION once, at the first occlusion query.
#ifdef _WIN32
			_putenv_s("KYTY_GPU_OCCLUSION", on ? "1" : "0");
#else
			setenv("KYTY_GPU_OCCLUSION", on ? "1" : "0", 1);
#endif
		} else if (arg == "--network-connected") {
			if (!ParseBool(value, options.config.network_connected)) {
				::printf("invalid boolean for %s: %s\n", arg.c_str(), value.c_str());
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
		} else if (arg == "--readback-tiled-images") {
			if (!ParseBool(value, options.config.readback_tiled_images)) {
				::printf("invalid boolean for %s: %s\n", arg.c_str(), value.c_str());
				return false;
			}
		} else if (arg == "--sync-raw-image-buffers") {
			if (!ParseBool(value, options.config.sync_raw_image_buffers)) {
				::printf("invalid boolean for %s: %s\n", arg.c_str(), value.c_str());
				return false;
			}
		} else if (arg == "--trophy-notifications") {
			if (!ParseBool(value, options.config.trophy_enabled)) {
				::printf("invalid boolean for %s: %s\n", arg.c_str(), value.c_str());
				return false;
			}
		} else if (arg == "--skip-notice-screen") {
			if (!ParseBool(value, options.config.skip_notice_screen)) {
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
		} else if (arg == "--dlss-mode") {
			std::string lower_val = value;
			std::transform(lower_val.begin(), lower_val.end(), lower_val.begin(), ::tolower);
			if (lower_val == "ultraperformance" || lower_val == "ultra_performance" || lower_val == "ultra-performance") {
				options.config.dlss_mode = 1;
			} else if (lower_val == "performance") {
				options.config.dlss_mode = 2;
			} else if (lower_val == "balanced") {
				options.config.dlss_mode = 3;
			} else if (lower_val == "quality") {
				options.config.dlss_mode = 4;
			} else if (lower_val == "dlaa") {
				options.config.dlss_mode = 5;
			} else if (lower_val == "off" || lower_val == "disabled" || lower_val == "false" || lower_val == "0") {
				options.config.dlss_mode = 0;
				options.config.dlss_enabled = false;
			} else {
				uint32_t mode_val = 0;
				if (ParseUint32(value, mode_val) && mode_val <= 5) {
					options.config.dlss_mode = mode_val;
				} else {
					::printf("invalid dlss mode: %s\n", value.c_str());
					return false;
				}
			}
		} else if (arg == "--dlss-sharpness" || arg == "--fsr-sharpness") {
			float sharpness = 0.0f;
			try {
				sharpness = std::stof(value);
			} catch (...) {
				::printf("invalid dlss sharpness: %s\n", value.c_str());
				return false;
			}
			options.config.dlss_sharpness = std::clamp(sharpness, 0.0f, 1.0f);
		} else if (arg == "--dlss-preset") {
			if (value == "PresetJ" || value == "preset_j" || value == "J") {
				options.config.dlss_preset = 1;
			} else if (value == "PresetK" || value == "preset_k" || value == "K") {
				options.config.dlss_preset = 2;
			} else if (value == "PresetL" || value == "preset_l" || value == "L") {
				options.config.dlss_preset = 3;
			} else if (value == "PresetM" || value == "preset_m" || value == "M") {
				options.config.dlss_preset = 4;
			} else if (value == "Default" || value == "default") {
				options.config.dlss_preset = 0;
			} else {
				uint32_t preset_val = 0;
				if (ParseUint32(value, preset_val) && preset_val <= 4) {
					options.config.dlss_preset = preset_val;
				} else {
					::printf("invalid dlss preset: %s\n", value.c_str());
					return false;
				}
			}
		} else if (arg == "--dlss-mfg") {
			options.config.dlss_enabled = true;
			options.config.dlss_fg_enabled = true;
			if (value == "3x" || value == "3X" || value == "3") {
				options.config.dlss_fg_mode = 2; // MFG_3X
			} else if (value == "4x" || value == "4X" || value == "4") {
				options.config.dlss_fg_mode = 3; // MFG_4X
			} else if (value == "dynamic" || value == "Dynamic") {
				options.config.dlss_fg_mode = 4; // MFG_Dynamic
			} else if (value == "2x" || value == "2X" || value == "2") {
				options.config.dlss_fg_mode = 1; // FG_2X
			} else {
				::printf("invalid dlss mfg mode (expected 2x, 3x, 4x, dynamic): %s\n", value.c_str());
				return false;
			}
		} else if (arg == "--reflex") {
			if (value == "on" || value == "On" || value == "1") {
				options.config.reflex_mode = 1;
			} else if (value == "boost" || value == "Boost" || value == "2") {
				options.config.reflex_mode = 2;
			} else if (value == "off" || value == "Off" || value == "0") {
				options.config.reflex_mode = 0;
			} else {
				::printf("invalid reflex mode (expected on, boost, off): %s\n", value.c_str());
				return false;
			}
		} else if (arg == "--directstorage") {
			if (!ParseBool(value, options.config.directstorage_enabled)) {
				::printf("invalid boolean for %s: %s\n", arg.c_str(), value.c_str());
				return false;
			}
		} else {
			::printf("unknown option: %s\n", arg.c_str());
			return false;
		}
	}

	if (options.config.gpu_assisted_validation_enabled) {
		options.config.vulkan_validation_enabled = true;
	}

	// KYTY_AMD_CPU=1|0|auto overrides --amd-cpu / --no-amd-cpu (the launcher's "Intel CPU
	// compatibility"), for presets and test harnesses.
	if (const char* env = std::getenv("KYTY_AMD_CPU"); env != nullptr && env[0] != '\0') {
		amd_cpu        = std::strcmp(env, "auto") == 0 ? AmdCpuChoice::Automatic
		                 : std::strcmp(env, "0") == 0  ? AmdCpuChoice::Off
		                                               : AmdCpuChoice::On;
		amd_cpu_source = amd_cpu == AmdCpuChoice::Automatic ? "automatic" : fmt::format("KYTY_AMD_CPU={}", env);
	}
	ResolveAmdCpu(amd_cpu, amd_cpu_source, options.config);

	return show_help || (!options.app0_dir.empty() && !options.elf.empty());
}

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
// NVIDIA Smooth Motion makes its frames with CUDA in the driver's present layer, which initializes
// CUDA only when the swapchain is created. By then the guest address space owns every free range
// below HOST_USER_MAX (memoryAddressSpace.inc), CUDA finds no room for its address range there, and
// cuInit fails with CUDA_ERROR_OUT_OF_MEMORY (2): Smooth Motion stays off (the layer's log is in
// C:/NvPresent/logs). So CUDA is initialized here, before any reservation. Its range must not land
// where the guest maps at fixed addresses (the default map base 0x200000000 and up, the AGC driver
// memory at 0xfe0000000), so the free ranges below a floor are held for the duration of cuInit and
// released right after it: CUDA's range lands above the floor, and the guest address space later
// reserves everything around it.
// KYTY_CUDA_EARLY_INIT=1 turns it on (default off: it adds a DLL, CUDA threads and startup time to
// every NVIDIA run, while only RTX 40/50 drivers with Smooth Motion gain anything).
// KYTY_CUDA_VA_FLOOR_GB: the floor in GiB, default and minimum 1024. The floor must sit above
// the whole guest range (HOST_USER_MAX + 1 = 1008 GiB): whatever CUDA reserves below it would be a
// permanent hole in guest VA, and a fixed-address guest mapping there would fail.
// Without nvcuda.dll (no NVIDIA driver) nothing happens.
static void InitCudaEarly() {
	if (const char* value = std::getenv("KYTY_CUDA_EARLY_INIT"); value == nullptr || std::strcmp(value, "1") != 0) {
		return;
	}
	HMODULE cuda = LoadLibraryExW(L"nvcuda.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
	if (cuda == nullptr) {
		return;
	}
	using CuInit    = int(__stdcall*)(unsigned int);
	const auto init = reinterpret_cast<CuInit>(GetProcAddress(cuda, "cuInit"));
	if (init == nullptr) {
		return;
	}
	// 1024 GiB = 0x100_0000_0000, above HOST_USER_MAX (0xfb_ffff_ffff); the cap keeps the shift sane.
	constexpr uint64_t MinFloorGb = 1024;
	constexpr uint64_t MaxFloorGb = 65536;
	uint64_t           floor_gb   = MinFloorGb;
	if (const char* value = std::getenv("KYTY_CUDA_VA_FLOOR_GB"); value != nullptr && std::atoi(value) > 0) {
		floor_gb = std::clamp<uint64_t>(static_cast<uint64_t>(std::atoi(value)), MinFloorGb, MaxFloorGb);
	}
	const uint64_t floor = floor_gb << 30u;
	// Hold the free ranges below the floor.
	std::vector<void*> held;
	uint64_t           next = 0x10000;
	while (next < floor) {
		MEMORY_BASIC_INFORMATION info {};
		if (VirtualQuery(reinterpret_cast<const void*>(next), &info, sizeof(info)) == 0) {
			break;
		}
		const auto base = reinterpret_cast<uint64_t>(info.BaseAddress);
		const auto end  = std::min<uint64_t>(floor, base + static_cast<uint64_t>(info.RegionSize));
		if (end <= next) {
			break;
		}
		// Reservations start on the 64 KiB allocation granularity.
		const auto start = (std::max<uint64_t>(base, next) + 0xffffu) & ~uint64_t {0xffff};
		if (info.State == MEM_FREE && end > start) {
			void* ptr = VirtualAlloc(reinterpret_cast<void*>(start), end - start, MEM_RESERVE, PAGE_NOACCESS);
			if (ptr != nullptr) {
				held.push_back(ptr);
			}
		}
		next = end;
	}
	const int result = init(0);
	for (void* ptr: held) {
		VirtualFree(ptr, 0, MEM_RELEASE);
	}
	::printf("CUDA: initialized before the guest address space, above %" PRIu64 " GiB (cuInit %d%s)\n", floor_gb,
	         result, result == 0 ? "" : ", NVIDIA Smooth Motion will not start");
}
#endif

static int Main(int argc, char* argv[]) {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	_putenv("DISABLE_VULKAN_OBS_CAPTURE=1");
	_putenv("VK_LOADER_LAYERS_DISABLE=VK_LAYER_OBS_HOOK,VK_LAYER_RTSS");
	InitCudaEarly();
#endif
	VirtualMemory::Init();
	InitializeThreads();
#if defined(KYTY_PGO_GENERATE)
	// After the address-space reservations: the thread's stack must not land in the guest range.
	StartPgoDumps();
#endif

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
