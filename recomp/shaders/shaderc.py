#!/usr/bin/env python3
"""Traduit les shaders vidés par le jeu (RECOMP_DUMP_SHADERS) en SPIR-V.

  python shaders/shaderc.py [--dump build/full/shaders] [--out build/full/shader_cache] [-j 8] [--only HASH]

Pour chaque étage distinct : <hash>.<étage>.glsl puis <hash>.spv (glslangValidator, MSYS2 ucrt64).
Le runtime charge <hash>.spv (hash = SHA-1 du code, 16 premiers caractères). Tout est dérivé du jeu :
privé.
"""
import argparse
import collections
import concurrent.futures as cf
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import nvshader  # noqa: E402
import glsl      # noqa: E402

# glslang : RECOMP_GLSLANG, sinon celui du PATH (outils/ fournis par le launcher), sinon MSYS2
GLSLANG = (os.environ.get("RECOMP_GLSLANG") or shutil.which("glslangValidator")
           or r"C:\msys64\ucrt64\bin\glslangValidator.exe")
SPIRV_VAL = shutil.which("spirv-val") or r"C:\msys64\ucrt64\bin\spirv-val.exe"
STAGE_EXT = {"vert": "vert", "frag": "frag", "comp": "comp", "geom": "geom", "tesc": "tesc", "tese": "tese"}


def work(args):
    st, out = args
    base = os.path.join(out, st.hash)
    try:
        src, em = glsl.translate(st)
    except glsl.Unsupported as e:
        return st, "unsupported", str(e)
    gl = f"{base}.{STAGE_EXT[st.kind]}.glsl"
    open(gl, "w", encoding="utf-8", newline="\n").write(src)
    spv = base + ".spv"
    r = subprocess.run([GLSLANG, "-V", "--target-env", "vulkan1.3", "-S", STAGE_EXT[st.kind], gl, "-o", spv],
                       capture_output=True, text=True)
    if r.returncode:
        msg = (r.stdout + r.stderr).strip().splitlines()
        err = next((l for l in msg if "ERROR" in l), msg[-1] if msg else "?")
        return st, "glslang", err[:300]
    open(base + ".meta", "w", encoding="utf-8", newline="\n").write(glsl.metadata(em))
    return st, "ok", "; ".join(sorted(set(em.warn)))[:200]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dump", default=os.path.join(HERE, "..", "build", "full", "shaders"))
    ap.add_argument("--out", default=os.path.join(HERE, "..", "build", "full", "shader_cache"))
    ap.add_argument("-j", type=int, default=os.cpu_count())
    ap.add_argument("--only")
    ap.add_argument("--kinds", default="vert,frag,comp")
    ap.add_argument("--nouveaux", action="store_true",
                    help="ne traduit que les étages sans SPIR-V (vidés en jeu depuis la dernière traduction)")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    stages = [s for s in nvshader.load_dump(a.dump) if s.kind in a.kinds.split(",")]
    if a.only:
        stages = [s for s in stages if s.hash.startswith(a.only) or s.name == a.only]
    if a.nouveaux:
        stages = [s for s in stages if not os.path.exists(os.path.join(a.out, s.hash + ".spv"))]
    res = collections.Counter()
    reasons = collections.Counter()
    by_kind = collections.Counter()
    ex = {}
    with cf.ThreadPoolExecutor(a.j) as pool:
        for st, status, msg in pool.map(work, [(s, a.out) for s in stages]):
            res[status] += 1
            by_kind[(st.kind, status)] += 1
            if status != "ok":
                key = msg.split("(")[0][:90] if status == "unsupported" else msg[:120]
                reasons[(status, key)] += 1
                ex.setdefault((status, key), st.name)
            elif msg and a.only:
                print("warnings:", msg)
    print(f"{len(stages)} stages: " + ", ".join(f"{k} {v}" for k, v in res.items()))
    print("by type: " + ", ".join(f"{k}/{s} {v}" for (k, s), v in sorted(by_kind.items())))
    for (status, key), n in reasons.most_common(40):
        print(f"  {n:4d}  [{status}] {key}   (ex. {ex[(status, key)]})")


if __name__ == "__main__":
    main()
