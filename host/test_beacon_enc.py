#!/usr/bin/env python3
"""Bit-exact test of the firmware's beacon re-encoder (firmware/main/beacon_enc.c) against dvbs2.py, on the PC.

beacon_enc.c is plain C: it is built here with the system compiler and asked to re-encode firmware/main/beacon.bin
  1. unchanged (the result must be beacon.bin itself),
  2. without / with pilots (the other setting than beacon.bin has),
  3. with another service name and provider,
and the results are compared with dvbs2.Encoder fed with the BBFRAMEs read back out of beacon.bin (LDPC is systematic).

usage (from the repository root):  python3 host/test_beacon_enc.py [DIR]
DIR holds beacon.bin, beacon_cfg.h and beacon_fec.h of a make_beacon.py run (default firmware/main); normal or short frames.
"""
import os
import re
import shutil
import subprocess
import sys
import tempfile

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import dvbs2  # noqa: E402
import make_beacon as mb  # noqa: E402

MAIN = os.path.join(HERE, "..", "firmware", "main")
HARNESS = r"""
#include <stdio.h>
#include <stdlib.h>
#include "beacon_enc.h"
int main(int argc, char **argv) {
    FILE *f = fopen(argv[1], "rb");
    static uint8_t src[1 << 17];
    size_t n = fread(src, 1, sizeof(src), f);
    fclose(f);
    (void)n;
    const int pilots = atoi(argv[3]);
    uint8_t *dst = calloc(1, benc_loop_bytes(pilots));
    uint32_t nsym = benc_build(dst, src, pilots, argv[4], argv[5]);
    if (!nsym) return 1;
    f = fopen(argv[2], "wb");
    fwrite(dst, 1, nsym / 4 + 16, f);
    fclose(f);
    return 0;
}
"""


def cfg(d):
    text = open(os.path.join(d, "beacon_cfg.h")).read()
    short = re.search(r"#define BEACON_FRAME_SHORT\s+1", text) is not None
    get = lambda k: re.search(rf"#define {k}\s+(\S+)", text).group(1)
    return dict(fec=get("BEACON_FEC").strip('"'), pilots=get("BEACON_PILOTS") == "1", symbols=int(get("BEACON_SYMBOLS").rstrip("u")),
                service=re.search(r'BEACON_SERVICE\s+"(.*)"', text).group(1), provider=re.search(r'BEACON_PROVIDER\s+"(.*)"', text).group(1),
                frame="short" if short else "normal")


def bbframes(loop, c):
    """The BBFRAMEs (scrambled, as dvbs2.Encoder.plframe takes them) read back out of the packed loop."""
    enc = dvbs2.Encoder(c["fec"], c["frame"], c["pilots"], mod="qpsk")
    slots = (16200 if c["frame"] == "short" else 64800) // 2 // 90
    s = np.frombuffer(loop, np.uint8)
    sym = np.stack([s & 3, s >> 2 & 3, s >> 4 & 3, s >> 6 & 3], 1).reshape(-1)[:c["symbols"]]
    out = []
    for f in range(c["symbols"] // enc.nsym):
        fr = sym[f * enc.nsym + 90:(f + 1) * enc.nsym]
        li, lq = (fr & 1).astype(np.int8) * 2 - 1, (fr >> 1).astype(np.int8) * 2 - 1
        r = dvbs2.rotation(len(fr))                      # undo the PL scrambling: multiply by j^-R
        si = np.where(r == 0, li, np.where(r == 1, lq, np.where(r == 2, -li, -lq)))
        sq = np.where(r == 0, lq, np.where(r == 1, -li, np.where(r == 2, -lq, li)))
        if c["pilots"]:
            keep = np.ones(len(fr), bool)
            for b in range((slots - 1) // 16):
                n0 = 1440 * (b + 1) + 36 * b
                keep[n0:n0 + 36] = False
            si, sq = si[keep], sq[keep]
        bits = np.empty(2 * len(si), np.uint8)
        bits[0::2], bits[1::2] = si < 0, sq < 0
        out.append(np.packbits(bits[:enc.kbch]).tobytes())
    return out


def encode(bbs, fec, pilots, frame="normal"):
    enc = dvbs2.Encoder(fec, frame, pilots, mod="qpsk")
    out = b"".join(enc.pack(*enc.plframe(bb)) for bb in bbs)
    return out + out[:16]


def patch_sdt(bb, kbch, service, provider):
    """make_beacon.py's SDT in packet 3 of the first BBFRAME, and its CRC-8 in the sync position of packet 4."""
    prbs = np.packbits(dvbs2.bb_prbs(kbch)).tobytes()
    plain = bytearray(a ^ b for a, b in zip(bb, prbs))
    pn, sn = provider.encode("latin-1"), service.encode("latin-1")
    desc = bytes([0x48, 3 + len(pn) + len(sn), 0x01, len(pn)]) + pn + bytes([len(sn)]) + sn
    sdt = mb.section(0x42, 1, bytes([0, 1, 0xFF]) + bytes([0, 1, 0xFC, 0x80 | len(desc) >> 8, len(desc) & 255]) + desc)
    pkt = mb.psi_packet(mb.PID_SDT, sdt, 0, True)
    p0 = 10 + 188 * 3
    assert plain[p0 + 1] & 0x1F == 0 and plain[p0 + 2] == mb.PID_SDT
    plain[p0 + 1:p0 + 188] = pkt[1:]
    plain[p0 + 188] = dvbs2.crc8(pkt[1:])
    return bytes(a ^ b for a, b in zip(plain, prbs))


def main():
    d = sys.argv[1] if len(sys.argv) > 1 else MAIN
    c = cfg(d)
    src = os.path.join(d, "beacon.bin")
    loop = open(src, "rb").read()
    with tempfile.TemporaryDirectory() as tmp:
        open(os.path.join(tmp, "t.c"), "w").write(HARNESS)
        # the encoder next to the headers under test: #include "..." looks in the source file's directory first
        for name, where in (("beacon_enc.c", MAIN), ("beacon_enc.h", MAIN), ("beacon_cfg.h", d), ("beacon_fec.h", d)):
            shutil.copy(os.path.join(where, name), tmp)
        exe = os.path.join(tmp, "t")
        subprocess.run(["cc", "-O2", "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-Wall", "-Werror", "-I", tmp, "-o", exe, os.path.join(tmp, "t.c"),
                        os.path.join(tmp, "beacon_enc.c")], check=True)

        def run(pilots, service, provider):
            out = os.path.join(tmp, "out.bin")
            subprocess.run([exe, src, out, str(int(pilots)), service, provider], check=True)
            return open(out, "rb").read()

        bbs = bbframes(loop, c)
        f = c["frame"]
        print(f"{d}: {c['fec']} {f} frames, pilots {int(c['pilots'])}, {len(bbs)} PLFRAMEs")
        assert encode(bbs, c["fec"], c["pilots"], f) == loop, "dvbs2.py does not reproduce beacon.bin from its own BBFRAMEs"
        checks = [
            ("unchanged", run(c["pilots"], c["service"], c["provider"]), loop),
            ("pilots " + ("off" if c["pilots"] else "on"), run(not c["pilots"], c["service"], c["provider"]), encode(bbs, c["fec"], not c["pilots"], f)),
        ]
        for pil in (c["pilots"], not c["pilots"]):
            new = [patch_sdt(bbs[0], len(bbs[0]) * 8, "TEST-SVC 123", "Some Provider/x")] + bbs[1:]
            checks.append((f"new names, pilots {int(pil)}", run(pil, "TEST-SVC 123", "Some Provider/x"), encode(new, c["fec"], pil, f)))
        ok = True
        for name, got, want in checks:
            same = got == want
            ok &= same
            print(f"{name:24s} {'OK' if same else 'DIFFERENT'}  ({len(got)} bytes)")
        sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
