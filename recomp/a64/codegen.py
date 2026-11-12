"""Génération du C++ d'un module : découverte des fonctions, émission des corps, table de dispatch.

Modèle d'exécution :
  * une fonction invitée = une fonction C++ `void f_<adresse>(CPUState* ctx)` ;
  * branchements internes → `goto L_<adresse>` ; `b` vers une autre fonction → appel terminal ;
  * `bl` → x30 = retour, appel C++ direct ; `ret` → `return` (pile d'ombre en mode vérif) ;
  * `blr`/`br` → recherche dans la table de dispatch (rt_call / rt_tailcall), sauf tables de
    sauts reconnues (→ `switch` local, voir jumptable.py).
"""
import bisect
import os
import types

import capstone

from . import jumptable
from .common import B, BCOND, BL, BLR, BR, RET, SEQ, TRAP, hx
from .translate import translate

MAX_FUNC_GAP = 0x100000  # garde-fou : longueur max d'une fonction sans FDE


# Valeurs vides partagées d'une fonction pas encore analysée (ou oubliée) : 249 000 fonctions dans main, et quatre
# conteneurs par fonction coûtaient ≈ 140 Mo à chaque processus de fullbuild.py. _analyze_function les remplace.
_SANS_INSNS = ()
_SANS_ADRESSES = frozenset()
_SANS_TABLES = types.MappingProxyType({})


class Function:
    __slots__ = ("start", "end", "origin", "insns", "labels", "jumptables", "calls")

    def __init__(self, start, end, origin):
        self.start = start
        self.end = end
        self.origin = origin       # eh_frame, bl, b, ptr, export, entry
        self.oublier()

    def oublier(self):
        """Rend la mémoire de l'analyse (instructions, étiquettes, tables de sauts, appels) : elle sera refaite au besoin."""
        self.insns = _SANS_INSNS
        self.labels = _SANS_ADRESSES
        self.jumptables = _SANS_TABLES   # pc du br → liste des cibles
        self.calls = _SANS_ADRESSES      # fonctions référencées (déclarations extern)


class Recompiler:
    def __init__(self, module, comments=True, seed_pointers=True, log=print, trace=True, hooked=()):
        self.m = module
        self.base = module.base
        # fonctions « accrochées » : même les appels directs passent par le dispatch, pour qu'un
        # remplacement natif (HLE) s'applique aussi aux appels internes au module
        self.hooked = set(hooked)
        self.comments = comments
        self.trace = trace          # False : pas de TRACE(pc) (build réel, code plus compact)
        self.log = log
        self.text_end = self.base + (module.text_size or module.size)
        self.cache = {}
        self.funcs = {}
        self.stats = dict(unimpl={}, mid_entries=0, ext_targets=0, br_indirect=0, jumptables=0)
        self.md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
        for s, e in module.functions:
            self._add(self.base + s, self.base + e, "eh_frame")
        self.fde_starts = sorted(self.funcs)
        # points d'entrée connus : symboles exportés, DT_INIT/FINI, début du module
        for name, off in module.exports.items():
            a = self.base + off
            if self.base <= a < self.text_end:
                self.ensure_function(a, "export")
        for tag in (12, 13):
            for off in module.dyn.get(tag, []):
                self.ensure_function(self.base + off, "entry")
        self.ensure_function(self.base, "entry")
        if seed_pointers:
            # pointeurs relogés vers .text (vtables, tables de fonctions, init_array)
            for off, typ, sym, add in module.relocs:
                if typ == 1027:
                    self._maybe_ptr(self.base + add)
            import struct
            for off in module.relr:
                v = struct.unpack_from("<Q", module.img, off)[0]
                self._maybe_ptr(self.base + v)

    # ------------------------------------------------------------------ fonctions
    def _add(self, start, end, origin):
        if start not in self.funcs:
            self.funcs[start] = Function(start, end, origin)
        return self.funcs[start]

    def _maybe_ptr(self, a):
        if self.base <= a < self.text_end and a % 4 == 0:
            self.ensure_function(a, "ptr")

    def containing(self, a):
        """Fonction eh_frame contenant `a` (ou None)."""
        i = bisect.bisect_right(self.fde_starts, a) - 1
        if i >= 0:
            f = self.funcs[self.fde_starts[i]]
            if f.start <= a < f.end:
                return f
        return None

    def ensure_function(self, a, origin):
        """Garantit qu'il existe une fonction commençant en `a` ; renvoie-la (ou None si hors
        .text)."""
        if a in self.funcs:
            return self.funcs[a]
        if not (self.base <= a < self.text_end) or a % 4:
            return None
        f = self.containing(a)
        if f is not None:
            # entrée au milieu d'une fonction : on génère une copie du suffixe
            self.stats["mid_entries"] += 1
            return self._add(a, f.end, origin + "-mid")
        i = bisect.bisect_right(self.fde_starts, a)
        limit = self.fde_starts[i] if i < len(self.fde_starts) else self.text_end
        limit = min(limit, a + MAX_FUNC_GAP)
        return self._add(a, self._extent(a, limit), origin)

    def _extent(self, a, limit):
        """Fin d'une fonction sans FDE : plus grande adresse atteignable depuis `a` (sans
        dépasser `limit`) en suivant le flot local."""
        seen = set()
        todo = [a]
        hi = a
        while todo:
            pc = todo.pop()
            while a <= pc < limit and pc not in seen:
                seen.add(pc)
                hi = max(hi, pc)
                i = self.insn(pc)
                if i.kind == BCOND and a <= i.target < limit:
                    todo.append(i.target)
                if i.kind == B:
                    if a <= i.target < limit:
                        todo.append(i.target)
                    break
                if i.kind in (BR, RET, TRAP):
                    break
                pc += 4
        return hi + 4

    def insn(self, pc):
        i = self.cache.get(pc)
        if i is None:
            w = self.m.word(pc - self.base)
            i = translate(w, pc)
            self.cache[pc] = i
        return i

    def analyze(self, only=None):
        """Décode toutes les fonctions (et celles découvertes en route)."""
        todo = list(self.funcs) if only is None else list(only)
        done = set()
        while todo:
            a = todo.pop()
            if a in done:
                continue
            done.add(a)
            f = self.funcs[a]
            self._analyze_function(f)
            for c in f.calls:
                if c in self.funcs and c not in done and only is None:
                    todo.append(c)
        return done

    def _analyze_function(self, f):
        f.insns = []
        f.labels = set()
        f.jumptables = {}
        f.calls = set()
        pc = f.start
        while pc < f.end:
            i = self.insn(pc)
            f.insns.append(i)
            if i.unimpl:
                self.stats["unimpl"][i.op] = self.stats["unimpl"].get(i.op, 0) + 1
            if i.kind in (B, BCOND):
                t = i.target
                if f.start <= t < f.end:
                    f.labels.add(t)
                else:
                    g = self.ensure_function(t, "b")
                    if g:
                        f.calls.add(t)
                    else:
                        self.stats["ext_targets"] += 1
            elif i.kind == BL:
                g = self.ensure_function(i.target, "bl")
                if g:
                    f.calls.add(i.target)
                else:
                    self.stats["ext_targets"] += 1
            elif i.kind == BR:
                tab = jumptable.recover(self, f, len(f.insns) - 1)
                if tab:
                    f.jumptables[pc] = tab
                    self.stats["jumptables"] += 1
                    for t in tab:
                        if f.start <= t < f.end:
                            f.labels.add(t)
                        elif t in self.funcs:
                            f.calls.add(t)      # cas d'une table vers une autre fonction
                else:
                    self.stats["br_indirect"] += 1
            pc += 4
        if f.end in self.funcs and self._falls_through(f):
            f.calls.add(f.end)

    @staticmethod
    def _falls_through(f):
        return f.insns and f.insns[-1].kind not in (B, BR, RET, TRAP)

    # ------------------------------------------------------------------ émission
    def fname(self, a):
        return f"f_{a:x}"

    def _goto(self, f, t):
        if f.start <= t < f.end:
            return f"goto L_{t:x};"
        if t in self.funcs and t not in self.hooked:
            return f"{{ MUSTTAIL return {self.fname(t)}(ctx); }}"
        return f"{{ MUSTTAIL return rt_tailcall(ctx, {hx(t)}); }}"

    def emit_function(self, f):
        out = []
        src = bytes(self.m.img[f.start - self.base:f.end - self.base])
        dis = {}
        if self.comments:
            for a, s, mn, op in self.md.disasm_lite(src, f.start):
                dis[a] = f"{mn} {op}".strip()
        out.append(f"// {f.origin} 0x{f.start:x}-0x{f.end:x}")
        out.append(f"void {self.fname(f.start)}(CPUState* ctx) {{")
        for i in f.insns:
            pc = i.pc
            if pc in f.labels:
                out.append(f"L_{pc:x}:")
            c = dis.get(pc, f".word 0x{i.word:08x}")
            line = f"    TRACE({hx(pc)});" if self.trace else ""
            body = self._emit_insn(f, i)
            if self.comments:
                out.append(f"{line} /* {pc:x}: {c} */")
            elif line:
                out.append(line)
            if body:
                out.append("    " + body)
        if self._falls_through(f):
            nxt = self.insn(f.end) if f.end < self.text_end and f.end not in self.funcs else None
            if nxt is not None and nxt.kind == TRAP:
                # bourrage après un appel sans retour : exécuter le trap comme le matériel
                tr = f"TRACE({hx(f.end)}); " if self.trace else ""
                out.append(f"    {tr}{nxt.code} return;")
            else:
                out.append("    " + self._goto(f, f.end))
        out.append("}")
        return "\n".join(out)

    def _emit_insn(self, f, i):
        k = i.kind
        if k == SEQ:
            return f"{{ {i.code} }}" if i.code else ""
        if k == B:
            return self._goto(f, i.target)
        if k == BCOND:
            return f"if ({i.cond}) {self._goto(f, i.target)}"
        ret = hx(i.pc + 4)
        if k == BL:
            t = i.target
            direct = t in self.funcs and t not in self.hooked
            call = f"{self.fname(t)}(ctx);" if direct else f"rt_call(ctx, {hx(t)});"
            return f"ctx->x[30] = {ret}; PUSH_RET({ret}); {call}"
        if k == BLR:
            return f"{{ uint64_t t = {i.reg}; ctx->x[30] = {ret}; PUSH_RET({ret}); rt_call(ctx, t); }}"
        if k == BR:
            tab = f.jumptables.get(i.pc)
            if tab:
                cases = " ".join(f"case {hx(t)}: {self._goto(f, t)}" for t in sorted(set(tab)))
                return (f"{{ uint64_t t = {i.reg}; switch (t) {{ {cases} "
                        f"default: MUSTTAIL return rt_tailcall(ctx, t); }} }}")
            return f"{{ MUSTTAIL return rt_tailcall(ctx, {i.reg}); }}"
        if k == RET:
            return f"CHECK_RET({i.reg}, {hx(i.pc)}); return;"
        if k == TRAP:
            return f"{i.code} return;"
        raise ValueError(k)

    def write(self, outdir, funcs=None, per_file=1500, prefix=None, dispatch=True):
        """Écrit les .cpp. `funcs` : adresses à émettre (défaut : toutes)."""
        os.makedirs(outdir, exist_ok=True)
        prefix = prefix or self.m.name
        addrs = sorted(self.funcs if funcs is None else funcs)
        files = []
        for n in range(0, len(addrs), per_file):
            chunk = addrs[n:n + per_file]
            mine = set(chunk)
            ext = set()
            bodies = []
            for a in chunk:
                f = self.funcs[a]
                if not f.insns:
                    self._analyze_function(f)
                bodies.append(self.emit_function(f))
                ext |= {c for c in f.calls if c not in mine and c in self.funcs}
            path = os.path.join(outdir, f"{prefix}_{n // per_file:04d}.cpp")
            with open(path, "w", newline="\n", encoding="utf-8") as fp:
                fp.write(f"// Généré par recomp/recomp.py depuis le module {self.m.name} "
                         f"(base 0x{self.base:x}). Dérivé du jeu : ne pas diffuser.\n")
                fp.write('#include "cpu.h"\n\n')
                for a in sorted(mine):
                    fp.write(f"void {self.fname(a)}(CPUState* ctx);\n")
                for a in sorted(ext):
                    fp.write(f"void {self.fname(a)}(CPUState* ctx);\n")
                fp.write("\n")
                fp.write("\n\n".join(bodies))
                fp.write("\n")
            files.append(path)
        if dispatch:
            files.append(self._write_dispatch(outdir, addrs, prefix))
        return files

    def write_stream(self, outdir, per_file=1500, prefix=None, log=print):
        """Génération complète à mémoire bornée : chaque lot est analysé, émis puis oublié ;
        les fonctions découvertes en route (entrées en milieu de fonction, stubs) sont émises
        dans des lots supplémentaires. Une seule table de dispatch à la fin."""
        prefix = prefix or self.m.name
        emitted, done, files, n = [], set(), [], 0
        while True:
            todo = sorted(a for a in self.funcs if a not in done)
            if not todo:
                break
            for k in range(0, len(todo), per_file):
                chunk = todo[k:k + per_file]
                files += self.write(outdir, chunk, per_file, prefix=f"{prefix}_{n:05d}",
                                    dispatch=False)
                n += 1
                done.update(chunk)
                emitted += chunk
                for a in chunk:
                    self.funcs[a].insns = []
                self.cache.clear()
                if n % 20 == 0:
                    log(f"  {len(done)}/{len(self.funcs)} fonctions émises")
        files.append(self._write_dispatch(outdir, sorted(emitted), prefix))
        return files

    def _write_dispatch(self, outdir, addrs, prefix):
        path = os.path.join(outdir, f"{prefix}_dispatch.cpp")
        with open(path, "w", newline="\n", encoding="utf-8") as fp:
            fp.write(f"// Table de dispatch du module {self.m.name}. Dérivé du jeu : ne pas diffuser.\n")
            fp.write('#include "cpu.h"\n\n')
            for a in addrs:
                fp.write(f"void {self.fname(a)}(CPUState* ctx);\n")
            fp.write(f"\nstatic const uint64_t k_addrs[] = {{\n")
            for a in addrs:
                fp.write(f"    {hx(a)},\n")
            fp.write("};\nstatic const RecompFn k_fns[] = {\n")
            for a in addrs:
                fp.write(f"    {self.fname(a)},\n")
            fp.write("};\n\n")
            fp.write(f"void recomp_register_{prefix}() {{\n"
                     f"    rt_register_functions(k_addrs, k_fns, {len(addrs)});\n}}\n")
            fp.write(f"static int s_registered = (recomp_register_{prefix}(), 0);\n")
        return path
