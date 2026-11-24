# TODO

- **Flash and test the battery-saving build.** It's in the code (power off after 10 s, CPU at 80 MHz,
  resume on the same screen / day / page / streak cursor via NVS) but not on the device yet: flashing
  failed on the noisy USB link. Flash while holding the side power button, then check: it switches off
  after 10 s, the side button brings back the same screen, and after a sync it opens on the newest day.
- **Wi-Fi sync from an Obsidian plugin (iPad / phone / desktop).** iOS plugins can't use Bluetooth or
  USB serial, only HTTP, so: a sync mode on the device that turns Wi-Fi on (joins the home network as
  `coreink.local`, or opens its own hotspot) and runs a small HTTP server; an Obsidian plugin that builds
  `journal.tsv` in JavaScript (a port of `sync_coreink.py`), uploads it in checksummed chunks, and sets the
  clock. Same whole-file CRC check before the new journal replaces the old one. Wi-Fi only on during a
  sync, to save the battery. This would also cover the "Sync to CoreInk" button below.
- **Obsidian "Sync to CoreInk" button** that runs `sync/sync_coreink.py` (desktop only).
- **Skip auto power-off on USB power**, so the device stays awake for syncing and flashing.
- **Resumable chunked flashing** in the repo (16 KB pieces, retried one at a time), for units whose
  USB-serial link corrupts bytes. That's how this unit was flashed.
