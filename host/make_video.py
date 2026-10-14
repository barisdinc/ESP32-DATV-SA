#!/usr/bin/env python3
"""Builds a video for the stand-alone transmitter: a looped DVB-S2 QPSK symbol stream that the ESP plays from its flash.

The same transmission as the built-in beacon (QPSK 3/4, pilots, normal frames, 32 whole TS packets per BBFRAME, see
make_beacon.py), at a slightly lower symbol rate (160 MHz / 484 cycles = 330.58 kBd instead of 333: the ESP needs 4 spare cycles per
symbol to fetch the symbols from its flash while it transmits), but from a video file: FFmpeg encodes a part of it to H.264 + MP2 in a constant-rate transport stream, null packets
spread evenly bring it to the channel rate (so the PCRs stay right), and every packet of the loop is DVB-S2 encoded here. The loop
restarts the time base when it wraps: the first packet of every PID that carries an adaptation field gets the discontinuity indicator,
the others (the tables) an adaptation-field-only packet with it in front. The wrap costs the receiver a short hiccup; a small mux
delay and no B frames keep the content that is still in flight at the wrap (and dropped there) short.

The output starts with a 4 KB header (VIDEO_MAGIC, loop length, symbol rate, code rate, pilots, CRC32 of the data, service name and
provider), then the symbols, 4 per byte, first symbol in the low bits, followed by the loop's first 64 bytes again (the ESP reads the
flash 64 bytes at a time, and a read may run past the end of the loop). Write it to the "video" partition at VIDEO_ADDR; the firmware plays it instead of the
test card when the header is valid.

usage (from the repository root):
  python3 host/make_video.py media/sintel_trailer.mp4 --start 10 --seconds 20 -o video.bin
  esptool --chip esp32c3 write-flash 0x110000 video.bin
"""
import argparse
import dataclasses
import os
import struct
import subprocess
import sys
import zlib

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import dvbs2  # noqa: E402
from make_beacon import PER_FRAME, PKT, bbframe  # noqa: E402
from tx_media import encoding_settings  # noqa: E402

VIDEO_MAGIC = b"DATVVID1"
VIDEO_VERSION = 2
VIDEO_ADDR = 0x110000                             # the "video" partition (firmware/partitions.csv)
VIDEO_SIZE = 0x2F0000
HEADER = 4096
CPU_HZ, SYMBOL_CYCLES = 160_000_000, 484         # 24 DAC samples of 20 cycles plus 4 spare cycles per symbol (lutf.S)
BAUD, FEC = round(CPU_HZ / SYMBOL_CYCLES), "3/4"
TAIL = 64                                         # loop bytes repeated past its end
MUX_DELAY = 0.3                                   # s: content in flight at the wrap (FFmpeg's default is 0.7)


def encode_ts(path, start, seconds, width, mux_cap, name, provider):
    """The constant-rate transport stream of the chosen part of the file, as FFmpeg writes it."""
    v = dataclasses.replace(encoding_settings(mux_cap), width=width)
    cmd = ["ffmpeg", "-nostdin", "-hide_banner", "-loglevel", "error", "-ss", str(start), "-t", str(seconds), "-i", path,
           "-map", "0:v:0", "-map", "0:a:0?", "-vf", f"scale={v.width}:-2,fps={v.fps}",
           "-c:v", "libx264", "-preset", "slow", "-profile:v", "main", "-g", str(round(2 * v.fps)), "-bf", "0",
           "-b:v", str(v.video_bps), "-maxrate", str(v.video_bps), "-bufsize", str(v.video_bps // 4), "-muxdelay", str(MUX_DELAY),
           "-x264-params", "nal-hrd=cbr:force-cfr=1", "-pix_fmt", "yuv420p",
           "-c:a", "mp2", "-b:a", f"{v.audio_k}k", "-ac", str(v.channels), "-ar", str(v.sample_rate),
           "-f", "mpegts", "-muxrate", str(v.mux), "-pcr_period", "100", "-pat_period", str(v.pat_period),
           "-mpegts_flags", "+resend_headers", "-metadata", f"service_name={name}", "-metadata", f"service_provider={provider}", "-"]
    ts = subprocess.run(cmd, check=True, stdout=subprocess.PIPE).stdout
    if len(ts) % PKT or any(ts[i] != 0x47 for i in range(0, len(ts), PKT)):
        raise SystemExit("FFmpeg did not write whole transport stream packets")
    return [ts[i:i + PKT] for i in range(0, len(ts), PKT)], v


def spread(pkts, n):
    """n packets: the stream with null packets spread evenly between its packets (or cut to n)."""
    m = min(len(pkts), n)
    out, j = [], 0
    for i in range(n):
        if j < m and i * m >= j * n:
            out.append(pkts[j])
            j += 1
        else:
            out.append(dvbs2.NULL_PACKET)
    return out


def mark_discontinuity(pkts):
    """Sets the discontinuity indicator in the first packet of every PID that has an adaptation field there; returns, for the other
    PIDs, adaptation-field-only packets with the indicator that go in front of the loop (their continuity counter is the one before
    the PID's first packet, as a packet without payload does not count)."""
    seen, front = set(), []
    for i, p in enumerate(pkts):
        pid = (p[1] & 0x1F) << 8 | p[2]
        if pid == 0x1FFF or pid in seen:
            continue
        seen.add(pid)
        if p[3] & 0x20 and p[4] > 0:
            q = bytearray(p)
            q[5] |= 0x80
            pkts[i] = bytes(q)
        else:
            front.append(bytes([0x47, pid >> 8, pid & 255, 0x20 | (p[3] - 1) & 15, PKT - 5, 0x80]) + b"\xFF" * (PKT - 6))
    return front


def header(nsym, pilots, data, name, provider):
    h = VIDEO_MAGIC + struct.pack("<IIIII", VIDEO_VERSION, nsym, BAUD, len(data), zlib.crc32(data))
    h += struct.pack("<BB", int(pilots), dvbs2.MODCOD[FEC]) + b"\0" * 2
    h += name.encode("latin-1")[:31].ljust(32, b"\0") + provider.encode("latin-1")[:31].ljust(32, b"\0")
    return h + b"\xFF" * (HEADER - len(h))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("film", help="any video file FFmpeg can read")
    ap.add_argument("-o", "--out", default="video.bin")
    ap.add_argument("--start", type=float, default=0.0, help="start of the part to take [s]")
    ap.add_argument("--seconds", type=float, default=20.0, help="length of the loop [s]; at most about 35 s fit the partition")
    ap.add_argument("--width", type=int, default=480, help="picture width")
    ap.add_argument("--no-pilots", action="store_true")
    ap.add_argument("--service-name", default="yATOLYE-TV")
    ap.add_argument("--service-provider", default="Technology Community")
    ap.add_argument("--ts", help="also write the transport stream loop here, for checking with ffprobe / ffplay")
    a = ap.parse_args()
    pilots = not a.no_pilots
    enc = dvbs2.Encoder(FEC, "normal", pilots, mod="qpsk")
    rate = PER_FRAME * 1504 * BAUD / enc.nsym     # the channel's TS rate with exactly PER_FRAME packets per frame
    frames = -(-round(a.seconds * rate / 1504) // PER_FRAME)
    frames += -frames % 4                         # whole bytes (even frames), and the chunk halves stay whole frames
    if HEADER + frames * enc.nsym // 4 + 16 > VIDEO_SIZE:
        raise SystemExit(f"{a.seconds} s do not fit the {VIDEO_SIZE // 1024} KB partition: lower --seconds")
    src, v = encode_ts(a.film, a.start, a.seconds, a.width, rate, a.service_name, a.service_provider)
    front = mark_discontinuity(src)
    pkts = front + spread(src, frames * PER_FRAME - len(front))
    if a.ts:
        open(a.ts, "wb").write(b"".join(pkts) * 2)
    crc = dvbs2.crc8(pkts[-1][1:])                # the first packet's CRC slot carries the CRC of the loop's last packet
    out = bytearray()
    for f in range(frames):
        bb, crc = bbframe(enc, pkts[f * PER_FRAME:(f + 1) * PER_FRAME], crc)
        out += enc.pack(*enc.plframe(bb))
    assert len(enc.pend) == 0
    nsym = len(out) * 4
    out += out[:TAIL]
    open(a.out, "wb").write(header(nsym, pilots, bytes(out), a.service_name, a.service_provider) + out)
    print(f"{a.film} from {a.start:g} s: {v.width} px, {v.fps} fps, video {v.video_bps // 1000} kb/s, audio {v.audio_k} kb/s; "
          f"{len(src)} TS packets + {len(pkts) - min(len(src), len(pkts))} null = {frames} PLFRAMEs, {nsym / BAUD:.1f} s loop")
    print(f"{a.out}: {HEADER + len(out)} bytes ({(HEADER + len(out)) / VIDEO_SIZE * 100:.0f} % of the partition). "
          f"Write it at 0x{VIDEO_ADDR:x}:  esptool --chip esp32c3 write-flash 0x{VIDEO_ADDR:x} {a.out}")


if __name__ == "__main__":
    main()
