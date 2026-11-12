"""Trace des fichiers du GCC fourni réellement utilisés par la construction du jeu.

Lance de vraies commandes de la construction (mêmes options que build.py / fullbuild.py / link.py) avec le GCC fourni
seul dans le PATH, et relève : programmes appelés (-v), en-têtes lus (-MD), entrées de l'éditeur de liens
(-Wl,--trace), commande de windres (-v), puis la fermeture des DLL importées par ces programmes (tables d'import PE).
Le travail (dont l'exécutable lié) se fait dans un dossier temporaire, jamais dans build/full.

  python trace_gcc.py <racine du GCC fourni, sans espaces> [sortie.txt]     (sortie : verif/gcc_utilises.txt)

À relancer après un changement de GCC, des options de compilation ou des bibliothèques liées, puis verifier.py rapide.
Il faut un jeu construit (objets de build/full) : l'édition de liens complète est refaite.
"""
import concurrent.futures as cf
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile

ICI = os.path.dirname(os.path.abspath(__file__))
RECOMP = os.path.dirname(ICI)
GCC = os.path.abspath(sys.argv[1])
SORTIE = sys.argv[2] if len(sys.argv) > 2 else os.path.join(ICI, "gcc_utilises.txt")
TRAV = os.path.join(tempfile.gettempdir(), "sar_trace_gcc")
shutil.rmtree(TRAV, ignore_errors=True)
os.makedirs(TRAV)

sys.path.insert(0, RECOMP)
import build  # noqa: E402

env = dict(os.environ)
env["PATH"] = os.pathsep.join([os.path.join(GCC, "bin"), r"C:\Windows\System32", r"C:\Windows"])
for k in ("CPATH", "C_INCLUDE_PATH", "CPLUS_INCLUDE_PATH", "LIBRARY_PATH", "GCC_EXEC_PREFIX", "COMPILER_PATH"):
    env.pop(k, None)

utilises = set()      # chemins absolus normalisés
journal = []


def note(p):
    p = os.path.normcase(os.path.normpath(p))
    if p.startswith(os.path.normcase(GCC) + os.sep):
        utilises.add(p)


def lancer(cmd, cwd=None):
    r = subprocess.run(cmd, cwd=cwd, env=env, capture_output=True, text=True, errors="replace")
    sortie = r.stdout + r.stderr
    journal.append("$ " + " ".join(cmd)[:300] + "\n" + sortie[-4000:])
    if r.returncode:
        raise SystemExit(f"ÉCHEC ({r.returncode}) : {' '.join(cmd)[:300]}\n{sortie[-3000:]}")
    return sortie


def programmes(sortie):
    """Programmes et greffons cités par -v (cc1plus, as, collect2, liblto_plugin, lto-wrapper…)."""
    for m in re.finditer(r"([A-Za-z]:[\\/][^\s\"']+?\.(?:exe|dll))", sortie):
        note(m.group(1))


def deps(fichier_d):
    texte = open(fichier_d, encoding="utf-8", errors="replace").read().replace("\\\n", " ")
    for tok in re.split(r"(?<!\\)\s+", texte.split(":", 1)[1] if ":" in texte[2:] else texte):
        tok = tok.replace("\\ ", " ").strip()
        if tok:
            note(tok)


def compiler(src, flags, nom):
    obj = os.path.join(TRAV, nom + ".o")
    d = os.path.join(TRAV, nom + ".d")
    s = lancer(["g++", *flags, "-v", "-MD", "-MF", d, "-c", src, "-o", obj])
    programmes(s)
    deps(d)
    return obj


# 1. runtime (options de link.py)
rt_flags = build.BASE_FLAGS + ["-O2", "-g1", "-Wall", "-Wno-unused-function",
                               "-I" + os.path.join(build.RUNTIME, "third_party"),
                               "-D_WIN32_WINNT=0x0A00", "-DWINVER=0x0A00", "-Wa,-muse-unaligned-vector-move"]
rt = [os.path.join(build.RUNTIME, "runtime.cpp"), os.path.join(build.RUNTIME, "fp.cpp")]
osdir = os.path.join(build.RUNTIME, "os")
rt += sorted(os.path.join(osdir, f) for f in os.listdir(osdir) if f.endswith(".cpp"))
# 2. code généré (options de fullbuild.py) : subsdk3, petit module, généré ici
gen = os.path.join(TRAV, "gen")
r = subprocess.run([sys.executable, "-X", "utf8", os.path.join(RECOMP, "recomp.py"), "gen", "subsdk3", "--out", gen,
                    "--no-comments"], capture_output=True, text=True, errors="replace")
if r.returncode:
    raise SystemExit("génération subsdk3 : " + r.stderr[-2000:])
gens = sorted(os.path.join(gen, f) for f in os.listdir(gen) if f.endswith(".cpp"))
gen_flags = build.BASE_FLAGS + ["-O2"]
with cf.ThreadPoolExecutor(os.cpu_count()) as ex:
    futs = [ex.submit(compiler, s, rt_flags, "rt_" + os.path.splitext(os.path.basename(s))[0]) for s in rt]
    futs += [ex.submit(compiler, s, gen_flags, "gen_" + os.path.splitext(os.path.basename(s))[0]) for s in gens]
    for f in futs:
        f.result()
print(f"compilations : {len(rt)} fichiers du runtime, {len(gens)} fichiers générés", flush=True)

# 3. windres (comme link.py : dans le dossier des objets, chemin relatif vers l'icône)
rc_dir = os.path.join(TRAV, "rt")
os.makedirs(rc_dir)
ico = os.path.join(RECOMP, "build", "full", "icone.ico")
shutil.copy(ico, os.path.join(TRAV, "icone.ico"))
open(os.path.join(rc_dir, "icone.rc"), "w", encoding="utf-8").write('1 ICON "../icone.ico"\n')
s = lancer(["windres", "-v", "icone.rc", "-O", "coff", "-o", "icone_res.o"], cwd=rc_dir)
programmes(s)
for m in re.finditer(r"(?:using|preprocessor)[^\n]*?\b([A-Za-z0-9_.+-]*(?:gcc|cpp)(?:\.exe)?)\b", s):
    pass
print("windres :", " | ".join(l for l in s.splitlines() if "gcc" in l or "cpp" in l)[:400], flush=True)
# windres appelle « gcc -E » : refaire cette commande avec -v pour voir cc1
s = lancer(["gcc", "-v", "-E", "-xc", "-DRC_INVOKED", os.path.join(rc_dir, "icone.rc"), "-o", os.path.join(TRAV, "rc.i")])
programmes(s)

# 4. édition de liens complète (objets de build/full, lus seulement ; sortie ici)
rsp = os.path.join(TRAV, "link.rsp")
lignes = [l for l in open(os.path.join(RECOMP, "build", "full", "link.rsp"), encoding="utf-8") if l.strip()]
open(rsp, "w", encoding="utf-8").writelines(lignes)
cmd = ["g++", "-v", "-o", os.path.join(TRAV, "lie.exe"), "@" + rsp, "-static", "-static-libgcc", "-static-libstdc++",
       "-latomic", "-lwinmm", "-lgdi32", "-luser32", "-lole32", "-lmfplat", "-lmfreadwrite", "-lmfuuid",
       "-Wl,--disable-high-entropy-va", "-Wl,--trace", "-Wl,-v"]
s = lancer(cmd)
programmes(s)
for l in s.splitlines():
    l = l.strip()
    m = re.match(r"^([A-Za-z]:[\\/].+?)(?:\((.+)\))?$", l)
    if m and os.path.exists(m.group(1)):
        note(m.group(1))
print("édition de liens : OK", flush=True)

# 5. greffon LTO et programmes cherchés par collect2 (ld réel)
for l in s.splitlines():
    if "collect2" in l:
        for m in re.finditer(r"-plugin(?:-opt=|\s+)([^\s]+)", l):
            note(m.group(1))

# 6. fermeture des DLL (tables d'import PE)
def imports_pe(p):
    b = open(p, "rb").read()
    pe = struct.unpack_from("<I", b, 0x3C)[0]
    nsec = struct.unpack_from("<H", b, pe + 6)[0]
    opt = pe + 24
    magic = struct.unpack_from("<H", b, opt)[0]
    dd = opt + (112 if magic == 0x20B else 96)
    rva_imp = struct.unpack_from("<I", b, dd + 8)[0]
    secs = []
    so = opt + struct.unpack_from("<H", b, pe + 20)[0]
    for i in range(nsec):
        vs, va, _, raw = struct.unpack_from("<IIII", b, so + i * 40 + 8)   # VirtualSize, VirtualAddress, SizeOfRaw, PointerToRaw
        secs.append((va, vs, raw))

    def off(rva):
        for va, vs, raw in secs:
            if va <= rva < va + max(vs, 1):
                return rva - va + raw
        return None
    noms = []
    o = off(rva_imp) if rva_imp else None
    while o is not None:
        champs = struct.unpack_from("<IIIII", b, o)
        if not any(champs):
            break
        n = off(champs[3])
        noms.append(b[n:b.index(b"\0", n)].decode("ascii", "replace"))
        o += 20
    return noms


bin_dir = os.path.join(GCC, "bin")
a_voir = [p for p in list(utilises) if p.endswith((".exe", ".dll"))]
vus = set()
while a_voir:
    p = a_voir.pop()
    if p in vus or not os.path.exists(p):
        continue
    vus.add(p)
    for nom in imports_pe(p):
        for d in (os.path.dirname(p), bin_dir):
            c = os.path.normcase(os.path.join(d, nom))
            if os.path.exists(c):
                note(c)
                a_voir.append(c)
                break

rel = sorted(os.path.relpath(p, os.path.normcase(GCC)).replace("\\", "/") for p in utilises)
entete = ("# Fichiers du GCC fourni que la construction du jeu utilise, relevés par verif/trace_gcc.py (programmes appelés,\n"
          "# en-têtes lus, entrées de l'éditeur de liens, DLL importées). Chemins relatifs à outils/gcc.\n")
open(SORTIE, "w", encoding="utf-8", newline="\n").write(entete + "\n".join(rel) + "\n")
open(os.path.join(TRAV, "trace_journal.txt"), "w", encoding="utf-8").write("\n\n".join(journal))
for f in os.listdir(TRAV):   # code généré, objets, exécutable et icône (dérivés du jeu) : ne pas les laisser traîner
    if f != "trace_journal.txt":
        p = os.path.join(TRAV, f)
        shutil.rmtree(p, ignore_errors=True) if os.path.isdir(p) else os.remove(p)
taille = sum(os.path.getsize(os.path.join(GCC, r)) for r in rel)
print(f"{len(rel)} fichiers utilisés, {taille / 1e6:.1f} Mo → {SORTIE}")
