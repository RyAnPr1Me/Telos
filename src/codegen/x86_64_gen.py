"""x86-64 machine code generator for the Telos compiler.

Translates a ``FunctionPlan`` (produced by the optimizer) directly into
x86-64 machine code bytes using the System V AMD64 ABI (Linux/macOS).

Calling convention
------------------
* Integer arguments: RDI, RSI, RDX, RCX, R8, R9 (up to 6 params).
* Return value: RAX.
* Callee-saved registers that we use: RBP only (via prologue/epilogue).
* All arithmetic is done in RAX/RCX — both caller-saved.

Stack layout (negative offsets from RBP)
-----------------------------------------
Each parameter and local variable occupies an 8-byte slot at a fixed
negative offset from RBP.  A pre-scan of the FunctionPlan collects every
variable name and assigns slots before emitting any code.

Expression evaluation
---------------------
``emit_expr`` evaluates an IRExpr and leaves the result in RAX.  Binary
operations use PUSH RAX / POP RCX to preserve the left-hand side while
evaluating the right-hand side:

    emit_expr(left)   → rax = left
    PUSH RAX          → stack saves left
    emit_expr(right)  → rax = right
    POP  RCX          → rcx = left
    <op> rax, rcx     → rax = left op right
"""

from __future__ import annotations

import struct
from typing import Dict, List, Optional

from ..ir.nodes import (
    IRBinOp,
    IRCall,
    IRConst,
    IRExpr,
    IRUnaryOp,
    IRVar,
)
from ..ir.simplify import simplify
from ..optimizer.plans import (
    AssignPlan,
    ClosedFormPlan,
    ConstantPlan,
    ExecutionPlan,
    FunctionPlan,
    LoopPlan,
)


class X86_64Error(Exception):
    pass


# System V AMD64 ABI parameter registers: (lower-3-bit reg code, needs REX.R)
_PARAM_REGS: List[tuple] = [
    (7, False),  # rdi
    (6, False),  # rsi
    (2, False),  # rdx
    (1, False),  # rcx
    (0, True),   # r8  (REX.R extends reg field to 8)
    (1, True),   # r9  (REX.R extends reg field to 9)
]

# Accumulation operator → (rax op= rcx) encoding
# After loading: rax = accumulator, rcx = body value
_ACC_OP_BYTES: Dict[str, bytes] = {
    "add": bytes([0x48, 0x01, 0xC8]),          # add rax, rcx
    "mul": bytes([0x48, 0x0F, 0xAF, 0xC1]),    # imul rax, rcx
    "sub": bytes([0x48, 0x29, 0xC8]),           # sub rax, rcx  (acc - body)
}


class X86_64Generator:
    """Emits x86-64 machine code bytes for a single FunctionPlan."""

    def generate(self, plan: FunctionPlan) -> bytes:
        """Return machine code bytes implementing *plan*."""
        self._code: bytearray = bytearray()
        self._var_slots: Dict[str, int] = {}   # name → rbp-relative offset (neg)
        self._next_slot: int = 0               # grows downward in bytes

        # Assign stack slots: parameters first, then locals
        for name in plan.param_names:
            self._alloc_slot(name)
        for step in plan.steps:
            self._prescan(step)

        # Total frame size, rounded up to a multiple of 16
        frame_size = ((self._next_slot + 15) // 16) * 16

        # ------------------------------------------------------------------
        # Prologue
        # ------------------------------------------------------------------
        self._push_rbp()
        self._mov_rbp_rsp()
        if frame_size:
            self._sub_rsp(frame_size)

        # Save parameters from ABI registers into their stack slots
        for idx, name in enumerate(plan.param_names):
            if idx >= len(_PARAM_REGS):
                raise X86_64Error(
                    f"Function has more than {len(_PARAM_REGS)} parameters "
                    f"(unsupported)"
                )
            reg_code, needs_rex_r = _PARAM_REGS[idx]
            self._store_reg_to_slot(reg_code, needs_rex_r, self._var_slots[name])

        # ------------------------------------------------------------------
        # Steps
        # ------------------------------------------------------------------
        for step in plan.steps:
            self._emit_step(step)

        # ------------------------------------------------------------------
        # Return expression
        # ------------------------------------------------------------------
        if plan.return_expr is not None:
            self._emit_expr(simplify(plan.return_expr))
        else:
            self._xor_rax_rax()

        # ------------------------------------------------------------------
        # Epilogue
        # ------------------------------------------------------------------
        if frame_size:
            self._add_rsp(frame_size)
        self._pop_rbp()
        self._ret()

        return bytes(self._code)

    # ------------------------------------------------------------------
    # Slot allocation helpers
    # ------------------------------------------------------------------

    def _alloc_slot(self, name: str) -> int:
        self._next_slot += 8
        offset = -self._next_slot
        self._var_slots[name] = offset
        return offset

    def _get_or_alloc(self, name: str) -> int:
        if name not in self._var_slots:
            return self._alloc_slot(name)
        return self._var_slots[name]

    def _prescan(self, step: ExecutionPlan) -> None:
        """Ensure all variables referenced by *step* have stack slots."""
        if isinstance(step, (ConstantPlan, ClosedFormPlan)):
            self._get_or_alloc(step.accumulator)
        elif isinstance(step, AssignPlan):
            self._get_or_alloc(step.var)
        elif isinstance(step, LoopPlan):
            self._get_or_alloc(step.accumulator)
            self._get_or_alloc(step.loop_var)
            self._get_or_alloc(f"_end_{step.loop_var}")

    # ------------------------------------------------------------------
    # Step emitters
    # ------------------------------------------------------------------

    def _emit_step(self, step: ExecutionPlan) -> None:
        if isinstance(step, ConstantPlan):
            self._mov_rax_imm(int(step.value))
            self._store_rax_to_slot(self._var_slots[step.accumulator])

        elif isinstance(step, ClosedFormPlan):
            self._emit_expr(simplify(step.formula))
            self._store_rax_to_slot(self._var_slots[step.accumulator])

        elif isinstance(step, AssignPlan):
            self._emit_expr(simplify(step.expr))
            self._store_rax_to_slot(self._var_slots[step.var])

        elif isinstance(step, LoopPlan):
            self._emit_loop(step)

    def _emit_loop(self, step: LoopPlan) -> None:
        """Emit an iterative loop for a reduction that could not be optimised."""
        acc_slot = self._var_slots[step.accumulator]
        i_slot   = self._var_slots[step.loop_var]
        end_slot = self._var_slots[f"_end_{step.loop_var}"]

        # acc = init_val
        self._emit_expr(step.init_val)
        self._store_rax_to_slot(acc_slot)

        # i = start
        self._emit_expr(step.start)
        self._store_rax_to_slot(i_slot)

        # _end = end  (evaluated once, stored)
        self._emit_expr(step.end)
        self._store_rax_to_slot(end_slot)

        # ---- loop top ----
        loop_top = len(self._code)

        # Compare i (from slot) with _end (from slot)
        self._load_slot_to_rax(i_slot)     # rax = i
        self._push_rax()
        self._load_slot_to_rax(end_slot)   # rax = end
        self._pop_rcx()                    # rcx = i
        # cmp rcx, rax  → sets flags for (rcx - rax) = (i - end)
        # REX.W 39 /r; r=rax(0), rm=rcx(1): ModRM = 11 000 001 = 0xC1
        self._emit(0x48, 0x39, 0xC1)

        # jge loop_end  (if i >= end, exit)
        self._emit(0x0F, 0x8D)
        jge_patch = len(self._code)
        self._emit_i32(0)

        # Evaluate body (loop variable is accessible via its slot)
        self._emit_expr(step.body)

        # acc op= body:  rax = acc, rcx = body → rax = acc op body
        # Save body in rcx
        self._emit(0x48, 0x89, 0xC1)          # mov rcx, rax
        self._load_slot_to_rax(acc_slot)       # rax = acc
        acc_op = _ACC_OP_BYTES.get(step.op)
        if acc_op is None:
            raise X86_64Error(f"Unsupported reduction op: {step.op!r}")
        self._code.extend(acc_op)              # rax op= rcx
        self._store_rax_to_slot(acc_slot)

        # i += 1  (ADD qword [rbp + i_slot], 1)
        self._add_slot_imm8(i_slot, 1)

        # jmp loop_top
        self._emit(0xE9)
        jmp_patch = len(self._code)
        self._emit_i32(0)

        # ---- loop end ----
        loop_end = len(self._code)

        # Patch the jge and jmp offsets
        self._patch_i32(jge_patch, loop_end - (jge_patch + 4))
        self._patch_i32(jmp_patch, loop_top  - (jmp_patch + 4))

        # Load accumulator into rax so the variable slot is updated
        self._load_slot_to_rax(acc_slot)
        self._store_rax_to_slot(acc_slot)

    # ------------------------------------------------------------------
    # Expression emitter
    # ------------------------------------------------------------------

    def _emit_expr(self, expr: IRExpr) -> None:
        """Evaluate *expr*; result in RAX."""
        if isinstance(expr, IRConst):
            self._mov_rax_imm(int(expr.value))

        elif isinstance(expr, IRVar):
            slot = self._var_slots.get(expr.name)
            if slot is None:
                raise X86_64Error(f"Unknown variable: {expr.name!r}")
            self._load_slot_to_rax(slot)

        elif isinstance(expr, IRBinOp):
            self._emit_expr(expr.left)
            self._push_rax()             # save left
            self._emit_expr(expr.right)  # rax = right
            self._pop_rcx()              # rcx = left
            self._emit_binop(expr.op)    # rax = left op right

        elif isinstance(expr, IRUnaryOp):
            self._emit_expr(expr.operand)
            if expr.op == "-":
                # NEG RAX: REX.W F7 /3; rm=rax(0): ModRM = 11 011 000 = 0xD8
                self._emit(0x48, 0xF7, 0xD8)
            elif expr.op == "!":
                # test rax, rax; setz al; movzx rax, al
                self._emit(0x48, 0x85, 0xC0)        # test rax, rax
                self._emit(0x0F, 0x94, 0xC0)        # setz al
                self._emit(0x48, 0x0F, 0xB6, 0xC0)  # movzx rax, al
            else:
                raise X86_64Error(f"Unsupported unary op: {expr.op!r}")

        elif isinstance(expr, IRCall):
            raise X86_64Error(
                "Function calls inside expressions are not yet supported "
                "by the x86-64 backend"
            )

        else:
            raise X86_64Error(
                f"Unsupported IR expression type: {type(expr).__name__}"
            )

    def _emit_binop(self, op: str) -> None:
        """Emit code for: rax = rcx (left) <op> rax (right)."""
        if op == "+":
            # add rax, rcx: REX.W 01 /r; r=rcx(001), rm=rax(000): 48 01 C8
            self._emit(0x48, 0x01, 0xC8)

        elif op == "-":
            # left - right = rcx - rax
            # sub rcx, rax: REX.W 29 /r; r=rax(000), rm=rcx(001): 48 29 C1
            self._emit(0x48, 0x29, 0xC1)
            # mov rax, rcx: REX.W 89 /r; r=rcx(001), rm=rax(000): 48 89 C8
            self._emit(0x48, 0x89, 0xC8)

        elif op == "*":
            # imul rax, rcx: REX.W 0F AF /r; r=rax(000), rm=rcx(001): 48 0F AF C1
            self._emit(0x48, 0x0F, 0xAF, 0xC1)

        elif op in ("/", "//"):
            # signed integer division: left // right = rcx // rax
            # Step 1: xchg rax, rcx  (rax=dividend, rcx=divisor)
            #   XCHG r/m64, r64: REX.W 87 /r; r=rcx(001), rm=rax(000): 48 87 C8
            self._emit(0x48, 0x87, 0xC8)
            # Step 2: cqo (sign-extend rax into rdx:rax)
            self._emit(0x48, 0x99)
            # Step 3: idiv rcx (quotient → rax, remainder → rdx)
            #   REX.W F7 /7; rm=rcx(001): ModRM = 11 111 001 = 0xF9
            self._emit(0x48, 0xF7, 0xF9)

        elif op == "%":
            # signed remainder: left % right = rcx % rax → rdx after idiv
            self._emit(0x48, 0x87, 0xC8)  # xchg rax, rcx
            self._emit(0x48, 0x99)         # cqo
            self._emit(0x48, 0xF7, 0xF9)  # idiv rcx
            # mov rax, rdx: REX.W 89 /r; r=rdx(010), rm=rax(000): 48 89 D0
            self._emit(0x48, 0x89, 0xD0)

        elif op in ("<", ">", "<=", ">=", "==", "!="):
            # cmp rcx, rax (flags based on rcx - rax)
            # CMP r/m64, r64: REX.W 39 /r; r=rax(000), rm=rcx(001): 48 39 C1
            self._emit(0x48, 0x39, 0xC1)
            self._emit_setcc(op)

        else:
            raise X86_64Error(f"Unsupported binary op: {op!r}")

    def _emit_setcc(self, op: str) -> None:
        """Emit SETcc AL + MOVZX RAX, AL after a CMP of (rcx - rax)."""
        # SETcc opcodes (0F XX) for comparing rcx against rax (rcx - rax)
        cc_byte = {
            "<":  0x9C,   # SETL  (SF≠OF, i.e., rcx < rax)
            ">":  0x9F,   # SETG  (ZF=0 and SF=OF, i.e., rcx > rax)
            "<=": 0x9E,   # SETLE (ZF=1 or SF≠OF)
            ">=": 0x9D,   # SETGE (SF=OF)
            "==": 0x94,   # SETE  (ZF=1)
            "!=": 0x95,   # SETNE (ZF=0)
        }[op]
        self._emit(0x0F, cc_byte, 0xC0)          # setcc al
        self._emit(0x48, 0x0F, 0xB6, 0xC0)       # movzx rax, al

    # ------------------------------------------------------------------
    # Low-level instruction emitters
    # ------------------------------------------------------------------

    def _push_rbp(self) -> None:
        self._emit(0x55)

    def _pop_rbp(self) -> None:
        self._emit(0x5D)

    def _mov_rbp_rsp(self) -> None:
        # MOV RBP, RSP: REX.W 89 /r; r=rsp(100), rm=rbp(101): 48 89 E5
        self._emit(0x48, 0x89, 0xE5)

    def _sub_rsp(self, n: int) -> None:
        if n <= 127:
            # SUB RSP, imm8: REX.W 83 /5; rm=rsp(100): ModRM = 11 101 100 = 0xEC
            self._emit(0x48, 0x83, 0xEC, n)
        else:
            # SUB RSP, imm32: REX.W 81 /5: 48 81 EC <imm32>
            self._emit(0x48, 0x81, 0xEC)
            self._emit_i32(n)

    def _add_rsp(self, n: int) -> None:
        if n <= 127:
            # ADD RSP, imm8: REX.W 83 /0; rm=rsp(100): ModRM = 11 000 100 = 0xC4
            self._emit(0x48, 0x83, 0xC4, n)
        else:
            # ADD RSP, imm32: REX.W 81 /0: 48 81 C4 <imm32>
            self._emit(0x48, 0x81, 0xC4)
            self._emit_i32(n)

    def _ret(self) -> None:
        self._emit(0xC3)

    def _push_rax(self) -> None:
        self._emit(0x50)

    def _pop_rcx(self) -> None:
        self._emit(0x59)

    def _xor_rax_rax(self) -> None:
        # XOR RAX, RAX: REX.W 31 /r; r=rax(0), rm=rax(0): 48 31 C0
        self._emit(0x48, 0x31, 0xC0)

    def _mov_rax_imm(self, value: int) -> None:
        """Move a 64-bit integer constant into RAX."""
        if -(2**31) <= value <= 2**31 - 1:
            # MOV RAX, imm32 (sign-extended): REX.W C7 /0; rm=rax(0): 48 C7 C0
            self._emit(0x48, 0xC7, 0xC0)
            self._emit_i32(value)
        else:
            # MOV RAX, imm64: REX.W B8+0: 48 B8 <imm64>
            self._emit(0x48, 0xB8)
            self._emit_i64(value)

    def _store_reg_to_slot(
        self, reg_code: int, needs_rex_r: bool, offset: int
    ) -> None:
        """MOV [RBP + offset], reg  — save an ABI parameter register to stack."""
        rex = 0x48 | (0x04 if needs_rex_r else 0x00)
        self._emit(rex, 0x89)
        self._emit_modrm_rbp(reg_code, offset)

    def _load_slot_to_rax(self, offset: int) -> None:
        """MOV RAX, [RBP + offset]."""
        # MOV r64, r/m64: REX.W 8B /r; reg=rax(000)
        self._emit(0x48, 0x8B)
        self._emit_modrm_rbp(0, offset)

    def _store_rax_to_slot(self, offset: int) -> None:
        """MOV [RBP + offset], RAX."""
        # MOV r/m64, r64: REX.W 89 /r; reg=rax(000)
        self._emit(0x48, 0x89)
        self._emit_modrm_rbp(0, offset)

    def _add_slot_imm8(self, offset: int, imm: int) -> None:
        """ADD qword [RBP + offset], imm8  — used to increment loop variable."""
        # ADD r/m64, imm8: REX.W 83 /0
        self._emit(0x48, 0x83)
        self._emit_modrm_rbp(0, offset)
        self._emit(imm & 0xFF)

    # ------------------------------------------------------------------
    # ModRM encoding for [RBP + offset]
    # ------------------------------------------------------------------

    def _emit_modrm_rbp(self, reg: int, offset: int) -> None:
        """Emit ModRM byte + displacement for [RBP + offset]."""
        if -128 <= offset <= 127:
            # mod=01 (disp8), rm=rbp(101)
            modrm = (0b01 << 6) | ((reg & 7) << 3) | 0b101
            self._emit(modrm)
            self._emit(offset & 0xFF)
        else:
            # mod=10 (disp32), rm=rbp(101)
            modrm = (0b10 << 6) | ((reg & 7) << 3) | 0b101
            self._emit(modrm)
            self._emit_i32(offset)

    # ------------------------------------------------------------------
    # Raw byte helpers
    # ------------------------------------------------------------------

    def _emit(self, *bytes_: int) -> None:
        self._code.extend(bytes_)

    def _emit_i32(self, value: int) -> None:
        self._code.extend(struct.pack("<i", value))

    def _emit_i64(self, value: int) -> None:
        self._code.extend(struct.pack("<q", value))

    def _patch_i32(self, pos: int, value: int) -> None:
        """Overwrite 4 bytes at *pos* with the little-endian encoding of *value*."""
        self._code[pos : pos + 4] = struct.pack("<i", value)


def generate_x86_64(plan: FunctionPlan) -> bytes:
    """Convenience wrapper: return x86-64 machine code bytes for *plan*."""
    return X86_64Generator().generate(plan)
