// zxload: play a WAV into an emulated ZX Spectrum and see what loads.
//
// Real Sinclair/Amstrad ROMs, the cycle-stepped Z80 from floooh/chips (zlib
// licence), EAR driven from the WAV with T-state accurate edges, 50 Hz
// interrupts, the keyboard matrix used to type LOAD "" ENTER (48K) or to pick
// the 128K menu's Tape Loader, optional ULA contention (48K), 128K paging.
// It stops when the CPU halts with interrupts disabled -- the test tapes end
// in DI; HALT -- or a few seconds after the tape ends, and dumps memory.
//
// usage: zxload in.wav out.mem [--128] [--dump128 banks.bin] [--contention]
//               [--speed F] [--extra-seconds S] [--watch ADDR [N]]
//               [--dump-at ADDR file] [--trace-from ADDR --pc-log FILE]
// ROMs are read from $ZXLOAD_ROMS, else from roms/ next to the executable
// (fetch.sh puts them there).
#define CHIPS_IMPL
#include "z80.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

static double CPU_HZ = 3500000.0;   // 3546900 on a 128K
static int FRAME_T = 69888;
static int CONT_START_T = 14335, LINE_T = 224;
#define CONT_START 14335

static uint8_t mem[65536];   // flat view, used for 48K and for dumps
static int is128; static uint8_t rom2[2][16384], ram[8][16384], last7ffd; static int paging_locked;
static inline uint8_t *slot(uint16_t a) {
    if (!is128) return &mem[a];
    switch (a >> 14) {
        case 0: return &rom2[(last7ffd >> 4) & 1][a & 0x3fff];
        case 1: return &ram[5][a & 0x3fff];
        case 2: return &ram[2][a & 0x3fff];
        default: return &ram[last7ffd & 7][a & 0x3fff];
    }
}
static inline uint8_t rd(uint16_t a) { return *slot(a); }
static inline void wr(uint16_t a, uint8_t v) { if (a >= 0x4000) *slot(a) = v; }
static void flat_view(void) { if (is128) for (int a = 0; a < 65536; a++) mem[a] = rd((uint16_t)a); }
static uint8_t keyrows[8];  // bit clear = pressed, like the hardware

// tape
static uint64_t *edges; static size_t nedges; static int level0;
static uint64_t tape_start = UINT64_MAX;
static size_t edge_idx; static int tape_level;

static int tape_level_at(uint64_t t) {
    if (t < tape_start) return 0;
    uint64_t rel = t - tape_start;
    while (edge_idx < nedges && edges[edge_idx] <= rel) { tape_level ^= 1; edge_idx++; }
    return tape_level;
}

static int contention_delay(uint64_t t) {
    static const int pat[8] = {6, 5, 4, 3, 2, 1, 0, 0};
    int ft = (int)(t % FRAME_T);
    if (ft < CONT_START_T || ft >= CONT_START_T + 192 * LINE_T) return 0;
    int lt = (ft - CONT_START_T) % LINE_T;
    if (lt >= 128) return 0;
    return pat[lt & 7];
}

// An "edge file" (tools/channel/channel.py): 'EDGE', u32 initial level,
// u64 count, then u64 T-state times of each level change.  It lets a WAV be
// put through a model of a real playback chain first.
static int load_edges(const char *path, double speed) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    char magic[4]; uint32_t lv; uint64_t n;
    if (fread(magic, 1, 4, f) != 4 || memcmp(magic, "EDGE", 4)) { fclose(f); return 0; }
    if (fread(&lv, 4, 1, f) != 1 || fread(&n, 8, 1, f) != 1) { fclose(f); return 0; }
    edges = malloc(sizeof(uint64_t) * (n + 1));
    if (fread(edges, 8, n, f) != n) { fprintf(stderr, "short edge file\n"); exit(2); }
    fclose(f);
    for (uint64_t i = 0; i < n; i++) edges[i] = (uint64_t)llround(edges[i] * (CPU_HZ / 3500000.0) / speed);
    nedges = n; level0 = (int)lv; tape_level = level0;
    fprintf(stderr, "edges: %zu, %.2f s\n", nedges, n ? edges[n - 1] / CPU_HZ : 0.0);
    return 1;
}

static void load_wav(const char *path, double speed) {
    if (load_edges(path, speed)) return;
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(2); }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(sz); if (fread(b, 1, sz, f) != (size_t)sz) exit(2); fclose(f);
    if (memcmp(b, "RIFF", 4) || memcmp(b + 8, "WAVE", 4)) { fprintf(stderr, "not a wav\n"); exit(2); }
    long p = 12; int ch = 1, bits = 8; unsigned rate = 44100; uint8_t *data = NULL; long dlen = 0;
    while (p + 8 <= sz) {
        uint32_t cl = b[p + 4] | b[p + 5] << 8 | b[p + 6] << 16 | (uint32_t)b[p + 7] << 24;
        if (!memcmp(b + p, "fmt ", 4)) {
            ch = b[p + 10] | b[p + 11] << 8;
            rate = b[p + 12] | b[p + 13] << 8 | b[p + 14] << 16 | (uint32_t)b[p + 15] << 24;
            bits = b[p + 22] | b[p + 23] << 8;
        } else if (!memcmp(b + p, "data", 4)) {
            data = b + p + 8; dlen = cl; if (p + 8 + dlen > sz) dlen = sz - p - 8;
        }
        p += 8 + cl + (cl & 1);
    }
    if (!data) { fprintf(stderr, "no data chunk\n"); exit(2); }
    int bps = bits / 8 * ch; long n = dlen / bps;
    edges = malloc(sizeof(uint64_t) * (n + 1)); nedges = 0;
    int prev = -1;
    for (long i = 0; i < n; i++) {
        int v;  // first channel, signed
        if (bits == 8) v = (int)data[i * bps] - 128;
        else v = (int16_t)(data[i * bps] | data[i * bps + 1] << 8);
        int lv = v > 0 ? 1 : 0;
        if (prev < 0) { prev = lv; level0 = lv; continue; }
        if (lv != prev) {
            edges[nedges++] = (uint64_t)llround((double)i * CPU_HZ / (rate * speed));
            prev = lv;
        }
    }
    fprintf(stderr, "wav: %u Hz %d-bit %dch, %ld samples, %zu edges, %.2f s\n",
            rate, bits, ch, n, nedges, n / (double)rate);
    tape_level = level0;
}

// Keyboard: (row, bit) for the keys we need.
enum { K_J, K_P, K_SYM, K_ENTER };
static const int krow[] = {6, 5, 7, 6};
static const int kbit[] = {3, 0, 1, 0};
static void key(int k, int down) {
    if (down) keyrows[krow[k]] &= ~(1 << kbit[k]); else keyrows[krow[k]] |= 1 << kbit[k];
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: zxload in.wav out.mem [opts]\n"); return 2; }
    int contention = 0; double speed = 1.0, extra = 3.0; int trace_from = -1; const char *pclog = NULL; int itrace=0; const char *preload_path = NULL; int preload_addr = 0, jp_addr = -1; double tape_at_override = -1; const char *dump128 = NULL; int dump_pc = -1; const char *dump_path = NULL; int watch_pc = -1; int watch_max = 50;
    static char romdir[4096];
    if (getenv("ZXLOAD_ROMS")) snprintf(romdir, sizeof romdir, "%s", getenv("ZXLOAD_ROMS"));
    else {
        snprintf(romdir, sizeof romdir, "%s", argv[0]);
        char *slash = strrchr(romdir, '/');
        if (slash) strcpy(slash, "/roms"); else strcpy(romdir, "roms");
    }
    static char rom[4200], r0[4200], r1[4200];
    snprintf(rom, sizeof rom, "%s/amstrad_zx48k.bin", romdir);
    snprintf(r0, sizeof r0, "%s/amstrad_zx128k_0.bin", romdir);
    snprintf(r1, sizeof r1, "%s/amstrad_zx128k_1.bin", romdir);
    for (int i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "--contention")) contention = 1;
        else if (!strcmp(argv[i], "--speed")) speed = atof(argv[++i]);
        else if (!strcmp(argv[i], "--128")) is128 = 1;
        else if (!strcmp(argv[i], "--dump128")) dump128 = argv[++i];
        else if (!strcmp(argv[i], "--preload")) { preload_path = argv[++i]; preload_addr = (int)strtol(argv[++i], NULL, 0); }
        else if (!strcmp(argv[i], "--jp")) jp_addr = (int)strtol(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--itrace")) itrace = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--tape-at")) tape_at_override = atof(argv[++i]);
        else if (!strcmp(argv[i], "--extra-seconds")) extra = atof(argv[++i]);
        else if (!strcmp(argv[i], "--trace-from")) trace_from = (int)strtol(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--pc-log")) pclog = argv[++i];
        else if (!strcmp(argv[i], "--watch")) { watch_pc = (int)strtol(argv[++i], NULL, 0); if (i + 1 < argc && argv[i+1][0] != '-') watch_max = atoi(argv[++i]); }
        else if (!strcmp(argv[i], "--dump-at")) { dump_pc = (int)strtol(argv[++i], NULL, 0); dump_path = argv[++i]; }
    }
    if (is128) {
        FILE *a = fopen(r0, "rb"), *b = fopen(r1, "rb");
        if (!a || !b || fread(rom2[0], 1, 16384, a) != 16384 || fread(rom2[1], 1, 16384, b) != 16384) { fprintf(stderr, "zxload: cannot read the 128K ROMs in %s\n", romdir); return 2; }
        fclose(a); fclose(b); FRAME_T = 70908; CONT_START_T = 14361; LINE_T = 228; CPU_HZ = 3546900.0;
    } else {
        FILE *rf = fopen(rom, "rb"); if (!rf || fread(mem, 1, 16384, rf) != 16384) { fprintf(stderr, "zxload: cannot read %s (run fetch.sh)\n", rom); return 2; } fclose(rf);
    }
    load_wav(argv[1], speed);
    memset(keyrows, 0xff, sizeof keyrows);
    FILE *pl = pclog ? fopen(pclog, "w") : NULL;

    if (preload_path) {
        FILE *pf = fopen(preload_path, "rb"); if (!pf) { fprintf(stderr, "preload\n"); return 2; }
        static uint8_t pbuf[65536]; size_t pn = fread(pbuf, 1, sizeof pbuf, pf); fclose(pf);
        for (size_t i = 0; i < pn && preload_addr + i < 65536; i++) {
            const uint16_t a = (uint16_t)(preload_addr + i);
            if (a >= 0x4000) *slot(a) = pbuf[i];   // banked RAM on a 128K
        }
    }
    z80_t cpu; uint64_t pins = z80_init(&cpu);
    uint64_t t = 0; int int_hold = 0; int wait_left = 0;
    // Script: boot 2.5 s, then type LOAD "" ENTER, then start tape.
    const uint64_t F = FRAME_T;
    struct { uint64_t at; int k, down; } script48[] = {
        {175 * F, K_J, 1}, {179 * F, K_J, 0},
        {185 * F, K_SYM, 1}, {187 * F, K_P, 1}, {191 * F, K_P, 0},
        {195 * F, K_P, 1}, {199 * F, K_P, 0}, {201 * F, K_SYM, 0},
        {207 * F, K_ENTER, 1}, {211 * F, K_ENTER, 0},
    }, script128[] = { {150 * F, K_ENTER, 1}, {155 * F, K_ENTER, 0} };  // "Tape Loader" is the default
    typeof(script48[0]) *script = is128 ? script128 : script48;
    size_t si = 0, ns = is128 ? 2 : sizeof script48 / sizeof script48[0];
    uint64_t tape_len = nedges ? edges[nedges - 1] : 0;
    uint64_t stop_at = 0; int stopped_halt = 0;
    uint64_t prev_m1_pc = 0; int traced = 0;
    if (jp_addr >= 0) {
        pins = z80_prefetch(&cpu, (uint16_t)jp_addr);
        cpu.sp = 0xff48; cpu.i = 0x3f; cpu.iff1 = cpu.iff2 = 0;
        si = ns;  // no typing
        tape_start = (uint64_t)((tape_at_override >= 0 ? tape_at_override : 1.0) * CPU_HZ);
        stop_at = tape_start + tape_len + (uint64_t)(extra * CPU_HZ);
    }
    for (;;) {
        if (si < ns && t >= script[si].at) { key(script[si].k, script[si].down); si++; if (si == ns) { tape_start = t + 25 * F; stop_at = tape_start + tape_len + (uint64_t)(extra * CPU_HZ); } }
        if (stop_at && t >= stop_at) break;
        if (t % F == 0) { pins |= Z80_INT; int_hold = is128 ? 36 : 32; }
        if (int_hold > 0 && --int_hold == 0) pins &= ~Z80_INT;

        pins = z80_tick(&cpu, pins);
        t++;
        if (itrace>0 && z80_opdone(&cpu)) { fprintf(stderr, "I %04x sp=%04x bc=%04x de=%04x hl=%04x a=%02x\n", cpu.pc, cpu.sp, cpu.bc, cpu.de, cpu.hl, cpu.a); if(--itrace==0) break; }

        if ((pins & Z80_HALT) && !cpu.iff1) { stopped_halt = 1; break; }

        if (wait_left > 0) { if (--wait_left > 0) pins |= Z80_WAIT; else pins &= ~Z80_WAIT; }
        if (pins & Z80_MREQ) {
            const uint16_t a = Z80_GET_ADDR(pins);
            if (contention && !(pins & Z80_WAIT) && wait_left == 0 && a >= 0x4000 && a < 0x8000 && (pins & (Z80_RD | Z80_WR))) {
                int d = contention_delay(t);
                if (d) { wait_left = d; pins |= Z80_WAIT; }
            }
            if (pins & Z80_RD) {
                Z80_SET_DATA(pins, rd(a));
                if ((pins & Z80_M1) && pl && trace_from >= 0 && a >= trace_from && a != prev_m1_pc) {
                    if (traced < 2000000) { fprintf(pl, "%llu %04x\n", (unsigned long long)t, a); traced++; }
                }
                if (pins & Z80_M1) prev_m1_pc = a;
                if ((pins & Z80_M1) && a == watch_pc && watch_max > 0) { watch_max--; fprintf(stderr, "W t=%.6f pc=%04x af=%04x bc=%04x de=%04x hl=%04x ix=%04x sp=%04x r=%02x [sp]=%02x%02x\n", t / CPU_HZ, a, cpu.af, cpu.bc, cpu.de, cpu.hl, cpu.ix, cpu.sp, cpu.r, rd((uint16_t)(cpu.sp+1)), rd(cpu.sp)); }
                if ((pins & Z80_M1) && a == dump_pc && dump_path) { flat_view(); FILE *d = fopen(dump_path, "wb"); fwrite(mem, 1, 65536, d); fclose(d); dump_path = NULL; fprintf(stderr, "dumped at %04x t=%.3f\n", a, t / CPU_HZ); }
            } else if (pins & Z80_WR) {
                wr(a, Z80_GET_DATA(pins));
            }
        } else if ((pins & Z80_IORQ) && !(pins & Z80_M1)) {
            const uint16_t port = Z80_GET_ADDR(pins);
            if (!(port & 1)) {
                if (contention && wait_left == 0 && !(pins & Z80_WAIT)) {
                    int d = contention_delay(t);
                    if (d) { wait_left = d; pins |= Z80_WAIT; }
                }
                if (pins & Z80_RD) {
                    uint8_t v = 0x1f, hi = port >> 8;
                    for (int r = 0; r < 8; r++) if (!(hi & (1 << r))) v &= keyrows[r];
                    v |= 0xa0;
                    if (tape_level_at(t)) v |= 0x40;
                    Z80_SET_DATA(pins, v);
                }
            } else if (pins & Z80_RD) {
                Z80_SET_DATA(pins, 0xff);
            } else if ((pins & Z80_WR) && is128 && !(port & 0x8002) && !paging_locked) {
                last7ffd = Z80_GET_DATA(pins);
                if (last7ffd & 0x20) paging_locked = 1;
            }
        } else if ((pins & Z80_IORQ) && (pins & Z80_M1)) {
            Z80_SET_DATA(pins, 0xff);  // IM 2 vector (floating bus)
        }
    }
    flat_view();
    FILE *o = fopen(argv[2], "wb"); fwrite(mem, 1, 65536, o); fclose(o);
    if (dump128) { FILE *d = fopen(dump128, "wb"); fwrite(ram, 1, sizeof ram, d); fclose(d); }
    if (is128) fprintf(stderr, "7ffd=%02x\n", last7ffd);
    if (pl) fclose(pl);
    printf("result=%s pc=%04x t=%.3fs tape_end=%.3fs sp=%04x iff1=%d\n",
           stopped_halt ? "HALT" : "TIMEOUT", cpu.pc, t / CPU_HZ,
           (tape_start + tape_len) / CPU_HZ, cpu.sp, cpu.iff1);
    fprintf(stderr, "regs af=%04x bc=%04x de=%04x hl=%04x ix=%04x iy=%04x af'=%04x bc'=%04x de'=%04x hl'=%04x i=%02x im=%d\n",
            cpu.af, cpu.bc, cpu.de, cpu.hl, cpu.ix, cpu.iy, cpu.af2, cpu.bc2, cpu.de2, cpu.hl2, cpu.i, cpu.im);
    return 0;
}
