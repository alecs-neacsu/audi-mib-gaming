/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Launcher / libretro frontend of the audi-mib-gaming kit.
 * Copyright (C) 2026 the audi-mib-gaming author.
 *
 * This program is free software; you can redistribute it and/or modify it under the terms of the GNU General
 * Public License as published by the Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version. It is distributed WITHOUT ANY WARRANTY; see licenses/GPL-2.0.txt.
 *
 * Additional permission (from the copyright holder of this file): you may also build this file together with
 * the Snes9x 2010 libretro core (build with -DCORE_SNES) and distribute the result under the terms of the
 * Snes9x licence (licenses/Snes9x-LICENSE.txt), which allows non-commercial use only.
 *
 * Comments below are in Romanian.
 */
/* PS1 pe MMI (MIB2 High, QNX 6.5): frontend libretro minimal pentru PCSX-ReARMed.
 * - imagine: textura GLES2 pe ecranul MMI (displayable "1234")
 * - control: volanul A5 = stick analogic stanga; pedala de frana = acceleratie progresiva (X pulsat),
 *   ridicarea piciorului = frana (Patrat 0.6 s); umar stanga = L1, umar dreapta = R1 (saritura/drift),
 *   apasare rotita = X, rotita = sus/jos, atingere trackpad = Cerc, BACK = Triunghi, MENU = Start,
 *   BACK/MENU tinut 1.5 s = iesire.
 * - SIGURANTA: fir separat; daca treapta nu mai e P sau masina se misca -> iesire imediata.
 * - lista de jocuri: cauta .cue/.pbp/.iso in /fs/<orice>/{PS1,ps1,PSX,psx,} */
#include <stdio.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#ifndef DESKTOP
#include <screen/screen.h>
#endif
#include "libretro.h"
#include "font.h"

#define W 1024
#define H 480
static FILE *lg; static double t0;
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
static void L(const char *f, ...) { if (!lg) return; va_list a; va_start(a, f); fprintf(lg, "%.2f ", now() - t0); vfprintf(lg, f, a); va_end(a); fputc('\n', lg); fflush(lg); }
static double steerMaxDeg = 80, antiDz = 0.20;
static char sysdir[256] = "/fs/sda0/game/psx/bios", savedir[256] = "/fs/sda0/game/psx/saves";

/* ================= intrari + siguranta ================= */
static volatile double wPrev = 0, wT0 = 0, wT1 = 0;
static volatile double vWheel = 0, vBrake = 0, vSpeed = 0, vHFt = -100; static volatile int vGear = -1, vRev = 0;
static volatile int bBrake = 0, bRevB = 0;
/* profil de controale, din .cht: "#controale=worms" -> 1 (fara volan/pedala). 0 = curse (implicit) */
static volatile int prof = 0;      /* 1 = worms, 2 = lupte (fara volan/pedala, controller digital) */
static int padDigital = 0;
static int stickDpad = 0, rStrafe = 0, axModern = 0, swapSticks = 0, invRY = 0;
static int padMap[16] = {0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15}, padMapOn = 0;   /* #mapa=fizic>joc,... (butoanele manetei mutate pe alte functii) */   /* din .cht: #stick=dpad, #stickdreapta=strafe, #axe=modern */
static volatile double padPS = 0;   /* butonul PS al DualShock 3 tinut (iesire) */
static volatile int wTri = 0, wCircle = 0, wSel = 0, wL2 = 0; static volatile double wAimUp = 0, wAimDn = 0;
/* butonul VOCE la Worms: apasare scurta = Triunghi (saritura inainte), dublu click sau tinut apasat = Cerc (saritura inapoi).
   Apasarea scurta asteapta 0.3 s sa vada daca vine al doilea click. */
static volatile double vDown = 0, vUp = 0, tTriUntil = 0, tCirUntil = 0; static volatile int vPend = 0;
static void voice_ev(int c) {
  double t = now();
  if (c == 2 || (c == 1 && vPend && t - vUp < 0.3)) { tCirUntil = t + 0.15; vPend = 0; vDown = 0; L("voce: dublu -> Cerc"); return; }
  if (c == 1) { vDown = t; return; }
  if (c == 0) { if (vDown > 0) { if (t - vDown >= 0.5) { tCirUntil = t + 0.15; L("voce: tinut -> Cerc"); } else { vPend = 1; vUp = t; } } vDown = 0; return; }
  if (vDown > 0) { tCirUntil = t + 0.15; vDown = 0; vPend = 0; L("voce: tinut -> Cerc"); }   /* 3/6/7 = tinut apasat */
}
static void voice_tick(void) {
  double t = now();
  if (vPend && t - vUp >= 0.3) { vPend = 0; tTriUntil = t + 0.15; }
  if (vDown > 0 && t - vDown >= 0.5) { vDown = 0; vPend = 0; tCirUntil = t + 0.15; }
}
static volatile int bX = 0, bCircle = 0, bTri = 0, bStart = 0, bR1 = 0, bL1 = 0, autoGas = 0, dUp = 0, dDown = 0, dLeft = 0, dRight = 0;
static volatile double tUp = 0, tDown = 0, tCircle = 0;  /* apasari scurte (rotita, trackpad) */
static volatile int menuNav = 0, joyDir = 0;                          /* in lista de jocuri: -1/+1 pasi, 2 = alege */
static volatile double backDown = 0, menuHold = 0, padSS = 0; static volatile int inList = 1; static void (*usb_stop)(void) = 0; static volatile int quitReq = 0; static double msgT = -10; static char msg[64] = "";
#ifdef CORE_SNES
static char sramPath[600] = "";
static unsigned sramSum = 0;
static void sram_load(const char *game) {   /* salvarea din cartus (SRAM): <saves>/<nume>.srm */
  const char *sd = getenv("MIB_SAVE"); const char *b = strrchr(game, '/'); b = b ? b + 1 : game;
  snprintf(sramPath, sizeof sramPath, "%s/%s", sd ? sd : "/tmp", b); char *e = strrchr(sramPath, '.'); if (e) strcpy(e, ".srm"); else strcat(sramPath, ".srm");
  size_t n = retro_get_memory_size(RETRO_MEMORY_SAVE_RAM); unsigned char *m = retro_get_memory_data(RETRO_MEMORY_SAVE_RAM);
  if (!n || !m) { sramPath[0] = 0; return; }
  FILE *f = fopen(sramPath, "rb"); if (f) { size_t k = fread(m, 1, n, f); fclose(f); L("SRAM incarcat %u/%u", (unsigned)k, (unsigned)n); }
  unsigned s = 0; for (size_t i = 0; i < n; i++) s = s * 31 + m[i]; sramSum = s;
}
static void sram_save(void) {
  if (!sramPath[0]) return; size_t n = retro_get_memory_size(RETRO_MEMORY_SAVE_RAM); unsigned char *m = retro_get_memory_data(RETRO_MEMORY_SAVE_RAM);
  if (!n || !m) return; unsigned s = 0; for (size_t i = 0; i < n; i++) s = s * 31 + m[i]; if (s == sramSum) return;
  FILE *f = fopen(sramPath, "wb"); if (f) { fwrite(m, 1, n, f); fclose(f); sramSum = s; L("SRAM salvat"); }
}
#else
static void sram_save(void) { }
#endif
static void hard_exit(const char *why, int rev) {
  alarm(4);
  FILE *f = fopen("/tmp/racer_exit", "w"); if (f) { fprintf(f, "%s %d\n", why, rev); fclose(f); }
  L("IESIRE: %s rev=%d", why, rev); sram_save();
#ifndef DESKTOP
  if (!rev) system("/net/mmx/eso/bin/apps/dmdt sc 0 0 >/dev/null 2>&1 &");   /* intai interfata MMI inapoi */
  { int s = socket(AF_INET, SOCK_DGRAM, 0); struct sockaddr_in a; memset(&a, 0, sizeof a); a.sin_family = AF_INET; a.sin_port = htons(47012); a.sin_addr.s_addr = htonl(0x7f000001);
    sendto(s, "S0", 2, 0, (struct sockaddr*)&a, sizeof a); }
#endif
  if (usb_stop) usb_stop();             /* apoi eliberez controller-ul USB */
  _exit(0);
}
/* inapoi in lista de jocuri: lansatorul reporneste emulatorul; ecranul si blocarea butoanelor raman active */
static void back_to_list(const char *why) {
  alarm(3);                             /* plasa de siguranta: daca ceva se blocheaza, procesul se inchide oricum in 3 s */
  FILE *f = fopen("/tmp/racer_exit", "w"); if (f) { fprintf(f, "lista %s\n", why); fclose(f); }
  L("INAPOI LA LISTA: %s", why); sram_save();
  if (usb_stop) usb_stop();
  _exit(0);
}
static double num(const char *s, const char *k, double d) { const char *p = strstr(s, k); return p ? atof(p + strlen(k)) : d; }
static int keylog = 0;
static volatile float gain = 1.0f;
/* Volumul jocului urmeaza volumul masinii: puntea trimite "A updateVolume a b v f" (DSISound).
 * Nu stim dinainte ce pereche (a,b) e volumul pe care il schimba butonul, asa ca o invat:
 * prima valoare care se schimba imediat dupa rotirea butonului de volum devine cheia, si o tin minte in volum.txt. */
#define NVK 24
static int vkA[NVK], vkB[NVK], vkV[NVK], nvk = 0, volKA = -999, volKB = -999, volMax = 0, vrA[NVK], vrMax[NVK], nvr = 0;
static double tKnob = -10; static char volfile[600] = "";
/* volumul jocului, reglat cu butonul real de volum (consola) si rotita dreapta de pe volan, scara 0..34 ca la masina */
static volatile int volLv = 26; static volatile double tVolShow = -10; static char lvfile[600] = "";
static void lv_gain(void) { float f = volLv / 34.0f; gain = 1.5f * f * sqrtf(f); }   /* 26 = nivelul de pana acum, 34 = +50% */
static void lv_step(int d) { int n = volLv + d; if (n < 0) n = 0; if (n > 34) n = 34; volLv = n; lv_gain(); tVolShow = now();
  if (lvfile[0]) { FILE *vf = fopen(lvfile, "w"); if (vf) { fprintf(vf, "%d\n", n); fclose(vf); } } }
static void vol_apply(int v) {
  int mx = volMax; for (int i = 0; i < nvr; i++) if (vrA[i] == volKA && vrMax[i] > 0) mx = vrMax[i];
  for (int i = 0; i < nvk; i++) if (vkV[i] > mx && vkA[i] == volKA) mx = vkV[i];
  if (mx <= 0) mx = 30;
  float f = v <= 0 ? 0.0f : (float)v / mx; if (f > 1) f = 1;
  gain = f * sqrtf(f); L("volum masina %d/%d -> castig %.3f (cheie %d %d)", v, mx, gain, volKA, volKB);
}
static void vol_msg(const char *m) {
  const char *p; int a, b, v, f;
  if ((p = strstr(m, "updateVolumeRange ")) && sscanf(p + 18, "%d %d %d", &a, &b, &v) == 3) {
    int k; for (k = 0; k < nvr; k++) if (vrA[k] == a) break; if (k == nvr && nvr < NVK) nvr++; if (k < nvr) { vrA[k] = a; vrMax[k] = v > b ? v : b; }
    if (a == volKA) for (int i = 0; i < nvk; i++) if (vkA[i] == volKA && vkB[i] == volKB) vol_apply(vkV[i]);
    return; }
  if (!(p = strstr(m, "updateVolume ")) || sscanf(p + 13, "%d %d %d %d", &a, &b, &v, &f) < 3) return;
  int k; for (k = 0; k < nvk; k++) if (vkA[k] == a && vkB[k] == b) break;
  int changed = k < nvk && vkV[k] != v;
  if (k == nvk) { if (nvk >= NVK) return; nvk++; vkA[k] = a; vkB[k] = b; }
  vkV[k] = v;
  if (!getenv("MIB_SWVOL")) return;     /* implicit: volumul il regleaza masina pe canalul jocului (puntea), fara castig in emulator */
  if (changed && now() - tKnob < 1.5 && (a != volKA || b != volKB)) {
    volKA = a; volKB = b; L("cheie volum invatata: %d %d", a, b);
    if (volfile[0]) { FILE *vf = fopen(volfile, "w"); if (vf) { fprintf(vf, "%d %d\n", a, b); fclose(vf); } } }
  if (a == volKA && b == volKB) vol_apply(v);
}
static void on_msg(char *m) {
  if (m[0] != 'V' && m[0] != 'G' && keylog < 600) { keylog++; L("IN %s", m); }
  if (m[0] == 'A') { if (strstr(m, "updateVolume")) { if (keylog >= 600) L("IN %s", m); vol_msg(m); } return; }
  if (m[0] == 'V') {
    if (strstr(m, "updateDynamicVehicleInfoHighFrequent {")) { double na = num(m, "WheelAngle=", vWheel), tt = now();
      if (tt - wT1 > 0.02) { wPrev = vWheel; wT0 = wT1; wT1 = tt; } vWheel = na; vBrake = num(m, "BrakePressure=", vBrake); vSpeed = num(m, "SpeedValue=", vSpeed); vHFt = now(); }
    else if (strstr(m, "updateDynamicVehicleInfoMidFrequent {")) vGear = (int)num(m, "AutomaticGearShiftTransMode=", vGear);
    else if (!strncmp(m, "V updateAutomaticGearShiftTransMode ", 36)) vGear = atoi(m + 36);
    else if (!strncmp(m, "V updateReverseGear ", 20)) vRev = !strncmp(m + 20, "true", 4);
    return;
  }
  int a, b, c; char bo[8]; int d, e;
  if (m[0] == 'K' && sscanf(m + 2, "%d %d %d", &a, &b, &c) == 3 && a == 5) {   /* joystick-ul rotitei */
    joyDir = (b == 24) ? 1 : (b == 26) ? 2 : (b == 28) ? 3 : (b == 30) ? 4 : 0;   /* din harta: 26 dreapta, 30 stanga */  /* 1 sus 2 dreapta 3 jos 4 stanga */
    if (joyDir == 1) menuNav = -1; else if (joyDir == 3) menuNav = 1;
    return;
  }
  if (m[0] == 'K' && sscanf(m + 2, "%d %d %d", &a, &b, &c) == 3 && a == 4) {   /* butoane volan */
    if (prof == 1) {                     /* Worms: sageti = mers, rotita apasata = X (tii = putere), voce scurt = Triunghi, voce dublu/tinut = Cerc */
      if (b == 57) bCircle = c != 0; else if (b == 46) bBrake = c != 0; else if (b == 47) bRevB = c != 0;
      else if (b == 50) voice_ev(c);       /* voce: scurt = inainte, dublu / tinut = inapoi */
      else L("buton K 4 %d nefolosit", b);  /* telefonul (53) suna oricum, nu il folosim */
      return; }
    if (b == 57) bCircle = c != 0;       /* rotita DREAPTA apasata = Cerc (foloseste obiectul); rotita stanga ramane pentru ceasuri */
    if (b == 46) bBrake = c != 0;        /* sageata dreapta, partea dreapta = frana (Patrat) */
    if (b == 47) bRevB = c != 0;         /* sageata stanga, partea dreapta = marsarier: Patrat + jos, fara acceleratie */
    return;
  }
  if (m[0] == 'K' && sscanf(m + 2, "%d %d %d", &a, &b, &c) == 3) {
    int dn = c == 1;
    if (b == 13) { if (dn) backDown = now(); else if (!c) backDown = 0; }          /* BACK tinut: inapoi la lista */
    if (b == 15 || b == 78) { if (dn) menuHold = now(); else if (!c) menuHold = 0; }  /* MENU tinut: iesire de tot */
    if (b == 13 || b == 15 || b == 78) {
      if (b == 13) { if (prof == 1) wL2 = dn; else bTri = dn; } else bStart = dn; }
    else if (b == 16) { if (prof == 1) wSel = dn; else bX = dn; if (dn) menuNav = 2; }
    else if (b == 98) bL1 = dn;
    else if (b == 97) bR1 = dn;
  } else if (prof == 1 && m[0] == 'E' && sscanf(m + 2, "%d %d %d", &a, &b, &c) == 3 && a == 4 && b == 44) {
    double tt = now(), step = 0.08 * (c < 0 ? -c : c);   /* Worms: rotita dreapta = tintire sus/jos, fiecare treapta ~80 ms de apasare */
    if (c > 0) { wAimDn = 0; wAimUp = (wAimUp > tt ? wAimUp : tt) + step; } else if (c < 0) { wAimUp = 0; wAimDn = (wAimDn > tt ? wAimDn : tt) + step; }
  } else if (m[0] == 'E' && sscanf(m + 2, "%d %d %d", &a, &b, &c) == 3 && ((a == 1 && b == 17) || (a == 4 && b == 44))) {
    tKnob = now(); if (!getenv("MIB_SWVOL")) lv_step(c);   /* butonul de volum (consola) / rotita dreapta -> volumul jocului */
  } else if (m[0] == 'E' && sscanf(m + 2, "%d %d %d", &a, &b, &c) == 3 && b == 16) {
    if (c > 0) { tDown = now(); menuNav = 1; } else if (c < 0) { tUp = now(); menuNav = -1; }
  } else if (m[0] == 'G' && sscanf(m + 2, "%d %d %d %7s %d %d", &a, &b, &c, bo, &d, &e) >= 4) {
    (void)c;   /* trackpad-ul nu mai face nimic in joc (apasari accidentale) */
  }
}
#ifndef DESKTOP
static int us = -1, cs = -1; static struct sockaddr_in beat;
#endif
#ifdef DESKTOP
static void desk_script(void); static void desk_pad(void);
#endif
static void *input_thread(void *x) {
  (void)x; double lb = 0;
  for (;;) {
#ifndef DESKTOP
    char buf[8192]; int n;
    while ((n = recv(us, buf, sizeof buf - 1, 0)) > 0) { buf[n] = 0; on_msg(buf); }
    if (now() - lb > 0.2) { sendto(cs, "S1", 2, 0, (struct sockaddr*)&beat, sizeof beat); lb = now(); }
#else
    double t = now() - t0; vGear = 1; vHFt = now(); vWheel = -60 * sin(t * 0.6); (void)lb;
    if (getenv("MIB_LISTSHOTS")) { }
    else if (getenv("MIB_SCRIPT")) { vWheel = 0; if (t > 3 && t < 3.2) menuNav = 2; desk_script(); desk_pad(); }
    else { if (t > 3 && t < 3.2) menuNav = 2; if (t > 20 && t < 20.2) bX = 1; else bX = 0; }
#endif
    if (vRev || vGear == 2) hard_exit("marsarier", 1);
    if (vGear > 1) hard_exit("treapta", 0);
    if (vSpeed > 0.5) hard_exit("viteza", 0);
    if (menuHold > 0 && now() - menuHold > 1.5) hard_exit("buton MENU", 0);
    if (backDown > 0 && now() - backDown > 1.5) { if (inList) hard_exit("buton BACK", 0); else back_to_list("BACK"); }
    if (padSS > 0 && now() - padSS > 1.5 && !inList) back_to_list("Start+Select");
#ifndef DESKTOP
    if (padPS > 0 && now() - padPS > 1.5) hard_exit("buton PS", 0);
#endif
    usleep(10000);
  }
  return 0;
}
/* treapta vine doar la schimbare; daca jocul a pornit fara sa o fi primit (-1), puntea a verificat deja P la pornire.
   Orice schimbare de treapta ulterioara ajunge aici si inchide jocul; viteza > 0.5 inchide oricum. */
static int ok_drive(void) { return (vGear == 1 || (vGear == -1 && vSpeed < 0.5)) && now() - vHFt < 1.0; }

/* ================= grafica ================= */
static EGLDisplay dpy; static EGLSurface es; static EGLContext ec;
#ifndef DESKTOP
static screen_context_t sctx; static screen_window_t win;
#endif
static int plat_init(void) {
#ifdef DESKTOP
  PFNEGLGETPLATFORMDISPLAYPROC gpd = (void*)eglGetProcAddress("eglGetPlatformDisplay");
  dpy = gpd(0x31DD, EGL_DEFAULT_DISPLAY, NULL); eglInitialize(dpy, 0, 0);
  EGLint ca[] = {EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8,EGL_SURFACE_TYPE,EGL_PBUFFER_BIT,EGL_RENDERABLE_TYPE,EGL_OPENGL_ES2_BIT,EGL_NONE};
  EGLConfig cfg; EGLint n; eglChooseConfig(dpy, ca, &cfg, 1, &n); EGLint pa[] = {EGL_WIDTH,W,EGL_HEIGHT,H,EGL_NONE}; es = eglCreatePbufferSurface(dpy, cfg, pa);
  eglBindAPI(EGL_OPENGL_ES_API); EGLint cta[] = {EGL_CONTEXT_CLIENT_VERSION,2,EGL_NONE};
  ec = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, cta); return !eglMakeCurrent(dpy, es, es, ec);
#else
  if (screen_create_context(&sctx, SCREEN_APPLICATION_CONTEXT)) { L("ctx fail"); return 1; }
  int nd = 0; screen_get_context_property_iv(sctx, SCREEN_PROPERTY_DISPLAY_COUNT, &nd);
  screen_display_t *ds = calloc(nd ? nd : 1, sizeof(*ds)); screen_get_context_property_pv(sctx, SCREEN_PROPERTY_DISPLAYS, (void**)ds);
  screen_create_window(&win, sctx); if (nd) screen_set_window_property_pv(win, SCREEN_PROPERTY_DISPLAY, (void**)&ds[0]);
  int v = SCREEN_FORMAT_RGBX8888; screen_set_window_property_iv(win, SCREEN_PROPERTY_FORMAT, &v);
  v = SCREEN_USAGE_OPENGL_ES2; screen_set_window_property_iv(win, SCREEN_PROPERTY_USAGE, &v);
  int sz[2] = {W, H}; screen_set_window_property_iv(win, SCREEN_PROPERTY_SIZE, sz); screen_set_window_property_iv(win, SCREEN_PROPERTY_BUFFER_SIZE, sz);
  v = 1; screen_set_window_property_iv(win, SCREEN_PROPERTY_VISIBLE, &v);
  screen_set_window_property_cv(win, SCREEN_PROPERTY_ID_STRING, 4, "1234");
  screen_create_window_buffers(win, 2);
  dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY); eglInitialize(dpy, 0, 0);
  EGLint ca[] = {EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8,EGL_SURFACE_TYPE,EGL_WINDOW_BIT,EGL_RENDERABLE_TYPE,EGL_OPENGL_ES2_BIT,EGL_NONE};
  EGLConfig cfg; EGLint n; eglChooseConfig(dpy, ca, &cfg, 1, &n); EGLint cta[] = {EGL_CONTEXT_CLIENT_VERSION,2,EGL_NONE};
  ec = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, cta); es = eglCreateWindowSurface(dpy, cfg, (EGLNativeWindowType)win, NULL);
  eglMakeCurrent(dpy, es, es, ec); eglSwapInterval(dpy, 1); return 0;
#endif
}
static GLuint pcol, ptex, tex; static GLint u_al = -1; static float vb[6 * 20000]; static int nv = 0;
static GLuint mksh(GLenum t, const char *s) { GLuint h = glCreateShader(t); glShaderSource(h, 1, &s, 0); glCompileShader(h); return h; }
static GLuint mkprog(const char *vs, const char *fs) { GLuint p = glCreateProgram(); glAttachShader(p, mksh(GL_VERTEX_SHADER, vs)); glAttachShader(p, mksh(GL_FRAGMENT_SHADER, fs));
  glBindAttribLocation(p, 0, "p"); glBindAttribLocation(p, 1, "c"); glLinkProgram(p); return p; }
static void gl_init(void) {
  pcol = mkprog("attribute vec2 p; attribute vec4 c; varying vec4 v; void main(){ v=c; gl_Position=vec4(p.x/512.0-1.0,1.0-p.y/240.0,0.0,1.0); }",
                "precision mediump float; varying vec4 v; void main(){ gl_FragColor=v; }");
  /* c.z = cat se estompeaza varful (0 = normal); folosit la reflexia iconitelor */
  ptex = mkprog("attribute vec2 p; attribute vec4 c; varying vec2 v; varying float w; void main(){ v=c.xy; w=c.z; gl_Position=vec4(p.x/512.0-1.0,1.0-p.y/240.0,0.0,1.0); }",
                "precision mediump float; varying vec2 v; varying float w; uniform sampler2D t; uniform float al; void main(){ gl_FragColor=texture2D(t,v)*vec4(1.0,1.0,1.0,al*(1.0-w)); }");
  glUseProgram(ptex); u_al = glGetUniformLocation(ptex, "al"); glUniform1f(u_al, 1.0f);
  glGenTextures(1, &tex); glBindTexture(GL_TEXTURE_2D, tex);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 1024, 512, 0, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, 0);
  glEnableVertexAttribArray(0); glEnableVertexAttribArray(1); glViewport(0, 0, W, H);
  glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}
static void flush(void) { if (!nv) return; glUseProgram(pcol); glVertexAttribPointer(0, 2, GL_FLOAT, 0, 24, vb); glVertexAttribPointer(1, 4, GL_FLOAT, 0, 24, vb + 2); glDrawArrays(GL_TRIANGLES, 0, nv); nv = 0; }
static void vtx(float x, float y, float r, float g, float b, float a) { float *p = vb + nv * 6; p[0] = x; p[1] = y; p[2] = r; p[3] = g; p[4] = b; p[5] = a; nv++; }
static void rect(float x, float y, float w, float h, float r, float g, float b, float a) { if (nv > 19990) flush();
  vtx(x,y,r,g,b,a); vtx(x+w,y,r,g,b,a); vtx(x+w,y+h,r,g,b,a); vtx(x,y,r,g,b,a); vtx(x+w,y+h,r,g,b,a); vtx(x,y+h,r,g,b,a); }
static void text(const char *s, float x, float y, float sc, float r, float g, float b) {
  for (; *s; s++, x += 6 * sc) { char ch = *s; if (ch >= 'a' && ch <= 'z') ch -= 32; const char *p = strchr(FONT_CH, ch); if (!p || ch == ' ') continue; int gi = p - FONT_CH;
    for (int rr = 0; rr < 7; rr++) for (int k = 0; k < 5; k++) if (FONT_BM[gi][rr] & (16 >> k)) rect(x + k * sc, y + rr * sc, sc, sc, r, g, b, 1); } }
static void swap(void) { flush(); eglSwapBuffers(dpy, es); }


/* ===== ecran de incarcare: 4 inele care se intrepatrund (in dreapta jos) ===== */
static void ring(float cx, float cy, float R, float th, float al, float glint) {
  const int N = 72; float ri = R - th, ro = R;
  for (int i = 0; i < N; i++) {
    if (nv > 19900) flush();
    float a0 = 6.2831853f * i / N, a1 = 6.2831853f * (i + 1) / N; float c0 = cosf(a0), s0 = sinf(a0), c1 = cosf(a1), s1 = sinf(a1);
    /* argintiu, mai luminos sus; "glint" = o raza de lumina care trece peste inel */
    float l0 = 0.70f - 0.22f * s0 + glint * expf(-((c0 - 0.4f) * (c0 - 0.4f)) * 8.0f) * 0.35f, l1 = 0.70f - 0.22f * s1 + glint * expf(-((c1 - 0.4f) * (c1 - 0.4f)) * 8.0f) * 0.35f;
    if (l0 > 1) l0 = 1; if (l1 > 1) l1 = 1;
    float rr[4] = { ri - 1.0f, ri, ro, ro + 1.0f }, aa[4] = { 0, al, al, 0 };
    for (int b = 0; b < 3; b++) {
      float ra = rr[b], rb = rr[b + 1], A = aa[b], B = aa[b + 1];
      vtx(cx + c0 * ra, cy + s0 * ra, l0, l0, l0 * 1.02f, A); vtx(cx + c1 * ra, cy + s1 * ra, l1, l1, l1 * 1.02f, A); vtx(cx + c1 * rb, cy + s1 * rb, l1, l1, l1 * 1.02f, B);
      vtx(cx + c0 * ra, cy + s0 * ra, l0, l0, l0 * 1.02f, A); vtx(cx + c1 * rb, cy + s1 * rb, l1, l1, l1 * 1.02f, B); vtx(cx + c0 * rb, cy + s0 * rb, l0, l0, l0 * 1.02f, B);
    }
  }
}
static void rings_frame(double t) {   /* t = secunde de la inceput */
  const float R = 22, TH = 4.2f, D = R * 1.42f;           /* distanta intre centre: inelele se suprapun */
  const float X0 = 1024 - 70 - R - 3 * D, Y0 = 480 - 62; /* nu fix in colt */
  glClearColor(0, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT);
  for (int i = 0; i < 4; i++) {
    double u = (t - i * 0.16) / 0.5; if (u < 0) continue; if (u > 1) u = 1;
    float e = 1 - (float)((1 - u) * (1 - u) * (1 - u));          /* ease-out */
    float x = X0 + i * D + (1 - e) * 70;                          /* vine din dreapta si se "agata" de cel dinainte */
    double g = (t - 1.05) / 0.45; float gl = (g > 0 && g < 1) ? (float)sin(g * 3.14159) : 0;
    ring(x, Y0, R, TH, e, gl);
  }
  swap();
}
static void loading_anim(void) {
  double t0 = now();
  for (;;) { double t = now() - t0; rings_frame(t);
#ifdef DESKTOP
    { static int k = 0; double marks[] = { 0.2, 0.45, 0.75, 1.3 }; if (getenv("MIB_LOADSHOTS") && k < 4 && t >= marks[k]) { static unsigned char px[W*H*4]; glReadPixels(0,0,W,H,GL_RGBA,GL_UNSIGNED_BYTE,px);
      char fn[32]; sprintf(fn, "inc%d.ppm", k++); FILE *f = fopen(fn, "wb"); fprintf(f, "P6 %d %d 255\n", W, H); for (int y = H - 1; y >= 0; y--) for (int x = 0; x < W; x++) fwrite(px + (y * W + x) * 4, 1, 3, f); fclose(f); } }
#endif
    if (t > 1.6) break; usleep(16000); }
  rings_frame(10);   /* ramane logo-ul complet cat se incarca jocul */
}

/* ================= libretro ================= */
static unsigned fw = 320, fh = 240; static int haveFrame = 0; static float aspect = 4.0f / 3.0f;
static enum retro_pixel_format pixfmt = RETRO_PIXEL_FORMAT_0RGB1555;
static unsigned short conv[1024 * 512];
static void video_cb(const void *data, unsigned w, unsigned h, size_t pitch) {
  if (!data) return; fw = w; fh = h; if (w > 1024 || h > 512) return;
  glBindTexture(GL_TEXTURE_2D, tex);
  if (pixfmt == RETRO_PIXEL_FORMAT_RGB565) {
    if (pitch == w * 2) glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, data);
    else { for (unsigned y = 0; y < h; y++) memcpy(conv + y * w, (const char*)data + y * pitch, w * 2); glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, conv); }
  } else {  /* 0RGB1555 -> RGB565 */
    for (unsigned y = 0; y < h; y++) { const unsigned short *s = (const unsigned short*)((const char*)data + y * pitch);
      for (unsigned x = 0; x < w; x++) { unsigned short c = s[x]; conv[y * w + x] = ((c & 0x7FE0) << 1) | (c & 0x1F) | ((c >> 4) & 0x20); } }
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, conv);
  }
  haveFrame = 1;
}
static void draw_frame(void) {
  if (!haveFrame) return;
  float dh = H, dw = dh * aspect; if (dw > W) { dw = W; dh = dw / aspect; } float x0 = (W - dw) / 2, y0 = (H - dh) / 2;
  float u = fw / 1024.0f, v = fh / 512.0f;
  float q[] = { x0,y0, 0,0,0,0, x0+dw,y0, u,0,0,0, x0+dw,y0+dh, u,v,0,0, x0,y0, 0,0,0,0, x0+dw,y0+dh, u,v,0,0, x0,y0+dh, 0,v,0,0 };
  glUseProgram(ptex); glBindTexture(GL_TEXTURE_2D, tex);
  glVertexAttribPointer(0, 2, GL_FLOAT, 0, 24, q); glVertexAttribPointer(1, 4, GL_FLOAT, 0, 24, q + 2); glDrawArrays(GL_TRIANGLES, 0, 6);
}
#define AR (44100)                          /* inel: 1 s stereo */
static volatile int g_srate = 0;            /* rata de esantionare ceruta de nucleu (PS1 44100, SNES ~32040) */
static int16_t aring[AR * 2]; static volatile unsigned awr = 0, ard = 0; static int sound_ok = 0;
static size_t audio_batch_cb(const int16_t *d, size_t f) {
  if (!sound_ok) return f;
  for (size_t i = 0; i < f; i++) { unsigned w = awr; if (w - ard >= AR - 1) break; { int l = (int)(d[i * 2] * gain), r = (int)(d[i * 2 + 1] * gain); if (l > 32767) l = 32767; if (l < -32768) l = -32768; if (r > 32767) r = 32767; if (r < -32768) r = -32768;
    aring[(w % AR) * 2] = (int16_t)l; aring[(w % AR) * 2 + 1] = (int16_t)r; } awr = w + 1; }
  return f;
}
static void audio_cb(int16_t l, int16_t r) { int16_t x[2] = {l, r}; audio_batch_cb(x, 1); }
#ifndef DESKTOP
#include <sys/asoundlib.h>
#include <dlfcn.h>
static void *audio_thread(void *x) {
  (void)x;
  for (int i = 0; i < 1000 && !g_srate; i++) usleep(10000); if (!g_srate) g_srate = 44100;
  void *lib = dlopen("libasound.so.2", RTLD_NOW); if (!lib) lib = dlopen("libasound.so", RTLD_NOW);
  if (!lib) { L("sunet: fara libasound (%s)", dlerror()); return 0; }
  int (*p_open)(snd_pcm_t**, int*, int*, int) = dlsym(lib, "snd_pcm_open_preferred");
  int (*p_info)(snd_pcm_t*, snd_pcm_channel_info_t*) = dlsym(lib, "snd_pcm_plugin_info");
  int (*p_params)(snd_pcm_t*, snd_pcm_channel_params_t*) = dlsym(lib, "snd_pcm_plugin_params");
  int (*p_setup)(snd_pcm_t*, snd_pcm_channel_setup_t*) = dlsym(lib, "snd_pcm_plugin_setup");
  int (*p_prep)(snd_pcm_t*, int) = dlsym(lib, "snd_pcm_plugin_prepare");
  ssize_t (*p_write)(snd_pcm_t*, const void*, size_t) = dlsym(lib, "snd_pcm_plugin_write");
  int (*p_status)(snd_pcm_t*, snd_pcm_channel_status_t*) = dlsym(lib, "snd_pcm_plugin_status");
  if (!p_open || !p_info || !p_params || !p_prep || !p_write) { L("sunet: functii lipsa"); return 0; }
  int (*p_openx)(snd_pcm_t**, int, int, int) = dlsym(lib, "snd_pcm_open");
  int (*p_close)(snd_pcm_t*) = dlsym(lib, "snd_pcm_close");
  unsigned (*p_dis)(snd_pcm_t*, unsigned) = dlsym(lib, "snd_pcm_plugin_set_disable");
  const char *want = getenv("MIB_SNDDEV");          /* ex. "6" pentru a forta un dispozitiv */
  int devs[] = {7, 6, 4, 1, 0};   /* 7 = ann2 (singurul care se aude in test, 8 bipuri) */ int nd = sizeof devs / sizeof devs[0];
  snd_pcm_t *h = 0; int card = 0, dev = -1, r = -1;
  snd_pcm_channel_params_t pp;
  for (int k = 0; k < nd; k++) {
    int d = devs[k]; if (want && d != atoi(want)) continue;
    if (d < 0) { int c2 = -1, d2 = -1; r = p_open(&h, &c2, &d2, SND_PCM_OPEN_PLAYBACK); card = c2; dev = d2; }
    else { if (!p_openx) continue; r = p_openx(&h, 0, d, SND_PCM_OPEN_PLAYBACK); card = 0; dev = d; }
    if (r < 0) { L("sunet: dev %d open %d", d, r); continue; }
    if (p_dis) p_dis(h, PLUGIN_DISABLE_MMAP);
    snd_pcm_channel_info_t pi; memset(&pi, 0, sizeof pi); pi.channel = SND_PCM_CHANNEL_PLAYBACK; int ri = p_info(h, &pi);
    L("sunet: dev %d/%d info %d rate %d-%d voices %d-%d frag %d-%d", d, dev, ri, pi.min_rate, pi.max_rate, pi.min_voices, pi.max_voices, pi.min_fragment_size, pi.max_fragment_size);
    int fs = 4096; if (pi.max_fragment_size > 0 && fs > pi.max_fragment_size) fs = pi.max_fragment_size; if (pi.min_fragment_size > fs) fs = pi.min_fragment_size;
    memset(&pp, 0, sizeof pp);
    pp.mode = SND_PCM_MODE_BLOCK; pp.channel = SND_PCM_CHANNEL_PLAYBACK; pp.start_mode = SND_PCM_START_FULL; pp.stop_mode = SND_PCM_STOP_STOP;
    pp.buf.block.frag_size = fs; pp.buf.block.frags_max = -1; pp.buf.block.frags_min = 1;
    pp.format.interleave = 1; pp.format.rate = g_srate; pp.format.voices = 2; pp.format.format = SND_PCM_SFMT_S16_LE;
    r = p_params(h, &pp);
    if (r < 0) { L("sunet: dev %d params %d why %u", d, r, pp.why_failed); if (p_close) p_close(h); h = 0; continue; }
    L("sunet: dev %d OK", d); break;
  }
  if (!h) { L("sunet: niciun dispozitiv"); return 0; }
  int frag = 2048;
  if (p_setup) { snd_pcm_channel_setup_t su; memset(&su, 0, sizeof su); su.channel = SND_PCM_CHANNEL_PLAYBACK; if (p_setup(h, &su) == 0) frag = su.buf.block.frag_size; }
  p_prep(h, SND_PCM_CHANNEL_PLAYBACK);
  L("sunet: card %d dev %d frag %d", card, dev, frag);
  sound_ok = 1; ard = awr;
  static int16_t buf[8192];
  int fr = frag / 4; if (fr > 4096) fr = 4096;
  for (;;) {
    unsigned avail = awr - ard;
    if (avail > (unsigned)(AR / 5)) { ard = awr - 2 * fr; avail = 2 * fr; }   /* intarziere maxima ~0.2 s */
    if (avail < (unsigned)fr) { usleep(3000); continue; }
    for (int i = 0; i < fr; i++) { buf[i * 2] = aring[(ard % AR) * 2]; buf[i * 2 + 1] = aring[(ard % AR) * 2 + 1]; ard++; }
    ssize_t w = p_write(h, buf, fr * 4);
    if (w < fr * 4) { snd_pcm_channel_status_t st; memset(&st, 0, sizeof st); st.channel = SND_PCM_CHANNEL_PLAYBACK;
      if (p_status) p_status(h, &st); p_prep(h, SND_PCM_CHANNEL_PLAYBACK); }
  }
  return 0;
}
#endif
static void input_poll_cb(void) { }
/* pedala de frana = acceleratie progresiva (X pulsat), ridicata = frana (Patrat) 0.6 s */
static unsigned frameNo = 0; static double pedalUpT = -10, pedalLastOn = -10; static int pedalOn = 0;
static void pedal_update(void) {
  double p = vBrake, t = now();
  int on = p > 6;
  if (on) pedalLastOn = t;
  if (pedalOn && !on) pedalUpT = t;
  pedalOn = on;
}
#ifdef DESKTOP
/* test pe PC: MIB_SCRIPT="cadru:buton,..." (S X O T Q U D L R), fiecare apasat 6 cadre; MIB_SHOTS="c1,c2,..." capturi */
static void desk_script(void) {
  static const char *sc; if (!sc) sc = getenv("MIB_SCRIPT");
  int f = (int)frameNo, st = 0, x = 0, o = 0, tr = 0, sq = 0, jd = 0; const char *p = sc;
  while (p && *p) { int fr; char b; if (sscanf(p, "%d:%c", &fr, &b) == 2 && f >= fr && f < fr + 6) {
      if (b == 'S') st = 1; else if (b == 'X') x = 1; else if (b == 'O') o = 1; else if (b == 'T') tr = 1; else if (b == 'Q') sq = 1;
      else if (b == 'U') jd = 1; else if (b == 'R') jd = 2; else if (b == 'D') jd = 3; else if (b == 'L') jd = 4; }
    p = strchr(p, ','); if (p) p++; }
  bStart = st; bX = x; bCircle = o; bTri = tr; bBrake = sq; joyDir = jd;
}
#endif
static int gas_now(void) {
  if (!ok_drive() || !pedalOn) return 0;
  double f = (vBrake - 6) / 70.0; if (f >= 0.9) return 1; if (f < 0) f = 0;
  f = 0.25 + 0.75 * f;                                   /* orice apasare = cel putin 25% */
  int period = 12, onf = 2 * (int)(f * 6 + 0.5); if (onf < 2) onf = 2;   /* in pasi de 2 cadre (jocul citeste la 30 Hz) */
  return (int)(frameNo % period) < onf;
}
/* volan -> stick: extrapolare intre citiri (10/s) + compensare zona moarta a jocului */
static volatile int16_t steer_out = 0; static volatile double steer_est = 0;
static void steer_update(void) {
  double t = now(), a = vWheel, dt = wT1 - wT0;
  if (dt > 0.03 && dt < 0.3) { double vel = (vWheel - wPrev) / dt, ahead = t - wT1; if (ahead > 0.1) ahead = 0.1; a += vel * ahead * 0.8; }
  steer_est = a;
  if (fabs(a) < 2) a = 0; else a -= (a > 0 ? 2 : -2);
  double s = -a / (steerMaxDeg - 2); if (s > 1) s = 1; if (s < -1) s = -1;
  if (s != 0) s = (s > 0 ? 1 : -1) * (antiDz + (1 - antiDz) * fabs(s));
  steer_out = (int16_t)(s * 32767);
}
#ifndef DESKTOP
/* ===== DualShock 3 pe USB (054c:0268), citit direct prin io-usb. Butoanele lui se adauga peste cele din masina. ===== */
#include <errno.h>
/* Citirea directa a controller-ului prin libusbdi (varianta veche, MIB_PADUSB) se compileaza doar cu -DWITH_USBDI.
   Fara ea programul nu contine nimic din biblioteca USB a QNX; controller-ul vine de la ds3d, prin UDP. */
#ifdef WITH_USBDI
#include <sys/usbdi.h>
void *_srealloc(void *p, size_t o, size_t n) { (void)o; return realloc(p, n); }   /* cerut de libusbdi.a */
#endif
static volatile unsigned char padRep[64]; static volatile double padT = 0; static volatile int padOn = 0;
#ifdef WITH_USBDI
typedef struct { struct usbd_device *dev; struct usbd_pipe *ep0, *in, *out; int gen; } ds3_t;
#endif
static volatile int padGen = 0; static volatile int readerAlive = 0;
static volatile int rumStrong = 0, rumWeak = 0;   /* vibratie ceruta de joc (0..255 / 0..1) */
#ifdef WITH_USBDI
static void ds3_out(ds3_t *d, int strong, int weak) {   /* raport de iesire: vibratie + LED 1 */
  unsigned char rep[36] = {0x01, 0x00, 0xff, 0x00, 0xff, 0x00, 0,0,0,0, 0x02, 0xff,0x27,0x10,0x00,0x32, 0xff,0x27,0x10,0x00,0x32, 0xff,0x27,0x10,0x00,0x32, 0xff,0x27,0x10,0x00,0x32, 0,0,0,0,0};
  rep[3] = weak ? 1 : 0; rep[5] = (unsigned char)strong;
  struct usbd_urb *u = usbd_alloc_urb(0); unsigned char *b = usbd_alloc(64); memcpy(b, rep, 36); int r = -1;
  if (d->out) { usbd_setup_interrupt(u, URB_DIR_OUT, b, 36); r = usbd_io(u, d->out, NULL, NULL, 200); }
  if (r != EOK && d->ep0) { memcpy(b, rep, 36); usbd_setup_vendor(u, URB_DIR_OUT, 0x09, USB_TYPE_CLASS | USB_RECIPIENT_INTERFACE, 0x0201, 0, b, 36); r = usbd_io(u, d->ep0, NULL, NULL, 200); }
  usbd_free(b); usbd_free_urb(u);
}
static void ds3_led(ds3_t *d) {      /* LED 1 aprins: raport de iesire 0x01 pe endpointul de iesire (sau prin SET_REPORT) */
  static const unsigned char rep[36] = {0x01, 0x00,0x00,0x00,0x00,0x00, 0,0,0,0, 0x02, 0xff,0x27,0x10,0x00,0x32, 0xff,0x27,0x10,0x00,0x32, 0xff,0x27,0x10,0x00,0x32, 0xff,0x27,0x10,0x00,0x32, 0,0,0,0,0};
  struct usbd_urb *u = usbd_alloc_urb(0); unsigned char *b = usbd_alloc(64); memcpy(b, rep, 36); int r = -1;
  if (d->out) { usbd_setup_interrupt(u, URB_DIR_OUT, b, 36); r = usbd_io(u, d->out, NULL, NULL, 500); }
  if (r != EOK && d->ep0) { memcpy(b, rep, 36); usbd_setup_vendor(u, URB_DIR_OUT, 0x09, USB_TYPE_CLASS | USB_RECIPIENT_INTERFACE, 0x0201, 0, b, 36); r = usbd_io(u, d->ep0, NULL, NULL, 500); }
  L("pad: LED r=%d", r); usbd_free(b); usbd_free_urb(u);
}
static void *ds3_read(void *x) {
  ds3_t *d = x; struct usbd_urb *urb = usbd_alloc_urb(0); unsigned char *buf = usbd_alloc(64); int bad = 0, n = 0; readerAlive++;
  while (d->gen == padGen && bad < 200) {
    memset(buf, 0, 64); usbd_setup_interrupt(urb, URB_DIR_IN | URB_SHORT_XFER_OK, buf, 64);
    int r = usbd_io(urb, d->in, NULL, NULL, 1000); _Uint32t st = 0, len = 0; usbd_urb_status(urb, &st, &len);
    if (r != EOK) { bad++; usleep(10000); continue; }
    bad = 0; if (len < 26 || buf[0] != 0x01) continue;          /* rapoartele bune incep cu 0x01 */
    { unsigned o2 = padRep[2], o3 = padRep[3];          /* in lista de jocuri: sus/jos = mutare, X = alege */
      if ((buf[2] & 0x10) && !(o2 & 0x10)) menuNav = -1; else if ((buf[2] & 0x40) && !(o2 & 0x40)) menuNav = 1; else if ((buf[3] & 0x40) && !(o3 & 0x40)) menuNav = 2; }
    memcpy((void*)padRep, buf, 26); padT = now(); padOn = 1;
    if (buf[4] & 1) { if (!padPS) padPS = now(); } else padPS = 0;   /* PS tinut 1.5 s = iesire */
    if ((buf[2] & 0x09) == 0x09) { if (!padSS) padSS = now(); } else padSS = 0;   /* Start+Select tinute = inapoi la lista */
    if (++n == 1) { L("pad: primul raport"); ds3_led(d); }
    { static int ls = -1, lw = -1; static double lt = 0; int sN = rumStrong, wN = rumWeak; double tt = now();
      if (sN != ls || wN != lw || ((sN || wN) && tt - lt > 0.5)) { ds3_out(d, sN, wN); ls = sN; lw = wN; lt = tt; } }
  }
  padOn = 0; L("pad: citire oprita (%d rapoarte)", n); readerAlive--; return 0;
}
static void ds3_ins(struct usbd_connection *c, usbd_device_instance_t *in) {
  if (in->ident.vendor != 0x054c || in->ident.device != 0x0268) return;
  ds3_t *d = calloc(1, sizeof *d); int r = usbd_attach(c, in, 0, &d->dev);
  if (r != EOK) { L("pad: attach %d", r); free(d); return; }
  struct usbd_desc_node *n, *inode = 0; usbd_descriptors_t *desc = usbd_parse_descriptors(d->dev, NULL, USB_DESC_DEVICE, 0, &n);
  if (desc) usbd_open_pipe(d->dev, desc, &d->ep0);
  if (!usbd_interface_descriptor(d->dev, 1, 0, 0, &inode)) { usbd_select_config(d->dev, 1); usbd_interface_descriptor(d->dev, 1, 0, 0, &inode); }
  for (int i = 0; i < 8 && inode; i++) { desc = usbd_parse_descriptors(d->dev, inode, USB_DESC_ENDPOINT, i, &n); if (!desc) break;
    if ((desc->endpoint.bmAttributes & 3) != USB_ATTRIB_INTERRUPT) continue;
    if (desc->endpoint.bEndpointAddress & USB_ENDPOINT_IN) { if (!d->in) usbd_open_pipe(d->dev, desc, &d->in); } else if (!d->out) usbd_open_pipe(d->dev, desc, &d->out); }
  if (!d->in || !d->ep0) { L("pad: fara endpointuri"); return; }
  struct usbd_urb *u = usbd_alloc_urb(0); unsigned char *b = usbd_alloc(64);   /* pornire: GET_REPORT 0xF2 */
  usbd_setup_vendor(u, URB_DIR_IN | URB_SHORT_XFER_OK, 0x01, USB_TYPE_CLASS | USB_RECIPIENT_INTERFACE, 0x03F2, 0, b, 17);
  r = usbd_io(u, d->ep0, NULL, NULL, 2000); L("pad: DualShock 3 conectat, pornire r=%d", r); usbd_free(b); usbd_free_urb(u);
  d->gen = ++padGen; pthread_t th; pthread_create(&th, 0, ds3_read, d);
}
static void ds3_rem(struct usbd_connection *c, usbd_device_instance_t *in) { (void)c; if (in->ident.vendor == 0x054c) { padGen++; padOn = 0; L("pad: deconectat"); } }
static struct usbd_connection *usbConn;
static void pad_stop(void) {   /* opresc intai firul de citire (altfel usbd_disconnect in timpul unei citiri = Memory fault) */
  padGen++; padOn = 0;
  for (int i = 0; i < 150 && readerAlive; i++) usleep(10000);
  /* fara usbd_disconnect: se poate bloca (asa s-a pierdut controlul la Start+Select); io-usb elibereaza singur la iesirea procesului */
}
static void *usb_thread(void *x) {
  (void)x; usbd_device_ident_t id = {0x054c, 0x0268, USBD_CONNECT_WILDCARD, USBD_CONNECT_WILDCARD, USBD_CONNECT_WILDCARD};
  static usbd_funcs_t fn = {_USBDI_NFUNCS, ds3_ins, ds3_rem, NULL};
  usbd_connect_parm_t cp; memset(&cp, 0, sizeof cp); cp.path = "/dev/io-usb/io-usb"; cp.vusb = USB_VERSION; cp.vusbd = USBD_VERSION; cp.ident = &id; cp.funcs = &fn; cp.connect_wait = 5;
  int r = usbd_connect(&cp, &usbConn); L("pad: io-usb %d", r);
  if (r == EOK) usb_stop = pad_stop;
  return 0;
}
#endif /* WITH_USBDI */
/* controller-ul vine acum de la ds3d (singurul care tine USB-ul): UDP 47013, "P" + raportul de 26 octeti */
static int rumSock = -1; static struct sockaddr_in rumTo;
static void *pad_rx(void *x) {
  (void)x; int s = socket(AF_INET, SOCK_DGRAM, 0); struct sockaddr_in a; memset(&a, 0, sizeof a);
  a.sin_family = AF_INET; a.sin_port = htons(47013); a.sin_addr.s_addr = htonl(0x7f000001);
  if (bind(s, (struct sockaddr*)&a, sizeof a)) { L("pad: bind 47013 esuat"); return 0; }
  L("pad: astept date de la ds3d"); unsigned char b[64]; int n = 0;
  for (;;) {
    int k = recv(s, b, sizeof b, 0); if (k < 27 || b[0] != 'P') continue;
    unsigned char *buf = b + 1;
    { unsigned o2 = padRep[2], o3 = padRep[3];          /* in lista de jocuri: sus/jos = mutare, X = alege */
      if ((buf[2] & 0x10) && !(o2 & 0x10)) menuNav = -1; else if ((buf[2] & 0x40) && !(o2 & 0x40)) menuNav = 1; else if ((buf[3] & 0x40) && !(o3 & 0x40)) menuNav = 2; }
    memcpy((void*)padRep, buf, 26); padT = now(); padOn = 1;
    /* PS / Start+Select conteaza doar dupa ce au fost eliberate o data (PS-ul tinut de la pornire sau de la iesirea
       din jocul anterior nu mai inchide emulatorul imediat) */
    static int psFree = 0, ssFree = 0;
    if (!(buf[4] & 1)) psFree = 1; if ((buf[2] & 0x09) != 0x09) ssFree = 1;
    if ((buf[4] & 1) && psFree) { if (!padPS) padPS = now(); } else padPS = 0;                       /* PS tinut 1.5 s = iesire */
    if ((buf[2] & 0x09) == 0x09 && ssFree) { if (!padSS) padSS = now(); } else padSS = 0;           /* Start+Select tinute = inapoi la lista */
    if (++n == 1) L("pad: primul raport (prin ds3d)");
  }
}
static int pad_live(void) { return padOn && now() - padT < 0.5; }
/* varianta fara punte (pornita din Meniul Verde): butoanele MMI nu ajung la noi, deci daca nu vine nimic de la controller
   MIB_NOPAD_EXIT secunde la rand, inchid si dau ecranul inapoi MMI-ului */
static void hard_exit(const char *why, int rev);
static void *nopad_watch(void *x) { (void)x; int lim = atoi(getenv("MIB_NOPAD_EXIT")), n = 0; if (lim < 5) lim = 5;
  for (;;) { sleep(1); if (pad_live()) n = 0; else if (++n >= lim) { L("fara controller de %d s -> iesire", lim); hard_exit("fara controller", 0); } } return 0; }
static bool set_rumble(unsigned port, enum retro_rumble_effect e, uint16_t v) {
  if (port) return false; if (e == RETRO_RUMBLE_STRONG) rumStrong = v >> 8; else rumWeak = v ? 1 : 0;
  static int ls = -1, lw = -1; if (rumStrong != ls || rumWeak != lw) { ls = rumStrong; lw = rumWeak;   /* vibratia -> ds3d */
    if (rumSock < 0) { rumSock = socket(AF_INET, SOCK_DGRAM, 0); memset(&rumTo, 0, sizeof rumTo); rumTo.sin_family = AF_INET; rumTo.sin_port = htons(47014); rumTo.sin_addr.s_addr = htonl(0x7f000001); }
    char m[32]; int k = snprintf(m, sizeof m, "R %d %d", ls, lw); sendto(rumSock, m, k, 0, (struct sockaddr*)&rumTo, sizeof rumTo); }
  return true; }
#endif
#ifdef DESKTOP
/* test pe PC: maneta simulata din MIB_PADSCRIPT="cadru1-cadru2:tasta,..." (LU LD LL LR RU RD RL RR = bete, t x p c = Triunghi X Patrat Cerc, l1 r1 l2 r2, su jo st dr = sageti) */
static volatile unsigned char padRep[64] = {0,0,0,0,0,0,128,128,128,128}; static volatile double padT = 0; static volatile int padOn = 1;
static int pad_live(void) { return 1; }
static void desk_pad(void) {
  static const char *sc; if (!sc) sc = getenv("MIB_PADSCRIPT"); unsigned char r[26] = {0}; r[6] = r[7] = r[8] = r[9] = 128;
  const char *p = sc; int f = (int)frameNo;
  while (p && *p) { int a, b; char k[4] = {0}; if (sscanf(p, "%d-%d:%3[A-Za-z0-9]", &a, &b, k) == 3 && f >= a && f < b) {
      if (!strcmp(k, "LU")) r[7] = 0; else if (!strcmp(k, "LD")) r[7] = 255; else if (!strcmp(k, "LL")) r[6] = 0; else if (!strcmp(k, "LR")) r[6] = 255;
      else if (!strcmp(k, "RU")) r[9] = 0; else if (!strcmp(k, "RD")) r[9] = 255; else if (!strcmp(k, "RL")) r[8] = 0; else if (!strcmp(k, "RR")) r[8] = 255;
      else if (!strcmp(k, "t")) r[3] |= 0x10; else if (!strcmp(k, "x")) r[3] |= 0x40; else if (!strcmp(k, "p")) r[3] |= 0x80; else if (!strcmp(k, "c")) r[3] |= 0x20;
      else if (!strcmp(k, "l1")) r[3] |= 0x04; else if (!strcmp(k, "r1")) r[3] |= 0x08; else if (!strcmp(k, "l2")) r[3] |= 0x01; else if (!strcmp(k, "r2")) r[3] |= 0x02;
      else if (!strcmp(k, "su")) r[2] |= 0x10; else if (!strcmp(k, "jo")) r[2] |= 0x40; else if (!strcmp(k, "st")) r[2] |= 0x80; else if (!strcmp(k, "dr")) r[2] |= 0x20;
      else if (!strcmp(k, "S")) r[2] |= 0x08; }
    p = strchr(p, ','); if (p) p++; }
  memcpy((void*)padRep, r, 26);
}
#endif
/* R2 al DS3 = acceleratie progresiva (cat de tare apesi), la jocurile de curse: X pulsat ca la pedala */
static int pad_gas(void) {
  if (!pad_live() || prof != 0) return 0; int p = padRep[19]; if (p < 12) return 0;
  double f = p / 255.0; if (f >= 0.9) return 1; f = 0.25 + 0.75 * f;
  int period = 12, onf = 2 * (int)(f * 6 + 0.5); if (onf < 2) onf = 2; return (int)(frameNo % period) < onf; }
static int stick_v(int k) { return (int)padRep[6 + k] - 128; }   /* 0 LX, 1 LY, 2 RX, 3 RY */
static int pad_raw(unsigned id) {
  unsigned b2 = padRep[2], b3 = padRep[3];
  switch (id) {
    case RETRO_DEVICE_ID_JOYPAD_SELECT: return b2 & 0x01; case RETRO_DEVICE_ID_JOYPAD_L3: return b2 & 0x02; case RETRO_DEVICE_ID_JOYPAD_R3: return b2 & 0x04;
    case RETRO_DEVICE_ID_JOYPAD_START: return b2 & 0x08; case RETRO_DEVICE_ID_JOYPAD_UP: return b2 & 0x10; case RETRO_DEVICE_ID_JOYPAD_RIGHT: return b2 & 0x20;
    case RETRO_DEVICE_ID_JOYPAD_DOWN: return b2 & 0x40; case RETRO_DEVICE_ID_JOYPAD_LEFT: return b2 & 0x80;
    case RETRO_DEVICE_ID_JOYPAD_L2: return b3 & 0x01; case RETRO_DEVICE_ID_JOYPAD_R2: return b3 & 0x02; case RETRO_DEVICE_ID_JOYPAD_L: return b3 & 0x04; case RETRO_DEVICE_ID_JOYPAD_R: return b3 & 0x08;
    case RETRO_DEVICE_ID_JOYPAD_X: return b3 & 0x10; case RETRO_DEVICE_ID_JOYPAD_A: return b3 & 0x20; case RETRO_DEVICE_ID_JOYPAD_B: return b3 & 0x40; case RETRO_DEVICE_ID_JOYPAD_Y: return b3 & 0x80;
  }
  return 0;
}
static int pad_btn(unsigned id) {
  if (!pad_live()) return 0;
  if (stickDpad == 4) {   /* Doom: stick stanga sus/jos = mers, stanga/dreapta = strafe (L1/R1); stick dreapta stanga/dreapta = rotire (sageti) */
    int lx = stick_v(0), ly = stick_v(1), rx = stick_v(2);
    if (id == RETRO_DEVICE_ID_JOYPAD_UP && ly < -60) return 1; if (id == RETRO_DEVICE_ID_JOYPAD_DOWN && ly > 60) return 1;
    if (id == RETRO_DEVICE_ID_JOYPAD_L && lx < -60) return 1; if (id == RETRO_DEVICE_ID_JOYPAD_R && lx > 60) return 1;
    if (id == RETRO_DEVICE_ID_JOYPAD_LEFT && rx < -60) return 1; if (id == RETRO_DEVICE_ID_JOYPAD_RIGHT && rx > 60) return 1; }
  if (stickDpad == 3) {   /* Quake II: stick stanga = mers (Triunghi inainte, X inapoi, Patrat/Cerc lateral) */
    int lx = stick_v(0), ly = stick_v(1);
    if (id == RETRO_DEVICE_ID_JOYPAD_X && ly < -50) return 1; if (id == RETRO_DEVICE_ID_JOYPAD_B && ly > 50) return 1;
    if (id == RETRO_DEVICE_ID_JOYPAD_Y && lx < -50) return 1; if (id == RETRO_DEVICE_ID_JOYPAD_A && lx > 50) return 1; }
  if (stickDpad == 2) {   /* Duke Nukem (digital): stanga = mers + strafe (Patrat tinut + sageata), dreapta = rotire + privire sus/jos (L1/L2) */
    int lx = stick_v(0), ly = stick_v(1), rx = stick_v(2), ry = stick_v(3), sl = lx < -60 || lx > 60;
    if (id == RETRO_DEVICE_ID_JOYPAD_UP && ly < -60) return 1; if (id == RETRO_DEVICE_ID_JOYPAD_DOWN && ly > 60) return 1;
    if (id == RETRO_DEVICE_ID_JOYPAD_Y && sl) return 1;
    if (id == RETRO_DEVICE_ID_JOYPAD_LEFT && (sl ? lx < -60 : rx < -60)) return 1; if (id == RETRO_DEVICE_ID_JOYPAD_RIGHT && (sl ? lx > 60 : rx > 60)) return 1;
    if (id == RETRO_DEVICE_ID_JOYPAD_L && ry < -70) return 1; if (id == RETRO_DEVICE_ID_JOYPAD_L2 && ry > 70) return 1; }
  else if (stickDpad == 1) {   /* stick stanga -> sageti (jocuri digitale, ex. Doom) */
    if (id == RETRO_DEVICE_ID_JOYPAD_UP && stick_v(1) < -60) return 1; if (id == RETRO_DEVICE_ID_JOYPAD_DOWN && stick_v(1) > 60) return 1;
    if (id == RETRO_DEVICE_ID_JOYPAD_LEFT && stick_v(0) < -60) return 1; if (id == RETRO_DEVICE_ID_JOYPAD_RIGHT && stick_v(0) > 60) return 1; }
  if (rStrafe) {     /* stick dreapta stanga/dreapta -> L1/R1 (strafe la Doom) */
    if (id == RETRO_DEVICE_ID_JOYPAD_L && stick_v(2) < -60) return 1; if (id == RETRO_DEVICE_ID_JOYPAD_R && stick_v(2) > 60) return 1; }
  if (padMapOn) { for (int p = 0; p < 16; p++) if (padMap[p] == (int)id && pad_raw(p)) return 1; return 0; }
  return pad_raw(id);
}
static int pad_axis(unsigned index, unsigned id, int16_t *out) {   /* 1 daca stick-ul e miscat (in afara zonei moarte) */
  if (!pad_live()) return 0;
  int right = index == RETRO_DEVICE_INDEX_ANALOG_RIGHT;
  if (stickDpad == 3) {   /* Quake II: vederea jocului (stick-ul lui stang) vine din stick-ul fizic drept, sus/jos normal */
    if (right) { *out = 0; return 0; }
    int v = (int)padRep[8 + (id == RETRO_DEVICE_ID_ANALOG_Y ? 1 : 0)] - 128; if (id == RETRO_DEVICE_ID_ANALOG_Y) { v = -v; if (v > 127) v = 127; }
    if (v > -10 && v < 10) { *out = 0; return 0; } *out = (int16_t)(v * 256 + (v > 0 ? 255 : 0)); return 1; }
  if (swapSticks) right = !right;                                    /* betele schimbate intre ele */
  if (axModern && id == RETRO_DEVICE_ID_ANALOG_X) right = !right;   /* axele X schimbate intre bete: stanga = mers + strafe, dreapta = rotire + privire */
  int v = padRep[6 + (right ? 2 : 0) + (id == RETRO_DEVICE_ID_ANALOG_Y ? 1 : 0)] - 128;
  if (invRY && right && id == RETRO_DEVICE_ID_ANALOG_Y) { v = -v; if (v > 127) v = 127; }    /* privirea sus/jos inversata (stick-ul fizic din dreapta) */
  if (v > -10 && v < 10) { *out = 0; return 0; }
  *out = (int16_t)(v * 256 + (v > 0 ? 255 : 0)); return 1;
}
static int16_t input_state_car(unsigned port, unsigned device, unsigned index, unsigned id);
static int16_t input_state_cb(unsigned port, unsigned device, unsigned index, unsigned id) {
  if (port != 0) return 0;
  if (device == RETRO_DEVICE_ANALOG) { int16_t v; if (pad_axis(index, id, &v)) return v; return input_state_car(port, device, index, id); }
  return input_state_car(port, device, index, id) || pad_btn(id) || (id == RETRO_DEVICE_ID_JOYPAD_B && pad_gas());
}
static int16_t input_state_car(unsigned port, unsigned device, unsigned index, unsigned id) {
  if (port != 0) return 0;
  double t = now(); int drive = ok_drive();
  if (device == RETRO_DEVICE_ANALOG) {
    if (index == RETRO_DEVICE_INDEX_ANALOG_LEFT && id == RETRO_DEVICE_ID_ANALOG_X) {
      if (!drive || prof != 0) return 0;
      return steer_out; }
    return 0;
  }
  if (prof == 1) {                       /* Worms Armageddon */
    switch (id) {
      case RETRO_DEVICE_ID_JOYPAD_B: return bCircle;                              /* X = foc (tii = putere) */
      case RETRO_DEVICE_ID_JOYPAD_X: voice_tick(); return tTriUntil > t;          /* Triunghi = saritura inainte */
      case RETRO_DEVICE_ID_JOYPAD_A: voice_tick(); return tCirUntil > t;          /* Cerc = saritura inapoi */
      case RETRO_DEVICE_ID_JOYPAD_Y: return joyDir != 0;                          /* Patrat = camera (cu joystick-ul) */
      case RETRO_DEVICE_ID_JOYPAD_SELECT: return wSel;                            /* meniul de arme */
      case RETRO_DEVICE_ID_JOYPAD_START: return bStart;
      case RETRO_DEVICE_ID_JOYPAD_L: return bL1;
      case RETRO_DEVICE_ID_JOYPAD_L2: return wL2;                                 /* BACK = L2 (timp fitil) */
      case RETRO_DEVICE_ID_JOYPAD_R2: return bR1;                                 /* umar dreapta = R2 (schimba viermele) */
      case RETRO_DEVICE_ID_JOYPAD_UP: return wAimUp > t || t - tUp < 0.12 || joyDir == 1;
      case RETRO_DEVICE_ID_JOYPAD_DOWN: return wAimDn > t || t - tDown < 0.12 || joyDir == 3;
      case RETRO_DEVICE_ID_JOYPAD_LEFT: return bRevB || joyDir == 4;
      case RETRO_DEVICE_ID_JOYPAD_RIGHT: return bBrake || joyDir == 2;
    }
    return 0;
  }
  switch (id) {
    case RETRO_DEVICE_ID_JOYPAD_B: return (bX || (prof == 0 && gas_now())) && !(drive && bRevB);                         /* X = acceleratie */
    case RETRO_DEVICE_ID_JOYPAD_Y: return drive && (bBrake || bRevB);                         /* Patrat = frana pe butonul de pe volan */
    case RETRO_DEVICE_ID_JOYPAD_L: return bL1;
    case RETRO_DEVICE_ID_JOYPAD_A: return bCircle || t - tCircle < 0.15;         /* Cerc */
    case RETRO_DEVICE_ID_JOYPAD_X: return bTri;                                  /* Triunghi */
    case RETRO_DEVICE_ID_JOYPAD_START: return bStart;
    case RETRO_DEVICE_ID_JOYPAD_R: return bR1;
    case RETRO_DEVICE_ID_JOYPAD_UP: return t - tUp < 0.12 || joyDir == 1;
    case RETRO_DEVICE_ID_JOYPAD_DOWN: return t - tDown < 0.12 || joyDir == 3 || (drive && bRevB);
    case RETRO_DEVICE_ID_JOYPAD_LEFT: return joyDir == 4;
    case RETRO_DEVICE_ID_JOYPAD_RIGHT: return joyDir == 2;
  }
  return 0;
}
static void log_cb(enum retro_log_level lv, const char *f, ...) { if (!lg) return; va_list a; va_start(a, f); fprintf(lg, "[core %d] ", lv); vfprintf(lg, f, a); va_end(a); fflush(lg); }
static bool env_cb(unsigned cmd, void *data) {
  switch (cmd) {
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY: *(const char**)data = sysdir; return true;
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY: *(const char**)data = savedir; return true;
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: { enum retro_pixel_format f = *(enum retro_pixel_format*)data;
      if (f == RETRO_PIXEL_FORMAT_RGB565 || f == RETRO_PIXEL_FORMAT_0RGB1555) { pixfmt = f; L("pixfmt %d", f); return true; } return false; }
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE: ((struct retro_log_callback*)data)->log = log_cb; return true;
    case RETRO_ENVIRONMENT_GET_CAN_DUPE: *(bool*)data = true; return true;
#ifndef DESKTOP
    case RETRO_ENVIRONMENT_GET_RUMBLE_INTERFACE: ((struct retro_rumble_interface*)data)->set_rumble_state = set_rumble; return true;
#endif
    case RETRO_ENVIRONMENT_GET_VARIABLE: { struct retro_variable *v = data; v->value = NULL;
      if (!strcmp(v->key, "pcsx_rearmed_rgb32_output")) v->value = "disabled";
      else if (!strcmp(v->key, "pcsx_rearmed_spu_thread")) v->value = "disabled";
      else if (!strcmp(v->key, "pcsx_rearmed_show_bios_bootlogo")) v->value = "disabled";
      else if (!strcmp(v->key, "pcsx_rearmed_frameskip_type")) v->value = "auto";
      return v->value != NULL; }
    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE: *(bool*)data = false; return true;
    case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION: *(unsigned*)data = 0; return true;
    case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS: return false;
    case RETRO_ENVIRONMENT_GET_LANGUAGE: *(unsigned*)data = RETRO_LANGUAGE_ENGLISH; return true;
    case RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE: *(int*)data = 1; return true;
    default: return false;
  }
}

/* ================= lista de jocuri ================= */
#define MAXG 64
static char games[MAXG][512]; static char gnames[MAXG][64]; static int ng = 0; static int gsnes[MAXG];
/* numele aratat in lista: jocurile de PlayStation primesc sufixul PSX, cele de Super Nintendo SNES; porturile raman cum sunt */
static const char *gdisp(int i) { static char b[MAXG][72]; if (!b[i][0]) snprintf(b[i], sizeof b[i], gsnes[i] == 2 ? "%s" : gsnes[i] ? "%s SNES" : "%s PSX", gnames[i]); return b[i]; }
static int is_nat(const char *n) { const char *e = strrchr(n, '.'); return e && !strcasecmp(e, ".nat"); }   /* joc nativ: fisierul e scriptul de pornire */
static int is_snes(const char *n) { const char *e = strrchr(n, '.'); return e && (!strcasecmp(e, ".sfc") || !strcasecmp(e, ".smc")); }
static int is_game(const char *n) { const char *e = strrchr(n, '.'); if (!e) return 0; return !strcasecmp(e, ".cue") || !strcasecmp(e, ".pbp") || !strcasecmp(e, ".iso") || !strcasecmp(e, ".img") || !strcasecmp(e, ".chd"); }
static void scan_dir(const char *d, int depth) {
  DIR *dd = opendir(d); if (!dd) return; struct dirent *e;
  while ((e = readdir(dd)) && ng < MAXG) { if (e->d_name[0] == '.') continue; char p[512]; snprintf(p, sizeof p, "%s/%s", d, e->d_name);
    struct stat st; if (stat(p, &st)) continue;
    if (S_ISDIR(st.st_mode)) { if (depth > 0) scan_dir(p, depth - 1); }
    else if (is_game(e->d_name) || is_snes(e->d_name) || is_nat(e->d_name)) { gsnes[ng] = is_nat(e->d_name) ? 2 : is_snes(e->d_name); snprintf(games[ng], 512, "%s", p); snprintf(gnames[ng], 64, "%s", e->d_name); char *x = strrchr(gnames[ng], '.'); if (x) *x = 0; L("joc: %s", p); ng++; } }
  closedir(dd);
}
static void find_games(const char *extra) {
  if (extra) scan_dir(extra, 2);
  if (getenv("MIB_SNESDIR")) scan_dir(getenv("MIB_SNESDIR"), 1);   /* jocurile SNES apar in acelasi lansator */
  DIR *fs = opendir("/fs"); if (!fs) return; struct dirent *e;
  while ((e = readdir(fs))) { if (e->d_name[0] == '.') continue; L("/fs/%s", e->d_name);
    const char *sub[] = {"PS1", "ps1", "PSX", "psx", "Ps1"}; for (int i = 0; i < 5; i++) { char p[256]; snprintf(p, sizeof p, "/fs/%s/%s", e->d_name, sub[i]); scan_dir(p, 2); } }
  closedir(fs);
}
/* ordinea din lista: <dosarul jocurilor>/ordine.txt, cate un nume pe rand (cum apare in lista); ce nu e trecut acolo ramane la sfarsit, in ordinea gasita */
static void order_games(const char *dir) {
  if (!dir || ng < 2) return; char p[512]; snprintf(p, sizeof p, "%s/ordine.txt", dir); FILE *f = fopen(p, "r"); if (!f) return;
  static int rank[MAXG]; for (int i = 0; i < ng; i++) rank[i] = 10000 + i; char ln[128]; int r = 0;
  while (fgets(ln, sizeof ln, f)) { ln[strcspn(ln, "\r\n")] = 0; if (!ln[0] || ln[0] == '#') continue; for (int i = 0; i < ng; i++) if (!strcasecmp(gnames[i], ln) && rank[i] >= 10000) { rank[i] = r; break; } r++; }
  fclose(f);
  for (int i = 1; i < ng; i++) for (int j = i; j > 0 && rank[j - 1] > rank[j]; j--) { char tg[512], tn[64]; int t;
      strcpy(tg, games[j]); strcpy(games[j], games[j - 1]); strcpy(games[j - 1], tg); strcpy(tn, gnames[j]); strcpy(gnames[j], gnames[j - 1]); strcpy(gnames[j - 1], tn);
      t = gsnes[j]; gsnes[j] = gsnes[j - 1]; gsnes[j - 1] = t; t = rank[j]; rank[j] = rank[j - 1]; rank[j - 1] = t; }
  L("ordine: %s", p);
}
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_NO_HDR
#include "stb_image.h"
static GLuint bgtex = 0; static int haveBezel = 0;
static void load_bg_from(const char *path);
static void load_bg(void) { const char *d = getenv("MIB_BG"); load_bg_from(d ? d : "/fs/sda0/game/psx/background.jpg"); }
static void load_bg_from(const char *path) {
  char p[300]; snprintf(p, sizeof p, "%s", path);
  int w, h, n; unsigned char *px = stbi_load(p, &w, &h, &n, 3); if (!px) { L("fara fundal (%s)", p); return; }
  if (w > 1024) w = 1024; if (h > 512) h = 512;
  static unsigned short t[1024 * 512]; memset(t, 0, sizeof t);
  static const unsigned char bayer[16] = {0,8,2,10,12,4,14,6,3,11,1,9,15,7,13,5};   /* dither: fara inele vizibile in degradeuri */
  for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) { unsigned char *c = px + (y * w + x) * 3; int d = bayer[(y & 3) * 4 + (x & 3)];
    int r = c[0] + (d >> 1), g = c[1] + (d >> 2), b = c[2] + (d >> 1); if (r > 255) r = 255; if (g > 255) g = 255; if (b > 255) b = 255;
    t[y * 1024 + x] = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3); }
  stbi_image_free(px);
  glGenTextures(1, &bgtex); glBindTexture(GL_TEXTURE_2D, bgtex);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 1024, 512, 0, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, t);
  L("fundal %dx%d incarcat", w, h);
}
static void draw_bg(void) {
  if (!bgtex) return; float v = 480.0f / 512;
  float q[] = { 0,0, 0,0,0,0, W,0, 1,0,0,0, W,H, 1,v,0,0, 0,0, 0,0,0,0, W,H, 1,v,0,0, 0,H, 0,v,0,0 };
  glUseProgram(ptex); glBindTexture(GL_TEXTURE_2D, bgtex);
  glVertexAttribPointer(0, 2, GL_FLOAT, 0, 24, q); glVertexAttribPointer(1, 4, GL_FLOAT, 0, 24, q + 2); glDrawArrays(GL_TRIANGLES, 0, 6);
}
static float textw2(const char *s, float sc) { return strlen(s) * 6 * sc - sc; }
/* iconite: langa joc, acelasi nume cu .png/.jpg (max 256x256) */
static GLuint icons[MAXG]; static int iconW[MAXG], iconH[MAXG], iconTried[MAXG];
static void load_icon(int k) {
  if (iconTried[k]) return; iconTried[k] = 1;
  const char *ext[] = {".png", ".jpg"}; unsigned char *px = 0; int w = 0, h = 0, n;
  for (int i = 0; i < 2 && !px; i++) { char p[600]; snprintf(p, sizeof p, "%s", games[k]); char *e = strrchr(p, '.'); if (e) *e = 0; strcat(p, ext[i]); px = stbi_load(p, &w, &h, &n, 4); }
  /* fara imagine proprie: una generica, dupa felul jocului (MIB_ICON_PSX / MIB_ICON_SNES) */
  if (!px && gsnes[k] != 2) { const char *d = getenv(gsnes[k] ? "MIB_ICON_SNES" : "MIB_ICON_PSX"); if (d) px = stbi_load(d, &w, &h, &n, 4); }
  if (!px) return; if (w > 512) w = 512; if (h > 512) h = 512;
  static unsigned char t[512 * 512 * 4]; memset(t, 0, sizeof t);
  for (int y = 0; y < h; y++) memcpy(t + y * 512 * 4, px + y * w * 4, w * 4);
  stbi_image_free(px);
  glGenTextures(1, &icons[k]); glBindTexture(GL_TEXTURE_2D, icons[k]);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 512, 512, 0, GL_RGBA, GL_UNSIGNED_BYTE, t);
  iconW[k] = w; iconH[k] = h; L("iconita %d: %dx%d", k, w, h);
}
static void draw_icon(int k, float cx, float cy, float maxw, float maxh, float alpha) {
  load_icon(k); if (!icons[k]) { text(gdisp(k), cx - textw2(gdisp(k), 3) / 2, cy - 10, 3, alpha, alpha, alpha); flush(); return; }
  float sc = maxw / iconW[k]; if (iconH[k] * sc > maxh) sc = maxh / iconH[k];
  float w = iconW[k] * sc, h = iconH[k] * sc, x0 = cx - w / 2, y0 = cy - h / 2, u = iconW[k] / 512.0f, v = iconH[k] / 512.0f;
  float q[] = { x0,y0, 0,0,0,0, x0+w,y0, u,0,0,0, x0+w,y0+h, u,v,0,0, x0,y0, 0,0,0,0, x0+w,y0+h, u,v,0,0, x0,y0+h, 0,v,0,0 };
  glUseProgram(ptex); glUniform1f(u_al, alpha); glBindTexture(GL_TEXTURE_2D, icons[k]);
  glVertexAttribPointer(0, 2, GL_FLOAT, 0, 24, q); glVertexAttribPointer(1, 4, GL_FLOAT, 0, 24, q + 2); glDrawArrays(GL_TRIANGLES, 0, 6);
  glUniform1f(u_al, 1.0f);
}

/* ===== lansator in stilul meniului MMI: carusel pe elipsa, jocul ales in fata, iconita mare in centru ===== */
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "stb_truetype.h"
static GLuint fontTex = 0, glowTex = 0, logoTex = 0; static int logoW = 0, logoH = 0; static stbtt_bakedchar fch[96]; static float fontPx = 30;
static void font_init(void) {
  const char *fp = getenv("MIB_FONT"); if (!fp) fp = "/fs/sda0/game/psx/font.otf";
  FILE *f = fopen(fp, "rb"); if (!f) { L("font lipsa %s", fp); return; }
  fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET); unsigned char *ttf = malloc(n); fread(ttf, 1, n, f); fclose(f);
  static unsigned char a[512 * 256]; int r = stbtt_BakeFontBitmap(ttf, 0, fontPx, a, 512, 256, 32, 96, fch); free(ttf);
  if (r == 0) { L("font: nu a incaput"); return; }
  unsigned char *rgba = malloc(512 * 256 * 4); for (int i = 0; i < 512 * 256; i++) { rgba[i*4] = rgba[i*4+1] = rgba[i*4+2] = 255; rgba[i*4+3] = a[i]; }
  glGenTextures(1, &fontTex); glBindTexture(GL_TEXTURE_2D, fontTex);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 512, 256, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba); free(rgba);
  /* stralucirea verde de sub obiectul ales (ca la MMI) */
  static unsigned char g[64 * 64 * 4]; for (int y = 0; y < 64; y++) for (int x = 0; x < 64; x++) {
    float dx = (x - 31.5f) / 32, dy = (y - 31.5f) / 32, d = dx * dx + dy * dy, v = d >= 1 ? 0 : (1 - d) * (1 - d);
    unsigned char *q = g + (y * 64 + x) * 4; q[0] = 90; q[1] = 255; q[2] = 60; q[3] = (unsigned char)(v * 255); }
  glGenTextures(1, &glowTex); glBindTexture(GL_TEXTURE_2D, glowTex);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, g);
}
static void tex_quad(GLuint t, float x0, float y0, float w, float h, float u0, float v0, float u1, float v1, float al) {
  float q[] = { x0,y0, u0,v0,0,0, x0+w,y0, u1,v0,0,0, x0+w,y0+h, u1,v1,0,0, x0,y0, u0,v0,0,0, x0+w,y0+h, u1,v1,0,0, x0,y0+h, u0,v1,0,0 };
  glUseProgram(ptex); glUniform1f(u_al, al); glBindTexture(GL_TEXTURE_2D, t);
  glVertexAttribPointer(0, 2, GL_FLOAT, 0, 24, q); glVertexAttribPointer(1, 4, GL_FLOAT, 0, 24, q + 2); glDrawArrays(GL_TRIANGLES, 0, 6); glUniform1f(u_al, 1.0f);
}
static float ttf_w(const char *s, float px) { float x = 0, y = 0, k = px / fontPx; stbtt_aligned_quad q;
  for (; *s; s++) if (*s >= 32 && *s < 128) stbtt_GetBakedQuad(fch, 512, 256, *s - 32, &x, &y, &q, 1); return x * k; }
static void ttf_text(const char *s, float x, float base, float px, float al) {   /* base = linia de baza */
  if (!fontTex) { text(s, x, base - 16, 2, al, al, al); flush(); return; }
  float k = px / fontPx, cx = 0, cy = 0; stbtt_aligned_quad q;
  for (; *s; s++) { if (*s < 32 || *s >= 128) continue; stbtt_GetBakedQuad(fch, 512, 256, *s - 32, &cx, &cy, &q, 1);
    tex_quad(fontTex, x + q.x0 * k, base + q.y0 * k, (q.x1 - q.x0) * k, (q.y1 - q.y0) * k, q.s0, q.t0, q.s1, q.t1, al); }
}
static void logo_load(void) {   /* logo-ul din centru (in locul masinii din MMI): PNG cu transparenta */
  const char *lp = getenv("MIB_LOGO"); if (!lp) lp = "/fs/sda0/game/psx/logo.png";
  int w, h, n; unsigned char *px = stbi_load(lp, &w, &h, &n, 4); if (!px) { L("fara logo (%s)", lp); return; }
  if (w > 512 || h > 512) { stbi_image_free(px); L("logo prea mare"); return; }
  static unsigned char t[512 * 512 * 4]; memset(t, 0, sizeof t);
  for (int y = 0; y < h; y++) memcpy(t + y * 512 * 4, px + y * w * 4, w * 4);
  stbi_image_free(px); logoW = w; logoH = h;
  glGenTextures(1, &logoTex); glBindTexture(GL_TEXTURE_2D, logoTex);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 512, 512, 0, GL_RGBA, GL_UNSIGNED_BYTE, t);
}
static void glow(float cx, float cy, float w, float h, float al) { if (glowTex) tex_quad(glowTex, cx - w / 2, cy - h / 2, w, h, 0, 0, 1, 1, al); }
static void frame_box(float x, float y, float w, float h, float a) {   /* chenar subtire verde (verdele MMI) in jurul numelui ales */
  const float R = 0.35f, G = 0.95f, B = 0.25f;
  rect(x, y, w, 2, R, G, B, a); rect(x, y + h - 2, w, 2, R, G, B, a); rect(x, y, 2, h, R, G, B, a); rect(x + w - 2, y, 2, h, R, G, B, a); }
/* reflexia stearsa a iconitei pe podea, ca la meniul MMI: imagine intoarsa sub iconita, care se stinge in jos */
static void draw_icon_refl(int k, float cx, float cy, float maxw, float maxh, float alpha) {
  load_icon(k); if (!icons[k]) return;
  float sc = maxw / iconW[k]; if (iconH[k] * sc > maxh) sc = maxh / iconH[k];
  float w = iconW[k] * sc, h = iconH[k] * sc, x0 = cx - w / 2, yb = cy + h / 2 + 1, rh = h * 0.55f;
  float u = iconW[k] / 512.0f, vb_ = iconH[k] / 512.0f, vt = vb_ * (1 - 0.55f);
  float a0 = 1 - 0.38f, a1 = 1;     /* sus 38% vizibil, jos dispare */
  float q[] = { x0,yb, 0,vb_,a0,0, x0+w,yb, u,vb_,a0,0, x0+w,yb+rh, u,vt,a1,0, x0,yb, 0,vb_,a0,0, x0+w,yb+rh, u,vt,a1,0, x0,yb+rh, 0,vt,a1,0 };
  glUseProgram(ptex); glUniform1f(u_al, alpha); glBindTexture(GL_TEXTURE_2D, icons[k]);
  glVertexAttribPointer(0, 2, GL_FLOAT, 0, 24, q); glVertexAttribPointer(1, 4, GL_FLOAT, 0, 24, q + 2); glDrawArrays(GL_TRIANGLES, 0, 6);
  glUniform1f(u_al, 1.0f);
}
static int pick_game(void) {
  int sel = 0; menuNav = 0; load_bg(); inList = 1; font_init(); logo_load();
  { FILE *lf = fopen("/tmp/psx_last", "r"); char ln[600]; if (lf) { if (fgets(ln, sizeof ln, lf)) { ln[strcspn(ln, "\n")] = 0; for (int i = 0; i < ng; i++) if (!strcmp(games[i], ln)) sel = i; } fclose(lf); } }
#ifdef DESKTOP
  double tshot = now() + 1.5; int shot = 0;
#endif
  /* caruselul: pozitia continua 'pos' urmareste selectia (cu trecere peste capat), animat */
  float pos = sel; int selc = sel; double tl = now(); float heroA = 1; int heroK = sel, heroPrev = -1;
  const float CX = 212, CY = 205, RX = 393, RY = 135;     /* elipsa meniului MMI original */
  const float LX = 230, LY = 205, LH = 180;               /* logo separat (doar daca nu e deja in fundal) */
  for (;;) {
    int m = menuNav; menuNav = 0;
    { /* stick-ul din stanga muta si el prin lista (cu repetare cat e tinut) */
      static double nx = 0; double tn2 = now(); int ly = (padOn && tn2 - padT < 0.5) ? (int)padRep[7] - 128 : 0;
      if (ly > -70 && ly < 70) nx = 0; else if (!m && tn2 >= nx) { m = ly < 0 ? -1 : 1; nx = tn2 + (nx == 0 ? 0.40 : (ly < -120 || ly > 120) ? 0.11 : 0.20); } }
    if (ng > 0) {
      if (m == 1) selc++; if (m == -1) selc--;
      sel = ((selc % ng) + ng) % ng;
      if (m == 2) return sel;
    }
    double t = now(); float dt = (float)(t - tl); tl = t; if (dt > 0.1f) dt = 0.1f;
    pos += (selc - pos) * (dt * 9 > 1 ? 1 : dt * 9);
    if (sel != heroK) { heroPrev = heroK; heroK = sel; heroA = 0; } heroA += dt * 4; if (heroA > 1) heroA = 1;
    glClearColor(0.02f, 0.02f, 0.025f, 1); glClear(GL_COLOR_BUFFER_BIT);
    draw_bg();
    if (!ng) { ttf_text("Nu am gasit jocuri", 60, 240, 30, 0.9f); swap(); usleep(20000); continue; }
    /* pozitiile din meniul MMI original (masurate pe captura 1024x480): elipsa centru (212,205), raze 393 x 135.
       Sloturi: -1 = elementul dinainte (stanga-jos), 0 = ales (fata), 1..6 = urmatoarele, urcand pe dreapta spre spate. */
    static const float SA[8] = {94.6f, 66.4f, 29.6f, 0.1f, -32.9f, -60.3f, -81.4f, -101.5f};
    static const float SZ[8] = {50, 66, 58, 56, 54, 50, 46, 44};
    static const float SAL[8] = {0.55f, 1, 1, 1, 1, 0.95f, 0.8f, 0.65f};
    static const float SLB[8] = {0, 1, 1, 1, 1, 1, 0, 0};
    static const float SDY[8] = {28, 0, 0, 0, 0, 0, 0, 0};     /* elementul dinainte (stanga-jos) putin mai jos */
    int ord[MAXG]; float sx[MAXG], sy[MAXG], ssz[MAXG], sal[MAXG], slb[MAXG];
    for (int i = 0; i < ng; i++) {
      float d = fmodf(i - pos, (float)ng); if (d < 0) d += ng; if (ng > 1 && d > ng - 1.5f) d -= ng;
      float k = d + 1, fade = 1;                       /* k in 0..7 = index in tabele */
      if (k < 0) { fade = 1 + k; k = 0; } if (k > 7) { fade = 1 - (k - 7); k = 7; } if (fade < 0) fade = 0;
      int k0 = (int)floorf(k); if (k0 > 6) k0 = 6; float f = k - k0; int k1 = k0 + 1;
      float ang = (SA[k0] + (SA[k1] - SA[k0]) * f) * 0.017453f;
      sx[i] = CX + RX * cosf(ang); sy[i] = CY + RY * sinf(ang) + SDY[k0] + (SDY[k1] - SDY[k0]) * f;
      ssz[i] = SZ[k0] + (SZ[k1] - SZ[k0]) * f; sal[i] = (SAL[k0] + (SAL[k1] - SAL[k0]) * f) * fade; slb[i] = (SLB[k0] + (SLB[k1] - SLB[k0]) * f) * fade;
      ord[i] = i; }
    for (int i = 1; i < ng; i++) for (int j = i; j > 0 && sy[ord[j]] < sy[ord[j-1]]; j--) { int tmp = ord[j]; ord[j] = ord[j-1]; ord[j-1] = tmp; }   /* din spate (sus) in fata */
    if (logoTex) { glow(LX, LY + LH * 0.52f, LH * 1.4f, LH * 0.22f, 0.22f); float sc = LH / logoH; tex_quad(logoTex, LX - logoW * sc / 2, LY - LH / 2, logoW * sc, LH, 0, 0, logoW / 512.0f, logoH / 512.0f, 1); }
    for (int n = 0; n < ng; n++) {
      int i = ord[n]; if (sal[i] <= 0.01f) continue;
      float x = sx[i], y = sy[i], sz = ssz[i]; int isSel = (i == sel);
      if (isSel) glow(x, y + sz * 0.50f, sz * 2.4f, sz * 0.70f, 0.85f);   /* lumina verde discreta sub cel ales */
      draw_icon_refl(i, x, y, sz, sz, sal[i]);
      draw_icon(i, x, y, sz, sz, sal[i]);
      if (slb[i] > 0.02f) {   /* eticheta alba in dreapta, ca in MMI; cea aleasa in chenar verde */
        float px = 26, tx = x + sz / 2 + 18, tw = ttf_w(gdisp(i), px);
        if (isSel) { rect(tx - 10, y - 24, tw + 20, 48, 0, 0, 0, 0.30f); frame_box(tx - 10, y - 24, tw + 20, 48, 0.95f); flush(); }
        ttf_text(gdisp(i), tx, y + 9, px, slb[i]);
      }
    }
    flush();
    swap(); usleep(1000);
#ifdef DESKTOP
    { static int ns = 0; static double tn = 0; if (!tn) tn = now() + 1.5;
      if (getenv("MIB_LISTSHOTS") && now() > tn && ns < 4) { char fn[32]; sprintf(fn, "lista%d.ppm", ns); static unsigned char px2[W*H*4]; glReadPixels(0,0,W,H,GL_RGBA,GL_UNSIGNED_BYTE,px2);
        FILE*f=fopen(fn,"wb"); fprintf(f,"P6 %d %d 255\n",W,H); for(int y=H-1;y>=0;y--) for(int x=0;x<W;x++) fwrite(px2+(y*W+x)*4,1,3,f); fclose(f); ns++; tn = now() + 1.0; menuNav = 1; }
      if (getenv("MIB_LISTSHOTS") && ns >= 4) _exit(0); }
    if (!shot && now() > tshot) { shot = 1; static unsigned char px[W*H*4]; glReadPixels(0,0,W,H,GL_RGBA,GL_UNSIGNED_BYTE,px);
      FILE*f=fopen("lista.ppm","wb"); fprintf(f,"P6 %d %d 255\n",W,H); for(int y=H-1;y>=0;y--) for(int x=0;x<W;x++) fwrite(px+(y*W+x)*4,1,3,f); fclose(f); }
#endif
  }
}

/* panouri laterale: stanga = volan, dreapta = pedala */
static void bar_h(float x, float y, float w, float v, float r, float g, float b) {   /* v in -1..1, centrat */
  rect(x, y, w, 14, 0.15f, 0.15f, 0.18f, 1); rect(x + w / 2 - 1, y - 3, 2, 20, 0.6f, 0.6f, 0.6f, 1);
  float c = x + w / 2, e = c + v * w / 2; if (e < c) rect(e, y, c - e, 14, r, g, b, 1); else rect(c, y, e - c, 14, r, g, b, 1); }
/* coduri (format DuckStation simplificat): langa joc, acelasi nume cu .cht. Liniile "#aspect=16:9" schimba formatul.
   Codurile 90AAAAAA VVVVVVVV (scriere 32 biti) sunt impartite in doua scrieri de 16 biti. */
static float cheat_aspect = 0;
static void load_cheats(const char *game) {
  char p[600]; snprintf(p, sizeof p, "%s", game); char *e = strrchr(p, '.'); if (e) strcpy(e, ".cht"); else strcat(p, ".cht");
  FILE *f = fopen(p, "r"); if (!f) { L("fara coduri (%s)", p); return; }
  static char buf[16384]; int n = 0, cnt = 0; char line[256];
  while (fgets(line, sizeof line, f)) {
    if (!strncmp(line, "#aspect=16:9", 12)) { cheat_aspect = 16.0f / 9.0f; continue; }
    if (!strncmp(line, "#controale=worms", 16)) { prof = 1; L("profil controale: worms"); continue; }
    if (!strncmp(line, "#controale=lupta", 16)) { prof = 2; padDigital = 1; L("profil controale: lupta"); continue; }
    if (!strncmp(line, "#mapa=", 6)) {
      static const char *nm[16] = {"x","patrat","select","start","sus","jos","stanga","dreapta","cerc","triunghi","l1","r1","l2","r2","l3","r3"};
      char tmp[256]; snprintf(tmp, sizeof tmp, "%s", line + 6); char *q = strtok(tmp, ", \r\n");
      while (q) { char *g = strchr(q, '>'); if (g) { *g = 0; int a = -1, b = -1; for (int k = 0; k < 16; k++) { if (!strcmp(q, nm[k])) a = k; if (!strcmp(g + 1, nm[k])) b = k; }
          if (a >= 0 && b >= 0) { padMap[a] = b; padMapOn = 1; L("buton %s -> %s", nm[a], nm[b]); } }
        q = strtok(0, ", \r\n"); }
      continue; }
    if (!strncmp(line, "#stick=doom", 11)) { stickDpad = 4; L("bete: stanga mers+strafe, dreapta rotire (Doom)"); continue; }
    if (!strncmp(line, "#stick=quake", 12)) { stickDpad = 3; L("bete: stanga mers (butoanele jocului), dreapta vedere"); continue; }
    if (!strncmp(line, "#stick=duke", 11)) { stickDpad = 2; L("bete: stanga mers+strafe, dreapta rotire+privire"); continue; }
    if (!strncmp(line, "#stick=dpad", 11)) { stickDpad = 1; L("stick stanga -> sageti"); continue; }
    if (!strncmp(line, "#stickdreapta=strafe", 20)) { rStrafe = 1; L("stick dreapta -> L1/R1"); continue; }
    if (!strncmp(line, "#bete=schimbate", 15)) { swapSticks = 1; L("bete schimbate intre ele"); continue; }
    if (!strncmp(line, "#privire=inversata", 18)) { invRY = 1; L("privire sus/jos inversata"); continue; }
    if (!strncmp(line, "#axe=modern", 11)) { axModern = 1; L("axe X schimbate (stanga mers+strafe, dreapta rotire)"); continue; }
    if (!strncmp(line, "#controale=pad", 14)) { prof = 2; padDigital = 0; L("profil controale: pad (DualShock analogic, fara volan/pedala)"); continue; }
    if (!strncmp(line, "#pad=digital", 12)) { padDigital = 1; continue; }
    if (!strncmp(line, "#widescreen=gte", 15)) { extern int gte_widescreen; gte_widescreen = 1; cheat_aspect = 16.0f / 9.0f; L("widescreen GTE (X 3/4)"); continue; }
    unsigned a, v; char vs[32];
    if (line[0] == '#' || line[0] == '[') continue;
    if (sscanf(line, "%x %31s", &a, vs) != 2) continue;
    v = (unsigned)strtoul(vs, 0, 16);
    if ((a >> 24) == 0x90) {                         /* 32 biti -> 2 x 16 biti */
      unsigned ad = 0x80000000u | (a & 0x00FFFFFF);
      n += snprintf(buf + n, sizeof buf - n, "%s%08X %04X+%08X %04X", n ? "+" : "", ad, v & 0xFFFF, ad + 2, v >> 16); cnt += 2;
    } else { n += snprintf(buf + n, sizeof buf - n, "%s%08X %04X", n ? "+" : "", a, v & 0xFFFF); cnt++; }
  }
  fclose(f);
  if (!cnt) L("fara coduri in %s, aspect %.2f", p, cheat_aspect);
  if (cnt) { retro_cheat_set(0, true, buf); L("coduri incarcate: %d din %s, aspect %.2f", cnt, p, cheat_aspect); }
}
/* indicatori discreti: dreapta = acceleratie (de jos in sus), stanga = frana, jos centru = viraj */
static int hud_debug = 0;
static void vbar(float cx, float y, float h, float f, float r, float g, float b) {
  float w = 12; if (f < 0) f = 0; if (f > 1) f = 1;
  rect(cx - w / 2 - 2, y - 2, w + 4, h + 4, 0, 0, 0, 0.35f);
  rect(cx - w / 2, y, w, h, 1, 1, 1, 0.08f);
  if (f > 0) rect(cx - w / 2, y + h * (1 - f), w, h * f, r, g, b, 0.6f);
}
static void hud_min(float x0, float dw) {
  if (prof == 0) {                       /* barele de curse doar la jocurile de curse */
  float lb = x0 / 2, rb = x0 + dw + x0 / 2; if (x0 < 20) { lb = 14; rb = W - 14; }
  float y = 80, h = H - 160;
  float pv = pedalOn ? (float)((vBrake - 6) / 70.0) : 0; if (pv > 1) pv = 1;
  vbar(rb, y, h, pv, 0.45f + 0.45f * pv, 0.85f - 0.2f * pv, 0.55f - 0.4f * pv);       /* menta -> chihlimbar */
  int br = bBrake; vbar(lb, y, h, br ? 1 : 0, 0.85f, 0.35f, 0.32f);
  float sv = steer_out / 32767.0f, cx = W / 2, yy = H - 12, len = sv * dw * 0.35f;
  if (len > 0) rect(cx - 1, yy - 4, len + 2, 8, 0, 0, 0, 0.35f); else rect(cx + len - 1, yy - 4, -len + 2, 8, 0, 0, 0, 0.35f);
  if (len > 0) rect(cx, yy - 2, len, 4, 1, 1, 1, 0.5f); else rect(cx + len, yy - 2, -len, 4, 1, 1, 1, 0.5f);
  rect(cx - 4, yy - 4, 8, 8, 0, 0, 0, 0.4f); rect(cx - 3, yy - 3, 6, 6, 1, 1, 1, 0.7f);
  }
  if (now() - tVolShow < 2.0) {          /* bara de volum, sus pe mijloc, 2 s dupa ce rotesti butonul */
    float bw = 240, bx = W / 2 - bw / 2, by = 18, fv = volLv / 34.0f;
    rect(bx - 3, by - 3, bw + 6, 14, 0, 0, 0, 0.45f); rect(bx, by, bw * fv, 8, 0.95f, 0.95f, 0.95f, 0.75f); }
}
static void debug_panels(void) {
  char b[64]; float L0 = 12, PW = 168, R0 = W - 180;
  text("VOLAN", L0, 40, 2, 0.8f, 0.8f, 0.8f);
  snprintf(b, sizeof b, "REAL %+5.0f", -vWheel); text(b, L0, 64, 2, 1, 1, 1);
  bar_h(L0, 86, PW, (float)(-vWheel / steerMaxDeg > 1 ? 1 : (-vWheel / steerMaxDeg < -1 ? -1 : -vWheel / steerMaxDeg)), 0.3f, 0.6f, 1);
  snprintf(b, sizeof b, "ESTIM %+5.0f", -steer_est); text(b, L0, 116, 2, 1, 1, 1);
  snprintf(b, sizeof b, "JOC %+4d%%", (int)(steer_out * 100 / 32767)); text(b, L0, 140, 2, 1, 0.85f, 0.2f);
  bar_h(L0, 162, PW, steer_out / 32767.0f, 1, 0.75f, 0.2f);
  snprintf(b, sizeof b, "MAX %d GR", (int)steerMaxDeg); text(b, L0, 196, 2, 0.7f, 0.7f, 0.7f);
  double age = now() - wT1; snprintf(b, sizeof b, "DATE %3dMS", (int)(age * 1000 > 999 ? 999 : age * 1000)); text(b, L0, 220, 2, age < 0.2 ? 0.5f : 1, age < 0.2 ? 0.9f : 0.3f, 0.4f);
  text("PEDALA", R0, 40, 2, 0.8f, 0.8f, 0.8f);
  snprintf(b, sizeof b, "FRANA %3.0f", (double)vBrake); text(b, R0, 64, 2, 1, 1, 1);
  float pv = (float)((vBrake - 6) / 70.0); if (pv < 0) pv = 0; if (pv > 1) pv = 1;
  rect(R0, 86, 168, 14, 0.15f, 0.15f, 0.18f, 1); rect(R0, 86, 168 * pv, 14, 0.2f, 0.9f, 0.3f, 1);
  int g = gas_now(); text(g ? "X ACCELERA" : "X -", R0, 116, 2, g ? 0.3f : 0.5f, g ? 1 : 0.5f, g ? 0.4f : 0.5f);
  int br = bBrake; text(br ? "PATRAT FRANA" : "PATRAT -", R0, 140, 2, br ? 1 : 0.5f, br ? 0.4f : 0.5f, br ? 0.3f : 0.5f);
  snprintf(b, sizeof b, "TREAPTA %s", vGear == 1 ? "P" : vGear == 2 ? "R" : vGear == 3 ? "N" : vGear == 4 ? "D" : "?"); text(b, R0, 172, 2, 0.8f, 0.8f, 0.8f);
  const char *jd[] = {"-", "SUS", "DREAPTA", "JOS", "STANGA"}; snprintf(b, sizeof b, "JOY %s", jd[joyDir & 7 ? (joyDir > 4 ? 0 : joyDir) : 0]); text(b, R0, 196, 2, 0.8f, 0.8f, 0.8f);
}
static volatile double g_acc, g_last, g_fps = 60, g_statT; static volatile int g_runs, g_swaps;
#ifdef DESKTOP
static void run_safe(void) { retro_run(); }
#else
void run_safe(void);
#endif
#ifndef DESKTOP
static void crash_sig(int sg) {   /* orice cadere: interfata MMI inapoi, butoanele deblocate */
  FILE *f = fopen("/tmp/racer_exit", "a"); if (f) { fprintf(f, "cadere semnal %d\n", sg); fclose(f); }
  system("/net/mmx/eso/bin/apps/dmdt sc 0 0 >/dev/null 2>&1");
  int s = socket(AF_INET, SOCK_DGRAM, 0); struct sockaddr_in a; memset(&a, 0, sizeof a); a.sin_family = AF_INET; a.sin_port = htons(47012); a.sin_addr.s_addr = htonl(0x7f000001);
  sendto(s, "S0", 2, 0, (struct sockaddr*)&a, sizeof a); _exit(1);
}
#endif
int main(int argc, char **argv) {
  t0 = now(); lg = fopen(getenv("MIB_LOG") ? getenv("MIB_LOG") : "/tmp/psx.log", "w");
#ifndef DESKTOP
  signal(SIGSEGV, crash_sig); signal(SIGBUS, crash_sig); signal(SIGILL, crash_sig); signal(SIGFPE, crash_sig);
#endif
  if (getenv("MIB_STEER")) steerMaxDeg = atof(getenv("MIB_STEER"));
  if (getenv("MIB_ANTIDZ")) antiDz = atof(getenv("MIB_ANTIDZ"));
  if (getenv("MIB_SYS")) snprintf(sysdir, sizeof sysdir, "%s", getenv("MIB_SYS"));
  if (getenv("MIB_SAVE")) snprintf(savedir, sizeof savedir, "%s", getenv("MIB_SAVE"));
  if (getenv("MIB_LVFILE")) { snprintf(lvfile, sizeof lvfile, "%s", getenv("MIB_LVFILE"));
    FILE *vf = fopen(lvfile, "r"); int v; if (vf) { if (fscanf(vf, "%d", &v) == 1 && v >= 0 && v <= 34) volLv = v; fclose(vf); } }
  lv_gain(); L("volum joc %d/34", volLv);
  if (getenv("MIB_VOLFILE")) { snprintf(volfile, sizeof volfile, "%s", getenv("MIB_VOLFILE"));
    FILE *vf = fopen(volfile, "r"); if (vf) { if (fscanf(vf, "%d %d", &volKA, &volKB) == 2) L("cheie volum din fisier: %d %d", volKA, volKB); else volKA = volKB = -999; fclose(vf); } }
  unlink("/tmp/racer_exit");
#ifndef DESKTOP
  us = socket(AF_INET, SOCK_DGRAM, 0); struct sockaddr_in sa; memset(&sa, 0, sizeof sa); sa.sin_family = AF_INET; sa.sin_port = htons(47011); sa.sin_addr.s_addr = htonl(0x7f000001);
  int one = 1; setsockopt(us, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one); L("udp bind %d", bind(us, (struct sockaddr*)&sa, sizeof sa)); fcntl(us, F_SETFL, O_NONBLOCK);
  cs = socket(AF_INET, SOCK_DGRAM, 0); beat = sa; beat.sin_port = htons(47012);
#endif
  pthread_t th; pthread_create(&th, 0, input_thread, 0);
#ifndef DESKTOP
#ifdef WITH_USBDI
  if (getenv("MIB_PADUSB")) { pthread_t ut; pthread_create(&ut, 0, usb_thread, 0); }   /* vechi: citire USB direct */
  else
#endif
  if (!getenv("MIB_NOPAD")) { pthread_t ut; pthread_create(&ut, 0, pad_rx, 0); }
  if (getenv("MIB_NOPAD_EXIT")) { pthread_t wt; pthread_create(&wt, 0, nopad_watch, 0); }   /* fara punte: controller-ul e singura cale de iesire */
#endif
  if (plat_init()) { L("plat fail"); return 1; }
  gl_init(); L("GL %s", glGetString(GL_RENDERER));
  int g = 0;
#ifdef CORE_SNES
  if (!getenv("MIB_GAME")) { L("MIB_GAME lipsa"); hard_exit("fara joc", 0); }
  snprintf(games[0], 512, "%s", getenv("MIB_GAME")); ng = 1; prof = 2; inList = 0; L("SNES: %s", games[0]);
  if (getenv("MIB_BEZEL")) load_bg_from(getenv("MIB_BEZEL"));   /* rama din jurul jocului */
#else
  find_games(argc > 1 ? argv[1] : NULL); order_games(argc > 1 ? argv[1] : NULL); L("jocuri %d", ng);
  g = pick_game(); L("ales %s", games[g]); inList = 0;
  { FILE *lf = fopen("/tmp/psx_last", "w"); if (lf) { fprintf(lf, "%s\n", games[g]); fclose(lf); } }
  if (gsnes[g] == 2) {   /* joc nativ (Duke Nukem 3D...): lansatorul ruleaza scriptul .nat */
    FILE *f = fopen("/tmp/racer_exit", "w"); if (f) { fprintf(f, "nativ %s\n", games[g]); fclose(f); }
    L("joc nativ -> %s", games[g]); _exit(0); }
  if (gsnes[g]) {   /* joc SNES: il porneste lansatorul cu emulatorul SNES; ecranul si blocarea butoanelor raman */
    FILE *f = fopen("/tmp/racer_exit", "w"); if (f) { fprintf(f, "snes %s\n", games[g]); fclose(f); }
    L("joc SNES -> snesfe"); _exit(0); }
#endif
  loading_anim();
  retro_set_environment(env_cb); retro_set_video_refresh(video_cb); retro_set_audio_sample(audio_cb); retro_set_audio_sample_batch(audio_batch_cb);
  retro_set_input_poll(input_poll_cb); retro_set_input_state(input_state_cb);
  retro_init();
#ifndef DESKTOP
  if (!getenv("MIB_NOSOUND")) { pthread_t at; pthread_create(&at, 0, audio_thread, 0); }
#endif
  struct retro_game_info gi; memset(&gi, 0, sizeof gi); gi.path = games[g];
#ifdef CORE_SNES
  { FILE *rf = fopen(games[g], "rb"); if (rf) { fseek(rf, 0, SEEK_END); long n = ftell(rf); fseek(rf, 0, SEEK_SET); void *b = malloc(n); if (fread(b, 1, n, rf) == (size_t)n) { gi.data = b; gi.size = n; } fclose(rf); } }
#endif
  if (!retro_load_game(&gi)) { L("load_game a esuat"); hard_exit("eroare_joc", 0); }
#ifndef DESKTOP
  { const char *ac = getenv("MIB_AUDIOCONN"); char cmd[32]; snprintf(cmd, sizeof cmd, "AREQ %s", ac ? ac : "156");   /* cer managerului audio conexiunea (156 = Android Auto media) */
    sendto(cs, cmd, strlen(cmd), 0, (struct sockaddr*)&beat, sizeof beat); L("trimis %s", cmd); }
#endif
  struct retro_system_av_info av; retro_get_system_av_info(&av); g_fps = av.timing.fps > 1 ? av.timing.fps : 60;
  g_srate = av.timing.sample_rate > 8000 ? (int)(av.timing.sample_rate + 0.5) : 44100; L("sunet: rata %d", g_srate);
  aspect = av.geometry.aspect_ratio > 0.5f ? av.geometry.aspect_ratio : 4.0f / 3.0f;
#ifdef CORE_SNES
  sram_load(games[g]);
#else
  load_cheats(games[g]); if (cheat_aspect > 0) aspect = cheat_aspect;
  {   /* rama in jurul jocului: <joc>_rama.png / .jpg langa joc (doar la jocurile 4:3) */
    const char *ext[] = {"_rama.png", "_rama.jpg"}; for (int i = 0; i < 2 && !haveBezel; i++) {
      char rp[600]; snprintf(rp, sizeof rp, "%s", games[g]); char *e = strrchr(rp, '.'); if (e) *e = 0; strcat(rp, ext[i]);
      FILE *rf = fopen(rp, "rb"); if (rf) { fclose(rf); bgtex = 0; load_bg_from(rp); haveBezel = bgtex != 0; L("rama: %s -> %d", rp, haveBezel); } } }
#endif
  if (padDigital) { retro_set_controller_port_device(0, RETRO_DEVICE_JOYPAD); L("controller: digital (fara analog)"); }
  else retro_set_controller_port_device(0, RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_ANALOG, 1));  /* DualShock */
  hud_debug = getenv("MIB_DEBUG") != NULL;
  L("fps joc %.2f aspect %.2f", g_fps, aspect);
  /* variabilele de timp stau in memorie (volatile): codul de emulare in asamblare poate strica registrele VFP */
  g_acc = 0; g_last = now(); g_statT = now(); g_runs = 0; g_swaps = 0;
  for (;;) {
    double tn = now(); g_acc = g_acc + (tn - g_last); g_last = tn;
    if (!(g_acc < 0.1)) g_acc = 0.1;   /* prinde si NaN */
    if (!(g_fps > 20 && g_fps < 100)) g_fps = 60;
    int guard = 0;
    while (g_acc >= 1.0 / g_fps && guard++ < 6) { pedal_update(); steer_update(); frameNo++; run_safe(); g_runs = g_runs + 1; g_acc = g_acc - 1.0 / g_fps; }
    glClearColor(0, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT);
#ifdef CORE_SNES
    draw_bg();     /* rama SNES (fundalul cu manerele) */
    { static double ts = 0; if (now() - ts > 15) { ts = now(); sram_save(); } }   /* salvarea jocului, periodic */
#else
    if (haveBezel) draw_bg();   /* rama PS1 (Doom, MK4...) */
#endif
    draw_frame();
    if (now() - msgT < 2) { rect(250, 8, 524, 34, 0, 0, 0, 0.6f); text(msg, 262, 15, 3, 1, 0.85f, 0.2f); }
    { float dh = H, dw = dh * aspect; if (dw > W) dw = W; float x0 = (W - dw) / 2; if (hud_debug) debug_panels(); else hud_min(x0, dw); }
    swap(); g_swaps = g_swaps + 1;
    if (now() - g_statT >= 5) { if (lg) fsync(fileno(lg)); double dt = now() - g_statT; L("emu %.1f fps, ecran %.1f fps, acc %.4f, volan %.0f frana %.0f treapta %d", g_runs / dt, g_swaps / dt, (double)g_acc, vWheel, vBrake, vGear); g_runs = 0; g_swaps = 0; g_statT = now(); }
#ifdef DESKTOP
    { static int fn = 0; fn++; static unsigned lastShot = 0; unsigned sh = 0; const char *ss = getenv("MIB_SHOTS");
      if (ss) { const char *q = ss; unsigned last = 0; while (q && *q) { unsigned v = (unsigned)atoi(q); last = v; if (frameNo >= v && lastShot < v) { sh = v; break; } q = strchr(q, ','); if (q) q++; }
        if (!sh && frameNo > last + 10) hard_exit("desk", 0); }
      else sh = frameNo >= 1500 && lastShot < 1500 ? 1500 : (frameNo >= 900 && lastShot < 900 ? 900 : 0); if (sh) { lastShot = sh; int fn2 = sh; static unsigned char px[W*H*4]; glReadPixels(0,0,W,H,GL_RGBA,GL_UNSIGNED_BYTE,px); char fnm[32]; sprintf(fnm,"psx%d.ppm",fn2);
        FILE*f=fopen(fnm,"wb"); fprintf(f,"P6 %d %d 255\n",W,H); for(int y=H-1;y>=0;y--) for(int x=0;x<W;x++) fwrite(px+(y*W+x)*4,1,3,f); fclose(f);} if (!getenv("MIB_SHOTS") && frameNo > 1550) hard_exit("desk", 0); usleep(16000); }
#endif
  }
}
