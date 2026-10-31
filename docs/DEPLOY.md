# Deploy
1. Console must run a homebrew loader with ShadowMountPlus (it registers title folders under `/data/homebrew/`).
2. Copy `dist/PPSA97531/` to `/data/homebrew/PPSA97531/` (FTP or similar). Wait ~15 s for ShadowMountPlus to register it; the
   app then appears on the home screen.
3. Create and fill (the app creates the empty folders on first start):
   - `/data/porpoise/rewind/base/` - exactly one `.wbfs` of your own Mario Kart Wii (RMCE01 / RMCP01 / RMCJ01)
   - `/data/porpoise/rewind/pack/RetroRewind6/` - your existing pack incl. `version.txt` (the app can update it). Without a pack, use **Install Full Game** in the app; it also installs the builder files
   - `/data/porpoise/rewind/builder/` - (installed by Install Full Game if missing) contents of the Retro Rewind ISO Builder (`copy-files.sh`, `create-rr-*.sh`, `extra/`, `Patches/`, ...)
4. Launch, Check updates, Install updates, choose region, Patch game. Output appears in `/data/porpoise/games/`.
Needs free space of roughly the size of the output plus the update ZIPs. The app is network-dependent only for updates.

## Jailbreak access to /data (first launch)
A PPSA title starts sandboxed. On launch the app checks whether it can write `/data/porpoise`. If not, it asks the
jailbreak environment to elevate it, then waits up to ~40 s and shows the result in the "Access" row.
It publishes three small marker files in its own `/download0` (harmless if nothing listens):
- `onionhen_jailbreak` and `etahen_jailbreak` - the OnionHEN / etaHEN app-jailbreak trigger
- `elevate_proc` with `{"PID":n}` after `seteuid(geteuid())` - the PS5-Lapy-JB-Daemon request

OnionHEN only elevates **allowlisted** titles, and its default list is `ITEM00001, NPXS39041, PKGI13337, PKGI12345,
TOOL00001` plus the prefix `LAPY`. Our title is `PPSA97531`, so add it once to `/data/OnionHEN/config.ini`
(OnionHEN keeps a second copy in `/user/data/OnionHEN/config.ini`; edit both, then restart OnionHEN):
```
[app_jailbreak]
enabled=true
exact_title_ids=ITEM00001,NPXS39041,PKGI13337,PKGI12345,TOOL00001,PPSA97531
```
Optional: `debug_notifications=true` shows a notification when OnionHEN jailbreaks the app.
After that it is automatic on every launch. Square on the main screen retries the request; Options shows the log with
the exact errno of any failing path.

## Permissions on your folders
The app can only write where its own account may write. Folders you create over FTP are usually owned by root with
mode 755, so reading works but "Install updates" fails with "cannot create working folders". The Access row names the
folder and the error. Fix: set `/data/porpoise` to 777 recursively in your FTP client, or delete `/data/porpoise` and
start the app once so it creates the tree itself, then copy your files in.

## Updates: latest only
"Install updates" downloads only the archives of the newest Retro Rewind release and skips releases in between.
The server publishes one archive per release containing that release's changes, so a pack that is several releases
behind may miss files that only older archives carried. If something is missing in-game, use the official full install.
