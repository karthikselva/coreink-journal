# CoreInk Journal

Your Obsidian daily journal on an **M5Stack CoreInk** (ESP32, 200×200 e-ink), fully offline.
Plug it into the PC, run the sync, unplug, read. Works best with notes written by the
[Journal Scroll Assistant](#where-the-data-comes-from) plugin, which adds a title, highlights and life-area ratings to each day.

## Controls

| Screen | Shows | Wheel up / down | Wheel press | Side (power) button |
| --- | --- | --- | --- | --- |
| Home | weekday, date, title, six life-area meters | previous / next day | → Summary | (already home) |
| Summary | the day's highlights | page | → Entry | → Home |
| Entry | the whole journal entry | page | next page | → Home |

- Opens on the most recent day.
- After 2 minutes without input it powers off. The e-ink keeps the last screen, and the side button turns it back on.
- Life-area meters: 3 bars focused, 2 some, 1 neglected, 0 not today.

## Build and flash (PlatformIO)

```
pio run -t upload
```

## Sync the journal

Uses PlatformIO's bundled Python, which already has `pyserial`:

```
%USERPROFILE%\.platformio\penv\Scripts\python.exe sync\sync_coreink.py --vault "D:\My Vault" --journal-dir "Daily Journal"
```

- `--vault` is the vault folder; `--journal-dir` (optional) narrows the search to one folder inside it. Set `COREINK_VAULT` and `COREINK_JOURNAL_DIR` to skip typing them each time.

- `--dry-run` builds the data and reports its size without sending.
- `--port COMx` picks the port yourself. By default it finds the CoreInk's USB-serial chip (CP210x or CH9102).

### Where the data comes from

The script reads every `DD-MM-YYYY - Journal.md` under the journal folder, keeps the days with any text, and sends one line per day. Title, highlights and ratings come from frontmatter written by Journal Scroll Assistant (life-area names are read from its settings, falling back to Family, Networking, Health, Career, Hobbies, Growth); a day without a title shows the start of its entry. Text is folded to ASCII, which is all the device fonts can draw. AI comparison callouts and images are left out.

## Storage

The 4 MB flash uses `default.csv`: two app slots and a ~1.4 MB LittleFS data partition. The whole journal (title, highlights, ratings and full text for ~470 days) is ~340 KB, so there is room for years more.

## Wire format

`/journal.tsv`, oldest first, one day per line:

```
iso \t date \t weekday \t title \t areas \t highlights \t body
```

`areas` is `Fam:3,Net:2,...`. Highlights and body escape `\\`, `\t` and `\n`. The serial protocol is documented at the top of `src/main.cpp`.

## Flashing from another computer (no install)

`coreink-journal-full.bin` (attached to each [GitHub release](../../releases)) is the whole firmware in one image (bootloader, partition table and app).
In Chrome or Edge, open Espressif's web flasher at https://espressif.github.io/esptool-js/, click **Connect**,
pick the CoreInk's port, add the file at offset **0x0** and click **Program**.
Rebuild it after code changes with `esptool.py merge_bin` or just use `pio run -t upload`.

## Troubleshooting

If flashing fails with "Invalid head of packet (0xE0)", "Serial data stream stopped" or
"The chip stopped responding", bytes are being corrupted on the USB-serial link. Try a lower
`upload_speed`, a different cable or USB port (preferably one directly on the PC, not a hub),
and the latest CP210x / CH9102 driver. The web flasher above is a handy cross-check from another computer.

What finally worked on a unit with a noisy link: **hold the side power button for the whole upload**.
While flashing, no firmware is running to hold the CoreInk's power latch (G12), so the chip can lose
power mid-transfer ("The chip stopped responding"). Writing the image in small pieces
(`esptool.py --no-stub write_flash <offset> <16 KB piece>`), retrying each piece until it verifies,
gets through even when whole uploads keep failing.

The sync itself is built for such a link: 768-byte base64 chunks, each with its own CRC-32 and
resent until acknowledged, then a whole-file CRC before the new journal replaces the old one.

## License

MIT, see [LICENSE](LICENSE).
