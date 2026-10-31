# Builder files

`RetroRewindBuilder.zip` holds the Retro Rewind ISO builder files that the app installs for you when you choose
**Install Full Game**:

- `copy-files.sh`, `create-rr-usa.sh`, `create-rr-pal.sh`, `create-rr-jpn.sh`
- `extra/` (loaders, strap, save banner, THP files, `wiivcpatch.xml`)
- `Patches/`

They are the **unmodified** files from the official Retro Rewind Team repository
[Retro-Rewind-Team/iso-patcher](https://github.com/Retro-Rewind-Team/iso-patcher) (`updatedbuilder.zip`, commit `8258c71`, 2026-06-03),
reduced to the files this app needs. The `.bat` files, `ddel.exe`, `check_update.ps1`, `create-all.*`, `set-image-type-*` and the
readmes of the original archive are left out because the PS5 app does not use them.

Credits and licence: see [NOTICE.md](NOTICE.md). The original files are licensed under GPL-3.0 (see [LICENSE](LICENSE)).

To update them: replace the zip with the same selection from a newer `updatedbuilder.zip`, bump the commit above, and run the test suite.
