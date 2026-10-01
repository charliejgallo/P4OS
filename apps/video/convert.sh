#!/bin/zsh
#
# Converts a video into what the Video app plays on P4OS: an AVI of MJPEG
# frames, and beside it the sound as a 16-bit PCM WAV with the same name.
#
#   apps/video/convert.sh clip.mp4                   # full: fits 1280x720 or 720x1280
#   apps/video/convert.sh clip.mp4 half              # 640x360 / 360x640, the PPA doubles it
#   apps/video/convert.sh clip.mp4 full 24 6 out/    # mode, fps, JPEG quality (2 best .. 31), folder
#
# Then copy both files to the card's videos folder (the portal's Files, or
# the card in the computer).
#
# The picture keeps its shape: a wide clip fits inside 1280x720, a tall one
# inside 720x1280, and the app letterboxes whatever does not fill the screen.
# 4:2:0 baseline JPEG, the one kind the P4's JPEG engine decodes. The
# engine pads its rows to 16 pixels (360 comes out 368 apart); the blit
# reads them that way, so any even size works.
#
# Why "half": a 640x360 frame is a quarter of the bytes of a 1280x720 one on
# the card and in the decoder, and the PPA scales it to the whole screen
# for free while it blits. Softer, but it plays at 30 fps with room to spare.
# "full" is sharp and heavier: the numbers the board must confirm are in
# README.md.
#
set -e

IN=${1:?usage: convert.sh <input> [full|half] [fps] [quality] [out_dir]}
MODE=${2:-full}
case $MODE in
    full) BW=1280; BH=720; DEF_FPS=24; DEF_Q=6 ;;
    half) BW=640;  BH=360; DEF_FPS=30; DEF_Q=5 ;;
    *) echo "mode is full or half"; exit 1 ;;
esac
FPS=${3:-$DEF_FPS}
Q=${4:-$DEF_Q}
OUT=${5:-$(dirname "$IN")}
NAME=$(basename "${IN%.*}")

command -v ffmpeg > /dev/null || { echo "ffmpeg is missing (brew install ffmpeg)"; exit 1; }
mkdir -p "$OUT"

# The long side goes to BW whichever way the clip stands.
ffmpeg -v error -y -i "$IN" \
    -vf "scale=w='if(gte(iw,ih),$BW,$BH)':h='if(gte(iw,ih),$BH,$BW)':force_original_aspect_ratio=decrease:force_divisible_by=2,fps=$FPS" \
    -pix_fmt yuvj420p -c:v mjpeg -q:v "$Q" -an \
    "$OUT/$NAME.avi"

# The sound, only if the input has any: mono 32 kHz, which the board's mixer
# resamples to its bus, and 64 KB/s of card next to the picture's megabytes.
if ffprobe -v error -select_streams a:0 -show_entries stream=codec_type -of csv=p=0 "$IN" 2>/dev/null | grep -q audio; then
    ffmpeg -v error -y -i "$IN" -vn -ac 1 -ar 32000 -c:a pcm_s16le "$OUT/$NAME.wav"
    has_audio=yes
else
    rm -f "$OUT/$NAME.wav"
    has_audio=no
fi

size=$(ffprobe -v error -select_streams v:0 -show_entries stream=width,height -of csv=s=x:p=0 "$OUT/$NAME.avi")
frames=$(ffprobe -v error -count_packets -select_streams v:0 -show_entries stream=nb_read_packets -of csv=p=0 "$OUT/$NAME.avi")
bytes=$(wc -c < "$OUT/$NAME.avi" | tr -d ' ')
echo "$OUT/$NAME.avi: $size, $frames frames at $FPS fps, $((bytes / 1024)) KB" \
     "($((bytes / frames / 1024)) KB per frame, $((bytes * FPS / frames / 1024)) KB/s), audio: $has_audio"
