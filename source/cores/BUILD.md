# Emulator cores

The two programs in `game/sys` are the launcher (`source/launcher/psxfe.c`) linked statically with a libretro core.

## PCSX ReARMed (PlayStation) – GNU GPL v2

- Upstream: https://github.com/libretro/pcsx_rearmed
- Commit: `7b0e8f8fadfbce3d170d195b9d9eecaabfa08c8e`
- Changes: `pcsx_rearmed.patch` in this folder (apply with `git apply`).
  Three files: cheats invalidate recompiled code, an optional widescreen projection in the GTE, one dynarec fix.
- Build: the libretro makefile (`Makefile.libretro`), static-library output, with the QNX cross compiler,
  ARM dynarec and the NEON GPU renderer enabled. The object files are archived into `libpcsx_qnx.a`.
  The exact `make` command line used for the binary in this repository was not recorded; the compiler
  options were the same as for the Snes9x core below (`-marm -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=softfp -O2`).

## Snes9x 2010 (Super Nintendo) – Snes9x licence, non-commercial

- Upstream: https://github.com/libretro/snes9x2010
- Commit: `fe690dd321fa5a46b5234a2bde089d2518c62b0e`
- Changes: `snes9x2010.patch` in this folder (one line in `build/Makefile.common`, so the image decoders are
  compiled into the static library).
- Build: `Makefile.libretro` with `STATIC_LINKING=1`, compiler
  `arm-...-nto-qnx6.5.0eabi-gcc -include stddef.h -marm -mcpu=cortex-a9 -mtune=cortex-a9 -mfpu=neon -mfloat-abi=softfp`,
  defines `-D__BLACKBERRY_QNX__ -O2 -DNDEBUG -DHAVE_THREADS -D__LIBRETRO__ -DSTATIC_LINKING`.
  The object files are archived into `snes9x2010_libretro_qnx.a`.

## Launcher

See `../launcher/build.sh`.
