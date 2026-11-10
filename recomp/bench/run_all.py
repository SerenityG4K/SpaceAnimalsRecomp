#!/usr/bin/env python3
"""Suite de validation complète (≈ 15 min). Écrit bench/results/validation.log.

  python bench/run_all.py [--quick] [--from N]   (--from : reprend à l'étape N, journal en ajout)
"""
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
H = [sys.executable, os.path.join(HERE, "harness.py")]
RARE = ("at_,cas,ld1,st1,ld2,st2,ld3,st3,ld4,st4,tbl,tbx,fmisc,fcvt,dot,smull,umull,mull,smlal,"
        "umlal,saddl,uaddl,ssubl,usubl,addlp,addlv,maxv,minv,uzp,trn,zip,ext,ins,dup,umov,smov,sri,"
        "sli,ushr,sshr,usra,ssra,shrn,qshrn,sshll,ushll,shll,xtn,sqxtn,uqxtn,sqxtun,cnt,rev,crc32,"
        "ldxr,stxr,ldapr,ldapur,ldar,stlr,fcm,facg,cmz,abs,neg,clz,cls,addhn,subhn,addw,subw,abd,"
        "aba,hadd,rhadd,qadd,qsub,shl_reg,rshl_reg,mla,mls,maxp,minp,addp,fmulx,minnmv,maxnmv,"
        "frecps,frsqrts,bsl,bit,bif,not,rbit,cvtf,fcvtz,movi,mvni,orr_imm,bic_imm,fmov_imm,cmeq,"
        "cmtst,cmhi,cmhs,cmgt,cmge,max,min,mul,sdot,udot")

SUITE = [
    ("func rtld", ["func", "rtld", "--trials", "16"]),
    ("func subsdk3", ["func", "subsdk3", "--trials", "16"]),
    ("fuzz rtld", ["fuzz", "rtld", "--trials", "16"]),
    ("fuzz subsdk3", ["fuzz", "subsdk3", "--trials", "16"]),
    ("fuzz main FP/SIMD", ["fuzz", "main", "--sample", "20000", "--fp-only", "--trials", "8"]),
    ("fuzz main entiers", ["fuzz", "main", "--sample", "6000", "--limit", "40000", "--trials", "6"]),
    ("fuzz main familles rares", ["fuzz", "main", "--op", RARE, "--limit", "25000", "--trials", "8"]),
    ("fuzz main saturants (sqdmulh/sqrdmulh/sqshl)",
     ["fuzz", "main", "--op", "sqdmulh,sqrdmulh,sqshl_reg,uqshl_reg", "--trials", "32"]),
    ("func main", ["func", "main", "--sample", "600", "--max-closure", "60", "--trials", "8"]),
]


def main():
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8")
    quick = "--quick" in sys.argv
    start = int(sys.argv[sys.argv.index("--from") + 1]) if "--from" in sys.argv else 0
    os.makedirs(os.path.join(HERE, "results"), exist_ok=True)
    log = open(os.path.join(HERE, "results", "validation.log"), "a" if start else "w",
               encoding="utf-8")
    steps = SUITE[:4] if quick else SUITE
    for name, args in steps[start:]:
        t0 = time.time()
        r = subprocess.run(H + args, capture_output=True, text=True, encoding="utf-8")
        out = (r.stdout + r.stderr).strip().splitlines()
        keep = [l for l in out if not l.startswith("  détails")]
        block = f"=== {name} (code {r.returncode}, {time.time() - t0:.0f} s)\n" + "\n".join(keep[-14:])
        print(block, flush=True)
        log.write(block + "\n\n")
        log.flush()


if __name__ == "__main__":
    main()
