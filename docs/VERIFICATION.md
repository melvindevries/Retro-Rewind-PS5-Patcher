# What is verified, and what is not
| Level | Status |
|---|---|
| Code compiles (host, gcc/clang, -Wall -Wextra) | YES |
| Cross-compiles/links for PS5 with prospero toolchain; signed FSELF integrity valid (`ps5-native-tool self --inspect`) | YES |
| Title folder structure + param.json validated by the build's checks (titleId/contentId/conceptId consistent) | YES |
| Every imported libc/kernel/pthread symbol exists in an SDK stub library | YES |
| Patch engine vs real WIT on host: byte-identical WBFS for USA/PAL/Japan synthetic discs; independent verifier; corruption, wrong region, multiple images, unsafe paths, cancel/failed-patch-keeps-old-output cases | YES (27/27 per region) |
| Update logic against a mock server: sequential multi-ZIP, merge rules, rollback, corrupt/unsafe ZIPs, interrupted merge recovery | YES (92 checks) |
| Install Full Game against a mock server: archive stored rooted / in a wrapper folder / bare, foreign folders skipped, missing version.txt, unsafe archive, truncated download, HTTP 404, no leftovers | YES (in the 81 update checks) |
| Builder install against a mock server (needed files only, junk dropped, incomplete/unsafe/truncated refused, own files kept) | YES |
| Patch engine vs real WIT using the current official `updatedbuilder.zip` (May 2026 scripts) | YES: byte-identical for USA/PAL/Japan synthetic discs |
| Install Full Game on a real PS5 with the real archives, and the TLS connection to github.com from the console | NO |
| Launched on a real PS5 | YES - by the author (needs OnionHEN allowlist) |
| Real PAL patch of a real Mario Kart Wii image on PS5, patched game runs | YES - by the author, full flow |
| USA / Japan on a console | NO (host tests only) |
| Installing a newer update on a console | NO (pack was already current) |
| Real download/version check from update.rwfc.net | YES on console (check and install path ran without error); the CI sandbox itself cannot reach it |

## Jailbreak request
Confirmed working on the author's console with the OnionHEN allowlist entry (marker files + allowlist). Other firmware/HEN versions untested.

## Known risks
- Access to `/data/porpoise/...` from a PPSA title relies on ShadowMountPlus' mounts; if the sandbox blocks it, the app will show I/O errors in the log and in the status panel.
- sceHttp behaviour with large files, redirects and certificate store is untested (the reference sequence is used; redirects are handled manually and must be https).
- ZIPs have no published hashes, so authenticity relies on TLS only.
- Tests use synthetic discs; the real MKWii image is assumed to have TMD size 0x208 and a partition at the same offset as in the analysed layout. The engine refuses (rather than guesses) when assumptions do not hold.
- Output is a single .wbfs file; speed on PS5 not measured (host: ~1.3 GB incl. verify in ~50 s).
