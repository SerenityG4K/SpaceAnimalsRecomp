#!/usr/bin/env python3
"""Banc de validation différentielle : code recompilé (DLL) contre Unicorn.

  python bench/harness.py func <module> [--only ADDR,...] [--trials N] [--max-steps N]
      Exécute chaque fonction du module côté recomp et côté Unicorn à partir du même état
      initial aléatoire, compare l'état CPU AVANT CHAQUE INSTRUCTION (trace complète), puis
      l'état final et la mémoire (image du module, pile, zone de travail, TLS).

  python bench/harness.py fuzz <module> [--trials N] [--limit N]
      Fuzzing instruction par instruction : chaque mot d'instruction distinct du module est
      traduit isolément, exécuté sur N états aléatoires des deux côtés, et comparé.

Les résultats détaillés sont écrits dans bench/results/ (privé : dérivé du jeu).
"""
import argparse
import collections
import json
import os
import random
import struct
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, ".."))
sys.path.insert(0, ROOT)
sys.path.insert(0, HERE)

import capstone  # noqa: E402

import build  # noqa: E402
from a64.codegen import Recompiler  # noqa: E402
from a64.common import BL, BLR, BR, RET, SEQ, TRAP, B, BCOND, hx  # noqa: E402
from a64.module import IMPORT_THUNK_BASE, Module  # noqa: E402
from a64.translate import translate  # noqa: E402
from guest import (THUNK_SIZE, SCRATCH_BASE, SCRATCH_SIZE, SENTINEL, STACK_BASE, STACK_SIZE, TLS_BASE,  # noqa: E402
                   TLS_SIZE, TRACE_SIZE, HostMem, RecompDll, RefEmu, State, diff_regs,
                   diff_states, random_value, random_vreg, round_up, scratch_pattern)

RESULTS = os.path.join(HERE, "results")
MD = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
ST_NAMES = {0: "ok", 1: "trap", 2: "unimpl", 3: "limit", 4: "badtarget", 5: "badret", 6: "undef",
            "fault": "fault"}


def disasm(word, pc):
    for a, s, m, o in MD.disasm_lite(struct.pack("<I", word), pc):
        return f"{m} {o}".strip()
    return f".word 0x{word:08x}"


class Bench:
    def __init__(self, name, dll_path, base=None):
        self.m = Module.from_work(name, base)
        self.img, self.thunks = self.m.relocated_image()
        self.thunks = self.m.import_thunks()
        self.host = HostMem()
        self.emu = RefEmu()
        size = round_up(len(self.img), 0x10000)
        img_padded = bytes(self.img) + bytes(size - len(self.img))
        self.host.map(self.m.base, size)
        self.emu.map(self.m.base, size)
        self.host.write(self.m.base, img_padded)
        self.emu.uc.mem_write(self.m.base, img_padded)
        # seules les parties inscriptibles (.data/.bss) sont réinitialisées et comparées
        rw = (self.m.data_off or 0) & ~0xFFF
        self.rw = img_padded[rw:]
        self.regions = [(self.m.base + rw, self.rw), (STACK_BASE, bytes(STACK_SIZE)),
                        (SCRATCH_BASE, bytes(SCRATCH_SIZE)), (TLS_BASE, bytes(TLS_SIZE))]
        for a, data in self.regions[1:]:
            self.host.map(a, len(data))
            self.emu.map(a, len(data))
        # zone des thunks d'import : mappée aussi côté hôte (mêmes octets que côté Unicorn), car
        # les imports de DONNÉES (GLOB_DAT) y pointent aussi tant que le HLE n'existe pas
        self.thunk_bytes = struct.pack("<I", 0xD65F03C0) * (THUNK_SIZE // 4)
        self.host.map(IMPORT_THUNK_BASE, THUNK_SIZE)
        self.regions.append((IMPORT_THUNK_BASE, self.thunk_bytes))
        self.dll = RecompDll(dll_path)
        self.coverage = set()
        for a in self.thunks:
            self.dll.dll.recomp_register_stub(a)

    def reset_memory(self, rng):
        scratch = scratch_pattern(rng, SCRATCH_SIZE)
        tls = scratch_pattern(rng, TLS_SIZE)
        contents = [self.rw, bytes(STACK_SIZE), scratch, tls, self.thunk_bytes]
        for (a, _), data in zip(self.regions, contents):
            self.host.write(a, data)
            self.emu.uc.mem_write(a, data)
        self.cur = contents

    def random_state(self, rng):
        s = State()
        for i in range(31):
            k = rng.random()
            if i < 8 and k < 0.5:
                s.x[i] = SCRATCH_BASE + 0x1000 + (rng.randrange(0, SCRATCH_SIZE - 0x20000) & ~0xF)
            elif i < 18 and k < 0.6:
                s.x[i] = rng.getrandbits(8)
            else:
                s.x[i] = random_value(rng)
        s.x[29] = STACK_BASE + STACK_SIZE - 0x100
        s.x[30] = SENTINEL
        s.sp = STACK_BASE + STACK_SIZE - 0x200
        s.nzcv = rng.getrandbits(4) << 28
        s.tpidr_el0 = TLS_BASE + 0x100
        s.tpidrro_el0 = TLS_BASE + 0x800
        s.v = [random_vreg(rng) for _ in range(32)]
        return s

    def mem_diffs(self, limit=8):
        out = []
        for (a, data) in self.regions:
            hm = self.host.read(a, len(data))
            um = bytes(self.emu.uc.mem_read(a, len(data)))
            if hm != um:
                i = 0
                while i < len(hm) and len(out) < limit:
                    if hm[i] != um[i]:
                        j = i
                        while j < len(hm) and hm[j] != um[j] and j - i < 16:
                            j += 1
                        out.append((a + i, um[i:j].hex(), hm[i:j].hex()))
                        i = j
                    else:
                        i += 1
        return out


# =====================================================================================
# Fonctions entières
# =====================================================================================
def compare_run(bench, func, state, max_steps, with_v=True):
    su, mu, fu, tu, wu = bench.emu.run(func, SENTINEL, state, True, with_v, max_steps)
    sr, mr, fr, tr, wr = bench.dll.call(func, state, True, with_v, max_steps)
    sr = ST_NAMES.get(sr, sr)
    nu, nr = len(tu) // TRACE_SIZE, len(tr) // TRACE_SIZE
    res = dict(func=hx(func), uc=su, recomp=sr, uc_msg=mu, recomp_msg=mr, steps_uc=nu, steps_recomp=nr)
    if bench.coverage is not None:
        bench.coverage.update(struct.unpack_from("<Q", tu, k * TRACE_SIZE)[0] for k in range(nu))
    for k in range(min(nu, nr)):
        eu = tu[k * TRACE_SIZE:(k + 1) * TRACE_SIZE]
        er = tr[k * TRACE_SIZE:(k + 1) * TRACE_SIZE]
        if eu != er:
            d = diff_regs(eu, er, with_v)
            prev = None
            if k:
                pc = struct.unpack_from("<Q", tu, (k - 1) * TRACE_SIZE)[0]
                word = bench.m.word(pc - bench.m.base) if bench.m.base <= pc < bench.m.base + bench.m.size else 0
                prev = f"{pc:x}: {disasm(word, pc)}"
            res.update(verdict="FAIL", step=k, culprit=prev,
                       diffs=[(n, hex(a), hex(b)) for n, a, b in d[:12]])
            return res
    if sr == "unimpl":
        res["verdict"] = "UNIMPL"
        return res
    if sr == "badret":
        res["verdict"] = "BADRET"
        return res
    if sr == "badtarget":
        import re
        mt = re.search(r"vers 0x([0-9a-f]+)", mr)
        t = int(mt.group(1), 16) if mt else 0
        text_end = bench.m.base + (bench.m.text_size or bench.m.size)
        if bench.m.base <= t < text_end and not bench.dll.dll.recomp_has_function(t) and nu >= nr:
            res["verdict"] = "NOTBUILT"   # cible réelle hors du lot compilé (banc partiel)
            return res
        if not (bench.m.base <= t < text_end) and nu >= nr:
            # appel indirect vers une adresse hors code (pointeur aléatoire du test) : Unicorn
            # exécute alors des données, la comparaison s'arrête là
            res["verdict"] = "NONCODE"
            return res
    if nu != nr:
        # cas toléré : saut indirect vers une adresse invalide (recomp : cible inconnue,
        # Unicorn : faute de lecture d'instruction au pas suivant)
        if sr == "badtarget" and su == "fault" and nu == nr:
            res["verdict"] = "PASS"
            return res
        res.update(verdict="FAIL", step=min(nu, nr), culprit="longueur de trace différente")
        return res
    if su == "ok" and sr == "ok":
        d = diff_states(fu, fr, with_v)
        md = bench.mem_diffs()
        if d or md:
            res.update(verdict="FAIL", step=nu, culprit="état final",
                       diffs=[(n, hex(a), hex(b)) for n, a, b in d[:12]],
                       mem=[(hex(a), u, r) for a, u, r in md])
            return res
        res["verdict"] = "PASS"
        return res
    pairs = {("fault", "fault"), ("limit", "limit"), ("trap", "trap"), ("fault", "badtarget"),
             ("trap", "undef")}
    if su == "trap" and mu == "exception 4" and sr == "fault":
        # « data abort » (adresse hors de l'espace 48 bits, ex. 0xffff…) côté Unicorn
        # = violation d'accès côté recomp
        su = "fault"
    if (su, sr) in pairs:
        res["verdict"] = "PASS"
        return res
    res.update(verdict="FAIL", step=nu, culprit=f"statuts différents uc={su} recomp={sr}")
    return res


def closure(rc, root, limit):
    """Fonctions atteignables par appels/sauts directs depuis `root` (None si > limit)."""
    seen = {root}
    todo = [root]
    while todo:
        a = todo.pop()
        f = rc.funcs[a]
        if not f.insns:
            rc._analyze_function(f)
        for c in f.calls:
            if c in rc.funcs and c not in seen:
                seen.add(c)
                if len(seen) > limit:
                    return None
                todo.append(c)
    return seen


def build_module_dll(name, base=None, log=print, sample=0, max_closure=0):
    m = Module.from_work(name, base)
    rc = Recompiler(m, comments=True)
    roots = None
    if sample:
        rng = random.Random(42)
        cands = [a for a, f in rc.funcs.items() if f.origin == "eh_frame"]
        rng.shuffle(cands)
        roots, emit = [], set()
        for a in cands:
            c = closure(rc, a, max_closure)
            if c is None:
                continue
            roots.append(a)
            emit |= c
            if len(roots) >= sample:
                break
        log(f"{len(roots)} fonctions racines, {len(emit)} fonctions émises (fermetures ≤ {max_closure})")
        out = os.path.join(ROOT, "out", f"{name}_bench")
        files = rc.write(out, sorted(emit), per_file=400)
    else:
        rc.analyze()
        out = os.path.join(ROOT, "out", name)
        files = rc.write(out)
    rc.roots = roots
    dll = os.path.join(ROOT, "build", f"{name}_bench.dll")
    t0 = time.time()
    build.build([f for f in files], dll, os.path.join(ROOT, "build", "obj", f"{name}_bench"),
                opt="-O1", defs=build.BENCH_DEFS, log=log)
    log(f"DLL {dll} compilée [{time.time() - t0:.1f} s]")
    return rc, dll


def cmd_func(args):
    rc, dll = build_module_dll(args.module, args.base, sample=args.sample,
                               max_closure=args.max_closure)
    bench = Bench(args.module, dll, args.base)
    funcs = sorted(rc.roots) if rc.roots is not None else sorted(rc.funcs)
    if args.only:
        funcs = [int(x, 16) for x in args.only.split(",")]
    results = []
    per_func = collections.defaultdict(collections.Counter)
    total = collections.Counter()
    ends = collections.Counter()
    t0 = time.time()
    total_steps = 0
    for f in funcs:
        for t in range(args.trials):
            rng = random.Random(f * 1000 + t)
            bench.reset_memory(rng)
            st = bench.random_state(rng)
            r = compare_run(bench, f, st, args.max_steps)
            r["trial"] = t
            total_steps += r["steps_uc"]
            per_func[f][r["verdict"]] += 1
            total[r["verdict"]] += 1
            ends[f"{r['uc']}/{r['recomp']}"] += 1
            if r["verdict"] != "PASS":
                results.append(r)
    dt = time.time() - t0
    os.makedirs(RESULTS, exist_ok=True)
    path = os.path.join(RESULTS, f"{args.module}_func.json")
    json.dump(dict(module=args.module, trials=args.trials, totals=total,
                   per_func={hx(k): v for k, v in per_func.items()}, failures=results),
              open(path, "w", encoding="utf-8"), indent=1)
    print(f"{args.module}: {len(funcs)} fonctions × {args.trials} essais, "
          f"{total_steps} instructions comparées pas à pas [{dt:.1f} s]")
    print("  verdicts :", dict(total))
    print("  fins d'exécution (unicorn/recomp) :", dict(ends.most_common()))
    allpc = {i.pc for fa in (rc.funcs if rc.roots is None else rc.funcs) for i in rc.funcs[fa].insns
             if i.kind != TRAP}
    cov = len(bench.coverage & allpc)
    print(f"  couverture : {cov}/{len(allpc)} instructions exécutées au moins une fois "
          f"({100 * cov / max(1, len(allpc)):.1f} %)")
    shown = set()
    for r in results:
        if r["verdict"] == "FAIL" and r["func"] not in shown and len(shown) < args.show:
            shown.add(r["func"])
            print(f"  ÉCHEC {r['func']} essai {r['trial']} pas {r.get('step')} "
                  f"(uc={r['uc']} recomp={r['recomp']}) coupable : {r.get('culprit')}")
            for d in r.get("diffs", [])[:6]:
                print(f"      {d[0]}: unicorn={d[1]} recomp={d[2]}")
            for d in r.get("mem", [])[:4]:
                print(f"      mem {d[0]}: unicorn={d[1]} recomp={d[2]}")
            if r["recomp_msg"]:
                print(f"      recomp : {r['recomp_msg']}")
    print(f"  détails : {path}")
    return 0 if total["FAIL"] == 0 else 1


# =====================================================================================
# Fuzzing instruction par instruction
# =====================================================================================
FUZZ_BASE = 0x0000007D00000000
SKIP_OPS = {"svc", "hint", "mrs", "msr", "isb", "dmb", "clrex", "sys", "msr_imm", "udf"}


def collect_insns(m, limit=None, filt=None, sample=0):
    """Mots d'instruction distincts (premier pc rencontré) des fonctions du module.
    `sample` : nombre de fonctions tirées au hasard (0 = toutes)."""
    seen = {}
    funcs = m.functions
    if sample and sample < len(funcs):
        funcs = random.Random(7).sample(funcs, sample)
    for s, e in funcs:
        for off in range(s, e, 4):
            w = m.word(off)
            if w in seen:
                continue
            pc = m.base + off
            i = translate(w, pc)
            if i.kind != SEQ or i.unimpl or i.op in SKIP_OPS or not i.code:
                continue
            if filt and not filt(i):
                continue
            seen[w] = i
    items = list(seen.values())
    if limit:
        random.Random(1).shuffle(items)
        items = items[:limit]
    return items


def gen_fuzz_cpp(insns, path, mutate=None):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as fp:
        fp.write("// Tests unitaires générés (une instruction par fonction). Dérivé du jeu.\n")
        fp.write('#include "cpu.h"\n\n')
        for k, i in enumerate(insns):
            code = i.code
            for m in mutate or ():   # test du banc lui-même : corruption volontaire
                a, b = m.split("=", 1)
                code = code.replace(a, b)
            fp.write(f"static void t_{k}(CPUState* ctx) {{ /* {i.pc:x}: {disasm(i.word, i.pc)} */ "
                     f"{code} }}\n")
        fp.write("\nstatic const uint64_t k_addrs[] = {\n")
        for k in range(len(insns)):
            fp.write(f"    {hx(FUZZ_BASE + 4 * k)},\n")
        fp.write("};\nstatic const RecompFn k_fns[] = {\n")
        for k in range(len(insns)):
            fp.write(f"    t_{k},\n")
        fp.write("};\n")
        fp.write(f"static int s_reg = (rt_register_functions(k_addrs, k_fns, {len(insns)}), 0);\n")


def _oracle_sminmax(i, st, fr, bench, base_addr):
    """LDSMAX/LDSMIN 8/16/32 bits : Unicorn 2.1 (QEMU 5) compare en non signé. Oracle ARM :
    mem' = max/min signé(mem, Rs), Rt = ancienne valeur."""
    bits = int(i.op.rsplit("x", 1)[-1]) if "max" in i.op else int(i.op.rsplit("n", 1)[-1])
    nb = bits // 8
    rs = (i.word >> 16) & 31
    rt = i.word & 31
    a = base_addr
    old = int.from_bytes(bench.cur[2][a - SCRATCH_BASE:a - SCRATCH_BASE + nb], "little")
    v = (st.x[rs] if rs != 31 else 0) & ((1 << bits) - 1)

    def sx(x):
        return x - (1 << bits) if x >> (bits - 1) else x
    new = max(sx(old), sx(v)) if "max" in i.op else min(sx(old), sx(v))
    new &= (1 << bits) - 1
    got = int.from_bytes(bench.host.read(a, nb), "little")
    ok_rt = rt == 31 or fr.x[rt] == old
    return got == new and ok_rt


REF_BUG_ORACLES = {f"at_s{k}{b}": _oracle_sminmax for k in ("max", "min") for b in (8, 16, 32)}


ALIGNED_OPS = ("at_", "cas", "ldxr", "stxr", "ldxp", "stxp", "ldar", "stlr", "ldapr", "ldapur")


def cmd_fuzz(args):
    m = Module.from_work(args.module, args.base)
    filt = None
    if args.op:
        filt = lambda i: any(i.op.startswith(p) for p in args.op.split(","))  # noqa: E731
    if args.fp_only:
        prev = filt
        filt = lambda i: i.fp and (prev is None or prev(i))  # noqa: E731
    insns = collect_insns(m, args.limit, filt, args.sample)
    print(f"{args.module}: {len(insns)} mots d'instruction distincts à tester")
    src = os.path.join(ROOT, "out", f"fuzz_{args.module}", "fuzz.cpp")
    gen_fuzz_cpp(insns, src, args.mutate)
    dll = os.path.join(ROOT, "build", f"fuzz_{args.module}.dll")
    t0 = time.time()
    build.build([src], dll, os.path.join(ROOT, "build", "obj", f"fuzz_{args.module}"), opt="-O1",
                defs=["-DRECOMP_BENCH"])
    print(f"  DLL compilée [{time.time() - t0:.1f} s]")
    bench = Bench(args.module, dll, args.base)
    rng0 = random.Random(1234)
    bench.reset_memory(rng0)
    win_lo, win_hi = 0x2000, 0x12000
    by_op = collections.defaultdict(collections.Counter)
    fails = []
    t0 = time.time()
    for k, i in enumerate(insns):
        for t in range(args.trials):
            rng = random.Random(i.word * 7919 + t)
            st = bench.random_state(rng)
            base_addr = None
            if i.mem_base is not None:
                base_addr = SCRATCH_BASE + 0x40000 + (rng.randrange(0, 0x40000) & ~0x3F)
                if not i.op.startswith(ALIGNED_OPS) and rng.random() < 0.3:
                    base_addr += rng.randrange(1, 16)
                if i.mem_base == 31:
                    st.sp = base_addr
                else:
                    st.x[i.mem_base] = base_addr
                if i.mem_index is not None and i.mem_index != 31 and i.mem_index != i.mem_base:
                    st.x[i.mem_index] = rng.randrange(0, 0x200)
            su, mu, fu, _, _ = bench.emu.run(i.pc, i.pc + 4, st, False, False, 2)
            sr, mr, fr, _, _ = bench.dll.call(FUZZ_BASE + 4 * k, st, False, False, 10)
            sr = ST_NAMES.get(sr, sr)
            verdict = "PASS"
            detail = None
            if su == "ok" and sr == "ok":
                d = diff_states(fu, fr)
                if d:
                    verdict, detail = "FAIL", [(n, hex(a), hex(b)) for n, a, b in d[:8]]
                elif base_addr is not None:
                    lo = base_addr - win_lo
                    hm = bench.host.read(lo, win_hi)
                    um = bytes(bench.emu.uc.mem_read(lo, win_hi))
                    if hm != um:
                        j = next(x for x in range(len(hm)) if hm[x] != um[x])
                        verdict, detail = "FAIL", [("mem", hex(lo + j), um[j:j + 16].hex(),
                                                    hm[j:j + 16].hex())]
            elif su == "fault" and sr == "fault":
                verdict = "PASS"
            else:
                verdict, detail = "FAIL", [("statut", su, sr, mr)]
            if verdict == "FAIL" and i.op in REF_BUG_ORACLES and su == "ok" and sr == "ok":
                # bogue connu de la référence : on vérifie notre résultat avec un oracle Python
                if REF_BUG_ORACLES[i.op](i, st, fr, bench, base_addr):
                    verdict = "REFBUG"
            if base_addr is not None:
                # remettre la fenêtre mémoire à l'état initial
                lo = base_addr - win_lo
                orig = bench.cur[2][lo - SCRATCH_BASE:lo - SCRATCH_BASE + win_hi]
                bench.host.write(lo, orig)
                bench.emu.uc.mem_write(lo, orig)
            by_op[i.op][verdict] += 1
            if verdict == "FAIL":
                fails.append(dict(pc=hx(i.pc), word=f"{i.word:08x}", asm=disasm(i.word, i.pc),
                                  op=i.op, trial=t, detail=detail,
                                  init={**{f"x{n}": hex(v) for n, v in enumerate(st.x)},
                                        "nzcv": hex(st.nzcv)}))
    dt = time.time() - t0
    tot = collections.Counter()
    for c in by_op.values():
        tot.update(c)
    os.makedirs(RESULTS, exist_ok=True)
    path = os.path.join(RESULTS, f"{args.module}_fuzz.json")
    json.dump(dict(module=args.module, totals=tot, by_op={k: dict(v) for k, v in by_op.items()},
                   failures=fails), open(path, "w", encoding="utf-8"), indent=1)
    print(f"  {sum(tot.values())} exécutions [{dt:.1f} s] : {dict(tot)}")
    bad = sorted(((k, v) for k, v in by_op.items() if v["FAIL"]), key=lambda x: -x[1]["FAIL"])
    for k, v in bad[:40]:
        ex = next(f for f in fails if f["op"] == k)
        print(f"  ÉCHEC {k:28s} {v['FAIL']}/{sum(v.values())}  ex: {ex['asm']}  {ex['detail']}")
    print(f"  détails : {path}")
    return 0 if not tot["FAIL"] else 1


def main():
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8")
    p = argparse.ArgumentParser()
    sub = p.add_subparsers(dest="cmd", required=True)
    f = sub.add_parser("func")
    f.add_argument("module")
    f.add_argument("--only")
    f.add_argument("--trials", type=int, default=8)
    f.add_argument("--max-steps", type=int, default=20000)
    f.add_argument("--show", type=int, default=15)
    f.add_argument("--sample", type=int, default=0, help="fonctions racines tirées au hasard")
    f.add_argument("--max-closure", type=int, default=40)
    f.add_argument("--base", type=lambda x: int(x, 0))
    z = sub.add_parser("fuzz")
    z.add_argument("module")
    z.add_argument("--trials", type=int, default=6)
    z.add_argument("--limit", type=int, default=0)
    z.add_argument("--op")
    z.add_argument("--sample", type=int, default=0, help="nombre de fonctions échantillonnées")
    z.add_argument("--fp-only", action="store_true")
    z.add_argument("--mutate", action="append", help="remplacement OLD=NEW (test du banc)")
    z.add_argument("--base", type=lambda x: int(x, 0))
    a = p.parse_args()
    return {"func": cmd_func, "fuzz": cmd_fuzz}[a.cmd](a)


if __name__ == "__main__":
    sys.exit(main())
