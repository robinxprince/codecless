#!/bin/zsh
# Checks the decoder core against FFmpeg's software decoders: every sampled frame, fetched
# in random order (so every seek path runs), must hash identically to `ffmpeg -f framemd5`.
# Usage: tests/verify.sh <work dir> [clip...]   (without clips, generates a synthetic set)
set -euo pipefail
clips=(${@:2:A})
cd "${0:A:h}/.."
make -s build/meprobe
work=${1:?usage: tests/verify.sh <work dir> [clip...]}
mkdir -p "$work"

gen() {  # gen <file> <ffmpeg video args...>
    local out="$work/$1"; shift
    [[ -f $out ]] || ffmpeg -hide_banner -loglevel error -y \
        -f lavfi -i "testsrc2=size=1280x720:rate=30000/1001,noise=alls=10:allf=t+u" \
        -f lavfi -i "sine=frequency=440:sample_rate=48000" -t 12 "$@" "$out"
}
if (( ${#clips} == 0 )); then
    gen vp9_altref.webm   -c:v libvpx-vp9 -deadline good -cpu-used 4 -auto-alt-ref 1 -lag-in-frames 25 -g 60 -b:v 3M -c:a libopus
    gen vp9_10bit.webm    -c:v libvpx-vp9 -deadline good -cpu-used 5 -profile:v 2 -pix_fmt yuv420p10le -g 90 -b:v 3M -c:a libopus
    gen av1_ntsc.mkv      -c:v libsvtav1 -preset 10 -g 60 -pix_fmt yuv420p -c:a libopus
    gen av1_10bit.mp4     -c:v libsvtav1 -preset 10 -g 120 -pix_fmt yuv420p10le -c:a aac
    gen av1_openg.webm    -c:v libsvtav1 -preset 10 -g 48 -svtav1-params irefresh-type=1 -pix_fmt yuv420p -c:a libopus
    clips=("$work"/{vp9_altref.webm,vp9_10bit.webm,av1_ntsc.mkv,av1_10bit.mp4,av1_openg.webm})
fi

failed=0
for f in $clips; do
    clip=${f:t}
    info=$(build/meprobe info "$f" 2>/dev/null)  # ffprobe misses the pixel format of some downloads
    fmt=yuv420p; [[ $info == *10-bit* ]] && fmt=yuv420p10le
    ffmpeg -hide_banner -loglevel error -i "$f" -map 0:v -pix_fmt $fmt -f framemd5 - |
        awk -F', *' '!/^#/ {print NR - 1 - skip, $6} /^#/ {skip++}' | awk '{print $1, $2}' > "$work/ref.txt"
    total=$(wc -l < "$work/ref.txt" | tr -d ' ')
    frames=($(jot -r 60 0 $((total - 1))) 0 $((total - 1)) 59 60 61 58 120 119)
    build/meprobe md5 "$f" $frames | sort -n -u > "$work/got.txt"
    mismatches=$(awk 'NR == FNR {ref[$1] = $2; next} !($1 in ref) || ref[$1] != $2' "$work/ref.txt" "$work/got.txt" | wc -l | tr -d ' ')
    checked=$(wc -l < "$work/got.txt" | tr -d ' ')
    backend=$(awk '/^decoder/ {$1=""; print}' <<< "$info")
    echo "$clip: $checked frames checked of $total, $mismatches mismatched, decoder:$backend"
    (( mismatches == 0 )) || failed=1
done
exit $failed
