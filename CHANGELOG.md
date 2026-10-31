# Changelog

## 01.001.000
- New **Install Full Game** button, shown only while no Retro Rewind pack is installed. Downloads `https://update.rwfc.net/RetroRewind/zip/RetroRewind.zip`, creates all missing folders and places the files in `/data/porpoise/rewind/pack/RetroRewind6/`. Same safe handling as updates (staging, CRC checks, journal, rollback; a failed install leaves nothing half-installed).
- Check/Install updates now say clearly when no pack is installed.

## 01.000.000
First public release.
- Controller UI: check/install Retro Rewind updates, choose region, patch, verify, diagnostic log.
- Updates download only the newest release archive and merge it into the existing pack (replace matching files, add new, keep the rest).
- Patch engine builds a verified WBFS (`RMCETO` / `RMCPTO` / `RMCJTO`) on the console; the base image is only read.
- Patch engine and updater covered by the host test suite.
