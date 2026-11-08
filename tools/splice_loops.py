#!/usr/bin/env python3
"""splice_loops.py [GEN_DIR] - splice native loops into translated functions
(config/splices.json; runtime/native_tex.c).

At each `L_<entry>:` label it inserts a call to fam_tex_splice(id): when that
returns 1 the native code has done the loop (registers and memory as the x86
loop leaves them) and control jumps to `L_<exit>`. At `L_<exit>:` it inserts
fam_tex_splice_exit() (verification of the translated loop when
FAMILIA_NATIVE_CHECK=1). The lifter emits a function's code in every C
function that contains it (jump-table targets become functions of their own),
so every copy holding both labels is patched. Idempotent."""
import json, re, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MARK = "/* splice */"

def main():
    gen = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "build/gen"
    cfg = json.loads((ROOT / "config/splices.json").read_text())
    by_mod = {}
    for s in cfg: by_mod.setdefault(s["module"], []).append(s)
    for mod, splices in by_mod.items():
        for f in sorted((gen / mod).glob("f_*.c")):
            text = f.read_text()
            if not any(f"L_{s['entry']}:" in text for s in splices): continue
            # split into functions at "void recomp_..." heads
            parts = re.split(r"(?m)^(?=void recomp_\w+\(void\) \{)", text)
            changed = 0
            out = []
            for p in parts:
                for s in splices:
                    e, x = s["entry"], s["exit"]
                    if f"\nL_{e}:\n" not in p or f"\nL_{x}:\n" not in p: continue
                    call = f"fam_tex_splice({s['id']})"
                    if call in p: continue
                    p = p.replace(f"\nL_{e}:\n",
                        f"\nL_{e}:\n    {{ extern int fam_tex_splice(int); RECOMP_REGS_OUT(); "
                        f"if ({call}) {{ RECOMP_REGS_IN(); goto L_{x}; }} RECOMP_REGS_IN(); }} {MARK}\n", 1)
                    if "fam_tex_splice_exit()" not in p.split(f"\nL_{x}:\n", 1)[1][:200]:
                        p = p.replace(f"\nL_{x}:\n",
                            f"\nL_{x}:\n    {{ extern void fam_tex_splice_exit(void); RECOMP_REGS_OUT(); fam_tex_splice_exit(); }} /* splice exit */\n", 1)
                    changed += 1
                out.append(p)
            if changed:
                f.write_text("".join(out))
                print(f"{mod}/{f.name}: {changed} loop splices")

if __name__ == "__main__":
    main()
