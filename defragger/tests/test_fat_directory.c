// SPDX-License-Identifier: GPL-3.0-or-later
#include <stdio.h>
#include <stdlib.h>

#include "fat_directory.h"

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            fprintf(stderr, "CHECK failed at %s:%d: %s\n",                    \
                    __FILE__, __LINE__, #condition);                            \
            return EXIT_FAILURE;                                               \
        }                                                                       \
    } while (0)

int main(void) {
    U32Vec empty = {0};
    CHECK(chain_fragments(&empty) == 0);

    uint32_t contiguous_values[] = {2, 3, 4, 5};
    U32Vec contiguous = {
        .v = contiguous_values,
        .len = sizeof(contiguous_values) / sizeof(contiguous_values[0]),
        .cap = sizeof(contiguous_values) / sizeof(contiguous_values[0]),
    };
    CHECK(chain_fragments(&contiguous) == 1);

    uint32_t fragmented_values[] = {2, 8, 9, 20, 21, 22};
    U32Vec fragmented = {
        .v = fragmented_values,
        .len = sizeof(fragmented_values) / sizeof(fragmented_values[0]),
        .cap = sizeof(fragmented_values) / sizeof(fragmented_values[0]),
    };
    CHECK(chain_fragments(&fragmented) == 3);

    FileList files = {0};
    DirRefList refs = {0};
    filelist_free(&files);
    dirreflist_free(&refs);
    CHECK(files.v == NULL && files.len == 0 && files.cap == 0);
    CHECK(refs.v == NULL && refs.len == 0 && refs.cap == 0);

    puts("FAT directory catalogue tests passed");
    return EXIT_SUCCESS;
}
