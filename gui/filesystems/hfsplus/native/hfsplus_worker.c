// SPDX-License-Identifier: GPL-3.0-or-later
#include "hfsplus_native.h"
#include "version.h"
#include "infiltratr/core.h"

#include <errno.h>
#include <limits.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define PROG "linux-defragger-hfsplus-worker"

static void usage(FILE *stream) {
    fprintf(stream,
        "Usage: %s --version | identify DEVICE | analyse-json DEVICE | "
        "defrag|growth-defrag|recover DEVICE --write --confirm DEVICE --journal PATH "
        "[--growth-percent 10] [--live-updates]\n", PROG);
}

static void result(const char *operation, const char *status, const char *message) {
    printf("@@RESULT {\"operation\":\"%s\",\"status\":\"%s\",\"message\":\"%s\"}\n",
           operation, status, message ? message : "");
    fflush(stdout);
}

static bool parse_unsigned(const char *text, unsigned *value) {
    uint64_t parsed = 0;
    if (!infiltratr_parse_u64_range(text, 10U, 0U, UINT_MAX, &parsed)) return false;
    *value = (unsigned)parsed;
    return true;
}

static char *stage_name(const char *journal) {
    size_t n = strlen(journal) + 16U;
    char *path = malloc(n);
    if (path) snprintf(path, n, "%s.hfsplus-stage", journal);
    return path;
}

static int save_journal(const char *journal, const char *device, const char *stage,
                        const char *operation) {
    FILE *file = fopen(journal, "w");
    if (!file) return -1;
    fprintf(file, "LINUX-DEFRAGGER-HFSPLUS-1\ndevice=%s\nstage=%s\noperation=%s\n",
            device, stage, operation);
    if (fflush(file) || fsync(fileno(file)) || fclose(file)) return -1;
    return 0;
}

static int load_journal(const char *journal, char **device, char **stage, char operation[32]) {
    FILE *file = fopen(journal, "r");
    if (!file) return -1;
    char line[4096];
    if (!fgets(line, sizeof(line), file)) {
        fclose(file); return -1;
    }
    infiltratr_trim_line_end(line);
    if (strcmp(line, "LINUX-DEFRAGGER-HFSPLUS-1")) {
        fclose(file); return -1;
    }
    while (fgets(line, sizeof(line), file)) {
        infiltratr_trim_line_end(line);
        if (!strncmp(line, "device=", 7)) *device = strdup(line + 7);
        else if (!strncmp(line, "stage=", 6)) *stage = strdup(line + 6);
        else if (!strncmp(line, "operation=", 10)) infiltratr_copy_string(operation, 32, line + 10);
    }
    fclose(file);
    return *device && *stage && operation[0] ? 0 : -1;
}

int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--version")) { puts(LD_VERSION); return 0; }
    if (argc < 3) { usage(stderr); return 2; }
    const char *mode = argv[1];
    const char *device = argv[2];
    char *error = NULL;
    if (!strcmp(mode, "identify")) {
        uint16_t signature = 0, version = 0;
        uint32_t attributes = 0;
        if (hfsplus_identify(device, &signature, &version, &attributes, &error)) {
            free(error); return 1;
        }
        (void)version;
        printf("{\"filesystem\":\"hfsplus\",\"variant\":\"%s\",\"journaled\":%s}\n",
               signature == 0x4858U ? "HFSX" : "HFS+",
               (attributes & 0x2000U) ? "true" : "false");
        return 0;
    }
    if (!strcmp(mode, "analyse-json")) {
        int rc = hfsplus_analyse_json(device, &error);
        if (rc) { fprintf(stderr, "%s\n", error ? error : "HFS+ analysis failed"); free(error); return 1; }
        return 0;
    }
    bool growth = !strcmp(mode, "growth-defrag");
    bool defrag = !strcmp(mode, "defrag");
    bool recover = !strcmp(mode, "recover");
    if (!growth && !defrag && !recover) { usage(stderr); return 2; }
    const char *confirm = NULL, *journal = NULL;
    unsigned growth_percent = 10U;
    bool write = false, live = false;
    for (int i = 3; i < argc; ++i) {
        if (!strcmp(argv[i], "--write")) write = true;
        else if (!strcmp(argv[i], "--live-updates")) live = true;
        else if (!strcmp(argv[i], "--confirm") && i + 1 < argc) confirm = argv[++i];
        else if (!strcmp(argv[i], "--journal") && i + 1 < argc) journal = argv[++i];
        else if (!strcmp(argv[i], "--growth-percent") && i + 1 < argc) { if (!parse_unsigned(argv[++i], &growth_percent)) { fprintf(stderr, "invalid --growth-percent\n"); return 2; } }
        else if ((!strcmp(argv[i], "--workers") || !strcmp(argv[i], "--ram-buffer") ||
                  !strcmp(argv[i], "--batch-clusters") || !strcmp(argv[i], "--live-map-cells")) && i + 1 < argc) ++i;
    }
    if (!write || !journal || !confirm || strcmp(confirm, device)) {
        fprintf(stderr, "HFS+ mutation requires --write --confirm DEVICE --journal PATH\n");
        return 2;
    }
    if (growth && growth_percent != 10U) {
        fprintf(stderr, "HFS+ Growth Defrag requires exactly 10 percent reserve\n"); return 2;
    }
    char *stage = NULL, *journal_device = NULL;
    char journal_operation[32] = {0};
    if (recover) {
        if (load_journal(journal, &journal_device, &stage, journal_operation)) {
            fprintf(stderr, "HFS+ recovery journal is missing or malformed\n"); return 1;
        }
        if (strcmp(journal_device, device)) {
            fprintf(stderr, "HFS+ recovery journal target mismatch\n"); free(journal_device); free(stage); return 1;
        }
        bool journal_growth = !strcmp(journal_operation, "growth-defrag");
        uint64_t written = 0;
        if (hfsplus_verify_layout(stage, journal_growth, 10U, &error) ||
            hfsplus_commit_stage(stage, device, &written, &error) ||
            hfsplus_verify_layout(device, journal_growth, 10U, &error)) {
            fprintf(stderr, "%s\n", error ? error : "HFS+ recovery failed");
            free(error); free(journal_device); free(stage); return 1;
        }
        unlink(stage); unlink(journal);
        printf("Recovered verified HFS+ source; committed %" PRIu64 " KiB of allocated blocks.\n", written / 1024U);
        result("recover", "completed", "");
        free(journal_device); free(stage); return 0;
    }
    stage = stage_name(journal);
    if (!stage) { fprintf(stderr, "out of memory\n"); return 1; }
    printf("Starting native C HFS+ %s on %s.\n", growth ? "Growth Defrag" : "Defrag", device);
    uint64_t planned = 0;
    if (hfsplus_build_stage(device, stage, growth, growth_percent, live, &planned, &error) ||
        hfsplus_verify_layout(stage, growth, growth_percent, &error)) {
        fprintf(stderr, "%s\n", error ? error : "HFS+ stage failed");
        free(error); unlink(stage); free(stage); return 1;
    }
    if (save_journal(journal, device, stage, mode)) {
        fprintf(stderr, "cannot persist HFS+ recovery journal: %s\n", strerror(errno));
        unlink(stage); free(stage); return 1;
    }
    printf("HFS+ source commit: writing %" PRIu64 " KiB of verified allocated blocks only.\n", planned / 1024U);
    uint64_t written = 0;
    if (hfsplus_commit_stage(stage, device, &written, &error) ||
        hfsplus_verify_layout(device, growth, growth_percent, &error)) {
        fprintf(stderr, "%s\n", error ? error : "HFS+ source commit failed");
        free(error); free(stage); return 1;
    }
    unlink(stage); unlink(journal);
    if (live) { printf("@@LIVE_RESET {\"reason\":\"authoritative post-commit HFS+ map\"}\n"); fflush(stdout); }
    printf("HFS+ %s completed; committed %" PRIu64 " KiB of allocated blocks.\n",
           growth ? "Growth Defrag" : "Defrag", written / 1024U);
    result(mode, "completed", "");
    free(stage);
    return 0;
}
