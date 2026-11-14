"""Décodeur SASS SM70+ (Volta → Ampere ; la Switch 2 est en SM 8.x).

Instruction de 128 bits (entier petit-boutiste). Champs communs :
  [0:9) opcode de base, [9:12) forme des opérandes, [12:15) prédicat de garde (7 = PT),
  15 négation du prédicat, [16:24) destination, [24:32) source A, [32:64) source B
  (registre [32:40), immédiat 32 bits ou constante : octet [38:54), banque [54:59)),
  [64:72) source C, [105:128) bits de contrôle (ordonnancement : ignorés ici).
Formes : 1 = B reg / C reg, 2 = B reg(64) / C imm, 3 = B reg(64) / C cbuf,
         4 = B imm / C reg, 5 = B cbuf / C reg, 6 = B ureg / C reg, 7 = B reg(64) / C ureg.
Modificateurs : A neg 72 / abs 73 ; B (emplacement 32) neg 63 / abs 62 ; emplacement 64 neg 75 /
abs 74.

Positions établies d'après la grammaire de turingas et l'encodeur SM70 de NAK (Mesa, licence MIT),
vérifiées sur les shaders du jeu. Ce fichier n'en reprend que des faits d'encodage.
"""
import struct

RZ = 255
URZ = 63
PT = 7


def bits(w, lo, hi):
    return (w >> lo) & ((1 << (hi - lo)) - 1)


def bit(w, b):
    return (w >> b) & 1


def sext(v, n):
    return v - (1 << n) if v & (1 << (n - 1)) else v


# ------------------------------------------------------------------ opérandes
class Reg:
    __slots__ = ("n", "neg", "abs", "uni")

    def __init__(self, n, neg=False, abs_=False, uni=False):
        self.n, self.neg, self.abs, self.uni = n, neg, abs_, uni

    @property
    def zero(self):
        return self.n == (URZ if self.uni else RZ)

    def __repr__(self):
        s = ("URZ" if self.n == URZ else f"UR{self.n}") if self.uni else ("RZ" if self.n == RZ else f"R{self.n}")
        if self.abs:
            s = f"|{s}|"
        return ("-" if self.neg else "") + s


class Imm:
    __slots__ = ("v",)

    def __init__(self, v):
        self.v = v & 0xffffffff

    neg = False
    abs = False

    def __repr__(self):
        return f"0x{self.v:x}"


class CBuf:
    __slots__ = ("bank", "off", "neg", "abs", "ubank")

    def __init__(self, bank, off, neg=False, abs_=False, ubank=None):
        self.bank, self.off, self.neg, self.abs, self.ubank = bank, off, neg, abs_, ubank

    def __repr__(self):
        b = f"UR{self.ubank}" if self.ubank is not None else f"0x{self.bank:x}"
        s = f"c[{b}][0x{self.off:x}]"
        if self.abs:
            s = f"|{s}|"
        return ("-" if self.neg else "") + s


class Pred:
    __slots__ = ("n", "neg", "uni")

    def __init__(self, n, neg=False, uni=False):
        self.n, self.neg, self.uni = n, neg, uni

    @property
    def true(self):
        return self.n == PT and not self.neg

    def __repr__(self):
        p = ("UPT" if self.n == PT else f"UP{self.n}") if self.uni else ("PT" if self.n == PT else f"P{self.n}")
        return ("!" if self.neg else "") + p


def pred_at(w, lo, notbit, uni=False):
    return Pred(bits(w, lo, lo + 3), bool(bit(w, notbit)), uni)


def cbuf_at(w):
    if bit(w, 91) and False:
        pass
    return CBuf(bits(w, 54, 59), bits(w, 38, 54), bool(bit(w, 63)), bool(bit(w, 62)))


def alu_operands(w, uni=False):
    """(A, B, C) selon la forme. Registres uniformes si `uni`."""
    form = bits(w, 9, 12)
    R = (lambda lo, neg=None, ab=None: Reg(bits(w, lo, lo + (6 if uni else 8)),
                                            bool(bit(w, neg)) if neg is not None else False,
                                            bool(bit(w, ab)) if ab is not None else False, uni))
    a = R(24, 72, 73)
    if uni:
        a.n = bits(w, 24, 30)
    if form == 1:
        b, c = R(32, 63, 62), R(64, 75, 74)
    elif form == 2:
        b, c = R(64, 75, 74), Imm(bits(w, 32, 64))
    elif form == 3:
        b, c = R(64, 75, 74), cbuf_at(w)
    elif form == 4:
        b, c = Imm(bits(w, 32, 64)), R(64, 75, 74)
    elif form == 5:
        b, c = cbuf_at(w), R(64, 75, 74)
    elif form == 6:
        b, c = Reg(bits(w, 32, 38), bool(bit(w, 63)), bool(bit(w, 62)), True), R(64, 75, 74)
    elif form == 7:
        b, c = R(64, 75, 74), Reg(bits(w, 32, 38), False, False, True)
    else:
        b, c = None, None
    if uni:
        for x in (b, c):
            if isinstance(x, Reg) and not x.uni:
                x.uni = True
                x.n &= 0x3f
    return a, b, c


def src_b2(w, uni=False):
    """Deuxième source des opérations à 2 entrées (FADD, FMUL…) : registre [32:40) en forme 1,
    sinon le champ 32 bits (immédiat / constante / registre uniforme)."""
    form = bits(w, 9, 12)
    if form == 1:
        return Reg(bits(w, 32, 40), bool(bit(w, 63)), bool(bit(w, 62)))
    if form in (2, 4):
        return Imm(bits(w, 32, 64))
    if form in (3, 5):
        return cbuf_at(w)
    if form in (6, 7):
        return Reg(bits(w, 32, 38), bool(bit(w, 63)), bool(bit(w, 62)), True)
    return None


# ------------------------------------------------------------------ instruction
class Insn:
    def __init__(self, pc, w):
        self.pc = pc
        self.w = w
        self.opc = bits(w, 0, 12)
        self.base = bits(w, 0, 9)
        self.form = bits(w, 9, 12)
        self.guard = Pred(bits(w, 12, 15), bool(bit(w, 15)))
        self.name = "?"
        self.dst = None            # Reg ou liste
        self.srcs = []
        self.pdst = []             # prédicats écrits
        self.psrc = []             # prédicats lus (opérandes)
        self.mods = {}
        self.target = None         # branchements (adresse absolue)

    def f(self, lo, hi):
        return bits(self.w, lo, hi)

    def b(self, n):
        return bit(self.w, n)

    def __repr__(self):
        g = "" if self.guard.true else f"@{self.guard} "
        ops = []
        if self.dst is not None:
            ops.append(repr(self.dst))
        ops += [repr(p) for p in self.pdst]
        ops += [repr(s) for s in self.srcs]
        ops += [repr(p) for p in self.psrc]
        m = "".join(f".{k}" if v is True else f".{k}={v}" for k, v in self.mods.items() if v not in (False, None, 0))
        t = f" → 0x{self.target:x}" if self.target is not None else ""
        return f"{self.pc:05x}: {g}{self.name}{m} {', '.join(ops)}{t}"


FCMP = {1: "lt", 2: "eq", 3: "le", 4: "gt", 5: "ne", 6: "ge", 7: "num", 8: "nan", 9: "ltu", 10: "equ",
        11: "leu", 12: "gtu", 13: "neu", 14: "geu", 0: "f", 15: "t"}
ICMP = {0: "f", 1: "lt", 2: "eq", 3: "le", 4: "gt", 5: "ne", 6: "ge", 7: "t"}
SETOP = {0: "and", 1: "or", 2: "xor", 3: "?"}
MUFU = {0: "cos", 1: "sin", 2: "ex2", 3: "lg2", 4: "rcp", 5: "rsq", 6: "rcp64h", 7: "rsq64h", 8: "sqrt", 9: "tanh"}
TEXDIM = {0: "1d", 1: "2d", 2: "3d", 3: "cube", 4: "1darray", 5: "2darray", 7: "cubearray", 6: "?"}
LODMODE = {0: "auto", 1: "lz", 2: "lb", 3: "ll", 4: "lc", 5: "lb.lc"}


def _dec_alu(i, name, nsrc, uni=False, dst=True):
    i.name = name
    a, b, c = alu_operands(i.w, uni)
    if dst:
        i.dst = Reg(bits(i.w, 16, 22) if uni else bits(i.w, 16, 24), uni=uni)
    if nsrc == 1:
        i.srcs = [src_b2(i.w, uni)]
    elif nsrc == 2:
        i.srcs = [a, src_b2(i.w, uni)]
    else:
        i.srcs = [a, b, c]


def decode(pc, w):
    i = Insn(pc, w)
    op, form = i.base, i.form
    uni = bool(bit(w, 91)) and op in (0x090, 0x092, 0x099, 0x08c, 0x087, 0x091, 0x0a4, 0x0a5, 0x096, 0x09a, 0x0bd, 0x0bf, 0x0be)
    W = w
    if op == 0x021:                                   # FADD
        _dec_alu(i, "fadd", 2)
        i.mods = {"sat": i.b(77), "rnd": i.f(78, 80), "ftz": i.b(80)}
    elif op == 0x020:                                 # FMUL
        _dec_alu(i, "fmul", 2)
        i.mods = {"sat": i.b(77), "rnd": i.f(78, 80), "ftz": i.b(80), "pdiv": i.f(84, 87)}
    elif op == 0x023:                                 # FFMA
        _dec_alu(i, "ffma", 3)
        i.mods = {"sat": i.b(77), "rnd": i.f(78, 80), "ftz": i.b(80)}
    elif op == 0x009:                                 # FMNMX (pred : vrai = min)
        _dec_alu(i, "fmnmx", 2)
        i.psrc = [pred_at(W, 87, 90)]
    elif op == 0x00b:                                 # FSETP
        _dec_alu(i, "fsetp", 2, dst=False)
        i.pdst = [pred_at(W, 81, 99), pred_at(W, 84, 99)]
        i.psrc = [pred_at(W, 87, 90)]
        i.mods = {"cmp": FCMP[i.f(76, 80)], "bop": SETOP[i.f(74, 76)], "ftz": i.b(80)}
    elif op == 0x00a:                                 # FSET
        _dec_alu(i, "fset", 2)
        i.psrc = [pred_at(W, 87, 90)]
        # .BF (résultat 1.0 / 0.0) : les 40 FSET vidés ont tous le bit 72 à 0 et le bit 80 à 1, et leur
        # résultat sert d'opérande flottant (FFMA, FMUL…) ; lu comme masque 0xffffffff (NaN), il
        # rendait noirs la fourrure et tout ce qui en dépend
        i.mods = {"cmp": FCMP[i.f(76, 80)], "bop": SETOP[i.f(74, 76)], "bf": 1}
    elif op == 0x008:                                 # FSEL
        _dec_alu(i, "fsel", 2)
        i.psrc = [pred_at(W, 87, 90)]
    elif op == 0x108:                                 # MUFU
        _dec_alu(i, "mufu", 1)
        # f16 : opération en demi-précision ; sub choisit la moitié du mot source, comme pour f2f (2026-11-13)
        i.mods = {"op": MUFU.get(i.f(74, 80), i.f(74, 80)), "f16": i.b(73), "sub": i.f(60, 62)}
    elif op == 0x022 and i.opc == 0x822:              # FSWZADD
        i.name = "fswzadd"
        i.dst = Reg(i.f(16, 24))
        i.srcs = [Reg(i.f(24, 32)), Reg(i.f(64, 72))]
        i.mods = {"sub": i.f(32, 40), "ndv": i.b(77)}
    elif op == 0x107:                                 # FRND
        _dec_alu(i, "frnd", 1)
        i.mods = {"rnd": i.f(78, 80), "ftz": i.b(80), "st": i.f(84, 86), "dt": i.f(75, 77)}
    elif op in (0x105, 0x111):                        # F2I
        _dec_alu(i, "f2i", 1)
        i.mods = {"signed": i.b(72), "dt": i.f(75, 77), "rnd": i.f(78, 80), "st": i.f(84, 86), "sub": i.f(60, 62)}
    elif op in (0x106, 0x112):                        # I2F
        _dec_alu(i, "i2f", 1)
        i.mods = {"signed": i.b(74), "dt": i.f(75, 77), "rnd": i.f(78, 80), "st": i.f(84, 86), "sub": i.f(60, 62)}
    elif op == 0x045:                                 # I2FP.F32.{S,U}32 (Ampere)
        _dec_alu(i, "i2fp", 1)
        i.mods = {"signed": i.b(74), "rnd": i.f(78, 80)}
    elif op in (0x104, 0x110):                        # F2F
        _dec_alu(i, "f2f", 1)
        i.mods = {"dt": i.f(75, 77), "rnd": i.f(78, 80), "ftz": i.b(80), "st": i.f(84, 86), "sub": i.f(60, 62)}
    elif op == 0x03e:                                 # F2FP.PACK_AB (2 × f32 → f16x2)
        _dec_alu(i, "f2fp", 3)
        i.mods = {"bf16": i.b(85) if False else 0, "rnd": i.f(79, 81)}
    elif op == 0x030:                                 # HADD2
        _dec_alu(i, "hadd2", 2)
        i.mods = {"sat": i.b(77), "f32": i.b(78), "sa": i.f(74, 76), "sb": i.f(60, 62)}
    elif op == 0x032:                                 # HMUL2
        _dec_alu(i, "hmul2", 2)
        i.mods = {"sat": i.b(77), "f32": i.b(78), "sa": i.f(74, 76), "sb": i.f(60, 62)}
    elif op == 0x031:                                 # HFMA2
        _dec_alu(i, "hfma2", 3)
        i.mods = {"sat": i.b(77), "f32": i.b(78), "sa": i.f(74, 76), "sb": i.f(60, 62), "sc": i.f(81, 83)}
    elif op == 0x034:                                 # HSETP2
        _dec_alu(i, "hsetp2", 2, dst=False)
        i.pdst = [pred_at(W, 81, 99), pred_at(W, 84, 99)]
        i.psrc = [pred_at(W, 87, 90)]
        i.mods = {"cmp": FCMP[i.f(76, 80)], "bop": SETOP[i.f(69, 71)], "h_and": i.b(71), "sa": i.f(74, 76),
                  "sb": i.f(60, 62)}
    elif op == 0x010 or (op == 0x090):                # IADD3 / UIADD3
        _dec_alu(i, "iadd3", 3, uni=uni)
        i.pdst = [pred_at(W, 81, 99, uni), pred_at(W, 84, 99, uni)]
        i.psrc = [pred_at(W, 87, 90, uni), pred_at(W, 77, 80, uni)]
        i.mods = {"x": i.b(74)}
    elif op == 0x024 or op == 0x0a4:                  # IMAD (+ .X : 74)
        _dec_alu(i, "imad", 3, uni=uni)
        i.mods = {"signed": i.b(73), "x": i.b(74)}
        i.psrc = [pred_at(W, 87, 90)]
    elif op == 0x025 or op == 0x0a5:                  # IMAD.WIDE / UIMAD.WIDE (0x2a5, 0x8a5 : division par 3…)
        _dec_alu(i, "imad.wide", 3, uni=uni)
        i.mods = {"signed": i.b(73)}
    elif op == 0x027:                                 # IMAD.HI
        _dec_alu(i, "imad.hi", 3)
        i.mods = {"signed": i.b(73), "x": i.b(74)}
    elif op == 0x012 or op == 0x092:                  # LOP3
        _dec_alu(i, "lop3", 3, uni=uni)
        i.mods = {"lut": i.f(72, 80)}
        i.pdst = [pred_at(W, 81, 99, uni)]
        i.psrc = [pred_at(W, 87, 90, uni)]
    elif op == 0x019 or op == 0x099:                  # SHF
        _dec_alu(i, "shf", 3, uni=uni)
        i.mods = {"type": {0: "s64", 1: "u64", 2: "s32", 3: "u32"}[i.f(73, 75)], "wrap": i.b(75),
                  "right": i.b(76), "hi": i.b(80)}
    elif op == 0x011 or op == 0x091:                  # LEA (dst = (A << s) + B ; .HI)
        _dec_alu(i, "lea", 3, uni=uni)
        i.mods = {"neg_a": i.b(72), "shift": i.f(75, 80), "hi": i.b(80), "x": i.b(74), "sx32": i.b(73)}
        i.pdst = [pred_at(W, 81, 99, uni)]
        i.psrc = [pred_at(W, 87, 90, uni)]
    elif op == 0x016 or op == 0x096:                  # PRMT
        _dec_alu(i, "prmt", 3, uni=uni)
        i.mods = {"mode": i.f(72, 75)}
    elif op == 0x007 or op == 0x087:                  # SEL
        _dec_alu(i, "sel", 2, uni=uni)
        i.psrc = [pred_at(W, 87, 90, uni)]
    elif op == 0x017:                                 # IMNMX
        _dec_alu(i, "imnmx", 2)
        i.psrc = [pred_at(W, 87, 90)]
        i.mods = {"signed": i.b(73)}
    elif op == 0x00c or op == 0x08c:                  # ISETP
        _dec_alu(i, "isetp", 2, uni=uni, dst=False)
        i.pdst = [pred_at(W, 81, 99, uni), pred_at(W, 84, 99, uni)]
        i.psrc = [pred_at(W, 87, 90, uni), pred_at(W, 68, 71, uni)]
        i.mods = {"cmp": ICMP[i.f(76, 79)], "signed": i.b(73), "bop": SETOP[i.f(74, 76)], "ex": i.b(72)}
    elif op == 0x013:                                 # IABS
        _dec_alu(i, "iabs", 1)
    elif op == 0x01a or op == 0x09a:                  # SGXT
        _dec_alu(i, "sgxt", 2, uni=uni)
        i.mods = {"signed": i.b(73)}
    elif op == 0x01b or op == 0x09b:                  # BMSK
        _dec_alu(i, "bmsk", 2, uni=uni)
        i.mods = {"wrap": i.b(75)}
    elif op == 0x100 or op == 0x0bd:                  # FLO / UFLO (0x2bd : élection d'un fil meneur)
        _dec_alu(i, "flo", 1, uni=uni)
        i.mods = {"signed": i.b(73), "sh": i.b(74), "not": i.b(63)}
    elif op == 0x109 or op == 0x0bf:                  # POPC / UPOPC
        _dec_alu(i, "popc", 1, uni=uni)
        i.mods = {"not": i.b(63)}
    elif op == 0x101 or op == 0x0be:                  # BREV / UBREV
        _dec_alu(i, "brev", 1, uni=uni)
    elif i.opc == 0x3c4:                              # REDUX : réduction sur le warp vers un registre uniforme
        # opération aux bits 78-80 : 0 AND, 1 OR, 2 XOR, 3 SUM, 4 MIN, 5 MAX (établi sur 0102 : la valeur 4
        # est suivie d'un ATOMS.MIN du fil meneur) ; bit 73 : signé
        i.name = "redux"
        i.dst = Reg(i.f(16, 22), uni=True)
        i.srcs = [Reg(i.f(24, 32))]
        i.mods = {"op": i.f(78, 81), "signed": i.b(73)}
    elif i.opc == 0x3a1:                              # ATOMS.POPC.INC (hypothèse) : + nombre de fils actifs
        i.name = "atoms.popcinc"
        i.dst = Reg(i.f(16, 24))
        i.srcs = [Reg(i.f(24, 32)), Imm(sext(i.f(40, 64), 24))]
    elif op == 0x002:                                 # MOV
        _dec_alu(i, "mov", 1)
        i.mods = {"lanes": i.f(72, 76)}
    elif op == 0x082 and i.opc in (0xc82, 0x882):     # UMOV (forme 6 : UR, 4 : imm)
        i.name = "umov"
        i.dst = Reg(i.f(16, 22), uni=True)
        i.srcs = [Imm(i.f(32, 64)) if form == 4 else Reg(i.f(32, 38), uni=True)]
    elif op == 0x01c:                                 # PLOP3 (P0 = lut0(a,b,c), P1 = lut1)
        i.name = "plop3"
        i.pdst = [pred_at(W, 81, 99), pred_at(W, 84, 99)]
        i.psrc = [pred_at(W, 87, 90), pred_at(W, 77, 80), pred_at(W, 68, 71, uni=bool(i.b(67)))]
        i.mods = {"lut0": i.f(64, 67) | (i.f(72, 77) << 3), "lut1": i.f(16, 24)}
    elif op == 0x003 and i.opc == 0x803:              # P2R (approx.)
        _dec_alu(i, "p2r", 2)
        i.mods = {"byte": i.f(74, 76)}
    elif op == 0x004 and i.opc == 0x804:              # R2P
        i.name = "r2p"
        i.srcs = [Reg(i.f(24, 32)), src_b2(W)]
        i.mods = {"byte": i.f(74, 76)}
    elif op == 0x119:                                 # S2R
        i.name = "s2r"
        i.dst = Reg(i.f(16, 24))
        i.mods = {"sr": i.f(72, 80)}
    elif op == 0x1c3:                                 # S2UR
        i.name = "s2ur"
        i.dst = Reg(i.f(16, 22), uni=True)
        i.mods = {"sr": i.f(72, 80)}
    elif op == 0x005 and i.opc == 0x805:              # CS2R
        i.name = "cs2r"
        i.dst = Reg(i.f(16, 24))
        i.mods = {"sr": i.f(72, 80), "w64": i.b(80)}
    elif op == 0x1c2 and i.opc == 0x3c2:              # R2UR
        i.name = "r2ur"
        i.dst = Reg(i.f(16, 22), uni=True)
        i.srcs = [Reg(i.f(24, 32))]
    elif i.opc == 0xab9:                              # ULDC
        i.name = "uldc"
        i.dst = Reg(i.f(16, 22), uni=True)
        i.srcs = [CBuf(i.f(54, 59), i.f(38, 54))]
        i.mods = {"type": i.f(73, 76)}
    elif i.opc == 0xabb:                              # ULDC c[banque][URb + imm] : indice dans un registre uniforme
        # (ex. « ULDC.64 UR4, c[0x6][UR4+0x10] » : table de décalages d'un flou parcourue par une boucle)
        i.name = "ldc"
        i.dst = Reg(i.f(16, 22), uni=True)
        i.srcs = [CBuf(i.f(54, 59), i.f(38, 54)), Reg(i.f(24, 30), uni=True)]
        i.mods = {"type": i.f(73, 76), "mode": 0}
    elif i.opc == 0xb82 or i.opc == 0x582:            # LDC
        i.name = "ldc"
        i.dst = Reg(i.f(16, 24))
        if i.opc == 0xb82:
            i.srcs = [CBuf(i.f(54, 59), i.f(38, 54)), Reg(i.f(24, 32))]
        else:
            i.srcs = [CBuf(0, i.f(38, 54), ubank=i.f(24, 30)), Reg(i.f(64, 72))]
        i.mods = {"type": i.f(73, 76), "mode": i.f(78, 80)}
    elif i.opc in (0x381, 0x981, 0x983, 0x984):       # LDG / LDL / LDS
        i.name = {0x381: "ldg", 0x981: "ldg", 0x983: "ldl", 0x984: "lds"}[i.opc]
        i.dst = Reg(i.f(16, 24))
        i.srcs = [Reg(i.f(24, 32)), Imm(sext(i.f(40, 64), 24))]
        if i.opc == 0x981:
            i.srcs.append(Reg(i.f(32, 38), uni=True))
        i.mods = {"type": i.f(73, 76), "e": i.b(90 if i.opc == 0x981 else 72)}
        if i.opc == 0x984:   # LDS [Ra.Xn + imm] : registre multiplié par 1, 4, 8 ou 16 (bits 78-79)
            i.mods["scale"] = (1, 4, 8, 16)[i.f(78, 80)]
    elif i.opc in (0x386, 0x986, 0x387, 0x987, 0x388, 0x988):   # STG / STL / STS
        i.name = {0x386: "stg", 0x986: "stg", 0x387: "stl", 0x987: "stl", 0x388: "sts", 0x988: "sts"}[i.opc]
        i.srcs = [Reg(i.f(24, 32)), Imm(sext(i.f(40, 64), 24)), Reg(i.f(32, 40))]
        if i.opc in (0x986, 0x987, 0x988):
            i.srcs.append(Reg(i.f(64, 70), uni=True))
        i.mods = {"type": i.f(73, 76), "e": i.b(90 if i.opc == 0x986 else 72)}
        if i.opc in (0x388, 0x988):   # STS [Ra.Xn + URb + imm] : même échelle que LDS (bits 78-79)
            i.mods["scale"] = (1, 4, 8, 16)[i.f(78, 80)]
    elif i.opc in (0x38c, 0x98c, 0x38d):              # ATOMS
        i.name = "atoms"
        i.dst = Reg(i.f(16, 24))
        i.srcs = [Reg(i.f(24, 32)), Imm(sext(i.f(40, 64), 24)), Reg(i.f(32, 40))]
        if i.opc == 0x38d:
            i.srcs.append(Reg(i.f(64, 72)))
            i.mods = {"op": "cas"}
        else:
            i.mods = {"op": i.f(87, 91), "type": i.f(73, 76)}
    elif i.opc in (0x3a8, 0x9a8, 0x3a9, 0x98e):       # ATOMG / RED
        i.name = "red" if i.opc == 0x98e else "atomg"
        i.dst = None if i.opc == 0x98e else Reg(i.f(16, 24))
        i.srcs = [Reg(i.f(24, 32)), Imm(sext(i.f(40, 64), 24)), Reg(i.f(32, 40))]
        if i.opc == 0x3a9:
            i.srcs.append(Reg(i.f(64, 72)))
            i.mods = {"op": "cas"}
        else:
            i.mods = {"op": i.f(87, 90 if i.opc == 0x98e else 91), "type": i.f(73, 76)}
    elif i.opc == 0x321:                              # ALD
        i.name = "ald"
        i.dst = Reg(i.f(16, 24))
        i.srcs = [Reg(i.f(24, 32)), Reg(i.f(32, 40))]   # décalage, sommet
        i.mods = {"addr": i.f(40, 50), "n": i.f(74, 76) + 1, "patch": i.b(76), "phys": i.b(77), "out": i.b(79)}
    elif i.opc == 0x322:                              # AST
        i.name = "ast"
        i.srcs = [Reg(i.f(32, 40)), Reg(i.f(24, 32)), Reg(i.f(64, 72))]   # donnée, décalage, sommet
        i.mods = {"addr": i.f(40, 50), "n": i.f(74, 76) + 1, "patch": i.b(76), "phys": i.b(77)}
    elif i.opc == 0x326:                              # IPA
        i.name = "ipa"
        i.dst = Reg(i.f(16, 24))
        i.srcs = [Reg(i.f(32, 40))]                   # décalage
        i.mods = {"addr": i.f(64, 72) << 2, "loc": i.f(76, 78), "freq": i.f(78, 80)}
    elif i.opc == 0x923:                              # ISBERD
        i.name = "isberd"
        i.dst = Reg(i.f(16, 24))
        i.srcs = [Reg(i.f(24, 32))]
    elif i.opc == 0x925:                              # PIXLD
        i.name = "pixld"
        i.dst = Reg(i.f(16, 24))
        i.mods = {"val": i.f(78, 81)}
    elif i.opc in (0xb60, 0xf60, 0x361, 0xb66, 0x367, 0xb63, 0xf63, 0x364, 0xb6c, 0xf6c, 0x36d, 0xb69, 0x36a):   # TEX & co
        # 0xf60 / 0xf63 / 0xf6c : formes de TEX / TLD4 / TXD à poignée en registres uniformes, voir plus bas
        i.name = {0xb60: "tex", 0xf60: "tex", 0x361: "tex", 0xb66: "tld", 0x367: "tld", 0xb63: "tld4", 0xf63: "tld4",
                  0x364: "tld4", 0xb6c: "txd", 0xf6c: "txd", 0x36d: "txd", 0xb69: "tmml", 0x36a: "tmml"}[i.opc]
        d1 = i.f(64, 72)
        i.dst = [Reg(i.f(16, 24)), Reg(d1)]
        i.srcs = [Reg(i.f(24, 32)), Reg(i.f(32, 40))]
        i.mods = {"dim": TEXDIM[i.f(61, 64)], "mask": i.f(72, 76), "scalar": i.b(60), "aoffi": i.b(76),
                  "ndv": i.b(77), "dc": i.b(78), "lod": LODMODE.get(i.f(87, 90), i.f(87, 90)) if i.name != "tld4" else 0,
                  "comp": i.f(87, 89) if i.name == "tld4" else 0, "ms": i.b(78) if i.name == "tld" else 0,
                  "toff": i.f(76, 78) if i.name == "tld4" else 0, "bindless": i.opc & 0x800 == 0}
        if i.opc in (0xf60, 0xf63, 0xf6c):
            # forme à poignée dans une paire de registres uniformes (bit 91) : URn = texture | échant. << 20,
            # URn+1 = indice d'échantillonneur, n = bits 40-45 (toujours pair). Établi sur le matériau
            # principal : le shader calcule UR4 = c[2][texture séparée] | c[2][échantillonneur séparé],
            # UR5 = UR4 >> 20 juste avant ; une lecture d'ombre, un tableau 2D et une texture 3D
            # utilisent ainsi « 4 » (l'ancienne lecture « emplacement 4 de BindTextures » les confondait)
            i.mods["ur"] = i.f(40, 46)
        elif i.opc & 0x800:
            i.mods["tbank"] = i.f(54, 59)
            i.mods["toff_cb"] = i.f(40, 54) * 4
        i.pdst = [pred_at(W, 81, 99)]
    elif i.opc in (0xb6f, 0x370):                     # TXQ
        i.name = "txq"
        i.dst = [Reg(i.f(16, 24)), Reg(i.f(64, 72))]
        i.srcs = [Reg(i.f(24, 32))]
        i.mods = {"query": i.f(62, 64), "mask": i.f(72, 76), "bindless": i.opc == 0x370}
        if i.opc == 0xb6f:
            i.mods["tbank"] = i.f(54, 59)
            i.mods["toff_cb"] = i.f(40, 54) * 4
    elif i.opc in (0x998, 0x99a, 0x99c, 0x99e, 0xb97, 0xb99, 0xb9b, 0xb9d):   # SULD / SUST
        # formes 0xb9x : poignée lue dans une banque constante (comme TEX 0xb60 / 0x361)
        bindless = i.opc < 0xb00
        b = i.opc if bindless else i.opc + 1 - 0x200
        i.name = "suld" if b in (0x998, 0x99a) else "sust"
        hreg = [Reg(i.f(64, 72))] if bindless else []
        if i.name == "suld":
            i.dst = Reg(i.f(16, 24))
            i.srcs = [Reg(i.f(24, 32))] + hreg
        else:
            i.srcs = [Reg(i.f(24, 32)), Reg(i.f(32, 40))] + hreg
        # dim (NAK) : 0 1D, 1 1D tampon, 2 1D tableau, 3 2D, 4 2D tableau, 5 3D
        i.mods = {"dim": i.f(61, 64), "mask": i.f(72, 76), "fmt": b in (0x998, 0x99c), "type": i.f(73, 76),
                  "bindless": bindless}
        if not bindless:
            i.mods["tbank"] = i.f(54, 59)
            i.mods["toff_cb"] = i.f(40, 54) * 4
    elif i.opc in (0x593, 0x59f):                     # SUATOM (avec retour) / SURED, poignée en banque constante
        # établi sur fog_noise_height : accumulation ADD (op 0) puis saturation OR (op 6) dans une image 3D
        # d'entiers 32 bits ; coordonnées Ra.., donnée Rb, dim aux bits 61-63 (numérotation de SULD/SUST)
        i.name = "suatom" if i.opc == 0x593 else "sured"
        i.dst = Reg(i.f(16, 24)) if i.opc == 0x593 else None
        i.srcs = [Reg(i.f(24, 32)), Reg(i.f(32, 40))]
        i.mods = {"dim": i.f(61, 64), "op": i.f(87, 91), "type": i.f(73, 76), "tbank": i.f(54, 59),
                  "toff_cb": i.f(40, 54) * 4, "fmt": True, "mask": 1}
    elif i.opc == 0x947 or i.opc == 0x547:            # BRA
        i.name = "bra"
        off = sext(i.f(34, 82), 48) * 4
        i.target = pc + 16 + off
        i.psrc = [pred_at(W, 87, 90)] if i.opc == 0x947 else [Pred(i.f(24, 27), bool(i.b(27)), True)]
    elif i.opc == 0x949:                              # BRX (saut indirect, tables de saut)
        # cible = (instruction suivante + immédiat) + Ra ; le compilateur règle l'immédiat pour que la
        # base vaille 0 : Ra est alors une adresse d'instruction lue dans la banque 1 (table de saut)
        i.name = "brx"
        i.srcs = [Reg(i.f(24, 32))]
        i.mods = {"base": pc + 16 + sext(i.f(34, 82), 48) * 4}
        i.psrc = [pred_at(W, 87, 90)]
    elif i.opc == 0x945:                              # BSSY
        i.name = "bssy"
        i.target = pc + 16 + sext(i.f(34, 64), 30) * 4
        i.mods = {"bar": i.f(16, 20)}
    elif i.opc == 0x941:                              # BSYNC
        i.name = "bsync"
        i.mods = {"bar": i.f(16, 20)}
    elif i.opc == 0x942:                              # BREAK
        i.name = "break"
    elif i.opc == 0x944:                              # CALL.REL
        i.target = pc + 16 + sext(i.f(34, 82), 48) * 4
        if (W >> 86) & 1:
            # CALL.REL.NOINC : saut sans empiler d'adresse de retour. Le compilateur s'en sert pour sortir
            # d'une boucle divergente (@P CALL.NOINC sortie ; BRA tête ; sortie: BSYNC) et pour appeler
            # un sous-programme dont l'adresse de retour est rangée dans un registre (RET.REL.NODEC).
            # L'empiler comme un vrai appel faisait déborder la pile des boucles imbriquées : écritures
            # hors du tableau local, faute de page GPU et périphérique perdu (cinématique).
            i.name = "bra"
            i.mods = {"noinc": True}
            i.psrc = [pred_at(W, 87, 90)]
        else:
            i.name = "call"
    elif i.opc == 0x950:                              # RET
        if (W >> 86) & 1:
            # RET.REL.NODEC Ra : retour à (instruction suivante + immédiat) + Ra ; l'immédiat est réglé
            # pour que la base vaille 0 et Ra contient l'adresse rangée par l'appelant (MOV Rx, adresse)
            i.name = "retr"
            i.srcs = [Reg(i.f(24, 32))]
            i.mods = {"base": pc + 16 + sext(i.f(34, 82), 48) * 4}
            i.psrc = [pred_at(W, 87, 90)]
        else:
            i.name = "ret"
    elif i.opc == 0x94d:                              # EXIT
        i.name = "exit"
        i.psrc = [pred_at(W, 87, 90)]
    elif i.opc == 0x95b:                              # KILL
        i.name = "kill"
        i.psrc = [pred_at(W, 87, 90)]
    elif i.opc in (0x918, 0x91a, 0x946, 0x948, 0x148, 0x992, 0x98f, 0x355, 0x356):   # NOP, DEPBAR, YIELD, WARPSYNC…
        i.name = {0x918: "nop", 0x91a: "nop", 0x946: "nop", 0x948: "nop", 0x148: "nop", 0x992: "membar",
                  0x98f: "nop", 0x355: "nop", 0x356: "nop"}[i.opc]
    elif i.opc in (0xb1d, 0x31d):                     # BAR
        i.name = "bar"
    elif i.opc in (0x389, 0x589, 0x989, 0xf89):       # SHFL
        i.name = "shfl"
        i.dst = Reg(i.f(16, 24))
        lane = Reg(i.f(32, 40)) if i.opc in (0x389, 0x589) else Imm(i.f(53, 58))
        c = Reg(i.f(64, 72)) if i.opc in (0x389, 0x989) else Imm(i.f(40, 53))
        i.srcs = [Reg(i.f(24, 32)), lane, c]
        i.pdst = [pred_at(W, 81, 99)]
        i.mods = {"op": {0: "idx", 1: "up", 2: "down", 3: "bfly"}[i.f(58, 60)]}
    elif i.opc in (0x806, 0x886):                     # VOTE
        i.name = "vote"
        i.dst = Reg(i.f(16, 24) if i.opc == 0x806 else i.f(16, 22), uni=i.opc == 0x886)
        i.pdst = [pred_at(W, 81, 99)]
        i.psrc = [pred_at(W, 87, 90)]
        i.mods = {"op": {0: "all", 1: "any", 2: "eq"}.get(i.f(72, 74), "?")}
    else:
        i.name = f"?{i.opc:03x}"
    return i


def disasm(raw: bytes):
    out = []
    for pc in range(0, len(raw) - 15, 16):
        lo, hi = struct.unpack_from("<QQ", raw, pc)
        out.append(decode(pc, lo | (hi << 64)))
    return out
