#!/bin/bash
# Installs the paper trader as a systemd service which starts with the server.
# Run it from the repository root after the build and after "paper_trader.py init":
#     ./deploy/install_service.sh

set -e

DIRECTORY="$(cd "$(dirname "$0")/.." && pwd)"
SERVICE_USER="$(id -un)"
SERVICE_FILE="/etc/systemd/system/paper-trader.service"

if [ ! -x "$DIRECTORY/build/bin/backtest" ]; then
    echo "build/bin/backtest does not exist. Build it first (see README)."
    exit 1
fi
if [ ! -f "$DIRECTORY/paper/config.json" ]; then
    echo "paper/config.json does not exist. Run first: python3 scripts/paper_trader.py init"
    exit 1
fi

echo "Installing $SERVICE_FILE (user $SERVICE_USER, directory $DIRECTORY)..."
sed -e "s|__USER__|$SERVICE_USER|" -e "s|__DIRECTORY__|$DIRECTORY|" \
    "$DIRECTORY/deploy/paper-trader.service" | sudo tee "$SERVICE_FILE" > /dev/null

sudo systemctl daemon-reload
sudo systemctl enable --now paper-trader

echo "The paper trader is running."
echo "  Status:    python3 scripts/paper_trader.py status"
echo "  Log:       tail -f paper/paper.log"
echo "  Stop:      sudo systemctl stop paper-trader"
echo "  Restart:   sudo systemctl restart paper-trader"
