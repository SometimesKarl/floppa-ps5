#include "graphics/host_gpu/renderer/pipeline/pipelineCache.h"

#include "common/assert.h"
#include "common/emulatorConfig.h"
#include "common/file.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "common/timer.h"
#include "graphics/guest_gpu/hardwareContext.h"
#include "graphics/host_gpu/renderer/colorRenderTarget.h"
#include "graphics/host_gpu/renderer/debug.h"
#include "graphics/host_gpu/renderer/depthRenderTarget.h"
#include "graphics/host_gpu/renderer/image/imageView.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/shader/recompiler/ShaderRecompiler.h"
#include "graphics/shader/shaderCompiler.h"
#include "kernel/memory.h"
#include "kytyGitVersion.h"
#include "loader/systemContent.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cinttypes>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fmt/format.h>
#include <limits>
#include <span>
#include <spirv-tools/libspirv.hpp>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>
#include <xxhash.h>

namespace Libs::Graphics {

namespace {

// KYTY_COMPILE_LOG=1 prints one line per shader compile and pipeline creation with the time of
// each step, stamped with the QPC so the frame log's hitches can be attributed to them.
bool CompileLogEnabled() {
	static const bool enabled = [] {
		const char* value = std::getenv("KYTY_COMPILE_LOG");
		return value != nullptr && value[0] == '1';
	}();
	return enabled;
}

double QpcMs(uint64_t begin, uint64_t end) {
	static const double ms_per_tick =
	    1000.0 / static_cast<double>(Common::Timer::QueryPerformanceFrequency());
	return static_cast<double>(end - begin) * ms_per_tick;
}

vk::PolygonMode ResolvePolygonMode(const HW::ModeControl& mode, bool cull_front, bool cull_back) {
	// CxPrimitiveSetup::PolygonMode disables both per-face modes when it is zero.
	if (mode.poly_mode == 0) {
		return vk::PolygonMode::eFill;
	}
	EXIT_NOT_IMPLEMENTED(mode.poly_mode != 1);
	if (cull_front && cull_back) {
		return vk::PolygonMode::eFill;
	}
	if (!cull_front && !cull_back && mode.polymode_front_ptype != mode.polymode_back_ptype) {
		EXIT("Pipeline: different polygon modes for two visible faces are unsupported\n");
	}
	// Vulkan has one polygon mode. A culled face does not constrain that mode.
	const auto polygon_mode = cull_front ? mode.polymode_back_ptype : mode.polymode_front_ptype;
	switch (polygon_mode) {
		case 0: return vk::PolygonMode::ePoint;
		case 1: return vk::PolygonMode::eLine;
		case 2: return vk::PolygonMode::eFill;
		default: EXIT("Pipeline: invalid polygon mode %u\n", polygon_mode);
	}
}

std::string DriverCacheSignature(const vk::PhysicalDeviceProperties& properties) {
	constexpr char hex[] = "0123456789abcdef";
	std::string    uuid(VK_UUID_SIZE * 2, '0');
	for (size_t i = 0; i < VK_UUID_SIZE; i++) {
		uuid[i * 2]     = hex[properties.pipelineCacheUUID[i] >> 4u];
		uuid[i * 2 + 1] = hex[properties.pipelineCacheUUID[i] & 0xfu];
	}
	// The driver keys every cached pipeline by its complete create info, SPIR-V included, so
	// entries from another emulator build can only miss, never match wrongly. Keying the file by
	// the build revision discarded every compiled pipeline on each update, and the first use of
	// each effect stuttered again.
	return fmt::format("KytyPC2:{:08x}:{:08x}:{:08x}:{}\n", properties.vendorID,
	                   properties.deviceID, properties.driverVersion, uuid);
}

std::string PipelineCacheTitleId() {
	std::string title_id;
	if ((!Loader::SystemContentParamSfoGetString("TITLE_ID", &title_id) || title_id.empty()) &&
	    (!Loader::SystemContentParamSfoGetString("CONTENT_ID", &title_id) || title_id.empty())) {
		return {};
	}
	if (!std::ranges::all_of(title_id, [](unsigned char c) {
		    return std::isalnum(c) != 0 || c == '-' || c == '_';
	    })) {
		return {};
	}
	return title_id;
}

template <typename... Args>
void PipelineCacheLog(fmt::format_string<Args...> format, Args&&... args) {
	auto message = fmt::format(format, std::forward<Args>(args)...);
	message += '\n';
	Log::WriteToConsoleAndLog(message);
}

bool ReadShaderGuestMemory(void*, uint64_t address, std::span<uint32_t> values) {
	return !values.empty() &&
	       Libs::LibKernel::Memory::TryReadGpuCleanBacking(address, values.data(), values.size_bytes());
}

void DumpShaderSpirv(const char* stage_name, uint64_t shader_hash,
                     const std::vector<uint32_t>& spirv) {
	if (!Config::GraphicsDebugDumpEnabled()) {
		return;
	}
	static std::atomic_int id = 0;
	const auto path = Config::GetShaderLogFolder() / fmt::format("{:04d}_new_shader_{}_{:016x}.spv",
	                                                             id++, stage_name, shader_hash);
	Common::File::CreateDirectories(path.parent_path());
	Common::File file(path);
	if (file.IsInvalid()) {
		const auto path_text = Common::PathToString(path);
		LOGF_COLOR(Log::Color::BrightRed, "Can't create file: %s\n", path_text.c_str());
		return;
	}
	file.Write(spirv.data(), spirv.size() * sizeof(uint32_t));
}

void DumpShaderOriginal(const char* stage_name, uint64_t shader_hash,
                        std::span<const uint32_t> code, const std::string& decoded_dump) {
	if (!Config::GraphicsDebugDumpEnabled()) {
		return;
	}
	EXIT_IF(code.empty());
	static std::atomic_int id = 0;
	const auto base = Config::GetShaderLogFolder() / "original" /
	                  fmt::format("{:04d}_new_shader_{}_{:016x}", id++, stage_name, shader_hash);
	Common::File::CreateDirectories(base.parent_path());
	for (const auto& [suffix, data, size]: {
	         std::tuple {".bin", static_cast<const void*>(code.data()), code.size_bytes()},
	         std::tuple {".rdna2", static_cast<const void*>(decoded_dump.data()),
	                     decoded_dump.size()},
	     }) {
		if (size == 0) {
			continue;
		}
		auto path = base;
		path += suffix;
		Common::File file(path);
		if (file.IsInvalid()) {
			const auto path_text = Common::PathToString(path);
			LOGF_COLOR(Log::Color::BrightRed, "Can't create file: %s\n", path_text.c_str());
		} else {
			file.Write(data, size);
		}
	}
}

bool ValidateShaderSpirv(const char* label, uint64_t shader_hash,
                         const std::vector<uint32_t>& spirv) {
	if (!Config::ShaderValidationEnabled()) {
		return true;
	}
	spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_3);
	std::string          messages;
	tools.SetMessageConsumer([&messages](spv_message_level_t, const char*,
	                                     const spv_position_t& position, const char* message) {
		messages += fmt::format("{}: {} ({}) {}\n", static_cast<int>(position.line),
		                        static_cast<int>(position.column), static_cast<int>(position.index),
		                        message);
	});
	if (tools.Validate(spirv)) {
		return true;
	}
	spvtools::SpirvTools disassembler(SPV_ENV_VULKAN_1_2);
	std::string          text;
	disassembler.Disassemble(spirv, &text,
	                         static_cast<uint32_t>(SPV_BINARY_TO_TEXT_OPTION_NO_HEADER) |
	                             static_cast<uint32_t>(SPV_BINARY_TO_TEXT_OPTION_FRIENDLY_NAMES) |
	                             static_cast<uint32_t>(SPV_BINARY_TO_TEXT_OPTION_COMMENT) |
	                             static_cast<uint32_t>(SPV_BINARY_TO_TEXT_OPTION_INDENT) |
	                             static_cast<uint32_t>(SPV_BINARY_TO_TEXT_OPTION_COLOR));
	LOGF_COLOR(Log::Color::BrightRed, "%s SPIR-V validation failed hash=0x%016" PRIx64 ":\n%s",
	           label, shader_hash, messages.c_str());
	LOGF("%s\n", text.c_str());
	return false;
}

} // namespace

struct PipelineCache::ProgramCache {
	struct ProgramKey {
		ShaderType            stage           = ShaderType::Unknown;
		uint64_t              hash            = 0;
		uint32_t              user_data_count = 0;
		uint32_t              code_size       = 0;
		std::vector<uint32_t> static_state;

		bool operator==(const ProgramKey&) const = default;
	};

	struct Permutation {
		ShaderRecompiler::IR::ResourceSpecialization specialization;
		ShaderRecompiler::IR::CompiledShaderInfo     program;
		ShaderProgram                                handle;
	};

	// One materialization and every input it read: user-data registers, guest words (ordinary)
	// and strict reads with their outcome. The walk is a function of those, the shader base and
	// the user-data count (later addresses follow from earlier values), so when all of them
	// still hold the stored resources are what the walk would produce. Per-draw user data the
	// walk never reads (constants) does not prevent reuse.
	enum class ReadKind : uint8_t { Word, Strict, UserData };
	struct MaterializeRead {
		uint64_t address     = 0; // guest address, or user-data index for ReadKind::UserData
		uint32_t count       = 0;
		uint32_t first_value = 0;
		ReadKind kind        = ReadKind::Word;
		bool     ok          = false;
	};
	struct MaterializeMemo {
		size_t                                       user_data_count = 0;
		uint64_t                                     shader_base = 0;
		std::vector<MaterializeRead>                 reads;
		std::vector<uint32_t>                        values;
		ShaderRecompiler::IR::ResourceSnapshot       resources;
		ShaderRecompiler::IR::ResourceSpecialization specialization;
		uint64_t                                     last_use = 0;
		bool                                         valid    = false;
	};
	static constexpr size_t MemoSlots = 4;

	struct SourceEntry {
		explicit SourceEntry(ShaderRecompiler::IR::ResourcePlan plan)
		    : resource_plan(std::move(plan)) {
			permutations.reserve(8);
		}

		ShaderRecompiler::IR::ResourcePlan           resource_plan;
		ShaderRecompiler::IR::ResourceSnapshot       resources;
		ShaderRecompiler::IR::ResourceSpecialization specialization;
		std::vector<Permutation>                    permutations;
		bool                                        skip_dispatch = false;
		std::array<MaterializeMemo, MemoSlots>      memo;
		int                                         current_memo = -1;
	};

	struct MemoCapture {
		std::vector<MaterializeRead>* reads  = nullptr;
		std::vector<uint32_t>*        values = nullptr;
	};

	static bool CapturingStrictRead(void* userdata, uint64_t address, std::span<uint32_t> values) {
		auto&      capture = *static_cast<MemoCapture*>(userdata);
		const bool ok      = ReadShaderGuestMemory(nullptr, address, values);
		capture.reads->push_back({address, static_cast<uint32_t>(values.size()),
		                          static_cast<uint32_t>(capture.values->size()), ReadKind::Strict,
		                          ok});
		if (ok) {
			capture.values->insert(capture.values->end(), values.begin(), values.end());
		}
		return ok;
	}

	static void ObserveDirectRead(void* context, uint64_t address, uint32_t value) {
		auto& capture = *static_cast<MemoCapture*>(context);
		capture.reads->push_back(
		    {address, 1, static_cast<uint32_t>(capture.values->size()), ReadKind::Word, true});
		capture.values->push_back(value);
	}

	static void ObserveUserData(void* context, uint32_t index, uint32_t value) {
		auto& capture = *static_cast<MemoCapture*>(context);
		capture.reads->push_back(
		    {index, 1, static_cast<uint32_t>(capture.values->size()), ReadKind::UserData, true});
		capture.values->push_back(value);
	}

	static bool MemoStillValid(const MaterializeMemo& memo, std::span<const uint32_t> user_data) {
		thread_local std::vector<uint32_t> scratch;
		for (const auto& read: memo.reads) {
			if (read.kind == ReadKind::UserData) {
				if (read.address >= user_data.size() ||
				    user_data[read.address] != memo.values[read.first_value]) {
					return false;
				}
			} else if (read.kind == ReadKind::Strict) {
				scratch.resize(read.count);
				const bool ok = ReadShaderGuestMemory(nullptr, read.address, scratch);
				if (ok != read.ok || (ok && !std::equal(scratch.begin(), scratch.end(),
				                                        memo.values.begin() + read.first_value))) {
					return false;
				}
			} else {
				uint32_t word = 0;
				if (!Libs::LibKernel::Memory::TryReadGuestWithoutFault(read.address, &word,
				                                                       sizeof(word))) {
					std::memcpy(&word, reinterpret_cast<const void*>(read.address), sizeof(word));
				}
				if (word != memo.values[read.first_value]) {
					return false;
				}
			}
		}
		return true;
	}

	// MaterializeResources for an existing entry, reusing a recent result whose inputs still hold.
	void MaterializeCached(SourceEntry& entry, std::span<const uint32_t> user_data,
	                       uint64_t shader_base) {
		static const bool stats   = std::getenv("KYTY_SRT_MEMO_STATS") != nullptr;
		static uint64_t   hits    = 0;
		static uint64_t   misses  = 0;
		static auto       printed = std::chrono::steady_clock::now();
		++memo_clock;
		for (size_t i = 0; i < entry.memo.size(); i++) {
			auto& memo = entry.memo[i];
			if (!memo.valid || memo.shader_base != shader_base ||
			    memo.user_data_count != user_data.size() || !MemoStillValid(memo, user_data)) {
				continue;
			}
			if (entry.current_memo != static_cast<int>(i)) {
				entry.resources      = memo.resources;
				entry.specialization = memo.specialization;
				entry.current_memo   = static_cast<int>(i);
			}
			// Bindings read the draw's own user data (shader constants) from the snapshot.
			entry.resources.user_data.assign(user_data.begin(), user_data.end());
			memo.last_use = memo_clock;
			++hits;
			return;
		}
		++misses;
		auto& slot = *std::ranges::min_element(entry.memo, {}, [](const MaterializeMemo& memo) {
			return memo.valid ? memo.last_use : 0;
		});
		slot.valid = false;
		slot.reads.clear();
		slot.values.clear();
		MemoCapture capture {&slot.reads, &slot.values};
		const ShaderRecompiler::IR::SrtRuntime runtime {
		    .user_data                  = user_data,
		    .shader_base                = shader_base,
		    .userdata                   = &capture,
		    .read_specialization_memory = CapturingStrictRead,
		};
		ShaderRecompiler::IR::SetSrtReadObserver(ObserveDirectRead, ObserveUserData, &capture);
		const bool materialized = ShaderRecompiler::IR::MaterializeResources(
		    entry.resource_plan, runtime, entry.resources, entry.specialization);
		ShaderRecompiler::IR::SetSrtReadObserver(nullptr, nullptr, nullptr);
		EXIT_IF(!materialized);
		slot.user_data_count = user_data.size();
		slot.shader_base    = shader_base;
		slot.resources      = entry.resources;
		slot.specialization = entry.specialization;
		slot.last_use       = memo_clock;
		slot.valid          = true;
		entry.current_memo  = static_cast<int>(&slot - entry.memo.data());
		if (stats && std::chrono::steady_clock::now() - printed > std::chrono::seconds(30)) {
			printed = std::chrono::steady_clock::now();
			std::printf("SRT memo: %" PRIu64 " hits, %" PRIu64 " misses\n", hits, misses);
		}
	}

	struct ProgramKeyHash {
		std::size_t operator()(const ProgramKey& key) const {
			std::size_t hash = static_cast<std::size_t>(key.stage);
			PipelineKeyHash::Mix(hash, static_cast<std::size_t>(key.hash));
			if constexpr (sizeof(std::size_t) < sizeof(uint64_t)) {
				PipelineKeyHash::Mix(hash, static_cast<std::size_t>(key.hash >> 32u));
			}
			PipelineKeyHash::Mix(hash, key.user_data_count);
			PipelineKeyHash::Mix(hash, key.code_size);
			PipelineKeyHash::Mix(hash, key.static_state.size());
			// Bucket same-shape static variants by source. ProgramKey equality performs the one
			// exact state comparison needed on a stable hit without hashing up to 429 words first.
			return hash;
		}
	};

	static constexpr std::size_t MaxStaticKeyWords = 13 + ShaderVertexInputInfo::RES_MAX * 13;

	Permutation CompilePermutation(const ShaderParams&                          params,
	                               const ShaderRecompiler::CompileOptions&      options,
	                               ShaderRecompiler::TranslateResult            translated,
	                               ShaderRecompiler::IR::ResourceSpecialization specialization,
	                               uint32_t push_data_start_dword) {
		const char* stage_name = nullptr;
		switch (options.stage) {
			case ShaderType::Vertex: stage_name = "vs"; break;
			case ShaderType::Mesh: stage_name = "ms"; break;
			case ShaderType::Local: stage_name = "ls"; break;
			case ShaderType::TessellationControl: stage_name = "hs"; break;
			case ShaderType::TessellationEvaluation: stage_name = "ds"; break;
			case ShaderType::Pixel: stage_name = "ps"; break;
			case ShaderType::Compute: stage_name = "cs"; break;
			default: EXIT("invalid pipeline shader stage\n");
		}
		const auto compile_begin = Common::Timer::QueryPerformanceCounter();
		auto result = ShaderRecompiler::CompileProgram(std::move(translated), options,
		                                               specialization, push_data_start_dword);
		const auto compile_end = Common::Timer::QueryPerformanceCounter();
		DumpShaderOriginal(stage_name, options.shader_hash, params.code, result.decoded_dump);
		if (Config::ShaderValidationEnabled() && background) {
			// Validation is a diagnostic (~14 ms per shader, up to 0.3 s): run it on a compile
			// worker so a new shader does not stall the frame; a failure still ends the run.
			background([label = options.dump_label, hash = options.shader_hash, stage_name,
			            spirv = result.spirv] {
				if (!ValidateShaderSpirv(label, hash, spirv)) {
					DumpShaderSpirv(stage_name, hash, spirv);
					EXIT("%s failed hash=0x%016" PRIx64 ": SPIR-V validation failed\n", label, hash);
				}
			});
		} else if (!ValidateShaderSpirv(options.dump_label, options.shader_hash, result.spirv)) {
			DumpShaderSpirv(stage_name, options.shader_hash, result.spirv);
			EXIT("%s failed hash=0x%016" PRIx64 ": SPIR-V validation failed\n", options.dump_label,
			     options.shader_hash);
		}
		DumpShaderSpirv(stage_name, options.shader_hash, result.spirv);
		const auto validate_end = Common::Timer::QueryPerformanceCounter();

		const auto module = CompileSPV(result.spirv, device);
		EXIT_IF(module == nullptr);
		if (CompileLogEnabled()) {
			const auto module_end = Common::Timer::QueryPerformanceCounter();
			std::printf("compile qpc=%" PRIu64 " stage=%s hash=%016" PRIx64
			            " translate_ms=%.2f spirv_ms=%.2f validate_ms=%.2f module_ms=%.2f words=%zu\n",
			            module_end, stage_name, options.shader_hash, last_translate_ms,
			            QpcMs(compile_begin, compile_end), QpcMs(compile_end, validate_end),
			            QpcMs(validate_end, module_end), result.spirv.size());
		}
		if (options.dump_ir) {
			LOGF("%s SPIR-V words=%" PRIu64 " wave_size=%u\n", options.dump_label,
			     static_cast<uint64_t>(result.spirv.size()), options.wave_size);
		}
		return {
		    .specialization = std::move(specialization),
		    .program        = std::move(result.program).TakeCompiledInfo(),
		    .handle         = {.id = ++next_shader_id, .module = module},
		};
	}

	template <typename InputInfo>
	ShaderProgram Get(const ShaderParams& params, InputInfo& input_info,
	                  uint32_t& push_data_cursor) {
		ShaderType stage;
		if constexpr (std::is_same_v<InputInfo, ShaderVertexInputInfo>) {
			stage = input_info.logical_stage;
		} else if constexpr (std::is_same_v<InputInfo, ShaderPixelInputInfo>) {
			stage = ShaderType::Pixel;
		} else {
			static_assert(std::is_same_v<InputInfo, ShaderComputeInputInfo>);
			stage = ShaderType::Compute;
		}

		const auto user_data = std::span(params.user_data).first(params.user_data_count);
		lookup_key.stage           = stage;
		lookup_key.hash            = params.hash;
		lookup_key.user_data_count = params.user_data_count;
		lookup_key.code_size       = static_cast<uint32_t>(params.code.size());
		{
			KYTY_PROFILER_BLOCK("ProgramCache: build static key");
			BuildStageStaticKey(input_info, lookup_key.static_state);
		}
		auto entry = [&] {
			KYTY_PROFILER_BLOCK("ProgramCache: find");
			return programs.find(lookup_key);
		}();
		if (entry != programs.end() && entry->second.skip_dispatch) {
			return {};
		}
		const ShaderRecompiler::IR::SrtRuntime       runtime {
		    .user_data                  = user_data,
		    .shader_base                = params.Base(),
		    .read_specialization_memory = ReadShaderGuestMemory,
		};
		if (entry != programs.end()) {
			MaterializeCached(entry->second, user_data, params.Base());
			if (const auto permutation = std::ranges::find_if(
			        entry->second.permutations, [&](const Permutation& candidate) {
				        const auto& layout = candidate.program.bindings;
				        return layout.push_data_start_dword ==
				                   ShaderRecompiler::IR::PushData::StartFor(
				                       push_data_cursor, layout.ShaderDataDwords()) &&
				               candidate.specialization == entry->second.specialization;
			        });
			    permutation != entry->second.permutations.end()) {
				input_info.stage = {.program   = &permutation->program,
				                    .resources = &entry->second.resources};
				permutation->program.bindings.AdvancePushData(push_data_cursor);
				return permutation->handle;
			}
		}

		ShaderStageInputInfo stage_input {};
		if constexpr (std::is_same_v<InputInfo, ShaderVertexInputInfo>) {
			stage_input.vertex = &input_info;
		} else if constexpr (std::is_same_v<InputInfo, ShaderPixelInputInfo>) {
			stage_input.pixel = &input_info;
		} else {
			stage_input.compute = &input_info;
		}
		const char* label = nullptr;
		switch (stage) {
			case ShaderType::Vertex: label = "ShaderRecompiler VS"; break;
			case ShaderType::Mesh: label = "ShaderRecompiler MS"; break;
			case ShaderType::Local: label = "ShaderRecompiler LS"; break;
			case ShaderType::TessellationControl: label = "ShaderRecompiler HS"; break;
			case ShaderType::TessellationEvaluation: label = "ShaderRecompiler DS"; break;
			case ShaderType::Pixel: label = "ShaderRecompiler PS"; break;
			case ShaderType::Compute: label = "ShaderRecompiler CS"; break;
			default: EXIT("invalid pipeline shader stage\n");
		}
		ShaderRecompiler::CompileOptions options;
		options.stage       = stage;
		options.shader_hash = params.hash;
		options.user_data   = user_data;
		options.back_code      = params.back_code;
		options.dump_ir     = Config::GetShaderLogDirection() != Config::LogDirection::Silent;
		options.early_dump  = options.dump_ir;
		options.dump_label  = label;
		options.input_info  = stage_input;

		if constexpr (std::is_same_v<InputInfo, ShaderVertexInputInfo>) {
			options.user_data_base = 8;
			options.wave_size = input_info.wave_size;
			if (stage == ShaderType::Mesh || stage == ShaderType::TessellationControl) {
				options.user_data_base = 0;
				options.wave_size = stage == ShaderType::Mesh ? input_info.mesh.wave_size : 64u;
			}
		} else {
			options.wave_size = input_info.wave_size;
		}
		const auto translate_begin = Common::Timer::QueryPerformanceCounter();
		auto translated = ShaderRecompiler::TranslateProgram(params.code, options);
		last_translate_ms = QpcMs(translate_begin, Common::Timer::QueryPerformanceCounter());
		if (translated.skip_dispatch) {
			entry = programs.try_emplace(lookup_key, ShaderRecompiler::IR::ResourcePlan {}).first;
			entry->second.skip_dispatch = true;
			return {};
		}
		if (entry == programs.end()) {
			entry = programs.try_emplace(lookup_key,
			    ShaderRecompiler::IR::ExtractResourcePlan(translated.program)).first;
			EXIT_IF(!ShaderRecompiler::IR::MaterializeResources(
			    entry->second.resource_plan, runtime, entry->second.resources,
			    entry->second.specialization));
		}
		entry->second.permutations.push_back(CompilePermutation(
		    params, options, std::move(translated), entry->second.specialization, push_data_cursor));
		const auto& permutation = entry->second.permutations.back();
		input_info.stage = {.program = &permutation.program, .resources = &entry->second.resources};
		permutation.program.bindings.AdvancePushData(push_data_cursor);

		std::array<size_t, static_cast<size_t>(ShaderType::TessellationEvaluation) + 1> counts {};
		for (const auto& [key, source]: programs) {
			counts[static_cast<size_t>(key.stage)] += source.permutations.size();
		}
		// Guest geometry shaders are compiled through the host mesh stage.
		std::printf("Shaders: VS %zu | PS %zu | CS %zu | GS %zu | LS %zu | HS %zu | TES %zu\n",
		            counts[static_cast<size_t>(ShaderType::Vertex)],
		            counts[static_cast<size_t>(ShaderType::Pixel)],
		            counts[static_cast<size_t>(ShaderType::Compute)],
		            counts[static_cast<size_t>(ShaderType::Mesh)],
		            counts[static_cast<size_t>(ShaderType::Local)],
		            counts[static_cast<size_t>(ShaderType::TessellationControl)],
		            counts[static_cast<size_t>(ShaderType::TessellationEvaluation)]);
		return permutation.handle;
	}

	explicit ProgramCache(vk::Device device): device(device) {
		lookup_key.static_state.reserve(MaxStaticKeyWords);
	}
	~ProgramCache() {
		for (const auto& [key, entry]: programs) {
			(void)key;
			for (const auto& permutation: entry.permutations) {
				device.destroyShaderModule(permutation.handle.module, nullptr);
			}
		}
	}

	std::unordered_map<ProgramKey, SourceEntry, ProgramKeyHash> programs;
	ProgramKey                                                  lookup_key;
	uint64_t                                                    memo_clock = 0;
	vk::Device                                                  device;
	uint64_t                                                    next_shader_id = 0;
	double                                                      last_translate_ms = 0;
	std::function<void(std::function<void()>)>                  background;
};

// Background workers for driver pipeline compiles (0.1-4 s each for this title's shaders on AMD),
// SPIR-V validation and driver-cache writes. Below normal priority: the game's threads and the
// GPU command thread come first.
struct PipelineCache::AsyncCompiler {
	struct Job {
		GraphicsPipelineBuildPtr build;
		std::function<void()>    task;
	};

	AsyncCompiler(GraphicContext& graphics, vk::PipelineCache driver_cache, uint32_t count)
	    : graphics(graphics), driver_cache(driver_cache) {
		for (uint32_t i = 0; i < count; i++) {
			workers.emplace_back([this, i] { Run(i); });
		}
	}
	~AsyncCompiler() { Close(); }
	KYTY_CLASS_NO_COPY(AsyncCompiler);

	// Takes the job only when it returns true; a closed compiler leaves it with the caller.
	bool Submit(Job& job) {
		{
			std::scoped_lock lock(mutex);
			if (closed) {
				return false;
			}
			queue.push_back(std::move(job));
		}
		cv.notify_one();
		return true;
	}

	// Waits for the running jobs; queued pipeline builds are discarded (their draws stay
	// skipped), queued tasks are dropped. Idempotent.
	void Close() {
		{
			std::scoped_lock lock(mutex);
			if (closed) {
				return;
			}
			closed = true;
		}
		cv.notify_all();
		for (auto& worker: workers) {
			worker.join();
		}
		workers.clear();
		for (auto& job: queue) {
			if (job.build != nullptr) {
				DiscardGraphicsPipelineBuild(graphics, *job.build);
			}
		}
		queue.clear();
	}

	void Run(uint32_t index) {
		const auto name = fmt::format("Thread_PipelineCompile{}", index);
		KYTY_PROFILER_THREAD(name.c_str());
		Common::Thread::LowerCurrentPriority();
		for (;;) {
			Job job;
			{
				std::unique_lock lock(mutex);
				cv.wait(lock, [this] { return closed || !queue.empty(); });
				if (closed) {
					return;
				}
				job = std::move(queue.front());
				queue.pop_front();
			}
			if (job.build != nullptr) {
				const auto begin = Common::Timer::QueryPerformanceCounter();
				FinishGraphicsPipeline(graphics, *job.build, driver_cache, false);
				if (CompileLogEnabled()) {
					const auto end = Common::Timer::QueryPerformanceCounter();
					std::printf("pipeline qpc=%" PRIu64 " kind=gfx-async worker=%u ms=%.2f\n", end,
					            index, QpcMs(begin, end));
				}
			}
			if (job.task) {
				job.task();
			}
		}
	}

	GraphicContext&          graphics;
	vk::PipelineCache        driver_cache;
	std::mutex               mutex;
	std::condition_variable  cv;
	std::deque<Job>          queue;
	std::vector<std::thread> workers;
	bool                     closed = false;
};

namespace {

// Opt-in: a draw skipped while its pipeline compiles is never redrawn, and ASTRO BOT renders
// some textures once (terrain materials): skipping left the desert sand flat yellow and rocks
// black for the rest of the session. KYTY_ASYNC_PIPELINES=1 trades that risk for no stalls.
bool AsyncPipelinesEnabled() {
	const char* value = std::getenv("KYTY_ASYNC_PIPELINES");
	return value != nullptr && value[0] == '1';
}

uint32_t CompileWorkerCount() {
	if (const char* value = std::getenv("KYTY_PIPELINE_WORKERS"); value != nullptr) {
		return std::clamp<uint32_t>(static_cast<uint32_t>(std::strtoul(value, nullptr, 10)), 1, 8);
	}
	return std::clamp<uint32_t>(std::thread::hardware_concurrency() / 4, 1, 3);
}

} // namespace

PipelineCache::PipelineCache(GraphicContext& graphics)
    : m_graphics(graphics), m_program_cache(std::make_unique<ProgramCache>(graphics.device)) {
	EXIT_NOT_IMPLEMENTED(!Common::Thread::IsMainThread());
	ShaderRecompiler::IR::SetSrtDirectReader(&Libs::LibKernel::Memory::TryReadGuestWithoutFault);
	InitializeDriverCache();
	m_async_pipelines = AsyncPipelinesEnabled();
	m_async = std::make_unique<AsyncCompiler>(m_graphics, m_driver_cache, CompileWorkerCount());
	m_program_cache->background = [this](std::function<void()> task) {
		RunInBackground(std::move(task));
	};
	PipelineCacheLog("Pipeline compiles: {} ({} workers, driver-cache probe {})",
	                 m_async_pipelines ? "asynchronous" : "synchronous", CompileWorkerCount(),
	                 m_graphics.pipeline_cache_control_enabled ? "on" : "off");
}

void PipelineCache::RunInBackground(std::function<void()> task) {
	AsyncCompiler::Job job {.build = nullptr, .task = std::move(task)};
	if (m_async == nullptr || !m_async->Submit(job)) {
		job.task();
	}
}

void PipelineCache::NotePipelineCreated() {
	m_created_since_save++;
	const auto now = Common::Timer::QueryPerformanceCounter();
	if (m_last_save_qpc == 0) {
		m_last_save_qpc = now;
	}
	const auto interval = Common::Timer::QueryPerformanceFrequency() * 60;
	if (m_driver_cache == nullptr || now - m_last_save_qpc < interval ||
	    m_save_pending.exchange(true)) {
		return;
	}
	m_created_since_save = 0;
	m_last_save_qpc      = now;
	RunInBackground([this] {
		WriteDriverCache();
		m_save_pending.store(false);
	});
}

PipelineCache::~PipelineCache() {
	Save();
	auto destroy = [this](const auto& pipelines) {
		for (const auto& [key, pipeline]: pipelines) {
			(void)key;
			m_graphics.device.destroyPipeline(pipeline->pipeline, nullptr);
			m_graphics.device.destroyPipelineLayout(pipeline->pipeline_layout, nullptr);
			m_graphics.device.destroyDescriptorSetLayout(pipeline->descriptor_set_layout, nullptr);
		}
	};
	destroy(m_graphics_pipelines);
	destroy(m_compute_pipelines);
	if (m_driver_cache != nullptr) {
		m_graphics.device.destroyPipelineCache(m_driver_cache, nullptr);
	}
}

void PipelineCache::InitializeDriverCache() {
	const auto title_id = PipelineCacheTitleId();
	if (title_id.empty()) {
		return;
	}
	if (KYTY_BUILD != KYTY_BUILD_RELEASE) {
		PipelineCacheLog("Vulkan pipeline cache: disabled (non-Release build)");
		return;
	}
	const std::string_view git_hash     = KYTY_GIT_HASH;
	const std::string_view git_revision = KYTY_GIT_REVISION;
	if (git_hash == "unknown" || git_revision == "unknown") {
		PipelineCacheLog("Vulkan pipeline cache: disabled (unknown git revision)");
		return;
	}
	if (git_hash.ends_with("-dirty")) {
		PipelineCacheLog("Vulkan pipeline cache: disabled (dirty build)");
		return;
	}

	m_driver_cache_path     = std::filesystem::path("_PipelineCache") / (title_id + ".bin");
	const auto path         = Common::PathToString(m_driver_cache_path);
	const bool cache_exists = Common::File::IsFileExisting(m_driver_cache_path);
	if (cache_exists) {
		PipelineCacheLog("Vulkan pipeline cache: loading {}", path);
	} else {
		PipelineCacheLog("Vulkan pipeline cache: initializing {}", path);
	}
	std::vector<uint8_t> initial_data;
	if (cache_exists) {
		Common::File file(m_driver_cache_path, Common::File::Mode::Read);
		const auto   file_size = file.IsInvalid() ? 0 : file.Size();
		const auto   signature = DriverCacheSignature(m_graphics.GetPhysicalDeviceProperties());
		// Pipelines of older builds stay in the file; start over once it grows past this bound.
		constexpr uint64_t MaxCacheFileSize = 512ull * 1024 * 1024;
		if (file_size >= signature.size() + sizeof(uint64_t) && file_size <= MaxCacheFileSize) {
			std::string cached_signature(signature.size(), '\0');
			uint64_t    payload_hash = 0;
			initial_data.resize(file_size - signature.size() - sizeof(payload_hash));
			uint32_t signature_read = 0;
			uint32_t hash_read      = 0;
			uint32_t payload_read   = 0;
			file.Read(cached_signature.data(), static_cast<uint32_t>(cached_signature.size()),
			          &signature_read);
			file.Read(&payload_hash, sizeof(payload_hash), &hash_read);
			file.Read(initial_data.data(), static_cast<uint32_t>(initial_data.size()),
			          &payload_read);
			file.Close();
			if (signature_read != cached_signature.size() || hash_read != sizeof(payload_hash) ||
			    payload_read != initial_data.size() || cached_signature != signature ||
			    XXH3_64bits(initial_data.data(), initial_data.size()) != payload_hash) {
				initial_data.clear();
				PipelineCacheLog(
				    "Vulkan pipeline cache: invalidating {} (driver, emulator, or data mismatch)",
				    path);
			}
		} else {
			file.Close();
			PipelineCacheLog("Vulkan pipeline cache: invalidating {} (invalid file size)", path);
		}
	}

	vk::PipelineCacheCreateInfo create {};
	create.initialDataSize = initial_data.size();
	create.pInitialData    = initial_data.empty() ? nullptr : initial_data.data();
	auto result = m_graphics.device.createPipelineCache(&create, nullptr, &m_driver_cache);
	if (result != vk::Result::eSuccess && !initial_data.empty()) {
		PipelineCacheLog("Vulkan pipeline cache: driver rejected {} ({}); starting empty", path,
		                 vk::to_string(result));
		initial_data.clear();
		create.initialDataSize = 0;
		create.pInitialData    = nullptr;
		result = m_graphics.device.createPipelineCache(&create, nullptr, &m_driver_cache);
	}
	if (result != vk::Result::eSuccess) {
		PipelineCacheLog("Vulkan pipeline cache: disabled ({})", vk::to_string(result));
		m_driver_cache = nullptr;
		return;
	}
	if (!initial_data.empty()) {
		PipelineCacheLog("Vulkan pipeline cache: loaded {} bytes from {}", initial_data.size(),
		                 path);
	} else {
		PipelineCacheLog("Vulkan pipeline cache: initialized empty");
	}
}

void PipelineCache::Save() {
	if (m_async != nullptr) {
		m_async->Close();
	}
	Common::LockGuard lock(m_mutex);
	if (m_driver_cache == nullptr) {
		return;
	}
	WriteDriverCache();
	m_graphics.device.destroyPipelineCache(m_driver_cache, nullptr);
	m_driver_cache = nullptr;
}

void PipelineCache::WriteDriverCache() {
	Common::LockGuard save_lock(m_save_mutex);
	if (m_driver_cache == nullptr) {
		return;
	}

	size_t               size = 0;
	vk::Result           result;
	std::vector<uint8_t> payload;
	for (uint32_t attempt = 0; attempt < 3; attempt++) {
		size   = 0;
		result = m_graphics.device.getPipelineCacheData(m_driver_cache, &size, nullptr);
		if (result != vk::Result::eSuccess || size == 0 ||
		    size > std::numeric_limits<uint32_t>::max()) {
			break;
		}
		payload.resize(size);
		result = m_graphics.device.getPipelineCacheData(m_driver_cache, &size, payload.data());
		if (result != vk::Result::eIncomplete) {
			break;
		}
	}
	if (result != vk::Result::eSuccess || size == 0 ||
	    size > std::numeric_limits<uint32_t>::max()) {
		PipelineCacheLog("Vulkan pipeline cache: save failed ({}, {} bytes)",
		                 vk::to_string(result), size);
		return;
	}
	payload.resize(size);
	auto       prefix       = DriverCacheSignature(m_graphics.GetPhysicalDeviceProperties());
	const auto payload_hash = XXH3_64bits(payload.data(), payload.size());
	prefix.append(reinterpret_cast<const char*>(&payload_hash), sizeof(payload_hash));
	if (!Common::File::CreateDirectories(m_driver_cache_path.parent_path())) {
		PipelineCacheLog("Vulkan pipeline cache: failed to create cache directory");
		return;
	}
	auto temp_path = m_driver_cache_path;
	temp_path += ".tmp";
	Common::File file;
	uint32_t     prefix_written  = 0;
	uint32_t     payload_written = 0;
	if (file.Create(temp_path)) {
		file.Write(prefix.data(), static_cast<uint32_t>(prefix.size()), &prefix_written);
		file.Write(payload.data(), static_cast<uint32_t>(payload.size()), &payload_written);
	}
	const bool flushed = !file.IsInvalid() && file.Flush();
	file.Close();
	if (prefix_written != prefix.size() || payload_written != payload.size() || !flushed ||
	    !Common::File::RenameFile(temp_path, m_driver_cache_path)) {
		PipelineCacheLog("Vulkan pipeline cache: failed to write {}",
		                 Common::PathToString(m_driver_cache_path));
		return;
	}
	PipelineCacheLog("Vulkan pipeline cache: saved {} bytes to {}", payload.size(),
	                 Common::PathToString(m_driver_cache_path));
}

PipelineCache::GraphicsPrograms PipelineCache::GetGraphicsPrograms(
    const HW::VertexShaderInfo& vertex_regs, const HW::PixelShaderInfo& pixel_regs,
    const HW::ShaderRegisters& sh, const HW::Context& context, const HW::UserConfig& user_config,
    std::span<const Prospero::ColorComponentMapping, 8> target_export_mapping, bool pixel_active,
    std::array<ShaderVertexInputInfo, 3>& vertex_info, ShaderPixelInputInfo& pixel_info) {
	KYTY_PROFILER_FUNCTION();
	const bool tess_active = user_config.GetPrimType() == Prospero::PrimitiveType::kPatch;
	std::array<ShaderParams, 3> vertex_params;
	if (tess_active) {
		vertex_params = PrepareTessellationPrograms(vertex_regs, context, vertex_info);
	} else {
		vertex_params[0] = PrepareProgram(vertex_regs, context, user_config, vertex_info[0]);
	}
	const bool mesh_active = vertex_info[0].logical_stage == ShaderType::Mesh;
	if (mesh_active) {
		EXIT_NOT_IMPLEMENTED(!m_graphics.mesh_shader_enabled);
		auto& mesh              = vertex_info[0].mesh;
		mesh.host_subgroup_size = m_graphics.subgroup_size;
		const auto& limits      = m_graphics.mesh_shader_properties;
		const auto  logical_threads =
		    mesh.threads_num[0] * mesh.threads_num[1] * mesh.threads_num[2];
		const auto host_threads = ((logical_threads + mesh.wave_size - 1u) / mesh.wave_size) *
		                          std::min(mesh.host_subgroup_size, mesh.wave_size);
		if (host_threads > limits.maxMeshWorkGroupInvocations ||
		    host_threads > limits.maxMeshWorkGroupSize[0] ||
		    mesh.max_vertices > limits.maxMeshOutputVertices ||
		    mesh.max_primitives > limits.maxMeshOutputPrimitives ||
		    mesh.lds_size_dwords * sizeof(uint32_t) > limits.maxMeshSharedMemorySize) {
			EXIT("mesh shader exceeds host limits: threads=%u vertices=%u primitives=%u LDS=%u\n",
			     host_threads, mesh.max_vertices, mesh.max_primitives, mesh.lds_size_dwords);
		}
	}
	ShaderParams pixel_params;
	if (pixel_active) {
		pixel_params = PrepareProgram(pixel_regs, sh, target_export_mapping, pixel_info);
		const auto& blend          = context.GetBlendControl(0);
		const auto  is_dual_source = [](uint8_t factor) {
			return factor >= static_cast<uint8_t>(Prospero::BlendFactor::kSrc1Color) &&
			       factor <= static_cast<uint8_t>(Prospero::BlendFactor::kOneMinusSrc1Alpha);
		};
		pixel_info.dual_source_blending =
		    blend.enable && !context.GetRenderTarget(0).info.blend_bypass &&
		    (is_dual_source(blend.color_srcblend) || is_dual_source(blend.color_destblend) ||
		     (blend.separate_alpha_blend &&
		      (is_dual_source(blend.alpha_srcblend) || is_dual_source(blend.alpha_destblend))));
		if (pixel_info.dual_source_blending) {
			// MRT1 supplies a second blend source for the same render target as MRT0.
			pixel_info.target_output_mode[1]    = pixel_info.target_output_mode[0];
			pixel_info.target_export_mapping[1] = pixel_info.target_export_mapping[0];
		}
	}
	if (context.GetClipControl().clip_disable) {
		const auto& viewport = context.GetScreenViewport().viewports[0];
		const auto& limits   = m_graphics.GetPhysicalDeviceProperties().limits;
		auto&       clip     = vertex_info[tess_active ? 2u : 0u].clip_space;
		clip.scale[0]        = viewport.xscale;
		clip.scale[1]        = viewport.yscale;
		clip.offset[0]       = viewport.xoffset;
		clip.offset[1]       = viewport.yoffset;
		clip.half_extent[0] =
		    static_cast<float>(std::min(limits.maxViewportDimensions[0], 16384u)) * 0.5f;
		clip.half_extent[1] =
		    static_cast<float>(std::min(limits.maxViewportDimensions[1], 16384u)) * 0.5f;
		clip.enabled = true;
	}
	Common::LockGuard lock(m_mutex);
	uint32_t          push_data_cursor =
	    mesh_active ? ShaderRecompiler::IR::PushData::MeshDrawDwordCount : 0;
	GraphicsPrograms  result;
	if (pixel_active) {
		result.pixel = m_program_cache->Get(pixel_params, pixel_info, push_data_cursor);
	}
	for (uint32_t i = 0; i < (tess_active ? 3u : 1u); i++) {
		result.vertex[i] = m_program_cache->Get(vertex_params[i], vertex_info[i], push_data_cursor);
	}
	return result;
}

ShaderProgram PipelineCache::GetComputeProgram(const HW::ComputeShaderInfo& regs,
                                               const HW::ShaderRegisters&   sh,
                                               ShaderComputeInputInfo&      input_info) {
	input_info.host_subgroup_size = m_graphics.SupportsComputeWave64() ? 64u : 32u;
	const auto        params      = PrepareProgram(regs, sh, input_info);
	Common::LockGuard lock(m_mutex);
	uint32_t          push_data_cursor = 0;
	return m_program_cache->Get(params, input_info, push_data_cursor);
}

bool PipelineStaticParameters::operator==(const PipelineStaticParameters& other) const noexcept {
	return std::memcmp(this, &other, sizeof(*this)) == 0;
}

PipelineCache::Pipeline& PipelineCache::GetGraphicsPipeline(
    std::span<const RenderColorInfo> colors, const RenderDepthInfo& depth,
    std::span<const ShaderVertexInputInfo> vertex_info, CommandBuffer& command,
    const ShaderPixelInputInfo* ps_input_info, vk::PrimitiveTopology topology,
    bool primitive_restart_enable, const GraphicsPrograms& programs) {
	const auto& vs_input_info  = vertex_info.front();
	const auto& vertex_program = programs.vertex[0];
	const auto& pixel_program  = programs.pixel;
	KYTY_PROFILER_BLOCK("PipelineCache::CreatePipeline(Gfx)", profiler::colors::DeepOrangeA200);

	EXIT_IF(colors.size() > RENDER_COLOR_ATTACHMENTS_MAX);
	EXIT_IF(!vertex_program);
	const bool ps_active = ps_input_info != nullptr;
	EXIT_IF(ps_active && !pixel_program);
	const auto color_count = static_cast<uint32_t>(colors.size());

	Common::LockGuard lock(m_mutex);
	auto&             ctx = command.GetRegisters();

	const HW::ModeControl& mc = ctx.GetModeControl();

	const auto vs_id = vertex_program.id;
	const auto ps_id = ps_active ? pixel_program.id : 0;

	GraphicsPipelineKey key {};
	for (uint32_t i = 0; i < programs.vertex.size(); i++) {
		key.vertex_shader_ids[i] = programs.vertex[i].id;
	}
	key.ps_shader_id            = ps_id;
	auto& static_params         = key.static_params;
	auto& rendering             = key.rendering;
	rendering.color_count       = 0;
	uint32_t attachment_samples = 0;
	for (uint32_t i = 0; i < color_count; i++) {
		const auto slot = colors[i].target_slot;
		EXIT_IF(slot >= RENDER_COLOR_ATTACHMENTS_MAX);
		rendering.color_count = std::max(rendering.color_count, slot + 1);
		EXIT_IF(!colors[i].image_id || colors[i].desc.view_info.format == vk::Format::eUndefined);
		static_params.color_mask[slot] = colors[i].export_mapping.ApplyMask(
		    render_target_mask_slot(ctx.GetRenderTargetMask(), colors[i].target_slot));
		rendering.color_formats[slot] = colors[i].desc.view_info.format;
		if (attachment_samples == 0) {
			attachment_samples = colors[i].desc.info.samples;
		} else if (attachment_samples != colors[i].desc.info.samples) {
			EXIT("mixed color attachment sample counts are unsupported: %u and %u\n",
			     attachment_samples, colors[i].desc.info.samples);
		}
		const auto& rt                        = ctx.GetRenderTarget(colors[i].target_slot);
		const auto& bc                        = ctx.GetBlendControl(colors[i].target_slot);
		static_params.color_srcblend[slot]       = bc.color_srcblend;
		static_params.color_comb_fcn[slot]       = bc.color_comb_fcn;
		static_params.color_destblend[slot]      = bc.color_destblend;
		static_params.alpha_srcblend[slot]       = bc.alpha_srcblend;
		static_params.alpha_comb_fcn[slot]       = bc.alpha_comb_fcn;
		static_params.alpha_destblend[slot]      = bc.alpha_destblend;
		static_params.separate_alpha_blend[slot] = bc.separate_alpha_blend;
		static_params.blend_enable[slot]         = bc.enable && !rt.info.blend_bypass;
	}
	const bool with_depth =
	    depth.desc.view_info.format != vk::Format::eUndefined && static_cast<bool>(depth.image_id);
	if (with_depth) {
		const auto aspects       = ImageViewOps::DepthAspectMask(depth.desc.view_info.format);
		rendering.depth_format   = aspects & vk::ImageAspectFlagBits::eDepth
		                               ? depth.desc.view_info.format
		                               : vk::Format::eUndefined;
		rendering.stencil_format = aspects & vk::ImageAspectFlagBits::eStencil
		                               ? depth.desc.view_info.format
		                               : vk::Format::eUndefined;
		if (attachment_samples == 0) {
			attachment_samples = depth.desc.info.samples;
		} else if (attachment_samples != depth.desc.info.samples) {
			EXIT("mixed color/depth sample counts are unsupported: %u and %u\n", attachment_samples,
			     depth.desc.info.samples);
		}
	}
	if (color_count == 0 && !with_depth) {
		attachment_samples = render_sample_count(ctx.GetAaConfig().msaa_num_samples);
		EXIT_IF(!static_cast<bool>(
		    m_graphics.GetPhysicalDeviceProperties().limits.framebufferNoAttachmentsSampleCounts &
		    vulkan_sample_count(attachment_samples)));
	}
	EXIT_IF(attachment_samples == 0 ||
	        vulkan_sample_count(attachment_samples) == vk::SampleCountFlagBits {});

	if (ps_active && depth.depth_test_enable && ps_input_info->ps_execute_on_noop) {
		static std::atomic<uint32_t> log_count {0};
		if (log_count.fetch_add(1, std::memory_order_relaxed) < 16) {
			LOGF("Pipeline: temporary: accepting EXEC_ON_NOOP with depth test enabled\n");
		}
	}

	const auto& clip_control               = ctx.GetClipControl();
	static_params.negative_one_to_one      = !clip_control.dx_clip_space;
	static_params.depth_clip_enable        = clip_control.IsZClipEnabled();
	static_params.topology                 = topology;
	static_params.primitive_restart_enable = primitive_restart_enable;
	static_params.samples                  = attachment_samples;
	static_params.sample_shading_enable =
	    ps_active && attachment_samples > 1 && ps_input_info->ps_sample_shading;
	if (static_params.sample_shading_enable && !m_graphics.sample_rate_shading_enabled) {
		EXIT("Pipeline: sample-rate shading is required but unsupported by the host\n");
	}
	static_params.depth_bounds_test_enable = depth.depth_bounds_test_enable;
	static_params.depth_min_bounds         = depth.depth_min_bounds;
	static_params.depth_max_bounds         = depth.depth_max_bounds;
	const bool rect_list = Prospero::IsRectList(command.GetUserConfig().GetPrimType());
	static_params.cull_back  = !rect_list && mc.cull_back;
	static_params.cull_front = !rect_list && mc.cull_front;
	static_params.face       = mc.face;
	static_params.provoking_vtx_last = mc.provoking_vtx_last;
	static_params.polygon_mode =
	    ResolvePolygonMode(mc, static_params.cull_front, static_params.cull_back);

	if (vs_input_info.stage.program->stage != ShaderType::Mesh) {
		EXIT_IF(vs_input_info.buffers_num < 0 ||
		        vs_input_info.buffers_num > ShaderVertexInputInfo::RES_MAX ||
		        vs_input_info.resources_num < 0 ||
		        vs_input_info.resources_num > ShaderVertexInputInfo::RES_MAX);
		key.vertex_input.binding_count   = static_cast<uint8_t>(vs_input_info.buffers_num);
		key.vertex_input.attribute_count = static_cast<uint8_t>(vs_input_info.resources_num);
		uint32_t attributes_num          = 0;
		for (int binding = 0; binding < vs_input_info.buffers_num; binding++) {
			const auto& buffer = vs_input_info.buffers[binding];
			EXIT_IF(buffer.attr_num < 0 || buffer.attr_num > ShaderVertexInputBuffer::ATTR_MAX);
			attributes_num += static_cast<uint32_t>(buffer.attr_num);
			EXIT_IF(attributes_num > static_cast<uint32_t>(vs_input_info.resources_num));
			key.vertex_input.bindings[binding] = {.stride   = buffer.stride,
			                                      .instance = buffer.fetch_index != 0};
			for (int attribute = 0; attribute < buffer.attr_num; attribute++) {
				const auto index = buffer.attr_indices[attribute];
				EXIT_IF(index < 0 || index >= vs_input_info.resources_num);
				key.vertex_input.attributes[index] = {
				    .offset  = buffer.attr_offsets[attribute],
				    .binding = static_cast<uint8_t>(binding),
				};
			}
		}
		EXIT_IF(attributes_num != static_cast<uint32_t>(vs_input_info.resources_num));
	}

	if (auto iter = m_graphics_pipelines.find(key); iter != m_graphics_pipelines.end()) {
		return *iter->second;
	}

	if (graphics_debug_dump_enabled()) {
		ShaderDbgDumpInputInfo(vs_input_info);
		if (ps_active) {
			ShaderDbgDumpInputInfo(*ps_input_info);
		}
		LOGF("PipelineTrace: shader modules VS=%" PRIu64 " module=%p PS=%" PRIu64 " module=%p\n",
		     vs_id, static_cast<void*>(vertex_program.module), ps_id,
		     static_cast<void*>(pixel_program.module));
	}

	auto cached = std::make_unique<Pipeline>();
	LogPipelineTrace("CreatePipelineInternal begin", vs_id, ps_id);
	const auto create_begin = Common::Timer::QueryPerformanceCounter();
	auto       build = PrepareGraphicsPipeline(m_graphics, *cached, rendering, key.vertex_input,
	                                           vertex_info, ps_input_info, programs, static_params);
	bool       deferred = false;
	if (m_async_pipelines) {
		// A driver-cache hit takes ~0.2 ms: create it now so the draw is not skipped. A real
		// compile (0.1-4 s) goes to a worker; until it finishes, draws using it are skipped
		// instead of the whole frame stalling behind the driver.
		if (!m_graphics.pipeline_cache_control_enabled ||
		    !FinishGraphicsPipeline(m_graphics, *build, m_driver_cache, true)) {
			cached->ready.store(false, std::memory_order_relaxed);
			AsyncCompiler::Job job {.build = std::move(build), .task = {}};
			deferred = m_async->Submit(job);
			if (!deferred) {
				build = std::move(job.build);
				cached->ready.store(true, std::memory_order_relaxed);
			}
		}
	}
	if (!deferred && cached->pipeline == nullptr) {
		FinishGraphicsPipeline(m_graphics, *build, m_driver_cache, false);
	}
	if (CompileLogEnabled()) {
		const auto create_end = Common::Timer::QueryPerformanceCounter();
		std::printf("pipeline qpc=%" PRIu64 " kind=%s vs=%" PRIu64 " ps=%" PRIu64
		            " ms=%.2f total=%zu\n",
		            create_end, deferred ? "gfx-deferred" : "gfx", vs_id, ps_id,
		            QpcMs(create_begin, create_end), m_graphics_pipelines.size() + 1);
	}
	LogPipelineTrace("CreatePipelineInternal done", vs_id, ps_id);
	NotePipelineCreated();

	EXIT_NOT_IMPLEMENTED(!deferred && cached->pipeline == nullptr);
	EXIT_NOT_IMPLEMENTED(cached->pipeline_layout == nullptr);

	auto [iter, inserted] = m_graphics_pipelines.emplace(std::move(key), std::move(cached));
	EXIT_IF(!inserted);

	return *iter->second;
}

PipelineCache::Pipeline&
PipelineCache::GetComputePipeline(const ShaderComputeInputInfo& input_info,
                                  const ShaderProgram&          compute_program) {
	KYTY_PROFILER_BLOCK("PipelineCache::CreatePipeline(Compute)", profiler::colors::RedA100);

	EXIT_IF(!compute_program);

	Common::LockGuard lock(m_mutex);

	if (auto iter = m_compute_pipelines.find(compute_program.id);
	    iter != m_compute_pipelines.end()) {
		return *iter->second;
	}

	if (graphics_debug_dump_enabled()) {
		ShaderDbgDumpInputInfo(input_info);
	}

	auto cached = std::make_unique<Pipeline>();
	const auto create_begin = Common::Timer::QueryPerformanceCounter();
	CreatePipelineInternal(m_graphics, *cached, input_info, compute_program.module, m_driver_cache);
	if (CompileLogEnabled()) {
		const auto create_end = Common::Timer::QueryPerformanceCounter();
		std::printf("pipeline qpc=%" PRIu64 " kind=cs cs=%" PRIu64 " ms=%.2f total=%zu\n", create_end,
		            compute_program.id, QpcMs(create_begin, create_end),
		            m_compute_pipelines.size() + 1);
	}
	NotePipelineCreated();

	EXIT_NOT_IMPLEMENTED(cached->pipeline == nullptr);
	EXIT_NOT_IMPLEMENTED(cached->pipeline_layout == nullptr);

	auto [iter, inserted] = m_compute_pipelines.emplace(compute_program.id, std::move(cached));
	EXIT_IF(!inserted);

	return *iter->second;
}
} // namespace Libs::Graphics
