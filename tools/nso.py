#!/usr/bin/env python3
"""Analyse et décompression des modules NSO (exécutables Horizon / Switch).

  python nso.py info  <fichier.nso> [...]
  python nso.py unpack <fichier.nso> <sortie.bin>   # image mémoire à plat (text|ro|data|bss)
"""
import hashlib
import struct
import sys

import lz4.block


def parse(path):
    d = open(path, "rb").read()
    if d[:4] != b"NSO0":
        raise ValueError(f"{path}: pas un NSO")
    flags = struct.unpack_from("<I", d, 0x0C)[0]
    segs = []
    for i, name in enumerate(("text", "ro", "data")):
        foff, moff, size, extra = struct.unpack_from("<IIII", d, 0x10 + i * 0x10)
        csize = struct.unpack_from("<I", d, 0x60 + i * 4)[0]
        sha = d[0xA0 + i * 0x20:0xC0 + i * 0x20]
        segs.append(dict(name=name, file_off=foff, mem_off=moff, size=size, extra=extra,
                         csize=csize, compressed=bool(flags >> i & 1),
                         check=bool(flags >> (i + 3) & 1), sha256=sha))
    module_id = d[0x40:0x60]
    api_off, api_size, dynstr_off, dynstr_size, dynsym_off, dynsym_size = struct.unpack_from("<6I", d, 0x88)
    return dict(raw=d, flags=flags, segs=segs, module_id=module_id, bss=segs[2]["extra"],
                api=(api_off, api_size), dynstr=(dynstr_off, dynstr_size), dynsym=(dynsym_off, dynsym_size))


def segment_data(n, s):
    blob = n["raw"][s["file_off"]:s["file_off"] + (s["csize"] if s["compressed"] else s["size"])]
    if s["compressed"]:
        blob = lz4.block.decompress(blob, uncompressed_size=s["size"])
    if len(blob) != s["size"]:
        raise ValueError(f"segment {s['name']} : taille {len(blob)} != {s['size']}")
    return blob


def image(n):
    segs = n["segs"]
    end = segs[2]["mem_off"] + segs[2]["size"] + n["bss"]
    img = bytearray(end)
    hashes_ok = True
    for s in segs:
        b = segment_data(n, s)
        if s["check"] and hashlib.sha256(b).digest() != s["sha256"]:
            hashes_ok = False
        img[s["mem_off"]:s["mem_off"] + len(b)] = b
    return img, hashes_ok


def info(path):
    n = parse(path)
    img, ok = image(n)
    print(f"== {path}")
    print(f"   module id : {n['module_id'][:20].hex()}")
    for s in n["segs"]:
        print(f"   .{s['name']:<5} mem 0x{s['mem_off']:08x}  taille 0x{s['size']:08x} ({s['size'] / 1048576:7.2f} Mo)"
              f"  lz4={s['compressed']}  hash={s['check']}")
    print(f"   .bss   taille 0x{n['bss']:08x}   hashes segments : {'OK' if ok else 'ÉCHEC'}")
    # MOD0 : pointeur à text+4
    mod0 = struct.unpack_from("<I", img, 4)[0]
    if img[mod0:mod0 + 4] == b"MOD0":
        dyn, bss_s, bss_e, eh_s, eh_e, modobj = struct.unpack_from("<6i", img, mod0 + 4)
        print(f"   MOD0 @0x{mod0:x} : .dynamic 0x{mod0 + dyn:x}, eh_frame_hdr 0x{mod0 + eh_s:x}-0x{mod0 + eh_e:x}")
    ro = n["segs"][1]["mem_off"]
    so, ss = n["dynsym"]
    st, sts = n["dynstr"]
    nsym = ss // 24
    strtab = bytes(img[ro + st:ro + st + sts])
    defined = imported = 0
    for i in range(nsym):
        name_off, info_b, other, shndx, value, size = struct.unpack_from("<IBBHQQ", img, ro + so + i * 24)
        if name_off == 0:
            continue
        if shndx == 0:
            imported += 1
        else:
            defined += 1
    print(f"   .dynsym : {nsym} symboles ({defined} définis, {imported} importés)")
    # nom du module (chaîne en tête du segment ro)
    name_len = struct.unpack_from("<I", img, ro + 4)[0] if struct.unpack_from("<I", img, ro)[0] == 0 else 0
    if 0 < name_len < 0x200:
        print(f"   chemin de build : {bytes(img[ro + 8:ro + 8 + name_len]).split(b'\0')[0].decode(errors='replace')}")
    return n, img


def main():
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8")
    if len(sys.argv) >= 3 and sys.argv[1] == "info":
        for p in sys.argv[2:]:
            info(p)
    elif len(sys.argv) == 4 and sys.argv[1] == "unpack":
        img, ok = image(parse(sys.argv[2]))
        open(sys.argv[3], "wb").write(img)
        print(f"{sys.argv[3]} : {len(img)} octets, hashes {'OK' if ok else 'ÉCHEC'}")
    else:
        print(__doc__)
        return 1


if __name__ == "__main__":
    sys.exit(main())
