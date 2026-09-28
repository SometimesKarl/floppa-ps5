#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SRTWALKER_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SRTWALKER_H_

#include "graphics/shader/recompiler/ir/ShaderIR.h"

#include <span>

namespace Libs::Graphics::ShaderRecompiler::IR {

class Value;

using SrtMemoryReader = bool (*)(void* userdata, uint64_t address, std::span<uint32_t> values);

struct SrtRuntime {
	std::span<const uint32_t> user_data;
	uint64_t                  shader_base                = 0;
	SrtMemoryReader           read_memory                = nullptr;
	void*                     userdata                   = nullptr;
	SrtMemoryReader           read_specialization_memory = nullptr;
	// MaterializeResources: reuse the values of the plan's previous walk whose inputs (user-data
	// registers, guest memory words) did not change. Not for callers that observe reads (a
	// memo capturing them needs every read performed).
	bool                      incremental                = false;
};

enum class RuntimeValueType { Any, Integer };

// Reads guest memory that the walker would otherwise dereference directly (runtime without
// read_memory). Returns false when the bytes must be read through the guest address instead,
// for example because the GPU wrote them. Installed once by the pipeline cache.
using SrtDirectReader = bool (*)(uint64_t address, void* data, uint64_t size);
void SetSrtDirectReader(SrtDirectReader reader);

// Observes, on the calling thread while set, every word the walker reads directly from guest
// memory (runtimes without read_memory). Used to validate cached materializations.
using SrtReadObserver = void (*)(void* context, uint64_t address, uint32_t value);
// Also observes user-data reads (index into SrtRuntime::user_data), with the same context.
using SrtUserDataObserver = void (*)(void* context, uint32_t index, uint32_t value);
void SetSrtReadObserver(SrtReadObserver observer, SrtUserDataObserver user_data_observer,
                        void* context);

// Selects the decoded-node evaluator (default) or the IR interpreter for plans that allow it.
// KYTY_SRT_VERIFY compares the two on every materialization.
void SetSrtFastEvaluation(bool enabled);
[[nodiscard]] bool SrtFastEvaluation();

// Collects reachable ReadConst values. Immediate offsets receive compact flat-buffer slots;
// dynamic offsets remain explicit and are never assigned a fake slot.
void BuildSrtPlan(Program& program);
bool ValidateRuntimeValue(const ResourcePlan& program, Value value,
                          RuntimeValueType type = RuntimeValueType::Any);
// Uses the strict reader for values that affect shader specialization.
SrtRuntime CleanRuntime(SrtRuntime runtime);

// One memoized evaluation session shared by the entire shader resource refresh.
class SrtWalker {
public:
	SrtWalker(const ResourcePlan& program, const SrtRuntime& runtime,
	          std::span<const uint8_t> clean_flat_slots = {}, SrtWalker* clean_evaluator = nullptr,
	          Value active_mask = {});
	~SrtWalker();
	SrtWalker(const SrtWalker&)            = delete;
	SrtWalker& operator=(const SrtWalker&) = delete;

	bool Evaluate(Value value, uint32_t& result);
	bool EvaluateDescriptor(uint32_t source, DescriptorValue& result);

	// Incremental walks. Every memoized value records dependency bits: one per user-data
	// register it read (the last bit shared by high registers) and one per bucket of guest
	// memory words it read. Before evaluating, KeepUnchanged keeps the values of this context's
	// previous walk whose bits miss `changed` (memory buckets whose words changed; registers
	// that differ are added here). Nothing is kept when the previous walk had other inputs
	// shapes (user-data count, shader base) or this walker evaluates under an EXEC mask.
	static uint64_t MemoryBit(uint64_t address);
	void            KeepUnchanged(uint64_t changed);
	// While set on this thread, every guest memory word a walker reads is appended here.
	static void RecordReads(std::vector<ResourcePlan::DeltaRead>* reads);
	// Reads a recorded word again through the same kind of reader.
	static bool ReadAgain(const ResourcePlan::DeltaRead& read, const SrtRuntime& clean_runtime,
	                      uint32_t& value);
	// An empty span means that all sources are active.
	std::span<const uint8_t> FindActiveSources();
	bool RefreshFlatBuffer(std::vector<uint32_t>& flat);

private:
	static ResourcePlan::EvaluationContext& AcquireContext(const ResourcePlan& program);
	static float Float32(uint64_t bits);
	bool EvaluateWide(Value value, uint64_t& result);
	bool Arg(const Inst& inst, size_t index, uint64_t& result);
	bool EvaluatePhi(const Inst& inst, uint64_t& result);
	bool EvaluateExtract(const Inst& inst, uint64_t& result);
	bool EvaluateRawRead(const Inst& inst, uint64_t& result);
	bool ReadRaw(const MemoryInfo& mem, bool constant_buffer, uint64_t low, uint64_t high,
	             uint64_t offset, uint64_t records, uint64_t& result);
	bool EvaluateInst(const Inst& inst, uint64_t& result);
	bool EvaluateIndex(uint32_t index, const Inst& inst, uint64_t& result);
	bool EvaluateNode(uint32_t index, const Inst& inst, uint64_t& result);
	void DecodeNode(uint32_t index, const Inst& inst);
	bool NodeArg(const SrtNode& node, uint32_t operand, uint64_t& result);
	uint64_t DecodeRoot(Value value);
	bool     EvaluateRoot(uint64_t root, uint32_t& result);
	bool     EvaluateFlatRead(size_t read, uint32_t& result);

	const ResourcePlan&              m_program;
	SrtRuntime                      m_runtime;
	std::span<const uint8_t>         m_clean_flat_slots;
	SrtWalker*                      m_clean_evaluator = nullptr;
	Value                           m_active_mask;
	ResourcePlan::EvaluationContext& m_context;
	bool                             m_fast = false;
	// The previous walk in this context: its generation (its inputs are in m_context).
	uint64_t                         m_previous_generation = 0;
};

} // namespace Libs::Graphics::ShaderRecompiler::IR

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SRTWALKER_H_ */
