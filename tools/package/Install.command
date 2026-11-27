#!/bin/zsh
# Installs the MediaEngine AV1 / VP9 importer for Premiere Pro. Double-click, or run in Terminal.
set -euo pipefail
here=${0:A:h}
dest="/Library/Application Support/Adobe/Common/Plug-ins/7.0/MediaCore/MediaEngine"

if [[ $(uname -m) != arm64 ]]; then
    echo "This plugin needs a Mac with Apple silicon (M1 or later)."
    exit 1
fi
if pgrep -f -q "/Contents/MacOS/Adobe Premiere Pro"; then
    echo "Please quit Premiere Pro first, then run this again."
    exit 1
fi

echo "Installing into $dest"
echo "macOS will ask for your login password (the plugin folder belongs to the system)."
sudo mkdir -p "$dest"
sudo rm -rf "$dest/MediaEngine.bundle"
sudo cp -R "$here/MediaEngine.bundle" "$dest/"
sudo xattr -dr com.apple.quarantine "$dest/MediaEngine.bundle" 2>/dev/null || true

mkdir -p ~/Library/Logs/MediaEngine
cp "$here/stats.html" ~/Library/Logs/MediaEngine/stats.html

echo
echo "Done. Start Premiere Pro and import an AV1 or VP9 file (.mp4, .mkv, .webm)."
echo "Live performance page: open ~/Library/Logs/MediaEngine/stats.html"
