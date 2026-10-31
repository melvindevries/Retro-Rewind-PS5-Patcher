<p align="center"><img src="docs/img/banner.png" alt="MKWII Retro Rewind Patcher" width="100%"></p>

<p align="center">
  <a href="../../releases/latest"><img src="https://img.shields.io/github/v/release/melvindevries/Retro-Rewind-PS5-Patcher?label=release" alt="release"></a>
  <img src="https://img.shields.io/badge/platform-PS5%20homebrew-003791" alt="PS5 homebrew">
  <a href="../../actions/workflows/build.yml"><img src="../../actions/workflows/build.yml/badge.svg" alt="build"></a>
  <img src="https://img.shields.io/badge/license-GPL--3.0--or--later-blue" alt="license">
</p>

**Update Retro Rewind and build your patched Mario Kart Wii image, entirely on your PS5, with a controller.**

[Download](#install) · [Setup](#install) · [Usage](#usage) · [Troubleshooting](#troubleshooting) · [Build from source](#build-from-source) · [Credits](#credits) · [Donate](#support-the-project)

## What it is
A native PS5 app. It checks the official [Retro Rewind](https://rwfc.net) update server, installs the newest update into your
pack, and builds a patched Mario Kart Wii `.wbfs` from **your own** copy of the game. No PC needed.

## Features
- **Install Full Game:** no Retro Rewind pack on the console yet? One button downloads the complete official pack, creates every folder it needs and puts the files in the right place. The button only shows while no pack is installed.
- **Updates:** checks the official server and installs only the newest update, merged into your pack (files replaced or added, nothing else touched).
- **Patching:** builds `RMCETO` (USA), `RMCPTO` (PAL) or `RMCJTO` (Japan) after you pick the region.
- **Safe:** your original image is only ever read. A failed patch never overwrites a good output (temporary file, full verify, then atomic rename).
- **Verify:** re-check the finished image from the menu.
- **Internal storage only:** everything lives in `/data/porpoise/...`, nothing on USB.
- **Any file name:** any `.wbfs` in the base folder is accepted.
- **Diagnostic log:** shows exactly what failed and why.

## Requirements
- A jailbroken PS5 running [ShadowMountPlus](https://github.com/drakmor/ShadowMountPlus) and OnionHEN.
- Your own legally obtained Mario Kart Wii image as a `.wbfs` file.
- The official Retro Rewind ISO Builder files and your Retro Rewind pack (`RetroRewind6`) from [rwfc.net](https://rwfc.net).
- An FTP client (or another way to copy files to the console) and an internet connection for updates.
- Free internal space of roughly one game image plus the update ZIP.

## Install
1. **Download** `PPSA97531.zip` from the [latest release](../../releases/latest). Optionally check it against `SHA256SUMS`.
2. **Unzip** it. You get a folder `PPSA97531` containing `eboot.bin`, `sce_module/` and `sce_sys/`.
3. **Copy** that whole folder to `/data/homebrew/PPSA97531/` on the console (FTP). Wait about 15 seconds; ShadowMountPlus registers it and the app shows up on the home screen.
4. **Allow the app in OnionHEN.** Without this the app cannot read or write `/data`. Add the title ID `PPSA97531` in
   **both** `/data/OnionHEN/config.ini` and `/user/data/OnionHEN/config.ini`:
   ```
   [app_jailbreak]
   enabled=true
   exact_title_ids=ITEM00001,NPXS39041,PKGI13337,PKGI12345,TOOL00001,PPSA97531
   ```
   Keep the IDs already in your list and append `,PPSA97531`. Then restart OnionHEN.
5. **Folders and permissions.** Start the app once so it creates `/data/porpoise`, or create it yourself. Then set `/data/porpoise`
   and everything in it to **777** (recursively) in your FTP client.
6. **Copy your files:**

   | What | Where |
   |---|---|
   | Your Mario Kart Wii image (one `.wbfs`, any name) | `/data/porpoise/rewind/base/` |
   | Retro Rewind pack (only if you already have one; otherwise the app installs it for you with **Install Full Game**) | `/data/porpoise/rewind/pack/RetroRewind6/` |
   | Retro Rewind ISO Builder files (`copy-files.sh`, `create-rr-*.sh`, `extra/`, `Patches/`, ...) | `/data/porpoise/rewind/builder/` |

7. **Start the app.** If the Access row does not show OK, press Square to retry.

## Usage
1. **No Retro Rewind pack yet?** Choose **Install Full Game**. The app downloads the complete pack from `update.rwfc.net`, creates the folders and installs it. The button disappears once the pack is installed.
2. **Already have a pack?** **Check for Retro Rewind updates**, then **Install updates**.
3. Choose your **region** with Left/Right (USA, PAL or Japan). It must match your base image.
4. **Patch game.** The result is `RMCETO.wbfs` (USA), `RMCPTO.wbfs` (PAL) or `RMCJTO.wbfs` (Japan) in `/data/porpoise/games/`.
5. Optionally **Verify output**, then start the game from your game launcher.

## Controls
| Button | Action |
|---|---|
| **Up / Down** | Select menu item |
| **Cross** | Activate |
| **Left / Right** | Change region |
| **Circle** | Cancel the running job |
| **Square** | Refresh status / retry access |
| **Options** | Open the log |

## Where things are kept
| Path | What |
|---|---|
| `/data/homebrew/PPSA97531/` | The app |
| `/data/porpoise/rewind/base/` | Your base game image |
| `/data/porpoise/rewind/pack/RetroRewind6/` | Retro Rewind pack (`version.txt` inside); created by Install Full Game |
| `/data/porpoise/rewind/builder/` | Builder scripts and assets |
| `/data/porpoise/games/` | Output (`RMCETO` / `RMCPTO` / `RMCJTO.wbfs`) |
| `/data/porpoise/rewind/state/` | Region choice and patch fingerprint |
| `/data/porpoise/rewind/logs/porpoise.log` | Log file |

## Troubleshooting
- **Cannot open the base folder / no game found.** The app is not allowed in OnionHEN yet (step 4), or the folder is empty or has no `.wbfs`.
- **"Cannot create working folders".** Folder permissions (step 5).
- **"Cannot read builder script".** The builder files are missing from `/data/porpoise/rewind/builder/` (step 6).
- **Image not recognised.** Rename it `RMCE01.wbfs`, `RMCP01.wbfs` or `RMCJ01.wbfs`.
- **"Retro Rewind is not installed yet" when checking for updates.** Use **Install Full Game** first.
- **Install Full Game fails.** Check the Access row and free space (the full pack is large), then press Options for the log.
- **Anything else.** Press Options for the log, or read `/data/porpoise/rewind/logs/porpoise.log`, and include it in an issue.

## Known limitations
- Only the newest update archive is installed. The server publishes one archive per release containing that release's changes, so if your pack is several releases behind, files that only older archives carried can be missing. In that case reinstall the pack from the official full download first.
- Delete lists inside update archives are not applied (merge only).
- The app needs the OnionHEN allowlist entry; it cannot work in a plain sandbox.
- The app does not download your game or the ISO Builder files (`copy-files.sh`, `create-rr-*.sh`, `extra/`, `Patches/`). It only downloads Retro Rewind archives from `update.rwfc.net`.
- Install Full Game is offered only while no pack is installed. To reinstall, delete `/data/porpoise/rewind/pack/RetroRewind6/` first.

## Build from source
See [docs/BUILD.md](docs/BUILD.md). Deploy details: [docs/DEPLOY.md](docs/DEPLOY.md). Host tests: `tests/run_all.sh`.
Releases are built by GitHub Actions when a version tag such as `01.000.000` is pushed.

## Support the project
This is free and open source. If it saved you time and you want to say thanks, you can donate:

| Network | Address |
|---|---|
| **Solana (SOL)** | `CzbwBTYjgYPbny5zdjVJKrrtySVWdUvSWYYtC2x9qSB` |
| **Ethereum (ETH)** | `0x8146E8855096C19BaF164FFF88326d5062863C6f` |

Always copy the address from this page. Never send funds because of a message or comment from someone else.

## Credits
Built on [ps5-native-app-boilerplate](https://github.com/BlackBearReloaded/ps5-native-app-boilerplate) and the ps5-payload-sdk,
with [miniz](https://github.com/richgel999/miniz) and the DejaVu font. [Wiimm's ISO Tools](https://wit.wiimm.de) was used only as a
test reference on a PC. [Retro Rewind](https://rwfc.net) is the work of its own team. Full list: [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## Legal
**Important:** this project ships no game data, no Sony files and no Retro Rewind files. You must own Mario Kart Wii and supply the image yourself.
It is not meant for piracy and does not help obtain games.

Not affiliated with Sony, Nintendo or the Retro Rewind team. "PlayStation" and "PS5" are trademarks of Sony Interactive
Entertainment. Mario Kart Wii is a trademark of Nintendo. Use at your own risk.

## License
GPL-3.0-or-later. See [LICENSE](LICENSE).
