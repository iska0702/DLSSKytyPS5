#include "common/assert.h"
#include "graphics/shader/recompiler/frontend/translate/Translator.h"
#include "graphics/shader/recompiler/CodegenOptions.h"
#include "graphics/shader/recompiler/frontend/translate/rrPlayroomProfile.h"

namespace Libs::Graphics::ShaderRecompiler::Frontend {

void Translator::FailMissingTranslation(const Decoder::Instruction& inst) {
	EXIT("opcode %s at pc 0x%08x has no IR translation",
	     Decoder::InstructionToString(inst).c_str(), inst.pc);
}

void Translator::TranslateInstruction(const Decoder::Instruction& inst) {
	current_opcode = inst.opcode;
	current_pc     = inst.pc;
	if (GetCodegenOptions().rr_guide_capture && GetCodegenOptions().rt_software &&
	    program.stage == ShaderType::Compute &&
	    PlayroomReflectionCapturePC(program.shader_hash) != UINT32_MAX &&
	    (inst.pc == PlayroomReflectionCapturePC(program.shader_hash) || inst.opcode == Decoder::Opcode::IMAGE_STORE)) {
		// Copy raw values before the guest destructively reuses v10. No guest
		// register, descriptor or HDR channel is changed by this instrumentation.
		const bool ray_parameter = inst.pc == PlayroomReflectionCapturePC(program.shader_hash);
		IR::Value distance(0u), hit(0u);
		if (ray_parameter) {
			distance=ir.GetVectorReg(static_cast<IR::VectorReg>(10));
			hit=ir.GetVectorReg(static_cast<IR::VectorReg>(12));
		}
		ir.Emit(IR::ValueOpcode::StoreRayGuide,
		    {distance, hit,
		     ir.GetVectorReg(static_cast<IR::VectorReg>(22)),
		     ir.GetVectorReg(static_cast<IR::VectorReg>(23)),
		     IR::Value(ray_parameter ? 0u : 1u), ir.GetExec()});
	}

	switch (inst.opcode) {
		case Decoder::Opcode::UNKNOWN:
		case Decoder::Opcode::COUNT:
			EXIT("decoded opcode has no IR translation at pc 0x%08x", inst.pc);
		case Decoder::Opcode::UNSUPPORTED:
			EXIT("unsupported decoded instruction: %s", Decoder::InstructionToString(inst).c_str());
		default: break;
	}

	switch (inst.family) {
		case Decoder::Family::SOP1:
		case Decoder::Family::SOP2:
		case Decoder::Family::SOPK:
		case Decoder::Family::SOPC:
		case Decoder::Family::SOPP: return EmitScalar(inst);
		case Decoder::Family::VOP1:
		case Decoder::Family::VOP2:
		case Decoder::Family::VOP3:
		case Decoder::Family::VOP3P:
		case Decoder::Family::VOPC: return EmitVector(inst);
		case Decoder::Family::SMEM:
		case Decoder::Family::MUBUF:
		case Decoder::Family::MTBUF:
		case Decoder::Family::FLAT:
		case Decoder::Family::DS:
		case Decoder::Family::MIMG: return EmitMemory(inst);
		case Decoder::Family::VINTRP: return EmitInterpolation(inst);
		case Decoder::Family::EXP: return EXP(inst);
		default: return FailMissingTranslation(inst);
	}
}

} // namespace Libs::Graphics::ShaderRecompiler::Frontend
