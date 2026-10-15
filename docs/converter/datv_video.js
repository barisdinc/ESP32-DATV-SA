// DVB-S2 encoder of the video converter page: the same loop as host/make_video.py, in the browser.
// Input: the constant-rate transport stream FFmpeg wrote; output: the video partition image (4 KB header + QPSK symbols, 4 per byte).
// QPSK 3/4, normal frames, 32 whole TS packets per BBFRAME (see host/make_beacon.py and host/dvbs2.py, the reference this is checked against).

export const VIDEO_ADDR = 0x110000, VIDEO_SIZE = 0x2F0000, HEADER = 4096, TAIL = 64;
export const BAUD = Math.round(160e6 / 484);       // 24 DAC samples of 20 cycles plus 4 spare cycles per symbol (lutf.S)
export const PKT = 188, PER_FRAME = 32, MODCOD = 7, MUX_DELAY = 0.3;
const KBCH = 48408, NLDPC = 64800, NK = 16200, Q = NK / 360;

// PLHEADER bits (SOF + PLSCODE of QPSK 3/4, normal frame) without and with pilots, from dvbs2.Encoder
const HDR = {
  false: "011000110100101110100000100111000101100010011111001100100110101100010000100010110100000101",
  true: "011000110100101110100000100010010000110111001010011001110011111001000101110111100001010000",
};
// BCH generator of the normal frame, t = 12 (x^192 included)
const BCH_G = 0x14e260e83845c511c50cf2cd8dc350889034785f7660255e7n;
// LDPC parity address table of the normal frame, rate 3/4 (EN 302 307 annex B)
const LDPC = [[0,821,2504,2722,3252,5243,6385,7374,7901,11200,13389,14611],[1,357,852,2001,2698,6752,7244,11359,11417,12772,13824,15310],[2,1708,6321,6399,7862,7977,12197,13444,13612,13860,14449,15137],[3,1560,3646,3812,5795,6975,7306,7866,8772,11804,13292,14327],[4,1230,1628,2113,4870,7626,9283,9689,10809,11407,14599,15241],[5,1400,1610,5411,5699,6303,7358,9446,12515,13925,14181,15876],[6,3405,4059,4651,5970,7853,7992,8836,9675,10278,10368,15336],[7,406,629,2109,3963,4441,7459,9153,12030,12221,12683,15212],[8,543,875,3497,3563,5771,6007,6235,8411,9186,13908,14202],[9,546,2071,3232,3399,4795,4932,6625,7250,7312,9781,12652],[10,488,6585,7069,7183,7455,8820,9238,10088,10158,11090,13134],[11,119,215,1903,7558,7961,10615,10818,11046,11545,14784,15619],[12,1469,2134,2692,3655,4917,5129,7150,8736,14768,15874,15944],[13,505,806,2622,3820,4216,6757,7957,8316,8923,13244,15589],[14,3041,4852,6551,8152,8758,11193,12860,13673,14463,15108,15733],[15,3149,11981],[16,6906,13416],[17,13098,13352],[18,2009,14460],[19,4314,7207],[20,3312,3945],[21,4418,6248],[22,2669,13975],[23,7571,9023],[24,2967,14172],[25,7138,7271],[26,6135,13670],[27,7490,14559],[28,2466,8657],[29,8599,12834],[30,3152,3470],[31,4365,13917],[32,6024,13730],[33,10973,14182],[34,2464,13167],[35,5281,15049],[36,1103,1849],[37,1069,2058],[38,6095,9654],[39,7667,14311],[40,8146,15617],[41,4588,11218],[42,6243,13660],[43,7874,8578],[44,2686,11741],[0,1022,1264],[1,9965,12604],[2,2707,8217],[3,3156,11793],[4,354,1514],[5,6978,14058],[6,7922,16079],[7,12138,15087],[8,5053,6470],[9,12687,14932],[10,1763,15458],[11,1721,8121],[12,549,12431],[13,4129,7091],[14,1426,8415],[15,7604,9783],[16,6295,11329],[17,1409,12061],[18,8065,9087],[19,2918,8438],[20,1293,14115],[21,3922,13851],[22,3851,4000],[23,1768,5865],[24,2655,14957],[25,5565,6332],[26,4303,12631],[27,11653,12236],[28,7632,16025],[29,4655,14128],[30,9584,13123],[31,9597,13987],[32,12110,15409],[33,8754,15490],[34,7416,15325],[35,2909,15549],[36,2995,8257],[37,4791,9406],[38,4854,11111],[39,2812,8521],[40,8476,14717],[41,7820,15360],[42,1179,7939],[43,2357,8678],[44,6216,7703],[0,3477,7067],[1,3931,13845],[2,7675,12899],[3,1754,8187],[4,1400,7785],[5,5891,9213],[6,2494,7703],[7,2576,7902],[8,4821,15682],[9,10426,11935],[10,904,1810],[11,9264,11332],[12,3570,11312],[13,2650,14916],[14,7679,7842],[15,6089,13084],[16,2751,3938],[17,4648,8509],[18,8917,12204],[19,5749,12443],[20,4431,12613],[21,1344,4014],[22,8488,13850],[23,1730,14896],[24,7126,14942],[25,8863,14983],[26,6578,8564],[27,396,4947],[28,297,12805],[29,6692,13878],[30,11186,11857],[31,11493,14395],[32,12251,16145],[33,7428,13462],[34,13119,14526],[35,2535,11243],[36,6465,12690],[37,6872,9334],[38,14023,15371],[39,8101,10187],[40,4848,11963],[41,6119,15125],[42,8051,14465],[43,5167,11139],[44,2883,14521]];

export function frameSymbols(pilots) {
  return 90 + NLDPC / 2 + (pilots ? 36 * Math.floor((NLDPC / 2 / 90 - 1) / 16) : 0);
}

/** The channel's TS rate [bit/s] with exactly PER_FRAME packets per frame. */
export function channelRate(pilots) {
  return PER_FRAME * 1504 * BAUD / frameSymbols(pilots);
}

/** PLFRAMEs of a loop of `seconds` (a multiple of 4: whole bytes, and the firmware's chunk halves stay whole frames). */
export function loopFrames(seconds, pilots) {
  let frames = Math.ceil(Math.round(seconds * channelRate(pilots) / 1504) / PER_FRAME);
  return frames + (4 - frames % 4) % 4;
}

export function imageSize(frames, pilots) {
  return HEADER + frames * frameSymbols(pilots) / 4 + TAIL;
}

/** The longest loop that fits the partition [s]. */
export function maxSeconds(pilots) {
  const frames = Math.floor((VIDEO_SIZE - HEADER - TAIL) * 4 / frameSymbols(pilots) / 4) * 4;
  return frames * PER_FRAME * 1504 / channelRate(pilots);
}

/** FFmpeg's encoding settings for the channel (tx_media.encoding_settings at a mux rate above 200 kb/s). */
export function encodingSettings(pilots, width) {
  const mux = Math.trunc(channelRate(pilots) * 0.965);
  const audioK = 32, patPeriod = 0.5, psi = Math.trunc(3 * 188 * 8 / patPeriod);
  return { mux, width, fps: 15, videoBps: Math.trunc((mux - audioK * 1000 - psi) * 0.88), audioK, channels: 1, sampleRate: 24000, patPeriod };
}

/** FFmpeg arguments (the same as make_video.encode_ts, written to a file instead of stdout). */
export function ffmpegArgs(input, output, start, seconds, v, name, provider) {
  return ["-hide_banner", "-ss", String(start), "-t", String(seconds), "-i", input,
    "-map", "0:v:0", "-map", "0:a:0?", "-vf", `scale=${v.width}:-2,fps=${v.fps}`,
    "-c:v", "libx264", "-preset", "slow", "-profile:v", "main", "-g", String(Math.round(2 * v.fps)), "-bf", "0",
    "-b:v", String(v.videoBps), "-maxrate", String(v.videoBps), "-bufsize", String(Math.floor(v.videoBps / 4)), "-muxdelay", String(MUX_DELAY),
    "-x264-params", "nal-hrd=cbr:force-cfr=1", "-pix_fmt", "yuv420p",
    "-c:a", "mp2", "-b:a", `${v.audioK}k`, "-ac", String(v.channels), "-ar", String(v.sampleRate),
    "-f", "mpegts", "-muxrate", String(v.mux), "-pcr_period", "100", "-pat_period", String(v.patPeriod),
    "-mpegts_flags", "+resend_headers", "-metadata", `service_name=${name}`, "-metadata", `service_provider=${provider}`, output];
}

// ------------------------------------------------------------------ CRCs and scramblers
const CRC8 = new Uint8Array(256);
for (let b = 0; b < 256; b++) {
  let c = b;
  for (let i = 0; i < 8; i++) c = c & 0x80 ? ((c << 1) ^ 0xD5) & 0xFF : (c << 1) & 0xFF;
  CRC8[b] = c;
}
function crc8(a, from = 0, to = a.length) {
  let c = 0;
  for (let i = from; i < to; i++) c = CRC8[c ^ a[i]];
  return c;
}

const CRC32 = new Uint32Array(256);
for (let n = 0; n < 256; n++) {
  let c = n;
  for (let k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320 ^ (c >>> 1) : c >>> 1;
  CRC32[n] = c >>> 0;
}
export function crc32(a) {
  let c = 0xFFFFFFFF;
  for (let i = 0; i < a.length; i++) c = CRC32[(c ^ a[i]) & 255] ^ (c >>> 8);
  return (c ^ 0xFFFFFFFF) >>> 0;
}

// BB scrambler 1 + x^14 + x^15, initial 100101010000000, as bytes over the K_bch bits
const BB_PRBS = (() => {
  const reg = [1, 0, 0, 1, 0, 1, 0, 1, 0, 0, 0, 0, 0, 0, 0], out = new Uint8Array(KBCH / 8);
  for (let i = 0; i < KBCH; i++) {
    const o = reg[13] ^ reg[14];
    reg.pop(); reg.unshift(o);
    out[i >> 3] |= o << (7 - (i & 7));
  }
  return out;
})();

// PL scrambler (Gold code 0): R(i) in 0..3, the symbol is multiplied by j^R
function goldRotation(length) {
  const N = 2 ** 18 - 1, x = new Uint8Array(N + 20), y = new Uint8Array(N + 20);
  x[0] = 1;
  y.fill(1, 0, 18);
  for (let i = 0; i < N + 2 - 18; i++) {
    x[i + 18] = x[i + 7] ^ x[i];
    y[i + 18] = y[i + 10] ^ y[i + 7] ^ y[i + 5] ^ y[i];
  }
  const r = new Uint8Array(length);
  for (let i = 0; i < length; i++) r[i] = (x[i] ^ y[i]) + 2 * (x[(i + 131072) % N] ^ y[(i + 131072) % N]);
  return r;
}

// ------------------------------------------------------------------ BCH and LDPC
// remainder of (b x^192) mod g for every byte b, as 6 words (most significant first)
const BCH_TAB = (() => {
  const tab = [], mask = (1n << 192n) - 1n;
  for (let b = 0n; b < 256n; b++) {
    let r = b << 192n;
    for (let bit = 199n; bit >= 192n; bit--) if ((r >> bit) & 1n) r ^= BCH_G << (bit - 192n);
    r &= mask;
    tab.push(Array.from({ length: 6 }, (_, i) => Number((r >> BigInt(32 * (5 - i))) & 0xFFFFFFFFn)));
  }
  return tab;
})();

function bchParity(data) {
  const s = new Uint32Array(6);
  for (let i = 0; i < data.length; i++) {
    const t = BCH_TAB[((s[0] >>> 24) ^ data[i]) & 255];
    for (let w = 0; w < 5; w++) s[w] = ((s[w] << 8) | (s[w + 1] >>> 24)) ^ t[w];
    s[5] = (s[5] << 8) ^ t[5];
  }
  const out = new Uint8Array(24);
  for (let w = 0; w < 6; w++) for (let k = 0; k < 4; k++) out[4 * w + k] = s[w] >>> (24 - 8 * k);
  return out;
}

function ldpcParity(info) {             // info: 48600 bits (one per byte) -> 16200 parity bits
  const p = new Uint8Array(NK);
  LDPC.forEach((row, g) => {
    for (let m = 0; m < 360; m++) {
      if (!info[360 * g + m]) continue;
      for (const x of row) p[(x + Q * m) % NK] ^= 1;
    }
  });
  for (let i = 1; i < NK; i++) p[i] ^= p[i - 1];
  return p;
}

// ------------------------------------------------------------------ transport stream loop
const NULL_PACKET = (() => {
  const p = new Uint8Array(PKT).fill(0xFF);
  p.set([0x47, 0x1F, 0xFF, 0x10]);
  return p;
})();

/** FFmpeg's transport stream -> 188 byte packets. */
export function splitPackets(ts) {
  if (ts.length % PKT) throw new Error("FFmpeg did not write whole transport stream packets");
  const pkts = [];
  for (let i = 0; i < ts.length; i += PKT) {
    if (ts[i] !== 0x47) throw new Error("FFmpeg did not write whole transport stream packets");
    pkts.push(ts.slice(i, i + PKT));
  }
  return pkts;
}

/** Discontinuity indicator in the first packet of every PID with an adaptation field there; adaptation-field-only packets for the
 *  other PIDs, to go in front of the loop (make_video.mark_discontinuity). */
function markDiscontinuity(pkts) {
  const seen = new Set(), front = [];
  pkts.forEach((p) => {
    const pid = (p[1] & 0x1F) << 8 | p[2];
    if (pid === 0x1FFF || seen.has(pid)) return;
    seen.add(pid);
    if (p[3] & 0x20 && p[4] > 0) {
      p[5] |= 0x80;
    } else {
      const q = new Uint8Array(PKT).fill(0xFF);
      q.set([0x47, pid >> 8, pid & 255, 0x20 | ((p[3] - 1) & 15), PKT - 5, 0x80]);
      front.push(q);
    }
  });
  return front;
}

function spread(pkts, n) {
  const m = Math.min(pkts.length, n), out = [];
  let j = 0;
  for (let i = 0; i < n; i++) {
    if (j < m && i * m >= j * n) out.push(pkts[j++]);
    else out.push(NULL_PACKET);
  }
  return out;
}

function header(nsym, pilots, data, name, provider) {
  const h = new Uint8Array(HEADER).fill(0xFF), dv = new DataView(h.buffer);
  h.set(new TextEncoder().encode("DATVVID1"));
  [2, nsym, BAUD, data.length, crc32(data)].forEach((v, i) => dv.setUint32(8 + 4 * i, v, true));
  h.set([pilots ? 1 : 0, MODCOD, 0, 0], 28);
  [name, provider].forEach((s, k) => {
    const f = new Uint8Array(32);
    for (let i = 0; i < Math.min(s.length, 31); i++) f[i] = s.charCodeAt(i);
    h.set(f, 32 + 32 * k);
  });
  return h;
}

/** Latin-1 text for the header and the SDT (the firmware shows it on the front panel). */
export function checkName(s) {
  return /^[\x20-\x7E\xA0-\xFF]*$/.test(s);
}

/**
 * The video partition image of a loop of `frames` PLFRAMEs from FFmpeg's transport stream.
 * onProgress(done, total) is called now and then; the work yields to the browser between frames.
 */
export async function buildImage(ts, frames, { pilots = true, name = "yATOLYE-TV", provider = "Technology Community", onProgress } = {}) {
  const src = splitPackets(ts);
  const front = markDiscontinuity(src);
  const pkts = front.concat(spread(src, frames * PER_FRAME - front.length));
  const hdr = Array.from(HDR[pilots], Number);
  const nsymFrame = frameSymbols(pilots), rot = goldRotation(nsymFrame - 90);
  const out = new Uint8Array(frames * nsymFrame / 4 + TAIL);
  const bb = new Uint8Array(KBCH / 8), info = new Uint8Array(NLDPC - NK);
  let crc = crc8(pkts[pkts.length - 1], 1), sym = 0;
  const put = (i, q) => {                         // level 1 = +1: I in bit 0, Q in bit 1
    out[sym >> 2] |= ((i > 0) | (q > 0) << 1) << (2 * (sym & 3));
    sym++;
  };
  for (let f = 0; f < frames; f++) {
    // BBFRAME: BBHEADER + 32 packets with the CRC-8 of the previous one in the sync byte, padding, BB scrambling
    bb.fill(0);
    const dfl = 8 * PER_FRAME * PKT;
    bb.set([0xF0, 0x00, 0x05, 0xE0, dfl >> 8, dfl & 255, 0x47, 0, 0]);
    bb[9] = crc8(bb, 0, 9);
    for (let k = 0; k < PER_FRAME; k++) {
      const p = pkts[f * PER_FRAME + k], o = 10 + k * PKT;
      bb[o] = crc;
      bb.set(p.subarray(1), o + 1);
      crc = crc8(p, 1);
    }
    for (let i = 0; i < bb.length; i++) bb[i] ^= BB_PRBS[i];
    // BCH + LDPC
    const par = bchParity(bb);
    for (let i = 0; i < KBCH; i++) info[i] = bb[i >> 3] >> (7 - (i & 7)) & 1;
    for (let i = 0; i < 192; i++) info[KBCH + i] = par[i >> 3] >> (7 - (i & 7)) & 1;
    const parity = ldpcParity(info);
    const bit = (i) => (i < info.length ? info[i] : parity[i - info.length]);
    // PLHEADER (pi/2 BPSK), then QPSK slots with pilots after every 16 slots, PL scrambled
    for (let i = 0; i < 90; i++) {
      const a = 1 - 2 * hdr[i];
      put(i % 2 ? -a : a, a);
    }
    let n = 0;
    const scrambled = (si, sq) => {
      const r = rot[n++];
      if (r === 0) put(si, sq);
      else if (r === 1) put(-sq, si);
      else if (r === 2) put(-si, -sq);
      else put(sq, -si);
    };
    const slots = NLDPC / 2 / 90;
    for (let s = 0; s < slots; s++) {
      for (let k = 0; k < 90; k++) {
        const c = 2 * (90 * s + k);
        scrambled(1 - 2 * bit(c), 1 - 2 * bit(c + 1));
      }
      if (pilots && s % 16 === 15 && s < slots - 1) for (let k = 0; k < 36; k++) scrambled(1, 1);
    }
    if (onProgress && (f % 4 === 3 || f === frames - 1)) {
      onProgress(f + 1, frames);
      await new Promise((r) => setTimeout(r, 0));
    }
  }
  const nsym = sym;
  out.copyWithin(nsym / 4, 0, TAIL);
  const image = new Uint8Array(HEADER + out.length);
  image.set(header(nsym, pilots, out, name, provider));
  image.set(out, HEADER);
  return { image, nsym, packets: src.length, nulls: pkts.length - Math.min(src.length, pkts.length) };
}
