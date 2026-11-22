#!/bin/sh
# Runs a build in the Azahar emulator (docker image linuxserver/azahar),
# headless, and records both screens to a video.
#   make EXTRA_CFLAGS=-DAUTOTEST=1     (scripted key presses, see source/main.c)
#   tools/emutest.sh Funkin3DS.3dsx 90 out.mkv
set -e
ROM=$(realpath "$1")
SECS=${2:-60}
OUT=$(realpath -m "${3:-emutest.mkv}")
DIR=$(mktemp -d)
cp "$ROM" "$DIR/game.3dsx"
cat > "$DIR/run.sh" <<'EOS'
#!/bin/sh
Xvfb :99 -screen 0 1280x1024x24 >/dev/null 2>&1 &
sleep 2
export DISPLAY=:99 HOME=/tmp/home
timeout -k 5 -s TERM "$1" azahar -d /emu/out.mkv /emu/game.3dsx > /emu/log.txt 2>&1 || true
EOS
docker run --rm -v "$DIR":/emu --entrypoint sh linuxserver/azahar /emu/run.sh "$SECS" || true
mv "$DIR/out.mkv" "$OUT"
rm -rf "$DIR"
echo "recorded $OUT (top screen above, bottom screen below)"
