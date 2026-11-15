#!/bin/sh
# MMI GAMES - minimal kit: installer. Run once from the Green Menu with the M.I.B. tool (run command.sh).
#
# Installs the "bridge": one file on the unit (xTcpKeyPanel.jar) is replaced by a version that also contains the
# bridge classes. That file is BUILT HERE, ON THE UNIT, from the unit's own original file plus \game\punte\bridge_classes.zip
# (the kit does not contain any file from the Audi firmware).
#   - builds only from a file that is byte-identical to the known original; otherwise stops and changes nothing;
#   - saves the original on the unit (/mnt/app/root/game_backup) and on the card (\game\backup) first;
#   - the built file is verified entry by entry before it is used, and the copy on the unit is verified after;
#   - if anything fails, the original is put back.
# Uninstall: create an empty file \game\UNINSTALL_BRIDGE, run this script again, reboot.
# Report: \game\install_report.txt
SD=/net/mmx/fs/sda0
G=$SD/game
P=/fs/sda0/game/punte
B=$G/backup
UB=/net/mmx/mnt/app/root/game_backup
N=xTcpKeyPanel.jar
J=/net/mmx/mnt/app/eso/hmi/lsd/jars/$N
C=$P/bridge_classes.zip
M=/tmp/mkbridge
NEW=/tmp/$N.new
mount -uw $SD
[ -d $G ] || { echo "ERROR: game folder not found on the SD card (SD1)"; exit 0; }
R=$G/install_report.txt
: > $R
say() { echo "$*"; echo "$*" >> $R; }
say "=== MMI GAMES kit installer ==="

if [ -f $G/UNINSTALL_BRIDGE ]; then
  say "--- UNINSTALL requested"
  S=""
  [ -f $UB/$N.orig ] && S=$UB
  [ -z "$S" ] && [ -f $B/$N.orig ] && S=$B
  [ -z "$S" ] && { say "ERROR: no backup of the original found (unit or card). Nothing changed."; exit 0; }
  mount -uw /net/mmx/mnt/app
  cp $S/$N.orig $J.tmp && mv -f $J.tmp $J; sync
  if cmp -s $S/$N.orig $J; then say "OK: original restored. Delete game/UNINSTALL_BRIDGE and REBOOT the unit."; else say "ERROR: restore could not be verified - run again."; fi
  exit 0
fi

for f in $P/mkbridge $C /fs/sda0/game/lansator.sh /fs/sda0/game/sys/psxfe; do [ -f $f ] || { say "ERROR: $f missing on the card. Nothing changed."; exit 0; }; done
[ -f $J ] || { say "STOP: $N not found on the unit - not the expected unit (MIB2 High). Nothing changed."; exit 0; }
cp $P/mkbridge $M && chmod 755 $M || { say "ERROR: cannot prepare the build tool. Nothing changed."; exit 0; }

ST=`$M check $J $C 2>/dev/null`
say "file on the unit: $ST"
SRC=""
case "$ST" in
  BRIDGE-SAME) say "OK: already installed. In P, press the brake and hold MENU 1.5 s to open the launcher."; rm -f $M; exit 0 ;;
  ORIGINAL) SRC=$J ;;
  BRIDGE-OTHER)
    # another version of the bridge is installed: rebuild from the original saved on the unit when it was installed
    if [ -f $UB/$N.orig ] && [ "`$M check $UB/$N.orig $C 2>/dev/null`" = "ORIGINAL" ]; then SRC=$UB/$N.orig; say "another bridge version is installed; rebuilding from the original saved on the unit"
    else say "STOP: another bridge version is installed and its original backup was not found on the unit. Nothing changed."; rm -f $M; exit 0; fi ;;
  *)
    mkdir -p $B; cp $J $B/unknown_$N 2>/dev/null; sync
    say "STOP: $N on this unit is not the known original (different firmware, or modified by something else)."
    say "      Not installing. Nothing changed on the unit. Its file was copied to game/backup/unknown_$N"
    rm -f $M; exit 0 ;;
esac

say "--- building the bridge file from $SRC"
rm -f $NEW
OUT=`$M build $SRC $C $NEW 2>&1`; RC=$?
say "build: $OUT (code $RC)"
if [ $RC -ne 0 ] || [ ! -f $NEW ] || [ "`$M check $NEW $C 2>/dev/null`" != "BRIDGE-SAME" ]; then say "ERROR: the bridge file could not be built and verified. Nothing changed on the unit."; rm -f $M $NEW; exit 0; fi

say "--- saving backups of the original"
mkdir -p $B; cp $SRC $B/$N.orig; sync
cmp -s $SRC $B/$N.orig || { say "ERROR: backup on the card could not be verified. Not installing."; rm -f $M $NEW; exit 0; }
mount -uw /net/mmx/mnt/app
mkdir -p $UB
if [ "$SRC" = "$J" ]; then cp $J $UB/$N.orig; sync; fi
cmp -s $SRC $UB/$N.orig || { say "ERROR: backup on the unit could not be verified. Not installing."; rm -f $M $NEW; exit 0; }
say "backup OK: $UB and game/backup"

say "--- installing"
cp $NEW $J.tmp && cmp -s $NEW $J.tmp && mv -f $J.tmp $J; sync
if cmp -s $NEW $J; then
  say "OK: bridge installed. REBOOT the unit and keep this card in slot SD1."
  say "After the reboot: car in P, press the brake and hold MENU for 1.5 s."
else
  say "ERROR: verification failed - putting the original back"
  rm -f $J.tmp; cp $UB/$N.orig $J; sync
  cmp -s $UB/$N.orig $J && say "original restored" || say "ERROR: could not restore - run again with game/UNINSTALL_BRIDGE"
fi
rm -f $M $NEW
exit 0
