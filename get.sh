#!/usr/bin/env bash
set -e

RESET="\033[0m"
BOLD="\033[1m"
GREEN="\033[32m"
CYAN="\033[36m"
DIM="\033[2m"

VERSION="0.0.1"
INSTALL_DIR="$HOME/.local/bin"
mkdir -p "$INSTALL_DIR"

echo -e "${DIM}⠋ Fetching mokai binary distribution tree v${VERSION}...${RESET}"

OS_TYPE="$(uname -s)"
if [ "$OS_TYPE" = "Linux" ]; then
  curl -sL "https://github.com/ketsebaoteth/mokai/releases/download/v${VERSION}/mokai-linux-x64" -o "$INSTALL_DIR/mokai"
else
  echo "This installer currently supports Linux environments. For Windows, use Scoop."
  exit 1
fi

chmod +x "$INSTALL_DIR/mokai"

echo -e "\n${GREEN}✨ Mokai installed perfectly!${RESET}"
echo -e "  ${DIM}Location:${RESET} $INSTALL_DIR/mokai"
echo -e "  Ensure ${CYAN}$INSTALL_DIR${RESET} is appended to your system's ${BOLD}\$PATH${RESET} variable."
