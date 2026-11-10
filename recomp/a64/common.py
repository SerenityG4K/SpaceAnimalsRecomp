"""Briques communes du traducteur A64 → C++ : représentation d'une instruction traduite,
accès aux registres, conditions, constantes."""

M64 = (1 << 64) - 1
M32 = (1 << 32) - 1

# Genres d'instruction (flot de contrôle)
SEQ = "seq"        # continue à l'instruction suivante
B = "b"            # saut direct inconditionnel
BCOND = "bcond"    # saut direct conditionnel (cond = expression C)
BL = "bl"          # appel direct
BR = "br"          # saut indirect (reg)
BLR = "blr"        # appel indirect (reg)
RET = "ret"        # retour (reg)
TRAP = "trap"      # brk/udf/hlt : termine le bloc


class Insn:
    __slots__ = ("pc", "word", "kind", "code", "target", "cond", "reg", "mem_base", "mem_index",
                 "unimpl", "op", "fp")

    def __init__(self, pc, word, kind=SEQ, code="", target=None, cond=None, reg=None, op=""):
        self.pc = pc
        self.word = word
        self.kind = kind
        self.code = code          # C++ (sémantique hors flot de contrôle)
        self.target = target      # adresse cible (B, BCOND, BL)
        self.cond = cond          # expression C de la condition (BCOND)
        self.reg = reg            # expression C du registre cible (BR, BLR, RET)
        self.mem_base = None      # registre de base d'un accès mémoire (pour le fuzzing)
        self.mem_index = None     # registre d'index d'un accès mémoire (pour le fuzzing)
        self.unimpl = False
        self.op = op              # nom court de l'opération (statistiques)
        self.fp = False           # touche à l'état FP/SIMD

    def __repr__(self):
        return f"<Insn {self.pc:#x} {self.word:08x} {self.op} {self.kind}>"


class Unimplemented(Exception):
    pass


class Unallocated(Exception):
    pass


def sext(v, bits):
    v &= (1 << bits) - 1
    return v - (1 << bits) if v >> (bits - 1) else v


def hx(v, bits=64):
    """Constante C non signée."""
    if bits == 64:
        return f"0x{v & M64:x}ull"
    return f"0x{v & M32:x}u"


def X(n, sp=False):
    """Lecture 64 bits du registre n (31 = SP ou XZR)."""
    if n == 31:
        return "ctx->sp" if sp else "UINT64_C(0)"
    return f"ctx->x[{n}]"


def W(n, sp=False):
    """Lecture 32 bits du registre n."""
    if n == 31:
        return "(uint32_t)ctx->sp" if sp else "UINT32_C(0)"
    return f"(uint32_t)ctx->x[{n}]"


def R(n, sf, sp=False):
    return X(n, sp) if sf else W(n, sp)


def setX(n, e, sp=False):
    if n == 31:
        return f"ctx->sp = {e};" if sp else f"(void)({e});"
    return f"ctx->x[{n}] = {e};"


def setW(n, e, sp=False):
    if n == 31:
        return f"ctx->sp = (uint32_t)({e});" if sp else f"(void)({e});"
    return f"ctx->x[{n}] = (uint32_t)({e});"


def setR(n, sf, e, sp=False):
    return setX(n, e, sp) if sf else setW(n, e, sp)


def T(sf):
    return "uint64_t" if sf else "uint32_t"


def ST(sf):
    return "int64_t" if sf else "int32_t"


def SZ(sf):
    return 64 if sf else 32


COND = [
    "ctx->zf", "!ctx->zf", "ctx->cf", "!ctx->cf", "ctx->nf", "!ctx->nf", "ctx->vf", "!ctx->vf",
    "(ctx->cf && !ctx->zf)", "!(ctx->cf && !ctx->zf)", "(ctx->nf == ctx->vf)", "(ctx->nf != ctx->vf)",
    "(!ctx->zf && ctx->nf == ctx->vf)", "!(!ctx->zf && ctx->nf == ctx->vf)", "1", "1",
]
COND_NAMES = ["eq", "ne", "hs", "lo", "mi", "pl", "vs", "vc", "hi", "ls", "ge", "lt", "gt", "le", "al", "nv"]


def set_nzcv_imm(nzcv):
    return (f"ctx->nf = {(nzcv >> 3) & 1}; ctx->zf = {(nzcv >> 2) & 1}; "
            f"ctx->cf = {(nzcv >> 1) & 1}; ctx->vf = {nzcv & 1};")


def decode_bit_masks(N, imms, immr, immediate, datasize):
    """DecodeBitMasks de l'ARM ARM. Renvoie (wmask, tmask)."""
    v = (N << 6) | (~imms & 0x3f)
    if v == 0:
        raise Unallocated("bitmask")
    length = v.bit_length() - 1
    if length < 1:
        raise Unallocated("bitmask")
    levels = (1 << length) - 1
    if immediate and (imms & levels) == levels:
        raise Unallocated("bitmask")
    S = imms & levels
    R_ = immr & levels
    d = (S - R_) & levels
    esize = 1 << length
    emask = (1 << esize) - 1
    welem = (1 << (S + 1)) - 1
    telem = (1 << (d + 1)) - 1
    wrot = ((welem >> R_) | (welem << (esize - R_))) & emask if R_ else welem
    wmask = tmask = 0
    for i in range(datasize // esize):
        wmask |= wrot << (i * esize)
        tmask |= telem << (i * esize)
    return wmask, tmask
