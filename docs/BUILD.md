# Build (Linux, x86-64)
Needs: clang-18, python3, ninja-compatible tooling as used by the boilerplate (`tools/setup-native-dependencies.sh` fetches
ps5-payload-sdk v0.42 into `.deps/native/` and pins checksums).
```
tools/build-app.sh          # stages app/src + core/ + miniz, cross-compiles, links, signs, assembles dist/PPSA97531
python3 tools/gen_font.py app/src/font_data.h   # only if you change the font (needs Pillow + DejaVu Sans Mono)
tests/run_all.sh <ORACLE_ROOT>                  # host tests (see below)
```
Host tests: `tests/run_all.sh` builds `build/rrcli` and `build/updcli` with the system cc. The update tests use a mock
transport and a generated server tree. Patch tests need oracle directories created with `tests/make_oracle.sh` (it builds
Wiimm's ISO Tools from `wiimms-iso-tools-master.zip` and a synthetic, non-Nintendo disc; no game data is shipped).
