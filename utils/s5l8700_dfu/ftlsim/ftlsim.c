/*  Host test of the YP-S3 FTL writer (firmware/target/arm/s5l8700/yps3/
    ftl-yps3.c) against a simulated NAND loaded with a real backup of the
    VFL contexts, FTL meta blocks and log blocks (yps3_backup.py).

    The simulated NAND enforces: no program of a non-erased page, pages of a
    block programmed in increasing order, no write/erase below block 42.
    The test writes random data, "reboots" (ftl_init from the NAND alone)
    and compares every written sector, then checks the context invariants
    the original firmware relies on.

    Build/run: make -C utils/s5l8700_dfu/ftlsim run BACKUP=nanddump/backup */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdbool.h>
#include "ftl-target.h"
#include "system.h"

#define DIES 2
#define BLOCKS 4096
#define PAGES 128
#define PSIZE (4096 + 128)
#define NPAGES (DIES * BLOCKS * PAGES)

static uint8_t *pages[NPAGES];
static int lastprog[DIES * BLOCKS];
bool nand_ll_use_dma, nand_ll_write_enable;
int nand_ll_last_status;
static unsigned long n_prog, n_erase, n_violations, n_read, n_read_bytes;

static void violation(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    printf("NAND VIOLATION: ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
    n_violations++;
}

void nand_ll_init(void) {}
int nand_ll_reset(int bank) { (void)bank; return 0; }
void nand_set_active(void) {}

int nand_ll_read_raw(int bank, unsigned int page, unsigned int col,
                     unsigned char *buf, unsigned int len)
{
    (void)bank;
    if (page >= NPAGES || col + len > PSIZE) return -1;
    n_read++;
    n_read_bytes += len;
    if (pages[page]) memcpy(buf, pages[page] + col, len);
    else memset(buf, 0xff, len);
    return 0;
}

/* fake ECC: 9 bytes of a checksum; decoding always succeeds */
int nand_ll_ecc_correct(void *data, const unsigned char *ecc, bool sector)
{
    (void)data; (void)ecc; (void)sector;
    return 0;
}

int nand_ll_ecc_encode(const void *data, unsigned char *ecc, int size_sel)
{
    const uint8_t *d = data;
    uint32_t h = 2166136261u;
    int i;
    (void)size_sel;
    for (i = 0; i < 512; i++) h = (h ^ d[i]) * 16777619u;
    for (i = 0; i < 9; i++) ecc[i] = h >> (8 * (i % 4)) ^ i;
    return 0;
}

int nand_ll_program_page(int bank, unsigned int page, const unsigned char *data,
                         const unsigned char *spare, unsigned int spare_len)
{
    unsigned int blk = page / PAGES, i;
    (void)bank;
    if (!nand_ll_write_enable) return -3;
    if ((blk % BLOCKS) < 42) { violation("program block %u", blk); return -3; }
    if (pages[page]) {
        for (i = 0; i < PSIZE; i++) if (pages[page][i] != 0xff) break;
        if (i < PSIZE) { violation("program of a written page %u", page); return -2; }
    }
    if ((int)(page % PAGES) <= lastprog[blk])
        violation("page %u programmed after page %d of its block", page % PAGES,
                  lastprog[blk]);
    lastprog[blk] = page % PAGES;
    if (!pages[page]) pages[page] = malloc(PSIZE);
    memset(pages[page], 0xff, PSIZE);
    memcpy(pages[page], data, 4096);
    memcpy(pages[page] + 4096, spare, spare_len);
    n_prog++;
    return 0;
}

int nand_ll_erase_block(int bank, unsigned int page)
{
    unsigned int blk = page / PAGES, p;
    (void)bank;
    if (!nand_ll_write_enable) return -3;
    if ((blk % BLOCKS) < 42) { violation("erase block %u", blk); return -3; }
    for (p = 0; p < PAGES; p++) {
        free(pages[blk * PAGES + p]);
        pages[blk * PAGES + p] = NULL;
    }
    lastprog[blk] = -1;
    n_erase++;
    return 0;
}

/* loads raw blocks: manifest lines "file offset die pblock" */
static void load_backup(const char *dir)
{
    char path[512], file[256];
    unsigned long off;
    int die, pb, n = 0;
    snprintf(path, sizeof(path), "%s/manifest.txt", dir);
    FILE *m = fopen(path, "r");
    if (!m) { perror(path); exit(1); }
    while (fscanf(m, "%255s %lu %d %d", file, &off, &die, &pb) == 4) {
        snprintf(path, sizeof(path), "%s/%s", dir, file);
        FILE *f = fopen(path, "rb");
        if (!f) { perror(path); exit(1); }
        fseek(f, off, SEEK_SET);
        int blk = die * BLOCKS + pb, p;
        for (p = 0; p < PAGES; p++) {
            uint8_t *pg = malloc(PSIZE);
            if (fread(pg, 1, PSIZE, f) != PSIZE) { printf("short %s\n", file); exit(1); }
            int i;
            for (i = 0; i < PSIZE && pg[i] == 0xff; i++);
            if (i == PSIZE) { free(pg); continue; }
            pages[blk * PAGES + p] = pg;
            lastprog[blk] = p;
        }
        fclose(f);
        n++;
    }
    fclose(m);
    printf("loaded %d raw blocks\n", n);
}

static void logf_print(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
}

/* shadow of the written sectors */
#define REGION_LBNS 40
#define FIRST_LBN 100
#define SECS (REGION_LBNS * 4096)
static uint8_t *shadow;
static uint8_t written[SECS];

extern bool ftl_write_enabled;
void ftl_sim_set_range(uint32_t first, uint32_t end);
void ftl_write_log(void (*logf_fn)(const char *fmt, ...));
int ftl_sim_check(void (*logf_fn)(const char *fmt, ...));

static int verify(const char *when)
{
    static uint8_t b[512];
    unsigned long bad = 0, n = 0;
    uint32_t s;
    for (s = 0; s < SECS; s++) {
        if (!written[s]) continue;
        n++;
        if (ftl_read(FIRST_LBN * 4096 + s, 1, b) || memcmp(b, shadow + s * 512, 512)) {
            if (bad < 5) printf("  mismatch at sector %u (%s)\n", FIRST_LBN * 4096 + s, when);
            bad++;
        }
    }
    printf("verify %s: %lu sectors, %lu bad\n", when, n, bad);
    return bad != 0;
}

/* "seq": a file copy over USB, sequential 'chunk' sector writes with FAT
   and FSInfo updates; prints the NAND work per MB written */
static int bench_seq(int mb, int chunk)
{
    static uint8_t data[256 * 512];
    uint32_t start = FIRST_LBN * 4096 + 3, s, end;   /* not page aligned */
    unsigned long p0 = n_prog, e0 = n_erase, r0 = n_read, rb0 = n_read_bytes;
    int k;

    ftl_sim_set_range(8, 7786496);
    ftl_write_enabled = true;
    nand_ll_write_enable = true;
    end = start + mb * 2048;
    for (s = start; s < end; s += chunk) {
        int n = MIN(chunk, (int)(end - s));
        for (k = 0; k < n * 512; k++) data[k] = rand();
        if (ftl_write(s, n, data)) { printf("write failed\n"); return 1; }
        if (((s - start) / chunk) % 16 == 15) {
            /* FAT sector and FSInfo, as the host flushes them */
            if (ftl_write(40 + (s / 65536) % 64, 1, data) ||
                ftl_write(9, 1, data)) { printf("fat write failed\n"); return 1; }
        }
    }
    if (ftl_sync()) return 1;
    printf("seq %d MB in %d sector writes: per MB %.1f programs, %.2f erases, "
           "%.1f reads (%.1f KB)\n", mb, chunk,
           (double)(n_prog - p0) / mb, (double)(n_erase - e0) / mb,
           (double)(n_read - r0) / mb, (double)(n_read_bytes - rb0) / 1024 / mb);
    ftl_write_log(logf_print);
    if (ftl_init()) { printf("ftl_init failed\n"); return 1; }
    return ftl_sim_check(logf_print) || n_violations;
}

int main(int argc, char **argv)
{
    int ops = argc > 2 ? atoi(argv[2]) : 2000, i, fails = 0;
    static uint8_t data[64 * 512];
    memset(lastprog, -1, sizeof(lastprog));
    /* FTLSIM_ECC=1: read through the ECC path (the fake ECC never corrects) */
    extern bool ftl_use_ecc;
    ftl_use_ecc = getenv("FTLSIM_ECC") != NULL;
    load_backup(argc > 1 ? argv[1] : "backup");
    shadow = calloc(SECS, 512);
    if (ftl_init()) { printf("ftl_init failed\n"); ftl_debug_log(logf_print); return 1; }
    ftl_debug_log(logf_print);
    fails += ftl_sim_check(logf_print);
    if (argc > 3 && !strcmp(argv[3], "seq")) {
        fails = bench_seq(ops, argc > 4 ? atoi(argv[4]) : 128);
        printf("%s\n", fails ? "FAILED" : "PASSED");
        return fails;
    }
    ftl_sim_set_range(8, 7786496);
    ftl_write_enabled = true;
    nand_ll_write_enable = true;
    static uint8_t mbr[9 * 512], fsinfo[512];
    ftl_read(0, 9, mbr);
    ftl_read(9, 1, fsinfo);
    srand(1234);
    for (i = 0; i < ops; i++) {
        /* mostly small writes concentrated on a few lbns, sometimes big */
        int lbn = (rand() % 3) ? rand() % 6 : rand() % REGION_LBNS;
        int n = (rand() % 8) ? 1 + rand() % 8 : 1 + rand() % 64;
        uint32_t s = lbn * 4096 + rand() % (4096 - n);
        int k;
        for (k = 0; k < n * 512; k++) data[k] = rand();
        uint32_t rc = ftl_write(FIRST_LBN * 4096 + s, n, data);
        if (rc) { printf("ftl_write rc %u at op %d\n", rc, i); fails++; break; }
        if (i % 7 == 0) {
            /* FAT updates its FSInfo sector (partition sector 1) all the
               time; it shares the first page with the MBR */
            for (k = 0; k < 512; k++) fsinfo[k] = rand();
            rc = ftl_write(9, 1, fsinfo);
            if (rc) { printf("fsinfo write rc %u at op %d\n", rc, i); fails++; break; }
        }
        memcpy(shadow + s * 512, data, n * 512);
        memset(written + s, 1, n);
        if ((i + 1) % 25 == 0)
            fails += verify("with cache");
        if ((i + 1) % 50 == 0) {
            /* reboot: write the cache, rebuild everything from the NAND */
            if (ftl_sync()) { printf("ftl_sync failed\n"); fails++; break; }
            if (ftl_init()) { printf("ftl_init failed after %d ops\n", i + 1); fails++; break; }
            ftl_write_enabled = true;
            ftl_sim_set_range(8, 7786496);
            fails += ftl_sim_check(logf_print);
            fails += verify("after reboot");
            {
                static uint8_t chk[16 * 512];
                if (ftl_read(0, 16, chk) || memcmp(chk + 9 * 512, fsinfo, 512) ||
                    memcmp(chk, mbr, 9 * 512)) {
                    printf("page 0 wrong after reboot at op %d\n", i);
                    fails++;
                }
            }
            if (fails) break;
        }
    }
    ftl_sync();
    ftl_write_log(logf_print);
    fails += verify("final");
    printf("NAND: %lu programs, %lu erases, %lu violations\n", n_prog, n_erase, n_violations);
    printf("%s\n", fails || n_violations ? "FAILED" : "PASSED");
    return fails || n_violations;
}
