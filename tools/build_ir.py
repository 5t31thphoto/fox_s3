#!/usr/bin/env python3
"""
build_ir.py — turn Flipper-format .ir libraries into one compact binary blob
that the Fox firmware memory-maps from the `foxdata` partition.

Design goals
------------
* Offline TV-B-Gone: a de-duplicated pool of POWER codes we can sweep.
* A modest set of *named* buttons (power/vol/mute/channel/input) per broad
  device category (tv / audio / projector), so "fox, volume up the TV" works
  after the user teaches the fox which single code is theirs.
* Everything pre-rendered to raw carrier + timing pairs so the firmware needs
  no protocol encoders at runtime — it just plays microsecond on/off pairs
  through the RMT peripheral.

The parsed Flipper protocols (NEC, NECext, NEC42, Samsung32, SIRC/15/20,
RC5/RC5X/RC6, Kaseikyo, Pioneer, RCA) are encoded here, at build time, on a
normal computer where we have all the room we want. That keeps the firmware
tiny and deterministic.

Binary format (little-endian), version 2
----------------------------------------
  magic      : 4 bytes  = b'FOXI'
  version    : u16      = 2
  n_codes    : u16
  strtab_off : u32      (offset of string table from file start)
  strtab_len : u32
  --- code table, n_codes entries, each 16 bytes ---------------------------
    name_off : u32      (byte offset into string table, NUL-terminated)
    category : u8       (0 tv, 1 audio, 2 projector, 3 ac, 4 misc)
    button   : u8       (0 power,1 vol_up,2 vol_dn,3 mute,4 ch_up,5 ch_dn,
                         6 input,7 play,8 pause,9 other)
    freq_div : u8       (carrier: freq = freq_div * 250 Hz; 0 => 38000)
    flags    : u8       (bit0 = code is in the power-sweep pool)
    data_off : u32      (offset into data blob of first u16 timing)
    n_pairs  : u16      (number of on/off pairs; timings are u16 count = 2*n_pairs)
    _pad     : u16
  --- data blob : u16 timings in microseconds, clamped to 65535 -------------
  --- string table : NUL-terminated names ----------------------------------

The firmware treats the whole thing as read-only mmap; no parsing, no malloc.
"""
import sys, os, re, glob, struct, argparse, hashlib

# ---- carrier defaults per protocol (Hz) -------------------------------------
PROTO_FREQ = {
    "NEC": 38000, "NECext": 38000, "NEC42": 38000, "NEC42ext": 38000,
    "Samsung32": 38000, "RC5": 36000, "RC5X": 36000, "RC6": 36000,
    "SIRC": 40000, "SIRC15": 40000, "SIRC20": 40000, "Kaseikyo": 37000,
    "Pioneer": 40000, "RCA": 56000,
}

CATEGORY = {"tv": 0, "audio": 1, "projector": 2, "ac": 3, "misc": 4}
BUTTON = {
    "power": 0, "vol_up": 1, "vol_dn": 2, "mute": 3, "ch_next": 4, "ch_prev": 5,
    "input": 6, "play": 7, "pause": 8, "other": 9,
}


def hex_le(s):
    """Flipper stores address/command as little-endian hex byte groups."""
    parts = s.strip().split()
    val = 0
    for i, b in enumerate(parts):
        val |= (int(b, 16) & 0xFF) << (8 * i)
    return val


# ---- protocol encoders: return list of (on_us, off_us) pairs ----------------
def bit_stream_lsb(value, nbits):
    return [(value >> i) & 1 for i in range(nbits)]


def enc_nec_like(addr_bits, cmd_bits, hdr=(9000, 4500), one=(560, 1690),
                 zero=(560, 560), stop=560):
    pulses = [hdr]
    for b in addr_bits + cmd_bits:
        pulses.append(one if b else zero)
    pulses.append((stop, 40000))
    return pulses


def enc_NEC(info):
    addr = hex_le(info.get("address", "00")) & 0xFF
    cmd = hex_le(info.get("command", "00")) & 0xFF
    frame = bit_stream_lsb(addr, 8) + bit_stream_lsb(addr ^ 0xFF, 8) + \
        bit_stream_lsb(cmd, 8) + bit_stream_lsb(cmd ^ 0xFF, 8)
    return enc_nec_like([], frame)


def enc_NECext(info):
    addr = hex_le(info.get("address", "0000")) & 0xFFFF
    cmd = hex_le(info.get("command", "0000")) & 0xFFFF
    frame = bit_stream_lsb(addr, 16) + bit_stream_lsb(cmd, 16)
    return enc_nec_like([], frame)


def enc_NEC42(info):
    addr = hex_le(info.get("address", "0000")) & 0x1FFF
    cmd = hex_le(info.get("command", "00")) & 0xFF
    frame = bit_stream_lsb(addr, 13) + bit_stream_lsb(addr ^ 0x1FFF, 13) + \
        bit_stream_lsb(cmd, 8) + bit_stream_lsb(cmd ^ 0xFF, 8)
    return enc_nec_like([], frame)


def enc_Samsung32(info):
    addr = hex_le(info.get("address", "00")) & 0xFF
    cmd = hex_le(info.get("command", "00")) & 0xFF
    frame = bit_stream_lsb(addr, 8) + bit_stream_lsb(addr, 8) + \
        bit_stream_lsb(cmd, 8) + bit_stream_lsb(cmd ^ 0xFF, 8)
    return enc_nec_like([], frame, hdr=(4500, 4500), one=(560, 1690),
                        zero=(560, 560))


def enc_Pioneer(info):
    return enc_NEC(info)  # Pioneer uses NEC-style framing here


def enc_RCA(info):
    addr = hex_le(info.get("address", "0")) & 0x0F
    cmd = hex_le(info.get("command", "00")) & 0xFF
    frame = bit_stream_lsb(addr, 4) + bit_stream_lsb(cmd, 8) + \
        bit_stream_lsb(addr ^ 0x0F, 4) + bit_stream_lsb(cmd ^ 0xFF, 8)
    return enc_nec_like([], frame, hdr=(4000, 4000), one=(500, 2000),
                        zero=(500, 1000), stop=500)


def enc_SIRC_n(info, nbits):
    cmd = hex_le(info.get("command", "00"))
    addr = hex_le(info.get("address", "00"))
    if nbits == 12:
        val = (cmd & 0x7F) | ((addr & 0x1F) << 7)
    elif nbits == 15:
        val = (cmd & 0x7F) | ((addr & 0xFF) << 7)
    else:  # 20
        val = (cmd & 0x7F) | ((addr & 0x1FFF) << 7)
    bits = bit_stream_lsb(val, nbits)
    pulses = [(2400, 600)]
    for b in bits:
        pulses.append((1200, 600) if b else (600, 600))
    pulses.append((600, 26000))
    return pulses


def enc_SIRC(info):
    return enc_SIRC_n(info, 12)


def enc_SIRC15(info):
    return enc_SIRC_n(info, 15)


def enc_SIRC20(info):
    return enc_SIRC_n(info, 20)


def _manchester(bits, half):
    """RC5/RC6 style: emit manchester as merged on/off pairs."""
    # Build a level sequence, then collapse to on/off durations.
    seq = []  # list of (level, duration)
    for b in bits:
        if b:  # rising: off then on
            seq.append((0, half)); seq.append((1, half))
        else:  # falling: on then off
            seq.append((1, half)); seq.append((0, half))
    # merge equal adjacent levels
    merged = []
    for lvl, dur in seq:
        if merged and merged[-1][0] == lvl:
            merged[-1][1] += dur
        else:
            merged.append([lvl, dur])
    # convert to on/off pairs, ensuring we start with an 'on'
    if merged and merged[0][0] == 0:
        merged.insert(0, [1, 0])  # zero-length on; harmless, filtered later
    pairs = []
    i = 0
    while i < len(merged):
        on = merged[i][1] if merged[i][0] == 1 else 0
        off = merged[i + 1][1] if i + 1 < len(merged) and merged[i + 1][0] == 0 else 0
        pairs.append((on, off))
        i += 2
    return [(a, b) for (a, b) in pairs if a or b]


def enc_RC5(info):
    addr = hex_le(info.get("address", "00")) & 0x1F
    cmd = hex_le(info.get("command", "00")) & 0x3F
    bits = [1, 1, 1] + bit_stream_msb(addr, 5) + bit_stream_msb(cmd, 6)
    p = _manchester(bits, 889)
    p.append((889, 89000))
    return p


def enc_RC5X(info):
    return enc_RC5(info)


def enc_RC6(info):
    addr = hex_le(info.get("address", "00")) & 0xFF
    cmd = hex_le(info.get("command", "00")) & 0xFF
    # simplified RC6 mode 0: leader + start bit + toggle + 8 addr + 8 cmd
    pulses = [(2666, 889)]
    bits = [1] + bit_stream_msb(addr, 8) + bit_stream_msb(cmd, 8)
    pulses += _manchester(bits, 444)
    pulses.append((444, 89000))
    return pulses


def enc_Kaseikyo(info):
    vendor = hex_le(info.get("address", "2002")) & 0xFFFF
    cmd = hex_le(info.get("command", "0000")) & 0xFFFF
    frame = bit_stream_lsb(vendor, 16) + bit_stream_lsb(cmd, 16) + \
        bit_stream_lsb(0, 16)
    pulses = [(3456, 1728)]
    for b in frame:
        pulses.append((432, 1296) if b else (432, 432))
    pulses.append((432, 40000))
    return pulses


def bit_stream_msb(value, nbits):
    return [(value >> (nbits - 1 - i)) & 1 for i in range(nbits)]


ENCODERS = {
    "NEC": enc_NEC, "NECext": enc_NECext, "NEC42": enc_NEC42,
    "Samsung32": enc_Samsung32, "SIRC": enc_SIRC, "SIRC15": enc_SIRC15,
    "SIRC20": enc_SIRC20, "RC5": enc_RC5, "RC5X": enc_RC5X, "RC6": enc_RC6,
    "Kaseikyo": enc_Kaseikyo, "Pioneer": enc_Pioneer, "RCA": enc_RCA,
}

# button name normalisation from Flipper naming conventions
BUTTON_ALIASES = {
    "power": "power", "pwr": "power", "poweroff": "power", "power_off": "power",
    "off": "power", "on": "power", "standby": "power",
    "vol_up": "vol_up", "volup": "vol_up", "vol+": "vol_up", "volume_up": "vol_up",
    "vol_dn": "vol_dn", "voldn": "vol_dn", "vol-": "vol_dn", "vol_down": "vol_dn",
    "volume_down": "vol_dn",
    "mute": "mute", "silence": "mute",
    "ch_next": "ch_next", "ch+": "ch_next", "chup": "ch_next", "channel_up": "ch_next",
    "ch_prev": "ch_prev", "ch-": "ch_prev", "chdn": "ch_prev", "channel_down": "ch_prev",
    "input": "input", "source": "input", "av": "input", "hdmi": "input",
    "play": "play", "pause": "pause",
}


def norm_button(name):
    key = re.sub(r"[^a-z0-9]+", "_", name.strip().lower()).strip("_")
    return BUTTON_ALIASES.get(key, "other")


def parse_ir_file(path, category):
    """Yield dicts: {name, button, category, freq, pulses}."""
    txt = open(path, encoding="utf-8", errors="ignore").read()
    # Split into records on lines that are a bare '#'
    records = re.split(r"\n#\s*\n", txt)
    for rec in records:
        info = {}
        for line in rec.splitlines():
            if ":" in line:
                k, _, v = line.partition(":")
                info[k.strip()] = v.strip()
        name = info.get("name")
        if not name:
            continue
        typ = info.get("type", "")
        if typ == "raw":
            m = info.get("data", "")
            if not m:
                continue
            nums = [int(x) for x in m.split()]
            if len(nums) < 2:
                continue
            if len(nums) % 2:
                nums = nums[:-1]
            freq = int(info.get("frequency", "38000") or "38000")
            pulses = list(zip(nums[0::2], nums[1::2]))
        elif typ == "parsed":
            proto = info.get("protocol", "")
            enc = ENCODERS.get(proto)
            if not enc:
                continue
            try:
                pulses = enc(info)
            except Exception:
                continue
            freq = PROTO_FREQ.get(proto, 38000)
        else:
            continue
        if not pulses:
            continue
        yield {
            "name": name,
            "button": norm_button(name),
            "category": category,
            "freq": freq,
            "pulses": pulses,
        }


def pulses_key(freq, pulses):
    """Dedup key: quantise timings to 50us buckets so near-identical
    codes collapse."""
    q = [(round(a / 50), round(b / 50)) for a, b in pulses]
    return (round(freq / 1000), tuple(q))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    ap.add_argument("--tv")
    ap.add_argument("--audio")
    ap.add_argument("--projector")
    ap.add_argument("--ac")
    ap.add_argument("--max-power", type=int, default=400,
                    help="cap on distinct POWER codes kept for the sweep")
    ap.add_argument("--max-named", type=int, default=60,
                    help="named non-power buttons kept per category")
    args = ap.parse_args()

    sources = []
    for cat, path in (("tv", args.tv), ("audio", args.audio),
                      ("projector", args.projector), ("ac", args.ac)):
        if path and os.path.exists(path):
            sources.append((cat, path))

    seen = set()
    power_codes = []
    named_codes = []
    named_count = {}

    for cat, path in sources:
        for c in parse_ir_file(path, cat):
            key = pulses_key(c["freq"], c["pulses"])
            if key in seen:
                continue
            seen.add(key)
            if c["button"] == "power":
                if len(power_codes) < args.max_power:
                    power_codes.append(c)
            else:
                nk = (cat, c["button"])
                if named_count.get(nk, 0) < args.max_named:
                    named_codes.append(c)
                    named_count[nk] = named_count.get(nk, 0) + 1

    codes = power_codes + named_codes
    if not codes:
        print("build_ir: no codes parsed!", file=sys.stderr)
        sys.exit(1)

    # ---- serialise ----------------------------------------------------------
    strtab = bytearray()
    str_off = {}

    def intern(s):
        if s in str_off:
            return str_off[s]
        off = len(strtab)
        strtab.extend(s.encode("utf-8", "ignore") + b"\x00")
        str_off[s] = off
        return off

    data = bytearray()
    table = bytearray()
    for c in codes:
        name_off = intern(c["name"][:31])
        # clamp timings to u16
        flat = []
        for a, b in c["pulses"]:
            flat.append(min(65535, max(1, int(a))))
            flat.append(min(65535, max(1, int(b))))
        n_pairs = len(flat) // 2
        data_off = len(data)
        for v in flat:
            data.extend(struct.pack("<H", v))
        freq_div = min(255, max(1, round(c["freq"] / 250)))
        flags = 1 if c["button"] == "power" else 0
        table.extend(struct.pack(
            "<IBBBBIHH",
            name_off,
            CATEGORY[c["category"]],
            BUTTON[c["button"]],
            freq_div,
            flags,
            data_off,
            n_pairs,
            0,
        ))

    header_size = 4 + 2 + 2 + 4 + 4
    table_off = header_size
    data_off_base = table_off + len(table)
    strtab_off = data_off_base + len(data)

    # fix up data_off in table entries to be absolute from file start
    fixed = bytearray()
    for i in range(len(codes)):
        entry = table[i * 16:(i + 1) * 16]
        name_off, cat, btn, fdiv, flags, doff, npair, pad = struct.unpack("<IBBBBIHH", entry)
        name_off_abs = strtab_off + name_off
        doff_abs = data_off_base + doff
        fixed.extend(struct.pack("<IBBBBIHH", name_off_abs, cat, btn, fdiv,
                                 flags, doff_abs, npair, pad))

    blob = bytearray()
    blob.extend(b"FOXI")
    blob.extend(struct.pack("<H", 2))
    blob.extend(struct.pack("<H", len(codes)))
    blob.extend(struct.pack("<I", strtab_off))
    blob.extend(struct.pack("<I", len(strtab)))
    blob.extend(fixed)
    blob.extend(data)
    blob.extend(strtab)

    with open(args.out, "wb") as f:
        f.write(blob)

    n_power = sum(1 for c in codes if c["button"] == "power")
    print(f"build_ir: {len(codes)} codes "
          f"({n_power} power / {len(codes) - n_power} named), "
          f"{len(blob)} bytes -> {args.out}")
    print(f"  sha256 {hashlib.sha256(blob).hexdigest()[:16]}")


if __name__ == "__main__":
    main()
