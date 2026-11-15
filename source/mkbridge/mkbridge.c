/* SPDX-License-Identifier: MIT - Copyright (C) 2026 the audi-mib-gaming author. See LICENSE. */
/* mkbridge - builds the game bridge jar ON THE HEAD UNIT from the unit's own original xTcpKeyPanel.jar plus the
 * project's own classes, so that no Audi file has to be distributed.
 *
 *   mkbridge check <jar> <classes.zip>            prints ORIGINAL, BRIDGE-SAME, BRIDGE-OTHER or UNKNOWN
 *   mkbridge build <orig.jar> <classes.zip> <out> merges; refuses anything that is not the known original
 *
 * The merge copies every entry of the original jar byte for byte (compressed data untouched), leaves out the entries
 * that classes.zip replaces, appends the entries of classes.zip, and writes a fresh central directory.
 * The result is then read back and verified before the program reports success.
 * Exit codes: 0 ok, 1 usage / I/O error, 2 not the known original, 3 verification of the result failed. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define ORIG_SIZE 16749u
#define ORIG_CRC  0xcaa33d8au        /* CRC-32 of the whole original file */

typedef struct { char name[256]; uint16_t method, mtime, mdate, nlen; uint32_t crc, csize, usize, lho, doff; } ent;
typedef struct { uint8_t *d; uint32_t n; ent *e; int ne; } zipf;

static uint32_t crc_tab[256];
static void crc_init(void) { for (uint32_t i = 0; i < 256; i++) { uint32_t c = i; for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1; crc_tab[i] = c; } }
static uint32_t crc32b(const uint8_t *p, uint32_t n) { uint32_t c = 0xFFFFFFFFu; while (n--) c = crc_tab[(c ^ *p++) & 0xff] ^ (c >> 8); return c ^ 0xFFFFFFFFu; }
static uint16_t r16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t r32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static void w16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void w32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

static int load(const char *path, zipf *z) {
  memset(z, 0, sizeof *z); FILE *f = fopen(path, "rb"); if (!f) { fprintf(stderr, "cannot open %s\n", path); return -1; }
  fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET); if (n < 22 || n > (8 << 20)) { fclose(f); fprintf(stderr, "%s: bad size\n", path); return -1; }
  z->d = malloc((size_t)n); z->n = (uint32_t)n; if (!z->d || fread(z->d, 1, (size_t)n, f) != (size_t)n) { fclose(f); fprintf(stderr, "%s: read error\n", path); return -1; } fclose(f);
  /* end of central directory */
  long eo = -1; for (long i = n - 22; i >= 0 && i >= n - 22 - 65535; i--) if (r32(z->d + i) == 0x06054b50u) { eo = i; break; }
  if (eo < 0) { fprintf(stderr, "%s: not a zip\n", path); return -1; }
  int cnt = r16(z->d + eo + 10); uint32_t cdsize = r32(z->d + eo + 12), cdoff = r32(z->d + eo + 16);
  if ((uint64_t)cdoff + cdsize > z->n || cnt > 2000) { fprintf(stderr, "%s: bad directory\n", path); return -1; }
  z->e = calloc((size_t)cnt + 1, sizeof(ent)); uint32_t p = cdoff;
  for (int i = 0; i < cnt; i++) {
    if (p + 46 > z->n || r32(z->d + p) != 0x02014b50u) { fprintf(stderr, "%s: bad directory entry %d\n", path, i); return -1; }
    ent *e = &z->e[i]; e->method = r16(z->d + p + 10); e->mtime = r16(z->d + p + 12); e->mdate = r16(z->d + p + 14);
    e->crc = r32(z->d + p + 16); e->csize = r32(z->d + p + 20); e->usize = r32(z->d + p + 24);
    e->nlen = r16(z->d + p + 28); uint16_t xl = r16(z->d + p + 30), cl = r16(z->d + p + 32); e->lho = r32(z->d + p + 42);
    if (e->nlen == 0 || e->nlen > 255 || p + 46 + e->nlen > z->n) { fprintf(stderr, "%s: bad name in entry %d\n", path, i); return -1; }
    memcpy(e->name, z->d + p + 46, e->nlen); e->name[e->nlen] = 0;
    if (e->method != 0 && e->method != 8) { fprintf(stderr, "%s: %s: unsupported compression\n", path, e->name); return -1; }
    if ((uint64_t)e->lho + 30 > z->n || r32(z->d + e->lho) != 0x04034b50u) { fprintf(stderr, "%s: %s: bad local header\n", path, e->name); return -1; }
    e->doff = e->lho + 30 + r16(z->d + e->lho + 26) + r16(z->d + e->lho + 28);
    if ((uint64_t)e->doff + e->csize > z->n) { fprintf(stderr, "%s: %s: data out of range\n", path, e->name); return -1; }
    p += 46u + e->nlen + xl + cl;
  }
  z->ne = cnt; return 0;
}
static ent *find(zipf *z, const char *name) { for (int i = 0; i < z->ne; i++) if (!strcmp(z->e[i].name, name)) return &z->e[i]; return NULL; }
static int is_original(zipf *z) { return z->n == ORIG_SIZE && crc32b(z->d, z->n) == ORIG_CRC; }
/* every entry of the classes zip is present with the same content */
static int has_all(zipf *jar, zipf *cls) { for (int i = 0; i < cls->ne; i++) { ent *c = &cls->e[i]; if (c->name[c->nlen - 1] == '/') continue; ent *j = find(jar, c->name); if (!j || j->crc != c->crc || j->usize != c->usize) return 0; } return 1; }

/* output buffer */
static uint8_t *ob; static uint32_t on, ocap;
static void put(const void *p, uint32_t n) { if (on + n > ocap) { ocap = (on + n) * 2 + 4096; ob = realloc(ob, ocap); if (!ob) { fprintf(stderr, "out of memory\n"); exit(1); } } memcpy(ob + on, p, n); on += n; }
typedef struct { ent e; uint32_t newoff; } oent;
static void add(zipf *src, ent *e, oent *out, int *no) {
  uint8_t h[30]; memset(h, 0, sizeof h); w32(h, 0x04034b50u); w16(h + 4, 20); w16(h + 6, 0); w16(h + 8, e->method); w16(h + 10, e->mtime); w16(h + 12, e->mdate);
  w32(h + 14, e->crc); w32(h + 18, e->csize); w32(h + 22, e->usize); w16(h + 26, e->nlen); w16(h + 28, 0);
  out[*no].e = *e; out[*no].newoff = on; (*no)++;
  put(h, 30); put(e->name, e->nlen); put(src->d + e->doff, e->csize);
}

int main(int argc, char **argv) {
  crc_init();
  if (argc == 4 && !strcmp(argv[1], "check")) {
    zipf j, c; if (load(argv[3], &c)) return 1;
    FILE *f = fopen(argv[2], "rb"); if (!f) { printf("MISSING\n"); return 0; } fclose(f);
    if (load(argv[2], &j)) { printf("UNKNOWN\n"); return 0; }
    if (is_original(&j)) printf("ORIGINAL\n"); else if (find(&j, "ro/a5/game/GameBridge.class")) printf(has_all(&j, &c) ? "BRIDGE-SAME\n" : "BRIDGE-OTHER\n"); else printf("UNKNOWN\n");
    return 0;
  }
  if (argc != 5 || strcmp(argv[1], "build")) { fprintf(stderr, "usage: mkbridge check <jar> <classes.zip> | mkbridge build <orig.jar> <classes.zip> <out.jar>\n"); return 1; }
  zipf o, c; if (load(argv[2], &o) || load(argv[3], &c)) return 1;
  if (!is_original(&o)) { fprintf(stderr, "%s is not the known original (size %u, crc %08x)\n", argv[2], (unsigned)o.n, (unsigned)crc32b(o.d, o.n)); return 2; }
  if (c.ne < 5 || !find(&c, "ro/a5/game/GameBridge.class")) { fprintf(stderr, "%s does not look like the bridge classes\n", argv[3]); return 1; }
  /* stored class files in classes.zip must match their own CRC before they go anywhere */
  for (int i = 0; i < c.ne; i++) { ent *e = &c.e[i]; if (e->method != 0 || e->csize != e->usize || crc32b(c.d + e->doff, e->csize) != e->crc) { fprintf(stderr, "%s: %s is damaged\n", argv[3], e->name); return 1; } }
  oent *out = calloc((size_t)(o.ne + c.ne) + 1, sizeof(oent)); int no = 0;
  for (int i = 0; i < o.ne; i++) if (!find(&c, o.e[i].name)) add(&o, &o.e[i], out, &no);
  for (int i = 0; i < c.ne; i++) add(&c, &c.e[i], out, &no);
  uint32_t cdoff = on;
  for (int i = 0; i < no; i++) { ent *e = &out[i].e; uint8_t h[46]; memset(h, 0, sizeof h); w32(h, 0x02014b50u); w16(h + 4, 20); w16(h + 6, 20); w16(h + 10, e->method); w16(h + 12, e->mtime); w16(h + 14, e->mdate);
    w32(h + 16, e->crc); w32(h + 20, e->csize); w32(h + 24, e->usize); w16(h + 28, e->nlen); w32(h + 38, e->name[e->nlen - 1] == '/' ? 0x10u : 0u); w32(h + 42, out[i].newoff); put(h, 46); put(e->name, e->nlen); }
  uint32_t cdsize = on - cdoff; uint8_t t[22]; memset(t, 0, sizeof t); w32(t, 0x06054b50u); w16(t + 8, (uint16_t)no); w16(t + 10, (uint16_t)no); w32(t + 12, cdsize); w32(t + 16, cdoff); put(t, 22);
  FILE *f = fopen(argv[4], "wb"); if (!f || fwrite(ob, 1, on, f) != on || fclose(f)) { fprintf(stderr, "cannot write %s\n", argv[4]); return 1; }
  /* read the result back from disk and verify it entry by entry against the sources */
  zipf r; if (load(argv[4], &r) || r.n != on || memcmp(r.d, ob, on)) { fprintf(stderr, "verification failed: file on disk differs\n"); return 3; }
  if (r.ne != no) { fprintf(stderr, "verification failed: entry count\n"); return 3; }
  for (int i = 0; i < r.ne; i++) { ent *e = &r.e[i]; ent *s = find(&c, e->name); zipf *sz = &c; if (!s) { s = find(&o, e->name); sz = &o; }
    if (!s || s->crc != e->crc || s->csize != e->csize || s->usize != e->usize || s->method != e->method || memcmp(r.d + e->doff, sz->d + s->doff, e->csize)) { fprintf(stderr, "verification failed: %s\n", e->name); return 3; }
    if (e->method == 0 && crc32b(r.d + e->doff, e->csize) != e->crc) { fprintf(stderr, "verification failed: crc of %s\n", e->name); return 3; } }
  for (int i = 0; i < o.ne; i++) if (!find(&r, o.e[i].name)) { fprintf(stderr, "verification failed: %s lost\n", o.e[i].name); return 3; }
  if (!has_all(&r, &c)) { fprintf(stderr, "verification failed: bridge classes\n"); return 3; }
  printf("OK %d entries, %u bytes\n", no, (unsigned)on); return 0;
}
