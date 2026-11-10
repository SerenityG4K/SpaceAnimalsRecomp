"""Chargement d'un module Horizon (NSO déjà mis à plat) : MOD0, .dynamic, symboles,
relocations (RELA, JMPREL, RELR) et découverte des fonctions via .eh_frame_hdr.

L'image à plat est celle produite par `tools/nso.py unpack` (text|ro|data|bss à leurs
offsets mémoire). Le module est « chargé » à une adresse de base fixe choisie par
l'appelant : le code généré contient des adresses absolues, donc la base doit être
identique à la génération et à l'exécution.
"""
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PROJECT = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, os.path.join(PROJECT, "tools"))

# Bases d'adresse par défaut (espace 39 bits façon Horizon, libre dans un processus Windows).
DEFAULT_BASES = {
    "rtld":    0x0000007100000000,
    "main":    0x0000007104000000,
    "subsdk0": 0x0000007120000000,
    "subsdk1": 0x0000007122000000,
    "subsdk2": 0x0000007126000000,
    "subsdk3": 0x0000007130000000,
    "sdk":     0x0000007140000000,
}
# Zone réservée aux « thunks » d'imports (adresses fictives, jamais exécutées comme code ARM).
IMPORT_THUNK_BASE = 0x0000007F00000000
IMPORT_THUNK_STRIDE = 0x10

DT = dict(NULL=0, NEEDED=1, PLTRELSZ=2, PLTGOT=3, HASH=4, STRTAB=5, SYMTAB=6, RELA=7, RELASZ=8,
          RELAENT=9, STRSZ=10, SYMENT=11, INIT=12, FINI=13, JMPREL=23, PLTREL=20, INIT_ARRAY=25,
          FINI_ARRAY=26, INIT_ARRAYSZ=27, FINI_ARRAYSZ=28, RELRSZ=35, RELR=36, RELRENT=37,
          RELACOUNT=0x6ffffff9, GNU_HASH=0x6ffffef5, FLAGS_1=0x6ffffffb)

R_AARCH64_ABS64 = 257
R_AARCH64_GLOB_DAT = 1025
R_AARCH64_JUMP_SLOT = 1026
R_AARCH64_RELATIVE = 1027


def _uleb(b, off):
    r = s = 0
    while True:
        x = b[off]; off += 1
        r |= (x & 0x7f) << s; s += 7
        if not x & 0x80:
            return r, off


def _sleb(b, off):
    r = s = 0
    while True:
        x = b[off]; off += 1
        r |= (x & 0x7f) << s; s += 7
        if not x & 0x80:
            if x & 0x40:
                r -= 1 << s
            return r, off


class Module:
    def __init__(self, name, image, base=None, text_size=None, ro_off=None, data_off=None):
        self.name = name
        self.img = bytearray(image)
        self.base = DEFAULT_BASES.get(name, 0x0000007150000000) if base is None else base
        self.size = len(self.img)
        mod0 = struct.unpack_from("<I", self.img, 4)[0]
        if self.img[mod0:mod0 + 4] != b"MOD0":
            raise ValueError(f"{name}: MOD0 introuvable")
        dyn, bss_s, bss_e, eh_s, eh_e, modobj = struct.unpack_from("<6i", self.img, mod0 + 4)
        self.mod0 = mod0
        self.dynamic_off = mod0 + dyn
        self.bss = (mod0 + bss_s, mod0 + bss_e)
        self.eh_frame_hdr = (mod0 + eh_s, mod0 + eh_e)
        self.text_size = text_size
        self.ro_off = ro_off
        self.data_off = data_off
        self._parse_dynamic()
        self._parse_symbols()
        self._parse_relocs()
        self.functions = self._parse_eh_frame()   # liste triée de (début, fin) en offsets

    # ------------------------------------------------------------------ chargement
    @classmethod
    def from_work(cls, name, base=None):
        """Charge work/<name>.bin et lit les tailles de segments dans le NSO d'origine."""
        import nso  # tools/nso.py
        nso_path = os.path.join(PROJECT, "extracted", "patch_v131072", "exefs", name)
        n = nso.parse(nso_path)
        segs = n["segs"]
        img_path = os.path.join(PROJECT, "work", f"{name}.bin")
        img = open(img_path, "rb").read()
        return cls(name, img, base, text_size=segs[0]["size"], ro_off=segs[1]["mem_off"],
                   data_off=segs[2]["mem_off"])

    # ------------------------------------------------------------------ .dynamic
    def _parse_dynamic(self):
        self.dyn = {}
        off = self.dynamic_off
        while True:
            tag, val = struct.unpack_from("<qQ", self.img, off)
            off += 16
            if tag == 0:
                break
            self.dyn.setdefault(tag, []).append(val)

    def _d(self, name, default=None):
        v = self.dyn.get(DT[name])
        return v[0] if v else default

    def _parse_symbols(self):
        strtab, strsz = self._d("STRTAB"), self._d("STRSZ")
        symtab, syment = self._d("SYMTAB"), self._d("SYMENT", 24)
        # nombre de symboles : la table s'arrête au début de la table de chaînes (cas usuel)
        nsym = (strtab - symtab) // syment if strtab > symtab else 0
        self.symbols = []
        for i in range(nsym):
            name_off, info, other, shndx, value, size = struct.unpack_from(
                "<IBBHQQ", self.img, symtab + i * syment)
            end = self.img.index(0, strtab + name_off)
            name = self.img[strtab + name_off:end].decode("utf-8", "replace")
            self.symbols.append(dict(name=name, info=info, shndx=shndx, value=value, size=size,
                                     type=info & 0xf, bind=info >> 4))
        self.exports = {s["name"]: s["value"] for s in self.symbols if s["shndx"] and s["name"]}
        self.imports = [s["name"] for s in self.symbols if not s["shndx"] and s["name"]]

    def _parse_relocs(self):
        self.relocs = []   # (offset, type, symidx, addend)
        for tab, sz in (("RELA", "RELASZ"), ("JMPREL", "PLTRELSZ")):
            off, size = self._d(tab), self._d(sz, 0)
            if off is None:
                continue
            for i in range(size // 24):
                r_off, r_info, r_add = struct.unpack_from("<QQq", self.img, off + i * 24)
                self.relocs.append((r_off, r_info & 0xffffffff, r_info >> 32, r_add))
        # RELR : relocations relatives compactées
        off, size = self._d("RELR"), self._d("RELRSZ", 0)
        self.relr = []
        if off is not None:
            where = 0
            for i in range(size // 8):
                e = struct.unpack_from("<Q", self.img, off + i * 8)[0]
                if not e & 1:
                    self.relr.append(e)
                    where = e + 8
                else:
                    bits = e >> 1
                    j = 0
                    while bits:
                        if bits & 1:
                            self.relr.append(where + j * 8)
                        bits >>= 1
                        j += 1
                    where += 63 * 8

    def relocated_image(self, resolve=None):
        """Image avec relocations appliquées pour la base choisie.

        `resolve(name)` renvoie l'adresse d'un symbole importé (ou None → thunk d'import).
        Renvoie (image, table des thunks {adresse: nom}).
        """
        img = bytearray(self.img)
        thunks = {}
        imp_index = {n: i for i, n in enumerate(self.imports)}

        def sym_addr(idx):
            s = self.symbols[idx]
            if s["shndx"]:
                return self.base + s["value"]
            a = resolve(s["name"]) if resolve else None
            if a is None:
                a = IMPORT_THUNK_BASE + imp_index.get(s["name"], 0) * IMPORT_THUNK_STRIDE
                thunks[a] = s["name"]
            return a

        for off in self.relr:
            v = struct.unpack_from("<Q", img, off)[0]
            struct.pack_into("<Q", img, off, (v + self.base) & (2**64 - 1))
        for off, typ, sym, add in self.relocs:
            if typ == R_AARCH64_RELATIVE:
                struct.pack_into("<Q", img, off, (self.base + add) & (2**64 - 1))
            elif typ in (R_AARCH64_GLOB_DAT, R_AARCH64_JUMP_SLOT, R_AARCH64_ABS64):
                struct.pack_into("<Q", img, off, (sym_addr(sym) + add) & (2**64 - 1))
            else:
                raise ValueError(f"{self.name}: relocation non gérée type {typ} @0x{off:x}")
        return img, thunks

    def import_thunks(self):
        return {IMPORT_THUNK_BASE + i * IMPORT_THUNK_STRIDE: n for i, n in enumerate(self.imports)}

    # ------------------------------------------------------------------ .eh_frame
    def _read_enc(self, enc, off, datarel_base=0):
        """Lit un pointeur encodé DWARF à l'offset `off`. Renvoie (valeur, nouvel offset)."""
        if enc == 0xff:
            return None, off
        fmt = enc & 0x0f
        app = enc & 0x70
        start = off
        if fmt == 0x00:
            v = struct.unpack_from("<Q", self.img, off)[0]; off += 8
        elif fmt == 0x01:
            v, off = _uleb(self.img, off)
        elif fmt == 0x02:
            v = struct.unpack_from("<H", self.img, off)[0]; off += 2
        elif fmt == 0x03:
            v = struct.unpack_from("<I", self.img, off)[0]; off += 4
        elif fmt == 0x04:
            v = struct.unpack_from("<Q", self.img, off)[0]; off += 8
        elif fmt == 0x09:
            v, off = _sleb(self.img, off)
        elif fmt == 0x0a:
            v = struct.unpack_from("<h", self.img, off)[0]; off += 2
        elif fmt == 0x0b:
            v = struct.unpack_from("<i", self.img, off)[0]; off += 4
        elif fmt == 0x0c:
            v = struct.unpack_from("<q", self.img, off)[0]; off += 8
        else:
            raise ValueError(f"encodage DWARF 0x{enc:x} non géré")
        if app == 0x10:
            v += start
        elif app == 0x30:
            v += datarel_base
        elif app not in (0x00,):
            raise ValueError(f"application DWARF 0x{app:x} non gérée")
        return v, off

    def _parse_cie(self, off):
        length = struct.unpack_from("<I", self.img, off)[0]
        p = off + 4
        if length == 0xffffffff:
            p += 8
        p += 4  # CIE id
        version = self.img[p]; p += 1
        end = self.img.index(0, p)
        aug = self.img[p:end].decode(); p = end + 1
        _, p = _uleb(self.img, p)       # code align
        _, p = _sleb(self.img, p)       # data align
        if version == 1:
            p += 1
        else:
            _, p = _uleb(self.img, p)
        fde_enc = 0x00
        if aug.startswith("z"):
            _, p = _uleb(self.img, p)
            for ch in aug[1:]:
                if ch == "R":
                    fde_enc = self.img[p]; p += 1
                elif ch == "P":
                    penc = self.img[p]; p += 1
                    _, p = self._read_enc(penc, p)
                elif ch == "L":
                    p += 1
                elif ch in "SBG":
                    pass
                else:
                    break
        return fde_enc

    def _parse_eh_frame(self):
        hdr, hdr_end = self.eh_frame_hdr
        if hdr_end <= hdr:
            return []
        version, ptr_enc, cnt_enc, tab_enc = self.img[hdr:hdr + 4]
        p = hdr + 4
        _, p = self._read_enc(ptr_enc, p, hdr)
        count, p = self._read_enc(cnt_enc, p, hdr)
        funcs = []
        cie_cache = {}
        for i in range(count):
            loc, p = self._read_enc(tab_enc, p, hdr)
            fde, p = self._read_enc(tab_enc, p, hdr)
            # FDE : longueur, pointeur CIE (relatif), pc_begin, pc_range
            q = fde + 4
            cie_ptr = struct.unpack_from("<I", self.img, q)[0]
            cie = q - cie_ptr
            if cie not in cie_cache:
                cie_cache[cie] = self._parse_cie(cie)
            enc = cie_cache[cie]
            q += 4
            pc_begin, q = self._read_enc(enc, q)
            pc_range, q = self._read_enc(enc & 0x0f, q)
            funcs.append((pc_begin, pc_begin + pc_range))
        funcs.sort()
        return funcs

    # ------------------------------------------------------------------ utilitaires
    def word(self, off):
        return struct.unpack_from("<I", self.img, off)[0]

    def in_text(self, off):
        return 0 <= off < (self.text_size or self.size)
