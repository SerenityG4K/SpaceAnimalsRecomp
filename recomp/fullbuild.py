#!/usr/bin/env python3
"""Build complet d'un ou plusieurs modules pour l'exécutable natif (Phase 2).

  python fullbuild.py <module> [<module> ...] [-j N] [-O 2] [--keep-src] [--insns 60000]

1. Découverte parallèle des fonctions (eh_frame + cibles bl/b + pointeurs, puis entrées en
   milieu de fonction et stubs découverts en route, par tours jusqu'au point fixe).
2. Découpage en lots équilibrés (~--insns instructions par fichier).
3. Chaque processus génère un lot, le compile (g++ -O2, cache par contenu), puis supprime la
   source (sauf --keep-src) : main complet ≈ 3 Go de C++, ≈ 0,9 Go d'objets.

Sorties : build/full/<module>/*.o, build/full/<module>/objs.txt, build/full/manifest.txt.
Tout est dérivé du jeu : privé.
"""
import argparse
import hashlib
import multiprocessing as mp
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import build                                   # noqa: E402
from a64.codegen import Recompiler             # noqa: E402
from a64.module import Module                  # noqa: E402

FULL = os.path.join(HERE, "build", "full")
SRC = os.path.join(HERE, "out", "full")


def gen_digest():
    """Empreinte du générateur : toute modification invalide les objets."""
    h = hashlib.sha1()
    for d in ("a64",):
        for f in sorted(os.listdir(os.path.join(HERE, d))):
            if f.endswith(".py"):
                h.update(open(os.path.join(HERE, d, f), "rb").read())
    return h.hexdigest()


class RC(Recompiler):
    """Recompiler qui note les fonctions ajoutées après l'initialisation."""

    def __init__(self, *a, **k):
        self.new = []
        super().__init__(*a, **k)
        self.new = []

    def _add(self, start, end, origin):
        if start not in self.funcs:
            self.new.append((start, end, origin))
        return super()._add(start, end, origin)


_rc = None


def hooked_addrs(m):
    """Adresses des fonctions listées dans hooks.txt pour ce module (appels via le dispatch)."""
    out = set()
    path = os.path.join(HERE, "hooks.txt")
    if not os.path.exists(path):
        return out
    for line in open(path, encoding="utf-8"):
        line = line.split("#")[0].split()
        if len(line) >= 2 and line[0] == m.name:
            if line[1].startswith("0x"):          # offset dans le module (fonction sans symbole)
                out.add(m.base + int(line[1], 16))
                continue
            if line[1] not in m.exports:
                raise RuntimeError(f"hooks.txt : {line[1]} absent de {m.name}")
            out.add(m.base + m.exports[line[1]])
    return out


def _init(name):
    global _rc
    m = Module.from_work(name)
    _rc = RC(m, comments=False, trace=False, log=lambda *a: None, hooked=hooked_addrs(m))


def _apply(additions):
    for a, e, o in additions:
        if a not in _rc.funcs:
            Recompiler._add(_rc, a, e, o)


def _discover(task):
    addrs, additions = task
    _apply(additions)
    _rc.new = []
    n = 0
    for a in addrs:
        f = _rc.funcs[a]
        _rc._analyze_function(f)
        n += len(f.insns)
        f.insns = []
        if len(_rc.cache) > 1_500_000:
            _rc.cache.clear()
    _rc.cache.clear()
    return _rc.new, n, dict(_rc.stats["unimpl"])


def _emit(task):
    idx, chunk, additions, name, flags, keep = task
    _apply(additions)
    t0 = time.time()
    srcdir = os.path.join(SRC, name)
    objdir = os.path.join(FULL, name)
    files = _rc.write(srcdir, chunk, per_file=len(chunk) + 1, prefix=f"{name}_{idx:05d}",
                      dispatch=False)
    for a in chunk:
        _rc.funcs[a].insns = []
    _rc.cache.clear()
    src = files[0]
    tgen = time.time() - t0
    obj = os.path.join(objdir, f"{name}_{idx:05d}.o")
    data = open(src, "rb").read()
    key = hashlib.sha1((" ".join(flags) + "\0").encode() + data + build.headers_digest()).hexdigest()
    stamp = obj + ".key"
    err = None
    if not (os.path.exists(obj) and os.path.exists(stamp) and open(stamp).read() == key):
        r = subprocess.run(["g++", *flags, "-c", src, "-o", obj], capture_output=True, text=True)
        if r.returncode:
            err = r.stderr[:6000]
        else:
            open(stamp, "w").write(key)
    if err is None and not keep:
        os.remove(src)
    return idx, obj, err, tgen, time.time() - t0 - tgen, len(data)


def build_module(name, jobs, opt, keep, insns_per_file, log=print):
    t0 = time.time()
    os.makedirs(os.path.join(FULL, name), exist_ok=True)
    os.makedirs(os.path.join(SRC, name), exist_ok=True)
    m = Module.from_work(name)
    hk = hooked_addrs(m)
    if hk:
        log(f"{name}: {len(hk)} fonction(s) accrochée(s) (hooks.txt)")
    rc0 = RC(m, comments=False, trace=False, log=lambda *a: None, hooked=hk)
    known = set(rc0.funcs)
    additions = {}
    pending = sorted(known)
    flags = build.BASE_FLAGS + ["-O" + opt]
    ctx = mp.get_context("spawn")
    with ctx.Pool(jobs, initializer=_init, initargs=(name,)) as pool:
        rnd = 0
        total_insns = 0
        unimpl = {}
        while pending:
            parts = max(1, min(jobs * 4, len(pending) // 50 or 1))
            add_list = sorted(additions.values())
            tasks = [(pending[i::parts], add_list) for i in range(parts)]
            new = {}
            for lst, n, un in pool.imap_unordered(_discover, tasks):
                total_insns += n
                for k, v in un.items():
                    unimpl[k] = unimpl.get(k, 0) + v
                for a, e, o in lst:
                    if a not in known and a not in new:
                        new[a] = (a, e, o)
            known |= set(new)
            additions.update(new)
            pending = sorted(new)
            log(f"{name}: tour {rnd} : {len(new)} fonctions découvertes "
                f"({len(known)} au total) [{time.time() - t0:.0f} s]")
            rnd += 1
        if unimpl:
            log(f"{name}: instructions non gérées : {unimpl}")
        # lots équilibrés par nombre d'instructions (approx. taille / 4)
        ends = {a: e for a, e, _ in additions.values()}
        allf = sorted(known)
        chunks, cur, w = [], [], 0
        for a in allf:
            f = rc0.funcs.get(a)
            size = ((f.end if f else ends[a]) - a) // 4
            cur.append(a)
            w += size
            if w >= insns_per_file:
                chunks.append(cur)
                cur, w = [], 0
        if cur:
            chunks.append(cur)
        log(f"{name}: {len(allf)} fonctions, {total_insns} instructions, {len(chunks)} lots")
        add_list = sorted(additions.values())
        tasks = [(i, c, add_list, name, flags, keep) for i, c in enumerate(chunks)]
        objs, errors = [None] * len(chunks), []
        done = 0
        src_bytes = 0
        for idx, obj, err, tg, tc, nb in pool.imap_unordered(_emit, tasks):
            objs[idx] = obj
            src_bytes += nb
            done += 1
            if err:
                errors.append((idx, err))
            if done % 20 == 0 or done == len(chunks):
                log(f"{name}: {done}/{len(chunks)} lots compilés, {src_bytes / 1e6:.0f} Mo de C++ "
                    f"[{time.time() - t0:.0f} s]")
    if errors:
        for idx, e in errors[:3]:
            log(f"--- erreur lot {idx}\n{e}")
        raise RuntimeError(f"{name}: {len(errors)} lot(s) en erreur")
    # table de dispatch du module
    for a, e, o in additions.values():
        Recompiler._add(rc0, a, e, o)
    dsrc = rc0._write_dispatch(os.path.join(SRC, name), allf, name)
    dobj = os.path.join(FULL, name, f"{name}_dispatch.o")
    o, err = build.compile_one("g++", dsrc, dobj, flags)
    if err:
        raise RuntimeError(f"{name}: dispatch : {err[:3000]}")
    objs.append(dobj)
    with open(os.path.join(FULL, name, "objs.txt"), "w") as fp:
        fp.write("\n".join(objs) + "\n")
    log(f"{name}: terminé, {len(objs)} objets [{time.time() - t0:.0f} s]")
    return m


def write_manifest(names):
    """Description des modules pour le chargeur natif (runtime/os/loader.cpp)."""
    lines = ["# nom base text_size ro_off data_off image (ordre des adresses)"]
    for n in names:
        m = Module.from_work(n)
        img = os.path.join(os.path.dirname(HERE), "work", f"{n}.bin")
        lines.append(f"{n} 0x{m.base:x} 0x{m.text_size:x} 0x{m.ro_off:x} 0x{m.data_off:x} {img}")
    # romfs du patch : fichiers stockés bruts dans le .nup (lus en place, jamais copiés).
    # Seul le TOC du conteneur est lu ici.
    # copie du joueur : RECOMP_NUP (launcher), sinon le premier .nup du dossier du projet
    proj = os.path.dirname(HERE)
    path = os.environ.get("RECOMP_NUP")
    if not path:
        nups = [f for f in os.listdir(proj) if f.endswith(".nup")]
        path = os.path.join(proj, nups[0]) if nups else None
    if path:
        sys.path.insert(0, os.path.join(proj, "tools"))
        import nup as nupmod
        n = nupmod.Nup(path)
        lines.append(f"nup {path}")
        for i, c in enumerate(n.contents):
            if c["type"] != 1:
                continue
            for p, fi in n.walk(i):
                if p.startswith("romfs/") and not fi["compressed"]:
                    lines.append(f"romfs 0x{fi['offset']:x} 0x{fi['size']:x} {p[len('romfs/'):]}")
                elif p.startswith("romfs/"):
                    print(f"romfs : {p} compressé dans le .nup, ignoré")
    os.makedirs(FULL, exist_ok=True)
    with open(os.path.join(FULL, "manifest.txt"), "w", encoding="utf-8") as fp:
        fp.write("\n".join(lines) + "\n")


def main():
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8", line_buffering=True)
    p = argparse.ArgumentParser()
    p.add_argument("modules", nargs="+")
    p.add_argument("-j", type=int, default=os.cpu_count())
    p.add_argument("-O", default="2")
    p.add_argument("--keep-src", action="store_true")
    p.add_argument("--insns", type=int, default=60000)
    a = p.parse_args()
    for n in a.modules:
        build_module(n, a.j, a.O, a.keep_src, a.insns)
    write_manifest(["main", "subsdk0", "subsdk1", "subsdk2", "subsdk3", "sdk"])


if __name__ == "__main__":
    main()
