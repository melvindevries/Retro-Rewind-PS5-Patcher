# Builder files

`RetroRewindBuilder.zip` holds the Retro Rewind ISO builder files that the app installs for you when you choose
**Install Full Game**:

- `copy-files.sh`, `create-rr-usa.sh`, `create-rr-pal.sh`, `create-rr-jpn.sh`
- `extra/` (Kamek loaders, strap, save banner, THP files)
- `Patches/`

They are the **unmodified** files from the official Retro Rewind builder archive
"Retro Rewind ISO Builder (Wiimm Edition) for v6.13.4 and above, fixed2" (`Retro_Rewind_ISO_Builder_Wiimm_Edition_for_v6.13.4_and_above_fixed2.zip`, scripts dated 2026-10-29),
reduced to the files this app needs (it includes the Kamek loaders released on 30 October 2026). The `.bat` files, `ddel.exe`, `create-all.*`, `set-image-type-*` and the
readmes of the original archive are left out because the PS5 app does not use them.

Credits and licence: see [NOTICE.md](NOTICE.md). The original files are licensed under GPL-3.0 (see [LICENSE](LICENSE)).

To publish a newer builder: replace the zip with the same selection from the newer official archive and put its date (ISO, e.g. `2026-10-29`) in `REVISION`. Installed apps read `REVISION` on **Check for updates** and download the zip again when it is newer than theirs. Only raise `RR_BUILDER_REV` in `core/rr_update.h` when an app version must refuse older builders.
