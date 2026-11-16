#!/bin/bash
# vinylclean — shows the control page full screen on a touchscreen connected
# to the Pi (official DSI screen, or HDMI screen + USB touch).
#
# Lightweight browser: "cog" (WPE WebKit engine, made for embedded use) inside
# "cage" (minimal full-screen compositor, no desktop). If cog is not available,
# Chromium in kiosk mode (uses more memory).
#
# The screen opens the "simple view" (two VU meters + sensitivity slider); its
# "Advanced" button opens all the settings, one page per group, without scrolling.
#
#   ./scripts/kiosk-install.sh                       install and start the screen
#   ./scripts/kiosk-install.sh --remove              remove the screen (sound keeps running)
#   ./scripts/kiosk-install.sh --resolution 1024x600 force the HDMI resolution (then reboot)
set -e
cd "$(dirname "$0")/.."
USER_NAME="${SUDO_USER:-$USER}"
URL="http://127.0.0.1:8080/simple.html"

if [ "$1" = "--resolution" ]; then
    RES="$2"
    if ! [[ "$RES" =~ ^[0-9]{3,4}x[0-9]{3,4}$ ]]; then echo "Example: $0 --resolution 1024x600"; exit 1; fi
    F=/boot/firmware/cmdline.txt; [ -f "$F" ] || F=/boot/cmdline.txt
    sudo cp "$F" "$F.before-vinylclean"
    # cmdline.txt is ONE line: replace an older HDMI-A-1 setting, then put ours at the start.
    sudo sed -i -E "s/ ?video=HDMI-A-1:[^ ]*//g; 1s/^/video=HDMI-A-1:${RES}@60 /" "$F"
    echo "Done: $(head -c 120 "$F")..."
    echo "Backup: $F.before-vinylclean. Reboot to apply: sudo reboot"
    exit 0
fi

if [ "$1" = "--remove" ]; then
    sudo systemctl disable --now vinylclean-kiosk.service 2>/dev/null || true
    sudo rm -f /etc/systemd/system/vinylclean-kiosk.service
    sudo systemctl daemon-reload
    sudo systemctl enable --now getty@tty1.service 2>/dev/null || true
    echo "Screen disabled."
    exit 0
fi

echo "=== 1/3  Packages (minimal display + lightweight browser) ==="
sudo apt-get update
sudo apt-get install -y --no-install-recommends cage fonts-dejavu-core
if sudo apt-get install -y --no-install-recommends cog; then
    KIOSK="/usr/bin/cage -d -- /usr/bin/cog ${URL}"
    echo "Browser: cog (lightweight)"
else
    sudo apt-get install -y --no-install-recommends chromium
    KIOSK="/usr/bin/cage -d -- /usr/bin/chromium --kiosk --noerrdialogs --disable-infobars --no-first-run --ozone-platform=wayland --enable-features=OverlayScrollbar --disk-cache-size=1 ${URL}"
    echo "Browser: chromium (cog not available)"
fi

echo "=== 2/3  Access to the screen and touch input ==="
sudo usermod -aG video,input,render,tty "${USER_NAME}"

echo "=== 3/3  Screen service ==="
sed -e "s|@USER@|${USER_NAME}|" -e "s|@KIOSK@|${KIOSK}|" scripts/vinylclean-kiosk.service | sudo tee /etc/systemd/system/vinylclean-kiosk.service > /dev/null
sudo systemctl daemon-reload
sudo systemctl disable --now getty@tty1.service 2>/dev/null || true
sudo systemctl enable --now vinylclean-kiosk.service
sleep 5
systemctl status vinylclean-kiosk.service --no-pager | head -10
echo
echo "The screen shows the control page. If the picture is upside down, see the README (rotation)."
echo "To remove it: ./scripts/kiosk-install.sh --remove"
