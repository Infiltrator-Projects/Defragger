// SPDX-License-Identifier: GPL-3.0-or-later
#include "ext_native.h"

#include "ld_io.h"
#include "ld_runtime.h"
#include "ld_stop.h"

#include <com_err.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <openssl/sha.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef LINUX_S_IFMT
#define LINUX_S_IFMT 0170000
#endif
#ifndef LINUX_S_IFDIR
#define LINUX_S_IFDIR 0040000
#endif
#ifndef LINUX_S_IFREG
#define LINUX_S_IFREG 0100000
#endif

typedef struct {
    int64_t logical;
    uint64_t physical;
    bool data;
} ExtBlockRef;

typedef struct {
    ExtBlockRef *items;
    size_t count;
    size_t capacity;
} ExtBlockVec;

static void block_push(ExtBlockVec *vec, int64_t logical, uint64_t physical, bool data) {
    if (physical == 0) return;
    if (vec->count == vec->capacity) {
        size_t next = vec->capacity == 0 ? 32U : vec->capacity * 2U;
        vec->items = ld_xrealloc(vec->items, next * sizeof(*vec->items));
        vec->capacity = next;
    }
    vec->items[vec->count++] = (ExtBlockRef){logical, physical, data};
}

static void block_free(ExtBlockVec *vec) {
    free(vec->items);
    memset(vec, 0, sizeof(*vec));
}

typedef struct {
    ExtBlockVec *blocks;
} CollectContext;

static int collect_all_blocks(ext2_filsys fs, blk64_t *blocknr,
                              e2_blkcnt_t blockcnt, blk64_t ref_blk,
                              int ref_offset, void *private_data) {
    (void)fs;
    (void)ref_blk;
    (void)ref_offset;
    CollectContext *context = private_data;
    if (*blocknr != 0)
        block_push(context->blocks, (int64_t)blockcnt, (uint64_t)*blocknr, blockcnt >= 0);
    return 0;
}

static int collect_inode_blocks(ext2_filsys fs, ext2_ino_t ino, bool data_only,
                                ExtBlockVec *blocks, char **error) {
    CollectContext context = {.blocks = blocks};
    int flags = BLOCK_FLAG_READ_ONLY | (data_only ? BLOCK_FLAG_DATA_ONLY : 0);
    errcode_t code = ext2fs_block_iterate3(fs, ino, flags, NULL,
                                           collect_all_blocks, &context);
    if (code != 0) {
        ext_set_error(error, "reading EXT inode %u allocation: %s",
                      (unsigned)ino, error_message(code));
        return -1;
    }
    return 0;
}

static bool allocation_is_contiguous(const ExtBlockVec *blocks, uint64_t *last_block) {
    if (blocks->count == 0) {
        if (last_block != NULL) *last_block = 0;
        return true;
    }
    uint64_t minimum = UINT64_MAX, maximum = 0;
    for (size_t index = 0; index < blocks->count; ++index) {
        uint64_t physical = blocks->items[index].physical;
        if (physical < minimum) minimum = physical;
        if (physical > maximum) maximum = physical;
    }
    if (last_block != NULL) *last_block = maximum;
    return maximum >= minimum && maximum - minimum + 1U == blocks->count;
}

static size_t data_count(const ExtBlockVec *blocks) {
    size_t result = 0;
    for (size_t index = 0; index < blocks->count; ++index)
        if (blocks->items[index].data) result++;
    return result;
}

static bool bitmap_free(ext2_filsys fs, uint64_t block) {
    if (block >= ext2fs_blocks_count(fs->super)) return false;
    return !ext2fs_test_block_bitmap2(fs->block_map, (blk64_t)block);
}

static void add_data_ranges(const ExtBlockVec *blocks, ExtRangeVec *ranges) {
    for (size_t index = 0; index < blocks->count; ++index)
        if (blocks->items[index].data)
            ext_range_push(ranges, blocks->items[index].physical,
                           blocks->items[index].physical + 1U);
}

static int scan_free_ranges(ext2_filsys fs, const ExtGeometry *geometry,
                            ExtRangeVec *ranges) {
    bool in_run = false;
    uint64_t start = 0;
    for (uint64_t block = geometry->first_data_block;
         block < geometry->total_blocks; ++block) {
        bool free_block = bitmap_free(fs, block);
        if (free_block && !in_run) {
            start = block;
            in_run = true;
        } else if (!free_block && in_run) {
            ext_range_push(ranges, start, block);
            in_run = false;
        }
    }
    if (in_run) ext_range_push(ranges, start, geometry->total_blocks);
    return 0;
}

int ext_scan_catalogue(const char *path, ExtGeometry *geometry,
                       ExtCatalogue *catalogue, char **error) {
    memset(catalogue, 0, sizeof(*catalogue));
    catalogue->growth_10_satisfied = true;
    if (ext_read_geometry(path, geometry, error) != 0) return -1;
    ext2_filsys fs = NULL;
    if (ext_open_fs(path, false, &fs, error) != 0) return -1;
    int result = -1;
    if (scan_free_ranges(fs, geometry, &catalogue->free_ranges) != 0) goto done;

    ext2_inode_scan scan = NULL;
    errcode_t code = ext2fs_open_inode_scan(fs, 0, &scan);
    if (code != 0) {
        ext_set_error(error, "opening EXT inode scan: %s", error_message(code));
        goto done;
    }
    struct ext2_inode_large inode;
    ext2_ino_t ino = 0;
    while ((code = ext2fs_get_next_inode_full(scan, &ino,
             (struct ext2_inode *)&inode, sizeof(inode))) == 0 && ino != 0) {
        if (inode.i_mode == 0 || inode.i_links_count == 0) continue;
        unsigned kind = (unsigned)inode.i_mode & LINUX_S_IFMT;
        if (kind != LINUX_S_IFREG && kind != LINUX_S_IFDIR) continue;
        catalogue->inodes_scanned++;
        ExtBlockVec blocks = {0};
        char *local_error = NULL;
        /* Fragmentation policy is about file/directory payload placement.
           Extent-tree and indirect blocks are filesystem metadata and stay fixed. */
        if (collect_inode_blocks(fs, ino, true, &blocks, &local_error) != 0) {
            catalogue->malformed_inodes++;
            free(local_error);
            block_free(&blocks);
            continue;
        }
        size_t count = data_count(&blocks);
        uint64_t last_allocation = 0;
        bool contiguous = allocation_is_contiguous(&blocks, &last_allocation);
        bool fragmented = blocks.count > 0U && !contiguous;
        ext2_ino_t first_normal = fs->super->s_first_ino != 0 ?
                                  fs->super->s_first_ino : EXT2_GOOD_OLD_FIRST_INO;
        /* Low-numbered EXT inodes include the resize inode and journal.  They
           are filesystem structures, not user objects and must never drive
           fragmentation or growth-reserve policy. */
        if (ino < first_normal && ino != EXT2_ROOT_INO) {
            block_free(&blocks);
            continue;
        }
        if (kind == LINUX_S_IFDIR) {
            catalogue->directories++;
            add_data_ranges(&blocks, &catalogue->directory_ranges);
            if (fragmented) catalogue->fragmented_directories++;
        } else {
            catalogue->regular_files++;
            if (fragmented) catalogue->fragmented_files++;
            if (count != 0U) {
                if (!contiguous) {
                    catalogue->growth_10_satisfied = false;
                } else {
                    uint64_t reserve = ((uint64_t)count + 9U) / 10U;
                    for (uint64_t offset = 1; offset <= reserve; ++offset) {
                        if (last_allocation + offset >= geometry->total_blocks ||
                            !bitmap_free(fs, last_allocation + offset)) {
                            catalogue->growth_10_satisfied = false;
                            break;
                        }
                    }
                }
            }
        }
        if (fragmented) add_data_ranges(&blocks, &catalogue->fragmented_ranges);
        block_free(&blocks);
    }
    if (code != 0) {
        ext_set_error(error, "reading EXT inode catalogue: %s", error_message(code));
        ext2fs_close_inode_scan(scan);
        goto done;
    }
    ext2fs_close_inode_scan(scan);
    ext_range_sort_merge(&catalogue->free_ranges);
    ext_range_sort_merge(&catalogue->fragmented_ranges);
    ext_range_sort_merge(&catalogue->directory_ranges);
    if (catalogue->regular_files == 0) catalogue->growth_10_satisfied = false;
    result = 0;
done:
    (void)ext2fs_close(fs);
    if (result != 0) ext_catalogue_free(catalogue);
    return result;
}

void ext_catalogue_free(ExtCatalogue *catalogue) {
    if (catalogue == NULL) return;
    ext_range_free(&catalogue->free_ranges);
    ext_range_free(&catalogue->fragmented_ranges);
    ext_range_free(&catalogue->directory_ranges);
    memset(catalogue, 0, sizeof(*catalogue));
}

static int sql_exec(sqlite3 *db, const char *sql, char **error) {
    char *message = NULL;
    int code = sqlite3_exec(db, sql, NULL, NULL, &message);
    if (code != SQLITE_OK) {
        ext_set_error(error, "EXT plan database: %s", message != NULL ? message : sqlite3_errmsg(db));
        sqlite3_free(message);
        return -1;
    }
    return 0;
}

int ext_open_plan_db(const char *path, bool create, sqlite3 **db, char **error) {
    if (create) {
        (void)unlink(path);
        char wal[4096], shm[4096];
        if (snprintf(wal, sizeof(wal), "%s-wal", path) > 0) (void)unlink(wal);
        if (snprintf(shm, sizeof(shm), "%s-shm", path) > 0) (void)unlink(shm);
    }
    int flags = create ? (SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE)
                       : SQLITE_OPEN_READWRITE;
#ifdef SQLITE_OPEN_NOFOLLOW
    flags |= SQLITE_OPEN_NOFOLLOW;
#endif
    int code = sqlite3_open_v2(path, db, flags, NULL);
    if (code != SQLITE_OK) {
        ext_set_error(error, "opening EXT plan database %s: %s", path,
                      *db != NULL ? sqlite3_errmsg(*db) : "SQLite error");
        if (*db != NULL) sqlite3_close(*db);
        *db = NULL;
        return -1;
    }
    if (sql_exec(*db, "PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL;", error) != 0)
        return -1;
    if (!create) return 0;
    return sql_exec(*db,
        "CREATE TABLE objects ("
        "inode INTEGER PRIMARY KEY,kind TEXT NOT NULL,mode INTEGER NOT NULL,"
        "links INTEGER NOT NULL,size INTEGER NOT NULL,data_block_count INTEGER NOT NULL,"
        "allocation_block_count INTEGER NOT NULL,payload_sha256 BLOB NOT NULL,sort_class INTEGER NOT NULL);"
        "CREATE TABLE blocks (inode INTEGER NOT NULL,sequence INTEGER NOT NULL,logical INTEGER NOT NULL,"
        "is_data INTEGER NOT NULL,old INTEGER PRIMARY KEY,target INTEGER,placed INTEGER NOT NULL DEFAULT 0,"
        "UNIQUE(inode,sequence));"
        "CREATE INDEX blocks_inode ON blocks(inode,sequence);"
        "CREATE TABLE reserves (inode INTEGER NOT NULL,start INTEGER NOT NULL,length INTEGER NOT NULL);"
        "CREATE TABLE spaces (start INTEGER PRIMARY KEY,length INTEGER NOT NULL);"
        "CREATE TABLE metadata (key TEXT PRIMARY KEY,value TEXT NOT NULL);", error);
}

static int payload_digest(int fd, uint32_t block_size, const ExtBlockVec *blocks,
                          uint8_t output[SHA256_DIGEST_LENGTH], char **error) {
    SHA256_CTX digest;
    if (SHA256_Init(&digest) != 1) {
        ext_set_error(error, "initializing EXT payload digest failed");
        return -1;
    }
    uint8_t *buffer = ld_xmalloc(block_size);
    for (size_t index = 0; index < blocks->count; ++index) {
        if (!blocks->items[index].data) continue;
        uint64_t physical = blocks->items[index].physical;
        ssize_t got = ld_pread_full(fd, buffer, block_size, physical * block_size);
        if (got < 0 || (size_t)got != block_size) {
            ext_set_error(error, "short read while hashing EXT payload data");
            free(buffer);
            return -1;
        }
        uint8_t logical[8];
        uint64_t value = (uint64_t)blocks->items[index].logical;
        for (unsigned byte = 0; byte < 8U; ++byte) logical[byte] = (uint8_t)(value >> (byte * 8U));
        if (SHA256_Update(&digest, logical, sizeof(logical)) != 1 ||
            SHA256_Update(&digest, buffer, block_size) != 1) {
            ext_set_error(error, "updating EXT payload digest failed");
            free(buffer);
            return -1;
        }
    }
    free(buffer);
    if (SHA256_Final(output, &digest) != 1) {
        ext_set_error(error, "finalizing EXT payload digest failed");
        return -1;
    }
    return 0;
}

static int bind_int64(sqlite3_stmt *stmt, int index, uint64_t value, char **error) {
    if (value > INT64_MAX) {
        ext_set_error(error, "EXT block number exceeds SQLite signed integer range");
        return -1;
    }
    return sqlite3_bind_int64(stmt, index, (sqlite3_int64)value) == SQLITE_OK ? 0 : -1;
}

int ext_catalog_plan(ext2_filsys fs, int raw_fd, sqlite3 *db,
                     const ExtGeometry *geometry, uint64_t *movable,
                     char **error) {
    *movable = 0;
    sqlite3_stmt *insert_object = NULL, *insert_block = NULL, *meta = NULL;
    if (sqlite3_prepare_v2(db,
        "INSERT INTO objects VALUES (?,?,?,?,?,?,?,?,?)", -1, &insert_object, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(db,
        "INSERT INTO blocks(inode,sequence,logical,is_data,old) VALUES (?,?,?,?,?)",
        -1, &insert_block, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(db, "INSERT INTO metadata VALUES (?,?)", -1, &meta, NULL) != SQLITE_OK) {
        ext_set_error(error, "preparing EXT plan database statements: %s", sqlite3_errmsg(db));
        goto fail;
    }
    if (sql_exec(db, "BEGIN IMMEDIATE", error) != 0) goto fail;
    ext2_inode_scan scan = NULL;
    errcode_t code = ext2fs_open_inode_scan(fs, 0, &scan);
    if (code != 0) {
        ext_set_error(error, "opening EXT inode plan scan: %s", error_message(code));
        goto rollback;
    }
    struct ext2_inode_large inode;
    ext2_ino_t ino = 0;
    uint64_t indexed = 0;
    while ((code = ext2fs_get_next_inode_full(scan, &ino,
             (struct ext2_inode *)&inode, sizeof(inode))) == 0 && ino != 0) {
        if (ld_stop_requested()) {
            ext_set_error(error, "stop requested before EXT source commit");
            ext2fs_close_inode_scan(scan);
            goto rollback;
        }
        unsigned kind = (unsigned)inode.i_mode & LINUX_S_IFMT;
        if (inode.i_mode == 0 || inode.i_links_count == 0 ||
            (kind != LINUX_S_IFREG && kind != LINUX_S_IFDIR)) continue;
        /* Reserved inodes, including the journal inode, are fixed filesystem
           metadata. Only the root directory is a movable low-numbered object. */
        if (ino < EXT2_GOOD_OLD_FIRST_INO && ino != EXT2_ROOT_INO)
            continue;
        ExtBlockVec blocks = {0};
        /* Plan only payload blocks. libext2fs owns the extent/indirect
           metadata that describes those mappings and updates it in place. */
        if (collect_inode_blocks(fs, ino, true, &blocks, error) != 0) {
            block_free(&blocks);
            ext2fs_close_inode_scan(scan);
            goto rollback;
        }
        size_t data_blocks = data_count(&blocks);
        uint8_t digest[SHA256_DIGEST_LENGTH];
        if (payload_digest(raw_fd, geometry->block_size, &blocks, digest, error) != 0) {
            block_free(&blocks);
            ext2fs_close_inode_scan(scan);
            goto rollback;
        }
        uint64_t size = kind == LINUX_S_IFREG ? EXT2_I_SIZE((struct ext2_inode *)&inode) : inode.i_size;
        int sort_class = ino == EXT2_ROOT_INO || ino == EXT2_JOURNAL_INO ? 0 :
                         (kind == LINUX_S_IFDIR ? 1 : 2);
        sqlite3_reset(insert_object); sqlite3_clear_bindings(insert_object);
        sqlite3_bind_int64(insert_object, 1, (sqlite3_int64)ino);
        sqlite3_bind_text(insert_object, 2, kind == LINUX_S_IFDIR ? "directory" : "file", -1, SQLITE_STATIC);
        sqlite3_bind_int(insert_object, 3, inode.i_mode);
        sqlite3_bind_int(insert_object, 4, inode.i_links_count);
        if (bind_int64(insert_object, 5, size, error) != 0) { block_free(&blocks); ext2fs_close_inode_scan(scan); goto rollback; }
        sqlite3_bind_int64(insert_object, 6, (sqlite3_int64)data_blocks);
        sqlite3_bind_int64(insert_object, 7, (sqlite3_int64)blocks.count);
        sqlite3_bind_blob(insert_object, 8, digest, SHA256_DIGEST_LENGTH, SQLITE_TRANSIENT);
        sqlite3_bind_int(insert_object, 9, sort_class);
        if (sqlite3_step(insert_object) != SQLITE_DONE) {
            ext_set_error(error, "cataloguing EXT inode %u: %s", (unsigned)ino, sqlite3_errmsg(db));
            block_free(&blocks); ext2fs_close_inode_scan(scan); goto rollback;
        }
        for (size_t index = 0; index < blocks.count; ++index) {
            sqlite3_reset(insert_block);
            sqlite3_clear_bindings(insert_block);
            sqlite3_bind_int64(insert_block, 1, (sqlite3_int64)ino);
            sqlite3_bind_int64(insert_block, 2, (sqlite3_int64)index);
            sqlite3_bind_int64(insert_block, 3, (sqlite3_int64)blocks.items[index].logical);
            sqlite3_bind_int(insert_block, 4, blocks.items[index].data ? 1 : 0);
            if (bind_int64(insert_block, 5, blocks.items[index].physical, error) != 0) {
                block_free(&blocks);
                ext2fs_close_inode_scan(scan);
                goto rollback;
            }
            if (sqlite3_step(insert_block) != SQLITE_DONE) {
                ext_set_error(error,
                    "shared or duplicate EXT allocation block %llu is not supported by the native mover",
                    (unsigned long long)blocks.items[index].physical);
                block_free(&blocks);
                ext2fs_close_inode_scan(scan);
                goto rollback;
            }
        }
        *movable += (uint64_t)blocks.count;
        indexed++;
        block_free(&blocks);
        if ((indexed % 256U) == 0U) {
            if (sql_exec(db, "COMMIT; BEGIN IMMEDIATE", error) != 0) {
                ext2fs_close_inode_scan(scan);
                goto rollback_no_tx;
            }
        }
    }
    ext2fs_close_inode_scan(scan);
    if (code != 0) {
        ext_set_error(error, "reading EXT inode plan catalogue: %s", error_message(code));
        goto rollback;
    }
    if (*movable == 0) {
        ext_set_error(error, "the EXT filesystem contains no movable file or directory blocks");
        goto rollback;
    }
    const char *keys[2] = {"block_size", "total_blocks"};
    uint64_t values[2] = {geometry->block_size, geometry->total_blocks};
    char rendered[64];
    for (size_t index = 0; index < 2U; ++index) {
        sqlite3_reset(meta);
        sqlite3_clear_bindings(meta);
        sqlite3_bind_text(meta, 1, keys[index], -1, SQLITE_STATIC);
        (void)snprintf(rendered, sizeof(rendered), "%llu",
                       (unsigned long long)values[index]);
        sqlite3_bind_text(meta, 2, rendered, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(meta) != SQLITE_DONE) {
            ext_set_error(error, "writing EXT plan metadata: %s", sqlite3_errmsg(db));
            goto rollback;
        }
    }
    if (sql_exec(db, "COMMIT", error) != 0) goto fail;
    sqlite3_finalize(insert_object);
    sqlite3_finalize(insert_block);
    sqlite3_finalize(meta);
    return 0;
rollback:
    (void)sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
rollback_no_tx:
fail:
    sqlite3_finalize(insert_object);
    sqlite3_finalize(insert_block);
    sqlite3_finalize(meta);
    return -1;
}
