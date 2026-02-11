#!/usr/bin/env python3
"""Simple V810 disassembler and VIP write scanner for Virtual Boy ROMs."""

from __future__ import annotations

import argparse
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, Iterable, List, Optional, Sequence

ADDRESS_MASK_27 = 0x07FF_FFFF
DEFAULT_BASE = 0x0700_0000
RESET_VECTOR_PC = 0x07FF_FFF0


def sign_extend(value: int, bits: int) -> int:
    sign_bit = 1 << (bits - 1)
    mask = (1 << bits) - 1
    value &= mask
    return (value ^ sign_bit) - sign_bit


def parse_int(value: str) -> int:
    value = value.strip().lower()
    if value.startswith("0x"):
        return int(value, 16)
    return int(value, 10)


def parse_start(value: str) -> int:
    if value.lower() == "reset":
        return RESET_VECTOR_PC
    return parse_int(value)


def reg_name(index: int) -> str:
    return f"r{index}"


SR_NAMES = {
    0: "EIPC",
    1: "EIPSW",
    2: "FEPC",
    3: "FEPSW",
    4: "ECR",
    5: "PSW",
    6: "PIR",
    7: "TKCW",
    24: "CHCW",
    25: "ADDTRE",
}

COND_NAMES = {
    0: "v",
    1: "c",
    2: "z",
    3: "nh",
    4: "s",
    5: "t",
    6: "lt",
    7: "le",
    8: "nv",
    9: "nc",
    10: "nz",
    11: "h",
    12: "ns",
    13: "f",
    14: "ge",
    15: "gt",
}

BRANCH_MNEMONICS = {
    0x40: "bv",
    0x41: "bl",
    0x42: "be",
    0x43: "bnh",
    0x44: "bn",
    0x45: "br",
    0x46: "blt",
    0x47: "ble",
    0x48: "bnv",
    0x49: "bnl",
    0x4A: "bne",
    0x4B: "bh",
    0x4C: "bp",
    0x4D: "nop",
    0x4E: "bge",
    0x4F: "bgt",
}

BSTR_SUBOPS = {
    0x00: "sch0bsu",
    0x01: "sch0bsd",
    0x02: "sch1bsu",
    0x03: "sch1bsd",
    0x08: "orbsu",
    0x09: "andbsu",
    0x0A: "xorbsu",
    0x0B: "movbsu",
    0x0C: "ornbsu",
    0x0D: "andnbsu",
    0x0E: "xornbsu",
    0x0F: "notbsu",
}

FPP_SUBOPS = {
    0x00: "cmpf.s",
    0x02: "cvt.ws",
    0x03: "cvt.sw",
    0x04: "addf.s",
    0x05: "subf.s",
    0x06: "mulf.s",
    0x07: "divf.s",
    0x08: "xb",
    0x09: "xh",
    0x0A: "rev",
    0x0B: "trnc.sw",
    0x0C: "mpyhw",
}

VIP_REGISTER_NAMES = {
    0x00: "IPENDING (read)",
    0x02: "IENABLE",
    0x04: "IPENDING clear",
    0x22: "DPCTRL",
    0x24: "BRTA",
    0x26: "BRTB",
    0x28: "BRTC",
    0x2A: "REST",
    0x2E: "FRMCYC",
    0x42: "XPCTRL",
    0x48: "SPT0",
    0x4A: "SPT1",
    0x4C: "SPT2",
    0x4E: "SPT3",
    0x60: "GPLT0",
    0x62: "GPLT1",
    0x64: "GPLT2",
    0x66: "GPLT3",
    0x68: "JPLT0",
    0x6A: "JPLT1",
    0x6C: "JPLT2",
    0x6E: "JPLT3",
    0x70: "BKCOL",
}


@dataclass
class DecodedInstruction:
    pc: int
    length: int
    word0: int
    word1: Optional[int]
    opcode: int
    mnemonic: str
    operands: str
    target: Optional[int] = None
    mem_write_base_reg: Optional[int] = None
    mem_write_disp: Optional[int] = None
    mem_write_kind: Optional[str] = None
    mem_write_src_reg: Optional[int] = None

    @property
    def text(self) -> str:
        return f"{self.mnemonic} {self.operands}".rstrip()


class RomReader:
    def __init__(self, data: bytes, base: int) -> None:
        self.data = data
        self.size = len(data)
        if self.size == 0:
            raise ValueError("ROM is empty")
        if self.size & (self.size - 1):
            raise ValueError("ROM size must be power-of-two for mirrored V810 addressing")
        self.mask = self.size - 1
        self.base = base

    def read16(self, pc: int) -> int:
        off0 = (pc - self.base) & self.mask
        off1 = (off0 + 1) & self.mask
        return self.data[off0] | (self.data[off1] << 8)


def decode_instruction(reader: RomReader, pc: int) -> DecodedInstruction:
    w0 = reader.read16(pc)
    opcode = (w0 >> 9) & 0x7F
    arg_lo = w0 & 0x1F
    arg_hi = (w0 >> 5) & 0x1F

    word1: Optional[int] = None
    length = 2
    mnemonic = f"op_{opcode:02x}"
    operands = ""
    target: Optional[int] = None
    mem_write_base_reg: Optional[int] = None
    mem_write_disp: Optional[int] = None
    mem_write_kind: Optional[str] = None
    mem_write_src_reg: Optional[int] = None

    def ext_word() -> int:
        nonlocal word1, length
        if word1 is None:
            word1 = reader.read16((pc + 2) & 0xFFFF_FFFF)
            length = 4
        return word1

    if opcode <= 0x0F:
        m = [
            "mov", "add", "sub", "cmp", "shl", "shr", "jmp", "sar",
            "mul", "div", "mulu", "divu", "or", "and", "xor", "not",
        ][opcode]
        mnemonic = m
        if opcode == 0x06:
            operands = f"[{reg_name(arg_lo)}]"
        else:
            operands = f"{reg_name(arg_lo)}, {reg_name(arg_hi)}"

    elif opcode == 0x10:
        mnemonic = "mov"
        operands = f"{sign_extend(arg_lo, 5)}, {reg_name(arg_hi)}"
    elif opcode == 0x11:
        mnemonic = "add"
        operands = f"{sign_extend(arg_lo, 5)}, {reg_name(arg_hi)}"
    elif opcode == 0x12:
        mnemonic = "setf"
        operands = f"{COND_NAMES.get(arg_lo & 0xF, f'0x{arg_lo & 0xF:x}')}, {reg_name(arg_hi)}"
    elif opcode == 0x13:
        mnemonic = "cmp"
        operands = f"{sign_extend(arg_lo, 5)}, {reg_name(arg_hi)}"
    elif opcode == 0x14:
        mnemonic = "shl"
        operands = f"{arg_lo & 0x1F}, {reg_name(arg_hi)}"
    elif opcode == 0x15:
        mnemonic = "shr"
        operands = f"{arg_lo & 0x1F}, {reg_name(arg_hi)}"
    elif opcode == 0x16:
        mnemonic = "ei"
    elif opcode == 0x17:
        mnemonic = "sar"
        operands = f"{arg_lo & 0x1F}, {reg_name(arg_hi)}"
    elif opcode == 0x18:
        mnemonic = "trap"
        operands = f"{arg_lo & 0x1F}"
    elif opcode == 0x19:
        mnemonic = "reti"
    elif opcode == 0x1A:
        mnemonic = "halt"
    elif opcode == 0x1C:
        mnemonic = "ldsr"
        sr = SR_NAMES.get(arg_lo & 0x1F, f"sr{arg_lo & 0x1F}")
        operands = f"{reg_name(arg_hi)}, {sr}"
    elif opcode == 0x1D:
        mnemonic = "stsr"
        sr = SR_NAMES.get(arg_lo & 0x1F, f"sr{arg_lo & 0x1F}")
        operands = f"{sr}, {reg_name(arg_hi)}"
    elif opcode == 0x1E:
        mnemonic = "di"
    elif opcode == 0x1F:
        subop = arg_lo & 0x1F
        mnemonic = BSTR_SUBOPS.get(subop, f"bstr_{subop:02x}")
        if arg_hi:
            operands = f"0x{arg_hi:x}"

    elif opcode in BRANCH_MNEMONICS:
        mnemonic = BRANCH_MNEMONICS[opcode]
        if mnemonic == "nop":
            operands = ""
        else:
            disp = sign_extend(w0 & 0x1FE, 9) & ~1
            target = (pc + disp) & 0xFFFF_FFFF
            operands = f"0x{target:08X}"

    elif opcode in (0x2A, 0x2B):
        imm26 = ((w0 & 0x03FF) << 16) | ext_word()
        disp = sign_extend(imm26, 26) & ~1
        target = (pc + disp) & 0xFFFF_FFFF
        mnemonic = "jr" if opcode == 0x2A else "jal"
        operands = f"0x{target:08X}"

    elif opcode in (0x28, 0x29, 0x2C, 0x2D, 0x2E, 0x2F):
        imm16 = ext_word()
        src = arg_lo
        dst = arg_hi
        if opcode == 0x28:
            mnemonic = "movea"
            operands = f"{sign_extend(imm16, 16)}, {reg_name(src)}, {reg_name(dst)}"
        elif opcode == 0x29:
            mnemonic = "addi"
            operands = f"{sign_extend(imm16, 16)}, {reg_name(src)}, {reg_name(dst)}"
        elif opcode == 0x2C:
            mnemonic = "ori"
            operands = f"0x{imm16:04X}, {reg_name(src)}, {reg_name(dst)}"
        elif opcode == 0x2D:
            mnemonic = "andi"
            operands = f"0x{imm16:04X}, {reg_name(src)}, {reg_name(dst)}"
        elif opcode == 0x2E:
            mnemonic = "xori"
            operands = f"0x{imm16:04X}, {reg_name(src)}, {reg_name(dst)}"
        elif opcode == 0x2F:
            mnemonic = "movhi"
            operands = f"0x{imm16:04X}, {reg_name(src)}, {reg_name(dst)}"

    elif opcode in (0x30, 0x31, 0x33, 0x38, 0x39, 0x3A, 0x3B):
        imm16 = ext_word()
        base_reg = arg_lo
        dest_reg = arg_hi
        disp = sign_extend(imm16, 16)
        m = {
            0x30: "ld.b",
            0x31: "ld.h",
            0x33: "ld.w",
            0x38: "in.b",
            0x39: "in.h",
            0x3A: "caxi",
            0x3B: "in.w",
        }[opcode]
        mnemonic = m
        if opcode == 0x3A:
            operands = f"{disp}[{reg_name(base_reg)}], {reg_name(dest_reg)}"
            mem_write_base_reg = base_reg
            mem_write_disp = disp
            mem_write_kind = "caxi"
        else:
            operands = f"{disp}[{reg_name(base_reg)}], {reg_name(dest_reg)}"

    elif opcode in (0x34, 0x35, 0x37, 0x3C, 0x3D, 0x3F):
        imm16 = ext_word()
        src_reg = arg_hi
        base_reg = arg_lo
        disp = sign_extend(imm16, 16)
        m = {
            0x34: "st.b",
            0x35: "st.h",
            0x37: "st.w",
            0x3C: "out.b",
            0x3D: "out.h",
            0x3F: "out.w",
        }[opcode]
        mnemonic = m
        operands = f"{reg_name(src_reg)}, {disp}[{reg_name(base_reg)}]"
        mem_write_base_reg = base_reg
        mem_write_disp = disp
        mem_write_kind = m
        mem_write_src_reg = src_reg

    elif opcode == 0x3E:
        w1 = ext_word()
        src = arg_lo
        dst = arg_hi
        subop = (w1 >> 10) & 0x3F
        mnemonic = FPP_SUBOPS.get(subop, f"fpp_{subop:02x}")

        if mnemonic in ("xb", "xh"):
            operands = f"{reg_name(dst)}"
        elif mnemonic == "rev":
            operands = f"{reg_name(src)}, {reg_name(dst)}"
        else:
            operands = f"{reg_name(src)}, {reg_name(dst)}"

    else:
        mnemonic = ".hword"
        operands = f"0x{w0:04X}"

    return DecodedInstruction(
        pc=pc,
        length=length,
        word0=w0,
        word1=word1,
        opcode=opcode,
        mnemonic=mnemonic,
        operands=operands,
        target=target,
        mem_write_base_reg=mem_write_base_reg,
        mem_write_disp=mem_write_disp,
        mem_write_kind=mem_write_kind,
        mem_write_src_reg=mem_write_src_reg,
    )


def disassemble_range(reader: RomReader, start_pc: int, count: int) -> List[DecodedInstruction]:
    pc = start_pc & 0xFFFF_FFFF
    out: List[DecodedInstruction] = []
    for _ in range(count):
        inst = decode_instruction(reader, pc)
        out.append(inst)
        pc = (pc + inst.length) & 0xFFFF_FFFF
    return out


def build_labels(instructions: Sequence[DecodedInstruction]) -> Dict[int, str]:
    labels: Dict[int, str] = {}
    for inst in instructions:
        if inst.target is None:
            continue
        if inst.target not in labels:
            labels[inst.target] = f"L_{inst.target:08X}"
    return labels


def format_disassembly(instructions: Sequence[DecodedInstruction], labels: Dict[int, str], with_labels: bool) -> str:
    lines: List[str] = []
    for inst in instructions:
        if with_labels and inst.pc in labels:
            lines.append(f"{labels[inst.pc]}:")

        words = f"{inst.word0:04X}"
        if inst.word1 is not None:
            words += f" {inst.word1:04X}"
        words = f"{words:<10}"

        text = inst.text
        if with_labels and inst.target is not None and inst.target in labels:
            text += f" ; -> {labels[inst.target]}"

        lines.append(f"{inst.pc:08X}: {words} {text}")
    return "\n".join(lines)


def classify_vip_target(addr: int) -> str:
    low = addr & ADDRESS_MASK_27
    seg = (low >> 24) & 0xFF

    if seg == 0:
        if 0x5E000 <= low < 0x5E080:
            reg_off = (low - 0x5E000) & 0xFE
            reg_name_str = VIP_REGISTER_NAMES.get(reg_off, f"offset 0x{reg_off:02X}")
            return f"VIP Register {reg_name_str}"

        if 0x20000 <= low <= 0x3FFFF:
            off = low & 0x1FFFF
            if 0x1D800 <= off < 0x1E000:
                rel = off - 0x1D800
                world = rel // 0x20
                in_world = rel % 0x20
                word = in_world // 2
                world_fields = {
                    0: "WORLD_CTRL(L/R enable,bgm,scx/scy,end/over,bgmap_base)",
                    1: "GX",
                    2: "GP",
                    3: "GY",
                    4: "MX",
                    5: "MP",
                    6: "MY",
                    7: "WINDOW_WIDTH",
                    8: "WINDOW_HEIGHT",
                    9: "PARAM_BASE",
                    10: "OVERPLANE_CHAR",
                }
                field = world_fields.get(word, f"WORD{word}")
                return (
                    f"VIP DRAM World Table (+0x{rel:04X}) "
                    f"[world={world} word={word} {field}]"
                )
            if 0x1E000 <= off < 0x20000:
                rel = off - 0x1E000
                obj = rel // 8
                in_obj = rel % 8
                word = in_obj // 2
                obj_fields = {
                    0: "JX",
                    1: "JP_EYEMASK",
                    2: "JY",
                    3: "ATTR(char,palette,hflip,vflip)",
                }
                field = obj_fields.get(word, f"WORD{word}")
                return (
                    f"VIP DRAM OAM (+0x{rel:04X}) "
                    f"[obj={obj} word={word} byte={in_obj} {field}]"
                )
            return f"VIP DRAM (+0x{off:05X})"

        if low < 0x20000:
            return f"VIP FB/CHR (+0x{low:05X})"

        return "VIP segment"

    if seg == 1:
        return "VSU segment"
    if seg == 2:
        return "HWCTRL segment"
    if seg == 5:
        return "WRAM segment"
    if seg == 6:
        return "Cart RAM segment"
    if seg == 7:
        return "Cart ROM segment"
    return "Unmapped"


def update_known_registers(inst: DecodedInstruction, known: List[Optional[int]]) -> None:
    known[0] = 0

    def k(reg: int) -> Optional[int]:
        return known[reg]

    def set_reg(reg: int, value: Optional[int]) -> None:
        if reg == 0:
            known[0] = 0
        elif value is None:
            known[reg] = None
        else:
            known[reg] = value & 0xFFFF_FFFF

    w0 = inst.word0
    opc = inst.opcode
    a = w0 & 0x1F
    b = (w0 >> 5) & 0x1F

    if opc == 0x00:  # mov
        set_reg(b, k(a))
    elif opc == 0x01:  # add
        if k(a) is not None and k(b) is not None:
            set_reg(b, k(b) + k(a))
        else:
            set_reg(b, None)
    elif opc == 0x02:  # sub
        if k(a) is not None and k(b) is not None:
            set_reg(b, k(b) - k(a))
        else:
            set_reg(b, None)
    elif opc == 0x04:  # shl
        if k(a) is not None and k(b) is not None:
            set_reg(b, (k(b) << (k(a) & 0x1F)))
        else:
            set_reg(b, None)
    elif opc == 0x05:  # shr
        if k(a) is not None and k(b) is not None:
            set_reg(b, (k(b) & 0xFFFF_FFFF) >> (k(a) & 0x1F))
        else:
            set_reg(b, None)
    elif opc == 0x07:  # sar
        if k(a) is not None and k(b) is not None:
            v = sign_extend(k(b) & 0xFFFF_FFFF, 32)
            set_reg(b, v >> (k(a) & 0x1F))
        else:
            set_reg(b, None)
    elif opc in (0x08, 0x09, 0x0A, 0x0B):  # mul/div/mulu/divu
        set_reg(30, None)
        set_reg(b, None)
    elif opc == 0x0C:  # or
        if k(a) is not None and k(b) is not None:
            set_reg(b, k(a) | k(b))
        else:
            set_reg(b, None)
    elif opc == 0x0D:  # and
        if k(a) is not None and k(b) is not None:
            set_reg(b, k(a) & k(b))
        else:
            set_reg(b, None)
    elif opc == 0x0E:  # xor
        if k(a) is not None and k(b) is not None:
            set_reg(b, k(a) ^ k(b))
        else:
            set_reg(b, None)
    elif opc == 0x0F:  # not
        if k(a) is not None:
            set_reg(b, ~k(a))
        else:
            set_reg(b, None)
    elif opc == 0x10:  # mov imm5
        set_reg(b, sign_extend(a, 5))
    elif opc == 0x11:  # add imm5
        if k(b) is not None:
            set_reg(b, k(b) + sign_extend(a, 5))
        else:
            set_reg(b, None)
    elif opc == 0x14:  # shl imm
        if k(b) is not None:
            set_reg(b, k(b) << (a & 0x1F))
        else:
            set_reg(b, None)
    elif opc == 0x15:  # shr imm
        if k(b) is not None:
            set_reg(b, (k(b) & 0xFFFF_FFFF) >> (a & 0x1F))
        else:
            set_reg(b, None)
    elif opc == 0x17:  # sar imm
        if k(b) is not None:
            set_reg(b, sign_extend(k(b) & 0xFFFF_FFFF, 32) >> (a & 0x1F))
        else:
            set_reg(b, None)
    elif opc == 0x1D:  # stsr
        set_reg(b, None)
    elif opc in (0x2A,):  # jr
        pass
    elif opc == 0x2B:  # jal
        set_reg(31, (inst.pc + 4) & 0xFFFF_FFFF)
    elif opc in (0x28, 0x29, 0x2C, 0x2D, 0x2E, 0x2F):
        if inst.word1 is None:
            return
        imm = inst.word1
        src = a
        dst = b
        src_val = k(src)
        if opc == 0x28:  # movea
            set_reg(dst, None if src_val is None else src_val + sign_extend(imm, 16))
        elif opc == 0x29:  # addi
            set_reg(dst, None if src_val is None else src_val + sign_extend(imm, 16))
        elif opc == 0x2C:  # ori
            set_reg(dst, None if src_val is None else (src_val | imm))
        elif opc == 0x2D:  # andi
            set_reg(dst, None if src_val is None else (src_val & imm))
        elif opc == 0x2E:  # xori
            set_reg(dst, None if src_val is None else (src_val ^ imm))
        elif opc == 0x2F:  # movhi
            set_reg(dst, None if src_val is None else ((imm << 16) + src_val))
    elif opc in (0x30, 0x31, 0x33, 0x38, 0x39, 0x3A, 0x3B):  # loads/in/caxi
        set_reg(b, None)
    elif opc == 0x3E:  # FPP
        set_reg(b, None)


def is_obj_bg_related(classification: str) -> bool:
    if "OAM" in classification or "World Table" in classification:
        return True
    if classification.startswith("VIP Register "):
        return any(
            key in classification
            for key in ("SPT", "GPLT", "JPLT", "XPCTRL", "DPCTRL", "BKCOL", "BRTA", "BRTB", "BRTC", "REST", "FRMCYC")
        )
    return False


def scan_vip_writes(
    reader: RomReader,
    start_pc: int,
    include_unknown: bool,
    focus_obj_bg: bool,
) -> List[str]:
    instruction_count = max(1, reader.size // 2)
    instructions = disassemble_range(reader, start_pc, instruction_count)

    known: List[Optional[int]] = [None] * 32
    known[0] = 0

    lines: List[str] = []
    for inst in instructions:
        if inst.mem_write_base_reg is not None and inst.mem_write_disp is not None and inst.mem_write_kind is not None:
            base_value = known[inst.mem_write_base_reg]
            if base_value is not None:
                raw_addr = (base_value + inst.mem_write_disp) & 0xFFFF_FFFF
                touched: List[int]
                kind = inst.mem_write_kind
                if kind in ("st.b", "out.b"):
                    touched = [raw_addr]
                elif kind in ("st.h", "out.h"):
                    touched = [raw_addr & 0xFFFFFFFE]
                elif kind in ("st.w", "out.w", "caxi"):
                    base_aligned = raw_addr & 0xFFFFFFFC
                    touched = [base_aligned, base_aligned | 2]
                else:
                    touched = [raw_addr]

                emitted = False
                src_extra = ""
                if inst.mem_write_src_reg is not None:
                    src_reg = inst.mem_write_src_reg
                    src_val = known[src_reg]
                    if src_val is None:
                        src_extra = f" src={reg_name(src_reg)}"
                    else:
                        src_extra = f" src={reg_name(src_reg)}(0x{src_val & 0xFFFFFFFF:08X})"
                for addr in touched:
                    classification = classify_vip_target(addr)
                    if not classification.startswith("VIP"):
                        continue
                    if focus_obj_bg and not is_obj_bg_related(classification):
                        continue
                    lines.append(
                        f"{inst.pc:08X}: {inst.text:<30} ; addr=0x{addr:08X} {classification}{src_extra}"
                    )
                    emitted = True
                if not emitted and include_unknown:
                    lines.append(
                        f"{inst.pc:08X}: {inst.text:<30} ; addr(raw)=0x{raw_addr:08X} no VIP target after alignment{src_extra}"
                    )
            elif include_unknown:
                lines.append(
                    f"{inst.pc:08X}: {inst.text:<30} ; base {reg_name(inst.mem_write_base_reg)} unknown"
                )

        update_known_registers(inst, known)

    return lines


def iter_roms(path: Path) -> Iterable[Path]:
    if path.is_file():
        yield path
        return

    for file_path in sorted(path.rglob("*.vb")):
        if file_path.is_file():
            yield file_path


def run_disasm(args: argparse.Namespace) -> str:
    rom_path = Path(args.rom)
    data = rom_path.read_bytes()
    reader = RomReader(data, args.base)

    start_pc = parse_start(args.start)
    instructions = disassemble_range(reader, start_pc, args.count)
    labels = build_labels(instructions) if not args.no_labels else {}

    header = [
        f"ROM: {rom_path}",
        f"Base: 0x{args.base:08X}",
        f"Start PC: 0x{start_pc:08X}",
        f"Count: {args.count}",
        "",
    ]
    body = format_disassembly(instructions, labels, not args.no_labels)
    return "\n".join(header) + body


def run_scan_vip(args: argparse.Namespace) -> str:
    target = Path(args.target)
    outputs: List[str] = []

    roms = list(iter_roms(target))
    if not roms:
        return f"No .vb files found under: {target}"

    for rom_path in roms:
        data = rom_path.read_bytes()
        reader = RomReader(data, args.base)
        start_pc = parse_start(args.start)
        lines = scan_vip_writes(
            reader,
            start_pc,
            args.include_unknown,
            args.focus == "obj-bg",
        )

        outputs.append(f"== {rom_path} ==")
        if lines:
            if args.limit and args.limit > 0:
                lines = lines[: args.limit]
            outputs.extend(lines)
        else:
            outputs.append("(no VIP write candidates found)")
        outputs.append("")

    return "\n".join(outputs).rstrip()


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="V810 disassembler and VIP write scanner for Virtual Boy ROMs"
    )
    sub = parser.add_subparsers(dest="command", required=True)

    p_disasm = sub.add_parser("disasm", help="Disassemble a ROM from a start PC")
    p_disasm.add_argument("rom", help="Path to .vb ROM")
    p_disasm.add_argument("--start", default="reset", help="Start PC (hex/dec) or 'reset' (default)")
    p_disasm.add_argument("--count", type=int, default=512, help="Number of instructions to decode")
    p_disasm.add_argument("--base", type=parse_int, default=DEFAULT_BASE, help="ROM base address (default: 0x07000000)")
    p_disasm.add_argument("--no-labels", action="store_true", help="Disable label generation")
    p_disasm.add_argument("--output", help="Output file path")

    p_scan = sub.add_parser("scan-vip", help="Scan for VIP-related memory writes")
    p_scan.add_argument("target", help="ROM file or directory containing .vb ROM files")
    p_scan.add_argument("--start", default="reset", help="Start PC for linear decode (default: reset)")
    p_scan.add_argument("--base", type=parse_int, default=DEFAULT_BASE, help="ROM base address (default: 0x07000000)")
    p_scan.add_argument("--include-unknown", action="store_true", help="Include writes where base register value is unknown")
    p_scan.add_argument(
        "--focus",
        choices=("all", "obj-bg"),
        default="all",
        help="Filter output focus: all VIP writes, or OBJ/BG related writes only",
    )
    p_scan.add_argument("--limit", type=int, default=0, help="Max result lines per ROM (0 = unlimited)")
    p_scan.add_argument("--output", help="Output file path")

    return parser


def main() -> int:
    parser = build_parser()
    args = parser.parse_args()

    if args.command == "disasm":
        out = run_disasm(args)
    elif args.command == "scan-vip":
        out = run_scan_vip(args)
    else:
        parser.error("Unknown command")
        return 2

    if args.output:
        output_path = Path(args.output)
        output_path.parent.mkdir(parents=True, exist_ok=True)
        output_path.write_text(out, encoding="utf-8")
        print(f"Wrote: {output_path}")
    else:
        print(out)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
