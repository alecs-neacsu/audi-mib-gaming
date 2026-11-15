#!/bin/bash
# Builds game/sys/psxfe (PS1) and game/sys/snesfe (SNES) for the MIB2 High unit (QNX 6.5, ARMv7).
# Needs: a GCC cross toolchain for arm-nto-qnx6.5.0eabi, a QNX 6.5 sysroot (libc, libm, libsocket),
# the unit's libscreen / libEGL / libGLESv2 to link against, and the two static cores built as in ../cores/BUILD.md.
set -e
: "${TC:?set TC to the toolchain prefix, e.g. /opt/qnx650/bin/arm-unknown-nto-qnx6.5.0eabi-}"
: "${SYSROOT:?set SYSROOT to the QNX 6.5 sysroot}"
: "${UNITLIB:?set UNITLIB to a folder with the unit's libscreen.so, libEGL.so, libGLESv2.so}"
: "${UNITINC:?set UNITINC to a folder with screen/screen.h, EGL/, GLES2/ headers}"
: "${PCSX_A:?set PCSX_A to libpcsx_qnx.a (PCSX ReARMed static core)}"
: "${SNES_A:?set SNES_A to snes9x2010_libretro_qnx.a (Snes9x 2010 static core)}"
LIBS="-L$UNITLIB -L$SYSROOT/usr/lib -L$SYSROOT/lib -lscreen -lEGL -lGLESv2 -lsocket -lm"
${TC}gcc -O2 -I"$UNITINC" -o psxfe psxfe.c runsafe_arm.S "$PCSX_A" $LIBS
${TC}strip psxfe
${TC}gcc -O2 -DCORE_SNES -I"$UNITINC" -o snesfe psxfe.c runsafe_arm.S "$SNES_A" $LIBS
${TC}strip snesfe
ls -la psxfe snesfe
