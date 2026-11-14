#!/usr/bin/env python3
"""Compile les passes du DLSS (runtime/os/dlss_avant.comp, dlss_apres.comp, dlss_composer.comp) en SPIR-V et écrit
runtime/os/dlss_spirv.h, inclus par vk_render.cpp. À relancer après une modification d'un de ces fichiers ;
dlss_spirv.h est rangé avec les sources (les joueurs n'ont pas à le refaire).

Rangé ici et non dans recomp/shaders/ : tout changement d'un script de ce dossier-là fait refaire l'étape « shaders »
de la construction (extraction et traduction des 2 388 shaders du jeu) chez chaque joueur à la mise à jour ; un .py
d'ici ne compte dans aucune signature d'étape (preparer.py, DEPENDANCES).

  python runtime/os/dlss_spirv.py

glslang : RECOMP_GLSLANG, sinon celui du PATH, sinon outils/glslang/bin/glslang.exe (fourni par le launcher).
"""
import os
import shutil
import struct
import subprocess
import sys
import tempfile

OS_DIR = os.path.dirname(os.path.abspath(__file__))
RACINE = os.path.dirname(os.path.dirname(OS_DIR))
PASSES = [("dlss_avant.comp", "DLSS_AVANT_SPV"), ("dlss_apres.comp", "DLSS_APRES_SPV"),
          ("dlss_composer.comp", "DLSS_COMPOSER_SPV")]


def glslang():
    for c in (os.environ.get("RECOMP_GLSLANG"), shutil.which("glslangValidator"), shutil.which("glslang"),
              os.path.join(RACINE, "outils", "glslang", "bin", "glslang.exe")):
        if c and os.path.exists(c):
            return c
    raise SystemExit("glslang introuvable (RECOMP_GLSLANG)")


def main():
    g = glslang()
    lignes = ["// Produit par runtime/os/dlss_spirv.py à partir de dlss_avant.comp, dlss_apres.comp et dlss_composer.comp : "
              "ne pas modifier à la main.",
              "#pragma once", "#include <cstdint>", ""]
    with tempfile.TemporaryDirectory() as tmp:
        for src, nom in PASSES:
            sortie = os.path.join(tmp, src + ".spv")
            r = subprocess.run([g, "-V", "--target-env", "vulkan1.3", "-o", sortie, os.path.join(OS_DIR, src)],
                               capture_output=True, text=True)
            if r.returncode != 0:
                raise SystemExit(f"{src} : échec de glslang\n{r.stdout}{r.stderr}")
            data = open(sortie, "rb").read()
            if len(data) % 4 or data[:4] != b"\x03\x02\x23\x07":
                raise SystemExit(f"{src} : SPIR-V inattendu")
            mots = struct.unpack(f"<{len(data) // 4}I", data)
            lignes.append(f"// {src} : {len(mots)} mots")
            lignes.append(f"static const uint32_t {nom}[] = {{")
            for i in range(0, len(mots), 8):
                lignes.append("    " + ", ".join(f"0x{m:08x}" for m in mots[i:i + 8]) + ",")
            lignes.append("};")
            lignes.append("")
    cible = os.path.join(OS_DIR, "dlss_spirv.h")
    with open(cible, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lignes))
    print(f"écrit : {cible}")


if __name__ == "__main__":
    sys.exit(main())
