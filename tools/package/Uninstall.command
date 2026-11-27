#!/bin/zsh
# Removes the MediaEngine AV1 / VP9 importer. Premiere's own importers take AV1/VP9 files back.
set -euo pipefail
dest="/Library/Application Support/Adobe/Common/Plug-ins/7.0/MediaCore/MediaEngine"
if pgrep -f -q "/Contents/MacOS/Adobe Premiere Pro"; then
    echo "Please quit Premiere Pro first, then run this again."
    exit 1
fi
sudo rm -rf "$dest"
rm -rf ~/Library/Logs/MediaEngine
echo "Removed. Clips imported with the plugin need re-importing (or relinking) to use Premiere's own importer."
