#!/usr/bin/env python3
"""Build complet d'un ou plusieurs modules pour l'exécutable natif (Phase 2).

  python fullbuild.py <module> [<module> ...] [-j N] [-O 2] [--keep-src] [--insns 60000]

1. Découverte parallèle des fonctions (eh_frame + cibles bl/b + pointeurs, puis entrées en
   milieu de fonction et stubs découverts en route, par tours jusqu'au point fixe).
2. Découpage en lots équilibrés (~--insns instructions par fichier).
3. Un ou deux processus Python génèrent les lots ; le processus principal les compile (clang++ s'il est fourni,
   sinon g++ ; -O2, cache par contenu), au plus -j à la fois, puis supprime chaque source (sauf --keep-src) :
   main complet ≈ 3 Go de C++, ≈ 0,9 Go d'objets.

Sorties : build/full/<module>/*.o, build/full/<module>/objs.txt, build/full/manifest.txt.
Tout est dérivé du jeu : privé.
"""
import argparse
import bisect
import hashlib
import multiprocessing as mp
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import build                                   # noqa: E402
from a64.codegen import Function, Recompiler   # noqa: E402
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
        self.communes = set()   # fonctions découvertes déjà alignées sur la liste commune (_apply)

    def _add(self, start, end, origin):
        if start not in self.funcs:
            self.new.append((start, end, origin))
        return super()._add(start, end, origin)


_rc = None


def go_libres():
    """Go de mémoire utilisables : min(RAM disponible, mémoire validable disponible) ; None hors de Windows.
    RECOMP_MEM_LIBRE_GO=n : valeur imposée (essais : simuler un PC qui a peu de mémoire libre)."""
    if os.environ.get("RECOMP_MEM_LIBRE_GO"):
        return float(os.environ["RECOMP_MEM_LIBRE_GO"])
    try:
        import ctypes

        class Etat(ctypes.Structure):
            _fields_ = [("dwLength", ctypes.c_ulong), ("dwMemoryLoad", ctypes.c_ulong),
                        ("ullTotalPhys", ctypes.c_ulonglong), ("ullAvailPhys", ctypes.c_ulonglong),
                        ("ullTotalPageFile", ctypes.c_ulonglong), ("ullAvailPageFile", ctypes.c_ulonglong),
                        ("ullTotalVirtual", ctypes.c_ulonglong), ("ullAvailVirtual", ctypes.c_ulonglong),
                        ("ullAvailExtendedVirtual", ctypes.c_ulonglong)]
        e = Etat()
        e.dwLength = ctypes.sizeof(Etat)
        if not ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(e)):
            return None
        return min(e.ullAvailPhys, e.ullAvailPageFile) / 1e9
    except (AttributeError, OSError):
        return None


# Mémoire privée d'un processus Python de fullbuild (main, relevé du 2026-11-11) : ≈ 170 Mo pendant la découverte,
# un peu plus pendant la génération d'un lot. Jusqu'à la 1.0.2 : ≈ 1 Go (copie de l'image du module, 250 Mo ; quatre
# conteneurs par fonction, 140 Mo ; cache d'instructions de 1,5 million d'entrées, 480 Mo jamais rendus au système).
GO_PAR_PYTHON = 0.3


def processus_python(voulus):
    """Nombre de processus Python (découverte, génération) que la mémoire libre permet, au plus `voulus`."""
    n = max(1, voulus)
    libre = go_libres()
    if libre is not None:
        n = max(1, min(n, int((libre - 1.0) / GO_PAR_PYTHON)))
    return n


LOTS = os.path.join(HERE, "lots.txt")


def lots_figes(name):
    """Adresses de début des lots du module, figées dans lots.txt (triées ; liste vide si le module n'y est pas).

    Sans cette liste, les lots sont découpés au fil des fonctions (≈ 60 000 instructions chacun) : une seule fonction
    découverte en plus ou en moins décale tous les lots suivants, change leur texte et les fait tous recompiler.
    Avec la liste, un lot est l'ensemble des fonctions dont l'adresse est entre deux débuts : un correctif du
    recompilateur ne fait recompiler que les lots dont le texte change vraiment. lots.txt reprend le découpage des
    versions 1.0.0 à 1.0.2 (« fullbuild.py … --figer-lots » l'écrit) ; il vaut pour cette version du jeu, comme
    hooks.txt.
    """
    out = []
    try:
        with open(LOTS, encoding="utf-8") as fp:
            for ligne in fp:
                t = ligne.split("#")[0].split()
                if len(t) == 2 and t[0] == name:
                    out.append(int(t[1], 16))
    except OSError:
        return []
    return sorted(out)


def figer_lots(name, chunks):
    """Écrit dans lots.txt les débuts des lots du module (les lignes des autres modules sont gardées)."""
    lignes = []
    try:
        with open(LOTS, encoding="utf-8") as fp:
            lignes = [l.rstrip("\n") for l in fp if l.split("#")[0].split()[:1] != [name]]
    except OSError:
        pass
    if not lignes:
        lignes = ["# Début de chaque lot de code généré : <module> <adresse de la première fonction, en hexadécimal>.",
                  "# Un lot regroupe les fonctions dont l'adresse va de son début à celui du lot suivant (fullbuild.py,",
                  "# lots_figes). Écrit par « python fullbuild.py sdk subsdk0 subsdk1 subsdk2 subsdk3 main --figer-lots » ;",
                  "# ne pas le refaire après une publication : déplacer un début fait recompiler les lots voisins chez",
                  "# tous les joueurs."]
    lignes += [f"{name} {c[0]:x}" for c in chunks]
    with open(LOTS, "w", encoding="utf-8", newline="\n") as fp:
        fp.write("\n".join(lignes) + "\n")


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
                raise RuntimeError(f"hooks.txt: {line[1]} missing from {m.name}")
            out.add(m.base + m.exports[line[1]])
    return out


def _init(name):
    global _rc
    m = Module.from_work(name)
    _rc = RC(m, comments=False, trace=False, log=lambda *a: None, hooked=hooked_addrs(m))


def _apply(additions):
    """Aligne les fonctions découvertes en route sur la liste commune à tous les processus. Chaque tâche part ainsi du
    même état, quel que soit le processus qui la reçoit et les tâches qu'il a faites avant : le code généré est le même
    d'une construction à l'autre (jusqu'à la 1.0.2, l'origine notée en commentaire d'une fonction — bl, b, ptr —
    dépendait de l'ordre d'arrivée des résultats, et quelques lots étaient recompilés pour rien)."""
    communes = _rc.communes
    for a, e, o in additions:
        if a not in communes:
            _rc.funcs[a] = Function(a, e, o)
            communes.add(a)
    for a, _, _ in _rc.new:   # découvertes d'une tâche précédente de ce processus, restées hors de la liste commune
        if a not in communes:
            _rc.funcs.pop(a, None)
    _rc.new = []


def _discover(task):
    addrs, additions = task
    _apply(additions)
    _rc.stats["unimpl"] = {}
    n = 0
    for a in addrs:
        f = _rc.funcs[a]
        _rc._analyze_function(f)
        n += len(f.insns)
        f.oublier()
        # cache des instructions traduites : vidé souvent. À 1,5 million d'entrées (jusqu'à la 1.0.2), il montait à
        # ≈ 480 Mo que Python ne rendait plus au système ; il ne sert qu'entre fonctions voisines.
        if len(_rc.cache) > 150_000:
            _rc.cache.clear()
    _rc.cache.clear()
    return _rc.new, n, dict(_rc.stats["unimpl"])


def _gen(task):
    """Génère un lot (≈ 0,3 s) et dit s'il faut le compiler : l'objet est gardé si son empreinte n'a pas changé."""
    idx, chunk, additions, name, flags, keep = task
    _apply(additions)
    t0 = time.time()
    srcdir = os.path.join(SRC, name)
    objdir = os.path.join(FULL, name)
    files = _rc.write(srcdir, chunk, per_file=len(chunk) + 1, prefix=f"{name}_{idx:05d}",
                      dispatch=False)
    for a in chunk:
        _rc.funcs[a].oublier()
    _rc.cache.clear()
    src = files[0]
    obj = os.path.join(objdir, f"{name}_{idx:05d}.o")
    data = open(src, "rb").read()
    cles = build.cles_objet(flags, data)   # sans le chemin d'installation ; l'ancienne forme reste acceptée
    key = cles[0]
    a_faire = not build.objet_a_jour(obj, cles)
    if not a_faire and not keep:
        os.remove(src)
    return idx, src, obj, key, a_faire, len(data), time.time() - t0


class Compilations:
    """Compilations des lots, lancées depuis le processus principal (au plus `jobs` à la fois).

    Jusqu'à la 1.0.2, chaque processus Python de la réserve générait son lot puis attendait le compilateur : ≈ 1 Go
    de Python par tâche, en plus du compilateur (≈ 1,3 Go en moyenne avec g++, 2,5 Go pour les plus gros lots). La
    génération ne prend que ≈ 0,3 s par lot contre 8 à 50 s de compilation : quelques processus Python suffisent à
    nourrir toutes les compilations, qui ne coûtent plus que la mémoire du compilateur.
    RECOMP_TEMPS=fichier : une ligne par lot compilé (nom, octets de C++, secondes), pour repérer les lots lents.
    """

    def __init__(self, cxx, flags, jobs):
        self.cxx, self.flags, self.jobs = cxx, flags, jobs
        self.actives = []   # (processus, idx, src, obj, key, fichier d'erreurs)
        self.echecs = {}    # idx → (src, obj, key, sortie du compilateur) : repris un par un à la fin (reprendre)

    def libres(self):
        return self.jobs - len(self.actives)

    def lancer(self, idx, src, obj, key):
        err = src + ".err"
        fe = open(err, "w", encoding="utf-8", errors="replace")
        cmd = [self.cxx, *self.flags, "-c", src, "-o", obj]
        if os.environ.get("RECOMP_ESSAI_ECHEC") == os.path.basename(obj):   # essai de la reprise : ce lot échoue une fois
            cmd.insert(1, "--option-inexistante")
        p = subprocess.Popen(cmd, stdout=fe, stderr=subprocess.STDOUT)
        p.debut = time.time()
        self.actives.append((p, idx, src, obj, key, err, fe))

    def finies(self, keep):
        """(idx, obj, erreur ou None) des compilations terminées."""
        out = []
        for t in list(self.actives):
            p, idx, src, obj, key, err, fe = t
            if p.poll() is None:
                continue
            self.actives.remove(t)
            fe.close()
            if os.environ.get("RECOMP_TEMPS"):
                with open(os.environ["RECOMP_TEMPS"], "a", encoding="utf-8") as ft:
                    ft.write(f"{os.path.basename(obj)} {os.path.getsize(src)} {time.time() - p.debut:.1f}\n")
            texte = None
            if p.returncode:
                texte = open(err, encoding="utf-8", errors="replace").read()[:6000] or f"code {p.returncode}"
            else:
                open(obj + ".key", "w").write(key)
            os.remove(err)
            if texte is None and not keep:
                os.remove(src)
            if texte is not None:
                self.echecs[idx] = (src, obj, key, texte)
            out.append((idx, obj, texte))
        return out

    def reprendre(self, keep, log, nom):
        """Recompile seul, l'un après l'autre, chaque lot en échec : un échec isolé vient le plus souvent de la machine
        (mémoire prise par un autre programme à cet instant, fichier tenu par un antivirus) et non du lot. Vu le
        2026-11-11 : clang planté sur un lot de main qui, relancé seul, compile en 8 s avec 174 Mo. Rend les erreurs
        qui restent : [(idx, sortie)]."""
        restent = []
        for idx, (src, obj, key, texte) in sorted(self.echecs.items()):
            r = subprocess.run([self.cxx, *self.flags, "-c", src, "-o", obj], capture_output=True, text=True,
                               errors="replace")
            if r.returncode:
                restent.append((idx, ((r.stdout or "") + (r.stderr or ""))[:6000] or texte))
                continue
            log(f"{nom}: batch {idx} recompiled alone after it failed ({texte.strip().splitlines()[0][:120] if texte.strip() else '?'})")
            open(obj + ".key", "w").write(key)
            if not keep:
                os.remove(src)
        self.echecs = {}
        return restent

    def arreter(self):
        for p, *_ in self.actives:
            p.kill()


def build_module(name, jobs, opt, keep, insns_per_file, log=print, figer=False):
    t0 = time.time()
    os.makedirs(os.path.join(FULL, name), exist_ok=True)
    os.makedirs(os.path.join(SRC, name), exist_ok=True)
    m = Module.from_work(name)
    hk = hooked_addrs(m)
    if hk:
        log(f"{name}: {len(hk)} hooked function(s) (hooks.txt)")
    rc0 = RC(m, comments=False, trace=False, log=lambda *a: None, hooked=hk)
    known = set(rc0.funcs)
    additions = {}
    pending = sorted(known)
    cxx, compilo, flags = build.drapeaux_genere(opt)
    log(f"{name}: compiler {compilo} {[f for f in flags if f.startswith('-O')][0]} ({cxx}), "
        f"{jobs} parallel compilation(s)")
    ctx = mp.get_context("spawn")
    # découverte : chaque processus Python garde le module entier ; aucune compilation ne tourne encore
    ndisc = processus_python(min(jobs, 16))
    if ndisc < min(jobs, 16):
        log(f"{name}: discovery on {ndisc} processes (free memory)")
    with ctx.Pool(ndisc, initializer=_init, initargs=(name,)) as pool:
        rnd = 0
        total_insns = 0
        unimpl = {}
        while pending:
            parts = max(1, min(64, len(pending) // 50 or 1))   # découpage fixe : indépendant du nombre de processus
            add_list = sorted(additions.values())
            tasks = [(pending[i::parts], add_list) for i in range(parts)]
            new = {}
            for lst, n, un in pool.imap(_discover, tasks):   # dans l'ordre : résultat reproductible
                total_insns += n
                for k, v in un.items():
                    unimpl[k] = unimpl.get(k, 0) + v
                for a, e, o in lst:
                    if a not in known and a not in new:
                        new[a] = (a, e, o)
            known |= set(new)
            additions.update(new)
            pending = sorted(new)
            log(f"{name}: round {rnd}: {len(new)} functions discovered "
                f"({len(known)} total) [{time.time() - t0:.0f} s]")
            rnd += 1
        if unimpl:
            log(f"{name}: unhandled instructions: {unimpl}")
        # lots équilibrés par nombre d'instructions (approx. taille / 4)
        ends = {a: e for a, e, _ in additions.values()}
        allf = sorted(known)
        debuts = [] if figer else lots_figes(name)
        if debuts:   # découpage figé : la fonction va dans le lot dont le début la précède (lots.txt)
            chunks = [[] for _ in debuts]
            for a in allf:
                chunks[max(0, bisect.bisect_right(debuts, a) - 1)].append(a)
        else:
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
        log(f"{name}: {len(allf)} functions, {total_insns} instructions, {len(chunks)} batches"
            + (" (frozen split, lots.txt)" if debuts else ""))
    if figer:
        figer_lots(name, chunks)
        log(f"{name}: {len(chunks)} batch starts written to {LOTS} [{time.time() - t0:.0f} s]")
        return m
    add_list = sorted(additions.values())
    tasks = [(i, c, add_list, name, flags, keep) for i, c in enumerate(chunks)]
    objs, errors = [None] * len(chunks), []
    vides = {i for i, c in enumerate(chunks) if not c}   # lot figé dont toutes les fonctions ont disparu : rien à compiler
    done = len(vides)
    src_bytes = 0
    # Génération : un processus Python écrit ≈ 3 lots par seconde, de quoi nourrir une vingtaine de compilations ;
    # il y en a plus (la moitié des compilations, 8 au plus) pour les reconstructions où presque tous les objets sont
    # déjà là : la génération est alors tout le travail. Ils ne tournent que quand il manque des sources d'avance.
    ngen = processus_python(max(1, min(jobs // 2, 8)))
    comp = Compilations(cxx, flags, jobs)

    def avance():
        if done % 20 == 0 or done == len(chunks):
            log(f"{name}: {done}/{len(chunks)} batches compiled, {src_bytes / 1e6:.0f} MB of C++ "
                f"[{time.time() - t0:.0f} s]")
    # lots à générer (pile : le premier à partir est à la fin). Le dernier lot du module part en premier : c'est le
    # plus long à compiler (main : ≈ 26 s contre ≈ 8 s), et lancé en dernier il tournait seul à la fin.
    attente = [i for i in list(range(len(chunks) - 1))[::-1] + [len(chunks) - 1] if i not in vides]
    generes = []                                # (idx, src, obj, key) prêts à compiler
    with ctx.Pool(ngen, initializer=_init, initargs=(name,)) as pool:
        en_cours = {}
        try:
            while attente or en_cours or generes or comp.actives:
                rien = True
                # sources d'avance bornées : pas plus de lots générés que de compilations possibles (disque)
                while attente and len(en_cours) + len(generes) < jobs + 2 * ngen:
                    i = attente.pop()
                    en_cours[i] = pool.apply_async(_gen, (tasks[i],))
                for i, r in list(en_cours.items()):
                    if not r.ready():
                        continue
                    del en_cours[i]
                    rien = False
                    idx, src, obj, key, a_faire, nb, _ = r.get()
                    src_bytes += nb
                    if a_faire:
                        generes.append((idx, src, obj, key))
                    else:
                        objs[idx] = obj
                        done += 1
                        avance()
                while generes and comp.libres() > 0:
                    comp.lancer(*generes.pop(0))
                    rien = False
                for idx, obj, err in comp.finies(keep):
                    rien = False
                    objs[idx] = obj
                    done += 1
                    avance()
                if rien:
                    time.sleep(0.05)
        finally:
            comp.arreter()
    errors = comp.reprendre(keep, log, name)
    if errors:
        for idx, e in errors[:3]:
            log(f"--- error in batch {idx}\n{e}")
        raise RuntimeError(f"{name}: {len(errors)} batch(es) failed")
    # table de dispatch du module
    for a, e, o in additions.values():
        Recompiler._add(rc0, a, e, o)
    dsrc = rc0._write_dispatch(os.path.join(SRC, name), allf, name)
    dobj = os.path.join(FULL, name, f"{name}_dispatch.o")
    o, err = build.compile_one(cxx, dsrc, dobj, flags)
    if err:
        raise RuntimeError(f"{name}: dispatch: {err[:3000]}")
    objs = [o for o in objs if o] + [dobj]
    with open(os.path.join(FULL, name, "objs.txt"), "w") as fp:
        fp.write("\n".join(objs) + "\n")
    log(f"{name}: done, {len(objs)} objects [{time.time() - t0:.0f} s]")
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
                    print(f"romfs: {p} compressed in the .nup, ignored")
    os.makedirs(FULL, exist_ok=True)
    with open(os.path.join(FULL, "manifest.txt"), "w", encoding="utf-8") as fp:
        fp.write("\n".join(lines) + "\n")


def main():
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8", line_buffering=True)
    p = argparse.ArgumentParser()
    p.add_argument("modules", nargs="+")
    p.add_argument("-j", type=int, default=os.cpu_count())
    p.add_argument("-O", default=None, help="niveau d'optimisation (défaut : 2)")
    p.add_argument("--keep-src", action="store_true")
    p.add_argument("--insns", type=int, default=60000)
    p.add_argument("--figer-lots", action="store_true",
                   help="écrire le découpage des lots dans lots.txt (découverte seule, rien n'est compilé)")
    a = p.parse_args()
    for n in a.modules:
        build_module(n, a.j, a.O, a.keep_src, a.insns, figer=a.figer_lots)
    if not a.figer_lots:
        write_manifest(["main", "subsdk0", "subsdk1", "subsdk2", "subsdk3", "sdk"])


if __name__ == "__main__":
    main()
