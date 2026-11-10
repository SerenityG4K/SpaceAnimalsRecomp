"""Traduction FP scalaire et AdvSIMD (NEON) → C++.

Les opérations vectorielles sont émises comme des boucles sur les lanes (copies des sources
d'abord, pour gérer Vd == Vn) ; GCC les vectorise souvent. La sémantique FP fine (NaN, min/max,
conversions, FMA) passe par les helpers de runtime/simd.h.
"""
from .common import (SEQ, Insn, Unallocated, Unimplemented, W, X, hx, set_nzcv_imm, setW, setX,
                     COND)

U = {8: "u8", 16: "u16", 32: "u32", 64: "u64"}
S = {8: "s8", 16: "s16", 32: "s32", 64: "s64"}
FL = {32: "f32", 64: "f64"}
UT = {8: "uint8_t", 16: "uint16_t", 32: "uint32_t", 64: "uint64_t", 128: "unsigned __int128"}
STT = {8: "int8_t", 16: "int16_t", 32: "int32_t", 64: "int64_t", 128: "__int128"}
FT = {32: "float", 64: "double"}
VSET = {32: "v128_f32", 64: "v128_f64"}


def mk(pc, w, code, op):
    i = Insn(pc, w, SEQ, code, op=op)
    i.fp = True
    return i


def vloop(rd, srcs, n, body, keep=False, q=1):
    """Boucle sur n lanes. `srcs` : [(nom, reg)]. `keep` : le résultat part de Vd (MLA, BSL…)."""
    decl = "".join(f"const V128 {nm} = ctx->v[{r}]; " for nm, r in srcs)
    init = f"V128 R = ctx->v[{rd}];" if keep else "V128 R{};"
    tail = " R.u64[1] = 0;" if keep and not q else ""
    return f"{decl}{init} for (int i = 0; i < {n}; i++) {{ {body} }}{tail} ctx->v[{rd}] = R;"


def lanes(q, esize):
    return (128 if q else 64) // esize


def mask_of(cond_expr, esize):
    return f"({UT[esize]})(0 - ({UT[esize]})({cond_expr}))"


# =====================================================================================
# Aiguillage principal
# =====================================================================================
def simd_fp(w, pc):
    op0 = w >> 28
    op1 = (w >> 23) & 3
    op2 = (w >> 19) & 15
    op3 = (w >> 10) & 0x1FF
    if op0 & 0b0101 == 0b0001:            # x0x1 : FP scalaire / conversions
        if op1 & 2:
            return fp_3src(w, pc)
        if not op2 & 4:
            return fp_fixed_conv(w, pc)
        if op3 & 0x3F == 0:
            return fp_int_conv(w, pc)
        if op3 & 0x1F == 0x10:
            return fp_1src(w, pc)
        if op3 & 0xF == 0x8:
            return fp_cmp(w, pc)
        if op3 & 0x7 == 0x4:
            return fp_imm(w, pc)
        k = op3 & 3
        if k == 1:
            return fp_ccmp(w, pc)
        if k == 2:
            return fp_2src(w, pc)
        return fp_csel(w, pc)
    if op0 & 0b1101 == 0b0101:            # 01x1 : AdvSIMD scalaire
        return simd_scalar(w, pc, op1, op2, op3)
    if op0 & 0b1001 == 0b0000:            # 0xx0 : AdvSIMD vectoriel
        return simd_vector(w, pc, op0, op1, op2, op3)
    raise Unimplemented("crypto/sve")


# =====================================================================================
# FP scalaire
# =====================================================================================
def ftype_size(ft):
    if ft == 0:
        return 32
    if ft == 1:
        return 64
    if ft == 3:
        return 16
    raise Unallocated("ftype")


def fsrc(n, sz):
    if sz == 16:
        return f"ctx->v[{n}].u16[0]"
    return f"ctx->v[{n}].{FL[sz]}[0]"


def fset(d, sz, e):
    if sz == 16:
        return f"ctx->v[{d}] = v128_zext16({e});"
    return f"ctx->v[{d}] = {VSET[sz]}({e});"


def need_sp(sz):
    if sz == 16:
        raise Unimplemented("fp16 arith")


def fp_1src(w, pc):
    sz = ftype_size((w >> 22) & 3)
    opc = (w >> 15) & 0x3F
    rn = (w >> 5) & 31
    rd = w & 31
    if (w >> 31) or (w >> 29) & 1:
        raise Unallocated("fp1")
    a = fsrc(rn, sz)
    if opc == 0:
        return mk(pc, w, fset(rd, sz, a), "fmov")
    if opc in (1, 2):
        if sz == 16:
            e = f"(uint16_t)({a} {'& 0x7fff' if opc == 1 else '^ 0x8000'})"
        else:
            e = f"f_{'abs' if opc == 1 else 'neg'}({a})"
        return mk(pc, w, fset(rd, sz, e), "fabs" if opc == 1 else "fneg")
    if opc == 3:
        need_sp(sz)
        return mk(pc, w, fset(rd, sz, f"fp_sqrt(ctx, {a})"), "fsqrt")
    if opc in (4, 5, 7):  # FCVT
        dst = {4: 32, 5: 64, 7: 16}[opc]
        if dst == sz:
            raise Unallocated("fcvt")
        if sz == 32 and dst == 64:
            e = f"fcvt_s2d(ctx, {a})"
        elif sz == 64 and dst == 32:
            e = f"fcvt_d2s(ctx, {a})"
        elif sz == 16:
            e = f"f16_to_f{dst}({a})"
        else:
            e = f"f{sz}_to_f16(ctx, {a})"
        return mk(pc, w, fset(rd, dst, e), "fcvt")
    if 8 <= opc <= 15 and opc != 13:
        need_sp(sz)
        mode = {8: "0", 9: "1", 10: "2", 11: "3", 12: "4", 14: "fpcr_rmode(ctx)",
                15: "fpcr_rmode(ctx)"}[opc]
        return mk(pc, w, fset(rd, sz, f"fp_rint(ctx, {a}, {mode})"), "frint")
    raise Unimplemented("fp1src")


def fp_cmp(w, pc):
    sz = ftype_size((w >> 22) & 3)
    need_sp(sz)
    rm = (w >> 16) & 31
    rn = (w >> 5) & 31
    opc2 = w & 31
    if (w >> 14) & 3 or opc2 & 7:
        raise Unallocated("fcmp")
    b = f"({FT[sz]})0" if opc2 & 8 else fsrc(rm, sz)
    return mk(pc, w, f"fp_cmp(ctx, {fsrc(rn, sz)}, {b});", "fcmp")


def fp_imm(w, pc):
    sz = ftype_size((w >> 22) & 3)
    imm8 = (w >> 13) & 0xFF
    rd = w & 31
    if (w >> 5) & 31:
        raise Unallocated("fmov imm")
    bits = vfp_expand_imm(imm8, sz)
    if sz == 16:
        return mk(pc, w, f"ctx->v[{rd}] = v128_zext16(0x{bits:x});", "fmov_imm")
    return mk(pc, w, f"ctx->v[{rd}] = v128_zext{sz}({hx(bits, 64 if sz == 64 else 32)});",
              "fmov_imm")


def vfp_expand_imm(imm8, sz):
    e = {16: 5, 32: 8, 64: 11}[sz]
    f = sz - e - 1
    sign = imm8 >> 7
    b6 = (imm8 >> 6) & 1
    exp = ((1 - b6) << (e - 1)) | ((((1 << (e - 3)) - 1) if b6 else 0) << 2) | ((imm8 >> 4) & 3)
    frac = (imm8 & 15) << (f - 4)
    return (sign << (sz - 1)) | (exp << f) | frac


def fp_ccmp(w, pc):
    sz = ftype_size((w >> 22) & 3)
    need_sp(sz)
    rm = (w >> 16) & 31
    cond = (w >> 12) & 15
    rn = (w >> 5) & 31
    nzcv = w & 15
    code = (f"if ({COND[cond]}) fp_cmp(ctx, {fsrc(rn, sz)}, {fsrc(rm, sz)}); "
            f"else {{ {set_nzcv_imm(nzcv)} }}")
    return mk(pc, w, code, "fccmp")


def fp_csel(w, pc):
    sz = ftype_size((w >> 22) & 3)
    rm = (w >> 16) & 31
    cond = (w >> 12) & 15
    rn = (w >> 5) & 31
    rd = w & 31
    code = f"ctx->v[{rd}] = ({COND[cond]}) ? {zext_scalar(rn, sz)} : {zext_scalar(rm, sz)};"
    return mk(pc, w, code, "fcsel")


def zext_scalar(n, sz):
    return f"v128_zext{sz}(ctx->v[{n}].u{sz}[0])"


FP2 = {0: "fp_mul", 1: "fp_div", 2: "fp_add", 3: "fp_sub", 4: "fp_max", 5: "fp_min",
       6: "fp_maxnm", 7: "fp_minnm", 8: "fp_nmul"}


def fp_2src(w, pc):
    sz = ftype_size((w >> 22) & 3)
    need_sp(sz)
    rm = (w >> 16) & 31
    opc = (w >> 12) & 15
    rn = (w >> 5) & 31
    rd = w & 31
    if opc not in FP2:
        raise Unallocated("fp2")
    return mk(pc, w, fset(rd, sz, f"{FP2[opc]}(ctx, {fsrc(rn, sz)}, {fsrc(rm, sz)})"),
              FP2[opc][3:])


def fp_3src(w, pc):
    sz = ftype_size((w >> 22) & 3)
    need_sp(sz)
    o1 = (w >> 21) & 1
    rm = (w >> 16) & 31
    o0 = (w >> 15) & 1
    ra = (w >> 10) & 31
    rn = (w >> 5) & 31
    rd = w & 31
    a, n, m = fsrc(ra, sz), fsrc(rn, sz), fsrc(rm, sz)
    if o1:
        a = f"f_neg({a})"
    if o0 ^ o1:
        n = f"f_neg({n})"
    name = ["fmadd", "fmsub", "fnmadd", "fnmsub"][o1 * 2 + o0]
    return mk(pc, w, fset(rd, sz, f"fp_muladd(ctx, {a}, {n}, {m})"), name)


RMODE_NAMES = {0: "n", 1: "p", 2: "m", 3: "z"}


def fp_int_conv(w, pc):
    sf = w >> 31
    ft = (w >> 22) & 3
    rmode = (w >> 19) & 3
    opc = (w >> 16) & 7
    rn = (w >> 5) & 31
    rd = w & 31
    if (w >> 29) & 1:
        raise Unallocated("fcvt S")
    ibits = 64 if sf else 32
    if opc in (6, 7):  # FMOV
        if ft == 2:
            if not sf or rmode != 1:
                raise Unallocated("fmov top")
            if opc == 6:
                return mk(pc, w, setX(rd, f"ctx->v[{rn}].u64[1]"), "fmov")
            return mk(pc, w, f"ctx->v[{rd}].u64[1] = {X(rn)};", "fmov")
        if rmode:
            raise Unallocated("fmov")
        sz = ftype_size(ft)
        if sz == 16:
            if opc == 6:
                return mk(pc, w, setX(rd, f"(uint64_t)ctx->v[{rn}].u16[0]"), "fmov")
            return mk(pc, w, f"ctx->v[{rd}] = v128_zext16((uint16_t){X(rn)});", "fmov")
        if sz != ibits:
            raise Unallocated("fmov size")
        if opc == 6:
            return mk(pc, w, setX(rd, f"(uint64_t)ctx->v[{rn}].u{sz}[0]"), "fmov")
        return mk(pc, w, f"ctx->v[{rd}] = v128_zext{sz}(({UT[sz]}){X(rn)});", "fmov")
    sz = ftype_size(ft)
    need_sp(sz)
    if opc in (2, 3):  # SCVTF / UCVTF
        if rmode:
            raise Unallocated("cvtf")
        src = (f"({STT[ibits]}){X(rn)}" if opc == 2 else f"({UT[ibits]}){X(rn)}")
        return mk(pc, w, fset(rd, sz, f"({FT[sz]}){src}"), "scvtf" if opc == 2 else "ucvtf")
    if opc in (0, 1, 4, 5):
        unsigned = opc & 1
        if opc in (4, 5):
            if rmode:
                raise Unallocated("fcvta")
            mode = 4
        else:
            mode = {0: 0, 1: 1, 2: 2, 3: 3}[rmode]
        ity = (UT if unsigned else STT)[ibits]
        e = f"fcvt_int<{ity}, {FT[sz]}>({fsrc(rn, sz)}, {mode})"
        if ibits == 32:
            return mk(pc, w, setW(rd, f"(uint32_t){e}"), "fcvt" + "nzpma"[mode])
        return mk(pc, w, setX(rd, f"(uint64_t){e}"), "fcvt" + "nzpma"[mode])
    raise Unimplemented("fjcvtzs")


def fp_fixed_conv(w, pc):
    sf = w >> 31
    sz = ftype_size((w >> 22) & 3)
    need_sp(sz)
    rmode = (w >> 19) & 3
    opc = (w >> 16) & 7
    scale = (w >> 10) & 63
    rn = (w >> 5) & 31
    rd = w & 31
    fbits = 64 - scale
    ibits = 64 if sf else 32
    if not sf and scale < 32:
        raise Unallocated("fixed scale")
    if rmode == 0 and opc in (2, 3):
        src = f"({STT[ibits]}){X(rn)}" if opc == 2 else f"({UT[ibits]}){X(rn)}"
        # 2^-fbits est exactement représentable : une seule erreur d'arrondi (la conversion)
        e = f"std::ldexp(({FT[sz]}){src}, -{fbits})"
        return mk(pc, w, fset(rd, sz, e), "cvtf_fixed")
    if rmode == 3 and opc in (0, 1):
        ity = (UT if opc else STT)[ibits]
        e = f"fcvt_fixed<{ity}, {FT[sz]}>({fsrc(rn, sz)}, {fbits}, 3)"
        return mk(pc, w, (setW if ibits == 32 else setX)(rd, f"({UT[ibits]}){e}"), "fcvtz_fixed")
    raise Unallocated("fixed conv")


# =====================================================================================
# AdvSIMD vectoriel
# =====================================================================================
def simd_vector(w, pc, op0, op1, op2, op3):
    if op1 & 2:
        if op1 == 3 and op3 & 1:
            raise Unallocated("simd")
        if not op3 & 1:
            return vec_indexed(w, pc, scalar=False)
        if op2 == 0:
            return vec_modimm(w, pc)
        return vec_shift_imm(w, pc, scalar=False)
    # op1 = 0x
    if op0 & 0b1011 == 0 and not op2 & 4 and op3 & 0x23 == 0:
        return vec_table(w, pc)
    if op0 & 0b1011 == 0 and not op2 & 4 and op3 & 0x23 == 2:
        return vec_permute(w, pc)
    if op0 & 0b1011 == 0b0010 and not op2 & 4 and op3 & 0x21 == 0:
        return vec_extract(w, pc)
    if op1 == 0 and op2 & 0xC == 0 and op3 & 0x21 == 1:
        return vec_copy(w, pc)
    if op2 & 4:
        if op3 & 1:
            return vec_three_same(w, pc, scalar=False)
        if op3 & 3 == 0:
            return vec_three_diff(w, pc)
        if op2 & 7 == 0b100 and op3 & 0x183 == 2:
            return vec_misc(w, pc, scalar=False)
        if op2 & 7 == 0b110 and op3 & 0x183 == 2:
            return vec_across(w, pc)
        if op2 == 0b1111 and op3 & 0x183 == 2:
            raise Unimplemented("fp16 vector")
        raise Unallocated("simd")
    if op3 & 0x21 == 0x21:
        return vec_three_ext(w, pc)
    if op2 & 8 == 8 and op3 & 0x31 == 1:
        raise Unimplemented("fp16 three same")
    raise Unallocated("simd vec")


def vec_copy(w, pc):
    q = (w >> 30) & 1
    op = (w >> 29) & 1
    imm5 = (w >> 16) & 31
    imm4 = (w >> 11) & 15
    rn = (w >> 5) & 31
    rd = w & 31
    if imm5 & 15 == 0:
        raise Unallocated("copy")
    size = (imm5 & -imm5).bit_length() - 1   # 0..3
    es = 8 << size
    idx = imm5 >> (size + 1)
    if op:   # INS (élément)
        sidx = imm4 >> size
        return mk(pc, w, f"ctx->v[{rd}].{U[es]}[{idx}] = ctx->v[{rn}].{U[es]}[{sidx}];", "ins")
    if imm4 == 0:   # DUP (élément)
        if size == 3 and not q:
            raise Unallocated("dup")
        return mk(pc, w, vloop(rd, [("A", rn)], lanes(q, es), f"R.{U[es]}[i] = A.{U[es]}[{idx}];"),
                  "dup")
    if imm4 == 1:   # DUP (général)
        if size == 3 and not q:
            raise Unallocated("dup")
        return mk(pc, w, f"{{ {UT[es]} g = ({UT[es]}){X(rn)}; "
                         + vloop(rd, [], lanes(q, es), f"R.{U[es]}[i] = g;") + " }", "dup_gen")
    if imm4 == 3:   # INS (général)
        return mk(pc, w, f"ctx->v[{rd}].{U[es]}[{idx}] = ({UT[es]}){X(rn)};", "ins_gen")
    if imm4 == 5:   # SMOV
        if q:
            return mk(pc, w, setX(rd, f"(uint64_t)(int64_t)ctx->v[{rn}].{S[es]}[{idx}]"), "smov")
        return mk(pc, w, setW(rd, f"(uint32_t)(int32_t)ctx->v[{rn}].{S[es]}[{idx}]"), "smov")
    if imm4 == 7:   # UMOV
        return mk(pc, w, setX(rd, f"(uint64_t)ctx->v[{rn}].{U[es]}[{idx}]"), "umov")
    raise Unallocated("copy")


def adv_simd_expand_imm(op, cmode, imm8):
    c = cmode >> 1
    if c == 0:
        imm = imm8
        return imm | imm << 32
    if c == 1:
        imm = imm8 << 8
        return imm | imm << 32
    if c == 2:
        imm = imm8 << 16
        return imm | imm << 32
    if c == 3:
        imm = imm8 << 24
        return imm | imm << 32
    if c == 4:
        imm = imm8
        return imm * 0x0001000100010001
    if c == 5:
        imm = imm8 << 8
        return imm * 0x0001000100010001
    if c == 6:
        if cmode & 1 == 0:
            imm = (imm8 << 8) | 0xFF
        else:
            imm = (imm8 << 16) | 0xFFFF
        return imm | imm << 32
    # c == 7
    if cmode & 1 == 0:
        if op == 0:
            return imm8 * 0x0101010101010101
        r = 0
        for i in range(8):
            if imm8 >> i & 1:
                r |= 0xFF << (8 * i)
        return r
    if op == 0:
        s = vfp_expand_imm(imm8, 32)
        return s | s << 32
    return vfp_expand_imm(imm8, 64)


def vec_modimm(w, pc):
    q = (w >> 30) & 1
    op = (w >> 29) & 1
    cmode = (w >> 12) & 15
    o2 = (w >> 11) & 1
    imm8 = (((w >> 16) & 7) << 5) | ((w >> 5) & 31)
    rd = w & 31
    if o2:
        raise Unimplemented("fmov fp16 vector")
    if cmode == 15 and op == 1 and not q:
        raise Unallocated("fmov 2d")
    imm = adv_simd_expand_imm(op, cmode, imm8)
    is_orr_bic = (cmode < 8 and cmode & 1) or (8 <= cmode < 12 and cmode & 1)
    if is_orr_bic:
        k = f"0x{imm:x}ull"
        if op == 0:
            code = f"ctx->v[{rd}].u64[0] |= {k};" + (f" ctx->v[{rd}].u64[1] |= {k};" if q else
                                                      f" ctx->v[{rd}].u64[1] = 0;")
            return mk(pc, w, code, "orr_imm")
        code = f"ctx->v[{rd}].u64[0] &= ~{k};" + (f" ctx->v[{rd}].u64[1] &= ~{k};" if q else
                                                   f" ctx->v[{rd}].u64[1] = 0;")
        return mk(pc, w, code, "bic_imm")
    invert = op == 1 and cmode < 14
    if invert:
        imm = ~imm & ((1 << 64) - 1)
    hi = imm if q else 0
    return mk(pc, w, f"ctx->v[{rd}] = v128_u64x2(0x{imm:x}ull, 0x{hi:x}ull);",
              "mvni" if invert else "movi")


def shift_imm_info(w):
    immh = (w >> 19) & 15
    immb = (w >> 16) & 7
    if immh == 0:
        raise Unallocated("shift imm")
    size = immh.bit_length() - 1
    es = 8 << size
    v = (immh << 3) | immb
    return es, 2 * es - v, v - es   # taille, décalage droit, décalage gauche


def vec_shift_imm(w, pc, scalar):
    q = 1 if scalar else (w >> 30) & 1
    u = (w >> 29) & 1
    opc = (w >> 11) & 31
    rn = (w >> 5) & 31
    rd = w & 31
    es, rsh, lsh = shift_imm_info(w)
    if scalar and es != 64 and opc not in (0b10010, 0b10011, 0b11100, 0b11111, 0b10000,
                                           0b10001, 0b01110, 0b01100):
        raise Unallocated("scalar shift size")
    n = 1 if scalar else lanes(q, es)
    if not scalar and es == 64 and not q and opc < 0b10000:
        raise Unallocated("shift 1d")
    ut, st = UT[es], STT[es]
    a = f"A.{(U if u else S)[es]}[i]"
    if opc in (0, 2, 4, 6):  # [U|S][R]SHR, [U|S][R]SRA
        rnd = opc & 4
        acc = opc & 2
        if rnd:
            bigt = "unsigned __int128" if u else "__int128"
            e = f"({ut})((({bigt}){a} + (({bigt})1 << {rsh - 1})) >> {rsh})"
        elif rsh >= es:
            e = "0" if u else f"({ut})({a} < 0 ? -1 : 0)"
        else:
            e = f"({ut})({a} >> {rsh})"
        if acc:
            body = f"R.{U[es]}[i] = ({ut})(D.{U[es]}[i] + {e});"
            return mk(pc, w, vloop(rd, [("A", rn), ("D", rd)], n, body), ("u" if u else "s") + "sra")
        body = f"R.{U[es]}[i] = {e};"
        return mk(pc, w, vloop(rd, [("A", rn)], n, body), ("u" if u else "s") + "shr")
    if opc == 0b01010:   # SHL / SLI
        if not u:
            body = f"R.{U[es]}[i] = ({ut})(A.{U[es]}[i] << {lsh});"
            return mk(pc, w, vloop(rd, [("A", rn)], n, body), "shl")
        m = ((1 << es) - 1) >> (es - lsh) if lsh else 0
        body = (f"R.{U[es]}[i] = ({ut})((A.{U[es]}[i] << {lsh}) | (D.{U[es]}[i] & 0x{m:x}ull));")
        return mk(pc, w, vloop(rd, [("A", rn), ("D", rd)], n, body), "sli")
    if opc == 0b01000 and u:   # SRI
        if rsh >= es:
            body = f"R.{U[es]}[i] = D.{U[es]}[i];"
        else:
            m = (((1 << es) - 1) << (es - rsh)) & ((1 << es) - 1)
            body = f"R.{U[es]}[i] = ({ut})((A.{U[es]}[i] >> {rsh}) | (D.{U[es]}[i] & 0x{m:x}ull));"
        return mk(pc, w, vloop(rd, [("A", rn), ("D", rd)], n, body), "sri")
    if opc == 0b10100:   # SSHLL / USHLL (2)
        if es == 64:
            raise Unallocated("shll")
        dn = lanes(1, es * 2)
        base = dn if q else 0
        src = f"A.{(U if u else S)[es]}[i + {base}]"
        body = f"R.{U[es * 2]}[i] = ({UT[es * 2]})(({STT[es * 2] if not u else UT[es * 2]}){src} << {lsh});"
        return mk(pc, w, vloop(rd, [("A", rn)], dn, body), ("u" if u else "s") + "shll")
    if opc in (0b10000, 0b10001) and not u and not scalar:   # SHRN / RSHRN (2)
        if es == 64:
            raise Unallocated("shrn")
        # ici es est la taille de la source réduite : immh code la taille de destination
        de = es
        se = es * 2
        rsh2 = 2 * de - ((((w >> 19) & 15) << 3) | ((w >> 16) & 7))
        rnd = opc & 1
        e = f"A.{U[se]}[i]"
        if rnd:
            e = f"(uint64_t)(((unsigned __int128){e} + ((unsigned __int128)1 << {rsh2 - 1})) >> {rsh2})"
        else:
            e = f"({e} >> {rsh2})"
        return mk(pc, w, narrow_store(rd, rn, q, de, f"({UT[de]}){e}"), "shrn")
    if opc in (0b10010, 0b10011, 0b10000, 0b10001) and (u or opc & 2 or scalar):
        # SQSHRN/UQSHRN/SQRSHRN/UQRSHRN/SQSHRUN/SQRSHRUN : saturants
        de = es
        se = es * 2
        if de == 64:
            raise Unallocated("qshrn")
        rsh2 = 2 * de - ((((w >> 19) & 15) << 3) | ((w >> 16) & 7))
        rnd = opc & 1
        unsigned_src = u and opc & 2
        to_unsigned = u
        sty = UT[se] if unsigned_src else STT[se]
        big = "unsigned __int128" if unsigned_src else "__int128"
        e = f"(({big})A.{(U if unsigned_src else S)[se]}[i]"
        if rnd:
            e += f" + (({big})1 << {rsh2 - 1})"
        e += f") >> {rsh2}"
        if unsigned_src:
            sat = f"sat_narrow_u<{UT[de]}, {big}>({e})"
        elif to_unsigned:
            sat = f"sat_narrow_su<{UT[de]}, {big}>({e})"
        else:
            sat = f"({UT[de]})sat_narrow_s<{STT[de]}, {big}>({e})"
        if scalar:
            code = f"const V128 A = ctx->v[{rn}]; V128 R{{}}; {{ const int i = 0; R.{U[de]}[0] = {sat}; }} ctx->v[{rd}] = R;"
            return mk(pc, w, code, "qshrn")
        return mk(pc, w, narrow_store(rd, rn, q, de, sat), "qshrn")
    if opc == 0b11100:   # SCVTF/UCVTF (virgule fixe)
        if es not in (32, 64):
            raise Unimplemented("cvtf fixed fp16")
        fb = rsh
        src = f"A.{(U if u else S)[es]}[i]"
        body = f"R.{FL[es]}[i] = std::ldexp(({FT[es]}){src}, -{fb});"
        return mk(pc, w, vloop(rd, [("A", rn)], n, body), "cvtf_fixed")
    if opc == 0b11111:   # FCVTZS/FCVTZU (virgule fixe)
        if es not in (32, 64):
            raise Unimplemented("fcvtz fixed fp16")
        fb = rsh
        ity = (UT if u else STT)[es]
        body = f"R.{U[es]}[i] = ({UT[es]})fcvt_fixed<{ity}, {FT[es]}>(A.{FL[es]}[i], {fb}, 3);"
        return mk(pc, w, vloop(rd, [("A", rn)], n, body), "fcvtz_fixed")
    if opc in (0b01110, 0b01100):   # SQSHL/UQSHL/SQSHLU (imm)
        raise Unimplemented("qshl imm")
    raise Unallocated("shift imm")


def narrow_store(rd, rn, q, de, expr):
    """Écrit n lanes de largeur `de` dans la moitié basse (ou haute si q : variante 2)."""
    n = 64 // de
    if q:
        return (f"const V128 A = ctx->v[{rn}]; V128 R = ctx->v[{rd}]; for (int i = 0; i < {n}; i++) "
                f"{{ R.{U[de]}[i + {n}] = {expr}; }} ctx->v[{rd}] = R;")
    return (f"const V128 A = ctx->v[{rn}]; V128 R{{}}; for (int i = 0; i < {n}; i++) "
            f"{{ R.{U[de]}[i] = {expr}; }} ctx->v[{rd}] = R;")


def vec_permute(w, pc):
    q = (w >> 30) & 1
    size = (w >> 22) & 3
    rm = (w >> 16) & 31
    opc = (w >> 12) & 7
    rn = (w >> 5) & 31
    rd = w & 31
    es = 8 << size
    if size == 3 and not q:
        raise Unallocated("permute")
    n = lanes(q, es)
    h = n // 2
    f = U[es]
    part = 1 if opc & 4 else 0
    k = opc & 3
    if k == 1:   # UZP
        body = (f"int j = 2 * i + {part}; R.{f}[i] = j < {n} ? A.{f}[j] : B.{f}[j - {n}];")
        name = "uzp"
    elif k == 2:  # TRN
        body = (f"R.{f}[i] = (i & 1) ? B.{f}[(i & ~1) + {part}] : A.{f}[(i & ~1) + {part}];")
        name = "trn"
    elif k == 3:  # ZIP
        body = f"int j = (i >> 1) + {part * h}; R.{f}[i] = (i & 1) ? B.{f}[j] : A.{f}[j];"
        name = "zip"
    else:
        raise Unallocated("permute")
    return mk(pc, w, vloop(rd, [("A", rn), ("B", rm)], n, body), name + str(part + 1))


def vec_extract(w, pc):
    q = (w >> 30) & 1
    rm = (w >> 16) & 31
    imm4 = (w >> 11) & 15
    rn = (w >> 5) & 31
    rd = w & 31
    if (w >> 22) & 3:
        raise Unallocated("ext")
    if not q and imm4 & 8:
        raise Unallocated("ext")
    n = 16 if q else 8
    body = f"int j = i + {imm4}; R.u8[i] = j < {n} ? A.u8[j] : B.u8[j - {n}];"
    return mk(pc, w, vloop(rd, [("A", rn), ("B", rm)], n, body), "ext")


def vec_table(w, pc):
    q = (w >> 30) & 1
    rm = (w >> 16) & 31
    ln = ((w >> 13) & 3) + 1
    tbx = (w >> 12) & 1
    rn = (w >> 5) & 31
    rd = w & 31
    n = 16 if q else 8
    tab = " ".join(f"memcpy(&T.u8[{16 * k}], &ctx->v[{(rn + k) % 32}], 16);"
                   for k in range(ln))
    default = "D.u8[i]" if tbx else "0"
    code = (f"struct {{ uint8_t u8[64]; }} T; {tab} const V128 I = ctx->v[{rm}]; const V128 D = ctx->v[{rd}]; "
            f"V128 R{{}}; for (int i = 0; i < {n}; i++) {{ unsigned j = I.u8[i]; "
            f"R.u8[i] = j < {16 * ln} ? T.u8[j] : {default}; }} ctx->v[{rd}] = R;")
    return mk(pc, w, code, "tbx" if tbx else "tbl")


# ---------------------------------------------------------------- trois registres, même taille
def int_same_expr(u, opc, es, size):
    """Expression C de la lane i (A, B, D = sources) pour les opcodes entiers de « three same »."""
    ut, st = UT[es], STT[es]
    a, b = f"A.{U[es]}[i]", f"B.{U[es]}[i]"
    sa, sb = f"A.{S[es]}[i]", f"B.{S[es]}[i]"
    big = "unsigned __int128" if u else "__int128"
    xa, xb = (a, b) if u else (sa, sb)
    if opc == 0b00000:   # [SU]HADD
        return f"({ut})((({big}){xa} + ({big}){xb}) >> 1)", "hadd"
    if opc == 0b00010:   # [SU]RHADD
        return f"({ut})((({big}){xa} + ({big}){xb} + 1) >> 1)", "rhadd"
    if opc == 0b00100:   # [SU]HSUB
        return f"({ut})((({big}){xa} - ({big}){xb}) >> 1)", "hsub"
    if opc == 0b00001:
        return (f"sat_add_u<{ut}>({a}, {b})" if u else f"({ut})sat_add_s<{st}>({sa}, {sb})"), "qadd"
    if opc == 0b00101:
        return (f"sat_sub_u<{ut}>({a}, {b})" if u else f"({ut})sat_sub_s<{st}>({sa}, {sb})"), "qsub"
    if opc == 0b00110:
        return mask_of(f"{xa} > {xb}", es), "cmhi" if u else "cmgt"
    if opc == 0b00111:
        return mask_of(f"{xa} >= {xb}", es), "cmhs" if u else "cmge"
    if opc == 0b01000:
        return (f"vshl_u<{ut}>({a}, (int8_t)B.u{es}[i])" if u else
                f"({ut})vshl_s<{st}>({sa}, (int8_t)B.u{es}[i])"), "shl_reg"
    if opc == 0b01010:
        return (f"vrshl_u<{ut}>({a}, (int8_t)B.u{es}[i])" if u else
                f"({ut})vrshl_s<{st}>({sa}, (int8_t)B.u{es}[i])"), "rshl_reg"
    if opc == 0b01100:
        return f"({xa} > {xb}) ? {a} : {b}", "max"
    if opc == 0b01101:
        return f"({xa} < {xb}) ? {a} : {b}", "min"
    if opc == 0b01110:
        return f"({ut})({xa} > {xb} ? {xa} - {xb} : {xb} - {xa})", "abd"
    if opc == 0b01111:
        return f"({ut})(D.{U[es]}[i] + ({ut})({xa} > {xb} ? {xa} - {xb} : {xb} - {xa}))", "aba"
    if opc == 0b10000:
        return f"({ut})({a} {'-' if u else '+'} {b})", "sub" if u else "add"
    if opc == 0b10001:
        if u:
            return mask_of(f"{a} == {b}", es), "cmeq"
        return mask_of(f"({a} & {b}) != 0", es), "cmtst"
    if opc == 0b10010:
        return f"({ut})(D.{U[es]}[i] {'-' if u else '+'} ({ut})({a} * {b}))", "mls" if u else "mla"
    if opc == 0b10011:
        if u:
            raise Unimplemented("pmul")
        return f"({ut})({a} * {b})", "mul"
    if opc == 0b10110:   # SQDMULH / SQRDMULH
        if es not in (16, 32):
            raise Unallocated("sqdmulh size")
        rnd = f" + ((__int128)1 << {es - 1})" if u else ""
        e = (f"({ut})sat_narrow_s<{st}, __int128>((2 * (__int128){sa} * (__int128){sb}{rnd}) >> {es})")
        return e, "sqrdmulh" if u else "sqdmulh"
    if opc == 0b01001:   # SQSHL / UQSHL (registre)
        if u:
            return f"vqshl_u<{ut}>({a}, (int8_t)B.u{es}[i])", "uqshl_reg"
        return f"({ut})vqshl_s<{st}>({sa}, (int8_t)B.u{es}[i])", "sqshl_reg"
    raise Unimplemented(f"three same {opc:05b}")


FP_SAME = {
    # (U, a, opc) : (helper, nom)
    (0, 0, 0b11000): ("fp_maxnm", "fmaxnm"), (0, 1, 0b11000): ("fp_minnm", "fminnm"),
    (0, 0, 0b11010): ("fp_add", "fadd"), (0, 1, 0b11010): ("fp_sub", "fsub"),
    (0, 0, 0b11011): ("fp_mulx", "fmulx"), (1, 0, 0b11011): ("fp_mul", "fmul"),
    (0, 0, 0b11110): ("fp_max", "fmax"), (0, 1, 0b11110): ("fp_min", "fmin"),
    (0, 0, 0b11111): ("fp_recps", "frecps"), (0, 1, 0b11111): ("fp_rsqrts", "frsqrts"),
    (1, 0, 0b11111): ("fp_div", "fdiv"), (1, 1, 0b11010): ("fp_abd", "fabd"),
}
FP_PAIR = {(1, 0, 0b11000): "fp_maxnm", (1, 1, 0b11000): "fp_minnm", (1, 0, 0b11010): "fp_add",
           (1, 0, 0b11110): "fp_max", (1, 1, 0b11110): "fp_min"}


def vec_three_same(w, pc, scalar):
    q = 1 if scalar else (w >> 30) & 1
    u = (w >> 29) & 1
    size = (w >> 22) & 3
    rm = (w >> 16) & 31
    opc = (w >> 11) & 31
    rn = (w >> 5) & 31
    rd = w & 31
    if opc == 0b00011 and not scalar:   # logique
        nq = 2 if q else 1
        if not u:
            e = ["A.u64[i] & B.u64[i]", "A.u64[i] & ~B.u64[i]", "A.u64[i] | B.u64[i]",
                 "A.u64[i] | ~B.u64[i]"][size]
            name = ["and", "bic", "orr", "orn"][size]
            if size == 2 and rn == rm:
                name = "mov"
            return mk(pc, w, vloop(rd, [("A", rn), ("B", rm)], nq, f"R.u64[i] = {e};"), name + "_v")
        e = ["A.u64[i] ^ B.u64[i]", "(D.u64[i] & A.u64[i]) | (~D.u64[i] & B.u64[i])",
             "(D.u64[i] & ~B.u64[i]) | (A.u64[i] & B.u64[i])",
             "(D.u64[i] & B.u64[i]) | (A.u64[i] & ~B.u64[i])"][size]
        name = ["eor", "bsl", "bit", "bif"][size]
        return mk(pc, w, vloop(rd, [("A", rn), ("B", rm), ("D", rd)], nq, f"R.u64[i] = {e};"),
                  name + "_v")
    if opc >= 0b11000:   # FP
        a_bit = (size >> 1) & 1
        es = 64 if size & 1 else 32
        if not scalar and es == 64 and not q:
            raise Unallocated("fp 1d")
        n = 1 if scalar else lanes(q, es)
        f = FL[es]
        key = (u, a_bit, opc)
        if key in FP_SAME:
            h, name = FP_SAME[key]
            body = f"R.{f}[i] = {h}(ctx, A.{f}[i], B.{f}[i]);"
            return mk(pc, w, vloop(rd, [("A", rn), ("B", rm)], n, body), name)
        if key in FP_PAIR and not scalar:
            h = FP_PAIR[key]
            body = (f"{FT[es]} x = (2 * i < {n}) ? A.{f}[2 * i] : B.{f}[2 * i - {n}]; "
                    f"{FT[es]} y = (2 * i + 1 < {n}) ? A.{f}[2 * i + 1] : B.{f}[2 * i + 1 - {n}]; "
                    f"R.{f}[i] = {h}(ctx, x, y);")
            return mk(pc, w, vloop(rd, [("A", rn), ("B", rm)], n, body), h[3:] + "p")
        if opc == 0b11001 and not u:   # FMLA / FMLS
            neg = a_bit
            x = f"f_neg(A.{f}[i])" if neg else f"A.{f}[i]"
            body = f"R.{f}[i] = fp_muladd(ctx, D.{f}[i], {x}, B.{f}[i]);"
            return mk(pc, w, vloop(rd, [("A", rn), ("B", rm), ("D", rd)], n, body),
                      "fmls" if neg else "fmla")
        if opc == 0b11100:   # FCMEQ / FCMGE / FCMGT
            c = {(0, 0): "==", (1, 0): ">=", (1, 1): ">"}.get((u, a_bit))
            if c is None:
                raise Unallocated("fcm")
            body = f"R.{U[es]}[i] = {mask_of(f'A.{f}[i] {c} B.{f}[i]', es)};"
            return mk(pc, w, vloop(rd, [("A", rn), ("B", rm)], n, body), "fcm")
        if opc == 0b11101 and u:   # FACGE / FACGT
            c = ">" if a_bit else ">="
            body = (f"R.{U[es]}[i] = {mask_of(f'std::fabs(A.{f}[i]) {c} std::fabs(B.{f}[i])', es)};")
            return mk(pc, w, vloop(rd, [("A", rn), ("B", rm)], n, body), "facg")
        raise Unimplemented(f"fp three same {u}{a_bit}{opc:05b}")
    es = 8 << size
    if scalar and es != 64 and opc not in (0b00001, 0b00101, 0b10110, 0b01001):
        raise Unallocated("scalar three same")
    if not scalar and es == 64 and not q:
        raise Unallocated("1d")
    n = 1 if scalar else lanes(q, es)
    if opc in (0b10100, 0b10101, 0b10111):   # pairwise [SU]MAXP/[SU]MINP/ADDP
        if es == 64 and opc != 0b10111:
            raise Unallocated("pairwise")
        xs = U if u else S
        f = xs[es]
        if opc == 0b10111:
            if u:
                raise Unallocated("addp")
            op = lambda x, y: f"({UT[es]})({x} + {y})"  # noqa: E731
            f = U[es]
            name = "addp"
        elif opc == 0b10100:
            op = lambda x, y: f"(({x}) > ({y}) ? ({x}) : ({y}))"  # noqa: E731
            name = "maxp"
        else:
            op = lambda x, y: f"(({x}) < ({y}) ? ({x}) : ({y}))"  # noqa: E731
            name = "minp"
        body = (f"auto x = (2 * i < {n}) ? A.{f}[2 * i] : B.{f}[2 * i - {n}]; "
                f"auto y = (2 * i + 1 < {n}) ? A.{f}[2 * i + 1] : B.{f}[2 * i + 1 - {n}]; "
                f"R.{U[es]}[i] = ({UT[es]}){op('x', 'y')};")
        return mk(pc, w, vloop(rd, [("A", rn), ("B", rm)], n, body), name)
    e, name = int_same_expr(u, opc, es, size)
    body = f"R.{U[es]}[i] = {e};"
    return mk(pc, w, vloop(rd, [("A", rn), ("B", rm), ("D", rd)], n, body), name)


def vec_three_diff(w, pc):
    q = (w >> 30) & 1
    u = (w >> 29) & 1
    size = (w >> 22) & 3
    rm = (w >> 16) & 31
    opc = (w >> 12) & 15
    rn = (w >> 5) & 31
    rd = w & 31
    if size == 3:
        raise Unallocated("three diff")
    es = 8 << size
    de = es * 2
    n = 64 // es
    base = n if q else 0
    xs = U if u else S
    dt = UT[de]
    sdt = (UT if u else STT)[de]
    a = f"({sdt})A.{xs[es]}[i + {base}]"
    b = f"({sdt})B.{xs[es]}[i + {base}]"
    if opc in (0b0000, 0b0010, 0b1100, 0b1000, 0b1010, 0b0111, 0b0101):
        if opc == 0b0000:
            e, name = f"({dt})({a} + {b})", "addl"
        elif opc == 0b0010:
            e, name = f"({dt})({a} - {b})", "subl"
        elif opc == 0b1100:
            e, name = f"({dt})({a} * {b})", "mull"
        elif opc == 0b1000:
            e, name = f"({dt})(D.{U[de]}[i] + ({dt})({a} * {b}))", "mlal"
        elif opc == 0b1010:
            e, name = f"({dt})(D.{U[de]}[i] - ({dt})({a} * {b}))", "mlsl"
        elif opc == 0b0111:
            e, name = f"({dt})({a} > {b} ? {a} - {b} : {b} - {a})", "abdl"
        else:
            e, name = f"({dt})(D.{U[de]}[i] + ({dt})({a} > {b} ? {a} - {b} : {b} - {a}))", "abal"
        body = f"R.{U[de]}[i] = {e};"
        return mk(pc, w, vloop(rd, [("A", rn), ("B", rm), ("D", rd)], n, body), ("u" if u else "s") + name)
    if opc in (0b0001, 0b0011):   # [SU]ADDW / [SU]SUBW
        aw = f"({sdt})A.{(U if u else S)[de]}[i]"
        e = f"({dt})({aw} {'+' if opc == 1 else '-'} {b})"
        body = f"R.{U[de]}[i] = {e};"
        return mk(pc, w, vloop(rd, [("A", rn), ("B", rm)], n, body), "addw" if opc == 1 else "subw")
    if opc in (0b0100, 0b0110):   # ADDHN/SUBHN (+R)
        if es == 8:
            pass
        # ici es est la taille de destination (étroite), sources de largeur 2*es
        se = es * 2
        op = "+" if opc == 0b0100 else "-"
        rnd = f" + ((uint64_t)1 << {es - 1})" if u else ""
        e = f"({UT[es]})((uint64_t)(({UT[se]})(A.{U[se]}[i] {op} B.{U[se]}[i]){rnd}) >> {es})"
        nl = 64 // es
        if q:
            code = (f"const V128 A = ctx->v[{rn}], B = ctx->v[{rm}]; V128 R = ctx->v[{rd}]; "
                    f"for (int i = 0; i < {nl}; i++) {{ R.{U[es]}[i + {nl}] = {e}; }} ctx->v[{rd}] = R;")
        else:
            code = (f"const V128 A = ctx->v[{rn}], B = ctx->v[{rm}]; V128 R{{}}; "
                    f"for (int i = 0; i < {nl}; i++) {{ R.{U[es]}[i] = {e}; }} ctx->v[{rd}] = R;")
        return mk(pc, w, code, "addhn" if opc == 4 else "subhn")
    raise Unimplemented(f"three diff {opc:04b}")


def vec_three_ext(w, pc):
    q = (w >> 30) & 1
    u = (w >> 29) & 1
    size = (w >> 22) & 3
    rm = (w >> 16) & 31
    opc = (w >> 11) & 15
    rn = (w >> 5) & 31
    rd = w & 31
    if opc == 0b0010 and size == 2:   # SDOT / UDOT
        xs = "u8" if u else "s8"
        body = (f"int32_t s = 0; for (int k = 0; k < 4; k++) s += (int32_t)A.{xs}[4 * i + k] * "
                f"(int32_t)B.{xs}[4 * i + k]; R.u32[i] = D.u32[i] + (uint32_t)s;")
        return mk(pc, w, vloop(rd, [("A", rn), ("B", rm), ("D", rd)], lanes(q, 32), body),
                  "udot" if u else "sdot")
    raise Unimplemented("three ext")


# ---------------------------------------------------------------- deux registres divers
def vec_misc(w, pc, scalar):
    q = 1 if scalar else (w >> 30) & 1
    u = (w >> 29) & 1
    size = (w >> 22) & 3
    opc = (w >> 12) & 31
    rn = (w >> 5) & 31
    rd = w & 31
    es = 8 << size
    n = 1 if scalar else lanes(q, es)
    one = [("A", rn)]
    # --- FP
    if opc >= 0b01100 and opc != 0b10010 and opc != 0b10011 and opc != 0b10100 and \
            opc != 0b10110 and opc != 0b10111:
        a_bit = size >> 1
        fes = 64 if size & 1 else 32
        if not scalar and fes == 64 and not q:
            raise Unallocated("fp misc 1d")
        fn = 1 if scalar else lanes(q, fes)
        f = FL[fes]
        x = f"A.{f}[i]"
        key = (u, a_bit, opc)
        cmpz = {(0, 1, 0b01100): ">", (1, 1, 0b01100): ">=", (0, 1, 0b01101): "==",
                (1, 1, 0b01101): "<=", (0, 1, 0b01110): "<"}
        if key in cmpz:
            body = f"R.{U[fes]}[i] = {mask_of(f'{x} {cmpz[key]} ({FT[fes]})0', fes)};"
            return mk(pc, w, vloop(rd, one, fn, body), "fcmz")
        simple = {(0, 1, 0b01111): f"f_abs({x})", (1, 1, 0b01111): f"f_neg({x})",
                  (1, 1, 0b11111): f"fp_sqrt(ctx, {x})",
                  (0, 0, 0b11000): f"fp_rint(ctx, {x}, 0)", (0, 1, 0b11000): f"fp_rint(ctx, {x}, 1)",
                  (0, 0, 0b11001): f"fp_rint(ctx, {x}, 2)", (0, 1, 0b11001): f"fp_rint(ctx, {x}, 3)",
                  (1, 0, 0b11000): f"fp_rint(ctx, {x}, 4)",
                  (1, 0, 0b11001): f"fp_rint(ctx, {x}, fpcr_rmode(ctx))",
                  (1, 1, 0b11001): f"fp_rint(ctx, {x}, fpcr_rmode(ctx))",
                  (0, 1, 0b11101): f"fp_recpe(ctx, {x})", (1, 1, 0b11101): f"fp_rsqrte(ctx, {x})",
                  (0, 1, 0b11111): f"fp_recpx(ctx, {x})"}
        if key in simple:
            body = f"R.{f}[i] = {simple[key]};"
            return mk(pc, w, vloop(rd, one, fn, body), f"fmisc{opc:05b}")
        cvt = {(0, 0, 0b11010): 0, (0, 1, 0b11010): 1, (0, 0, 0b11011): 2, (0, 1, 0b11011): 3,
               (0, 0, 0b11100): 4}
        if (0, a_bit, opc) in cvt and not (u and a_bit and opc == 0b11100):
            mode = cvt[(0, a_bit, opc)]
            ity = (UT if u else STT)[fes]
            body = f"R.{U[fes]}[i] = ({UT[fes]})fcvt_int<{ity}, {FT[fes]}>({x}, {mode});"
            return mk(pc, w, vloop(rd, one, fn, body), "fcvt_v")
        if opc == 0b11101 and a_bit == 0:   # SCVTF / UCVTF
            src = f"A.{(U if u else S)[fes]}[i]"
            body = f"R.{f}[i] = ({FT[fes]}){src};"
            return mk(pc, w, vloop(rd, one, fn, body), "cvtf_v")
        raise Unimplemented(f"fp misc {u}{a_bit}{opc:05b}")
    if opc == 0b10110 and not scalar:   # FCVTN(2) / FCVTXN
        if u:
            raise Unimplemented("fcvtxn")
        if size & 1:
            e = "fcvt_d2s(ctx, A.f64[i])"
            nl, f = 2, "f32"
        else:
            e = "f64_to_f16(ctx, (double)A.f32[i])"
            nl, f = 4, "u16"
        off = nl if q else 0
        init = f"V128 R = ctx->v[{rd}];" if q else "V128 R{};"
        code = (f"const V128 A = ctx->v[{rn}]; {init} for (int i = 0; i < {nl}; i++) "
                f"{{ R.{f}[i + {off}] = {e}; }} ctx->v[{rd}] = R;")
        return mk(pc, w, code, "fcvtn")
    if opc == 0b10111 and not scalar and not u:   # FCVTL(2)
        if size & 1:
            nl = 2
            base = 2 if q else 0
            body = f"R.f64[i] = fcvt_s2d(ctx, A.f32[i + {base}]);"
        else:
            nl = 4
            base = 4 if q else 0
            body = f"R.f32[i] = f16_to_f32(A.u16[i + {base}]);"
        return mk(pc, w, vloop(rd, one, nl, body), "fcvtl")
    # --- entier
    ut, st = UT[es], STT[es]
    a, sa = f"A.{U[es]}[i]", f"A.{S[es]}[i]"
    if opc in (0b10010, 0b10100) or (opc == 0b10010 and u):   # XTN / SQXTN / UQXTN / SQXTUN
        if size == 3:
            raise Unallocated("xtn")
        de = es
        se = es * 2
        if opc == 0b10010 and not u:
            e = f"({UT[de]})A.{U[se]}[i]"
            name = "xtn"
        elif opc == 0b10010 and u:
            e = f"sat_narrow_su<{UT[de]}, {STT[se]}>(A.{S[se]}[i])"
            name = "sqxtun"
        elif u:
            e = f"sat_narrow_u<{UT[de]}, {UT[se]}>(A.{U[se]}[i])"
            name = "uqxtn"
        else:
            e = f"({UT[de]})sat_narrow_s<{STT[de]}, {STT[se]}>(A.{S[se]}[i])"
            name = "sqxtn"
        if scalar:
            code = f"const V128 A = ctx->v[{rn}]; V128 R{{}}; {{ const int i = 0; R.{U[de]}[0] = {e}; }} ctx->v[{rd}] = R;"
            return mk(pc, w, code, name)
        return mk(pc, w, narrow_store(rd, rn, q, de, e), name)
    if scalar and es != 64 and opc not in (0b00011, 0b00111):
        raise Unallocated("scalar misc size")
    if not scalar and es == 64 and not q and opc not in (0b00000,):
        raise Unallocated("1d")
    if opc == 0b00000:   # REV64 / REV32
        if u:
            if size >= 2:
                raise Unallocated("rev32")
            grp = 32 // es
        else:
            if size == 3:
                raise Unallocated("rev64")
            grp = 64 // es
        body = f"R.{U[es]}[i] = A.{U[es]}[i ^ {grp - 1}];"
        return mk(pc, w, vloop(rd, one, n, body), "rev32" if u else "rev64")
    if opc == 0b00001 and not u:   # REV16
        if size:
            raise Unallocated("rev16")
        return mk(pc, w, vloop(rd, one, n, "R.u8[i] = A.u8[i ^ 1];"), "rev16")
    if opc in (0b00010, 0b00110):   # [SU]ADDLP / [SU]ADALP
        if size == 3:
            raise Unallocated("addlp")
        de = es * 2
        dn = lanes(q, de)
        xs = U if u else S
        e = f"({UT[de]})(({(UT if u else STT)[de]})A.{xs[es]}[2 * i] + ({(UT if u else STT)[de]})A.{xs[es]}[2 * i + 1])"
        if opc == 0b00110:
            e = f"({UT[de]})(D.{U[de]}[i] + {e})"
        return mk(pc, w, vloop(rd, [("A", rn), ("D", rd)], dn, f"R.{U[de]}[i] = {e};"), "addlp")
    if opc == 0b00100:   # CLS / CLZ
        if size == 3:
            raise Unallocated("clz")
        if u:
            e = f"({ut})({a} ? __builtin_clz((uint32_t){a}) - {32 - es} : {es})"
        else:
            e = f"({ut})(cls32((uint32_t)(int32_t){sa}) - {32 - es})"
        return mk(pc, w, vloop(rd, one, n, f"R.{U[es]}[i] = {e};"), "clz" if u else "cls")
    if opc == 0b00101:   # CNT / NOT / RBIT
        if not u:
            if size:
                raise Unallocated("cnt")
            return mk(pc, w, vloop(rd, one, n, "R.u8[i] = (uint8_t)__builtin_popcount(A.u8[i]);"), "cnt")
        if size == 0:
            return mk(pc, w, vloop(rd, one, n, "R.u8[i] = (uint8_t)~A.u8[i];"), "not")
        if size == 1:
            return mk(pc, w, vloop(rd, one, lanes(q, 8), "R.u8[i] = (uint8_t)(rbit32(A.u8[i]) >> 24);"), "rbit")
        raise Unallocated("not")
    if opc == 0b01000:   # CMGT #0 / CMGE #0
        return mk(pc, w, vloop(rd, one, n, f"R.{U[es]}[i] = {mask_of(f'{sa} {'>=' if u else '>'} 0', es)};"),
                  "cmz")
    if opc == 0b01001:   # CMEQ #0 / CMLE #0
        return mk(pc, w, vloop(rd, one, n, f"R.{U[es]}[i] = {mask_of(f'{sa} {'<=' if u else '=='} 0', es)};"),
                  "cmz")
    if opc == 0b01010 and not u:   # CMLT #0
        return mk(pc, w, vloop(rd, one, n, f"R.{U[es]}[i] = {mask_of(f'{sa} < 0', es)};"), "cmz")
    if opc == 0b01011:   # ABS / NEG
        if u:
            e = f"({ut})(0 - {a})"
        else:
            e = f"({ut})({sa} < 0 ? 0 - {a} : {a})"
        return mk(pc, w, vloop(rd, one, n, f"R.{U[es]}[i] = {e};"), "neg" if u else "abs")
    if opc == 0b00111:   # SQABS / SQNEG
        raise Unimplemented("sqabs/sqneg")
    if opc == 0b10011 and u:   # SHLL(2)
        if size == 3:
            raise Unallocated("shll")
        de = es * 2
        base = lanes(1, de) if q else 0
        body = f"R.{U[de]}[i] = ({UT[de]})((({UT[de]})A.{U[es]}[i + {base}]) << {es});"
        return mk(pc, w, vloop(rd, one, lanes(1, de), body), "shll")
    if opc == 0b00011:
        raise Unimplemented("suqadd/usqadd")
    raise Unimplemented(f"misc {u}{opc:05b}")


def vec_across(w, pc):
    q = (w >> 30) & 1
    u = (w >> 29) & 1
    size = (w >> 22) & 3
    opc = (w >> 12) & 31
    rn = (w >> 5) & 31
    rd = w & 31
    if opc in (0b01100, 0b01111):   # FMAXNMV/FMINNMV/FMAXV/FMINV (simple précision, Q=1)
        if not u or size & 1 or not q:
            raise Unimplemented("fp across")
        a_bit = size >> 1
        h = {(0b01100, 0): "fp_maxnm", (0b01100, 1): "fp_minnm", (0b01111, 0): "fp_max",
             (0b01111, 1): "fp_min"}[(opc, a_bit)]
        # réduction par paires (ordre de l'ARM ARM : Reduce récursif)
        code = (f"const V128 A = ctx->v[{rn}]; float x = {h}(ctx, A.f32[0], A.f32[1]); "
                f"float y = {h}(ctx, A.f32[2], A.f32[3]); ctx->v[{rd}] = v128_f32({h}(ctx, x, y));")
        return mk(pc, w, code, h[3:] + "v")
    es = 8 << size
    if size == 3 or (size == 2 and not q):
        raise Unallocated("across")
    n = lanes(q, es)
    xs = U if u else S
    if opc == 0b00011:   # [SU]ADDLV
        de = es * 2
        acc = (UT if u else STT)[de]
        code = (f"const V128 A = ctx->v[{rn}]; {acc} s = 0; for (int i = 0; i < {n}; i++) s += A.{xs[es]}[i]; "
                f"ctx->v[{rd}] = v128_zext{de}(({UT[de]})s);")
        return mk(pc, w, code, "addlv")
    if opc == 0b11011 and not u:   # ADDV
        code = (f"const V128 A = ctx->v[{rn}]; {UT[es]} s = 0; for (int i = 0; i < {n}; i++) s += A.{U[es]}[i]; "
                f"ctx->v[{rd}] = v128_zext{es}(s);")
        return mk(pc, w, code, "addv")
    if opc in (0b01010, 0b11010):   # [SU]MAXV / [SU]MINV
        c = ">" if opc == 0b01010 else "<"
        code = (f"const V128 A = ctx->v[{rn}]; auto s = A.{xs[es]}[0]; for (int i = 1; i < {n}; i++) "
                f"if (A.{xs[es]}[i] {c} s) s = A.{xs[es]}[i]; ctx->v[{rd}] = v128_zext{es}(({UT[es]})s);")
        return mk(pc, w, code, "maxv" if c == ">" else "minv")
    raise Unimplemented("across")


# ---------------------------------------------------------------- élément indexé
def vec_indexed(w, pc, scalar):
    q = 1 if scalar else (w >> 30) & 1
    u = (w >> 29) & 1
    size = (w >> 22) & 3
    L = (w >> 21) & 1
    M = (w >> 20) & 1
    rm = (w >> 16) & 15
    opc = (w >> 12) & 15
    H = (w >> 11) & 1
    rn = (w >> 5) & 31
    rd = w & 31
    fp_ops = {0b0001: "fmla", 0b0101: "fmls", 0b1001: "fmul"}
    if opc in fp_ops and size >= 2 and not (u and opc != 0b1001):
        es = 64 if size & 1 else 32
        if es == 64:
            if L:
                raise Unallocated("idx")
            idx = H
        else:
            idx = (H << 1) | L
        rmf = (M << 4) | rm
        n = 1 if scalar else lanes(q, es)
        if not scalar and es == 64 and not q:
            raise Unallocated("idx 1d")
        f = FL[es]
        b = f"B.{f}[{idx}]"
        if opc == 0b1001:
            h = "fp_mulx" if u else "fp_mul"
            body = f"R.{f}[i] = {h}(ctx, A.{f}[i], {b});"
            return mk(pc, w, vloop(rd, [("A", rn), ("B", rmf)], n, body), "fmulx_idx" if u else "fmul_idx")
        x = f"f_neg(A.{f}[i])" if opc == 0b0101 else f"A.{f}[i]"
        body = f"R.{f}[i] = fp_muladd(ctx, D.{f}[i], {x}, {b});"
        return mk(pc, w, vloop(rd, [("A", rn), ("B", rmf), ("D", rd)], n, body), fp_ops[opc] + "_idx")
    if size not in (1, 2):
        raise Unimplemented("idx size")
    es = 8 << size
    if size == 1:
        idx = (H << 2) | (L << 1) | M
        rmi = rm
    else:
        idx = (H << 1) | L
        rmi = (M << 4) | rm
    ut = UT[es]
    if opc in (0b1100, 0b1101) and not u:   # SQDMULH / SQRDMULH (élément)
        st = STT[es]
        rnd = f" + ((__int128)1 << {es - 1})" if opc == 0b1101 else ""
        e = (f"({ut})sat_narrow_s<{st}, __int128>((2 * (__int128)A.{S[es]}[i] * "
             f"(__int128)B.{S[es]}[{idx}]{rnd}) >> {es})")
        n = 1 if scalar else lanes(q, es)
        return mk(pc, w, vloop(rd, [("A", rn), ("B", rmi)], n, f"R.{U[es]}[i] = {e};"),
                  "sqrdmulh_idx" if opc == 0b1101 else "sqdmulh_idx")
    if scalar:
        raise Unimplemented("scalar idx int")
    n = lanes(q, es)
    if opc in (0b1000, 0b0000, 0b0100):   # MUL / MLA / MLS
        if opc == 0b1000 and u:
            raise Unallocated("mul idx")
        if opc != 0b1000 and not u:
            raise Unimplemented("idx int")
        prod = f"({ut})(A.{U[es]}[i] * B.{U[es]}[{idx}])"
        if opc == 0b1000:
            e, name = prod, "mul_idx"
        elif opc == 0b0000:
            e, name = f"({ut})(D.{U[es]}[i] + {prod})", "mla_idx"
        else:
            e, name = f"({ut})(D.{U[es]}[i] - {prod})", "mls_idx"
        return mk(pc, w, vloop(rd, [("A", rn), ("B", rmi), ("D", rd)], n, f"R.{U[es]}[i] = {e};"), name)
    if opc in (0b1010, 0b0010, 0b0110):   # [SU]MULL / [SU]MLAL / [SU]MLSL (élément)
        de = es * 2
        nl = 64 // es
        base = nl if q else 0
        xs = U if u else S
        sdt = (UT if u else STT)[de]
        prod = f"({UT[de]})(({sdt})A.{xs[es]}[i + {base}] * ({sdt})B.{xs[es]}[{idx}])"
        if opc == 0b1010:
            e = prod
        elif opc == 0b0010:
            e = f"({UT[de]})(D.{U[de]}[i] + {prod})"
        else:
            e = f"({UT[de]})(D.{U[de]}[i] - {prod})"
        return mk(pc, w, vloop(rd, [("A", rn), ("B", rmi), ("D", rd)], nl, f"R.{U[de]}[i] = {e};"),
                  "mull_idx")
    if opc == 0b1110 and size == 2:   # SDOT/UDOT (élément)
        xs = "u8" if u else "s8"
        body = (f"int32_t s = 0; for (int k = 0; k < 4; k++) s += (int32_t)A.{xs}[4 * i + k] * "
                f"(int32_t)B.{xs}[4 * {idx} + k]; R.u32[i] = D.u32[i] + (uint32_t)s;")
        return mk(pc, w, vloop(rd, [("A", rn), ("B", rmi), ("D", rd)], lanes(q, 32), body), "dot_idx")
    raise Unimplemented(f"idx {u}{opc:04b}")


# =====================================================================================
# AdvSIMD scalaire
# =====================================================================================
def simd_scalar(w, pc, op1, op2, op3):
    if op1 & 2:
        if op1 == 2 and op3 & 1:
            return vec_shift_imm(w, pc, scalar=True)
        if not op3 & 1:
            return scalar_indexed(w, pc)
        raise Unallocated("scalar")
    if op1 == 0 and op2 & 0xC == 0 and op3 & 0x21 == 1:   # copie scalaire (DUP)
        imm5 = (w >> 16) & 31
        if (w >> 29) & 1 or (w >> 11) & 15:
            raise Unallocated("scalar copy")
        size = (imm5 & -imm5).bit_length() - 1
        es = 8 << size
        idx = imm5 >> (size + 1)
        rn = (w >> 5) & 31
        rd = w & 31
        return mk(pc, w, f"ctx->v[{rd}] = v128_zext{es}(ctx->v[{rn}].{U[es]}[{idx}]);", "dup_s")
    if op2 & 4:
        if op3 & 1:
            return vec_three_same(w, pc, scalar=True)
        if op2 & 7 == 0b100 and op3 & 0x183 == 2:
            return vec_misc(w, pc, scalar=True)
        if op2 & 7 == 0b110 and op3 & 0x183 == 2:
            return scalar_pairwise(w, pc)
        if op3 & 3 == 0:
            raise Unimplemented("scalar three diff")
    raise Unimplemented("scalar simd")


def scalar_pairwise(w, pc):
    u = (w >> 29) & 1
    size = (w >> 22) & 3
    opc = (w >> 12) & 31
    rn = (w >> 5) & 31
    rd = w & 31
    if not u and opc == 0b11011 and size == 3:   # ADDP d
        return mk(pc, w, f"ctx->v[{rd}] = v128_zext64(ctx->v[{rn}].u64[0] + ctx->v[{rn}].u64[1]);", "addp_s")
    if u:
        es = 64 if size & 1 else 32
        a_bit = size >> 1
        h = {(0b01100, 0): "fp_maxnm", (0b01100, 1): "fp_minnm", (0b01101, 0): "fp_add",
             (0b01111, 0): "fp_max", (0b01111, 1): "fp_min"}.get((opc, a_bit))
        if h:
            f = FL[es]
            return mk(pc, w, f"ctx->v[{rd}] = {VSET[es]}({h}(ctx, ctx->v[{rn}].{f}[0], ctx->v[{rn}].{f}[1]));",
                      h[3:] + "p_s")
    raise Unimplemented("scalar pairwise")


def scalar_indexed(w, pc):
    u = (w >> 29) & 1
    size = (w >> 22) & 3
    opc = (w >> 12) & 15
    if opc in (0b0001, 0b0101, 0b1001) and size >= 2:
        return vec_indexed(w, pc, scalar=True)
    if opc in (0b1100, 0b1101) and not u and size in (1, 2):
        return vec_indexed(w, pc, scalar=True)
    raise Unimplemented("scalar idx")


# =====================================================================================
# Chargements / rangements de structures (LD1..LD4, ST1..ST4, LDnR, lanes)
# =====================================================================================
def ldst_struct(w, pc):
    q = (w >> 30) & 1
    single = (w >> 24) & 1
    post = (w >> 23) & 1
    L = (w >> 22) & 1
    rm = (w >> 16) & 31
    rn = (w >> 5) & 31
    rt = w & 31
    base = X(rn, sp=True)
    if not single:
        if not post and rm:
            raise Unallocated("ldst multiple")
        opcode = (w >> 12) & 15
        size = (w >> 10) & 3
        conf = {0b0000: (4, 1), 0b0010: (1, 4), 0b0100: (3, 1), 0b0110: (1, 3), 0b0111: (1, 1),
                0b1000: (2, 1), 0b1010: (1, 2)}
        if opcode not in conf:
            raise Unallocated("ldst multiple opcode")
        selem, rpt = conf[opcode]
        if size == 3 and not q and selem > 1:
            raise Unallocated("ldst multiple 1d")
        es = 8 << size
        ebytes = es // 8
        nl = lanes(q, es)
        total = rpt * selem * nl * ebytes
        parts = []
        if selem == 1:
            # LD1/ST1 multiples : registres consécutifs, octets contigus
            regb = 16 if q else 8
            for r in range(rpt):
                reg = (rt + r) % 32
                if L:
                    if q:
                        parts.append(f"ctx->v[{reg}] = rd128(a + {r * 16});")
                    else:
                        parts.append(f"ctx->v[{reg}] = v128_zext64(rd64(a + {r * 8}));")
                else:
                    if q:
                        parts.append(f"wr128(a + {r * 16}, ctx->v[{reg}]);")
                    else:
                        parts.append(f"wr64(a + {r * 8}, ctx->v[{reg}].u64[0]);")
            _ = regb
        else:
            # LDn/STn entrelacés : élément e du registre r à l'adresse (e*selem + r)*ebytes
            rd_fn = {8: "rd8", 16: "rd16", 32: "rd32", 64: "rd64"}[es]
            wr_fn = {8: "wr8", 16: "wr16", 32: "wr32", 64: "wr64"}[es]
            if L:
                parts.append("V128 T[4] = {};")
                parts.append(f"for (int e = 0; e < {nl}; e++) for (int r = 0; r < {selem}; r++) "
                             f"T[r].{U[es]}[e] = {rd_fn}(a + (uint64_t)((e * {selem} + r) * {ebytes}));")
                for r in range(selem):
                    parts.append(f"ctx->v[{(rt + r) % 32}] = T[{r}];")
            else:
                regs = ", ".join(f"ctx->v[{(rt + r) % 32}]" for r in range(selem))
                parts.append(f"const V128 T[{selem}] = {{ {regs} }};")
                parts.append(f"for (int e = 0; e < {nl}; e++) for (int r = 0; r < {selem}; r++) "
                             f"{wr_fn}(a + (uint64_t)((e * {selem} + r) * {ebytes}), T[r].{U[es]}[e]);")
        name = ("ld" if L else "st") + f"{selem}"
    else:
        if not post and rm:
            raise Unallocated("ldst single")
        R_ = (w >> 21) & 1
        opcode = (w >> 13) & 7
        S_ = (w >> 12) & 1
        size = (w >> 10) & 3
        selem = (((opcode & 1) << 1) | R_) + 1
        scale = opcode >> 1
        replicate = False
        if scale == 3:
            if not L or S_:
                raise Unallocated("ldnr")
            scale = size
            replicate = True
            index = 0
        elif scale == 0:
            index = (q << 3) | (S_ << 2) | size
        elif scale == 1:
            if size & 1:
                raise Unallocated("ldst single h")
            index = (q << 2) | (S_ << 1) | (size >> 1)
        else:
            if size & 2:
                raise Unallocated("ldst single s/d")
            if size & 1 == 0:
                index = (q << 1) | S_
            else:
                if S_:
                    raise Unallocated("ldst single d")
                index = q
                scale = 3
        es = 8 << scale
        eb = es // 8
        rd_fn = {8: "rd8", 16: "rd16", 32: "rd32", 64: "rd64"}[es]
        wr_fn = {8: "wr8", 16: "wr16", 32: "wr32", 64: "wr64"}[es]
        parts = []
        total = selem * eb
        if replicate:
            nl = lanes(q, es)
            for r in range(selem):
                reg = (rt + r) % 32
                parts.append(f"{{ {UT[es]} e = {rd_fn}(a + {r * eb}); V128 R{{}}; "
                             f"for (int i = 0; i < {nl}; i++) R.{U[es]}[i] = e; ctx->v[{reg}] = R; }}")
            name = f"ld{selem}r"
        else:
            for r in range(selem):
                reg = (rt + r) % 32
                if L:
                    parts.append(f"ctx->v[{reg}].{U[es]}[{index}] = {rd_fn}(a + {r * eb});")
                else:
                    parts.append(f"{wr_fn}(a + {r * eb}, ctx->v[{reg}].{U[es]}[{index}]);")
            name = ("ld" if L else "st") + f"{selem}_lane"
    code = f"uint64_t a = {base}; " + " ".join(parts)
    if post:
        if rm == 31:
            code += " " + setX(rn, f"a + {total}", sp=True)
        else:
            code += " " + setX(rn, f"a + {X(rm)}", sp=True)
    i = Insn(pc, w, SEQ, code, op=name)
    i.mem_base = rn
    if post and rm != 31:
        i.mem_index = rm
    i.fp = True
    return i
