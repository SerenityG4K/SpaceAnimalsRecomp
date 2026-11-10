#!/usr/bin/env python3
"""Lecteur / extracteur du conteneur « NUP » (dump Switch 2 déjà déchiffré).

Format déduit de l'analyse du fichier (tout en little-endian) :

  0x000  "NUP\\x01"  u16 ?  u16 0x80 (taille en-tête)   u32 0x80 (offset TOC)
  0x010  u64 taille du TOC          ; 0x018 sha256 du TOC
  0x038  u64 début des données      ; 0x040 u64 taille totale du fichier
  0x048  u32 nombre de contenus     ; 0x050 16 octets (id ?)
  0x080  TOC : "TOC\\x01" u32 version u32 nb_dossiers u32 0
               u64 nb_fichiers  u64 offset table de chaînes (relatif au TOC)
               u64 taille table de chaînes  u64 0
  puis   contenus  (0x40 o.) : u64 title_id, u32 dossier racine, u32 dossier exefs,
                               u32 dossier romfs, u32 version, u32 type (0=base, 1=patch),
                               u32 0, u64 offset méta, u64 taille méta, 16 o. réservés
  puis   dossiers  (0x18 o.) : u32 nom_off, u32 nom_len, u32 1er enfant, u32 nb enfants, 8 o.
  puis   fichiers  (0x50 o.) : u32 nom_off, u32 nom_len, u64 taille, u64 offset,
                               u64 taille stockée, sha256[32], u64 compressé (zstd), u64 0
  puis   tableau d'enfants u32 : n < nb_dossiers -> dossier n ; sinon fichier n - nb_dossiers
  puis   table de chaînes (noms concaténés, sans terminateur)
"""
import argparse
import fnmatch
import hashlib
import os
import struct
import sys

import zstandard

CHUNK = 8 * 1024 * 1024


class Nup:
    def __init__(self, path):
        self.path = path
        self.f = open(path, "rb")
        head = self.f.read(0x80)
        if head[:4] != b"NUP\x01":
            raise ValueError("pas un fichier NUP")
        toc_off = struct.unpack_from("<I", head, 0x08)[0]
        self.toc_size = struct.unpack_from("<Q", head, 0x10)[0]
        self.data_start = struct.unpack_from("<Q", head, 0x38)[0]
        self.total_size = struct.unpack_from("<Q", head, 0x40)[0]
        n_contents = struct.unpack_from("<I", head, 0x48)[0]

        self.f.seek(toc_off)
        toc = self.f.read(self.toc_size)
        if toc[:4] != b"TOC\x01":
            raise ValueError("TOC introuvable")
        _ver, n_dirs = struct.unpack_from("<II", toc, 4)
        n_files, str_off, str_size = struct.unpack_from("<QQQ", toc, 0x10)
        strtab = toc[str_off:str_off + str_size]
        name = lambda o, l: strtab[o:o + l].decode("utf-8")

        pos = 0x30
        self.contents = []
        for _ in range(n_contents):
            tid, root, exefs, romfs, version, ctype, _r, moff, msize = struct.unpack_from(
                "<QIIIIIIQQ", toc, pos)
            self.contents.append(dict(title_id=tid, root=root, exefs=exefs, romfs=romfs,
                                      version=version, type=ctype, meta_off=moff, meta_size=msize))
            pos += 0x40
        self.dirs = []
        for _ in range(n_dirs):
            no, nl, cs, cc = struct.unpack_from("<IIII", toc, pos)
            self.dirs.append(dict(name=name(no, nl), child_start=cs, child_count=cc))
            pos += 0x18
        self.files = []
        for _ in range(n_files):
            no, nl, size, off, stored = struct.unpack_from("<IIQQQ", toc, pos)
            sha = toc[pos + 0x20:pos + 0x40]
            comp = struct.unpack_from("<Q", toc, pos + 0x40)[0]
            self.files.append(dict(name=name(no, nl), size=size, offset=off, stored=stored,
                                   sha256=sha, compressed=bool(comp)))
            pos += 0x50
        n_children = sum(d["child_count"] for d in self.dirs)
        self.children = list(struct.unpack_from(f"<{n_children}I", toc, pos))
        self.n_dirs = n_dirs

    def walk(self, content_idx):
        """Rend (chemin relatif, fichier) pour chaque fichier d'un contenu."""
        def rec(d, prefix):
            info = self.dirs[d]
            for c in self.children[info["child_start"]:info["child_start"] + info["child_count"]]:
                if c < self.n_dirs:
                    yield from rec(c, prefix + self.dirs[c]["name"] + "/")
                else:
                    fi = self.files[c - self.n_dirs]
                    yield prefix + fi["name"], fi
        yield from rec(self.contents[content_idx]["root"], "")

    def label(self, i):
        c = self.contents[i]
        return f"{'base' if c['type'] == 0 else 'patch'}_v{c['version']}"

    def stream(self, fi):
        """Itère sur le contenu décompressé d'un fichier."""
        self.f.seek(fi["offset"])
        remaining = fi["stored"]

        def raw():
            nonlocal remaining
            while remaining:
                b = self.f.read(min(CHUNK, remaining))
                if not b:
                    raise IOError("fin de fichier inattendue")
                remaining -= len(b)
                yield b

        if not fi["compressed"]:
            yield from raw()
            return
        dobj = zstandard.ZstdDecompressor().decompressobj()
        for b in raw():
            out = dobj.decompress(b)
            if out:
                yield out

    def extract(self, fi, dest, verify=True):
        h = hashlib.sha256()
        written = 0
        os.makedirs(os.path.dirname(dest), exist_ok=True)
        with open(dest + ".part", "wb") as out:
            for b in self.stream(fi):
                out.write(b)
                written += len(b)
                if verify:
                    h.update(b)
        if written != fi["size"]:
            raise IOError(f"{dest}: taille {written} != {fi['size']}")
        ok = None
        if verify:
            ok = h.digest() == fi["sha256"]
        os.replace(dest + ".part", dest)
        return ok

    def meta(self, i):
        c = self.contents[i]
        self.f.seek(c["meta_off"])
        blob = self.f.read(c["meta_size"])
        if blob[:4] == b"\x28\xb5\x2f\xfd":
            blob = zstandard.ZstdDecompressor().decompress(blob, max_output_size=64 << 20)
        return blob


def human(n):
    for u in ("o", "Ko", "Mo", "Go"):
        if n < 1024:
            return f"{n:.1f} {u}" if u != "o" else f"{n} o"
        n /= 1024
    return f"{n:.1f} To"


def main():
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8")
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("nup")
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("list")
    ex = sub.add_parser("extract")
    ex.add_argument("-o", "--out", default="extracted")
    ex.add_argument("-c", "--content", type=int, action="append",
                    help="index du contenu (défaut : tous)")
    ex.add_argument("-i", "--include", action="append", default=[],
                    help="motif glob sur le chemin (ex. 'exefs/*')")
    ex.add_argument("--max-size", type=int, default=None, help="ignore les fichiers plus gros (octets)")
    ex.add_argument("--no-verify", action="store_true")
    ex.add_argument("--meta", action="store_true", help="extrait aussi le bloc méta de chaque contenu")
    a = ap.parse_args()

    n = Nup(a.nup)
    if a.cmd == "list":
        for i, c in enumerate(n.contents):
            print(f"[{i}] {n.label(i)}  title_id={c['title_id']:016X}  méta={human(c['meta_size'])}")
            for p, fi in n.walk(i):
                z = "zstd" if fi["compressed"] else "brut"
                print(f"     {p:<34} {human(fi['size']):>10}  ({z}, stocké {human(fi['stored'])}) @0x{fi['offset']:x}")
        return

    for i in (a.content if a.content else range(len(n.contents))):
        base = os.path.join(a.out, n.label(i))
        if a.meta:
            os.makedirs(base, exist_ok=True)
            with open(os.path.join(base, "meta.bin"), "wb") as m:
                m.write(n.meta(i))
            print(f"{base}/meta.bin")
        for p, fi in n.walk(i):
            if a.include and not any(fnmatch.fnmatch(p, pat) for pat in a.include):
                continue
            if a.max_size is not None and fi["size"] > a.max_size:
                print(f"ignoré (taille) : {p}")
                continue
            dest = os.path.join(base, p)
            ok = n.extract(fi, dest, verify=not a.no_verify)
            state = "" if ok is None else ("  sha256 OK" if ok else "  sha256 DIFFÉRENT")
            print(f"{dest}  {human(fi['size'])}{state}")


if __name__ == "__main__":
    sys.exit(main())
