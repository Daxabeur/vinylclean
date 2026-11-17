#!/bin/bash
# vinylclean — installation on the Raspberry Pi (or any Debian/Ubuntu Linux).
#
#   ./scripts/install.sh            build, test and install the tools
#   ./scripts/install.sh --service  same + start vc-live automatically at boot
#   ./scripts/kiosk-install.sh      (afterwards, optional) touchscreen
set -e
cd "$(dirname "$0")/.."

echo "=== 1/4  Required packages ==="
sudo apt-get update
sudo apt-get install -y build-essential cmake pkg-config libasound2-dev libflac-dev

echo "=== 2/4  Build (a few minutes on a Pi 3B+) ==="
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j2

echo "=== 3/4  Automatic tests ==="
./build/vc-tests --quick
./build/vc-tests-crackle --quick
./build/vc-tests-loudness
./build/vc-tests-rec
./build/vc-tests-riaa
./build/vc-tests-antidist
./build/vc-tests-side
./build/vc-tests-drift

echo "=== 4/4  Install into /usr/local/bin ==="
sudo cmake --install build --prefix /usr/local
echo "Installed: vc-live, vc-file, vc-bench, vc-tests, vc-tests-crackle, vc-tests-loudness, vc-tests-rec, vc-tests-riaa, vc-tests-antidist, vc-tests-side, vc-tests-drift"
echo "Control page: /usr/local/share/vinylclean/index.html (editable, reloaded every time it is opened)"

if [ "$1" = "--service" ]; then
    echo "=== Automatic start service ==="
    USER_NAME="${SUDO_USER:-$USER}"
    if [ ! -f /etc/default/vinylclean ]; then
        sudo install -m 644 scripts/vinylclean.default /etc/default/vinylclean
    fi
    USER_HOME="$(getent passwd "${USER_NAME}" | cut -d: -f6)"
    sed -e "s|@USER@|${USER_NAME}|" -e "s|@HOME@|${USER_HOME}|" scripts/vinylclean-live.service | sudo tee /etc/systemd/system/vinylclean-live.service > /dev/null
    sudo systemctl daemon-reload
    sudo systemctl enable vinylclean-live.service
    sudo systemctl restart vinylclean-live.service
    sleep 3
    systemctl status vinylclean-live.service --no-pager | head -12
    echo
    echo "Control page: http://$(hostname).local:8080   (or http://$(hostname -I | cut -d' ' -f1):8080)"
    echo "Only one program can use the sound card: stop any other audio software using it first."
fi

echo "=== Done ==="
