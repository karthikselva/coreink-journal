"""Sync an Obsidian daily journal to the M5Stack CoreInk over USB.

Reads every "DD-MM-YYYY - Journal.md" under the journal folder, keeps the days that have any
text, and sends one line per day (oldest first) to the CoreInk, which stores it on its own
flash and works fully offline afterwards:

    iso \\t date \\t weekday \\t title \\t areas \\t highlights \\t body

Title, highlights and life-area ratings come from the note's frontmatter (written by Journal
Scroll Assistant). A day without a title shows the start of its entry instead. Text is folded
to plain ASCII, which is all the device's fonts can draw.

Usage (PlatformIO's Python already has pyserial):
    %USERPROFILE%\\.platformio\\penv\\Scripts\\python.exe sync\\sync_coreink.py --vault "D:\\My Vault"
    ... --journal-dir "Daily Journal"   only search this folder inside the vault
    (or set COREINK_VAULT / COREINK_JOURNAL_DIR instead of passing these two)
    ... --port COM5           pick the port yourself
    ... --dry-run              build the data and report its size, don't send
    ... --out journal.tsv      also write the data to a file
"""

import argparse
import base64
import json
import os
import re
import sys
import time
import unicodedata
import zlib
from datetime import date, datetime
from pathlib import Path

ASSISTANT_SETTINGS = Path(".obsidian") / "plugins" / "journal-scroll-assistant" / "data.json"
DEFAULT_AREAS = ["Family", "Networking", "Health", "Career", "Hobbies", "Growth"]
LEVEL = {"focused": 3, "some": 2, "neglected": 1, "none": 0}
MONTHS = "Jan Feb Mar Apr May Jun Jul Aug Sep Oct Nov Dec".split()
WEEKDAYS = "Monday Tuesday Wednesday Thursday Friday Saturday Sunday".split()
NAME_RE = re.compile(r"^(\d{2})-(\d{2})-(\d{4})\b.*\.md$")
# USB-serial chips used on CoreInk units: Silicon Labs CP210x and WCH CH9102.
COREINK_USB = {(0x10C4, 0xEA60), (0x1A86, 0x55D4)}
BAUD = 115200

# ------------------------------------------------------------------ text

ASCII_MAP = {
    "\u2018": "'", "\u2019": "'", "\u201a": "'", "\u201c": '"', "\u201d": '"', "\u201e": '"',
    "\u2013": "-", "\u2014": "-", "\u2212": "-", "\u2026": "...", "\u00a0": " ", "\u2022": "-",
    "\u20b9": "Rs ", "\u00d7": "x", "\u2192": "->", "\u2190": "<-",
}


def to_ascii(s: str) -> str:
    s = "".join(ASCII_MAP.get(c, c) for c in s)
    s = unicodedata.normalize("NFKD", s)
    return "".join(c for c in s if 32 <= ord(c) < 127 or c in "\n\t")


def escape(s: str) -> str:
    return s.replace("\\", "\\\\").replace("\t", "\\t").replace("\r", "").replace("\n", "\\n")


def clean_body(md: str) -> str:
    """Journal markdown -> readable plain text with paragraphs and bullets kept."""
    md = re.sub(r"%% journal-scroll-assistant: comparisons %%[\s\S]*", "", md)  # AI comparisons
    md = re.sub(r"%%[\s\S]*?%%", "", md)                                         # comments
    md = re.sub(r"<!--[\s\S]*?-->", "", md)
    out = []
    for line in md.splitlines():
        s = line.strip()
        if re.fullmatch(r"!\[\[[^\]]*\]\]|!\[[^\]]*\]\([^)]*\)", s):  # image-only line
            continue
        if re.fullmatch(r"-{3,}|\*{3,}|_{3,}", s):                     # section rule
            out.append("")
            continue
        s = re.sub(r"!\[\[[^\]]*\]\]|!\[[^\]]*\]\([^)]*\)", "", s)     # inline images
        s = re.sub(r"\[\[([^\]|]*)\|([^\]]*)\]\]", r"\2", s)           # [[path|alias]] -> alias
        s = re.sub(r"\[\[([^\]]*)\]\]", lambda m: m.group(1).split("/")[-1].split("#")[0], s)
        s = re.sub(r"\[([^\]]+)\]\([^)]*\)", r"\1", s)                 # [text](url) -> text
        s = re.sub(r"^>\s*\[![^\]]*\][+-]?\s*", "", s)                 # callout header
        s = re.sub(r"^>\s?", "", s)
        heading = re.match(r"^#{1,6}\s+(.*)", s)
        if heading:
            out += ["", heading.group(1).upper()]
            continue
        s = re.sub(r"^[*+]\s+", "- ", s)
        s = re.sub(r"^\d+[.)]\s+", "- ", s)
        s = re.sub(r"^- \[[ xX]\]\s*", "- ", s)
        s = re.sub(r"(\*\*|__|\*|_|`|==|~~)", "", s)
        out.append(s)
    text = "\n".join(out)
    text = re.sub(r"\n{3,}", "\n\n", text).strip()
    return to_ascii(text)


def parse_frontmatter(text: str):
    """Just the keys we need (title, highlights, focus) from simple YAML frontmatter."""
    m = re.match(r"\A\ufeff?---\r?\n(.*?)\r?\n---\r?\n?", text, re.S)
    if not m:
        return {}, text
    fm, body = m.group(1), text[m.end():]
    data = {}
    t = re.search(r"^title:\s*(.+)$", fm, re.M)
    if t:
        data["title"] = t.group(1).strip().strip("'\"")
    h = re.search(r"^highlights:\s*\n((?:[ \t]+-.*\n?)*)", fm, re.M)
    if h:
        data["highlights"] = [x.strip().strip("'\"") for x in re.findall(r"^[ \t]+-\s+(.*)$", h.group(1), re.M)]
    f = re.search(r"^focus:\s*\n((?:[ \t]+\S.*\n?)*)", fm, re.M)
    if f:
        data["focus"] = dict(re.findall(r"^[ \t]+([^:\n]+):\s*(\S+)", f.group(1), re.M))
    return data, body


def first_words(text: str, n: int) -> str:
    s = re.sub(r"\s+", " ", text).strip()
    return s if len(s) <= n else s[: n].rsplit(" ", 1)[0] + "..."


def life_areas(vault: Path):
    try:
        areas = json.loads((vault / ASSISTANT_SETTINGS).read_text(encoding="utf-8")).get("areas", "")
        names = [a.strip() for a in areas.split(",") if a.strip()]
        return names or DEFAULT_AREAS
    except (OSError, ValueError):
        return DEFAULT_AREAS


def build_rows(vault: Path, journal: Path):
    areas = life_areas(vault)
    days = []
    for p in journal.rglob("*.md"):
        m = NAME_RE.match(p.name)
        if not m:
            continue
        d, mo, y = int(m.group(1)), int(m.group(2)), int(m.group(3))
        try:
            when = date(y, mo, d)
        except ValueError:
            continue  # typos like 13-13-2024
        fm, body_md = parse_frontmatter(p.read_text(encoding="utf-8", errors="replace"))
        body = clean_body(body_md)
        if not body:
            continue  # only days with an entry
        # No title yet: the start of the entry, skipping section headings like "PERSONAL".
        prose = "\n".join(l for l in body.splitlines() if l.strip() and not (l.isupper() and len(l) < 40))
        title = to_ascii(fm.get("title", "")).strip() or first_words(prose.lstrip("- "), 70)
        highlights = [to_ascii(h) for h in fm.get("highlights", []) if h.strip()]
        focus = {k.strip().lower(): LEVEL.get(v.strip().lower(), 0) for k, v in fm.get("focus", {}).items()}
        area_str = ",".join(f"{a[:3]}:{focus.get(a.lower(), 0)}" for a in areas) if focus else ""
        hl_text = "\n".join(f"- {h}" for h in highlights)
        days.append((when, [
            when.isoformat(),
            f"{when.day} {MONTHS[when.month - 1]} {when.year}",
            WEEKDAYS[when.weekday()],
            escape(title),
            area_str,
            escape(hl_text),
            escape(body),
        ]))
    days.sort(key=lambda x: x[0])
    # Two files for one date (conflict copies): keep the longer one.
    unique = {}
    for when, row in days:
        if when not in unique or len(row[6]) > len(unique[when][6]):
            unique[when] = row
    return [unique[k] for k in sorted(unique)]


# ------------------------------------------------------------------ serial


def find_port():
    from serial.tools import list_ports
    hits = [p for p in list_ports.comports() if (p.vid, p.pid) in COREINK_USB]
    if not hits:
        names = ", ".join(f"{p.device} ({p.description})" for p in list_ports.comports()) or "none"
        sys.exit(f"No CoreInk found. Plug it in with a data USB-C cable. Ports seen: {names}")
    if len(hits) > 1:
        sys.exit("Several possible CoreInks: " + ", ".join(p.device for p in hits) + ". Pick one with --port.")
    return hits[0].device


def read_line(ser, timeout):
    """One line from the device, or None on timeout. Garbled bytes come through as-is."""
    end = time.time() + timeout
    buf = b""
    while time.time() < end:
        c = ser.read(1)
        if not c:
            continue
        if c == b"\n":
            return buf.decode("ascii", "replace").strip()
        buf += c
    return None


def ask(ser, line: str, ok: re.Pattern, timeout=1.5, tries=12):
    """Send a command until a reply matching `ok` comes back. The link corrupts bytes now and then,
    and every command is safe to resend, so anything unclear just means: send it again."""
    for _ in range(tries):
        ser.reset_input_buffer()
        ser.write((line + "\n").encode("ascii"))
        end = time.time() + timeout
        while time.time() < end:
            reply = read_line(ser, end - time.time())
            if reply is None:
                break
            m = ok.fullmatch(reply)
            if m:
                return m
            if reply.startswith("F "):
                return reply  # a definite failure, not noise
    return None


def set_clock(ser):
    """Set the CoreInk's RTC to this PC's local time, so the streak screen knows what "today" is."""
    now = int(time.time())
    offset = int(datetime.now().astimezone().utcoffset().total_seconds())
    signed = f"{now} {offset}"
    r = ask(ser, f"T {signed} {zlib.crc32(signed.encode()) & 0xFFFFFFFF:08x}", re.compile(r"KT"), tries=5)
    if r and not isinstance(r, str):
        print("Clock set.")
    else:
        print("Couldn't set the clock (older firmware?); the streak screen will count from the latest entry.")


def send(port, data: bytes, chunk=768):
    import serial
    ser = serial.Serial()
    ser.port, ser.baudrate, ser.timeout = port, BAUD, 0.05
    # Don't toggle DTR/RTS on open: that resets the ESP32, and a reset on the CoreInk releases its
    # power latch, switching it off.
    ser.dtr = False
    ser.rts = False
    ser.open()
    try:
        pong = ask(ser, "PING", re.compile(r"PONG (\d+)"), tries=20)
        if not pong or isinstance(pong, str):
            sys.exit(f"The CoreInk on {port} didn't answer. Is it switched on and running the journal firmware?")
        print(f"Connected on {port}: the CoreInk has {pong.group(1)} days now.")
        set_clock(ser)
        t0 = time.time()
        crc = zlib.crc32(data) & 0xFFFFFFFF
        for attempt in range(3):
            r = ask(ser, f"B {len(data)}", re.compile(r"KB"))
            if not r or isinstance(r, str):
                sys.exit(f"Couldn't start the upload: {r or 'no answer'}")
            retries = 0
            for off in range(0, len(data), chunk):
                piece = data[off: off + chunk]
                line = f"W {off} {zlib.crc32(piece) & 0xFFFFFFFF:08x} {base64.b64encode(piece).decode()}"
                m = ask(ser, line, re.compile(rf"K {off}"), timeout=1.0, tries=25)
                if not m or isinstance(m, str):
                    sys.exit(f"Gave up at byte {off}: the link is too noisy right now. Try again.")
                done = min(off + chunk, len(data))
                print(f"\r  sent {done // 1024} / {len(data) // 1024} KB", end="", flush=True)
            print()
            r = ask(ser, f"E {len(data)} {crc:08x}", re.compile(r"D (\d+)"), timeout=4)
            if r and not isinstance(r, str):
                print(f"Done in {time.time() - t0:.0f}s: the CoreInk now has {r.group(1)} days.")
                return
            print(f"Final check failed ({r or 'no answer'}); sending again.")
        sys.exit("The final check kept failing. Nothing was changed on the CoreInk.")
    finally:
        ser.close()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--vault", default=os.environ.get("COREINK_VAULT"),
                    help="path to the Obsidian vault (or set COREINK_VAULT)")
    ap.add_argument("--journal-dir", default=os.environ.get("COREINK_JOURNAL_DIR", ""),
                    help="journal folder inside the vault (default: search the whole vault)")
    ap.add_argument("--port", help="serial port, e.g. COM5 (default: find the CoreInk)")
    ap.add_argument("--dry-run", action="store_true", help="build the data and report, don't send")
    ap.add_argument("--out", help="also write the data to this file")
    args = ap.parse_args()
    if not args.vault:
        ap.error("give --vault or set COREINK_VAULT")
    vault = Path(args.vault)
    journal = vault / args.journal_dir
    if not journal.is_dir():
        sys.exit(f"Journal folder not found: {journal}")

    rows = build_rows(vault, journal)
    data = ("\n".join("\t".join(r) for r in rows) + "\n").encode("ascii")
    titled = sum(1 for r in rows if r[4])
    print(f"{len(rows)} days ({titled} with ratings), {len(data) / 1024:.0f} KB, "
          f"{rows[0][1] if rows else '-'} to {rows[-1][1] if rows else '-'}")
    if args.out:
        Path(args.out).write_bytes(data)
        print(f"Wrote {args.out}")
    if args.dry_run:
        return
    send(args.port or find_port(), data)


if __name__ == "__main__":
    main()
