// SPDX-License-Identifier: GPL-3.0-or-later
#include "btrfs_native.h"

#include "infiltratr/endian.h"
#include "infiltratr/posix_io.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#define BTRFS_SUPER_OFFSET (64ULL * 1024ULL)
#define BTRFS_SUPER_SIZE 4096U
#define BTRFS_HEADER_SIZE 101U
#define BTRFS_ITEM_SIZE 25U
#define BTRFS_KEY_PTR_SIZE 33U
#define BTRFS_DISK_KEY_SIZE 17U
#define BTRFS_CHUNK_FIXED_SIZE 48U
#define BTRFS_STRIPE_SIZE 32U
#define BTRFS_MAX_TREE_LEVEL 8U
#define BTRFS_MAX_TREE_BLOCKS 8000000U
#define BTRFS_MAX_SYSTEM_ARRAY 2048U

#define BTRFS_INODE_ITEM 1U
#define BTRFS_EXTENT_DATA 108U
#define BTRFS_ROOT_ITEM 132U
#define BTRFS_EXTENT_ITEM 168U
#define BTRFS_METADATA_ITEM 169U
#define BTRFS_CHUNK_ITEM 228U
#define BTRFS_EXTENT_TREE_OBJECTID 2ULL
#define BTRFS_FS_TREE_OBJECTID 5ULL
#define BTRFS_FIRST_FREE_OBJECTID 256ULL

#define BTRFS_FILE_EXTENT_INLINE 0U
#define BTRFS_FILE_EXTENT_REG 1U
#define BTRFS_FILE_EXTENT_PREALLOC 2U

#define BTRFS_BLOCK_GROUP_RAID0 8ULL
#define BTRFS_BLOCK_GROUP_RAID10 64ULL
#define BTRFS_BLOCK_GROUP_RAID5 128ULL
#define BTRFS_BLOCK_GROUP_RAID6 256ULL
#define BTRFS_STRIPED_PROFILES \
    (BTRFS_BLOCK_GROUP_RAID0 | BTRFS_BLOCK_GROUP_RAID10 | \
     BTRFS_BLOCK_GROUP_RAID5 | BTRFS_BLOCK_GROUP_RAID6)

typedef struct {
    int fd;
    uint64_t size;
} Reader;

typedef struct {
    uint64_t objectid;
    uint8_t type;
    uint64_t offset;
} Key;

typedef struct {
    uint64_t devid;
    uint64_t physical;
} Stripe;

typedef struct {
    uint64_t logical;
    uint64_t length;
    uint64_t chunk_type;
    uint64_t stripe_len;
    Stripe *stripes;
    uint16_t stripe_count;
} Chunk;

typedef struct {
    Chunk *items;
    size_t count;
    size_t capacity;
} ChunkVec;

typedef struct {
    Key key;
    uint8_t *data;
    uint32_t size;
} TreeItem;

typedef struct {
    TreeItem *items;
    size_t count;
    size_t capacity;
} ItemVec;

typedef struct {
    BtrfsRange *items;
    size_t count;
    size_t capacity;
} RangeVec;

typedef struct {
    uint64_t *items;
    size_t count;
    size_t capacity;
} U64Vec;

typedef struct {
    uint64_t logical;
    uint8_t level;
} TreeRef;

typedef struct {
    TreeRef *items;
    size_t count;
    size_t capacity;
} RefVec;

typedef struct {
    uint64_t *slots;
    size_t capacity;
    size_t count;
} U64Set;

typedef struct {
    uint64_t objectid;
    uint64_t bytenr;
    uint64_t key_offset;
    uint32_t refs;
    uint8_t level;
} RootRecord;

typedef struct {
    RootRecord *items;
    size_t count;
    size_t capacity;
} RootVec;

typedef struct {
    uint64_t logical;
    uint64_t physical;
    uint64_t length;
    uint64_t disk_start;
    uint64_t disk_length;
    bool encoded;
} FileRun;

typedef struct {
    FileRun *items;
    size_t count;
    size_t capacity;
} RunVec;

static void set_error(char *error, size_t error_size, const char *format, ...)
{
    if (error == NULL || error_size == 0U)
        return;
    va_list args;
    va_start(args, format);
    (void)vsnprintf(error, error_size, format, args);
    va_end(args);
}

static uint16_t le16(const uint8_t *data)
{
    return infiltratr_load_le16(data);
}

static uint32_t le32(const uint8_t *data)
{
    return infiltratr_load_le32(data);
}

static uint64_t le64(const uint8_t *data)
{
    return infiltratr_load_le64(data);
}

static int open_reader(const char *path, Reader *reader, char *error, size_t error_size)
{
    struct stat status;
    if (stat(path, &status) != 0) {
        set_error(error, error_size, "cannot stat Btrfs target: %s", strerror(errno));
        return -1;
    }
    if (!S_ISREG(status.st_mode) && !S_ISBLK(status.st_mode)) {
        set_error(error, error_size, "Btrfs target must be a block device or regular image");
        return -1;
    }
    const int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        set_error(error, error_size, "cannot open Btrfs target: %s", strerror(errno));
        return -1;
    }
    uint64_t size = 0U;
    if (S_ISBLK(status.st_mode)) {
        if (ioctl(fd, BLKGETSIZE64, &size) != 0) {
            set_error(error, error_size, "cannot determine Btrfs device size: %s", strerror(errno));
            (void)close(fd);
            return -1;
        }
    } else {
        if (status.st_size < 0) {
            set_error(error, error_size, "invalid Btrfs image size");
            (void)close(fd);
            return -1;
        }
        size = (uint64_t)status.st_size;
    }
    reader->fd = fd;
    reader->size = size;
    return 0;
}

static void close_reader(Reader *reader)
{
    if (reader->fd >= 0)
        (void)close(reader->fd);
    reader->fd = -1;
    reader->size = 0U;
}

static int read_exact(const Reader *reader, uint64_t offset, void *buffer, size_t length,
                      char *error, size_t error_size)
{
    if (offset > reader->size || (uint64_t)length > reader->size - offset) {
        set_error(error, error_size, "Btrfs read lies outside the device");
        return -1;
    }
    if (infiltratr_pread_full(reader->fd, buffer, length, offset) != 0) {
        set_error(error, error_size, "cannot read Btrfs metadata: %s", strerror(errno));
        return -1;
    }
    return 0;
}

static int grow_array(void **items, size_t *capacity, size_t count, size_t item_size,
                      char *error, size_t error_size)
{
    if (count < *capacity)
        return 0;
    size_t next = *capacity == 0U ? 16U : *capacity * 2U;
    if (next < *capacity || next > SIZE_MAX / item_size) {
        set_error(error, error_size, "Btrfs metadata collection is too large");
        return -1;
    }
    void *replacement = realloc(*items, next * item_size);
    if (replacement == NULL) {
        set_error(error, error_size, "out of memory analysing Btrfs metadata");
        return -1;
    }
    *items = replacement;
    *capacity = next;
    return 0;
}

static int range_push(RangeVec *vec, uint64_t start, uint64_t end,
                      char *error, size_t error_size)
{
    if (end <= start)
        return 0;
    if (grow_array((void **)&vec->items, &vec->capacity, vec->count,
                   sizeof(*vec->items), error, error_size) != 0)
        return -1;
    vec->items[vec->count++] = (BtrfsRange){start, end};
    return 0;
}

static int u64_push(U64Vec *vec, uint64_t value, char *error, size_t error_size)
{
    if (grow_array((void **)&vec->items, &vec->capacity, vec->count,
                   sizeof(*vec->items), error, error_size) != 0)
        return -1;
    vec->items[vec->count++] = value;
    return 0;
}

static int ref_push(RefVec *vec, uint64_t logical, uint8_t level,
                    char *error, size_t error_size)
{
    if (grow_array((void **)&vec->items, &vec->capacity, vec->count,
                   sizeof(*vec->items), error, error_size) != 0)
        return -1;
    vec->items[vec->count++] = (TreeRef){logical, level};
    return 0;
}

static int item_push(ItemVec *vec, Key key, const uint8_t *data, uint32_t size,
                     char *error, size_t error_size)
{
    if (grow_array((void **)&vec->items, &vec->capacity, vec->count,
                   sizeof(*vec->items), error, error_size) != 0)
        return -1;
    uint8_t *copy = NULL;
    if (size != 0U) {
        copy = malloc(size);
        if (copy == NULL) {
            set_error(error, error_size, "out of memory copying Btrfs tree item");
            return -1;
        }
        memcpy(copy, data, size);
    }
    vec->items[vec->count++] = (TreeItem){key, copy, size};
    return 0;
}

static void item_vec_free(ItemVec *vec)
{
    for (size_t i = 0U; i < vec->count; ++i)
        free(vec->items[i].data);
    free(vec->items);
    memset(vec, 0, sizeof(*vec));
}

static int range_compare(const void *left, const void *right)
{
    const BtrfsRange *a = left;
    const BtrfsRange *b = right;
    if (a->start < b->start) return -1;
    if (a->start > b->start) return 1;
    if (a->end < b->end) return -1;
    if (a->end > b->end) return 1;
    return 0;
}

static void range_merge(RangeVec *vec)
{
    if (vec->count < 2U)
        return;
    qsort(vec->items, vec->count, sizeof(*vec->items), range_compare);
    size_t output = 0U;
    for (size_t i = 0U; i < vec->count; ++i) {
        const BtrfsRange current = vec->items[i];
        if (current.end <= current.start)
            continue;
        if (output != 0U && current.start <= vec->items[output - 1U].end) {
            if (current.end > vec->items[output - 1U].end)
                vec->items[output - 1U].end = current.end;
        } else {
            vec->items[output++] = current;
        }
    }
    vec->count = output;
}

static Key parse_key(const uint8_t *data)
{
    return (Key){le64(data), data[8], le64(data + 9U)};
}

static bool chunk_equal(const Chunk *a, const Chunk *b)
{
    if (a->logical != b->logical || a->length != b->length ||
        a->chunk_type != b->chunk_type || a->stripe_len != b->stripe_len ||
        a->stripe_count != b->stripe_count)
        return false;
    for (uint16_t i = 0U; i < a->stripe_count; ++i) {
        if (a->stripes[i].devid != b->stripes[i].devid ||
            a->stripes[i].physical != b->stripes[i].physical)
            return false;
    }
    return true;
}

static void chunk_free(Chunk *chunk)
{
    free(chunk->stripes);
    memset(chunk, 0, sizeof(*chunk));
}

static void chunk_vec_free(ChunkVec *vec)
{
    for (size_t i = 0U; i < vec->count; ++i)
        chunk_free(&vec->items[i]);
    free(vec->items);
    memset(vec, 0, sizeof(*vec));
}

static int parse_chunk(const uint8_t *data, size_t size, uint64_t logical,
                       Chunk *chunk, char *error, size_t error_size)
{
    if (size < BTRFS_CHUNK_FIXED_SIZE) {
        set_error(error, error_size, "truncated Btrfs chunk item");
        return -1;
    }
    const uint64_t length = le64(data);
    const uint64_t stripe_len = le64(data + 16U);
    const uint64_t chunk_type = le64(data + 24U);
    const uint16_t stripe_count = le16(data + 44U);
    if (length == 0U || stripe_len == 0U || stripe_count == 0U) {
        set_error(error, error_size, "invalid Btrfs chunk geometry");
        return -1;
    }
    const size_t stripe_bytes = BTRFS_CHUNK_FIXED_SIZE +
                                (size_t)stripe_count * BTRFS_STRIPE_SIZE;
    if (stripe_bytes > size) {
        set_error(error, error_size, "truncated Btrfs chunk stripes");
        return -1;
    }
    Stripe *stripes = calloc((size_t)stripe_count, sizeof(*stripes));
    if (stripes == NULL) {
        set_error(error, error_size, "out of memory reading Btrfs chunk stripes");
        return -1;
    }
    for (uint16_t i = 0U; i < stripe_count; ++i) {
        const size_t pos = BTRFS_CHUNK_FIXED_SIZE + (size_t)i * BTRFS_STRIPE_SIZE;
        stripes[i].devid = le64(data + pos);
        stripes[i].physical = le64(data + pos + 8U);
    }
    *chunk = (Chunk){logical, length, chunk_type, stripe_len, stripes, stripe_count};
    return 0;
}

static int chunk_add_unique(ChunkVec *vec, Chunk *chunk, char *error, size_t error_size)
{
    for (size_t i = 0U; i < vec->count; ++i) {
        if (vec->items[i].logical == chunk->logical) {
            if (!chunk_equal(&vec->items[i], chunk)) {
                set_error(error, error_size, "conflicting Btrfs chunk mappings");
                return -1;
            }
            chunk_free(chunk);
            return 0;
        }
    }
    if (grow_array((void **)&vec->items, &vec->capacity, vec->count,
                   sizeof(*vec->items), error, error_size) != 0)
        return -1;
    vec->items[vec->count++] = *chunk;
    memset(chunk, 0, sizeof(*chunk));
    return 0;
}

static int chunk_compare(const void *left, const void *right)
{
    const Chunk *a = left;
    const Chunk *b = right;
    if (a->logical < b->logical) return -1;
    if (a->logical > b->logical) return 1;
    return 0;
}

static int mapper_prepare(ChunkVec *chunks, uint64_t devid, uint64_t device_size,
                          char *error, size_t error_size)
{
    qsort(chunks->items, chunks->count, sizeof(*chunks->items), chunk_compare);
    uint64_t previous_end = 0U;
    bool have_previous = false;
    for (size_t i = 0U; i < chunks->count; ++i) {
        const Chunk *chunk = &chunks->items[i];
        if (chunk->logical > UINT64_MAX - chunk->length) {
            set_error(error, error_size, "Btrfs chunk address overflows");
            return -1;
        }
        if (have_previous && chunk->logical < previous_end) {
            set_error(error, error_size, "overlapping Btrfs chunks");
            return -1;
        }
        if ((chunk->chunk_type & BTRFS_STRIPED_PROFILES) != 0U) {
            set_error(error, error_size,
                      "striped Btrfs profiles are not yet supported by the native analyser");
            return -1;
        }
        size_t local = 0U;
        for (uint16_t s = 0U; s < chunk->stripe_count; ++s) {
            if (chunk->stripes[s].devid != devid)
                continue;
            const uint64_t physical = chunk->stripes[s].physical;
            if (physical > device_size || chunk->length > device_size - physical) {
                set_error(error, error_size, "Btrfs chunk stripe points outside the device");
                return -1;
            }
            local++;
        }
        if (local == 0U) {
            set_error(error, error_size, "Btrfs chunk has no stripe on this device");
            return -1;
        }
        previous_end = chunk->logical + chunk->length;
        have_previous = true;
    }
    return 0;
}

static const Chunk *mapper_find(const ChunkVec *chunks, uint64_t logical)
{
    size_t low = 0U;
    size_t high = chunks->count;
    while (low < high) {
        const size_t middle = low + (high - low) / 2U;
        if (chunks->items[middle].logical <= logical)
            low = middle + 1U;
        else
            high = middle;
    }
    if (low == 0U)
        return NULL;
    const Chunk *chunk = &chunks->items[low - 1U];
    if (logical < chunk->logical || logical - chunk->logical >= chunk->length)
        return NULL;
    return chunk;
}

static int mapper_read_physical(const ChunkVec *chunks, uint64_t devid,
                                uint64_t logical, uint64_t length, uint64_t *physical,
                                char *error, size_t error_size)
{
    const Chunk *chunk = mapper_find(chunks, logical);
    if (chunk == NULL) {
        set_error(error, error_size, "Btrfs logical address has no chunk mapping");
        return -1;
    }
    const uint64_t delta = logical - chunk->logical;
    if (length > chunk->length - delta) {
        set_error(error, error_size, "Btrfs tree block crosses a chunk boundary");
        return -1;
    }
    for (uint16_t i = 0U; i < chunk->stripe_count; ++i) {
        if (chunk->stripes[i].devid == devid) {
            *physical = chunk->stripes[i].physical + delta;
            return 0;
        }
    }
    set_error(error, error_size, "Btrfs logical address is not stored on this device");
    return -1;
}

static int mapper_ranges(const ChunkVec *chunks, uint64_t devid, uint64_t device_size,
                         uint64_t logical, uint64_t length, RangeVec *output,
                         char *error, size_t error_size)
{
    if (length == 0U)
        return 0;
    uint64_t cursor = logical;
    uint64_t remaining = length;
    while (remaining != 0U) {
        const Chunk *chunk = mapper_find(chunks, cursor);
        if (chunk == NULL) {
            set_error(error, error_size, "Btrfs logical extent has no chunk mapping");
            return -1;
        }
        const uint64_t delta = cursor - chunk->logical;
        const uint64_t available = chunk->length - delta;
        const uint64_t take = remaining < available ? remaining : available;
        size_t local = 0U;
        for (uint16_t i = 0U; i < chunk->stripe_count; ++i) {
            if (chunk->stripes[i].devid != devid)
                continue;
            const uint64_t start = chunk->stripes[i].physical + delta;
            if (start > device_size || take > device_size - start) {
                set_error(error, error_size, "Btrfs physical extent points outside the device");
                return -1;
            }
            if (range_push(output, start, start + take, error, error_size) != 0)
                return -1;
            local++;
        }
        if (local == 0U) {
            set_error(error, error_size, "Btrfs extent has no local physical mirror");
            return -1;
        }
        cursor += take;
        remaining -= take;
    }
    return 0;
}

static uint64_t hash_u64(uint64_t value)
{
    value ^= value >> 33U;
    value *= UINT64_C(0xff51afd7ed558ccd);
    value ^= value >> 33U;
    value *= UINT64_C(0xc4ceb9fe1a85ec53);
    value ^= value >> 33U;
    return value;
}

static int set_rehash(U64Set *set, size_t new_capacity, char *error, size_t error_size)
{
    uint64_t *slots = calloc(new_capacity, sizeof(*slots));
    if (slots == NULL) {
        set_error(error, error_size, "out of memory tracking Btrfs tree blocks");
        return -1;
    }
    for (size_t i = 0U; i < set->capacity; ++i) {
        const uint64_t value = set->slots[i];
        if (value == 0U)
            continue;
        size_t pos = (size_t)(hash_u64(value) & (uint64_t)(new_capacity - 1U));
        while (slots[pos] != 0U)
            pos = (pos + 1U) & (new_capacity - 1U);
        slots[pos] = value;
    }
    free(set->slots);
    set->slots = slots;
    set->capacity = new_capacity;
    return 0;
}

static int set_insert(U64Set *set, uint64_t value, bool *inserted,
                      char *error, size_t error_size)
{
    if (value == 0U) {
        set_error(error, error_size, "invalid zero Btrfs tree pointer");
        return -1;
    }
    if (set->capacity == 0U && set_rehash(set, 1024U, error, error_size) != 0)
        return -1;
    if ((set->count + 1U) * 10U >= set->capacity * 7U) {
        if (set->capacity > SIZE_MAX / 2U ||
            set_rehash(set, set->capacity * 2U, error, error_size) != 0)
            return -1;
    }
    size_t pos = (size_t)(hash_u64(value) & (uint64_t)(set->capacity - 1U));
    while (set->slots[pos] != 0U) {
        if (set->slots[pos] == value) {
            *inserted = false;
            return 0;
        }
        pos = (pos + 1U) & (set->capacity - 1U);
    }
    set->slots[pos] = value;
    set->count++;
    *inserted = true;
    return 0;
}

static int tree_walk(const Reader *reader, const ChunkVec *chunks, uint64_t devid,
                     uint32_t node_size, uint64_t root, uint8_t root_level,
                     ItemVec *items, U64Vec *blocks, char *error, size_t error_size)
{
    RefVec stack = {0};
    U64Set visited = {0};
    uint8_t *raw = malloc(node_size);
    if (raw == NULL) {
        set_error(error, error_size, "out of memory reading Btrfs tree block");
        return -1;
    }
    if (ref_push(&stack, root, root_level, error, error_size) != 0) {
        free(raw);
        return -1;
    }
    int result = -1;
    while (stack.count != 0U) {
        const TreeRef current = stack.items[--stack.count];
        bool inserted = false;
        if (set_insert(&visited, current.logical, &inserted, error, error_size) != 0)
            goto done;
        if (!inserted)
            continue;
        if (visited.count > BTRFS_MAX_TREE_BLOCKS) {
            set_error(error, error_size, "Btrfs tree traversal exceeded the safety limit");
            goto done;
        }
        if (current.logical % node_size != 0U) {
            set_error(error, error_size, "invalid Btrfs tree-block address");
            goto done;
        }
        uint64_t physical = 0U;
        if (mapper_read_physical(chunks, devid, current.logical, node_size, &physical,
                                 error, error_size) != 0 ||
            read_exact(reader, physical, raw, node_size, error, error_size) != 0)
            goto done;
        if (le64(raw + 48U) != current.logical) {
            set_error(error, error_size, "Btrfs tree-block bytenr mismatch");
            goto done;
        }
        const uint8_t level = raw[100U];
        if (level > BTRFS_MAX_TREE_LEVEL || level != current.level) {
            set_error(error, error_size, "invalid Btrfs tree level");
            goto done;
        }
        const uint32_t nritems = le32(raw + 96U);
        const size_t element_size = level == 0U ? BTRFS_ITEM_SIZE : BTRFS_KEY_PTR_SIZE;
        if ((size_t)nritems > (SIZE_MAX - BTRFS_HEADER_SIZE) / element_size ||
            BTRFS_HEADER_SIZE + (size_t)nritems * element_size > node_size) {
            set_error(error, error_size, "invalid Btrfs tree item count");
            goto done;
        }
        if (u64_push(blocks, current.logical, error, error_size) != 0)
            goto done;
        if (level == 0U) {
            const size_t table_end = BTRFS_HEADER_SIZE + (size_t)nritems * BTRFS_ITEM_SIZE;
            for (uint32_t index = 0U; index < nritems; ++index) {
                const size_t pos = BTRFS_HEADER_SIZE + (size_t)index * BTRFS_ITEM_SIZE;
                const Key key = parse_key(raw + pos);
                const uint32_t relative = le32(raw + pos + 17U);
                const uint32_t data_size = le32(raw + pos + 21U);
                if ((uint64_t)BTRFS_HEADER_SIZE + relative > node_size) {
                    set_error(error, error_size, "Btrfs leaf item lies outside the tree block");
                    goto done;
                }
                const size_t data_offset = BTRFS_HEADER_SIZE + (size_t)relative;
                if (data_offset < table_end || data_size > node_size - data_offset) {
                    set_error(error, error_size, data_offset < table_end ?
                              "Btrfs leaf item overlaps its item table" :
                              "Btrfs leaf item lies outside the tree block");
                    goto done;
                }
                if (item_push(items, key, raw + data_offset, data_size, error, error_size) != 0)
                    goto done;
            }
        } else {
            for (uint32_t reverse = nritems; reverse != 0U; --reverse) {
                const uint32_t index = reverse - 1U;
                const size_t pos = BTRFS_HEADER_SIZE + (size_t)index * BTRFS_KEY_PTR_SIZE;
                const uint64_t child = le64(raw + pos + 17U);
                if (child == 0U) {
                    set_error(error, error_size, "invalid Btrfs child pointer");
                    goto done;
                }
                if (ref_push(&stack, child, (uint8_t)(level - 1U), error, error_size) != 0)
                    goto done;
            }
        }
    }
    result = 0;
done:
    free(raw);
    free(stack.items);
    free(visited.slots);
    return result;
}

static int parse_system_chunks(const uint8_t *superblock, ChunkVec *chunks,
                               char *error, size_t error_size)
{
    const uint32_t size = le32(superblock + 160U);
    if (size == 0U || size > BTRFS_MAX_SYSTEM_ARRAY || 811U + size > BTRFS_SUPER_SIZE) {
        set_error(error, error_size, "invalid Btrfs system chunk array size");
        return -1;
    }
    const uint8_t *data = superblock + 811U;
    size_t pos = 0U;
    while (pos < size) {
        if (size - pos < BTRFS_DISK_KEY_SIZE + BTRFS_CHUNK_FIXED_SIZE) {
            set_error(error, error_size, "truncated Btrfs system chunk array");
            return -1;
        }
        const Key key = parse_key(data + pos);
        if (key.type != BTRFS_CHUNK_ITEM) {
            set_error(error, error_size, "unexpected key in Btrfs system chunk array");
            return -1;
        }
        const uint16_t stripes = le16(data + pos + BTRFS_DISK_KEY_SIZE + 44U);
        const size_t item_size = BTRFS_CHUNK_FIXED_SIZE + (size_t)stripes * BTRFS_STRIPE_SIZE;
        if (item_size > size - pos - BTRFS_DISK_KEY_SIZE) {
            set_error(error, error_size, "truncated Btrfs system chunk item");
            return -1;
        }
        Chunk chunk = {0};
        if (parse_chunk(data + pos + BTRFS_DISK_KEY_SIZE, item_size, key.offset,
                        &chunk, error, error_size) != 0)
            return -1;
        if (chunk_add_unique(chunks, &chunk, error, error_size) != 0) {
            chunk_free(&chunk);
            return -1;
        }
        pos += BTRFS_DISK_KEY_SIZE + item_size;
    }
    return 0;
}

static int root_push_or_update(RootVec *roots, uint64_t objectid, uint64_t bytenr,
                               uint32_t refs, uint8_t level, uint64_t key_offset,
                               char *error, size_t error_size)
{
    for (size_t i = 0U; i < roots->count; ++i) {
        if (roots->items[i].objectid != objectid)
            continue;
        if (key_offset >= roots->items[i].key_offset)
            roots->items[i] = (RootRecord){objectid, bytenr, key_offset, refs, level};
        return 0;
    }
    if (grow_array((void **)&roots->items, &roots->capacity, roots->count,
                   sizeof(*roots->items), error, error_size) != 0)
        return -1;
    roots->items[roots->count++] = (RootRecord){objectid, bytenr, key_offset, refs, level};
    return 0;
}

static int collect_roots(const ItemVec *items, RootVec *roots,
                         char *error, size_t error_size)
{
    for (size_t i = 0U; i < items->count; ++i) {
        const TreeItem *item = &items->items[i];
        if (item->key.type != BTRFS_ROOT_ITEM || item->size < 239U)
            continue;
        const uint64_t bytenr = le64(item->data + 176U);
        const uint32_t refs = le32(item->data + 216U);
        const uint8_t level = item->data[238U];
        if (bytenr != 0U && level <= BTRFS_MAX_TREE_LEVEL &&
            root_push_or_update(roots, item->key.objectid, bytenr, refs, level,
                                item->key.offset, error, error_size) != 0)
            return -1;
    }
    return 0;
}

static const RootRecord *find_root(const RootVec *roots, uint64_t objectid)
{
    for (size_t i = 0U; i < roots->count; ++i)
        if (roots->items[i].objectid == objectid)
            return &roots->items[i];
    return NULL;
}

static int run_push_coalesced(RunVec *runs, FileRun run,
                              char *error, size_t error_size)
{
    if (run.length == 0U)
        return 0;
    if (runs->count != 0U) {
        FileRun *last = &runs->items[runs->count - 1U];
        if (!last->encoded && !run.encoded &&
            last->logical <= UINT64_MAX - last->length &&
            last->physical <= UINT64_MAX - last->length &&
            last->logical + last->length == run.logical &&
            last->physical + last->length == run.physical) {
            if (last->length > UINT64_MAX - run.length ||
                last->disk_length > UINT64_MAX - run.disk_length) {
                set_error(error, error_size, "Btrfs file extent length overflows");
                return -1;
            }
            last->length += run.length;
            last->disk_length += run.disk_length;
            return 0;
        }
    }
    if (grow_array((void **)&runs->items, &runs->capacity, runs->count,
                   sizeof(*runs->items), error, error_size) != 0)
        return -1;
    runs->items[runs->count++] = run;
    return 0;
}

static int finish_inode(uint32_t mode, bool have_inode, RunVec *runs,
                        BtrfsAnalysis *analysis, RangeVec *fragmented,
                        char *error, size_t error_size)
{
    if (!have_inode) {
        runs->count = 0U;
        return 0;
    }
    const uint32_t type = mode & (uint32_t)S_IFMT;
    if (type == (uint32_t)S_IFREG) {
        analysis->regular_files++;
        if (runs->count > 1U) {
            analysis->fragmented_files++;
            for (size_t i = 0U; i < runs->count; ++i) {
                const FileRun *run = &runs->items[i];
                if (run->disk_start > UINT64_MAX - run->disk_length) {
                    set_error(error, error_size, "Btrfs fragmented range overflows");
                    return -1;
                }
                if (range_push(fragmented, run->disk_start,
                               run->disk_start + run->disk_length,
                               error, error_size) != 0)
                    return -1;
            }
        }
    } else if (type == (uint32_t)S_IFDIR) {
        analysis->directories++;
    }
    runs->count = 0U;
    return 0;
}

static int scan_filesystem_tree(const ItemVec *items, const ChunkVec *chunks,
                                uint64_t devid, uint64_t device_size,
                                BtrfsAnalysis *analysis, RangeVec *fragmented,
                                char *error, size_t error_size)
{
    uint64_t current_inode = UINT64_MAX;
    uint32_t inode_mode = 0U;
    bool have_inode = false;
    RunVec runs = {0};
    int result = -1;
    for (size_t i = 0U; i < items->count; ++i) {
        const TreeItem *item = &items->items[i];
        if (item->key.objectid != current_inode) {
            if (current_inode != UINT64_MAX &&
                finish_inode(inode_mode, have_inode, &runs, analysis, fragmented,
                             error, error_size) != 0)
                goto done;
            current_inode = item->key.objectid;
            inode_mode = 0U;
            have_inode = false;
        }
        if (item->key.type == BTRFS_INODE_ITEM && item->size >= 56U) {
            inode_mode = le32(item->data + 52U);
            have_inode = true;
            continue;
        }
        if (item->key.type != BTRFS_EXTENT_DATA || item->size < 21U)
            continue;
        const uint8_t extent_type = item->data[20U];
        if (extent_type == BTRFS_FILE_EXTENT_INLINE)
            continue;
        if ((extent_type != BTRFS_FILE_EXTENT_REG &&
             extent_type != BTRFS_FILE_EXTENT_PREALLOC) || item->size < 53U) {
            analysis->malformed_items++;
            continue;
        }
        const uint64_t disk_bytenr = le64(item->data + 21U);
        const uint64_t disk_num_bytes = le64(item->data + 29U);
        const uint64_t extent_offset = le64(item->data + 37U);
        const uint64_t num_bytes = le64(item->data + 45U);
        if (disk_bytenr == 0U || num_bytes == 0U)
            continue;
        const bool encoded = item->data[16U] != 0U || item->data[17U] != 0U ||
                             le16(item->data + 18U) != 0U;
        uint64_t logical = disk_bytenr;
        uint64_t length = encoded ? disk_num_bytes : num_bytes;
        if (!encoded) {
            if (disk_bytenr > UINT64_MAX - extent_offset) {
                analysis->malformed_items++;
                continue;
            }
            logical += extent_offset;
        }
        if (length == 0U) {
            analysis->malformed_items++;
            continue;
        }
        RangeVec physical = {0};
        char local_error[256] = {0};
        if (mapper_ranges(chunks, devid, device_size, logical, length, &physical,
                          local_error, sizeof(local_error)) != 0 || physical.count == 0U) {
            free(physical.items);
            analysis->malformed_items++;
            continue;
        }
        const uint64_t disk_start = physical.items[0].start;
        const uint64_t disk_length = physical.items[0].end - physical.items[0].start;
        free(physical.items);
        const FileRun run = {
            item->key.offset,
            disk_start,
            num_bytes,
            disk_start,
            encoded ? disk_length : num_bytes,
            encoded,
        };
        if (run_push_coalesced(&runs, run, error, error_size) != 0)
            goto done;
    }
    if (current_inode != UINT64_MAX &&
        finish_inode(inode_mode, have_inode, &runs, analysis, fragmented,
                     error, error_size) != 0)
        goto done;
    result = 0;
done:
    free(runs.items);
    return result;
}

static int scan_filesystems(const Reader *reader, const ChunkVec *chunks, uint64_t devid,
                            uint32_t node_size, const RootVec *roots,
                            BtrfsAnalysis *analysis, RangeVec *fragmented,
                            char *error, size_t error_size)
{
    for (size_t i = 0U; i < roots->count; ++i) {
        const RootRecord *record = &roots->items[i];
        if (record->refs == 0U ||
            (record->objectid != BTRFS_FS_TREE_OBJECTID &&
             record->objectid < BTRFS_FIRST_FREE_OBJECTID))
            continue;
        analysis->filesystem_roots_scanned++;
        ItemVec items = {0};
        U64Vec blocks = {0};
        char local_error[256] = {0};
        if (tree_walk(reader, chunks, devid, node_size, record->bytenr, record->level,
                      &items, &blocks, local_error, sizeof(local_error)) != 0) {
            analysis->malformed_items++;
            item_vec_free(&items);
            free(blocks.items);
            continue;
        }
        analysis->filesystem_tree_blocks += (uint64_t)blocks.count;
        if (scan_filesystem_tree(&items, chunks, devid, analysis->total_bytes,
                                 analysis, fragmented, error, error_size) != 0) {
            item_vec_free(&items);
            free(blocks.items);
            return -1;
        }
        item_vec_free(&items);
        free(blocks.items);
    }
    return 0;
}

bool btrfs_probe(const char *path)
{
    Reader reader = {.fd = -1, .size = 0U};
    char error[128];
    if (open_reader(path, &reader, error, sizeof(error)) != 0)
        return false;
    uint8_t magic[8];
    const bool matched =
        read_exact(&reader, BTRFS_SUPER_OFFSET + 0x40U, magic, sizeof(magic),
                   error, sizeof(error)) == 0 &&
        memcmp(magic, "_BHRfS_M", sizeof(magic)) == 0;
    close_reader(&reader);
    return matched;
}

void btrfs_analysis_free(BtrfsAnalysis *analysis)
{
    if (analysis == NULL)
        return;
    free(analysis->used_ranges);
    free(analysis->fragmented_ranges);
    memset(analysis, 0, sizeof(*analysis));
}

int btrfs_analyse(const char *path, BtrfsAnalysis *analysis,
                  char *error, size_t error_size)
{
    if (analysis == NULL) {
        set_error(error, error_size, "missing Btrfs analysis output");
        return -1;
    }
    memset(analysis, 0, sizeof(*analysis));
    Reader reader = {.fd = -1, .size = 0U};
    ChunkVec chunks = {0};
    ItemVec chunk_items = {0};
    U64Vec chunk_blocks = {0};
    ItemVec root_items = {0};
    U64Vec root_blocks = {0};
    ItemVec extent_items = {0};
    U64Vec extent_blocks = {0};
    RootVec roots = {0};
    RangeVec used = {0};
    RangeVec fragmented = {0};
    int result = -1;

    if (open_reader(path, &reader, error, error_size) != 0)
        goto done;
    if (reader.size < BTRFS_SUPER_OFFSET + BTRFS_SUPER_SIZE) {
        set_error(error, error_size, "Btrfs target is too small for a superblock");
        goto done;
    }
    uint8_t superblock[BTRFS_SUPER_SIZE];
    if (read_exact(&reader, BTRFS_SUPER_OFFSET, superblock, sizeof(superblock),
                   error, error_size) != 0)
        goto done;
    if (memcmp(superblock + 0x40U, "_BHRfS_M", 8U) != 0) {
        set_error(error, error_size, "not a Btrfs volume");
        goto done;
    }

    const uint64_t root = le64(superblock + 80U);
    const uint64_t chunk_root = le64(superblock + 88U);
    const uint64_t total_bytes = le64(superblock + 112U);
    const uint64_t bytes_used = le64(superblock + 120U);
    const uint64_t num_devices = le64(superblock + 136U);
    const uint32_t sector_size = le32(superblock + 144U);
    const uint32_t node_size = le32(superblock + 148U);
    const uint8_t root_level = superblock[198U];
    const uint8_t chunk_root_level = superblock[199U];
    const uint64_t devid = le64(superblock + 201U);

    if (num_devices != 1U) {
        set_error(error, error_size,
                  "native exact Btrfs analysis currently supports single-device filesystems only");
        goto done;
    }
    if (total_bytes == 0U || total_bytes > reader.size) {
        set_error(error, error_size, "invalid Btrfs device size");
        goto done;
    }
    if (sector_size < 4096U || (sector_size & (sector_size - 1U)) != 0U) {
        set_error(error, error_size, "unsupported Btrfs sector size");
        goto done;
    }
    if (node_size < sector_size || node_size > 65536U ||
        (node_size & (node_size - 1U)) != 0U) {
        set_error(error, error_size, "unsupported Btrfs node size");
        goto done;
    }
    if (root == 0U || chunk_root == 0U || root_level > BTRFS_MAX_TREE_LEVEL ||
        chunk_root_level > BTRFS_MAX_TREE_LEVEL) {
        set_error(error, error_size, "invalid Btrfs tree roots");
        goto done;
    }

    analysis->total_bytes = total_bytes;
    analysis->physical_bytes = reader.size;
    analysis->logical_bytes_used = bytes_used;
    analysis->sector_size = sector_size;
    analysis->node_size = node_size;
    analysis->device_id = devid;
    analysis->fragmentation_available = true;

    if (parse_system_chunks(superblock, &chunks, error, error_size) != 0 ||
        mapper_prepare(&chunks, devid, total_bytes, error, error_size) != 0)
        goto done;
    if (tree_walk(&reader, &chunks, devid, node_size, chunk_root, chunk_root_level,
                  &chunk_items, &chunk_blocks, error, error_size) != 0)
        goto done;
    for (size_t i = 0U; i < chunk_items.count; ++i) {
        const TreeItem *item = &chunk_items.items[i];
        if (item->key.type != BTRFS_CHUNK_ITEM)
            continue;
        Chunk chunk = {0};
        if (parse_chunk(item->data, item->size, item->key.offset,
                        &chunk, error, error_size) != 0)
            goto done;
        if (chunk_add_unique(&chunks, &chunk, error, error_size) != 0) {
            chunk_free(&chunk);
            goto done;
        }
    }
    if (mapper_prepare(&chunks, devid, total_bytes, error, error_size) != 0)
        goto done;

    if (tree_walk(&reader, &chunks, devid, node_size, root, root_level,
                  &root_items, &root_blocks, error, error_size) != 0 ||
        collect_roots(&root_items, &roots, error, error_size) != 0)
        goto done;
    const RootRecord *extent_record = find_root(&roots, BTRFS_EXTENT_TREE_OBJECTID);
    if (extent_record == NULL) {
        set_error(error, error_size, "Btrfs root tree does not contain the extent tree");
        goto done;
    }
    if (tree_walk(&reader, &chunks, devid, node_size, extent_record->bytenr,
                  extent_record->level, &extent_items, &extent_blocks,
                  error, error_size) != 0)
        goto done;

    for (size_t i = 0U; i < extent_items.count; ++i) {
        const TreeItem *item = &extent_items.items[i];
        uint64_t length = 0U;
        if (item->key.type == BTRFS_EXTENT_ITEM)
            length = item->key.offset;
        else if (item->key.type == BTRFS_METADATA_ITEM)
            length = node_size;
        if (item->key.objectid != 0U && length != 0U &&
            mapper_ranges(&chunks, devid, total_bytes, item->key.objectid, length,
                          &used, error, error_size) != 0)
            goto done;
    }

    const uint64_t mirrors[] = {
        64ULL * 1024ULL,
        64ULL * 1024ULL * 1024ULL,
        256ULL * 1024ULL * 1024ULL * 1024ULL,
    };
    for (size_t i = 0U; i < sizeof(mirrors) / sizeof(mirrors[0]); ++i) {
        if (mirrors[i] <= total_bytes && BTRFS_SUPER_SIZE <= total_bytes - mirrors[i] &&
            range_push(&used, mirrors[i], mirrors[i] + BTRFS_SUPER_SIZE,
                       error, error_size) != 0)
            goto done;
    }
    range_merge(&used);

    if (scan_filesystems(&reader, &chunks, devid, node_size, &roots,
                         analysis, &fragmented, error, error_size) != 0)
        goto done;
    range_merge(&fragmented);

    analysis->chunk_count = chunks.count;
    analysis->chunk_tree_blocks = chunk_blocks.count;
    analysis->root_tree_blocks = root_blocks.count;
    analysis->extent_tree_blocks = extent_blocks.count;
    analysis->used_ranges = used.items;
    analysis->used_range_count = used.count;
    used.items = NULL;
    used.count = used.capacity = 0U;
    analysis->fragmented_ranges = fragmented.items;
    analysis->fragmented_range_count = fragmented.count;
    fragmented.items = NULL;
    fragmented.count = fragmented.capacity = 0U;
    result = 0;

done:
    if (result != 0)
        btrfs_analysis_free(analysis);
    free(used.items);
    free(fragmented.items);
    free(roots.items);
    item_vec_free(&extent_items);
    free(extent_blocks.items);
    item_vec_free(&root_items);
    free(root_blocks.items);
    item_vec_free(&chunk_items);
    free(chunk_blocks.items);
    chunk_vec_free(&chunks);
    if (reader.fd >= 0)
        close_reader(&reader);
    return result;
}
