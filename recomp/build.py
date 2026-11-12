"""Compilation du C++ généré avec g++ (mingw) : compilation parallèle avec cache par fichier.

  python build.py <dossier .cpp> <sortie.dll|.a|.exe|objs> [--bench] [-O2] [-j N]

Utilisé aussi comme module par le banc (bench/harness.py)."""
import concurrent.futures as cf
import hashlib
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
RUNTIME = os.path.join(HERE, "runtime")

BASE_FLAGS = ["-std=c++17", "-fno-strict-aliasing", "-ffp-contract=off", "-mcx16", "-march=x86-64-v3",
              "-fwrapv", "-I" + RUNTIME, "-Wno-unused-label", "-Wno-unused-variable",
              "-Wno-unused-but-set-variable"]
BENCH_DEFS = ["-DRECOMP_BENCH", "-DRECOMP_TRACE", "-DRECOMP_CHECK_RET"]


def find_cxx():
    for c in ("g++", "clang++"):
        p = shutil.which(c)
        if p:
            return p
    raise RuntimeError("no C++ compiler found (g++/clang++)")


# En-têtes inclus par le code généré : seuls eux invalident le cache des objets générés.
GEN_HEADERS = ("cpu.h", "simd.h")


def compilateur_genere():
    """Compilateur du code généré (≈ 850 lots de main) : (chemin, nom, drapeaux propres).

    clang++ compile ces lots deux fois plus vite que g++ à optimisation égale (-O2 : ≈ 12 s par lot au lieu de ≈ 25 s,
    mesure du 2026-11-11). Il est pris s'il est fourni à côté de g++ (outils du paquet) ; RECOMP_CXX=chemin impose un
    compilateur (clang++ ou g++). Le runtime et l'édition de liens restent à g++ (link.py) : même ABI (mingw, UCRT).
    Les drapeaux propres à clang changent l'empreinte des objets : passer d'un compilateur à l'autre recompile tout.
    """
    choix = os.environ.get("RECOMP_CXX")
    if not choix:
        gxx = shutil.which("g++")
        clang = os.path.join(os.path.dirname(gxx), "clang++.exe") if gxx else None
        choix = clang if clang and os.path.exists(clang) else (gxx or find_cxx())
    nom = "clang" if "clang" in os.path.basename(choix).lower() else "gcc"
    if nom == "gcc":
        return choix, nom, []
    # [[clang::musttail]] exige la signature de l'appelant : clang_compat.h fait passer la cible des sauts indirects
    # par l'état CPU (rt_tailjump) ; -w : le code généré n'a pas à produire d'avertissements
    return choix, nom, ["--target=x86_64-w64-windows-gnu", "-include", os.path.join(RUNTIME, "clang_compat.h"), "-w"]


def drapeaux_genere(opt=None):
    """(compilateur, nom, drapeaux) du code généré à l'optimisation -O<opt> (défaut : -O2 ; RECOMP_OPT=n l'impose).

    Mesures du 2026-11-11 (temps processeur du jeu par image, images 600 à 2400 ; construction complète sur 16 fils) :
    g++ -O2 14,9 à 15,6 ms, 66 min ; clang -O2 14,1 à 14,2 ms, 21,6 min ; clang -O1 16,6 à 16,9 ms, 16,7 min. Les
    autres recomps de la Switch 2 compilent en -O1 et gardent -O2 pour le code chaud (profil d'une partie) ; sans
    profil, -O1 partout coûte ≈ 17 % de processeur en jeu pour 5 min de construction gagnées une seule fois : -O2.
    """
    cxx, nom, propres = compilateur_genere()
    opt = opt or os.environ.get("RECOMP_OPT") or "2"
    return cxx, nom, BASE_FLAGS + ["-O" + str(opt)] + propres


def headers_digest(flags=()):
    """En-têtes du code généré, plus clang_compat.h quand clang l'inclut (drapeaux du compilateur)."""
    hs = GEN_HEADERS + (("clang_compat.h",) if any(f.endswith("clang_compat.h") for f in flags) else ())
    return b"".join(open(os.path.join(RUNTIME, h), "rb").read() for h in hs)


def cles_objet(flags, data, extra=b""):
    """Empreintes d'un objet : (actuelle, ancienne).

    L'empreinte porte sur les drapeaux, la source et les en-têtes. Jusqu'à la 1.0.2, les drapeaux y entraient tels
    quels, donc avec le chemin du dossier d'installation (-I…\\recomp\\runtime) : un joueur qui déplaçait ou renommait
    son dossier recompilait tout à la construction suivante. L'empreinte actuelle remplace ce chemin par « <runtime> ».
    L'ancienne (chemin compris) reste acceptée pour les objets déjà compilés : objet_a_jour la remplace sans recompiler.
    """
    neutres = [f.replace(RUNTIME, "<runtime>") for f in flags]
    fin = data + headers_digest(flags) + extra
    return (hashlib.sha1((" ".join(neutres) + "\0").encode() + fin).hexdigest(),
            hashlib.sha1((" ".join(flags) + "\0").encode() + fin).hexdigest())


def objet_a_jour(obj, cles):
    """L'objet existe-t-il avec une des deux empreintes ? Une ancienne empreinte est remplacée par l'actuelle."""
    stamp = obj + ".key"
    if not (os.path.exists(obj) and os.path.exists(stamp)):
        return False
    vue = open(stamp).read()
    if vue == cles[0]:
        return True
    if vue == cles[1]:
        open(stamp, "w").write(cles[0])
        return True
    return False


def compile_one(cxx, src, obj, flags):
    extra = b""
    osdir = os.path.join(RUNTIME, "os")
    if os.path.dirname(os.path.abspath(src)) == osdir:
        # couche hôte (runtime/os) : dépend aussi de ses propres en-têtes
        extra = b"".join(open(os.path.join(osdir, h), "rb").read()
                         for h in sorted(os.listdir(osdir)) if h.endswith(".h"))
    cles = cles_objet(flags, open(src, "rb").read(), extra)
    key = cles[0]
    stamp = obj + ".key"
    if objet_a_jour(obj, cles):
        return obj, None
    r = subprocess.run([cxx, *flags, "-c", src, "-o", obj], capture_output=True, text=True)
    if r.returncode:
        return obj, r.stderr
    open(stamp, "w").write(key)
    return obj, None


def build(srcs, out, objdir, opt="-O2", defs=(), jobs=None, kind="dll", extra_link=(), log=print):
    # code généré : le compilateur des lots du jeu (clang s'il est fourni, RECOMP_CXX) ; runtime : g++, comme dans
    # l'exécutable du jeu (link.py)
    cxx, nom, propres = compilateur_genere()
    gxx = shutil.which("g++") or cxx
    os.makedirs(objdir, exist_ok=True)
    flags = BASE_FLAGS + [opt] + list(defs) + propres
    rt_flags = BASE_FLAGS + [opt] + list(defs)
    runtime = [os.path.join(RUNTIME, "runtime.cpp"), os.path.join(RUNTIME, "fp.cpp")]
    srcs = list(srcs) + runtime
    jobs = jobs or os.cpu_count()
    objs, errors = [], []
    with cf.ThreadPoolExecutor(jobs) as ex:
        futs = []
        for s in srcs:
            o = os.path.join(objdir, os.path.splitext(os.path.basename(s))[0] + ".o")
            futs.append(ex.submit(compile_one, gxx if s in runtime else cxx, s, o, rt_flags if s in runtime else flags))
        for f in futs:
            o, err = f.result()
            objs.append(o)
            if err:
                errors.append((o, err))
    if errors:
        for o, e in errors[:5]:
            log(f"--- compilation error {o}\n{e[:4000]}")
        raise RuntimeError(f"{len(errors)} file(s) failed")
    if kind == "objs":
        return objs
    link = [shutil.which("g++") or cxx, "-o", out, *objs, "-static", "-static-libgcc", "-static-libstdc++", "-latomic",
            *extra_link]
    if kind == "dll":
        link.insert(1, "-shared")
    r = subprocess.run(link, capture_output=True, text=True)
    if r.returncode:
        raise RuntimeError("link: " + r.stderr[:4000])
    return out


def main():
    import argparse
    p = argparse.ArgumentParser()
    p.add_argument("srcdir")
    p.add_argument("out")
    p.add_argument("--bench", action="store_true")
    p.add_argument("-O", default="2")
    p.add_argument("-j", type=int, default=None)
    a = p.parse_args()
    srcs = sorted(os.path.join(a.srcdir, f) for f in os.listdir(a.srcdir) if f.endswith(".cpp"))
    kind = "objs" if a.out == "objs" else ("dll" if a.out.endswith(".dll") else "exe")
    objdir = os.path.join(HERE, "build", "obj", os.path.basename(os.path.normpath(a.srcdir)) +
                          ("_bench" if a.bench else "") + f"_O{a.O}")
    import time
    t0 = time.time()
    r = build(srcs, a.out, objdir, opt="-O" + a.O, defs=BENCH_DEFS if a.bench else (), jobs=a.j,
              kind=kind)
    print(f"OK {r if kind != 'objs' else str(len(r)) + ' objects'} [{time.time() - t0:.1f} s]")


if __name__ == "__main__":
    sys.exit(main())
