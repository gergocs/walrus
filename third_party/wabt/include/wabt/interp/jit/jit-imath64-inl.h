/*
 * Copyright 2020 WebAssembly Community Group participants
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/* Only included by jit-backend.cc */

static void emitImmediate(sljit_compiler* compiler, Instruction* instr) {
  Operand* result = instr->operands();

  if (result->location.type == Operand::Unused) {
    return;
  }

  JITArg dst;
  operandToArg(result, dst);

  sljit_s32 opcode = SLJIT_MOV;

  if ((result->location.value_info & LocationInfo::kSizeMask) == 1) {
    opcode = SLJIT_MOV32;
  }

  sljit_sw imm = static_cast<sljit_sw>(instr->value().value64);
  sljit_emit_op1(compiler, opcode, dst.arg, dst.argw, SLJIT_IMM, imm);
}

static void emitLocalMove(sljit_compiler* compiler, Instruction* instr) {
  Operand* operand = instr->operands();

  if (operand->location.type == Operand::Unused) {
    assert(!(instr->info() & Instruction::kKeepInstruction));
    return;
  }

  assert(instr->info() & Instruction::kKeepInstruction);

  JITArg src, dst;

  if (instr->opcode() == Opcode::LocalGet) {
    operandToArg(operand, dst);
    src.arg = SLJIT_MEM1(kFrameReg);
    src.argw = static_cast<sljit_sw>(instr->value().value);
  } else {
    dst.arg = SLJIT_MEM1(kFrameReg);
    dst.argw = static_cast<sljit_sw>(instr->value().value);
    operandToArg(operand, src);
  }

  sljit_s32 opcode = SLJIT_MOV;

  if ((operand->location.value_info & LocationInfo::kSizeMask) == 1) {
    opcode = SLJIT_MOV32;
  }

  sljit_emit_op1(compiler, opcode, dst.arg, dst.argw, src.arg, src.argw);
}

enum DivRemOptions : sljit_s32 {
  DivRem32 = 1 << 1,
  DivRemSigned = 1 << 0,
  DivRemRemainder = 2 << 1,
};

static void emitDivRem(sljit_compiler* compiler,
                       sljit_s32 opcode,
                       JITArg* args,
                       sljit_s32 options) {
  CompileContext* context = CompileContext::get(compiler);

  if ((args[1].arg & SLJIT_IMM) && args[1].argw == 0) {
    sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R2, 0, SLJIT_IMM,
                   ExecutionContext::DivisionError);
    sljit_set_label(sljit_emit_jump(compiler, SLJIT_JUMP), context->trap_label);
    return;
  }

  sljit_s32 mov_opcode = (options & DivRem32) ? SLJIT_MOV32 : SLJIT_MOV;
  MOVE_TO_REG(compiler, mov_opcode, SLJIT_R1, args[1].arg, args[1].argw);
  MOVE_TO_REG(compiler, mov_opcode, SLJIT_R0, args[0].arg, args[0].argw);

  if (args[1].arg & SLJIT_IMM) {
    if ((options & DivRemSigned) && args[1].argw == -1) {
      sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R2, 0, SLJIT_IMM,
                     ExecutionContext::DivisionError);

      sljit_s32 type = SLJIT_EQUAL;
      sljit_sw min = static_cast<sljit_sw>(INT64_MIN);

      if (options & DivRem32) {
        type |= SLJIT_32;
        min = static_cast<sljit_sw>(INT32_MIN);
      }

      sljit_jump* cmp =
          sljit_emit_cmp(compiler, type, SLJIT_R1, 0, SLJIT_IMM, min);
      sljit_set_label(cmp, context->trap_label);
    }
  } else if (options & DivRemSigned) {
    sljit_s32 add_opcode = (options & DivRem32) ? SLJIT_ADD32 : SLJIT_ADD;
    sljit_s32 sub_opcode = (options & DivRem32) ? SLJIT_SUB32 : SLJIT_SUB;

    sljit_emit_op2(compiler, add_opcode, SLJIT_R1, 0, SLJIT_R1, 0, SLJIT_IMM,
                   1);
    sljit_emit_op2u(compiler, sub_opcode | SLJIT_SET_LESS_EQUAL | SLJIT_SET_Z,
                    SLJIT_R1, 0, SLJIT_IMM, 1);

    sljit_jump* jump_from = sljit_emit_jump(compiler, SLJIT_LESS_EQUAL);
    sljit_label* resume_label = sljit_emit_label(compiler);

    SlowCase::Type type = SlowCase::Type::SignedDivide;
    if (options & DivRem32) {
      type = SlowCase::Type::SignedDivide32;
    }

    context->add(new SlowCase(type, jump_from, resume_label, nullptr));

    sljit_emit_op2(compiler, sub_opcode, SLJIT_R1, 0, SLJIT_R1, 0, SLJIT_IMM,
                   1);
  } else {
    sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R2, 0, SLJIT_IMM,
                   ExecutionContext::DivisionError);

    sljit_s32 type = SLJIT_EQUAL;
    if (options & DivRem32) {
      type |= SLJIT_32;
    }

    sljit_jump* cmp = sljit_emit_cmp(compiler, type, SLJIT_R1, 0, SLJIT_IMM, 0);
    sljit_set_label(cmp, context->trap_label);
  }

  sljit_emit_op0(compiler, opcode);

  sljit_s32 result_reg = (options & DivRemRemainder) ? SLJIT_R1 : SLJIT_R0;
  MOVE_FROM_REG(compiler, mov_opcode, args[2].arg, args[2].argw, result_reg);
}

static void emitBinary(sljit_compiler* compiler, Instruction* instr) {
  Operand* operands = instr->operands();
  JITArg args[3];

  for (int i = 0; i < 3; ++i) {
    operandToArg(operands + i, args[i]);
  }

  sljit_s32 opcode;

  switch (instr->opcode()) {
    case Opcode::I32Add:
      opcode = SLJIT_ADD32;
      break;
    case Opcode::I32Sub:
      opcode = SLJIT_SUB32;
      break;
    case Opcode::I32Mul:
      opcode = SLJIT_MUL32;
      break;
    case Opcode::I32DivS:
      emitDivRem(compiler, SLJIT_DIV_S32, args, DivRem32 | DivRemSigned);
      return;
    case Opcode::I32DivU:
      emitDivRem(compiler, SLJIT_DIV_U32, args, DivRem32);
      return;
    case Opcode::I32RemS:
      emitDivRem(compiler, SLJIT_DIVMOD_S32, args,
                 DivRem32 | DivRemSigned | DivRemRemainder);
      return;
    case Opcode::I32RemU:
      emitDivRem(compiler, SLJIT_DIVMOD_U32, args,
                 DivRem32 | DivRemSigned | DivRemRemainder);
      return;
    case Opcode::I32Rotl:
      opcode = SLJIT_ROTL32;
      break;
    case Opcode::I32Rotr:
      opcode = SLJIT_ROTR32;
      break;
    case Opcode::I32And:
      opcode = SLJIT_AND32;
      break;
    case Opcode::I32Or:
      opcode = SLJIT_OR32;
      break;
    case Opcode::I32Xor:
      opcode = SLJIT_XOR32;
      break;
    case Opcode::I32Shl:
      opcode = SLJIT_SHL32;
      break;
    case Opcode::I32ShrS:
      opcode = SLJIT_ASHR32;
      break;
    case Opcode::I32ShrU:
      opcode = SLJIT_LSHR32;
      break;
    case Opcode::I64Add:
      opcode = SLJIT_ADD;
      break;
    case Opcode::I64Sub:
      opcode = SLJIT_SUB;
      break;
    case Opcode::I64Mul:
      opcode = SLJIT_MUL;
      break;
    case Opcode::I64DivS:
      emitDivRem(compiler, SLJIT_DIV_SW, args, DivRemSigned);
      return;
    case Opcode::I64DivU:
      emitDivRem(compiler, SLJIT_DIV_UW, args, 0);
      return;
    case Opcode::I64RemS:
      emitDivRem(compiler, SLJIT_DIVMOD_SW, args,
                 DivRemSigned | DivRemRemainder);
      return;
    case Opcode::I64RemU:
      emitDivRem(compiler, SLJIT_DIVMOD_UW, args,
                 DivRemSigned | DivRemRemainder);
      return;
    case Opcode::I64Rotl:
      opcode = SLJIT_ROTL;
      break;
    case Opcode::I64Rotr:
      opcode = SLJIT_ROTR;
      break;
    case Opcode::I64And:
      opcode = SLJIT_AND;
      break;
    case Opcode::I64Or:
      opcode = SLJIT_OR;
      break;
    case Opcode::I64Xor:
      opcode = SLJIT_XOR;
      break;
    case Opcode::I64Shl:
      opcode = SLJIT_SHL;
      break;
    case Opcode::I64ShrS:
      opcode = SLJIT_ASHR;
      break;
    case Opcode::I64ShrU:
      opcode = SLJIT_LSHR;
      break;
    default:
      WABT_UNREACHABLE;
      break;
  }

  sljit_emit_op2(compiler, opcode, args[2].arg, args[2].argw, args[0].arg,
                 args[0].argw, args[1].arg, args[1].argw);
}

static void emitExtend(sljit_compiler* compiler,
                       sljit_s32 opcode,
                       sljit_s32 big_endian_increase,
                       JITArg* args) {
  sljit_s32 reg = GET_TARGET_REG(args[1].arg, SLJIT_R0);

  assert((args[0].arg >> 8) == 0);
#if (defined SLJIT_BIG_ENDIAN && SLJIT_BIG_ENDIAN)
  if (args[0].arg & SLJIT_MEM) {
    args[0].argw += big_endian_increase;
  }
#endif /* SLJIT_BIG_ENDIAN */

  sljit_emit_op1(compiler, opcode, reg, 0, args[0].arg, args[0].argw);

  sljit_s32 mov_opcode = (big_endian_increase < 4) ? SLJIT_MOV32 : SLJIT_MOV;
  MOVE_FROM_REG(compiler, mov_opcode, args[1].arg, args[1].argw, reg);
}

static void emitUnary(sljit_compiler* compiler, Instruction* instr) {
  Operand* operands = instr->operands();
  JITArg args[2];

  for (int i = 0; i < 2; ++i) {
    operandToArg(operands + i, args[i]);
  }

  sljit_s32 opcode;

  switch (instr->opcode()) {
    case Opcode::I32Clz:
      opcode = SLJIT_CLZ32;
      break;
    case Opcode::I32Ctz:
      opcode = SLJIT_CTZ32;
      break;
    case Opcode::I64Clz:
      opcode = SLJIT_CLZ;
      break;
    case Opcode::I64Ctz:
      opcode = SLJIT_CTZ;
      break;
    case Opcode::I32Popcnt:
    case Opcode::I64Popcnt:
      // Not supported yet.
      return;
    case Opcode::I32Extend8S:
      emitExtend(compiler, SLJIT_MOV32_S8, 3, args);
      return;
    case Opcode::I32Extend16S:
      emitExtend(compiler, SLJIT_MOV32_S16, 2, args);
      return;
    case Opcode::I64Extend8S:
      emitExtend(compiler, SLJIT_MOV_S8, 7, args);
      return;
    case Opcode::I64Extend16S:
      emitExtend(compiler, SLJIT_MOV_S16, 6, args);
      return;
    case Opcode::I64Extend32S:
      emitExtend(compiler, SLJIT_MOV_S32, 4, args);
      return;
    default:
      WABT_UNREACHABLE;
      break;
  }

  // If the operand is an immediate then it is necesarry to move it into a
  // register because immediate source arguments are not supported.
  if (args[0].arg & SLJIT_IMM) {
    sljit_s32 mov = SLJIT_MOV;

    if ((operands->location.value_info & LocationInfo::kSizeMask) == 1) {
      mov = SLJIT_MOV32;
    }

    sljit_emit_op1(compiler, mov, SLJIT_R0, 0, args[0].arg, args[0].argw);
    args[0].arg = SLJIT_R0;
    args[0].argw = 0;
  }

  sljit_emit_op1(compiler, opcode, args[1].arg, args[1].argw, args[0].arg,
                 args[0].argw);
}

static bool emitCompare(sljit_compiler* compiler, Instruction* instr) {
  Operand* operand = instr->operands();
  sljit_s32 opcode, type;
  JITArg params[2];

  for (Index i = 0; i < instr->paramCount(); ++i) {
    operandToArg(operand, params[i]);
    operand++;
  }

  switch (instr->opcode()) {
    case Opcode::I32Eqz:
    case Opcode::I64Eqz:
      opcode = SLJIT_SUB | SLJIT_SET_Z;
      type = SLJIT_EQUAL;
      params[1].arg = SLJIT_IMM;
      params[1].argw = 0;
      break;
    case Opcode::I32Eq:
    case Opcode::I64Eq:
      opcode = SLJIT_SUB | SLJIT_SET_Z;
      type = SLJIT_EQUAL;
      break;
    case Opcode::I32Ne:
    case Opcode::I64Ne:
      opcode = SLJIT_SUB | SLJIT_SET_Z;
      type = SLJIT_NOT_EQUAL;
      break;
    case Opcode::I32LtS:
    case Opcode::I64LtS:
      opcode = SLJIT_SUB | SLJIT_SET_SIG_LESS;
      type = SLJIT_SIG_LESS;
      break;
    case Opcode::I32LtU:
    case Opcode::I64LtU:
      opcode = SLJIT_SUB | SLJIT_SET_LESS;
      type = SLJIT_LESS;
      break;
    case Opcode::I32GtS:
    case Opcode::I64GtS:
      opcode = SLJIT_SUB | SLJIT_SET_SIG_GREATER;
      type = SLJIT_SIG_GREATER;
      break;
    case Opcode::I32GtU:
    case Opcode::I64GtU:
      opcode = SLJIT_SUB | SLJIT_SET_GREATER;
      type = SLJIT_GREATER;
      break;
    case Opcode::I32LeS:
    case Opcode::I64LeS:
      opcode = SLJIT_SUB | SLJIT_SET_SIG_LESS_EQUAL;
      type = SLJIT_SIG_LESS_EQUAL;
      break;
    case Opcode::I32LeU:
    case Opcode::I64LeU:
      opcode = SLJIT_SUB | SLJIT_SET_LESS_EQUAL;
      type = SLJIT_LESS_EQUAL;
      break;
    case Opcode::I32GeS:
    case Opcode::I64GeS:
      opcode = SLJIT_SUB | SLJIT_SET_SIG_GREATER_EQUAL;
      type = SLJIT_SIG_GREATER_EQUAL;
      break;
    case Opcode::I32GeU:
    case Opcode::I64GeU:
      opcode = SLJIT_SUB | SLJIT_SET_GREATER_EQUAL;
      type = SLJIT_GREATER_EQUAL;
      break;
    default:
      WABT_UNREACHABLE;
      break;
  }

  if (operand->location.type != Operand::Unused) {
    if ((operand[-1].location.value_info & LocationInfo::kSizeMask) == 1) {
      opcode |= SLJIT_32;
    }

    sljit_emit_op2u(compiler, opcode, params[0].arg, params[0].argw,
                    params[1].arg, params[1].argw);
    operandToArg(operand, params[0]);
    sljit_emit_op_flags(compiler, SLJIT_MOV32, params[0].arg, params[0].argw,
                        type);
    return false;
  }

  Instruction* next_instr = instr->next()->asInstruction();

  assert(next_instr->opcode() == Opcode::BrIf ||
         next_instr->opcode() == Opcode::InterpBrUnless);

  if (next_instr->opcode() == Opcode::InterpBrUnless) {
    type ^= 0x1;
  }

  if ((operand[-1].location.value_info & LocationInfo::kSizeMask) == 1) {
    type |= SLJIT_32;
  }

  sljit_jump* jump =
      sljit_emit_cmp(compiler, type, params[0].arg, params[0].argw,
                     params[1].arg, params[1].argw);
  next_instr->value().target_label->jumpFrom(jump);
  return true;
}

static void emitConvert(sljit_compiler* compiler, Instruction* instr) {
  Operand* operands = instr->operands();
  JITArg args[2];

  for (int i = 0; i < 2; ++i) {
    operandToArg(operands + i, args[i]);
  }

  switch (instr->opcode()) {
    case Opcode::I32WrapI64:
      if (args[0].arg & SLJIT_MEM) {
        sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R0, 0, args[0].arg,
                       args[0].argw);
        sljit_emit_op1(compiler, SLJIT_MOV32, args[1].arg, args[1].argw,
                       SLJIT_R0, 0);
      } else {
        sljit_emit_op1(compiler, SLJIT_MOV32, args[1].arg, args[1].argw,
                       args[0].arg, args[0].argw);
      }
      return;
    case Opcode::I64ExtendI32S:
      if (!(args[0].arg & SLJIT_MEM)) {
        sljit_emit_op1(compiler, SLJIT_MOV_S32, args[1].arg, args[1].argw,
                       args[0].arg, args[0].argw);
      } else {
        sljit_emit_op1(compiler, SLJIT_MOV_S32, SLJIT_R0, 0, args[0].arg,
                       args[0].argw);
        sljit_emit_op1(compiler, SLJIT_MOV, args[1].arg, args[1].argw, SLJIT_R0,
                       0);
      }
      return;
    case Opcode::I64ExtendI32U:
      if (!(args[0].arg & SLJIT_MEM)) {
        sljit_emit_op1(compiler, SLJIT_MOV_U32, args[1].arg, args[1].argw,
                       args[0].arg, args[0].argw);
      } else {
        sljit_emit_op1(compiler, SLJIT_MOV_U32, SLJIT_R0, 0, args[0].arg,
                       args[0].argw);
        sljit_emit_op1(compiler, SLJIT_MOV, args[1].arg, args[1].argw, SLJIT_R0,
                       0);
      }
      return;
    default:
      WABT_UNREACHABLE;
      break;
  }
}
