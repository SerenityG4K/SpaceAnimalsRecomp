"""Traduction d'une instruction A64 (mot de 32 bits) en C++.

Le décodage suit l'arbre de l'ARM ARM (classes op0 = bits[28:25]). Chaque fonction renvoie un
`Insn` : le code C++ de la sémantique et, pour les branchements, les informations de flot de
contrôle (la génération des `goto`/appels est faite par `codegen`).

Conventions du C++ émis (voir runtime/cpu.h) :
  ctx->x[0..30], ctx->sp, drapeaux ctx->nf/z/c/v (0/1), ctx->v[0..31] (V128),
  rd8/rd16/rd32/rd64/rd128 et wr8.. pour la mémoire invitée (identité adresse invité = hôte),
  helpers adds64/subs64/... pour les drapeaux, at_* pour les atomiques LSE.
"""
from .common import (B, BCOND, BL, BLR, BR, COND, M32, M64, RET, SEQ, TRAP, Insn, R, ST, SZ, T,
                     Unallocated, Unimplemented, W, X, decode_bit_masks, hx, set_nzcv_imm, setR,
                     setW, setX, sext)
from . import simd


def translate(w, pc):
    """Traduit le mot `w` situé à l'adresse invitée `pc`. Ne lève jamais : les encodages non
    gérés deviennent un appel à rt_unimpl."""
    try:
        op0 = (w >> 25) & 0xF
        if op0 in (8, 9):
            i = dp_imm(w, pc)
        elif op0 in (10, 11):
            i = branch_sys(w, pc)
        elif op0 & 0b0101 == 0b0100:
            i = ldst(w, pc)
        elif op0 & 0b0111 == 0b0101:
            i = dp_reg(w, pc)
        elif op0 & 0b0111 == 0b0111:
            i = simd.simd_fp(w, pc)
        elif w >> 16 == 0:
            i = Insn(pc, w, TRAP, f"rt_trap(ctx, {hx(pc)}, 0x{w:08x}u);", op="udf")
        else:
            raise Unallocated("op0")
        return i
    except Unimplemented as e:
        i = Insn(pc, w, SEQ, f"rt_unimpl(ctx, {hx(pc)}, 0x{w:08x}u); /* {e} */", op=f"?{e}")
        i.unimpl = True
        return i
    except Unallocated as e:
        i = Insn(pc, w, TRAP, f"rt_undefined(ctx, {hx(pc)}, 0x{w:08x}u); /* {e} */", op="undef")
        i.unimpl = True
        return i


# =====================================================================================
# Traitement de données — immédiat
# =====================================================================================
def dp_imm(w, pc):
    op = (w >> 23) & 7
    sf = w >> 31
    rd = w & 31
    rn = (w >> 5) & 31
    if op in (0, 1):  # ADR / ADRP
        imm = sext((((w >> 5) & 0x7FFFF) << 2) | ((w >> 29) & 3), 21)
        if w >> 31:
            val = ((pc & ~0xFFF) + (imm << 12)) & M64
            return Insn(pc, w, code=setX(rd, hx(val)), op="adrp")
        return Insn(pc, w, code=setX(rd, hx((pc + imm) & M64)), op="adr")
    if op == 2:  # add/sub immédiat
        sub = (w >> 30) & 1
        S = (w >> 29) & 1
        sh = (w >> 22) & 1
        imm = ((w >> 10) & 0xFFF) << (12 if sh else 0)
        a = R(rn, sf, sp=True)
        if S:
            fn = ("subs" if sub else "adds") + str(SZ(sf))
            return Insn(pc, w, code=setR(rd, sf, f"{fn}(ctx, {a}, {hx(imm, SZ(sf))})"),
                        op=fn)
        e = f"{a} {'-' if sub else '+'} {hx(imm, SZ(sf))}"
        return Insn(pc, w, code=setR(rd, sf, e, sp=True), op="sub" if sub else "add")
    if op == 3:
        raise Unimplemented("addg/subg (MTE)")
    if op == 4:  # logique immédiat
        opc = (w >> 29) & 3
        N = (w >> 22) & 1
        if not sf and N:
            raise Unallocated("logimm")
        imm, _ = decode_bit_masks(N, (w >> 10) & 0x3F, (w >> 16) & 0x3F, True, SZ(sf))
        a = R(rn, sf)
        c = hx(imm, SZ(sf))
        if opc == 3:
            return Insn(pc, w, code=setR(rd, sf, f"logic{SZ(sf)}(ctx, {a} & {c})"), op="ands")
        opname = ["&", "|", "^"][opc]
        return Insn(pc, w, code=setR(rd, sf, f"{a} {opname} {c}", sp=True),
                    op=["and", "orr", "eor"][opc])
    if op == 5:  # move wide
        opc = (w >> 29) & 3
        hw = (w >> 21) & 3
        imm16 = (w >> 5) & 0xFFFF
        if opc == 1 or (not sf and hw >= 2):
            raise Unallocated("movw")
        sh = hw * 16
        bits = SZ(sf)
        if opc == 0:
            return Insn(pc, w, code=setR(rd, sf, hx(~(imm16 << sh), bits)), op="movn")
        if opc == 2:
            return Insn(pc, w, code=setR(rd, sf, hx(imm16 << sh, bits)), op="movz")
        keep = hx(~(0xFFFF << sh), bits)
        return Insn(pc, w, code=setR(rd, sf, f"({R(rd, sf)} & {keep}) | {hx(imm16 << sh, bits)}"),
                    op="movk")
    if op == 6:
        return bitfield(w, pc, sf, rd, rn)
    # op == 7 : EXTR
    if (w >> 29) & 3 or ((w >> 22) & 1) != sf or (w >> 21) & 1:
        raise Unallocated("extr")
    rm = (w >> 16) & 31
    lsb = (w >> 10) & 0x3F
    bits = SZ(sf)
    if not sf and lsb >= 32:
        raise Unallocated("extr")
    if lsb == 0:
        e = R(rm, sf)
    else:
        e = f"({R(rm, sf)} >> {lsb}) | ({R(rn, sf)} << {bits - lsb})"
    return Insn(pc, w, code=setR(rd, sf, e), op="extr")


def mask(n):
    return (1 << n) - 1


def bitfield(w, pc, sf, rd, rn):
    opc = (w >> 29) & 3
    N = (w >> 22) & 1
    immr = (w >> 16) & 0x3F
    imms = (w >> 10) & 0x3F
    bits = SZ(sf)
    if opc == 3 or N != sf or (not sf and (immr >= 32 or imms >= 32)):
        raise Unallocated("bitfield")
    t, st = T(sf), ST(sf)
    src = R(rn, sf)
    if opc == 2:  # UBFM
        if imms >= immr:
            width = imms - immr + 1
            e = f"({src} >> {immr})" if immr else src
            if width < bits:
                e = f"{e} & {hx(mask(width), bits)}"
            return Insn(pc, w, code=setR(rd, sf, e), op="ubfx")
        width = imms + 1
        sh = bits - immr
        return Insn(pc, w, code=setR(rd, sf, f"({t})(({src} & {hx(mask(width), bits)}) << {sh})"),
                    op="ubfiz")
    if opc == 0:  # SBFM
        if imms >= immr:
            # extraction signée des bits [imms:immr]
            e = f"({t})(({st})({t})({src} << {bits - 1 - imms}) >> {bits - 1 - imms + immr})"
            return Insn(pc, w, code=setR(rd, sf, e), op="sbfx")
        sh = bits - immr
        e = (f"({t})((({t})(({st})({t})({src} << {bits - 1 - imms}) >> {bits - 1 - imms})) "
             f"<< {sh})")
        return Insn(pc, w, code=setR(rd, sf, e), op="sbfiz")
    # BFM
    dst = R(rd, sf)
    if imms >= immr:
        width = imms - immr + 1
        m = mask(width)
        e = f"({dst} & {hx(~m, bits)}) | (({src} >> {immr}) & {hx(m, bits)})"
        return Insn(pc, w, code=setR(rd, sf, e), op="bfxil")
    width = imms + 1
    lsb = bits - immr
    m = mask(width) << lsb
    e = f"({dst} & {hx(~m, bits)}) | ({t})(({src} << {lsb}) & {hx(m, bits)})"
    return Insn(pc, w, code=setR(rd, sf, e), op="bfi")


# =====================================================================================
# Branchements, exceptions, système
# =====================================================================================
SYSREG_RO = {
    # (op0, op1, CRn, CRm, op2) : expression C
    (3, 3, 13, 0, 2): "ctx->tpidr_el0",
    (3, 3, 13, 0, 3): "ctx->tpidrro_el0",
    (3, 3, 4, 2, 0): "rt_get_nzcv(ctx)",
    (3, 3, 4, 4, 0): "(uint64_t)ctx->fpcr",
    (3, 3, 4, 4, 1): "(uint64_t)ctx->fpsr",
    (3, 3, 14, 0, 0): "rt_cntfrq()",
    (3, 3, 14, 0, 1): "rt_cntvct()",
    (3, 3, 14, 0, 2): "rt_cntvct()",
    (3, 3, 14, 0, 5): "rt_cntvct()",  # CNTPCTSS
    (3, 3, 14, 0, 6): "rt_cntvct()",  # CNTVCTSS
    (3, 3, 0, 0, 7): "UINT64_C(4)",   # DCZID_EL0 : DC ZVA sur 64 octets
}


def branch_sys(w, pc):
    if (w >> 24) & 0xFE == 0x54:  # B.cond / BC.cond
        if (w >> 24) & 1:
            raise Unallocated("bcond")
        imm = sext((w >> 5) & 0x7FFFF, 19) * 4
        cond = w & 15
        t = (pc + imm) & M64
        if cond >= 14:
            return Insn(pc, w, B, target=t, op="b")
        return Insn(pc, w, BCOND, target=t, cond=COND[cond], op="b.cond")
    if (w >> 24) == 0xD4:
        return exception_gen(w, pc)
    if (w >> 22) == 0x354:
        return system(w, pc)
    if (w >> 25) == 0x6B:
        return branch_reg(w, pc)
    if (w >> 26) & 0x1F == 0b00101:
        t = (pc + sext(w & 0x3FFFFFF, 26) * 4) & M64
        if w >> 31:
            return Insn(pc, w, BL, target=t, op="bl")
        return Insn(pc, w, B, target=t, op="b")
    if (w >> 25) & 0x3F == 0b011010:  # CBZ/CBNZ
        sf = w >> 31
        rt = w & 31
        t = (pc + sext((w >> 5) & 0x7FFFF, 19) * 4) & M64
        nz = (w >> 24) & 1
        return Insn(pc, w, BCOND, target=t, cond=f"{R(rt, sf)} {'!=' if nz else '=='} 0",
                    op="cbnz" if nz else "cbz")
    if (w >> 25) & 0x3F == 0b011011:  # TBZ/TBNZ
        bit = ((w >> 31) << 5) | ((w >> 19) & 31)
        rt = w & 31
        t = (pc + sext((w >> 5) & 0x3FFF, 14) * 4) & M64
        nz = (w >> 24) & 1
        return Insn(pc, w, BCOND, target=t,
                    cond=f"(({X(rt)} >> {bit}) & 1) {'!=' if nz else '=='} 0",
                    op="tbnz" if nz else "tbz")
    raise Unallocated("branch")


def exception_gen(w, pc):
    opc = (w >> 21) & 7
    imm16 = (w >> 5) & 0xFFFF
    ll = w & 3
    if opc == 0 and ll == 1:
        return Insn(pc, w, SEQ, f"rt_svc(ctx, 0x{imm16:x}u, {hx(pc)});", op="svc")
    if opc == 1 and ll == 0:
        return Insn(pc, w, TRAP, f"rt_trap(ctx, {hx(pc)}, 0x{w:08x}u);", op="brk")
    if opc == 2 and ll == 0:
        return Insn(pc, w, TRAP, f"rt_trap(ctx, {hx(pc)}, 0x{w:08x}u);", op="hlt")
    raise Unimplemented("hvc/smc/dcps")


def system(w, pc):
    L = (w >> 21) & 1
    op0 = (w >> 19) & 3
    op1 = (w >> 16) & 7
    crn = (w >> 12) & 15
    crm = (w >> 8) & 15
    op2 = (w >> 5) & 7
    rt = w & 31
    if op0 == 0 and not L:
        if crn == 2 and rt == 31:
            # HINT : NOP, YIELD, WFE, PAC*/AUT* (PAC désactivé → nop), BTI, XPACLRI...
            return Insn(pc, w, SEQ, "", op="hint")
        if crn == 3 and rt == 31:
            if op2 == 2:
                return Insn(pc, w, SEQ, "ctx->excl_valid = 0;", op="clrex")
            if op2 in (4, 5):  # DSB / DMB
                kind = crm & 3
                if kind == 1:
                    return Insn(pc, w, SEQ, "__atomic_thread_fence(__ATOMIC_ACQUIRE);", op="dmb")
                if kind == 2:
                    return Insn(pc, w, SEQ, "__atomic_thread_fence(__ATOMIC_RELEASE);", op="dmb")
                return Insn(pc, w, SEQ, "__atomic_thread_fence(__ATOMIC_SEQ_CST);", op="dmb")
            if op2 in (6, 7):  # ISB / SB
                return Insn(pc, w, SEQ, "__atomic_signal_fence(__ATOMIC_SEQ_CST);", op="isb")
            raise Unimplemented("barrier")
        if crn == 4 and rt == 31:
            return Insn(pc, w, SEQ, f"/* msr pstate {op1},{op2},#{crm} ignoré */", op="msr_imm")
        raise Unimplemented("hint/barrier")
    if op0 == 1:
        if L:
            raise Unimplemented("sysl")
        if op1 == 3 and crn == 7 and crm == 4 and op2 == 1:
            return Insn(pc, w, SEQ, f"rt_dc_zva({X(rt)});", op="dc_zva")
        # maintenance de cache (DC CVAC/CIVAC/CVAU, IC IVAU...) : sans effet ici
        return Insn(pc, w, SEQ, f"/* sys {op1},c{crn},c{crm},{op2} ignoré */", op="sys")
    enc = (op0, op1, crn, crm, op2)
    enc_c = f"0x{(op0 << 14) | (op1 << 11) | (crn << 7) | (crm << 3) | op2:x}u"
    if L:  # MRS
        e = SYSREG_RO.get(enc, f"rt_mrs(ctx, {enc_c}, {hx(pc)})")
        return Insn(pc, w, SEQ, setX(rt, e), op="mrs")
    v = X(rt)
    if enc == (3, 3, 13, 0, 2):
        code = f"ctx->tpidr_el0 = {v};"
    elif enc == (3, 3, 4, 2, 0):
        code = f"rt_set_nzcv(ctx, {v});"
    elif enc == (3, 3, 4, 4, 0):
        code = f"rt_set_fpcr(ctx, (uint32_t){v});"
    elif enc == (3, 3, 4, 4, 1):
        code = f"ctx->fpsr = (uint32_t){v};"
    else:
        code = f"rt_msr(ctx, {enc_c}, {v}, {hx(pc)});"
    return Insn(pc, w, SEQ, code, op="msr")


def branch_reg(w, pc):
    opc = (w >> 21) & 15
    op2 = (w >> 16) & 31
    op3 = (w >> 10) & 63
    rn = (w >> 5) & 31
    if op2 != 31:
        raise Unallocated("branch_reg")
    if opc in (0, 8):     # BR, BRAAZ/BRABZ, BRAA/BRAB
        return Insn(pc, w, BR, reg=X(rn), op="br")
    if opc in (1, 9):     # BLR, BLRAA...
        return Insn(pc, w, BLR, reg=X(rn), op="blr")
    if opc == 2:          # RET, RETAA/RETAB
        if op3 in (2, 3):
            rn = 30
        return Insn(pc, w, RET, reg=X(rn), op="ret")
    if opc in (4, 5):
        return Insn(pc, w, TRAP, f"rt_trap(ctx, {hx(pc)}, 0x{w:08x}u);", op="eret")
    raise Unallocated("branch_reg")


# =====================================================================================
# Chargements / rangements
# =====================================================================================
MEMT = {0: "uint8_t", 1: "uint16_t", 2: "uint32_t", 3: "uint64_t"}
SMEMT = {0: "int8_t", 1: "int16_t", 2: "int32_t", 3: "int64_t"}
RDFN = {0: "rd8", 1: "rd16", 2: "rd32", 3: "rd64", 4: "rd128"}
WRFN = {0: "wr8", 1: "wr16", 2: "wr32", 3: "wr64", 4: "wr128"}
ATSZ = {0: "8", 1: "16", 2: "32", 3: "64"}


def gpr_load(rt, size, signed, dest64, addr):
    """Code de chargement d'un GPR depuis `addr` (expression)."""
    v = f"{RDFN[size]}({addr})"
    if signed:
        v = f"({SMEMT[size]}){v}"
        if dest64:
            return setX(rt, f"(uint64_t)(int64_t){v}")
        return setW(rt, f"(uint32_t)(int32_t){v}")
    return setX(rt, f"(uint64_t){v}")


def vreg_load(rt, size, addr):
    if size == 4:
        return f"ctx->v[{rt}] = rd128({addr});"
    return f"ctx->v[{rt}] = v128_zext{8 << size}({RDFN[size]}({addr}));"


def vreg_store(rt, size, addr):
    if size == 4:
        return f"wr128({addr}, ctx->v[{rt}]);"
    field = {0: "u8", 1: "u16", 2: "u32", 3: "u64"}[size]
    return f"{WRFN[size]}({addr}, ctx->v[{rt}].{field}[0]);"


def gpr_store(rt, size, addr):
    v = X(rt) if size == 3 else f"({MEMT[size]}){X(rt)}"
    return f"{WRFN[size]}({addr}, {v});"


def ldst_access(V, size, opc):
    """Renvoie (genre, taille log2, signé, dest64) ; genre ∈ st, ld, vst, vld, prfm."""
    if not V:
        if opc == 0:
            return "st", size, False, False
        if opc == 1:
            return "ld", size, False, size == 3
        if opc == 2:
            if size == 3:
                return "prfm", 3, False, False
            return "ld", size, True, True
        if size >= 2:
            raise Unallocated("ldst opc")
        return "ld", size, True, False
    if opc & 2:
        if size != 0:
            raise Unallocated("ldst simd")
        return ("vst" if opc == 2 else "vld"), 4, False, False
    return ("vst" if opc == 0 else "vld"), size, False, False


def access_code(kind, rt, size, signed, dest64, addr):
    if kind == "st":
        return gpr_store(rt, size, addr)
    if kind == "ld":
        return gpr_load(rt, size, signed, dest64, addr)
    if kind == "vst":
        return vreg_store(rt, size, addr)
    if kind == "vld":
        return vreg_load(rt, size, addr)
    return ""  # prfm


def extend_reg(rm, option, shift, sf):
    """Opérande registre étendu (add/sub étendu, ldr à registre)."""
    ty = option & 3
    signed = option >> 2
    if sf:
        base = {0: f"(uint64_t)(uint8_t){X(rm)}", 1: f"(uint64_t)(uint16_t){X(rm)}",
                2: f"(uint64_t)(uint32_t){X(rm)}", 3: X(rm)}[ty]
        if signed:
            base = {0: f"(uint64_t)(int64_t)(int8_t){X(rm)}", 1: f"(uint64_t)(int64_t)(int16_t){X(rm)}",
                    2: f"(uint64_t)(int64_t)(int32_t){X(rm)}", 3: X(rm)}[ty]
    else:
        base = {0: f"(uint32_t)(uint8_t){X(rm)}", 1: f"(uint32_t)(uint16_t){X(rm)}",
                2: W(rm), 3: W(rm)}[ty]
        if signed:
            base = {0: f"(uint32_t)(int32_t)(int8_t){X(rm)}", 1: f"(uint32_t)(int32_t)(int16_t){X(rm)}",
                    2: W(rm), 3: W(rm)}[ty]
    if shift:
        return f"(({T(sf)}){base} << {shift})"
    return base


def ldst(w, pc):
    op0 = w >> 28
    V = (w >> 26) & 1
    op2 = (w >> 23) & 3
    op3 = (w >> 16) & 0x3F
    op4 = (w >> 10) & 3
    lo = op0 & 3
    if op0 & 0b1011 == 0 and V:
        return simd.ldst_struct(w, pc)
    if lo == 0:
        if V or op2 & 2:
            raise Unimplemented("ldst class")
        return ldst_exclusive(w, pc)
    if lo == 1:
        if op2 & 2:
            if V or op3 & 0x20 or op4:
                raise Unimplemented("ldapr/stlr unscaled")
            return ldapur(w, pc)
        return load_literal(w, pc)
    if lo == 2:
        return ldst_pair(w, pc)
    # lo == 3
    if op2 & 2:
        return ldst_reg(w, pc, "uimm")
    if not op3 & 0x20:
        return ldst_reg(w, pc, ["unscaled", "post", "unpriv", "pre"][op4])
    if op4 == 0:
        return atomic_op(w, pc)
    if op4 == 2:
        return ldst_reg(w, pc, "regoff")
    raise Unimplemented("ldraa/ldrab")


def ldst_reg(w, pc, mode):
    size = w >> 30
    V = (w >> 26) & 1
    opc = (w >> 22) & 3
    rn = (w >> 5) & 31
    rt = w & 31
    kind, sz, signed, dest64 = ldst_access(V, size, opc)
    base = X(rn, sp=True)
    wb = None
    if mode == "uimm":
        off = ((w >> 10) & 0xFFF) << sz
        addr = f"{base} + {off}" if off else base
    elif mode == "regoff":
        rm = (w >> 16) & 31
        option = (w >> 13) & 7
        S = (w >> 12) & 1
        if not option & 2:
            raise Unallocated("ldr regoff option")
        addr = f"{base} + {extend_reg(rm, option, sz if S else 0, 1)}"
    else:
        imm = sext((w >> 12) & 0x1FF, 9)
        if mode in ("unscaled", "unpriv"):
            addr = f"{base} + {hx(imm)}" if imm else base
        elif mode == "pre":
            addr = f"{base} + {hx(imm)}"
            wb = "a"
        else:  # post
            addr = base
            wb = f"a + {hx(imm)}"
    code = access_code(kind, rt, sz, signed, dest64, "a")
    if wb:
        code = f"uint64_t a = {addr}; {code} {setX(rn, wb, sp=True)}"
    else:
        code = f"uint64_t a = {addr}; {code}"
    if kind == "prfm":
        code = ""
    i = Insn(pc, w, SEQ, code, op=f"{kind}{8 << sz}_{mode}")
    i.mem_base = rn
    if mode == "regoff":
        i.mem_index = (w >> 16) & 31
    i.fp = bool(V)
    return i


def load_literal(w, pc):
    opc = w >> 30
    V = (w >> 26) & 1
    rt = w & 31
    a = (pc + sext((w >> 5) & 0x7FFFF, 19) * 4) & M64
    ac = hx(a)
    if not V:
        if opc == 0:
            code = setX(rt, f"(uint64_t)rd32({ac})")
        elif opc == 1:
            code = setX(rt, f"rd64({ac})")
        elif opc == 2:
            code = setX(rt, f"(uint64_t)(int64_t)(int32_t)rd32({ac})")
        else:
            code = ""
    else:
        if opc == 3:
            raise Unallocated("ldr lit")
        code = vreg_load(rt, [2, 3, 4][opc], ac)
    i = Insn(pc, w, SEQ, code, op="ldr_lit")
    i.fp = bool(V)
    return i


def ldst_pair(w, pc):
    opc = w >> 30
    V = (w >> 26) & 1
    mode = (w >> 23) & 3   # 0 no-alloc, 1 post, 2 offset, 3 pre
    L = (w >> 22) & 1
    imm7 = sext((w >> 15) & 0x7F, 7)
    rt2 = (w >> 10) & 31
    rn = (w >> 5) & 31
    rt = w & 31
    if V:
        if opc == 3:
            raise Unallocated("ldp simd")
        sz = opc + 2
    else:
        if opc == 3:
            raise Unallocated("ldp")
        if opc == 1 and not L:
            raise Unimplemented("stgp (MTE)")
        sz = 3 if opc == 2 else 2
    if mode == 0 and opc == 1 and not V:
        raise Unallocated("ldnp")
    off = imm7 << sz
    base = X(rn, sp=True)
    step = 1 << sz
    if mode == 1:
        addr = base
    else:
        addr = f"{base} + {hx(off)}" if off else base
    parts = []
    if V:
        if L:
            parts.append(vreg_load(rt, sz, "a"))
            parts.append(vreg_load(rt2, sz, f"a + {step}"))
        else:
            parts.append(vreg_store(rt, sz, "a"))
            parts.append(vreg_store(rt2, sz, f"a + {step}"))
    else:
        if L:
            signed = opc == 1
            dest64 = sz == 3 or signed
            # charger les deux valeurs avant d'écrire (rt peut valoir rn)
            t = "uint64_t" if sz == 3 else "uint32_t"
            parts.append(f"{t} v1 = {RDFN[sz]}(a); {t} v2 = {RDFN[sz]}(a + {step});")
            if signed:
                parts.append(setX(rt, "(uint64_t)(int64_t)(int32_t)v1"))
                parts.append(setX(rt2, "(uint64_t)(int64_t)(int32_t)v2"))
            else:
                parts.append(setX(rt, "(uint64_t)v1"))
                parts.append(setX(rt2, "(uint64_t)v2"))
        else:
            parts.append(gpr_store(rt, sz, "a"))
            parts.append(gpr_store(rt2, sz, f"a + {step}"))
    code = f"uint64_t a = {addr}; " + " ".join(parts)
    if mode == 1:
        code += " " + setX(rn, f"a + {hx(off)}", sp=True)
    elif mode == 3:
        code += " " + setX(rn, "a", sp=True)
    name = ("ld" if L else "st") + "p" + ("v" if V else "") + ["n", "_post", "", "_pre"][mode]
    i = Insn(pc, w, SEQ, code, op=name)
    i.mem_base = rn
    i.fp = bool(V)
    return i


def ldst_exclusive(w, pc):
    size = w >> 30
    o2 = (w >> 23) & 1
    L = (w >> 22) & 1
    o1 = (w >> 21) & 1
    rs = (w >> 16) & 31
    o0 = (w >> 15) & 1
    rt2 = (w >> 10) & 31
    rn = (w >> 5) & 31
    rt = w & 31
    base = X(rn, sp=True)
    sz = ATSZ[size]
    if o2 and o1:  # CAS*
        if rt2 != 31:
            raise Unallocated("cas")
        old = f"at_cas{sz}(a, ({MEMT[size]}){X(rs)}, ({MEMT[size]}){X(rt)})"
        code = f"uint64_t a = {base}; {setX(rs, f'(uint64_t){old}')}"
        i = Insn(pc, w, SEQ, code, op=f"cas{sz}")
        i.mem_base = rn
        return i
    if not o2 and o1 and size < 2:  # CASP*
        if rt2 != 31 or rs & 1 or rt & 1:
            raise Unallocated("casp")
        bits = 32 if size == 0 else 64
        code = (f"uint64_t a = {base}; at_casp{bits}(a, &ctx->x[{rs}], &ctx->x[{rs + 1}], "
                f"{X(rt)}, {X(rt + 1)});")
        if rs == 30:
            raise Unimplemented("casp x30")
        i = Insn(pc, w, SEQ, code, op=f"casp{bits}")
        i.mem_base = rn
        return i
    if o2 and not o1:  # LDAR/STLR (et LDLAR/STLLR)
        if L:
            code = f"uint64_t a = {base}; {setX(rt, f'(uint64_t)at_load{sz}(a)')}"
            op = "ldar"
        else:
            code = f"uint64_t a = {base}; at_store{sz}(a, ({MEMT[size]}){X(rt)});"
            op = "stlr"
        i = Insn(pc, w, SEQ, code, op=op)
        i.mem_base = rn
        return i
    # exclusifs : LDXR/STXR/LDXP/STXP (moniteur exclusif émulé par CAS)
    if not o1:
        if L:
            code = f"uint64_t a = {base}; {setX(rt, f'rt_ldxr{sz}(ctx, a)')}"
            op = "ldxr"
        else:
            code = (f"uint64_t a = {base}; "
                    f"{setW(rs, f'rt_stxr{sz}(ctx, a, ({MEMT[size]}){X(rt)})')}")
            op = "stxr"
    else:
        bits = 32 if size == 2 else 64
        if L:
            code = f"uint64_t a = {base}; rt_ldxp{bits}(ctx, a, {rt}, {rt2});"
            op = "ldxp"
        else:
            code = (f"uint64_t a = {base}; "
                    f"{setW(rs, f'rt_stxp{bits}(ctx, a, {X(rt)}, {X(rt2)})')}")
            op = "stxp"
    i = Insn(pc, w, SEQ, code, op=op)
    i.mem_base = rn
    return i


def ldapur(w, pc):
    size = w >> 30
    opc = (w >> 22) & 3
    imm = sext((w >> 12) & 0x1FF, 9)
    rn = (w >> 5) & 31
    rt = w & 31
    sz = ATSZ[size]
    a = f"uint64_t a = {X(rn, sp=True)} + {hx(imm)};"
    if opc == 0:
        code = f"{a} at_store{sz}(a, ({MEMT[size]}){X(rt)});"
    elif opc == 1:
        code = f"{a} {setX(rt, f'(uint64_t)at_load{sz}(a)')}"
    elif opc == 2:
        if size == 3:
            raise Unallocated("ldapurs")
        code = f"{a} {setX(rt, f'(uint64_t)(int64_t)({SMEMT[size]})at_load{sz}(a)')}"
    else:
        if size >= 2:
            raise Unallocated("ldapurs")
        code = f"{a} {setW(rt, f'(uint32_t)(int32_t)({SMEMT[size]})at_load{sz}(a)')}"
    i = Insn(pc, w, SEQ, code, op="ldapur")
    i.mem_base = rn
    return i


ATOMIC_OPS = ["add", "clr", "eor", "set", "smax", "smin", "umax", "umin"]


def atomic_op(w, pc):
    size = w >> 30
    V = (w >> 26) & 1
    rs = (w >> 16) & 31
    o3 = (w >> 15) & 1
    opc = (w >> 12) & 7
    rn = (w >> 5) & 31
    rt = w & 31
    if V:
        raise Unallocated("atomic simd")
    sz = ATSZ[size]
    base = X(rn, sp=True)
    if o3:
        if opc == 0:
            fn = f"at_swp{sz}"
        elif opc == 4:
            # LDAPR (RCpc)
            code = f"uint64_t a = {base}; {setX(rt, f'(uint64_t)at_load{sz}(a)')}"
            i = Insn(pc, w, SEQ, code, op="ldapr")
            i.mem_base = rn
            return i
        else:
            raise Unimplemented("ld64b/st64b")
    else:
        fn = f"at_{ATOMIC_OPS[opc]}{sz}"
    code = f"uint64_t a = {base}; {setX(rt, f'(uint64_t){fn}(a, ({MEMT[size]}){X(rs)})')}"
    i = Insn(pc, w, SEQ, code, op=fn)
    i.mem_base = rn
    return i


# =====================================================================================
# Traitement de données — registre
# =====================================================================================
def shift_op(rm, sf, shift, amt):
    v = R(rm, sf)
    if amt == 0:
        return v
    bits = SZ(sf)
    if shift == 0:
        return f"({T(sf)})({v} << {amt})"
    if shift == 1:
        return f"({v} >> {amt})"
    if shift == 2:
        return f"({T(sf)})(({ST(sf)}){v} >> {amt})"
    return f"ror{bits}({v}, {amt})"


def dp_reg(w, pc):
    sf = w >> 31
    op0 = (w >> 30) & 1
    op1 = (w >> 28) & 1
    op2 = (w >> 21) & 15
    op3 = (w >> 10) & 63
    rd = w & 31
    rn = (w >> 5) & 31
    rm = (w >> 16) & 31
    bits = SZ(sf)
    t = T(sf)
    if not op1:
        if not op2 & 8:  # logique registre décalé
            opc = (w >> 29) & 3
            shift = (w >> 22) & 3
            N = (w >> 21) & 1
            amt = (w >> 10) & 63
            if not sf and amt >= 32:
                raise Unallocated("log shift")
            b = shift_op(rm, sf, shift, amt)
            if N:
                b = f"({t})~{b}"
            a = R(rn, sf)
            if opc == 3:
                return Insn(pc, w, code=setR(rd, sf, f"logic{bits}(ctx, {a} & {b})"),
                            op="bics" if N else "ands")
            o = ["&", "|", "^"][opc]
            names = [["and", "orr", "eor"], ["bic", "orn", "eon"]][N]
            if opc == 1 and rn == 31 and not N and shift == 0 and amt == 0:
                return Insn(pc, w, code=setR(rd, sf, R(rm, sf)), op="mov")
            return Insn(pc, w, code=setR(rd, sf, f"{a} {o} {b}"), op=names[opc])
        sub = (w >> 30) & 1
        S = (w >> 29) & 1
        if not op2 & 1:  # add/sub registre décalé
            shift = (w >> 22) & 3
            amt = (w >> 10) & 63
            if shift == 3 or (not sf and amt >= 32):
                raise Unallocated("addsub shift")
            a = R(rn, sf)
            b = shift_op(rm, sf, shift, amt)
            if S:
                fn = ("subs" if sub else "adds") + str(bits)
                return Insn(pc, w, code=setR(rd, sf, f"{fn}(ctx, {a}, {b})"), op=fn)
            return Insn(pc, w, code=setR(rd, sf, f"{a} {'-' if sub else '+'} {b}"),
                        op="sub" if sub else "add")
        # add/sub registre étendu
        if (w >> 22) & 3:
            raise Unallocated("addsub ext")
        option = (w >> 13) & 7
        imm3 = (w >> 10) & 7
        if imm3 > 4:
            raise Unallocated("addsub ext shift")
        a = R(rn, sf, sp=True)
        b = extend_reg(rm, option, imm3, sf)
        if S:
            fn = ("subs" if sub else "adds") + str(bits)
            return Insn(pc, w, code=setR(rd, sf, f"{fn}(ctx, {a}, {b})"), op=fn + "_ext")
        return Insn(pc, w, code=setR(rd, sf, f"{a} {'-' if sub else '+'} {b}", sp=True),
                    op=("sub" if sub else "add") + "_ext")
    # op1 == 1
    if op2 == 0:
        if op3 == 0:  # ADC/SBC
            sub = (w >> 30) & 1
            S = (w >> 29) & 1
            a = R(rn, sf)
            b = R(rm, sf)
            if sub:
                b = f"({t})~{b}"
            if S:
                return Insn(pc, w, code=setR(rd, sf, f"adcs{bits}(ctx, {a}, {b}, ctx->cf)"),
                            op="sbcs" if sub else "adcs")
            return Insn(pc, w, code=setR(rd, sf, f"{a} + {b} + ({t})ctx->cf"),
                        op="sbc" if sub else "adc")
        if op3 & 0x1F == 1:  # RMIF
            raise Unimplemented("rmif")
        if op3 & 0xF == 2:   # SETF8/16
            raise Unimplemented("setf")
        raise Unallocated("dp_reg op2=0")
    if op2 == 2:  # comparaison conditionnelle
        if (w >> 29) & 1 == 0 or (w >> 10) & 1 or (w >> 4) & 1:
            raise Unallocated("ccmp")
        sub = (w >> 30) & 1
        cond = (w >> 12) & 15
        nzcv = w & 15
        a = R(rn, sf)
        if (w >> 11) & 1:
            b = hx((w >> 16) & 31, bits)
        else:
            b = R(rm, sf)
        fn = ("subs" if sub else "adds") + str(bits)
        code = f"if ({COND[cond]}) {{ (void){fn}(ctx, {a}, {b}); }} else {{ {set_nzcv_imm(nzcv)} }}"
        return Insn(pc, w, code=code, op="ccmp" if sub else "ccmn")
    if op2 == 4:  # sélection conditionnelle
        if (w >> 29) & 1 or (w >> 11) & 1:
            raise Unallocated("csel")
        inv = (w >> 30) & 1
        o2 = (w >> 10) & 1
        cond = (w >> 12) & 15
        a = R(rn, sf)
        b = R(rm, sf)
        if not inv and not o2:
            e, op = b, "csel"
        elif not inv and o2:
            e, op = f"({t})({b} + 1)", "csinc"
        elif inv and not o2:
            e, op = f"({t})~{b}", "csinv"
        else:
            e, op = f"({t})(0 - {b})", "csneg"
        return Insn(pc, w, code=setR(rd, sf, f"({COND[cond]}) ? {a} : {e}"), op=op)
    if op2 == 6:
        if op0:
            return dp_1src(w, pc, sf, rd, rn)
        return dp_2src(w, pc, sf, rd, rn, rm)
    if op2 & 8:
        return dp_3src(w, pc, sf, rd, rn, rm)
    raise Unallocated("dp_reg")


def dp_1src(w, pc, sf, rd, rn):
    if (w >> 29) & 1:
        raise Unallocated("1src S")
    opcode2 = (w >> 16) & 31
    opcode = (w >> 10) & 63
    bits = SZ(sf)
    a = R(rn, sf)
    if opcode2 == 1:
        # PAC*/AUT*/XPAC* : sans PAC, le pointeur est inchangé
        return Insn(pc, w, code="", op="pac")
    if opcode2:
        raise Unallocated("1src")
    if opcode == 0:
        return Insn(pc, w, code=setR(rd, sf, f"rbit{bits}({a})"), op="rbit")
    if opcode == 1:
        return Insn(pc, w, code=setR(rd, sf, f"rev16_{bits}({a})"), op="rev16")
    if opcode == 2:
        if sf:
            return Insn(pc, w, code=setX(rd, f"rev32_64({a})"), op="rev32")
        return Insn(pc, w, code=setW(rd, f"__builtin_bswap32({a})"), op="rev")
    if opcode == 3:
        if not sf:
            raise Unallocated("rev")
        return Insn(pc, w, code=setX(rd, f"__builtin_bswap64({a})"), op="rev")
    if opcode == 4:
        return Insn(pc, w, code=setR(rd, sf, f"clz{bits}({a})"), op="clz")
    if opcode == 5:
        return Insn(pc, w, code=setR(rd, sf, f"cls{bits}({a})"), op="cls")
    if opcode == 6:
        return Insn(pc, w, code=setR(rd, sf, f"ctz{bits}({a})"), op="ctz")
    if opcode == 7:
        return Insn(pc, w, code=setR(rd, sf, f"__builtin_popcountll({a})"), op="cnt")
    if opcode == 8:
        return Insn(pc, w, code=setR(rd, sf, f"({T(sf)})(({ST(sf)}){a} < 0 ? 0 - {a} : {a})"),
                    op="abs")
    raise Unallocated("1src")


def dp_2src(w, pc, sf, rd, rn, rm):
    S = (w >> 29) & 1
    opcode = (w >> 10) & 63
    bits = SZ(sf)
    a = R(rn, sf)
    b = R(rm, sf)
    if S:
        raise Unallocated("2src S")
    if opcode == 2:
        return Insn(pc, w, code=setR(rd, sf, f"udiv{bits}({a}, {b})"), op="udiv")
    if opcode == 3:
        return Insn(pc, w, code=setR(rd, sf, f"sdiv{bits}({a}, {b})"), op="sdiv")
    if 8 <= opcode <= 11:
        m = bits - 1
        if opcode == 8:
            e = f"({T(sf)})({a} << ({b} & {m}))"
        elif opcode == 9:
            e = f"({a} >> ({b} & {m}))"
        elif opcode == 10:
            e = f"({T(sf)})(({ST(sf)}){a} >> ({b} & {m}))"
        else:
            e = f"ror{bits}({a}, {b} & {m})"
        return Insn(pc, w, code=setR(rd, sf, e), op=["lslv", "lsrv", "asrv", "rorv"][opcode - 8])
    if opcode == 12:
        return Insn(pc, w, code=setX(rd, "UINT64_C(0)"), op="pacga")
    if 16 <= opcode <= 23:
        sz = opcode & 3
        c = (opcode >> 2) & 1
        if (sz == 3) != bool(sf):
            raise Unallocated("crc32")
        fn = f"crc32{'c' if c else ''}_{8 << sz}"
        return Insn(pc, w, code=setW(rd, f"{fn}({W(rn)}, {X(rm) if sz == 3 else W(rm)})"),
                    op=fn)
    if 24 <= opcode <= 27:  # CSSC smax/umax/smin/umin
        k = opcode - 24
        st = ST(sf)
        if k == 0:
            e = f"({st}){a} > ({st}){b} ? {a} : {b}"
        elif k == 1:
            e = f"{a} > {b} ? {a} : {b}"
        elif k == 2:
            e = f"({st}){a} < ({st}){b} ? {a} : {b}"
        else:
            e = f"{a} < {b} ? {a} : {b}"
        return Insn(pc, w, code=setR(rd, sf, e), op="minmax")
    raise Unallocated("2src")


def dp_3src(w, pc, sf, rd, rn, rm):
    op54 = (w >> 29) & 3
    op31 = (w >> 21) & 7
    o0 = (w >> 15) & 1
    ra = (w >> 10) & 31
    if op54:
        raise Unallocated("3src")
    t = T(sf)
    if op31 == 0:
        a, b, c = R(rn, sf), R(rm, sf), R(ra, sf)
        e = f"{c} {'-' if o0 else '+'} ({t})({a} * {b})"
        return Insn(pc, w, code=setR(rd, sf, e), op="msub" if o0 else "madd")
    if not sf:
        raise Unallocated("3src 32")
    if op31 in (1, 5):
        if op31 == 1:
            p = f"(uint64_t)((int64_t)(int32_t){X(rn)} * (int64_t)(int32_t){X(rm)})"
        else:
            p = f"((uint64_t)(uint32_t){X(rn)} * (uint64_t)(uint32_t){X(rm)})"
        e = f"{X(ra)} {'-' if o0 else '+'} {p}"
        return Insn(pc, w, code=setX(rd, e), op=("s" if op31 == 1 else "u") + ("msubl" if o0 else "maddl"))
    if op31 == 2 and not o0:
        return Insn(pc, w, code=setX(rd, f"smulh64({X(rn)}, {X(rm)})"), op="smulh")
    if op31 == 6 and not o0:
        return Insn(pc, w, code=setX(rd, f"umulh64({X(rn)}, {X(rm)})"), op="umulh")
    raise Unallocated("3src")
