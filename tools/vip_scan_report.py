#!/usr/bin/env python3
"""Build a markdown report from vb_disasm.py scan-vip outputs."""

from __future__ import annotations

import argparse
import collections
import datetime as dt
import re
from pathlib import Path
from typing import Dict, List, Tuple


ROM_HEADER_RE = re.compile(r"^==\s+(.*?)\s+==$")
OP_RE = re.compile(r":\s*([a-z0-9\.]+)\s", re.IGNORECASE)


def parse_scan(path: Path) -> Dict[str, object]:
    lines = path.read_text(encoding="utf-8", errors="replace").splitlines()

    rom: str | None = None
    rom_counts: collections.Counter[str] = collections.Counter()
    category_counts: collections.Counter[str] = collections.Counter()
    op_counts: collections.Counter[str] = collections.Counter()
    register_counts: collections.Counter[str] = collections.Counter()
    world_field_counts: collections.Counter[str] = collections.Counter()
    oam_field_counts: collections.Counter[str] = collections.Counter()

    for line in lines:
        m_header = ROM_HEADER_RE.match(line)
        if m_header:
            rom = m_header.group(1)
            continue

        if not line or line.startswith("(no VIP write candidates found)"):
            continue

        if ";" not in line:
            continue

        if rom is not None:
            rom_counts[rom] += 1

        m_op = OP_RE.search(line)
        if m_op:
            op_counts[m_op.group(1)] += 1

        vip_pos = line.find("VIP ")
        if vip_pos < 0:
            continue
        classification = line[vip_pos:]

        if classification.startswith("VIP Register "):
            category_counts["VIP Register"] += 1
            register_name = classification.replace("VIP Register ", "", 1).strip()
            register_counts[register_name] += 1
            continue

        if classification.startswith("VIP DRAM World Table"):
            category_counts["VIP DRAM World Table"] += 1
            m_world = re.search(r"\s([A-Z_]+(?:\([^)]*\))?|WORD\d+)\]\s*(?:src=|$)", line)
            if m_world:
                world_field_counts[m_world.group(1)] += 1
            continue

        if classification.startswith("VIP DRAM OAM"):
            category_counts["VIP DRAM OAM"] += 1
            m_oam = re.search(r"\s(JX|JP_EYEMASK|JY|ATTR\([^)]*\)|WORD\d+)\]\s*(?:src=|$)", line)
            if m_oam:
                oam_field_counts[m_oam.group(1)] += 1
            continue

        if classification.startswith("VIP FB/CHR"):
            category_counts["VIP FB/CHR"] += 1
            continue

        if classification.startswith("VIP DRAM"):
            category_counts["VIP DRAM (other)"] += 1
            continue

        category_counts["VIP other"] += 1

    return {
        "path": str(path),
        "rom_counts": rom_counts,
        "category_counts": category_counts,
        "op_counts": op_counts,
        "register_counts": register_counts,
        "world_field_counts": world_field_counts,
        "oam_field_counts": oam_field_counts,
        "total": sum(rom_counts.values()),
    }


def fmt_top(counter: collections.Counter[str], n: int) -> List[Tuple[str, int]]:
    return counter.most_common(n)


def emit_table(items: List[Tuple[str, int]], headers: Tuple[str, str]) -> str:
    out = [f"| {headers[0]} | {headers[1]} |", "|---|---:|"]
    if not items:
        out.append("| (none) | 0 |")
    else:
        for k, v in items:
            out.append(f"| `{k}` | {v} |")
    return "\n".join(out)


def build_report(scan_all: Dict[str, object], scan_objbg: Dict[str, object], target_dir: str) -> str:
    date_str = dt.datetime.now().strftime("%Y-%m-%d %H:%M:%S")

    all_rom_counts = scan_all["rom_counts"]
    all_cat = scan_all["category_counts"]
    all_ops = scan_all["op_counts"]
    all_regs = scan_all["register_counts"]

    objbg_rom_counts = scan_objbg["rom_counts"]
    objbg_ops = scan_objbg["op_counts"]
    objbg_world = scan_objbg["world_field_counts"]
    objbg_oam = scan_objbg["oam_field_counts"]

    lines: List[str] = []
    lines.append("# VIP Operations Dump Report (All Extracted ROMs)")
    lines.append("")
    lines.append(f"- Generated: `{date_str}`")
    lines.append(f"- Target ROM directory: `{target_dir}`")
    lines.append(f"- Full scan file: `{scan_all['path']}`")
    lines.append(f"- OBJ/BG-focused scan file: `{scan_objbg['path']}`")
    lines.append("")
    lines.append("## How This Dump Was Produced")
    lines.append("```powershell")
    lines.append('python tools/vb_disasm.py scan-vip "D:\\Users\\keita\\Downloads\\VB\\extracted" --focus all --output logs/scan/vip_all_extracted_full.txt')
    lines.append('python tools/vb_disasm.py scan-vip "D:\\Users\\keita\\Downloads\\VB\\extracted" --focus obj-bg --output logs/scan/vip_objbg_extracted_full.txt')
    lines.append("python tools/vip_scan_report.py --target-dir \"D:\\Users\\keita\\Downloads\\VB\\extracted\" --scan-all logs/scan/vip_all_extracted_full.txt --scan-objbg logs/scan/vip_objbg_extracted_full.txt --output logs/reports/vip_operations_full_report.md")
    lines.append("```")
    lines.append("")
    lines.append("## Summary")
    lines.append(f"- ROMs scanned (`--focus all`): **{len(all_rom_counts)}**")
    lines.append(f"- Total VIP-related write candidates (`--focus all`): **{scan_all['total']}**")
    lines.append(f"- ROMs with OBJ/BG table hits (`--focus obj-bg`): **{len(objbg_rom_counts)}**")
    lines.append(f"- Total OBJ/BG write candidates (`--focus obj-bg`): **{scan_objbg['total']}**")
    lines.append("")
    lines.append("### Category Breakdown (`--focus all`)")
    lines.append(emit_table(fmt_top(all_cat, 10), ("Category", "Count")))
    lines.append("")
    lines.append("### Instruction Form Breakdown (`--focus all`)")
    lines.append(emit_table(fmt_top(all_ops, 10), ("Instruction", "Count")))
    lines.append("")
    lines.append("### Top ROMs by VIP Write Count (`--focus all`)")
    lines.append(emit_table(fmt_top(all_rom_counts, 12), ("ROM", "Count")))
    lines.append("")
    lines.append("### OBJ/BG Field Hits (`--focus obj-bg`)")
    lines.append("#### OAM Fields")
    lines.append(emit_table(fmt_top(objbg_oam, 10), ("OAM Field", "Count")))
    lines.append("")
    lines.append("#### World Table Fields")
    lines.append(emit_table(fmt_top(objbg_world, 16), ("World Field", "Count")))
    lines.append("")
    lines.append("#### Instruction Forms (`--focus obj-bg`)")
    lines.append(emit_table(fmt_top(objbg_ops, 10), ("Instruction", "Count")))
    lines.append("")
    lines.append("### VIP Register Hits (`--focus all`)")
    lines.append(emit_table(fmt_top(all_regs, 16), ("Register", "Count")))
    lines.append("")
    lines.append("## Call/Operation Usage")
    lines.append("All lines represent write-like operations decoded from V810 code:")
    lines.append("- `st.b/st.h/st.w src, disp[base]`")
    lines.append("  - Effective address: `addr = base + sign_extend(disp16)`")
    lines.append("  - Writes 8/16/32-bit payload to memory-mapped VIP region if `addr` resolves there.")
    lines.append("- `out.b/out.h/out.w src, disp[base]`")
    lines.append("  - Also memory-mapped writes; frequently used for peripheral/VIP style I/O access patterns.")
    lines.append("- `caxi disp[base], rX`")
    lines.append("  - Atomic compare-exchange style op; this scanner treats it as a write candidate because it can modify target memory.")
    lines.append("")
    lines.append("## Parameter Description and Renderer Usage")
    lines.append("### OBJ/OAM (VIP DRAM `0x1E000..0x1FFFF`, 8 bytes per OBJ)")
    lines.append("- `word0: JX`")
    lines.append("  - Base X term for OBJ placement.")
    lines.append("- `word1: JP_EYEMASK`")
    lines.append("  - `JP_raw = word1 & 0x3FFF`, interpreted as signed 14-bit parallax.")
    lines.append("  - `bit15` = left-eye enable, `bit14` = right-eye enable (`1=enabled`, `0=masked`).")
    lines.append("- `word2: JY`")
    lines.append("  - Y anchor used by `tile_y = (Y - JY) & 0xFF`.")
    lines.append("- `word3: ATTR(char,palette,hflip,vflip)`")
    lines.append("  - Character index, palette, and flip flags.")
    lines.append("- Renderer usage (from `vip_draw.inc`):")
    lines.append("  - `x = sign10(JX + (eye==R ? JP : -JP))`")
    lines.append("  - `depth_value = clamp(sign14(JP))`")
    lines.append("")
    lines.append("### World Table (VIP DRAM `0x1D800..0x1DFFF`, 0x20 bytes/world)")
    lines.append("- `WORLD_CTRL(L/R enable,bgm,scx/scy,end/over,bgmap_base)`")
    lines.append("  - Includes `lron`, BG mode (`Normal/H-Bias/Affine/OBJ`), size selectors, and end/overplane flags.")
    lines.append("- `GX/GP/GY`")
    lines.append("  - Destination-space X parallax, X/Y origin terms.")
    lines.append("- `MX/MP/MY`")
    lines.append("  - Source-space base/parallax terms.")
    lines.append("- `WINDOW_WIDTH`, `WINDOW_HEIGHT`")
    lines.append("  - Clipping rectangle.")
    lines.append("- `PARAM_BASE`")
    lines.append("  - Row parameter base for H-Bias/Affine.")
    lines.append("- `OVERPLANE_CHAR`")
    lines.append("  - Fallback char for overplane behavior.")
    lines.append("- Renderer usage highlights:")
    lines.append("  - Normal/H-Bias: `srcX = MX + (eye==R ? MP : -MP)` and `destX = GX + (eye==R ? GP : -GP)`")
    lines.append("  - H-Bias adds row term: `srcX += DRAM[(param_base + (((RealY - DestY) * 2) | eye)) & 0xFFFF]`")
    lines.append("  - Affine row params: `(MX, MP, MY, DX, DY)` and eye-dependent `MP` sign rule in `DrawAffine`.")
    lines.append("")
    lines.append("### VIP Registers (example write semantics from `vip.c`)")
    lines.append("- `IENABLE (0x02)`: interrupt enable mask (`V & 0xE01F`).")
    lines.append("- `IPENDING clear (0x04)`: clears pending interrupt bits (`InterruptPending &= ~V`).")
    lines.append("- `DPCTRL (0x22)`: display control (`V & 0x703`), includes reset behavior when bit0 set.")
    lines.append("- `XPCTRL (0x42)`: drawing control (`V & 0x0002`) and `SBCMP=(V>>8)&0x1F`; bit0 triggers draw state reset/toggle.")
    lines.append("- `SPT0..SPT3 (0x48..0x4E)`: OBJ search table pointers (`V & 0x3FF`).")
    lines.append("- `GPLT0..GPLT3 (0x60..0x66)`: BG palettes (`V & 0xFC`).")
    lines.append("- `JPLT0..JPLT3 (0x68..0x6E)`: OBJ palettes (`V & 0xFC`).")
    lines.append("- `BKCOL (0x70)`: background color (`V & 0x3`).")
    lines.append("")
    lines.append("## Notes and Limits")
    lines.append("- This is static linear disassembly + lightweight register tracking.")
    lines.append("- A line is a *candidate* write site; it is not guaranteed to execute in every frame.")
    lines.append("- Unknown or broad regions may appear as `VIP DRAM (other)` or `VIP other`; use runtime trace for frame-accurate confirmation.")
    lines.append("")
    lines.append("## Next Step for Runtime-Accurate JP/JX/EyeMask")
    lines.append("- Hook VIP write paths (`VIP_Write8/16` and/or bus write site in V810 core).")
    lines.append("- Log per-frame: `PC`, absolute VIP address, value, decoded field (`JX/JP_EYEMASK/JY/ATTR/world field/register`).")
    lines.append("- Compare logs against a fixed frame number to map exact active draw parameters.")

    return "\n".join(lines) + "\n"


def main() -> None:
    parser = argparse.ArgumentParser(description="Generate markdown report from VIP scan dumps.")
    parser.add_argument("--target-dir", required=True, help="ROM directory used for scan context")
    parser.add_argument("--scan-all", required=True, type=Path, help="scan-vip output with --focus all")
    parser.add_argument("--scan-objbg", required=True, type=Path, help="scan-vip output with --focus obj-bg")
    parser.add_argument("--output", required=True, type=Path, help="Output markdown path")
    args = parser.parse_args()

    all_data = parse_scan(args.scan_all)
    objbg_data = parse_scan(args.scan_objbg)
    report = build_report(all_data, objbg_data, args.target_dir)

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(report, encoding="utf-8")
    print(f"Wrote: {args.output}")


if __name__ == "__main__":
    main()
