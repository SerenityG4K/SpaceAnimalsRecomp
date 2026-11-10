"""Récupération des tables de sauts (switch LLVM) devant un `br xN`.

Motif typique (les définitions peuvent être remontées loin avant le br) :

    cmp   x8, #N            ; borne
    b.hi  défaut
    adr   x9, ancre         ; (ou adrp+add)
    ldrb  w10, [x25, x8]    ; x25 = table (adr/adrp+add), entrées 1/2/4 octets
    add   x9, x9, x10, lsl #2
    br    x9

On évalue symboliquement le registre du br en remontant aux définitions (dernière écriture
précédente dans l'ordre des adresses). Le résultat n'a pas besoin d'être exact : le `switch`
émis ne fait que `case A: goto L_A`, ce qui est toujours correct ; une cible manquante tombe
dans `default: rt_tailcall` et provoque une erreur visible.
"""
from .common import sext

MAX_ENTRIES = 4096


def _dest(w):
    """Registre(s) écrits par l'instruction (approximation suffisante pour la recherche)."""
    op0 = (w >> 25) & 0xF
    if op0 in (10, 11):                         # branchements / système
        if (w >> 26) & 0x3F == 0b100101:        # BL
            return (30,)
        if (w >> 21) & 0x7FF == 0b11010110001:  # BLR
            return (30,)
        if (w >> 22) == 0x354 and (w >> 21) & 1:  # MRS
            return (w & 31,)
        return ()
    if op0 & 0b0101 == 0b0100:                  # chargements / rangements
        if (w >> 26) & 1:                       # SIMD
            return ()
        if (w >> 27) & 7 == 0b101:              # paires
            if (w >> 22) & 1:
                return (w & 31, (w >> 10) & 31) + (((w >> 5) & 31,) if (w >> 23) & 1 else ())
            return ()
        if (w >> 27) & 7 == 0b111 and (w >> 24) & 3 == 0 and ((w >> 22) & 3) == 0:
            return ()                           # STR*
        if (w >> 27) & 7 == 0b111 and (w >> 24) & 3 == 1 and ((w >> 22) & 3) == 0:
            return ()
        if (w >> 24) & 0x3F == 0b001000 and not (w >> 22) & 1 and not ((w >> 21) & 1 and (w >> 23) & 1):
            return ((w >> 16) & 31,)            # STXR/STLR : statut
        return (w & 31,)
    if op0 & 0b0111 == 0b0111:                  # FP/SIMD : seuls FMOV/UMOV vers GPR comptent
        return (w & 31,)
    return (w & 31,)


class _Ctx:
    def __init__(self, rc, f, idx):
        self.rc = rc
        self.f = f
        self.insns = f.insns
        self.idx = idx

    def find_def(self, reg, before):
        for j in range(before - 1, -1, -1):
            w = self.insns[j].word
            if reg in _dest(w):
                return j
        return None

    def value(self, reg, before, depth=0):
        """Valeur constante du registre (ou None) ; peut renvoyer ('load', ...) pour une
        entrée de table."""
        if depth > 12 or reg == 31:
            return None
        j = self.find_def(reg, before)
        if j is None:
            return None
        i = self.insns[j]
        w, pc = i.word, i.pc
        if (w & 0x1F000000) == 0x10000000:      # ADR / ADRP
            imm = sext((((w >> 5) & 0x7FFFF) << 2) | ((w >> 29) & 3), 21)
            if w >> 31:
                return ((pc & ~0xFFF) + (imm << 12)) & (2**64 - 1)
            return (pc + imm) & (2**64 - 1)
        if (w & 0x7F800000) == 0x11000000:      # ADD imm (32/64)
            v = self.value((w >> 5) & 31, j, depth + 1)
            if isinstance(v, int):
                return v + (((w >> 10) & 0xFFF) << (12 if (w >> 22) & 1 else 0))
            return None
        if (w & 0x7FE0FFE0) == 0x2A0003E0:      # MOV (ORR reg)
            return self.value((w >> 16) & 31, j, depth + 1)
        if (w & 0x7F200000) == 0x0B000000:      # ADD registre décalé
            if (w >> 22) & 3:
                return None
            sh = (w >> 10) & 63
            a = self.value((w >> 5) & 31, j, depth + 1)
            b = self.value((w >> 16) & 31, j, depth + 1)
            return self._combine(a, b, sh, False)
        if (w & 0x7FE00000) == 0x0B200000:      # ADD registre étendu
            opt = (w >> 13) & 7
            sh = (w >> 10) & 7
            a = self.value((w >> 5) & 31, j, depth + 1)
            b = self.value((w >> 16) & 31, j, depth + 1)
            return self._combine(a, b, sh, opt == 6)
        # chargement d'entrée : LDRB/LDRH/LDR(W)/LDRSW/LDRSB/LDRSH [Xn, Xm{, lsl}]
        if (w & 0x3B200C00) == 0x38200800 and not (w >> 26) & 1:
            size = w >> 30
            opc = (w >> 22) & 3
            if opc == 0:
                return None
            signed = opc >= 2
            base = self.value((w >> 5) & 31, j, depth + 1)
            if not isinstance(base, int):
                return None
            return ("load", base, size, signed, (w >> 16) & 31, j)
        if (w & 0x3B000000) == 0x39000000 and not (w >> 26) & 1:  # LDR imm non signé
            return None
        return None

    @staticmethod
    def _combine(a, b, sh, sxtw):
        if isinstance(a, int) and isinstance(b, tuple):
            return ("entry", a, b, sh)
        if isinstance(b, int) and isinstance(a, tuple) and sh == 0:
            return ("entry", b, a, 0)
        if isinstance(a, int) and isinstance(b, int):
            return a + (b << sh)
        return None

    def bound(self, reg, before):
        """Cherche `cmp reg, #imm` suivi d'un b.hi / b.hs avant `before`."""
        for j in range(before - 1, max(-1, before - 64), -1):
            w = self.insns[j].word
            if (w & 0x7F80001F) == 0x7100001F and ((w >> 5) & 31) == reg:   # SUBS xzr, reg, #imm
                imm = ((w >> 10) & 0xFFF) << (12 if (w >> 22) & 1 else 0)
                for k in range(j + 1, min(len(self.insns), j + 6)):
                    w2 = self.insns[k].word
                    if (w2 & 0xFF000010) == 0x54000000:
                        cond = w2 & 15
                        if cond == 8:      # hi : idx <= imm
                            return imm + 1
                        if cond == 2:      # hs : idx < imm
                            return imm
                return None
            if reg in _dest(w):
                # l'index a été recalculé : la borne doit précéder (on continue sur la source)
                if (w & 0x7FE0FFE0) == 0x2A0003E0:
                    reg = (w >> 16) & 31
                    continue
                return None
        return None


def recover(rc, f, idx):
    i = f.insns[idx]
    w = i.word
    reg = (w >> 5) & 31
    c = _Ctx(rc, f, idx)
    v = c.value(reg, idx)
    if not (isinstance(v, tuple) and v[0] == "entry"):
        return None
    _, anchor, load, sh = v
    if not (isinstance(load, tuple) and load[0] == "load"):
        return None
    _, table, size, signed, idx_reg, load_j = load
    n = c.bound(idx_reg, load_j + 1) or c.bound(idx_reg, idx)
    m = rc.m
    off = table - m.base
    if not (0 <= off < m.size):
        return None
    esz = 1 << size
    limit = n if n else MAX_ENTRIES
    targets = []
    for k in range(limit):
        p = off + k * esz
        if p + esz > m.size:
            break
        e = int.from_bytes(m.img[p:p + esz], "little", signed=signed)
        t = anchor + (e << sh)
        if not (f.start <= t < f.end) or t % 4:
            if n:
                return None   # table bornée incohérente : on renonce
            break
        targets.append(t)
    return targets or None
