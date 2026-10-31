# Changelog

## 01.000.000
First public release.
- Controller UI: check/install Retro Rewind updates, choose region, patch, verify, diagnostic log.
- Updates download only the newest release archive and merge it into the existing pack (replace matching files, add new, keep the rest).
- Patch engine builds a verified WBFS (`RMCETO` / `RMCPTO` / `RMCJTO`) on the console; the base image is only read.
- Patch engine and updater covered by the host test suite.
