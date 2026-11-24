# TODO

- **Streak screen.** The top button (EXT, G5, the one spare control) toggles between Home and a Streak
  screen: current streak, longest streak, total days journaled. A day counts if it has any entry.
  Counted back from today using the RTC (BM8563), which the sync sets to the PC's local time
  (new `T <epoch> <utc-offset>` command). If today has no entry yet, show the streak up to yesterday
  instead of dropping to zero.
- **Obsidian "Sync to CoreInk" button** that runs `sync/sync_coreink.py` (desktop only).
- **Skip auto power-off on USB power**, so the device stays awake for syncing and flashing.
- **Resumable chunked flashing** in the repo (16 KB pieces, retried one at a time), for units whose
  USB-serial link corrupts bytes. That's how this unit was flashed.
