#!/usr/bin/env bash
# ============================================================================
# start_rustfs.sh - start the local rustfs S3 server for the UGP CDN (macOS).
#
# macOS counterpart of start_rustfs.bat. rustfs is an S3-compatible object
# store (MinIO-class, written in Rust). Put the rustfs binary next to this
# script (see README "发布到 S3 兼容存储" / Docs/DEBUG.md §0), then run:
#
#   ./start_rustfs.sh
#
# and publish the CDN into it with:
#
#   python3 s3_sync.py --endpoint http://127.0.0.1:9000 --access-key ugpadmin \
#       --secret-key ugpadmin123 --bucket ugp-cdn --create --public \
#       --sync <project>/Saved/UGP_Package/CloudDir
#
# Then point the client at: -cdn=http://127.0.0.1:9000/ugp-cdn (or
# -cdn=file:///... for the local dir). The bucket must be public-read: the
# client GETs without signing.
#
# Where to get the binary:
#   Apple Silicon: rustfs/rustfs releases -> rustfs-macos-aarch64-*.zip
#   Intel (x86_64): official builds stopped shipping after 1.0.0-beta.10;
#   use rustfs-macos-x86_64-v1.0.0-beta.10-preview.1.zip (the version this
#   script's liblzma fix-up step was written for).
#
# Defaults (override via env vars): RUSTFS_DATA / RUSTFS_ADDR / RUSTFS_KEY /
# RUSTFS_SECRET. Requires bash 3.2+ (stock macOS).
# ============================================================================
set -u
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" && pwd)"

# --- rustfs binary sits next to this script ---
RUSTFS_BIN=""
for CAND in "$SCRIPT_DIR/rustfs" "$SCRIPT_DIR/rustfs.exe"; do
  if [ -x "$CAND" ]; then RUSTFS_BIN="$CAND"; break; fi
done
if [ -z "$RUSTFS_BIN" ]; then
  echo "[ERROR] rustfs binary not found next to this script: $SCRIPT_DIR/rustfs"
  exit 1
fi

# --- One-time linker fix-up for the macos-x86_64 beta build -----------------
# That build references Homebrew's /usr/local/opt/xz/lib/liblzma.5.dylib,
# which most machines do not have. macOS ships the same ABI (liblzma.5) in
# the dyld shared cache, so re-point the binary at /usr/lib/liblzma.5.dylib
# and re-sign ad-hoc. Needs the Xcode Command Line Tools (install_name_tool).
if otool -L "$RUSTFS_BIN" 2>/dev/null | grep -q '/usr/local/opt/xz/lib/liblzma\.5\.dylib'; then
  if command -v install_name_tool >/dev/null && command -v codesign >/dev/null; then
    echo "[rustfs] re-linking liblzma.5 -> /usr/lib/liblzma.5.dylib (one-time fix)"
    install_name_tool -change /usr/local/opt/xz/lib/liblzma.5.dylib \
      /usr/lib/liblzma.5.dylib "$RUSTFS_BIN" && codesign -f -s - "$RUSTFS_BIN"
  else
    echo "[ERROR] $RUSTFS_BIN needs liblzma.5 (brew xz) and Xcode CLT"
    echo "        is missing, so it cannot be re-linked automatically."
    echo "        Install CLT (xcode-select --install) or 'brew install xz'."
    exit 1
  fi
fi

# --- Data volume: project Saved/rustfs-data (auto-derived from script location) ---
PROJECT_DIR="$(cd "$SCRIPT_DIR/../../../.." && pwd)"
: "${RUSTFS_DATA:=$PROJECT_DIR/Saved/rustfs-data}"
: "${RUSTFS_ADDR:=127.0.0.1:9000}"
: "${RUSTFS_KEY:=ugpadmin}"
: "${RUSTFS_SECRET:=ugpadmin123}"

mkdir -p "$RUSTFS_DATA"

# --- Port check: refuse to start a SECOND CDN server on the same port -------
# Extract the port (text after the LAST ':') from RUSTFS_ADDR, then look for
# a LISTENING socket on it - another rustfs/CDN instance would conflict and
# silently shadow this one.
PORT="${RUSTFS_ADDR##*:}"
if lsof -nP -iTCP:"$PORT" -sTCP:LISTEN 2>/dev/null | grep -q LISTEN; then
  echo "[ERROR] port $PORT is already LISTENING - a rustfs/CDN server seems"
  echo "        to be running. Stop it first, or set RUSTFS_ADDR to another"
  echo "        port, then start this one."
  exit 2
fi

echo "============================================================================"
echo "[rustfs] binary : $RUSTFS_BIN"
echo "[rustfs] data   : $RUSTFS_DATA"
echo "[rustfs] address: $RUSTFS_ADDR   console: http://127.0.0.1:9001"
echo "[rustfs] keys   : $RUSTFS_KEY / $RUSTFS_SECRET"
echo "[rustfs] client : -cdn=http://$RUSTFS_ADDR/<bucket>   (or -cdn=file:///<local CDN dir>)"
echo "============================================================================"
# --console-enable: the macos beta requires the flag for the :9001 console
# (the Windows exe the .bat targets enables it by default).
exec "$RUSTFS_BIN" server "$RUSTFS_DATA" \
  --address "$RUSTFS_ADDR" \
  --access-key "$RUSTFS_KEY" \
  --secret-key "$RUSTFS_SECRET" \
  --console-enable "$@"
