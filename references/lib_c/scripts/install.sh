#!/usr/bin/env bash
# Download prebuilt m4engine library from GitHub Releases.
# Usage: ./scripts/install.sh [--version v1.0.0-beta.3] [--dest ./lib]
#
# Requires: gh CLI authenticated (gh auth login)
# Detects platform automatically (darwin-arm64, linux-amd64, linux-arm64)

set -euo pipefail

REPO="ngoky/lib_c"
VERSION="latest"
DEST="${M4ENGINE_INSTALL_DIR:-./m4engine}"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --version) VERSION="$2"; shift 2 ;;
    --dest)    DEST="$2"; shift 2 ;;
    *)         echo "Unknown option: $1"; exit 1 ;;
  esac
done

# Detect platform
OS=$(uname -s | tr '[:upper:]' '[:lower:]')
ARCH=$(uname -m)
case "$ARCH" in
  x86_64)  ARCH="amd64" ;;
  aarch64) ARCH="arm64" ;;
  arm64)   ARCH="arm64" ;;
esac
PLATFORM="${OS}-${ARCH}"

echo "[m4engine] platform: ${PLATFORM}"
echo "[m4engine] version:  ${VERSION}"
echo "[m4engine] dest:     ${DEST}"

# Check gh CLI
if ! command -v gh &>/dev/null; then
  echo "[m4engine] ERROR: gh CLI not found. Install: https://cli.github.com/"
  echo "  brew install gh   # macOS"
  echo "  sudo apt install gh   # Ubuntu"
  exit 1
fi

# Check auth
if ! gh auth status &>/dev/null; then
  echo "[m4engine] ERROR: gh not authenticated. Run: gh auth login"
  exit 1
fi

# Download
TMPDIR=$(mktemp -d)
trap "rm -rf $TMPDIR" EXIT

if [ "$VERSION" = "latest" ]; then
  echo "[m4engine] downloading latest release..."
  gh release download --repo "$REPO" --pattern "*${PLATFORM}*" --dir "$TMPDIR"
else
  echo "[m4engine] downloading ${VERSION}..."
  gh release download "$VERSION" --repo "$REPO" --pattern "*${PLATFORM}*" --dir "$TMPDIR"
fi

# Extract
TARBALL=$(ls "$TMPDIR"/*.tar.gz 2>/dev/null | head -1)
if [ -z "$TARBALL" ]; then
  echo "[m4engine] ERROR: no tarball found for ${PLATFORM}"
  echo "[m4engine] available assets:"
  if [ "$VERSION" = "latest" ]; then
    gh release view --repo "$REPO" --json assets -q '.assets[].name'
  else
    gh release view "$VERSION" --repo "$REPO" --json assets -q '.assets[].name'
  fi
  exit 1
fi

mkdir -p "$DEST"
tar xzf "$TARBALL" --strip-components=1 -C "$DEST"

echo "[m4engine] installed to ${DEST}/"
echo "[m4engine] lib:     ${DEST}/lib/"
echo "[m4engine] include: ${DEST}/include/"
echo "[m4engine] version: $(cat "${DEST}/VERSION" 2>/dev/null || echo 'unknown')"

# Set env hint
case "$OS" in
  darwin) LIBEXT="dylib" ;;
  *)      LIBEXT="so" ;;
esac
echo ""
echo "Add to your environment:"
echo "  export M4ENGINE_LIB=${DEST}/lib/libm4engine.${LIBEXT}"
