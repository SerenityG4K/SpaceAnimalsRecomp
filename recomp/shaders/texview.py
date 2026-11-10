#!/usr/bin/env python3
"""Décode une texture « packagée » vidée par RECOMP_TEX_DUMP (build/full/tex_dump/*.bin) en PNG.

  python shaders/texview.py <fichier.bin> [sortie.png] [--niveaux N]

Même disposition block-linear que le runtime (runtime/os/vk_render.cpp, upload_packaged), établie
sur les données du jeu :
  * GOB de 64 o × 8 lignes ; l'unité de 16 o (octets x[0..3]) est rangée selon y0 (bit 4), y1 (bit 5),
    x4 (bit 6), y2 (bit 7), x5 (bit 8) ;
  * blocs de bh GOB de haut en colonnes d'un GOB de large ; bh d'un niveau = min(2^L, plus petite
    puissance de 2 couvrant le niveau, 16 au plus), L = 1er octet de NVNpackagedTextureLayout ;
  * niveaux à la suite (largeur alignée sur 64 o, hauteur sur 8·bh lignes).
Formats : BC1-BC7 (décodés par Pillow), RGBA8. Le fichier vidé et le PNG sont des données du jeu :
à garder privés (build/).
"""
import os
import re
import sys

import numpy as np
from PIL import Image

BC = {65: (1, 8), 66: (1, 8), 67: (2, 16), 68: (3, 16), 69: (1, 8), 70: (1, 8), 71: (2, 16), 72: (3, 16),
      73: (4, 8), 74: (4, 8), 75: (5, 16), 76: (5, 16), 77: (7, 16), 78: (7, 16), 79: (6, 16), 80: (6, 16)}
RGBA8 = {37, 55, 56}


def std_bh(rows):
    bh = 16
    while bh > 1 and rows <= (bh // 2) * 8:
        bh //= 2
    return bh


def offsets(wbytes, rows, bh):
    """Adresse de chaque unité de 16 o (rows × ceil(wbytes/16)) dans un niveau block-linear."""
    wg = (wbytes + 63) // 64
    ys = np.arange(rows)[:, None]
    xs = (np.arange((wbytes + 15) // 16) * 16)[None, :]
    g = ((ys & 1) << 4) | (((ys >> 1) & 1) << 5) | (((xs >> 4) & 1) << 6) | (((ys >> 2) & 1) << 7) | (((xs >> 5) & 1) << 8)
    return (ys // (8 * bh)) * wg * 512 * bh + (xs // 64) * 512 * bh + ((ys % (8 * bh)) // 8) * 512 + g


def level(data, base, w, h, fmt, l0):
    unit, bpu = (4, BC[fmt][1]) if fmt in BC else (1, 4)
    wbytes, rows = (w + unit - 1) // unit * bpu, (h + unit - 1) // unit
    bh = min(1 << min(l0, 4), std_bh(rows))
    off = base + offsets(wbytes, rows, bh)
    idx = (off[..., None] + np.arange(16)[None, None, :]).reshape(rows, -1)[:, :wbytes]
    raw = data[idx].tobytes()
    pw, ph = (w + unit - 1) // unit * unit, (h + unit - 1) // unit * unit
    if fmt in BC:
        n = BC[fmt][0]
        im = Image.frombytes("L" if n == 4 else "RGBA", (pw, ph), raw, "bcn", n)
    else:
        im = Image.frombytes("RGBA", (pw, ph), raw)
    size = (wbytes + 63) // 64 * 64 * ((rows + 8 * bh - 1) // (8 * bh) * 8 * bh)
    return im.crop((0, 0, w, h)), size


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    nlev = 1
    if "--niveaux" in sys.argv:
        nlev = int(sys.argv[sys.argv.index("--niveaux") + 1])
        args = [a for a in args if a != str(nlev)]
    src = args[0]
    out = args[1] if len(args) > 1 else os.path.splitext(src)[0] + ".png"
    m = re.search(r"_(\d+)x(\d+)_f(\d+)_n(\d+)_L([0-9a-f]{2})", os.path.basename(src))
    if not m:
        sys.exit("nom attendu : <clé>_<l>x<h>_f<format>_n<niveaux>_L<disposition>.bin")
    w, h, fmt, lv, l0 = int(m[1]), int(m[2]), int(m[3]), int(m[4]), int(m[5], 16)
    if fmt not in BC and fmt not in RGBA8:
        sys.exit(f"format NVN {fmt} non géré par cet outil")
    data = np.frombuffer(open(src, "rb").read(), dtype=np.uint8)
    base, tiles = 0, []
    for k in range(min(nlev, lv)):
        im, size = level(data, base, max(1, w >> k), max(1, h >> k), fmt, l0)
        tiles.append(im.convert("RGBA"))
        base += size
    canvas = Image.new("RGBA", (sum(t.width for t in tiles), tiles[0].height))
    x = 0
    for t in tiles:
        canvas.paste(t, (x, 0))
        x += t.width
    canvas.save(out)
    print(f"{out} : {w}x{h} format {fmt}, {len(tiles)} niveau(x), disposition L={l0}")


if __name__ == "__main__":
    main()
