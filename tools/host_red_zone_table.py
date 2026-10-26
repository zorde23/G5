import argparse
import re
import shutil
import subprocess
import sys
from pathlib import Path

DESCRIPTION = """Write the red-zone table APS5_RED_ZONE_GUARD reads for host libraries: for each .prx or .dll in
--libs, the functions whose code addresses memory below RSP, as '<begin rva> <end rva> host red zone' lines in
<file>.redzone.txt next to it. Host code built with -DAPS5_HOST_NO_RED_ZONE=ON keeps no values there; what remains
is prebuilt code, such as hand-written FFmpeg assembly. A library with no such function gets no file.

Functions are delimited by the COFF symbols objdump lists in .text; an access is an instruction with an [rsp-0x...]
operand; section symbols such as .gcc_except_table, data the linker placed in .text, are skipped."""

FUNCTION = re.compile(r"^([0-9a-f]+) <([^>]+)>:$")
BELOW_RSP = re.compile(r"\[rsp-0x[0-9a-f]+\]")


def image_base(objdump, path):
    output = subprocess.run([objdump, "-p", str(path)], capture_output=True, text=True, check=True).stdout
    for line in output.splitlines():
        if line.startswith("ImageBase"):
            return int(line.split()[1], 16)
    raise ValueError(f"{path}: no ImageBase")


def text_end(objdump, path):
    output = subprocess.run([objdump, "-h", str(path)], capture_output=True, text=True, check=True).stdout
    for line in output.splitlines():
        fields = line.split()
        if len(fields) >= 4 and fields[1] == ".text":
            return int(fields[3], 16) + int(fields[2], 16)
    raise ValueError(f"{path}: no .text section")


def red_zone_functions(objdump, path):
    output = subprocess.run([objdump, "-d", "-j", ".text", "--no-show-raw-insn", "-M", "intel", str(path)], capture_output=True, text=True, check=True).stdout
    starts = []
    marked = set()
    data = False
    for line in output.splitlines():
        match = FUNCTION.match(line)
        if match:
            starts.append(int(match.group(1), 16))
            data = match.group(2).startswith(".")
        elif starts and not data and BELOW_RSP.search(line):
            marked.add(starts[-1])
    if not starts:
        raise ValueError(f"{path}: no function symbols in .text")
    ends = starts[1:] + [text_end(objdump, path)]
    return [(start, end) for start, end in zip(starts, ends) if start in marked]


def main():
    parser = argparse.ArgumentParser(description=DESCRIPTION, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--libs", required=True, type=Path, help="directory of the host libraries")
    parser.add_argument("--objdump", default=shutil.which("objdump") or "objdump", help="objdump of the MinGW toolchain")
    args = parser.parse_args()
    written = 0
    for path in sorted(list(args.libs.glob("*.prx")) + list(args.libs.glob("*.dll"))):
        ranges = red_zone_functions(args.objdump, path)
        table = path.with_name(path.name + ".redzone.txt")
        if not ranges:
            table.unlink(missing_ok=True)
            continue
        base = image_base(args.objdump, path)
        table.write_text("".join(f"{begin - base:x} {end - base:x} host red zone\n" for begin, end in ranges))
        written += 1
        print(f"{path.name}: {len(ranges)} functions")
    print(f"{written} tables written")
    return 0


if __name__ == "__main__":
    sys.exit(main())
