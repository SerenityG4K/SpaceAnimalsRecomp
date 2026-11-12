"""Extrait tous les shaders NVN du jeu depuis sa romfs, sans jouer, pour les traduire ensuite (shaderc.py).

  python -I shaders/romfs_shaders.py [--nup <jeu.nup>] [--out build/full/shaders]
  python shaders/shaderc.py -j 12

Le .nup est lu en lecture seule, et seulement l'index de data.zip, le premier bloc de chaque entrée, puis les
paquets de shaders en entier (≈ 2,2 Go lus, 40 Mo de shaders).

romfs/data.zip (contenu « patch ») : archive ZIP64 précédée d'un en-tête « VZIP ». Noms d'entrées = empreintes
hexadécimales ; méthode 0 déclarée, mais données en blocs LZ4 chaînés de 64 Ko : u32 taille compressée (bit 31 :
bloc stocké tel quel) puis le bloc, qui peut référencer les 64 Ko décompressés précédents.

Paquet de shader (signature 0x19866891, 2 828 dans la version 2.0.0) :
  u32 signature, u32 taille du code, u32 taille de la section de contrôle, u32 0, u64 empreinte,
  puis le code (la banque constante 1 du programme se trouve entre ctl[0x40] et ctl[0x44]),
  puis la section de contrôle NVN (0x98761234 ; ctl[0x38] = taille du code proprement dit).
L'empreinte retenue par le runtime (nvn_cmd.cpp, read_stages) est sha1(code[:ctl[0x38]])[:16] : les fichiers
écrits ici (h<empreinte>_r.ctl / .bin / .cb1) se mêlent aux vidages faits en jeu (h<empreinte>_<étage>.*).
"""
import argparse
import hashlib
import os
import struct
import sys

import lz4.block

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "tools"))
import nup  # noqa: E402

PKG_MAGIC = 0x19866891
CTL_MAGIC = 0x98761234


class DataZip:
    def __init__(self, nup_path, content="patch"):
        self.N = nup.Nup(nup_path)
        ci = next(i for i in range(len(self.N.contents)) if self.N.label(i).startswith(content))
        fi = next(f for p, f in self.N.walk(ci) if p.endswith("romfs/data.zip"))
        if fi["compressed"]:
            raise IOError("data.zip compressed in the .nup: unexpected case")
        self.base, self.size = fi["offset"], fi["size"]
        self.entries = self._central_directory()

    def _rd(self, off, n):
        self.N.f.seek(self.base + off)
        return self.N.f.read(n)

    def _central_directory(self):
        tail = self._rd(self.size - 4096, 4096)
        z = tail[tail.rfind(b"PK\x06\x06"):]
        _, _, _, _, _, _, _, n_tot, cd_size, cd_off = struct.unpack_from("<IQHHIIQQQQ", z, 0)
        cd = self._rd(cd_off, cd_size)
        ents, p = [], 0
        while p < len(cd) and cd[p:p + 4] == b"PK\x01\x02":
            (_, _, _, _, _, _, _, csz, usz, nl, xl, cl, _, _, _, lho) = struct.unpack_from("<HHHHHHIIIHHHHHII", cd, p + 4)
            name = cd[p + 46:p + 46 + nl].decode("latin-1")
            x = cd[p + 46 + nl:p + 46 + nl + xl]
            q = 0
            while q + 4 <= len(x):
                xid, xs = struct.unpack_from("<HH", x, q)
                if xid == 1:   # ZIP64 : tailles et position réelles
                    vals = list(struct.unpack_from("<" + "Q" * (xs // 8), x, q + 4))
                    if usz == 0xffffffff: usz = vals.pop(0)
                    if csz == 0xffffffff: csz = vals.pop(0)
                    if lho == 0xffffffff: lho = vals.pop(0)
                q += 4 + xs
            ents.append((name, lho, csz, usz))
            p += 46 + nl + xl + cl
        if len(ents) != n_tot:
            raise IOError(f"central directory: {len(ents)} entries read out of {n_tot}")
        ents.sort(key=lambda e: e[1])
        return ents

    def _data_start(self, lho):
        h = self._rd(lho, 30)
        nl, xl = struct.unpack_from("<HH", h, 26)
        return lho + 30 + nl + xl

    def read(self, ent, limit=None):
        """Contenu décompressé d'une entrée (ou ses `limit` premiers octets, au bloc près)."""
        _, lho, csz, usz = ent
        want_total = usz if limit is None else min(usz, limit)
        start = self._data_start(lho)
        if csz == usz:   # entrée stockée telle quelle, sans blocs
            return self._rd(start, want_total)
        out = bytearray()
        p = 0
        comp = self._rd(start, csz) if limit is None else None
        while len(out) < want_total:
            if comp is None:
                cs = struct.unpack("<I", self._rd(start + p, 4))[0]
                blk = self._rd(start + p + 4, cs & 0x7fffffff)
            else:
                cs = struct.unpack_from("<I", comp, p)[0]
                blk = comp[p + 4:p + 4 + (cs & 0x7fffffff)]
            want = min(65536, usz - len(out))
            if cs & 0x80000000 or (cs & 0x7fffffff) == want:
                out += blk
            else:
                out += lz4.block.decompress(blk, uncompressed_size=want, dict=bytes(out[-65536:]))
            p += 4 + (cs & 0x7fffffff)
        return bytes(out)


def stage_kind(code):
    sph0 = struct.unpack_from("<I", code, 0)[0]
    if (sph0 & 0x1f) in (1, 2) and ((sph0 >> 5) & 0x1f) == 4:
        return {1: "vert", 2: "tesc", 3: "tese", 4: "geom", 5: "frag"}.get((sph0 >> 10) & 0xf, "?")
    return "comp"


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    ap = argparse.ArgumentParser()
    ap.add_argument("--nup", default=os.path.join(HERE, "..", "..", "StarFox [0400E88025B1A000][v131072].nup"))
    ap.add_argument("--out", default=os.path.join(HERE, "..", "build", "full", "shaders"))
    a = ap.parse_args()
    dz = DataZip(a.nup)
    os.makedirs(a.out, exist_ok=True)
    known = {f[1:17] for f in os.listdir(a.out) if f.startswith("h") and f.endswith(".bin")}
    n_pkg = n_new = n_err = 0
    kinds = {}
    for ent in dz.entries:
        if ent[3] < 0x18 + 0xe0:
            continue
        try:
            head = dz.read(ent, limit=4)
        except lz4.block.LZ4BlockError:
            n_err += 1   # entrée à un autre format (rares) : pas un paquet de shader
            continue
        if struct.unpack_from("<I", head, 0)[0] != PKG_MAGIC:
            continue
        d = dz.read(ent)
        _, csz, ksz, _ = struct.unpack_from("<IIII", d, 0)
        code, ctl = d[0x18:0x18 + csz], d[0x18 + csz:0x18 + csz + ksz]
        if len(ctl) < 0x48 or struct.unpack_from("<I", ctl, 0)[0] != CTL_MAGIC:
            continue
        n_pkg += 1
        size = struct.unpack_from("<I", ctl, 0x38)[0]
        h = hashlib.sha1(code[:size]).hexdigest()[:16]
        if h in known:
            continue
        known.add(h)
        n_new += 1
        k = stage_kind(code)
        kinds[k] = kinds.get(k, 0) + 1
        open(os.path.join(a.out, f"h{h}_r.ctl"), "wb").write(ctl)
        open(os.path.join(a.out, f"h{h}_r.bin"), "wb").write(code[:size])
        c0, c1 = struct.unpack_from("<II", ctl, 0x40)
        if c1 > c0 >= size and c1 <= len(code) and c1 - c0 <= 0x10000:
            open(os.path.join(a.out, f"h{h}_r.cb1"), "wb").write(code[c0:c1])
    print(f"{len(dz.entries)} entries in data.zip ({n_err} unreadable in LZ4), {n_pkg} shader packages, "
          f"{n_new} new stages {kinds}")


if __name__ == "__main__":
    main()
