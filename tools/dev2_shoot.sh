#!/bin/sh
# dev2_shoot.sh SECONDS SHOT_EVERY "SCRIPT" [EXTRA_ENV] - run on the test handheld with scripted input,
# pull screenshots to build/shots and the log to build/logs/dev_last.txt
cd "$(dirname "$0")/.."
D=/mnt/sdcard/ports/the_familia
# never two games at once: clear anything a previous run left behind, before and after
KILL='for p in $(pidof familia.aarch64 gptokeyb); do kill -9 $p; done; for p in $(ps -o pid,args | grep "[d]ev-run" | cut -c1-6); do kill $p; done'
tools/dev.sh "$KILL" >/dev/null 2>&1
trap 'tools/dev.sh "$KILL" >/dev/null 2>&1' EXIT
tools/dev.sh "rm -f $D/conf/shot_*.ppm; printf 'FAMILIA_PROBE=1\nFAMILIA_SHOT=$2\nFAMILIA_INPUT_SCRIPT=\"$3\"\n$4\nexport FAMILIA_PROBE FAMILIA_SHOT FAMILIA_INPUT_SCRIPT\n' > $D/conf/dev.cfg; cd $D && timeout -k 5 $(( $1 + 40 )) ./dev-run.sh $1 > /dev/null 2>&1"
rm -rf build/shots; mkdir -p build/shots
tools/dev.sh "cd $D/conf && tar cf - shot_*.ppm 2>/dev/null" | tar xf - -C build/shots 2>/dev/null
tools/dev.sh "cat $D/log.txt" > build/logs/dev_last.txt
python3 - <<'PY'
import glob
from PIL import Image
fs = sorted(glob.glob('build/shots/shot_*.ppm'))
for f in fs: Image.open(f).save(f[:-4] + '.png')
if fs:
    ims = [Image.open(f).resize((213, 160)) for f in fs]
    cols = 4; rows = (len(ims) + cols - 1) // cols
    sheet = Image.new('RGB', (cols * 213, rows * 160))
    for i, im in enumerate(ims): sheet.paste(im, ((i % cols) * 213, (i // cols) * 160))
    sheet.save('build/shots/sheet.png')
print(len(fs), 'shots')
PY
grep -E "\[input\] script|ExitProcess|fault|crash" build/logs/dev_last.txt | head
