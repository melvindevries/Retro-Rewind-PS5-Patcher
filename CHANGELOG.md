# Changelog

## 01.000.003
- New look: Retro Rewind logo and disc artwork in the header, a rainbow checker background, rounded panels, a gradient progress bar.
- Controls at the bottom of the screen are shown as PlayStation button symbols instead of words.
- No change to patching or updating.

## 01.000.002
- Fix: "FATAL ERROR: Incompatible file (version 2), please upgrade your Kamek Loader." The builder tools are replaced with the newest official Retro Rewind builder (v6.13.4 and above, fixed2), which contains the Kamek loaders released on 30 October. The old builder files are removed from the repository (they remain in the git history only).
- **Check for updates** also reads `builder/REVISION` in this repository and downloads the builder again when a newer one is published there, so a new official builder reaches everyone without a new app version. Builders installed by an earlier app version are refreshed the same way. 
- **Patch game** now downloads the builder by itself when it is missing or outdated (no extra button needed), and falls back to the installed one if only the newest-version check fails.
- The patch engine understands the new builder scripts (the Riivolution-only steps are skipped, as they are not part of the disc image).

## 01.000.001
- Fix: patching a pack installed with Install Full Game failed with "script source ... Language/JPN/Font/Font.szs does not exist".
- Patching no longer stops when the pack lacks optional language files (`Language/*/Font/...`). Like the official builder script, they are skipped, left as in the original game, and listed in the log. Missing core files (e.g. `Binaries/Code.pul`) still stop the patch.
- Install Full Game logs which folders the downloaded pack contains and which archive entries were skipped.

## 01.000.000
First public release.
- **Install Full Game** button, shown only while no Retro Rewind pack is installed. First installs the builder tools from this repository (`builder/`, the unmodified official Retro Rewind builder files, credited in `builder/NOTICE.md`), then downloads `https://update.rwfc.net/RetroRewind/zip/RetroRewind.zip`. All missing folders are created and the files are placed automatically. Same safe handling as updates (staging, CRC checks, rollback; a failed install leaves nothing half-installed).
- Works with the current official builder scripts (which name the PAL loader folder `PAL` but ship it as `EUR`).
- The Install Full Game entry is always shown and greyed out while a pack is installed.
- Controller UI: check/install Retro Rewind updates, choose region, patch, verify, diagnostic log.
- Updates download only the newest release archive and merge it into the existing pack (replace matching files, add new, keep the rest).
- Patch engine builds a verified WBFS (`RMCETO` / `RMCPTO` / `RMCJTO`) on the console; the base image is only read.
- Patch engine and updater covered by the host test suite.
