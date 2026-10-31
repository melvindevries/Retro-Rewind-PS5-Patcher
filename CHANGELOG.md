# Changelog

## 01.000.000
First public release.
- **Install Full Game** button, shown only while no Retro Rewind pack is installed. First installs the official ISO builder files (only `copy-files.sh`, `create-rr-*.sh`, `extra/` and `Patches/`; the rest of the archive is not written), then downloads `https://update.rwfc.net/RetroRewind/zip/RetroRewind.zip`. All missing folders are created and the files are placed automatically. Same safe handling as updates (staging, CRC checks, rollback; a failed install leaves nothing half-installed).
- Works with the current official builder (`updatedbuilder.zip`, which names the PAL loader folder `PAL` in its script but ships it as `EUR`).
- Controller UI: check/install Retro Rewind updates, choose region, patch, verify, diagnostic log.
- Updates download only the newest release archive and merge it into the existing pack (replace matching files, add new, keep the rest).
- Patch engine builds a verified WBFS (`RMCETO` / `RMCPTO` / `RMCJTO`) on the console; the base image is only read.
- Patch engine and updater covered by the host test suite.
