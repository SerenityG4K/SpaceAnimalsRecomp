"""Infrastructure commune du banc différentiel : mémoire invitée à adresse fixe dans le processus
Python (pour la DLL recompilée), état CPU partagé (ctypes), émulateur de référence Unicorn."""
import ctypes
import os
import random
import struct
import sys

from unicorn import (UC_ARCH_ARM64, UC_HOOK_CODE, UC_HOOK_INTR, UC_HOOK_MEM_WRITE, UC_MODE_ARM,
                     UC_PROT_ALL, Uc, UcError)
import unicorn.arm64_const as C

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, ".."))
sys.path.insert(0, ROOT)

from a64.module import IMPORT_THUNK_BASE  # noqa: E402

# Régions fixes du banc (identiques côté Unicorn et côté DLL)
STACK_BASE, STACK_SIZE = 0x0000007E00000000, 0x100000
SCRATCH_BASE, SCRATCH_SIZE = 0x0000007E10000000, 0x100000
TLS_BASE, TLS_SIZE = 0x0000007E20000000, 0x10000
THUNK_SIZE = 0x10000
SENTINEL = 0x0000007FFFFF0000        # adresse de retour finale (non mappée)

STATE_SIZE = 864
TRACE_SIZE = 784
WRITE_SIZE = 88
OFF_SP, OFF_PC, OFF_FLAGS, OFF_FPCR, OFF_TPIDR, OFF_V = 248, 256, 264, 280, 288, 352

XREGS = [getattr(C, f"UC_ARM64_REG_X{i}") for i in range(29)] + [C.UC_ARM64_REG_X29,
                                                                C.UC_ARM64_REG_X30]
VREGS = [getattr(C, f"UC_ARM64_REG_Q{i}") for i in range(32)]
TRACE_REGS = XREGS + [C.UC_ARM64_REG_SP, C.UC_ARM64_REG_NZCV]
REG_NAMES = [f"x{i}" for i in range(31)] + ["sp", "nzcv"] + [f"v{i}" for i in range(32)]

MEM_COMMIT_RESERVE = 0x3000
PAGE_READWRITE = 0x04
k32 = ctypes.WinDLL("kernel32", use_last_error=True)
k32.VirtualAlloc.restype = ctypes.c_void_p
k32.VirtualAlloc.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_uint32, ctypes.c_uint32]


def round_up(x, a):
    return (x + a - 1) & ~(a - 1)


class HostMem:
    """Régions allouées à leur adresse invitée dans ce processus (adresse invité == hôte)."""

    def __init__(self):
        self.regions = []

    def map(self, addr, size):
        size = round_up(size, 0x10000)
        p = k32.VirtualAlloc(ctypes.c_void_p(addr), size, MEM_COMMIT_RESERVE, PAGE_READWRITE)
        if p != addr:
            raise OSError(f"VirtualAlloc 0x{addr:x} impossible (erreur {ctypes.get_last_error()})")
        self.regions.append((addr, size))

    def write(self, addr, data):
        ctypes.memmove(addr, bytes(data), len(data))

    def read(self, addr, n):
        return ctypes.string_at(addr, n)


class State:
    """État CPU au format runtime/cpu.h (tampon aligné)."""

    def __init__(self):
        self.x = [0] * 31
        self.sp = 0
        self.nzcv = 0
        self.fpcr = 0
        self.tpidr_el0 = 0
        self.tpidrro_el0 = 0
        self.v = [0] * 32

    def pack(self):
        b = bytearray(STATE_SIZE)
        struct.pack_into("<31Q", b, 0, *self.x)
        struct.pack_into("<Q", b, OFF_SP, self.sp)
        n = self.nzcv
        struct.pack_into("<4I", b, OFF_FLAGS, (n >> 31) & 1, (n >> 30) & 1, (n >> 29) & 1, (n >> 28) & 1)
        struct.pack_into("<II", b, OFF_FPCR, self.fpcr, 0)
        struct.pack_into("<QQ", b, OFF_TPIDR, self.tpidr_el0, self.tpidrro_el0)
        for i, v in enumerate(self.v):
            b[OFF_V + 16 * i:OFF_V + 16 * i + 16] = v.to_bytes(16, "little")
        return bytes(b)

    @classmethod
    def unpack(cls, b):
        s = cls()
        s.x = list(struct.unpack_from("<31Q", b, 0))
        s.sp = struct.unpack_from("<Q", b, OFF_SP)[0]
        n, z, c, v = struct.unpack_from("<4I", b, OFF_FLAGS)
        s.nzcv = (n << 31) | (z << 30) | (c << 29) | (v << 28)
        s.fpcr = struct.unpack_from("<I", b, OFF_FPCR)[0]
        s.tpidr_el0, s.tpidrro_el0 = struct.unpack_from("<QQ", b, OFF_TPIDR)
        s.v = [int.from_bytes(b[OFF_V + 16 * i:OFF_V + 16 * i + 16], "little") for i in range(32)]
        return s

    def regs(self):
        return self.x + [self.sp, self.nzcv] + self.v


class RecompDll:
    """DLL du code recompilé (compilée avec RECOMP_BENCH)."""

    def __init__(self, path):
        self.dll = ctypes.CDLL(path)
        d = self.dll
        d.recomp_call.restype = ctypes.c_int
        d.recomp_call.argtypes = [ctypes.c_void_p, ctypes.c_uint64]
        d.recomp_trace_config.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_uint64]
        for f in ("recomp_trace_count", "recomp_writes_count", "recomp_steps", "recomp_function_count"):
            getattr(d, f).restype = ctypes.c_uint64
        for f in ("recomp_trace_data", "recomp_writes_data"):
            getattr(d, f).restype = ctypes.c_void_p
        d.recomp_last_error.restype = ctypes.c_char_p
        d.recomp_register_stub.argtypes = [ctypes.c_uint64]
        d.recomp_has_function.argtypes = [ctypes.c_uint64]
        assert d.recomp_state_size() == STATE_SIZE, "disposition CPUState différente"
        assert d.recomp_trace_entry_size() == TRACE_SIZE
        assert d.recomp_write_entry_size() == WRITE_SIZE
        # état dans une page dédiée (alignement 16 requis par V128)
        self.state_addr = k32.VirtualAlloc(None, 0x1000, MEM_COMMIT_RESERVE, PAGE_READWRITE)

    def call(self, addr, state, trace=True, with_v=True, max_steps=100000):
        """Exécute la fonction ; renvoie (statut, message, état final, trace, écritures)."""
        self.dll.recomp_trace_config(1 if trace else 0, 1 if with_v else 0, max_steps)
        ctypes.memmove(self.state_addr, state.pack(), STATE_SIZE)
        try:
            st = self.dll.recomp_call(self.state_addr, addr)
            msg = self.dll.recomp_last_error().decode("utf-8", "replace")
        except OSError as e:   # violation d'accès attrapée par ctypes (SEH)
            st, msg = "fault", str(e)
        out = State.unpack(ctypes.string_at(self.state_addr, STATE_SIZE))
        n = self.dll.recomp_trace_count()
        tr = ctypes.string_at(self.dll.recomp_trace_data(), n * TRACE_SIZE) if n else b""
        nw = self.dll.recomp_writes_count()
        wr = ctypes.string_at(self.dll.recomp_writes_data(), nw * WRITE_SIZE) if nw else b""
        return st, msg, out, tr, wr


class RefEmu:
    """Émulateur de référence Unicorn (CPU « max » : LSE, RCpc… ; PAC désactivé par SCTLR)."""

    def __init__(self, thunks=()):
        uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        uc.ctl_set_cpu_model(C.UC_CPU_ARM64_MAX)
        self.uc = uc
        self.regions = []
        self.thunk_lo = IMPORT_THUNK_BASE
        self.thunk_hi = IMPORT_THUNK_BASE + THUNK_SIZE
        uc.mem_map(IMPORT_THUNK_BASE, THUNK_SIZE, UC_PROT_ALL)
        uc.mem_write(IMPORT_THUNK_BASE, struct.pack("<I", 0xD65F03C0) * (THUNK_SIZE // 4))  # ret
        self.trace = []
        self.writes = []
        self.steps = 0
        self.max_steps = 100000
        self.tracing = True
        self.with_v = True
        self.stop_reason = None
        uc.hook_add(UC_HOOK_CODE, self._on_code)
        uc.hook_add(UC_HOOK_MEM_WRITE, self._on_write)
        uc.hook_add(UC_HOOK_INTR, self._on_intr)

    def map(self, addr, size):
        size = round_up(size, 0x10000)
        self.uc.mem_map(addr, size, UC_PROT_ALL)
        self.regions.append((addr, size))

    def _on_code(self, uc, addr, size, _):
        if self.thunk_lo <= addr < self.thunk_hi:
            uc.reg_write(C.UC_ARM64_REG_X0, 0)   # stub d'import : renvoie 0
            return
        self.steps += 1
        if self.steps > self.max_steps:
            self.stop_reason = "limit"
            uc.emu_stop()
            return
        if self.tracing:
            r = uc.reg_read_batch(TRACE_REGS)
            e = struct.pack("<Q31QQII", addr, *r[:31], r[31], r[32], 0)
            if self.with_v:
                e += b"".join(v.to_bytes(16, "little") for v in uc.reg_read_batch(VREGS))
            else:
                e += bytes(512)
            self.trace.append(e)

    def _on_write(self, uc, access, addr, size, value, _):
        if self.tracing:
            self.writes.append((self.steps - 1, addr, size, value))

    def _on_intr(self, uc, intno, _):
        if intno == 2:  # svc
            uc.reg_write(C.UC_ARM64_REG_X0, 0)
            uc.reg_write(C.UC_ARM64_REG_X1, 0)
        else:
            self.stop_reason = f"exception {intno}"
            uc.emu_stop()

    def set_state(self, s):
        uc = self.uc
        uc.reg_write_batch(list(zip(XREGS, s.x)))
        uc.reg_write(C.UC_ARM64_REG_SP, s.sp)
        uc.reg_write(C.UC_ARM64_REG_NZCV, s.nzcv)
        uc.reg_write(C.UC_ARM64_REG_FPCR, s.fpcr)
        uc.reg_write(C.UC_ARM64_REG_FPSR, 0)
        uc.reg_write(C.UC_ARM64_REG_TPIDR_EL0, s.tpidr_el0)
        uc.reg_write(C.UC_ARM64_REG_TPIDRRO_EL0, s.tpidrro_el0)
        uc.reg_write_batch(list(zip(VREGS, s.v)))

    def get_state(self):
        uc = self.uc
        s = State()
        s.x = list(uc.reg_read_batch(XREGS))
        s.sp = uc.reg_read(C.UC_ARM64_REG_SP)
        s.nzcv = uc.reg_read(C.UC_ARM64_REG_NZCV)
        s.fpcr = uc.reg_read(C.UC_ARM64_REG_FPCR)
        s.tpidr_el0 = uc.reg_read(C.UC_ARM64_REG_TPIDR_EL0)
        s.tpidrro_el0 = uc.reg_read(C.UC_ARM64_REG_TPIDRRO_EL0)
        s.v = list(uc.reg_read_batch(VREGS))
        return s

    def run(self, begin, until, state, trace=True, with_v=True, max_steps=100000):
        self.trace = []
        self.writes = []
        self.steps = 0
        self.max_steps = max_steps
        self.tracing = trace
        self.with_v = with_v
        self.stop_reason = None
        self.set_state(state)
        status, msg = "ok", ""
        try:
            self.uc.emu_start(begin, until)
        except UcError as e:
            status, msg = "fault", str(e)
        if self.stop_reason == "limit":
            status, msg = "limit", "limite de pas"
        elif self.stop_reason:
            status, msg = "trap", self.stop_reason
        return status, msg, self.get_state(), b"".join(self.trace), self.writes


def random_value(rng):
    k = rng.random()
    if k < 0.15:
        return rng.choice([0, 1, 2, 0xFFFFFFFF, 0xFFFFFFFFFFFFFFFF, 0x7FFFFFFF, 0x80000000,
                           0x7FFFFFFFFFFFFFFF, 0x8000000000000000, 0xFF, 0xFFFF, 0x100000000])
    if k < 0.4:
        return rng.getrandbits(rng.choice([4, 8, 16, 32]))
    return rng.getrandbits(64)


FP32_SPECIAL = [0x00000000, 0x80000000, 0x3F800000, 0xBF800000, 0x7F800000, 0xFF800000,
                0x7FC00000, 0x7FA00000, 0xFFC00001, 0x00000001, 0x807FFFFF, 0x7F7FFFFF,
                0x4F000000, 0xCF000000, 0x5F000000, 0x3F000000, 0x40490FDB, 0x4B000000]
FP64_SPECIAL = [0, 1 << 63, 0x3FF0000000000000, 0xBFF0000000000000, 0x7FF0000000000000,
                0xFFF0000000000000, 0x7FF8000000000000, 0x7FF4000000000000, 1,
                0x000FFFFFFFFFFFFF, 0x41E0000000000000, 0xC3E0000000000000, 0x3FE0000000000000,
                0x4330000000000000, 0x400921FB54442D18]


def random_vreg(rng):
    k = rng.random()
    if k < 0.3:
        return rng.getrandbits(128)
    if k < 0.65:   # lanes float32 « intéressantes »
        v = 0
        for i in range(4):
            if rng.random() < 0.5:
                lane = rng.choice(FP32_SPECIAL)
            else:
                lane = struct.unpack("<I", struct.pack("<f", rng.uniform(-1e3, 1e3)))[0]
            v |= lane << (32 * i)
        return v
    v = 0
    for i in range(2):
        if rng.random() < 0.5:
            lane = rng.choice(FP64_SPECIAL)
        else:
            lane = struct.unpack("<Q", struct.pack("<d", rng.uniform(-1e6, 1e6)))[0]
        v |= lane << (64 * i)
    return v


def scratch_pattern(rng, size, p_ptr=None):
    """Contenu de la zone de travail : mélange de pointeurs vers la zone et de valeurs.
    Une fois sur deux (si p_ptr n'est pas imposé), mode « objets » : 75 % de pointeurs valides,
    pour que les chaînes de pointeurs restent dans la zone."""
    if p_ptr is None:
        p_ptr = 0.3 if rng.random() < 0.5 else 0.75
    b = bytearray(size)
    words = size // 8
    vals = []
    for i in range(words):
        k = rng.random()
        if k < p_ptr:
            vals.append(SCRATCH_BASE + rng.randrange(0, size - 0x1000) & ~7)
        elif k < p_ptr + (1 - p_ptr) * 0.5:
            vals.append(rng.getrandbits(8) if rng.random() < 0.8 else 0)
        else:
            vals.append(rng.getrandbits(64))
    struct.pack_into(f"<{words}Q", b, 0, *vals)
    return bytes(b)


def diff_regs(a, b, with_v=True):
    """Liste des registres différents entre deux entrées de trace (784 octets)."""
    out = []
    pa, pb = struct.unpack_from("<Q", a)[0], struct.unpack_from("<Q", b)[0]
    if pa != pb:
        out.append(("pc", pa, pb))
    xa = struct.unpack_from("<31QQI", a, 8)
    xb = struct.unpack_from("<31QQI", b, 8)
    for i in range(33):
        if xa[i] != xb[i]:
            out.append((REG_NAMES[i], xa[i], xb[i]))
    if with_v:
        for i in range(32):
            va = a[272 + 16 * i:288 + 16 * i]
            vb = b[272 + 16 * i:288 + 16 * i]
            if va != vb:
                out.append((f"v{i}", int.from_bytes(va, "little"), int.from_bytes(vb, "little")))
    return out


def diff_states(a, b, with_v=True):
    out = []
    ra, rb = a.regs(), b.regs()
    n = len(ra) if with_v else 33
    for i in range(n):
        if ra[i] != rb[i]:
            out.append((REG_NAMES[i], ra[i], rb[i]))
    return out
