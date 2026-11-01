# Changelog

## 01.000.000
First public release.
- **Install Full Game** button, shown only while no Retro Rewind pack is installed. First installs the builder tools from this repository (`builder/`, the unmodified official Retro Rewind builder files, credited in `builder/NOTICE.md`), then downloads `https://update.rwfc.net/RetroRewind/zip/RetroRewind.zip`. All missing folders are created and the files are placed automatically. Same safe handling as updates (staging, CRC checks, rollback; a failed install leaves nothing half-installed).
- Works with the current official builder scripts (which name the PAL loader folder `PAL` but ship it as `EUR`).
- Patching no longer stops when the pack lacks optional language files (`Language/*/Font/...`). Like the official builder script, they are skipped, left as in the original game, and listed in the log. Missing core files (e.g. `Binaries/Code.pul`) still stop the patch.
- The Install Full Game entry is always shown and greyed out while a pack is installed.
- Controller UI: check/install Retro Rewind updates, choose region, patch, verify, diagnostic log.
- Updates download only the newest release archive and merge it into the existing pack (replace matching files, add new, keep the rest).
- Patch engine builds a verified WBFS (`RMCETO` / `RMCPTO` / `RMCJTO`) on the console; the base image is only read.
- Patch engine and updater covered by the host test suite.
