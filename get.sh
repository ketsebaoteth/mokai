#!/usr/bin/env bash
set -euo pipefail

INSTALL_DIR="$HOME/.local/bin"
URL="https://github.com/ketsebaoteth/mokai/releases/latest/download/mokai-rel"

mkdir -p "$INSTALL_DIR"

echo "Downloading mokai..."

if command -v curl >/dev/null 2>&1; then
  curl -sSLf "$URL" -o "$INSTALL_DIR/mokai"
elif command -v wget >/dev/null 2>&1; then
  wget -qO "$INSTALL_DIR/mokai" "$URL"
else
  echo "error: curl or wget is required" >&2
  exit 1
fi

if [ ! -s "$INSTALL_DIR/mokai" ]; then
  echo "error: download failed or binary is empty" >&2
  exit 1
fi

chmod +x "$INSTALL_DIR/mokai"

case ":$PATH:" in
*":$INSTALL_DIR:"*) ;;
*)
  for rc in "$HOME/.bashrc" "$HOME/.zshrc" "$HOME/.profile"; do
    if [ -f "$rc" ] && ! grep -qs "$INSTALL_DIR" "$rc"; then
      echo "export PATH=\"\$HOME/.local/bin:\$PATH\"" >>"$rc"
      echo "Added $INSTALL_DIR to $rc"
    fi
  done
  ;;
esac

echo "Installed mokai to $INSTALL_DIR/mokai"
echo "Restart your terminal or run: export PATH=\"\$HOME/.local/bin:\$PATH\""
