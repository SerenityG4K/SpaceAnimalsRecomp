"""Shaders NVN de la Switch 2 : SASS NVIDIA SM 8.x (Ampere, encodage SM70+ : 128 bits/instruction).

Un étage = (section de contrôle, code). Section de contrôle (0xE0 octets, produite par le compilateur
hors ligne de NVN) : +0x00 magique 0x98761234, +0x38 taille du code (octets), +0x4c nombre de
registres, +0x58/+0x5c/+0x60 taille de groupe (calcul). Code des étages graphiques : en-tête SPH v4
(0x80 octets, même structure que sur Maxwell) puis les instructions.

Les fichiers analysés proviennent de RECOMP_DUMP_SHADERS (build/full/shaders/, privé).
"""
import hashlib
import os
import struct

CTL_MAGIC = 0x98761234
SPH_SIZE = 0x80

# types d'étage du SPH (bits 10..13 du premier mot)
SPH_STAGE = {1: "vert", 2: "tesc", 3: "tese", 4: "geom", 5: "frag"}


class Stage:
    def __init__(self, ctl: bytes, code: bytes, name="", cb1: bytes = b""):
        self.name = name
        self.ctl = ctl
        self.cb1 = cb1   # banque constante 1 du programme (tables de saut de BRX, constantes)
        w = struct.unpack_from("<64I", ctl.ljust(256, b"\0"))
        if w[0] != CTL_MAGIC:
            raise ValueError(f"{name}: section de contrôle inattendue 0x{w[0]:08x}")
        self.code_size = w[0x38 // 4]
        self.num_regs = w[0x4c // 4]
        self.local_size = (w[0x58 // 4], w[0x5c // 4], w[0x60 // 4])
        self.raw = code[:self.code_size]
        sph0 = struct.unpack_from("<I", self.raw)[0] if len(self.raw) >= 4 else 0
        # en-tête SPH : type 1 (VTG) ou 2 (PS), version 4
        if (sph0 & 0x1f) in (1, 2) and ((sph0 >> 5) & 0x1f) == 4:
            self.kind = SPH_STAGE.get((sph0 >> 10) & 0xf, "?")
            self.sph = self.raw[:SPH_SIZE]
            self.insns_bytes = self.raw[SPH_SIZE:]
        else:
            self.kind = "comp"
            self.sph = b""
            self.insns_bytes = self.raw
        self.hash = hashlib.sha1(self.raw).hexdigest()[:16]

    def words(self):
        """Instructions (entier 128 bits, octets petit-boutistes)."""
        b = self.insns_bytes
        for i in range(0, len(b) - 15, 16):
            lo, hi = struct.unpack_from("<QQ", b, i)
            yield i, lo | (hi << 64)


def load_dump(dirpath):
    """Tous les étages distincts d'un dossier de vidage (<n>_<i>.ctl / .bin)."""
    seen = {}
    for f in sorted(os.listdir(dirpath)):
        if not f.endswith(".ctl"):
            continue
        base = f[:-4]
        ctl = open(os.path.join(dirpath, f), "rb").read()
        binp = os.path.join(dirpath, base + ".bin")
        if not os.path.exists(binp):
            continue
        cb1p = os.path.join(dirpath, base + ".cb1")
        cb1 = open(cb1p, "rb").read() if os.path.exists(cb1p) else b""
        try:
            st = Stage(ctl, open(binp, "rb").read(), base, cb1)
        except ValueError:
            continue
        if st.hash not in seen or (cb1 and not seen[st.hash].cb1):   # préférer le vidage avec banque 1
            seen[st.hash] = st
    return list(seen.values())
