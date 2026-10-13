#include "loader/gamePatch.h"

#include "common/assert.h"
#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "common/stringUtils.h"
#include "common/virtualMemory.h"
#include "graphics/presentation/dlss/dlssConfig.h"
#include "kernel/memory.h"
#include "loader/elf.h"
#include "loader/gamePatchFilter.h"
#include "loader/gamePatchIdentity.h"
#include "loader/runtimeLinker.h"
#include "loader/systemContent.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <utility>
#include <vector>

namespace Loader::GamePatch {

namespace {

constexpr uint64_t kPageSize = 0x4000;

struct Write {
	uint64_t             source_address = 0;
	uint64_t             target_address = 0;
	std::vector<uint8_t> off;
	std::vector<uint8_t> on;
};

struct Plan {
	std::string              title_id;
	std::string              version;
	std::string              process;
	std::string              executable_sha256;
	std::vector<Write>       writes;
	std::vector<std::string> mod_names;
	std::vector<uint64_t>    cave_pages;
};

using Json = nlohmann::json;

std::unique_ptr<Plan> g_pending_plan;
std::vector<uint64_t> g_applied_cave_pages;

bool Fail(std::string* error, std::string message) {
	*error = std::move(message);
	return false;
}

bool ParseBytes(const std::string& text, std::vector<uint8_t>* bytes) {
	if (text.empty() || (text.size() % 2) != 0) {
		return false;
	}

	bytes->resize(text.size() / 2);
	for (size_t index = 0; index < bytes->size(); index++) {
		(*bytes)[index] = static_cast<uint8_t>(std::stoul(text.substr(index * 2, 2), nullptr, 16));
	}
	return true;
}

bool LoadPlan(const std::filesystem::path& path, Plan* plan, std::string* error) {
	std::ifstream file(path, std::ios::binary);
	if (!file) {
		return Fail(error, "could not read cheat file");
	}

	const auto root = Json::parse(file, nullptr, false);
	if (!root.is_object() || !root.contains("id") || !root.contains("version") ||
	    !root.contains("process") || !root.contains("mods")) {
		return Fail(error, "expected a GoldHEN-style mods JSON file");
	}
	plan->title_id = root["id"].get<std::string>();
	plan->version  = root["version"].get<std::string>();
	plan->process  = root["process"].get<std::string>();
	plan->executable_sha256 = root.value("executable_sha256", std::string {});
	if (plan->version == "*" && !IsPatchSHA256(plan->executable_sha256)) {
		return Fail(error, "version-independent patches require an executable SHA-256");
	}

	const auto* disable_env = std::getenv(kDisableEnv);
	const auto  patterns    = ParseDisableList(disable_env != nullptr ? disable_env : "");
	std::vector<ModEntry> entries;
	for (const auto& mod: root["mods"]) {
		ModEntry entry;
		if (mod.contains("name") && mod["name"].is_string()) {
			entry.name = mod["name"].get<std::string>();
		}
		entry.enabled = mod.value("enabled", true);
		entries.push_back(std::move(entry));
	}

	// Automatic internal render resolution selection when DLSS is active
	if (Config::DLSSEnabled() && Config::GetDLSSMode() != static_cast<uint32_t>(Libs::Graphics::DLSSMode::Off)) {
		const auto dlss_mode = static_cast<Libs::Graphics::DLSSMode>(Config::GetDLSSMode());
		const auto target_w  = Config::GetScreenWidth();
		const auto target_h  = Config::GetScreenHeight();

		float scale = 1.0f;
		switch (dlss_mode) {
			case Libs::Graphics::DLSSMode::DLAA:
				scale = 1.0f;
				break;
			case Libs::Graphics::DLSSMode::Quality:
				scale = 0.666667f; // ~66.7% (1.5x upscaling)
				break;
			case Libs::Graphics::DLSSMode::Balanced:
				scale = 0.58f;     // ~58% (1.72x upscaling)
				break;
			case Libs::Graphics::DLSSMode::Performance:
				scale = 0.50f;     // 50% (2.0x upscaling)
				break;
			case Libs::Graphics::DLSSMode::UltraPerformance:
				scale = 0.333333f; // ~33.3% (3.0x upscaling)
				break;
			default:
				break;
		}

		const uint32_t ideal_w = static_cast<uint32_t>(std::round(target_w * scale));
		const uint32_t ideal_h = static_cast<uint32_t>(std::round(target_h * scale));

		bool user_has_enabled_resolution_mod = false;
		std::string user_enabled_mod_name;
		for (auto& entry: entries) {
			if (entry.enabled && entry.name.find("render resolution") != std::string::npos) {
				if (dlss_mode == Libs::Graphics::DLSSMode::DLAA &&
				    (entry.name.find("Native") == std::string::npos ||
				     entry.name.find(fmt::format("({}x{})", target_w, target_h)) == std::string::npos)) {
					entry.enabled = false;
					continue;
				}
				if (!user_has_enabled_resolution_mod) {
					user_has_enabled_resolution_mod = true;
					user_enabled_mod_name = entry.name;
				} else {
					// Disable conflicting extra resolution mods if multiple were enabled in file
					entry.enabled = false;
				}
			}
		}

		if (user_has_enabled_resolution_mod) {
			Log::WriteToConsoleAndLog(fmt::format(
			    "Game patch: [DLSS] Keeping user-configured mod \"{}\" (DLSS will upscale from it)\n",
			    user_enabled_mod_name));
		} else {
			// Find available resolution mods and select the one closest to the ideal DLSS percentage
			int best_index = -1;
			int64_t best_diff = -1;

			for (size_t i = 0; i < entries.size(); i++) {
				const auto& entry = entries[i];
				if (entry.name.find("render resolution") == std::string::npos) {
					continue;
				}
				if (dlss_mode == Libs::Graphics::DLSSMode::DLAA && entry.name.find("Native") == std::string::npos)
					continue;
				// Parse WxH from mod name
				uint32_t mod_w = 0, mod_h = 0;
				// "Fixed" itself contains 'x'; find the numeric WxH separator.
				size_t x_pos = 0;
				while ((x_pos = entry.name.find('x', x_pos)) != std::string::npos) {
					if (x_pos > 0 && x_pos + 1 < entry.name.size() &&
					    std::isdigit(static_cast<unsigned char>(entry.name[x_pos - 1])) &&
					    std::isdigit(static_cast<unsigned char>(entry.name[x_pos + 1]))) break;
					++x_pos;
				}
				if (x_pos != std::string::npos && x_pos > 0 && x_pos + 1 < entry.name.size()) {
					size_t start_w = x_pos;
					while (start_w > 0 && std::isdigit(static_cast<unsigned char>(entry.name[start_w - 1]))) {
						start_w--;
					}
					size_t end_h = x_pos + 1;
					while (end_h < entry.name.size() && std::isdigit(static_cast<unsigned char>(entry.name[end_h]))) {
						end_h++;
					}
					if (start_w < x_pos && end_h > x_pos + 1) {
						mod_w = static_cast<uint32_t>(std::stoul(entry.name.substr(start_w, x_pos - start_w)));
						mod_h = static_cast<uint32_t>(std::stoul(entry.name.substr(x_pos + 1, end_h - (x_pos + 1))));
					}
				}

				if (mod_w > 0 && mod_h > 0) {
					// Compare difference in height (primary) and width to ideal DLSS render resolution
					const int64_t diff = std::abs(static_cast<int64_t>(mod_h) - static_cast<int64_t>(ideal_h)) * 1000 +
					                     std::abs(static_cast<int64_t>(mod_w) - static_cast<int64_t>(ideal_w));
					if (best_diff < 0 || diff < best_diff) {
						best_diff = diff;
						best_index = static_cast<int>(i);
					}
				}
			}

			if (best_index >= 0) {
				for (size_t i = 0; i < entries.size(); i++) {
					if (entries[i].name.find("render resolution") != std::string::npos) {
						if (static_cast<int>(i) == best_index) {
							entries[i].enabled = true;
							Log::WriteToConsoleAndLog(fmt::format(
							    "Game patch: [DLSS] Auto-activated resolution mod \"{}\" for {} ({:.0f}% of {}x{} -> ideal: {}x{})\n",
							    entries[i].name, Libs::Graphics::DLSSModeToString(dlss_mode),
							    scale * 100.0f, target_w, target_h, ideal_w, ideal_h));
						} else {
							entries[i].enabled = false;
						}
					}
				}
			}
		}
	}

	const auto selection = SelectMods(entries, patterns);
	for (size_t index = 0; index < entries.size(); index++) {
		const char* reason = "applying";
		std::string detail;
		if (selection.decisions[index] == ModDecision::SkipDisabledInFile) {
			reason = "skipping";
			detail = " (disabled in the patch file)";
		} else if (selection.decisions[index] == ModDecision::SkipDisabledByEnv) {
			reason = "skipping";
			detail = fmt::format(" ({} matched \"{}\")", kDisableEnv,
			                     patterns[selection.pattern_of_mod[index]]);
		}
		Log::WriteToConsoleAndLog(fmt::format("Game patch: {} mod {} \"{}\"{}\n", reason,
		                                      index + 1, entries[index].name, detail));
	}
	for (size_t index = 0; index < patterns.size(); index++) {
		if (!selection.pattern_matched[index]) {
			Log::WriteToConsoleAndLog(fmt::format("Game patch: {} pattern \"{}\" matched no mod\n",
			                                      kDisableEnv, patterns[index]));
		}
	}

	size_t mod_index = 0;
	for (const auto& mod: root["mods"]) {
		if (selection.decisions[mod_index++] != ModDecision::Apply) {
			continue;
		}
		const auto name = mod["name"].get<std::string>();
		plan->mod_names.push_back(name);
		for (const auto& entry: mod["memory"]) {
			Write write;
			write.source_address = std::stoull(entry["offset"].get<std::string>(), nullptr, 16);
			if (!ParseBytes(entry["off"].get<std::string>(), &write.off) ||
			    !ParseBytes(entry["on"].get<std::string>(), &write.on) ||
			    write.off.size() != write.on.size()) {
				return Fail(error, "invalid mod memory bytes");
			}
			plan->writes.push_back(std::move(write));
		}
	}
	return true;
}

bool ValidateGame(const Plan& plan, const Program* main_program, std::string* error) {
	if (main_program == nullptr || main_program->base_vaddr == 0) {
		return Fail(error, "main executable is not loaded");
	}
	std::string title_id;
	std::string version;
	if (!SystemContentParamSfoGetString("TITLE_ID", &title_id) ||
	    !SystemContentParamSfoGetString("APP_VER", &version) ||
	    !Common::EqualNoCase(main_program->file_name.filename().string(), plan.process) ||
	    !Common::EqualNoCase(title_id, plan.title_id) ||
	    (plan.version != "*" && version != plan.version)) {
		return Fail(error, "cheat file does not match the loaded game");
	}
	if (!plan.executable_sha256.empty() &&
	    !PatchExecutableMatches(main_program->file_name, plan.executable_sha256)) {
		return Fail(error, "cheat file executable SHA-256 does not match the loaded game");
	}
	return true;
}

void Translate(const Program& program, int64_t offset_shift, uint64_t source_address,
               uint64_t* target_address) {
	const int64_t target_rva = static_cast<int64_t>(source_address) + offset_shift;
	if (target_rva < 0) {
		*target_address = 0;
		return;
	}
	*target_address = program.base_vaddr + static_cast<uint64_t>(target_rva);
}

bool IsInsideProgram(const Program& program, uint64_t address, size_t size) {
	return size != 0 && address >= program.base_vaddr &&
	       address + size <= program.base_vaddr + program.mapped_size;
}

bool IsZero(const std::vector<uint8_t>& bytes) {
	return std::all_of(bytes.begin(), bytes.end(), [](uint8_t byte) { return byte == 0; });
}

bool TranslateAndValidate(Plan* plan, const Program& program, int64_t offset_shift) {
	for (auto& write: plan->writes) {
		Translate(program, offset_shift, write.source_address, &write.target_address);
		if (!IsZero(write.off) &&
		    (!IsInsideProgram(program, write.target_address, write.off.size()) ||
		     std::memcmp(reinterpret_cast<const void*>(write.target_address), write.off.data(),
		                 write.off.size()) != 0)) {
			return false;
		}
	}
	return true;
}

bool Matches(Plan* plan, const Program& program, int64_t* offset_shift) {
	const auto anchor = std::find_if(plan->writes.begin(), plan->writes.end(),
	                                 [](const Write& write) { return !IsZero(write.off); });
	if (anchor == plan->writes.end() || program.mapped_size < anchor->off.size()) {
		return false;
	}

	const auto* ehdr = program.elf->GetEhdr();
	const auto* phdr = program.elf->GetPhdr();
	for (Elf64_Half index = 0; index < ehdr->e_phnum; index++) {
		const auto& segment = phdr[index];
		if ((segment.p_type != PT_LOAD && segment.p_type != PT_OS_RELRO) ||
		    segment.p_filesz < anchor->off.size()) {
			continue;
		}

		const auto* begin = reinterpret_cast<const uint8_t*>(program.base_vaddr + segment.p_vaddr);
		const auto* end   = begin + segment.p_filesz;
		for (auto* current = begin; current < end;) {
			const auto* found = std::search(current, end, anchor->off.begin(), anchor->off.end());
			if (found == end) {
				break;
			}
			const auto rva = static_cast<uint64_t>(found - begin) + segment.p_vaddr;
			const int64_t shift = static_cast<int64_t>(rva) - static_cast<int64_t>(anchor->source_address);
			if (TranslateAndValidate(plan, program, shift)) {
				*offset_shift = shift;
				return true;
			}
			current = found + 1;
		}
	}
	return false;
}

void ReleaseCaves(std::vector<uint64_t>* cave_pages) {
	for (auto page = cave_pages->rbegin(); page != cave_pages->rend(); ++page) {
		EXIT_IF(!Libs::LibKernel::Memory::FreeGuestMemory(*page, kPageSize));
	}
	cave_pages->clear();
}

bool AllocateCave(Plan* plan, const Program& program, std::string* error) {
	std::vector<uint64_t> pages;
	for (const auto& write: plan->writes) {
		if (IsInsideProgram(program, write.target_address, write.off.size())) {
			continue;
		}
		if (!IsZero(write.off)) {
			return Fail(error, "non-zero write is outside the matched module");
		}

		const auto write_end = write.target_address + write.off.size();
		for (auto page = write.target_address & ~(kPageSize - 1); page < write_end;
		     page += kPageSize) {
			if (std::find(pages.begin(), pages.end(), page) == pages.end()) {
				pages.push_back(page);
			}
		}
	}

	for (const auto page: pages) {
		const auto allocated = Libs::LibKernel::Memory::AllocateRuntimeMemory(
		    page, kPageSize, Common::VirtualMemory::Mode::ExecuteReadWrite, "game_cheat_code_cave",
		    true);
		if (allocated != page) {
			if (allocated != 0) {
				Libs::LibKernel::Memory::FreeGuestMemory(allocated, kPageSize);
			}
			ReleaseCaves(&plan->cave_pages);
			return Fail(error, "could not allocate code-cave page");
		}
		plan->cave_pages.push_back(page);
	}
	return true;
}

bool ApplyWrites(Plan* plan, const Program& program, std::string* error) {
	if (!AllocateCave(plan, program, error)) {
		return false;
	}

	for (int external = 1; external >= 0; external--) {
		for (const auto& write: plan->writes) {
			if (static_cast<int>(
			        !IsInsideProgram(program, write.target_address, write.on.size())) != external) {
				continue;
			}
			std::memcpy(reinterpret_cast<void*>(write.target_address), write.on.data(),
			            write.on.size());
			if (!Common::VirtualMemory::FlushInstructionCache(write.target_address,
			                                                  write.on.size())) {
				return Fail(error, "could not finalize cheat write");
			}
		}
	}
	return true;
}

} // namespace

bool Apply(const std::filesystem::path& plan_path, Program* main_program,
           const std::vector<Program*>& programs) {
	Plan        plan;
	std::string error;
	if (!LoadPlan(plan_path, &plan, &error) || !ValidateGame(plan, main_program, &error)) {
		::printf("Game cheat error: %s\n", error.c_str());
		return false;
	}
	if (plan.writes.empty()) {
		Log::WriteToConsoleAndLog("Game patch: no mod writes left to apply\n");
		return true;
	}

	g_pending_plan = std::make_unique<Plan>(std::move(plan));
	for (auto* program: programs) {
		if (!ApplyPending(program)) {
			return false;
		}
		if (g_pending_plan == nullptr) {
			return true;
		}
	}
	::printf("Game cheat: waiting for the matching module\n");
	return true;
}

bool ApplyPending(Program* program) {
	int64_t offset_shift = 0;
	if (g_pending_plan == nullptr || program == nullptr || program->elf == nullptr ||
	    !Matches(g_pending_plan.get(), *program, &offset_shift)) {
		return true;
	}
	Log::WriteToConsoleAndLog(fmt::format(
	    "Game cheat: matched {} with offset shift 0x{:x}\n",
	    program->file_name.filename().string(),
	    static_cast<unsigned long long>(offset_shift)));

	std::string error;
	if (!ApplyWrites(g_pending_plan.get(), *program, &error)) {
		ReleaseCaves(&g_pending_plan->cave_pages);
		g_pending_plan.reset();
		::printf("Game cheat error: %s\n", error.c_str());
		return false;
	}

	for (const auto& name: g_pending_plan->mod_names) {
		::printf("Successfully applied cheat: %s\n", name.c_str());
	}
	g_applied_cave_pages = std::move(g_pending_plan->cave_pages);
	g_pending_plan.reset();
	return true;
}

void Clear() {
	if (g_pending_plan != nullptr) {
		ReleaseCaves(&g_pending_plan->cave_pages);
	}
	g_pending_plan.reset();
	ReleaseCaves(&g_applied_cave_pages);
}

} // namespace Loader::GamePatch
