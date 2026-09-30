// Dune-shaped resource walk: benchmark and differential check.
//
// ASTRO BOT's dune scene issues ~14k small draws a second and the GPU thread spends about a third
// of its time materializing shader resources (docs: perf-handoff/FINDINGS.md, X52). This builds a
// pixel shader shaped like those draws with the real resource tracking pass: descriptors behind
// two levels of guest pointers, a per-draw constant table that moves every draw, flattened SRT
// words, and resources under guest-controlled branches. It then walks it the way the pipeline
// cache does for a memo miss (no read_memory: direct reads of mapped memory, strict reads for
// specialization values).
//
// Every walk of the decoded-node evaluator is compared with the IR interpreter
// (SetSrtFastEvaluation(false)): the benchmark doubles as a differential test of evaluator
// changes. `srt_walk_bench [walks]`; ctest runs it with a small count.

#include "graphics/guest_gpu/gpu_defs.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"
#include "graphics/shader/recompiler/ir/passes/ResourceMaterialization.h"
#include "graphics/shader/recompiler/ir/passes/ResourceTracking.h"
#include "graphics/shader/recompiler/ir/passes/SrtWalker.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace Libs::Graphics::ShaderRecompiler::IR;
using Libs::Graphics::ShaderType;
namespace Decoder = Libs::Graphics::ShaderRecompiler::Decoder;

void Check(bool condition, const char* message) {
	if (!condition) {
		throw std::runtime_error(message);
	}
}

// Guest memory for the walk: host memory whose addresses serve as guest addresses, so direct
// reads (runtime without read_memory) dereference it as the emulator dereferences mapped memory.
struct Arena {
	std::unique_ptr<uint32_t[]> words;
	size_t                      used = 0;
	size_t                      size = 0;

	explicit Arena(size_t count): words(new uint32_t[count] {}), size(count) {}
	// Allocates `count` words aligned to 16 bytes (descriptor tables are aligned on the GPU).
	uint32_t* Allocate(size_t count) {
		used = (used + 3u) & ~size_t {3};
		Check(used + count <= size, "arena exhausted");
		auto* result = words.get() + used;
		used += count;
		return result;
	}
	static uint64_t Address(const uint32_t* word) { return reinterpret_cast<uint64_t>(word); }
	static void StorePointer(uint32_t* at, const uint32_t* target) {
		const auto address = Address(target);
		at[0]              = static_cast<uint32_t>(address);
		at[1]              = static_cast<uint32_t>(address >> 32u);
	}
};

uint64_t g_strict_reads = 0;

bool StrictRead(void*, uint64_t address, std::span<uint32_t> values) {
	g_strict_reads += values.size();
	std::memcpy(values.data(), reinterpret_cast<const void*>(address), values.size_bytes());
	return true;
}

struct Builder {
	Program program;
	Block*  block = nullptr;
	uint32_t pc   = 0x100;

	Builder() {
		program.stage           = ShaderType::Pixel;
		program.user_data_count = 16;
		block                   = AddBlock();
	}

	Block* AddBlock() {
		auto  storage = std::make_unique<Block>();
		auto* result  = storage.get();
		program.block_storage.push_back(std::move(storage));
		program.blocks.push_back(result);
		program.block_info.push_back({.id = static_cast<uint32_t>(program.block_info.size())});
		return result;
	}

	Value Emit(ValueOpcode opcode, std::initializer_list<Value> args, uint64_t flags = 0,
	           Block* destination = nullptr) {
		auto& inst = (destination != nullptr ? destination : block)->AppendNewInst(opcode, args, flags);
		return Value(&inst);
	}
	template <typename T>
	Value Emit(ValueOpcode opcode, std::initializer_list<Value> args, T flags,
	           Block* destination = nullptr) {
		uint64_t bits = 0;
		std::memcpy(&bits, &flags, sizeof(flags));
		return Emit(opcode, args, bits, destination);
	}
	MemoryFlags AddMemory(MemoryInfo memory) {
		const auto index = static_cast<uint32_t>(program.memory_info.size());
		program.memory_info.push_back(memory);
		pc += 4;
		return {index, pc};
	}
	Value UserData(uint32_t index) {
		return Emit(ValueOpcode::GetUserData, {Value(static_cast<ScalarReg>(index))});
	}
	Value Address(Value low, Value high) {
		return Emit(ValueOpcode::GetAddressResource, {low, high}, MemoryFlags {0, pc});
	}
	// s_load_dword from `address` + `offset` bytes.
	Value ScalarLoad(Value address, uint32_t offset, Block* destination = nullptr) {
		MemoryInfo memory;
		memory.kind   = ResourceKind::ScalarAddress;
		memory.offset = offset;
		return Emit(ValueOpcode::LoadAddressU32, {address, Value(0u), Value(0u), Value(true)},
		            AddMemory(memory), destination);
	}
	template <size_t N>
	std::array<Value, N> ScalarLoads(Value address, uint32_t offset, Block* destination = nullptr) {
		std::array<Value, N> result;
		for (uint32_t i = 0; i < N; i++) {
			result[i] = ScalarLoad(address, offset + i * 4u, destination);
		}
		return result;
	}
	Value Pointer(Value table, uint32_t offset) {
		return Address(ScalarLoad(table, offset), ScalarLoad(table, offset + 4u));
	}
	void Sample(const std::array<Value, 8>& image, const std::array<Value, 4>& sampler,
	            Block* destination = nullptr) {
		const auto handle = Emit(ValueOpcode::GetImageResource,
		                         {image[0], image[1], image[2], image[3], image[4], image[5],
		                          image[6], image[7]},
		                         MemoryFlags {0, pc}, destination);
		const auto sampler_handle = Emit(ValueOpcode::GetSamplerResource,
		                                 {sampler[0], sampler[1], sampler[2], sampler[3]},
		                                 MemoryFlags {0, pc}, destination);
		const auto address = Emit(ValueOpcode::MakeImageAddress,
		                          {Value(0u), Value(0u), Value(0u), Value(0u), Value(0u),
		                           Value(0u), Value(0u), Value(0u), Value(0u), Value(0u),
		                           Value(0u), Value(0u), Value(0u)},
		                          0, destination);
		MemoryInfo memory;
		memory.kind            = ResourceKind::Image;
		memory.image_dimension = Decoder::ImageDimension::Dim2D;
		Emit(ValueOpcode::ImageSampleRaw, {handle, sampler_handle, address}, AddMemory(memory),
		     destination);
	}
	Value BufferHandle(const std::array<Value, 4>& words, Block* destination = nullptr) {
		return Emit(ValueOpcode::GetBufferResource, {words[0], words[1], words[2], words[3]},
		            MemoryFlags {0, pc}, destination);
	}
	void LoadBuffer(const std::array<Value, 4>& words, Block* destination = nullptr) {
		MemoryInfo memory;
		memory.kind = ResourceKind::Buffer;
		Emit(ValueOpcode::LoadBufferU32,
		     {BufferHandle(words, destination), Value(0u), Value(0u), Value(0u), Value(true)},
		     AddMemory(memory), destination);
	}
	Value ReadConstant(const std::array<Value, 4>& words, uint32_t offset,
	                   Block* destination = nullptr) {
		MemoryInfo memory;
		memory.kind   = ResourceKind::ScalarBuffer;
		memory.offset = offset;
		return Emit(ValueOpcode::ReadConstBuffer, {BufferHandle(words, destination), Value(0u)},
		            AddMemory(memory), destination);
	}

	// As the resource tracking test fixture: keep every memory result alive and track.
	void Track() {
		for (size_t index = 0; index < program.block_info.size(); ++index) {
			const auto condition = program.block_info[index].condition;
			if (!condition.IsEmpty()) {
				Emit(ValueOpcode::Reference, {condition}, 0, program.blocks[index]);
			}
		}
		for (auto* target: program.blocks) {
			for (auto& inst: *target) {
				if (inst.HasUses() || inst.MayHaveSideEffects() ||
				    (BufferAccessOf(inst.GetOpcode()) == BufferAccess::None &&
				     AddressOpcodeInfoOf(inst.GetOpcode()).access == AddressAccess::None &&
				     ImageOpcodeInfoOf(inst.GetOpcode()).access == ImageAccess::None)) {
					continue;
				}
				auto value = Value(&inst);
				if (value.GetType() == Type::U32x4) {
					value = Emit(ValueOpcode::CompositeExtractU32x4, {value, Value(0u)}, 0, target);
				}
				Check(value.GetType() == Type::U32, "unhandled memory result type");
				Emit(ValueOpcode::ReferenceU32, {value}, 0, target);
			}
		}
		TrackResources(program, {}, {});
	}
};

namespace CFG = Libs::Graphics::ShaderRecompiler::CFG;

// Guest layout (all in the arena):
//   user data s[0:1] -> scene table: +0 material table pointer, +8 view table pointer
//   user data s[2:3] -> per-draw table (a different one every draw): 8 constant words + a V#
//   material table: 6 T# (8 words) at +0, 6 S# (4 words) at +0xc0, 2 V# at +0x120
//   view table: 2 V# (constant buffers) at +0, flag words at +0x20 (branch conditions)
// The per-draw V# points at a constant buffer whose first word selects a branch per draw.
constexpr uint32_t MaterialImages   = 6;
constexpr uint32_t MaterialSamplers = 6;
constexpr uint32_t DrawTables       = 64;

struct Scene {
	Arena                  arena {1u << 16};
	uint32_t*              scene     = nullptr;
	uint32_t*              material  = nullptr;
	uint32_t*              view      = nullptr;
	std::vector<uint32_t*> draws;
	std::vector<uint32_t*> draw_constants;
};

void StoreBuffer(uint32_t* at, const uint32_t* target, uint32_t records) {
	const auto address = Arena::Address(target);
	at[0]              = static_cast<uint32_t>(address);
	at[1]              = static_cast<uint32_t>(address >> 32u) | (16u << 16u);
	at[2]              = records;
	at[3]              = 0x4dfacu;
}

Scene BuildScene() {
	Scene scene;
	scene.scene    = scene.arena.Allocate(16);
	scene.material = scene.arena.Allocate(0x140 / 4);
	scene.view     = scene.arena.Allocate(0x40 / 4);
	Arena::StorePointer(scene.scene, scene.material);
	Arena::StorePointer(scene.scene + 2, scene.view);
	constexpr std::array<uint32_t, 8> image {0x0208a200u, 0xca900000u, 0x800fc00fu, 0x90960facu,
	                                         0u,          0x60u,       0u,          0u};
	for (uint32_t i = 0; i < MaterialImages; i++) {
		std::memcpy(scene.material + i * 8u, image.data(), sizeof(image));
		scene.material[i * 8u] += i * 0x100u; // distinct textures
	}
	for (uint32_t i = 0; i < MaterialSamplers; i++) {
		scene.material[0xc0 / 4 + i * 4u] = i;
	}
	auto* material_data = scene.arena.Allocate(64);
	StoreBuffer(scene.material + 0x120 / 4, material_data, 64);
	StoreBuffer(scene.material + 0x130 / 4, material_data + 32, 32);
	auto* view_data = scene.arena.Allocate(64);
	StoreBuffer(scene.view, view_data, 64);
	StoreBuffer(scene.view + 4, view_data + 16, 64);
	scene.view[0x20 / 4] = 1u;
	scene.view[0x24 / 4] = 0u;
	for (uint32_t i = 0; i < DrawTables; i++) {
		auto* table     = scene.arena.Allocate(16);
		auto* constants = scene.arena.Allocate(64);
		for (uint32_t word = 0; word < 8; word++) {
			table[word] = i * 8u + word;
		}
		StoreBuffer(table + 8, constants, 64 * 4);
		constants[0] = i % 3u; // per-draw branch selector
		scene.draws.push_back(table);
		scene.draw_constants.push_back(constants);
	}
	return scene;
}

ResourcePlan BuildPlan() {
	Builder b;
	auto* entry = b.block;
	// entry -> (view flag 0 != 0 ? lit : unlit) -> merge -> (draw selector != 0 ? detail : merge2)
	auto* lit    = b.AddBlock();
	auto* unlit  = b.AddBlock();
	auto* merge  = b.AddBlock();
	auto* detail = b.AddBlock();
	auto* done   = b.AddBlock();
	entry->AddBranch(lit);
	entry->AddBranch(unlit);
	lit->AddBranch(merge);
	unlit->AddBranch(merge);
	merge->AddBranch(detail);
	merge->AddBranch(done);
	detail->AddBranch(done);
	b.program.block_info[0].terminator = {
	    .kind = CFG::TerminatorKind::ConditionalBranch, .true_block = 1, .false_block = 2};
	b.program.block_info[1].terminator = {.kind = CFG::TerminatorKind::Branch, .true_block = 3};
	b.program.block_info[2].terminator = {.kind = CFG::TerminatorKind::Branch, .true_block = 3};
	b.program.block_info[3].terminator = {
	    .kind = CFG::TerminatorKind::ConditionalBranch, .true_block = 4, .false_block = 5};
	b.program.block_info[4].terminator = {.kind = CFG::TerminatorKind::Branch, .true_block = 5};

	const auto scene    = b.Address(b.UserData(0), b.UserData(1));
	const auto draw     = b.Address(b.UserData(2), b.UserData(3));
	const auto material = b.Pointer(scene, 0);
	const auto view     = b.Pointer(scene, 8);

	std::vector<std::array<Value, 8>> images;
	std::vector<std::array<Value, 4>> samplers;
	for (uint32_t i = 0; i < MaterialImages; i++) {
		images.push_back(b.ScalarLoads<8>(material, i * 32u));
	}
	for (uint32_t i = 0; i < MaterialSamplers; i++) {
		samplers.push_back(b.ScalarLoads<4>(material, 0xc0u + i * 16u));
	}
	const auto material_cb0 = b.ScalarLoads<4>(material, 0x120u);
	const auto material_cb1 = b.ScalarLoads<4>(material, 0x130u);
	const auto view_cb0     = b.ScalarLoads<4>(view, 0x00u);
	const auto view_cb1     = b.ScalarLoads<4>(view, 0x10u);
	const auto draw_cb      = b.ScalarLoads<4>(draw, 0x20u);

	// Per-draw scalar constants and view constants feed the shader through the flat SRT.
	for (uint32_t i = 0; i < 8; i++) {
		b.ReadConstant(draw_cb, i * 4u);
	}
	for (uint32_t i = 0; i < 4; i++) {
		b.ReadConstant(view_cb0, i * 4u);
	}
	for (uint32_t i = 0; i < 8; i++) {
		(void)b.ScalarLoad(draw, i * 4u);
	}
	b.LoadBuffer(material_cb0);
	b.LoadBuffer(material_cb1);
	b.LoadBuffer(view_cb1);
	b.Sample(images[0], samplers[0]);
	b.Sample(images[1], samplers[1]);

	const auto view_flag = b.ScalarLoad(view, 0x20u);
	b.program.block_info[0].condition =
	    b.Emit(ValueOpcode::INotEqual32, {view_flag, Value(0u)}, 0, entry);
	b.Sample(images[2], samplers[2], lit);
	b.Sample(images[3], samplers[3], unlit);
	const auto selector = b.ReadConstant(draw_cb, 0u, merge);
	b.program.block_info[3].condition =
	    b.Emit(ValueOpcode::INotEqual32, {selector, Value(0u)}, 0, merge);
	b.Sample(images[4], samplers[4], detail);
	b.Sample(images[5], samplers[5], done);
	b.Track();
	return ExtractResourcePlan(b.program);
}

struct Walk {
	bool                   ok = false;
	ResourceSnapshot       snapshot;
	ResourceSpecialization specialization;
};

bool Same(const Walk& a, const Walk& b) {
	return a.ok == b.ok &&
	       (!a.ok || (a.snapshot.buffers == b.snapshot.buffers &&
	                  a.snapshot.images == b.snapshot.images &&
	                  a.snapshot.samplers == b.snapshot.samplers &&
	                  a.snapshot.flattened_srt == b.snapshot.flattened_srt &&
	                  a.specialization == b.specialization));
}

std::array<uint32_t, 16> UserData(const Scene& scene, uint32_t draw) {
	std::array<uint32_t, 16> user_data {};
	Arena::StorePointer(user_data.data(), scene.scene);
	Arena::StorePointer(user_data.data() + 2, scene.draws[draw % scene.draws.size()]);
	for (uint32_t i = 4; i < user_data.size(); i++) {
		user_data[i] = draw * 16u + i;
	}
	return user_data;
}

} // namespace

int main(int argc, char** argv) {
	try {
		const uint32_t walks = argc > 1 ? static_cast<uint32_t>(std::strtoul(argv[1], nullptr, 10))
		                                : 200000u;
		// `fast`: time only the decoded-node evaluator (for profiling it alone).
		const bool fast_only = argc > 2 && std::string(argv[2]) == "fast";
		auto       scene = BuildScene();
		const auto plan  = BuildPlan();
		Check(plan.resource_tracking_complete && plan.srt_plan_complete, "plan incomplete");
		std::printf("srt_walk_bench: %zu descriptor sources, %zu buffers, %zu images, %zu samplers, "
		            "%zu flat SRT reads, %zu blocks, %u evaluation values\n",
		            plan.descriptor_sources.size(), plan.info.buffers.size(),
		            plan.info.images.size(), plan.info.samplers.size(), plan.srt_reads.size(),
		            plan.control_flow.size(), plan.evaluation_value_count);

		// Differential: decoded nodes against the IR interpreter, draws cycling through every
		// per-draw table and both view branches. Every seventh walk the draw's constant buffer
		// has no records, so the branch selector read fails (both successors stay active).
		std::mt19937 random(0x5eedd00eu);
		Walk         fast;
		Walk         reference;
		const uint32_t checks = fast_only ? 0u : std::min<uint32_t>(walks, 4096u);
		for (uint32_t i = 0; i < checks; i++) {
			const auto draw = static_cast<uint32_t>(random());
			if ((i & 63u) == 0u) {
				scene.view[0x20 / 4] ^= 1u;
			}
			auto*      records = scene.draws[draw % scene.draws.size()] + 8 + 2;
			const auto saved   = *records;
			if (i % 7u == 3u) {
				*records = 0;
			}
			const auto user_data = UserData(scene, draw);
			const SrtRuntime runtime {.user_data = user_data, .read_specialization_memory = StrictRead};
			SetSrtFastEvaluation(false);
			reference.ok = MaterializeResources(plan, runtime, reference.snapshot,
			                                    reference.specialization);
			SetSrtFastEvaluation(true);
			fast.ok = MaterializeResources(plan, runtime, fast.snapshot, fast.specialization);
			*records = saved;
			Check(reference.ok, "reference walk failed");
			if (!Same(fast, reference)) {
				std::fprintf(stderr, "srt_walk_bench: walk %u differs (seed 0x5eedd00e)\n", i);
				return 1;
			}
		}
		std::printf("srt_walk_bench: %u walks identical to the IR interpreter\n", checks);

		for (const bool interpreter: {false, true}) {
			if (interpreter && fast_only) {
				break;
			}
			SetSrtFastEvaluation(!interpreter);
			Walk     walk;
			uint64_t failures = 0;
			g_strict_reads    = 0;
			const auto begin  = std::chrono::steady_clock::now();
			for (uint32_t i = 0; i < walks; i++) {
				const auto user_data = UserData(scene, i);
				const SrtRuntime runtime {.user_data                  = user_data,
				                          .read_specialization_memory = StrictRead};
				failures += MaterializeResources(plan, runtime, walk.snapshot, walk.specialization)
				                ? 0u
				                : 1u;
			}
			const auto elapsed = std::chrono::duration<double, std::nano>(
			                         std::chrono::steady_clock::now() - begin)
			                         .count();
			std::printf("srt_walk_bench: %-16s %u walks, %.0f ns/walk, %.1f strict reads/walk, "
			            "%llu failed\n",
			            interpreter ? "IR interpreter" : "decoded nodes", walks,
			            elapsed / static_cast<double>(walks),
			            static_cast<double>(g_strict_reads) / static_cast<double>(walks),
			            static_cast<unsigned long long>(failures));
			Check(failures == 0, "benchmark walk failed");
		}
		SetSrtFastEvaluation(true);
	} catch (const std::exception& exception) {
		std::fprintf(stderr, "srt_walk_bench failed: %s\n", exception.what());
		return 1;
	}
	return 0;
}

// The full emulator supplies these assertion hooks through common.
namespace Common {
int DbgExitHandler(const char*, int, std::string_view text) {
	throw std::runtime_error(std::string(text));
}
int DbgExitHandler(const char*, int, fmt::text_style, std::string_view text) {
	throw std::runtime_error(std::string(text));
}
int DbgExitIfHandler(const char* expression, const char* file, int line) {
	throw std::runtime_error(std::string("assertion: ") + expression + " at " + file + ':' +
	                         std::to_string(line));
}
int DbgNotImplementedHandler(const char* expression, const char* file, int line) {
	throw std::runtime_error(std::string("not implemented: ") + expression + " at " + file + ':' +
	                         std::to_string(line));
}
void DbgExit(int) { throw std::runtime_error("assertion failed"); }
} // namespace Common

#include "graphics/shader/recompiler/ir/Block.cpp"
#include "graphics/shader/recompiler/ir/Program.cpp"
#include "graphics/shader/recompiler/ir/Type.cpp"
#include "graphics/shader/recompiler/ir/Value.cpp"
#include "graphics/shader/recompiler/ir/opcodes/ValueOpcodes.cpp"
#include "graphics/shader/recompiler/ir/passes/DeadCodeElimination.cpp"
#include "graphics/shader/recompiler/ir/passes/ConstantPropagation.cpp"
