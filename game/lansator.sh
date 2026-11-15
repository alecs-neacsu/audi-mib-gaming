#!/bin/sh
# MMI GAMES - minimal kit: launcher (started by the bridge: in P, brake pressed + MENU held 1.5 s).
# Shows the game list (PS1 games from \game\psx, SNES games from \game\snes) and runs the chosen game.
exec > /tmp/lansator_out.txt 2>&1 < /dev/null
export PATH=/proc/boot:/bin:/usr/bin:/sbin:/usr/sbin:/mnt/app/armle/bin:/mnt/app/armle/usr/bin:/mnt/app/armle/sbin:$PATH
echo "launcher started `date`"
SD=/net/mmx/fs/sda0
G=$SD/game
K=/fs/sda0/game
Y=$K/sys
D=/net/mmx/eso/bin/apps/dmdt
mount -uw $SD
mkdir -p $G/logs $G/saves/psx $G/saves/snes
dm() { $D "$@" > /dev/null 2>&1 & DP=$!; j=0; while kill -0 $DP 2>/dev/null && [ $j -lt 5 ]; do sleep 1; j=$((j+1)); done; kill -0 $DP 2>/dev/null && kill $DP; }
cp /tmp/gamebridge.log $G/logs/bridge_log.txt 2>/dev/null
cp $Y/psxfe /tmp/psxfe && chmod 755 /tmp/psxfe
cp $Y/snesfe /tmp/snesfe && chmod 755 /tmp/snesfe
# controller: ds3d keeps the USB DualShock 3 and sends its state to the emulator. Started once, never stopped.
alive() { [ -f "$1" ] && kill -0 `cat "$1"` 2>/dev/null; }
if ! alive /tmp/ds3d.pid && [ ! -f $G/ds3_off ]; then cp $Y/ds3d /tmp/ds3watch && chmod 755 /tmp/ds3watch && /tmp/ds3watch > /dev/null 2>&1 < /dev/null & sleep 1; echo "ds3d started"; fi
export LD_LIBRARY_PATH=/mnt/app/armle/lib:/mnt/app/armle/usr/lib:/proc/boot:/lib:/usr/lib:/lib/dll
export MIB_LOG=$G/logs/psx_log.txt MIB_SYS=$K/bios MIB_SAVE=$K/saves/psx MIB_STEER=80 MIB_VOLFILE=$K/saves/volum.txt MIB_LVFILE=$K/saves/volum_joc.txt
export MIB_BG=$Y/background.jpg MIB_FONT=$Y/font.otf MIB_LOGO=none MIB_SNESDIR=$K/snes
export MIB_ICON_PSX=$Y/icon_psx.png MIB_ICON_SNES=$Y/icon_snes.png
# optional: \game\conexiune.txt = audio connection number to ask the unit for (default 156); \game\sunet.txt = sound device
[ -f $K/sunet.txt ] && export MIB_SNDDEV=`sed 's/[^0-9]//g' $K/sunet.txt | head -1`
[ -f $K/conexiune.txt ] && export MIB_AUDIOCONN=`sed 's/[^0-9]//g' $K/conexiune.txt | head -1`
: > /tmp/audio.log
rm -f /tmp/psx_last
touch /tmp/lansator_ruleaza
( while [ -f /tmp/lansator_ruleaza ]; do sleep 10; cp /tmp/gamebridge.log $G/logs/bridge_log.txt 2>/dev/null; cp /tmp/ds3d.log $G/logs/ds3d_log.txt 2>/dev/null; cp /tmp/lansator_out.txt $G/logs/launcher_out.txt 2>/dev/null; sync; done ) > /dev/null 2>&1 &
n=0; NEXT=""
while :; do
  n=$((n+1))
  rm -f /tmp/racer_exit
  if [ -n "$NEXT" ]; then
    MIB_GAME="$NEXT" MIB_SAVE=$K/saves/snes MIB_BEZEL=$Y/bezel.png MIB_LOG=$G/logs/snes_log.txt /tmp/snesfe > $G/logs/snes_out.txt 2>&1 &
  else
    /tmp/psxfe $K/psx > $G/logs/psx_out.txt 2>&1 &
  fi
  P=$!
  if [ $n -eq 1 ]; then sleep 3; else sleep 1; fi
  if kill -0 $P 2>/dev/null; then dm dc 99 1234; dm sc 0 99; else echo "stopped at start (round $n)"; fi
  wait $P
  NEXT=""
  echo "round $n: `cat /tmp/racer_exit 2>/dev/null`"
  if [ -f /tmp/racer_exit ] && grep -q "^snes " /tmp/racer_exit; then NEXT=`sed -n 's/^snes //p' /tmp/racer_exit | head -1`; [ $n -lt 50 ] && continue; fi
  if [ -f /tmp/racer_exit ] && grep -q "^lista" /tmp/racer_exit; then [ $n -lt 50 ] && continue; fi
  break
done
# give the screen back to the MMI (except on reverse, where the camera takes over)
grep -q "marsarier" /tmp/racer_exit 2>/dev/null || dm sc 0 0
rm -f /tmp/psxfe /tmp/snesfe /tmp/lansator_ruleaza
cp /tmp/gamebridge.log $G/logs/bridge_log.txt 2>/dev/null; cp /tmp/ds3d.log $G/logs/ds3d_log.txt 2>/dev/null; cp /tmp/lansator_out.txt $G/logs/launcher_out.txt 2>/dev/null
sync
exit 0
