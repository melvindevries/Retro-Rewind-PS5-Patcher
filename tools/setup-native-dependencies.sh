#!/usr/bin/env bash
# Dependency bootstrapper for the Porpoise Retro Rewind app.
# Derived from ps5-native-app-boilerplate (BlackBearReloaded, GPL-3.0-or-later).
# Change: host zlib comes from the system zlib1g-dev package (static libz.a)
# because zlib.net is not reachable from every build environment. zlib is only
# used by the host-side converter, never linked into the PS5 title.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cache="$root/.deps/native"
sdk="$cache/ps5-payload-sdk"
zlib_root="$cache/zlib/root"
sdk_url="https://github.com/ps5-payload-dev/sdk/releases/download/v0.42/ps5-payload-sdk.zip"
sdk_hash="8cfbc7cd5811e719eb4f0c47eea668d3dc7b40bc8ab11c4a5031d40c23ec02da"
skip_sdk=false
[[ ${1:-} == --skip-sdk ]] && skip_sdk=true
mkdir -p "$cache"
if ! $skip_sdk && [[ ! -x "$sdk/bin/prospero-lld" ]]; then
    archive="$cache/ps5-payload-sdk.zip"
    curl -fsSL --retry 3 -o "$archive.download" "$sdk_url"
    printf '%s  %s\n' "$sdk_hash" "$archive.download" | sha256sum --check --strict
    mv "$archive.download" "$archive"
    unzip -q -o "$archive" -d "$cache"
fi
if ! $skip_sdk && [[ ! -x "$sdk/bin/prospero-lld" || ! -d "$sdk/target/include" ]]; then
    echo "the pinned PS5 payload SDK is incomplete" >&2; exit 2
fi
if [[ ! -f "$zlib_root/usr/include/zlib.h" || -z $(find "$zlib_root" -name libz.a -print -quit 2>/dev/null) ]]; then
    [[ -f /usr/include/zlib.h && -f /usr/lib/x86_64-linux-gnu/libz.a ]] || {
        echo "install zlib1g-dev (host zlib with static library)" >&2; exit 2; }
    mkdir -p "$zlib_root/usr/include" "$zlib_root/usr/lib"
    cp /usr/include/zlib.h /usr/include/zconf.h "$zlib_root/usr/include/" 2>/dev/null || \
      cp /usr/include/x86_64-linux-gnu/zconf.h "$zlib_root/usr/include/"
    cp /usr/lib/x86_64-linux-gnu/libz.a "$zlib_root/usr/lib/"
fi
zlib_library=$(find "$zlib_root" -type f -name libz.a -print -quit)
printf 'SDK_ROOT=%s\nZLIB_INCLUDE=%s\nZLIB_ARCHIVE=%s\n' "$sdk" "$zlib_root/usr/include" "$zlib_library"
