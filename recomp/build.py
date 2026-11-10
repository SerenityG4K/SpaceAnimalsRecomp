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
    raise RuntimeError("aucun compilateur C++ trouvé (g++/clang++)")


# En-têtes inclus par le code généré : seuls eux invalident le cache des objets générés.
GEN_HEADERS = ("cpu.h", "simd.h")


def headers_digest():
    return b"".join(open(os.path.join(RUNTIME, h), "rb").read() for h in GEN_HEADERS)


def compile_one(cxx, src, obj, flags):
    extra = b""
    osdir = os.path.join(RUNTIME, "os")
    if os.path.dirname(os.path.abspath(src)) == osdir:
        # couche hôte (runtime/os) : dépend aussi de ses propres en-têtes
        extra = b"".join(open(os.path.join(osdir, h), "rb").read()
                         for h in sorted(os.listdir(osdir)) if h.endswith(".h"))
    key = hashlib.sha1((" ".join(flags) + "\0").encode() + open(src, "rb").read() +
                       headers_digest() + extra).hexdigest()
    stamp = obj + ".key"
    if os.path.exists(obj) and os.path.exists(stamp) and open(stamp).read() == key:
        return obj, None
    r = subprocess.run([cxx, *flags, "-c", src, "-o", obj], capture_output=True, text=True)
    if r.returncode:
        return obj, r.stderr
    open(stamp, "w").write(key)
    return obj, None


def build(srcs, out, objdir, opt="-O2", defs=(), jobs=None, kind="dll", extra_link=(), log=print):
    cxx = find_cxx()
    os.makedirs(objdir, exist_ok=True)
    flags = BASE_FLAGS + [opt] + list(defs)
    srcs = list(srcs) + [os.path.join(RUNTIME, "runtime.cpp"), os.path.join(RUNTIME, "fp.cpp")]
    jobs = jobs or os.cpu_count()
    objs, errors = [], []
    with cf.ThreadPoolExecutor(jobs) as ex:
        futs = []
        for s in srcs:
            o = os.path.join(objdir, os.path.splitext(os.path.basename(s))[0] + ".o")
            futs.append(ex.submit(compile_one, cxx, s, o, flags))
        for f in futs:
            o, err = f.result()
            objs.append(o)
            if err:
                errors.append((o, err))
    if errors:
        for o, e in errors[:5]:
            log(f"--- erreur de compilation {o}\n{e[:4000]}")
        raise RuntimeError(f"{len(errors)} fichier(s) en erreur")
    if kind == "objs":
        return objs
    link = [cxx, "-o", out, *objs, "-static", "-static-libgcc", "-static-libstdc++", "-latomic",
            *extra_link]
    if kind == "dll":
        link.insert(1, "-shared")
    r = subprocess.run(link, capture_output=True, text=True)
    if r.returncode:
        raise RuntimeError("édition de liens : " + r.stderr[:4000])
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
    print(f"OK {r if kind != 'objs' else str(len(r)) + ' objets'} [{time.time() - t0:.1f} s]")


if __name__ == "__main__":
    sys.exit(main())
