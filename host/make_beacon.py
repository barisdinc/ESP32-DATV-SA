#!/usr/bin/env python3
"""Builds the stand-alone beacon of the firmware: a short DVB-S2 QPSK symbol loop that the ESP plays from RAM after power-up.

The ESP cannot encode video or LDPC while its single core drives the DAC, so the whole transmission is computed here: a still
test card (H.264), PAT / PMT / SDT and PCR in a transport stream of exactly N x 32 packets, N DVB-S2 PLFRAMEs, packed as the
firmware's QPSK ring expects (4 symbols per byte, first symbol in the low bits). Every BBFRAME carries exactly 32 whole TS packets
(DFL = 32 x 1504 bits, the rest of the frame is padding), so the loop repeats seamlessly at the DVB-S2 level. At the TS level
every loop restarts the time base: the first PCR and the first PSI packets carry the discontinuity indicator, and the video PID's
continuity counter runs through the loop boundary without a jump.

usage (from the repository root):  python3 host/make_beacon.py   ->  firmware/main/beacon.bin, firmware/main/beacon_cfg.h
"""
import argparse
import os
import subprocess
import sys
import tempfile
import zlib

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import dvbs2  # noqa: E402

PKT = 188
PER_FRAME = 32                                    # TS packets per BBFRAME (normal frame, QPSK 3/4 holds 32.27)
PID_PMT, PID_VIDEO, PID_SDT = 0x1000, 0x100, 0x11
PCR_EVERY = 12                                    # slots between PCRs (about 37 ms at 333 kS/s)
PCR0 = 10 * 27_000_000                            # PCR at the start of every loop
MARGIN = 0.03                                     # seconds between the arrival of a frame and its decoding time
FPS = 25


def crc32_mpeg(data):
    crc = 0xFFFFFFFF
    for b in data:
        crc ^= b << 24
        for _ in range(8):
            crc = (crc << 1) ^ 0x04C11DB7 if crc & 0x80000000 else crc << 1
            crc &= 0xFFFFFFFF
    return crc


def section(table_id, ext, body, private=False):
    """A long-form PSI section (syntax indicator 1, version 0, current) with its CRC."""
    n = 5 + len(body) + 4
    h = bytes([table_id, 0xB0 | (0x40 if private else 0) | (n >> 8), n & 255, ext >> 8, ext & 255, 0xC1, 0, 0])
    s = h + body
    return s + crc32_mpeg(s).to_bytes(4, "big")


def psi_packet(pid, sec, cc, discontinuity):
    payload = b"\x00" + sec                       # pointer field
    if discontinuity:
        head = bytes([0x47, 0x40 | pid >> 8, pid & 255, 0x30 | cc, 1, 0x80])
    else:
        head = bytes([0x47, 0x40 | pid >> 8, pid & 255, 0x10 | cc])
    p = head + payload
    assert len(p) <= PKT
    return p + b"\xFF" * (PKT - len(p))


def pcr_packet(pcr, cc, discontinuity):
    base, ext = divmod(pcr, 300)
    pcrb = bytes([base >> 25 & 255, base >> 17 & 255, base >> 9 & 255, base >> 1 & 255, (base & 1) << 7 | 0x7E | ext >> 8, ext & 255])
    af = bytes([0x10 | (0x80 if discontinuity else 0)]) + pcrb
    p = bytes([0x47, PID_VIDEO >> 8, PID_VIDEO & 255, 0x20 | cc, PKT - 5]) + af
    return p + b"\xFF" * (PKT - len(p))


def pts_bytes(pts):
    return bytes([0x21 | (pts >> 29 & 0x0E), pts >> 22 & 255, 0x01 | (pts >> 14 & 0xFE), pts >> 7 & 255, 0x01 | (pts << 1 & 0xFE)])


def pes_payloads(au, pts):
    """PES packet of one access unit split into TS payloads (lists of bytes, the first one starts the PES)."""
    pes = b"\x00\x00\x01\xE0\x00\x00\x80\x80\x05" + pts_bytes(pts) + au
    return [pes[i:i + 184] for i in range(0, len(pes), 184)]


def video_packet(chunk, cc, start):
    head = bytes([0x47, (0x40 if start else 0) | PID_VIDEO >> 8, PID_VIDEO & 255])
    if len(chunk) == 184:
        return head + bytes([0x10 | cc]) + chunk
    stuff = 184 - len(chunk) - 1                  # adaptation field length byte, then flags + stuffing
    af = bytes([stuff]) + (bytes([0]) + b"\xFF" * (stuff - 1) if stuff else b"")
    return head + bytes([0x30 | cc]) + af + chunk


def nal_units(es):
    """Annex B byte stream -> NAL units without start codes."""
    out, i = [], es.find(b"\x00\x00\x01")
    while i >= 0:
        j = es.find(b"\x00\x00\x01", i + 3)
        nal = es[i + 3:j if j >= 0 else len(es)]
        if j >= 0 and nal.endswith(b"\x00"):
            nal = nal.rstrip(b"\x00")
        out.append(nal)
        i = j
    return out


def encode_card(width, height, qp, frames, name, provider, info):
    """H.264 access units of a still test card: an IDR frame, then P frames (skips). SEI is dropped (x264 writes its options there)."""
    font = "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"
    esc = lambda t: t.replace("\\", "\\\\").replace(":", "\\:").replace("'", "\\'")
    text = (f"drawbox=x=0:y=ih*0.62:w=iw:h=ih*0.38:color=black:t=fill,"
            f"drawtext=fontfile={font}:text='{esc(name)}':fontcolor=white:fontsize=h/7:x=(w-text_w)/2:y=h*0.645,"
            f"drawtext=fontfile={font}:text='{esc(provider)}':fontcolor=yellow:fontsize=h/14:x=(w-text_w)/2:y=h*0.80,"
            f"drawtext=fontfile={font}:text='{esc(info)}':fontcolor=white:fontsize=h/22:x=(w-text_w)/2:y=h*0.905")
    with tempfile.TemporaryDirectory() as d:
        out = os.path.join(d, "card.h264")
        subprocess.run(["ffmpeg", "-nostdin", "-hide_banner", "-loglevel", "error", "-f", "lavfi",
                        "-i", f"smptehdbars=size={width}x{height}:rate={FPS},{text}", "-frames:v", str(frames),
                        "-c:v", "libx264", "-preset", "veryslow", "-profile:v", "main", "-level", "3.0", "-pix_fmt", "yuv420p",
                        "-bf", "0", "-g", "250", "-qp", str(qp), "-x264-params", "aud=1:scenecut=0", "-f", "h264", out], check=True)
        es = open(out, "rb").read()
    aus, cur = [], []
    for nal in nal_units(es):
        t = nal[0] & 31
        if t == 9 and cur:
            aus.append(cur)
            cur = []
        if t != 6:
            cur.append(nal)
    aus.append(cur)
    return [b"".join(b"\x00\x00\x00\x01" + n for n in au) for au in aus]


def build_ts(aus, slots, slot_s, name, provider):
    """Lays out one loop of `slots` packets. Returns the packets and the decoding times of the frames that fit."""
    pat = section(0x00, 1, bytes([0, 1, 0xE0 | PID_PMT >> 8, PID_PMT & 255]))
    pmt = section(0x02, 1, bytes([0xE0 | PID_VIDEO >> 8, PID_VIDEO & 255, 0xF0, 0, 0x1B, 0xE0 | PID_VIDEO >> 8, PID_VIDEO & 255, 0xF0, 0]))
    pn, sn = provider.encode("latin-1"), name.encode("latin-1")
    desc = bytes([0x48, 3 + len(pn) + len(sn), 0x01, len(pn)]) + pn + bytes([len(sn)]) + sn
    sdt = section(0x42, 1, bytes([0, 1, 0xFF]) + bytes([0, 1, 0xFC, 0x80 | len(desc) >> 8, len(desc) & 255]) + desc)
    pcr_slots = set(range(0, slots, PCR_EVERY))
    psi_slots = {1: (0, pat), 2: (PID_PMT, pmt), 3: (PID_SDT, sdt)}
    free = [i for i in range(slots) if i not in pcr_slots and i not in psi_slots]
    loop_s = slots * slot_s
    # Place frames one after another; each one is decoded MARGIN after its last packet arrived, at least one frame period after the previous.
    chunks, dts, used = [], [], 0
    for k, au in enumerate(aus):
        pay = pes_payloads(au, 0)
        if used + len(pay) > len(free):
            break
        t = (free[used + len(pay) - 1] + 1) * slot_s + MARGIN
        if dts:
            t = max(t, dts[-1] + 1 / FPS)
        if t > loop_s - 0.01:
            break
        used += len(pay)
        dts.append(t)
        chunks.append(au)
    if not chunks:
        raise SystemExit("the IDR frame does not fit into the loop: raise --qp or --frames")
    payloads = []
    for au, t in zip(chunks, dts):
        payloads += [(c, i == 0) for i, c in enumerate(pes_payloads(au, round((PCR0 / 300) + t * 90000)))]
    # Continuity across the loop boundary: the number of video payload packets must be a multiple of 16. Extra packets
    # take the tail of the last PES one byte at a time (adaptation-field stuffing does the rest).
    while len(payloads) % 16:
        j = max((j for j, (c, _) in enumerate(payloads) if len(c) > 1), default=None)
        if j is None:
            raise SystemExit("cannot pad the video PID to a multiple of 16 packets")
        c, s = payloads[j]
        payloads[j:j + 1] = [(c[:-1], s), (c[-1:], False)]
    if len(payloads) > len(free):
        raise SystemExit("the video does not fit into the loop: raise --qp")
    pkts, cc, vi = [], 15, 0
    for i in range(slots):
        if i in pcr_slots:
            pkts.append(pcr_packet(PCR0 + round(i * slot_s * 27e6), cc, i == 0))
        elif i in psi_slots:
            pid, sec = psi_slots[i]
            pkts.append(psi_packet(pid, sec, 0, True))
        elif vi < len(payloads):
            cc = (cc + 1) & 15
            c, s = payloads[vi]
            pkts.append(video_packet(c, cc, s))
            vi += 1
        else:
            pkts.append(dvbs2.NULL_PACKET)
    assert cc == 15 and all(len(p) == PKT and p[0] == 0x47 for p in pkts)
    return pkts, dts, len(chunks)


def bbframe(enc, data, crc_prev):
    """One BBFRAME with exactly PER_FRAME whole packets (SYNCD 0) and padding to K_bch, BB scrambled."""
    stream = bytearray()
    for p in data:
        stream.append(crc_prev)
        stream += p[1:]
        crc_prev = dvbs2.crc8(p[1:])
    dfl = 8 * len(stream)
    h = bytearray([0xF0, 0x00, 0x05, 0xE0, dfl >> 8, dfl & 255, 0x47, 0, 0])
    h.append(dvbs2.crc8(h))
    bits = np.unpackbits(np.frombuffer(bytes(h) + bytes(stream), np.uint8))
    bits = np.concatenate((bits, np.zeros(enc.kbch - len(bits), np.uint8)))
    return np.packbits(bits ^ dvbs2.bb_prbs(enc.kbch)).tobytes(), crc_prev


def c_str(s):
    """C string literal for beacon_cfg.h (the front panel shows the service name and provider)."""
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--freq", type=float, default=2404.750, help="MHz")
    ap.add_argument("--baud", type=int, default=333000)
    ap.add_argument("--fec", default="3/4")
    ap.add_argument("--no-pilots", action="store_true")
    ap.add_argument("--frames", type=int, default=4, help="PLFRAMEs in the loop (even; RAM holds about 4 with pilots)")
    ap.add_argument("--amp", type=int, default=300)
    ap.add_argument("--width", type=int, default=640)
    ap.add_argument("--qp", type=int, default=0, help="H.264 QP of the card; 0 = the best that fits")
    ap.add_argument("--service-name", default="TA7W-TV")
    ap.add_argument("--service-provider", default="TA7W-TRABZON")
    ap.add_argument("--out", default=os.path.join(HERE, "..", "firmware", "main"))
    ap.add_argument("--ts", help="also write the TS loop (repeated 25 times) here, for checking with ffprobe / ffplay")
    a = ap.parse_args()
    if a.frames < 2 or a.frames % 2:
        raise SystemExit("--frames must be even (a PLFRAME is not a whole number of bytes)")
    pilots = not a.no_pilots
    enc = dvbs2.Encoder(a.fec, "normal", pilots, mod="qpsk")
    if enc.dfl < PER_FRAME * 1504:
        raise SystemExit(f"FEC {a.fec} holds fewer than {PER_FRAME} packets per frame")
    sps, period = 24, 20
    if 160_000_000 // a.baud != sps * period:
        raise SystemExit("the beacon loop runs at 24 samples per symbol and 8 MS/s: --baud must be about 333000")
    slots = a.frames * PER_FRAME
    slot_s = a.frames * enc.nsym / a.baud / slots
    height = (a.width * 9 // 16) // 2 * 2
    info = f"DVB-S2 QPSK {a.fec}  SR {a.baud // 1000}  {a.freq:.3f} MHz"
    for qp in ([a.qp] if a.qp else range(22, 52)):
        aus = encode_card(a.width, height, qp, 12, a.service_name, a.service_provider, info)
        try:
            pkts, dts, nframes = build_ts(aus, slots, slot_s, a.service_name, a.service_provider)
            break
        except SystemExit as e:
            if a.qp:
                raise
            print(f"qp {qp}: {e}")
    print(f"card {a.width}x{height} qp {qp}: IDR {len(aus[0])} bytes, {nframes} frames per loop, decoding at "
          + ", ".join(f"{t * 1000:.0f}" for t in dts) + f" ms of {slots * slot_s * 1000:.0f} ms")
    if a.ts:
        open(a.ts, "wb").write(b"".join(pkts) * 25)
    crc = dvbs2.crc8(pkts[-1][1:])                    # the first packet's CRC slot carries the CRC of the loop's last packet
    out = bytearray()
    for f in range(a.frames):
        bb, crc = bbframe(enc, pkts[f * PER_FRAME:(f + 1) * PER_FRAME], crc)
        out += enc.pack(*enc.plframe(bb))
    assert len(enc.pend) == 0 and len(out) * 4 == a.frames * enc.nsym
    nsym = len(out) * 4
    out += out[:16]                                   # the loop reads up to 3 symbols past its end before it wraps
    os.makedirs(a.out, exist_ok=True)
    open(os.path.join(a.out, "beacon.bin"), "wb").write(out)
    khz = round(a.freq * 1000)
    tsr = PER_FRAME * a.frames * 1504 / (slots * slot_s)
    cfg = f"""/* Generated by host/make_beacon.py - do not edit by hand.
 * {a.service_name} / {a.service_provider}: DVB-S2 QPSK {a.fec} normal frames{', pilots' if pilots else ''}, {a.baud} Bd, {a.freq:.3f} MHz,
 * {a.frames} PLFRAMEs = {nsym} symbols ({nsym / a.baud * 1000:.1f} ms), TS {tsr / 1000:.1f} kb/s, CRC32 {zlib.crc32(bytes(out)):08x}. */
#pragma once
#define BEACON_FREQ_KHZ   {khz}u
#define BEACON_BAUD       {a.baud}u
#define BEACON_SPS        {sps}u
#define BEACON_AMP        {a.amp}
#define BEACON_SYMBOLS    {nsym}u
#define BEACON_FEC        {c_str(a.fec)}
#define BEACON_PILOTS     {int(pilots)}
#define BEACON_SERVICE    {c_str(a.service_name)}
#define BEACON_PROVIDER   {c_str(a.service_provider)}
"""
    open(os.path.join(a.out, "beacon_cfg.h"), "w").write(cfg)
    print(f"{len(out)} bytes, {nsym} symbols = {nsym / a.baud * 1000:.1f} ms loop, TS {tsr / 1000:.1f} kb/s -> {os.path.normpath(a.out)}")


if __name__ == "__main__":
    main()
