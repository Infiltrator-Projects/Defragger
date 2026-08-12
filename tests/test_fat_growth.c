// SPDX-License-Identifier: GPL-3.0-or-later
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fat_growth.h"

#define CHECK(condition)                                                     \
    do {                                                                     \
        if (!(condition)) {                                                  \
            fprintf(                                                         \
                stderr,                                                      \
                "%s:%d: check failed: %s\n",                                 \
                __FILE__,                                                    \
                __LINE__,                                                    \
                #condition                                                   \
            );                                                               \
            exit(EXIT_FAILURE);                                              \
        }                                                                    \
    } while (0)

static uint32_t random_state = UINT32_C(0x59F47A21);

static uint32_t next_random(void) {
    random_state = random_state * UINT32_C(1664525) + UINT32_C(1013904223);
    return random_state;
}

static void check_random_plans(Fat32 *filesystem) {
    GrowthObject items[8] = {0};
    GrowthObjectList objects = {.v = items, .cap = 8};
    for (size_t iteration = 0; iteration < 5000; iteration++) {
        memset(filesystem->fat, 0,
               filesystem->fat_entry_count * sizeof(*filesystem->fat));
        for (uint32_t cluster = 2; cluster <= filesystem->max_cluster; cluster++) {
            if (next_random() % 29 == 0) filesystem->fat[cluster] = FAT32_BAD;
        }
        objects.len = 1 + next_random() % 8;
        for (size_t index = 0; index < objects.len; index++) {
            items[index].is_root = index == 0;
            items[index].is_dir = index == 0 || next_random() % 3 == 0;
            items[index].path = NULL;
            items[index].clusters = 1 + next_random() % 9;
            items[index].target = 0;
            items[index].reserve_after = 0;
        }

        size_t reserve_total = 0;
        uint32_t layout_end = 0;
        if (!plan_growth_layout(
                filesystem, &objects, 10, 90,
                &reserve_total, &layout_end)) {
            continue;
        }

        size_t expected_reserve = 0;
        uint32_t cursor = 2;
        for (size_t index = 0; index < objects.len; index++) {
            GrowthObject *item = &items[index];
            CHECK(item->target >= cursor);
            for (size_t offset = 0; offset < item->clusters; offset++) {
                CHECK(item->target + offset < 90);
                CHECK(filesystem->fat[item->target + offset] != FAT32_BAD);
            }
            size_t expected = item->is_dir ? 0 : (item->clusters + 9) / 10;
            CHECK(item->reserve_after == expected);
            expected_reserve += expected;
            cursor = item->target + (uint32_t)item->clusters;
            size_t usable_reserve = 0;
            while (usable_reserve < expected) {
                CHECK(cursor < 90);
                if (filesystem->fat[cursor] != FAT32_BAD) usable_reserve++;
                cursor++;
            }
        }
        CHECK(reserve_total == expected_reserve);
        CHECK(layout_end == cursor - 1);
    }
}


int main(void) {
    Fat32 filesystem = {
        .fat_type = FAT_TYPE_32,
        .cluster_count = 98,
        .max_cluster = 99,
        .fat_entry_count = 100,
    };
    filesystem.fat = calloc(
        filesystem.fat_entry_count,
        sizeof(*filesystem.fat)
    );
    CHECK(filesystem.fat != NULL);

    GrowthObject items[] = {
        {
            .is_root = true,
            .is_dir = true,
            .path = "<root>",
            .clusters = 3,
        },
        {
            .is_root = false,
            .is_dir = false,
            .path = "/ordinary.bin",
            .clusters = 10,
        },
    };
    GrowthObjectList objects = {
        .v = items,
        .len = sizeof(items) / sizeof(items[0]),
        .cap = sizeof(items) / sizeof(items[0]),
    };
    size_t reserve = 0;
    uint32_t final_cluster = 0;
    CHECK(plan_growth_layout(
        &filesystem,
        &objects,
        10,
        90,
        &reserve,
        &final_cluster
    ));
    CHECK(objects.v[0].target == 2);
    CHECK(objects.v[0].reserve_after == 0);
    CHECK(objects.v[1].target == 5);
    CHECK(objects.v[1].reserve_after == 1);
    CHECK(reserve == 1);
    CHECK(final_cluster == 15);

    filesystem.fat[2] = FAT32_BAD;
    CHECK(plan_growth_layout(
        &filesystem,
        &objects,
        10,
        90,
        &reserve,
        &final_cluster
    ));
    CHECK(objects.v[0].target == 3);
    CHECK(objects.v[1].target == 6);
    CHECK(final_cluster == 16);

    U32Vec contiguous = {
        .v = (uint32_t[]){30, 31, 32},
        .len = 3,
        .cap = 3,
    };
    CHECK(chain_is_exact_run(&contiguous, 30));
    contiguous.v[2] = 33;
    CHECK(!chain_is_exact_run(&contiguous, 30));

    check_random_plans(&filesystem);

    free(filesystem.fat);
    puts("FAT growth planner test passed");
    return 0;
}
