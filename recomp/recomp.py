#!/usr/bin/env python3
"""Recompilateur statique ARM64 (Horizon / Switch 2) → C++.

  python recomp.py gen   <module> [--out DIR] [--funcs N] [--base 0x...] [--no-comments]
  python recomp.py stats <module>            couverture du décodeur
"""
import argparse
import collections
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from a64.codegen import Recompiler  # noqa: E402
from a64.module import Module       # noqa: E402


def load(args):
    base = int(args.base, 0) if getattr(args, "base", None) else None
    return Module.from_work(args.module, base)


def cmd_gen(args):
    t0 = time.time()
    m = load(args)
    rc = Recompiler(m, comments=not args.no_comments)
    sel = None
    if args.funcs:
        # premier lot : les N premières fonctions (par adresse), analysées seules ; les
        # fonctions appelées hors du lot restent des déclarations externes
        sel = sorted(a for a, f in rc.funcs.items() if f.origin == "eh_frame")[args.skip:args.skip + args.funcs]
        rc.analyze(only=sel)
    elif not args.stream:
        rc.analyze()
    out = args.out or os.path.join(HERE, "out", m.name)
    if args.stream:
        files = rc.write_stream(out, per_file=args.per_file)
        print(f"{m.name}: {len(rc.funcs)} fonctions, {len(files)} fichiers → {out} [{time.time() - t0:.1f} s]")
        return
    files = rc.write(out, sel, per_file=args.per_file)
    emitted = [rc.funcs[a] for a in (sel if sel is not None else rc.funcs)]
    n_ins = sum(len(f.insns) for f in emitted)
    n_un = sum(1 for f in emitted for i in f.insns if i.unimpl)
    print(f"{m.name}: {len(emitted)} fonctions émises (sur {len(rc.funcs)} connues), {n_ins} instructions, {n_un} non gérées "
          f"({100 * n_un / max(1, n_ins):.3f} %), {len(files)} fichiers → {out} "
          f"[{time.time() - t0:.1f} s]")
    st = rc.stats
    print(f"  entrées en milieu de fonction : {st['mid_entries']}, cibles hors .text : "
          f"{st['ext_targets']}, br indirects : {st['br_indirect']}, tables de sauts : {st['jumptables']}")
    if st["unimpl"]:
        top = sorted(st["unimpl"].items(), key=lambda x: -x[1])[:30]
        print("  non gérées :", ", ".join(f"{k}={v}" for k, v in top))


def cmd_stats(args):
    m = load(args)
    rc = Recompiler(m, comments=False)
    rc.analyze()
    ops = collections.Counter()
    un = collections.Counter()
    for f in rc.funcs.values():
        for i in f.insns:
            ops[i.op] += 1
            if i.unimpl:
                un[i.op] += 1
    tot = sum(ops.values())
    print(f"{m.name}: {len(rc.funcs)} fonctions, {tot} instructions, {sum(un.values())} non gérées")
    for k, v in un.most_common(60):
        print(f"  {k:40s} {v}")


def main():
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8")
    p = argparse.ArgumentParser()
    sub = p.add_subparsers(dest="cmd", required=True)
    g = sub.add_parser("gen")
    g.add_argument("module")
    g.add_argument("--out")
    g.add_argument("--funcs", type=int, default=0)
    g.add_argument("--skip", type=int, default=0)
    g.add_argument("--per-file", type=int, default=1500)
    g.add_argument("--base")
    g.add_argument("--no-comments", action="store_true")
    g.add_argument("--stream", action="store_true", help="génération complète à mémoire bornée")
    s = sub.add_parser("stats")
    s.add_argument("module")
    s.add_argument("--base")
    args = p.parse_args()
    {"gen": cmd_gen, "stats": cmd_stats}[args.cmd](args)


if __name__ == "__main__":
    main()
