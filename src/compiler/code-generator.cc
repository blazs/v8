// Copyright 2013 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/compiler/code-generator.h"

#include "src/compiler/code-generator-impl.h"
#include "src/compiler/linkage.h"
#include "src/compiler/pipeline.h"

namespace v8 {
namespace internal {
namespace compiler {

CodeGenerator::CodeGenerator(InstructionSequence* code)
    : code_(code),
      current_block_(NULL),
      current_source_position_(SourcePosition::Invalid()),
      masm_(code->zone()->isolate(), NULL, 0),
      resolver_(this),
      safepoints_(code->zone()),
      lazy_deoptimization_entries_(
          LazyDeoptimizationEntries::allocator_type(code->zone())),
      deoptimization_states_(
          DeoptimizationStates::allocator_type(code->zone())),
      deoptimization_literals_(Literals::allocator_type(code->zone())),
      translations_(code->zone()) {
  deoptimization_states_.resize(code->GetDeoptimizationEntryCount(), NULL);
}


Handle<Code> CodeGenerator::GenerateCode() {
  CompilationInfo* info = linkage()->info();

  // Emit a code line info recording start event.
  PositionsRecorder* recorder = masm()->positions_recorder();
  LOG_CODE_EVENT(isolate(), CodeStartLinePosInfoRecordEvent(recorder));

  // Place function entry hook if requested to do so.
  if (linkage()->GetIncomingDescriptor()->IsJSFunctionCall()) {
    ProfileEntryHookStub::MaybeCallEntryHook(masm());
  }

  // Architecture-specific, linkage-specific prologue.
  info->set_prologue_offset(masm()->pc_offset());
  AssemblePrologue();

  // Assemble all instructions.
  for (InstructionSequence::const_iterator i = code()->begin();
       i != code()->end(); ++i) {
    AssembleInstruction(*i);
  }

  FinishCode(masm());

  UpdateSafepointsWithDeoptimizationPc();
  safepoints()->Emit(masm(), frame()->GetSpillSlotCount());

  // TODO(titzer): what are the right code flags here?
  Code::Kind kind = Code::STUB;
  if (linkage()->GetIncomingDescriptor()->IsJSFunctionCall()) {
    kind = Code::OPTIMIZED_FUNCTION;
  }
  Handle<Code> result = v8::internal::CodeGenerator::MakeCodeEpilogue(
      masm(), Code::ComputeFlags(kind), info);
  result->set_is_turbofanned(true);
  result->set_stack_slots(frame()->GetSpillSlotCount());
  result->set_safepoint_table_offset(safepoints()->GetCodeOffset());

  PopulateDeoptimizationData(result);

  // Emit a code line info recording stop event.
  void* line_info = recorder->DetachJITHandlerData();
  LOG_CODE_EVENT(isolate(), CodeEndLinePosInfoRecordEvent(*result, line_info));

  return result;
}


Safepoint::Id CodeGenerator::RecordSafepoint(PointerMap* pointers,
                                             Safepoint::Kind kind,
                                             int arguments,
                                             Safepoint::DeoptMode deopt_mode) {
  const ZoneList<InstructionOperand*>* operands =
      pointers->GetNormalizedOperands();
  Safepoint safepoint =
      safepoints()->DefineSafepoint(masm(), kind, arguments, deopt_mode);
  for (int i = 0; i < operands->length(); i++) {
    InstructionOperand* pointer = operands->at(i);
    if (pointer->IsStackSlot()) {
      safepoint.DefinePointerSlot(pointer->index(), zone());
    } else if (pointer->IsRegister() && (kind & Safepoint::kWithRegisters)) {
      Register reg = Register::FromAllocationIndex(pointer->index());
      safepoint.DefinePointerRegister(reg, zone());
    }
  }
  return safepoint.id();
}


void CodeGenerator::AssembleInstruction(Instruction* instr) {
  if (instr->IsBlockStart()) {
    // Bind a label for a block start and handle parallel moves.
    BlockStartInstruction* block_start = BlockStartInstruction::cast(instr);
    current_block_ = block_start->block();
    if (FLAG_code_comments) {
      // TODO(titzer): these code comments are a giant memory leak.
      Vector<char> buffer = Vector<char>::New(32);
      SNPrintF(buffer, "-- B%d start --", block_start->block()->id());
      masm()->RecordComment(buffer.start());
    }
    masm()->bind(block_start->label());
  }
  if (instr->IsGapMoves()) {
    // Handle parallel moves associated with the gap instruction.
    AssembleGap(GapInstruction::cast(instr));
  } else if (instr->IsSourcePosition()) {
    AssembleSourcePosition(SourcePositionInstruction::cast(instr));
  } else {
    // Assemble architecture-specific code for the instruction.
    AssembleArchInstruction(instr);

    // Assemble branches or boolean materializations after this instruction.
    FlagsMode mode = FlagsModeField::decode(instr->opcode());
    FlagsCondition condition = FlagsConditionField::decode(instr->opcode());
    switch (mode) {
      case kFlags_none:
        return;
      case kFlags_set:
        return AssembleArchBoolean(instr, condition);
      case kFlags_branch:
        return AssembleArchBranch(instr, condition);
    }
    UNREACHABLE();
  }
}


void CodeGenerator::AssembleSourcePosition(SourcePositionInstruction* instr) {
  SourcePosition source_position = instr->source_position();
  if (source_position == current_source_position_) return;
  DCHECK(!source_position.IsInvalid());
  if (!source_position.IsUnknown()) {
    int code_pos = source_position.raw();
    masm()->positions_recorder()->RecordPosition(source_position.raw());
    masm()->positions_recorder()->WriteRecordedPositions();
    if (FLAG_code_comments) {
      Vector<char> buffer = Vector<char>::New(256);
      CompilationInfo* info = linkage()->info();
      int ln = Script::GetLineNumber(info->script(), code_pos);
      int cn = Script::GetColumnNumber(info->script(), code_pos);
      if (info->script()->name()->IsString()) {
        Handle<String> file(String::cast(info->script()->name()));
        base::OS::SNPrintF(buffer.start(), buffer.length(), "-- %s:%d:%d --",
                           file->ToCString().get(), ln, cn);
      } else {
        base::OS::SNPrintF(buffer.start(), buffer.length(),
                           "-- <unknown>:%d:%d --", ln, cn);
      }
      masm()->RecordComment(buffer.start());
    }
  }
  current_source_position_ = source_position;
}


void CodeGenerator::AssembleGap(GapInstruction* instr) {
  for (int i = GapInstruction::FIRST_INNER_POSITION;
       i <= GapInstruction::LAST_INNER_POSITION; i++) {
    GapInstruction::InnerPosition inner_pos =
        static_cast<GapInstruction::InnerPosition>(i);
    ParallelMove* move = instr->GetParallelMove(inner_pos);
    if (move != NULL) resolver()->Resolve(move);
  }
}


void CodeGenerator::UpdateSafepointsWithDeoptimizationPc() {
  int patch_count = static_cast<int>(lazy_deoptimization_entries_.size());
  for (int i = 0; i < patch_count; ++i) {
    LazyDeoptimizationEntry entry = lazy_deoptimization_entries_[i];
    // TODO(jarin) make sure that there is no code (other than nops)
    // between the call position and the continuation position.
    safepoints()->SetDeoptimizationPc(entry.safepoint_id(),
                                      entry.deoptimization()->pos());
  }
}


void CodeGenerator::PopulateDeoptimizationData(Handle<Code> code_object) {
  CompilationInfo* info = linkage()->info();
  int deopt_count = code()->GetDeoptimizationEntryCount();
  int patch_count = static_cast<int>(lazy_deoptimization_entries_.size());
  if (patch_count == 0 && deopt_count == 0) return;
  Handle<DeoptimizationInputData> data =
      DeoptimizationInputData::New(isolate(), deopt_count, TENURED);

  Handle<ByteArray> translation_array =
      translations_.CreateByteArray(isolate()->factory());

  data->SetTranslationByteArray(*translation_array);
  data->SetInlinedFunctionCount(Smi::FromInt(0));
  data->SetOptimizationId(Smi::FromInt(info->optimization_id()));
  // TODO(jarin) The following code was copied over from Lithium, not sure
  // whether the scope or the IsOptimizing condition are really needed.
  if (info->IsOptimizing()) {
    // Reference to shared function info does not change between phases.
    AllowDeferredHandleDereference allow_handle_dereference;
    data->SetSharedFunctionInfo(*info->shared_info());
  } else {
    data->SetSharedFunctionInfo(Smi::FromInt(0));
  }

  Handle<FixedArray> literals = isolate()->factory()->NewFixedArray(
      static_cast<int>(deoptimization_literals_.size()), TENURED);
  {
    AllowDeferredHandleDereference copy_handles;
    for (unsigned i = 0; i < deoptimization_literals_.size(); i++) {
      literals->set(i, *deoptimization_literals_[i]);
    }
    data->SetLiteralArray(*literals);
  }

  // No OSR in Turbofan yet...
  BailoutId osr_ast_id = BailoutId::None();
  data->SetOsrAstId(Smi::FromInt(osr_ast_id.ToInt()));
  data->SetOsrPcOffset(Smi::FromInt(-1));

  // Populate deoptimization entries.
  for (int i = 0; i < deopt_count; i++) {
    FrameStateDescriptor* descriptor = code()->GetDeoptimizationEntry(i);
    data->SetAstId(i, descriptor->bailout_id());
    CHECK_NE(NULL, deoptimization_states_[i]);
    data->SetTranslationIndex(
        i, Smi::FromInt(deoptimization_states_[i]->translation_id_));
    data->SetArgumentsStackHeight(i, Smi::FromInt(0));
    data->SetPc(i, Smi::FromInt(-1));
  }

  code_object->set_deoptimization_data(*data);
}


void CodeGenerator::AddSafepointAndDeopt(Instruction* instr) {
  CallDescriptor::DeoptimizationSupport deopt =
      static_cast<CallDescriptor::DeoptimizationSupport>(
          MiscField::decode(instr->opcode()));

  bool needs_frame_state = (deopt & CallDescriptor::kNeedsFrameState) != 0;

  Safepoint::Id safepoint_id = RecordSafepoint(
      instr->pointer_map(), Safepoint::kSimple, 0,
      needs_frame_state ? Safepoint::kLazyDeopt : Safepoint::kNoLazyDeopt);

  if ((deopt & CallDescriptor::kLazyDeoptimization) != 0) {
    RecordLazyDeoptimizationEntry(instr, safepoint_id);
  }

  if (needs_frame_state) {
    // If the frame state is present, it starts at argument 1
    // (just after the code address).
    InstructionOperandConverter converter(this, instr);
    // Argument 1 is deoptimization id.
    int deoptimization_id = converter.ToConstant(instr->InputAt(1)).ToInt32();
    // The actual frame state values start with argument 2.
    int first_state_value_offset = 2;
#if DEBUG
    // Make sure all the values live in stack slots or they are immediates.
    // (The values should not live in register because registers are clobbered
    // by calls.)
    FrameStateDescriptor* descriptor =
        code()->GetDeoptimizationEntry(deoptimization_id);
    for (int i = 0; i < descriptor->size(); i++) {
      InstructionOperand* op = instr->InputAt(first_state_value_offset + i);
      CHECK(op->IsStackSlot() || op->IsImmediate());
    }
#endif
    BuildTranslation(instr, first_state_value_offset, deoptimization_id);
    safepoints()->RecordLazyDeoptimizationIndex(deoptimization_id);
  }
}


void CodeGenerator::RecordLazyDeoptimizationEntry(Instruction* instr,
                                                  Safepoint::Id safepoint_id) {
  InstructionOperandConverter i(this, instr);

  Label after_call;
  masm()->bind(&after_call);

  // The continuation and deoptimization are the last two inputs:
  BasicBlock* cont_block =
      i.InputBlock(static_cast<int>(instr->InputCount()) - 2);
  BasicBlock* deopt_block =
      i.InputBlock(static_cast<int>(instr->InputCount()) - 1);

  Label* cont_label = code_->GetLabel(cont_block);
  Label* deopt_label = code_->GetLabel(deopt_block);

  lazy_deoptimization_entries_.push_back(LazyDeoptimizationEntry(
      after_call.pos(), cont_label, deopt_label, safepoint_id));
}


int CodeGenerator::DefineDeoptimizationLiteral(Handle<Object> literal) {
  int result = static_cast<int>(deoptimization_literals_.size());
  for (unsigned i = 0; i < deoptimization_literals_.size(); ++i) {
    if (deoptimization_literals_[i].is_identical_to(literal)) return i;
  }
  deoptimization_literals_.push_back(literal);
  return result;
}


void CodeGenerator::BuildTranslation(Instruction* instr,
                                     int first_argument_index,
                                     int deoptimization_id) {
  // We should build translation only once.
  DCHECK_EQ(NULL, deoptimization_states_[deoptimization_id]);

  FrameStateDescriptor* descriptor =
      code()->GetDeoptimizationEntry(deoptimization_id);
  Translation translation(&translations_, 1, 1, zone());
  translation.BeginJSFrame(descriptor->bailout_id(),
                           Translation::kSelfLiteralId,
                           descriptor->size() - descriptor->parameters_count());

  for (int i = 0; i < descriptor->size(); i++) {
    AddTranslationForOperand(&translation, instr,
                             instr->InputAt(i + first_argument_index));
  }

  deoptimization_states_[deoptimization_id] =
      new (zone()) DeoptimizationState(translation.index());
}


void CodeGenerator::AddTranslationForOperand(Translation* translation,
                                             Instruction* instr,
                                             InstructionOperand* op) {
  if (op->IsStackSlot()) {
    translation->StoreStackSlot(op->index());
  } else if (op->IsDoubleStackSlot()) {
    translation->StoreDoubleStackSlot(op->index());
  } else if (op->IsRegister()) {
    InstructionOperandConverter converter(this, instr);
    translation->StoreRegister(converter.ToRegister(op));
  } else if (op->IsDoubleRegister()) {
    InstructionOperandConverter converter(this, instr);
    translation->StoreDoubleRegister(converter.ToDoubleRegister(op));
  } else if (op->IsImmediate()) {
    InstructionOperandConverter converter(this, instr);
    Constant constant = converter.ToConstant(op);
    Handle<Object> constant_object;
    switch (constant.type()) {
      case Constant::kInt32:
        constant_object =
            isolate()->factory()->NewNumberFromInt(constant.ToInt32());
        break;
      case Constant::kFloat64:
        constant_object =
            isolate()->factory()->NewHeapNumber(constant.ToFloat64());
        break;
      case Constant::kHeapObject:
        constant_object = constant.ToHeapObject();
        break;
      default:
        UNREACHABLE();
    }
    int literal_id = DefineDeoptimizationLiteral(constant_object);
    translation->StoreLiteral(literal_id);
  } else {
    UNREACHABLE();
  }
}

#if !V8_TURBOFAN_BACKEND

void CodeGenerator::AssembleArchInstruction(Instruction* instr) {
  UNIMPLEMENTED();
}


void CodeGenerator::AssembleArchBranch(Instruction* instr,
                                       FlagsCondition condition) {
  UNIMPLEMENTED();
}


void CodeGenerator::AssembleArchBoolean(Instruction* instr,
                                        FlagsCondition condition) {
  UNIMPLEMENTED();
}


void CodeGenerator::AssemblePrologue() { UNIMPLEMENTED(); }


void CodeGenerator::AssembleReturn() { UNIMPLEMENTED(); }


void CodeGenerator::AssembleMove(InstructionOperand* source,
                                 InstructionOperand* destination) {
  UNIMPLEMENTED();
}


void CodeGenerator::AssembleSwap(InstructionOperand* source,
                                 InstructionOperand* destination) {
  UNIMPLEMENTED();
}


void CodeGenerator::AddNopForSmiCodeInlining() { UNIMPLEMENTED(); }

#endif  // !V8_TURBOFAN_BACKEND

}  // namespace compiler
}  // namespace internal
}  // namespace v8
