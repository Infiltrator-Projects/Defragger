// SPDX-License-Identifier: GPL-3.0-or-later
/* Durable FAT journal serialization and parsing. */

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ld_runtime.h"
#include "ld_path.h"
#include "infiltratr/core.h"
#include "fat_journal.h"

void journal_free(Journal *j) {
    free(j->device_path);
    u32vec_free(&j->source);
    memset(j, 0, sizeof(*j));
}

void relocation_journal_free(RelocationJournal *j) {
    free(j->device_path);
    free(j->moves);
    free(j->dir_patches);
    memset(j, 0, sizeof(*j));
}

void relocation_journal_add_move(RelocationJournal *j, RelocationMove move) {
    j->moves = ld_xrealloc(j->moves, (j->move_count + 1) * sizeof(*j->moves));
    j->moves[j->move_count++] = move;
}

void relocation_journal_add_dir_patch(RelocationJournal *j, RelocationDirPatch patch) {
    j->dir_patches = ld_xrealloc(j->dir_patches,
                              (j->dir_patch_count + 1) * sizeof(*j->dir_patches));
    j->dir_patches[j->dir_patch_count++] = patch;
}


static uint64_t parse_u64_value(const char *text, unsigned int base, const char *field) {
    uint64_t value = 0;
    if (!infiltratr_parse_u64(text, base, &value)) {
        char message[160];
        (void)snprintf(message, sizeof(message), "invalid FAT journal %s", field);
        ld_die(message);
    }
    return value;
}

static uint32_t parse_u32_value(const char *text, unsigned int base, const char *field) {
    uint64_t value = 0;
    if (!infiltratr_parse_u64_range(text, base, 0U, UINT32_MAX, &value)) {
        char message[160];
        (void)snprintf(message, sizeof(message), "invalid FAT journal %s", field);
        ld_die(message);
    }
    return (uint32_t)value;
}

static size_t parse_size_value(const char *text, const char *field) {
    uint64_t value = 0;
    if (!infiltratr_parse_u64_range(text, 10U, 0U, (uint64_t)SIZE_MAX, &value)) {
        char message[160];
        (void)snprintf(message, sizeof(message), "invalid FAT journal %s", field);
        ld_die(message);
    }
    return (size_t)value;
}

static JournalStage parse_stage_value(const char *text) {
    uint64_t value = 0;
    if (!infiltratr_parse_u64_range(text, 10U, J_PREPARED, J_OLD_FREED, &value))
        ld_die("invalid FAT journal stage");
    return (JournalStage)value;
}

void relocation_journal_write(const char *path, const RelocationJournal *j) {
    char *tmp = NULL;
    FILE *fp = ld_path_open_atomic_temp(path, &tmp);
    if (fp == NULL) ld_die_errno("create relocation journal");
    fprintf(fp, "%s\n", RELOCATION_JOURNAL_MAGIC);
    fprintf(fp, "device=%s\n", j->device_path);
    fprintf(fp, "volume_id=%08" PRIx32 "\n", j->volume_id);
    fprintf(fp, "stage=%d\n", (int)j->stage);
    fprintf(fp, "root_old=%" PRIu32 "\n", j->root_old);
    fprintf(fp, "root_new=%" PRIu32 "\n", j->root_new);
    fprintf(fp, "move_count=%zu\n", j->move_count);
    for (size_t i = 0; i < j->move_count; i++) {
        const RelocationMove *m = &j->moves[i];
        fprintf(fp, "move=%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32 "\n",
                m->source, m->destination, m->next, m->predecessor);
    }
    fprintf(fp, "dir_patch_count=%zu\n", j->dir_patch_count);
    for (size_t i = 0; i < j->dir_patch_count; i++) {
        const RelocationDirPatch *p = &j->dir_patches[i];
        fprintf(fp, "dir_patch=%" PRIu64 ",%" PRIu32 ",%" PRIu32 "\n",
                p->offset, p->old_target, p->new_target);
    }
    if (fflush(fp) != 0) ld_die_errno("flush relocation journal");
    if (fsync(fileno(fp)) != 0) ld_die_errno("fsync relocation journal");
    if (fclose(fp) != 0) ld_die_errno("close relocation journal");
    if (rename(tmp, path) != 0) ld_die_errno("install relocation journal");
    ld_path_fsync_parent(path);
    free(tmp);
}

bool journal_has_magic(const char *path, const char *magic) {
    FILE *fp = fopen(path, "r");
    if (fp == NULL) ld_die_errno("open journal");
    char *line = NULL;
    size_t cap = 0;
    bool match = getline(&line, &cap, fp) >= 0;
    if (match) infiltratr_trim_line_end(line);
    match = match && strcmp(line, magic) == 0;
    free(line);
    fclose(fp);
    return match;
}

Journal journal_read(const char *path) {
    FILE *fp = fopen(path, "r");
    if (fp == NULL) ld_die_errno("open journal");
    Journal j = {0};
    char *line = NULL;
    size_t cap = 0;
    if (getline(&line, &cap, fp) < 0 || strcmp(line, JOURNAL_MAGIC "\n") != 0) {
        ld_die("invalid journal header");
    }
    size_t expected_count = 0;
    while (getline(&line, &cap, fp) >= 0) {
        infiltratr_trim_line_end(line);
        char *eq = strchr(line, '=');
        if (eq == NULL) continue;
        *eq++ = '\0';
        if (strcmp(line, "device") == 0) j.device_path = ld_xstrdup(eq);
        else if (strcmp(line, "volume_id") == 0) j.volume_id = parse_u32_value(eq, 16U, "volume_id");
        else if (strcmp(line, "stage") == 0) j.stage = parse_stage_value(eq);
        else if (strcmp(line, "dirent_offset") == 0) j.dirent_offset = parse_u64_value(eq, 10U, "dirent_offset");
        else if (strcmp(line, "old_first") == 0) j.old_first = parse_u32_value(eq, 10U, "old_first");
        else if (strcmp(line, "dest_start") == 0) j.dest_start = parse_u32_value(eq, 10U, "dest_start");
        else if (strcmp(line, "count") == 0) expected_count = parse_size_value(eq, "count");
        else if (strcmp(line, "source") == 0) {
            char *save = NULL;
            for (char *tok = strtok_r(eq, ",", &save); tok != NULL; tok = strtok_r(NULL, ",", &save)) {
                u32vec_push(&j.source, parse_u32_value(tok, 10U, "source cluster"));
            }
        }
    }
    free(line);
    fclose(fp);
    if (j.device_path == NULL || j.source.len != expected_count || j.source.len == 0) {
        journal_free(&j);
        ld_die("journal is incomplete or corrupt");
    }
    return j;
}

RelocationJournal relocation_journal_read(const char *path) {
    FILE *fp = fopen(path, "r");
    if (fp == NULL) ld_die_errno("open relocation journal");
    RelocationJournal j = {0};
    char *line = NULL;
    size_t cap = 0;
    if (getline(&line, &cap, fp) < 0 || strcmp(line, RELOCATION_JOURNAL_MAGIC "\n") != 0) {
        ld_die("invalid relocation journal header");
    }
    size_t expected_moves = 0;
    size_t expected_patches = 0;
    while (getline(&line, &cap, fp) >= 0) {
        infiltratr_trim_line_end(line);
        char *eq = strchr(line, '=');
        if (eq == NULL) continue;
        *eq++ = '\0';
        if (strcmp(line, "device") == 0) j.device_path = ld_xstrdup(eq);
        else if (strcmp(line, "volume_id") == 0) j.volume_id = parse_u32_value(eq, 16U, "volume_id");
        else if (strcmp(line, "stage") == 0) j.stage = parse_stage_value(eq);
        else if (strcmp(line, "root_old") == 0) j.root_old = parse_u32_value(eq, 10U, "root_old");
        else if (strcmp(line, "root_new") == 0) j.root_new = parse_u32_value(eq, 10U, "root_new");
        else if (strcmp(line, "move_count") == 0) expected_moves = parse_size_value(eq, "move_count");
        else if (strcmp(line, "dir_patch_count") == 0) {
            expected_patches = parse_size_value(eq, "dir_patch_count");
        } else if (strcmp(line, "move") == 0) {
            RelocationMove m = {0};
            if (sscanf(eq, "%" SCNu32 ",%" SCNu32 ",%" SCNu32 ",%" SCNu32,
                       &m.source, &m.destination, &m.next, &m.predecessor) != 4) {
                relocation_journal_free(&j);
                ld_die("invalid relocation journal move record");
            }
            relocation_journal_add_move(&j, m);
        } else if (strcmp(line, "dir_patch") == 0) {
            RelocationDirPatch p = {0};
            if (sscanf(eq, "%" SCNu64 ",%" SCNu32 ",%" SCNu32,
                       &p.offset, &p.old_target, &p.new_target) != 3) {
                relocation_journal_free(&j);
                ld_die("invalid relocation journal directory patch");
            }
            relocation_journal_add_dir_patch(&j, p);
        }
    }
    free(line);
    fclose(fp);
    if (j.device_path == NULL || j.move_count == 0 || j.move_count != expected_moves ||
        j.dir_patch_count != expected_patches || j.root_old < 2 || j.root_new < 2) {
        relocation_journal_free(&j);
        ld_die("relocation journal is incomplete or corrupt");
    }
    return j;
}

void journal_remove(const char *path) {
    if (unlink(path) != 0 && errno != ENOENT) ld_die_errno("remove journal");
    ld_path_fsync_parent(path);
}

bool path_exists(const char *path) {
    return access(path, F_OK) == 0;
}
