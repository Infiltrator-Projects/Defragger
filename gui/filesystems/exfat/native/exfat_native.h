// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LINUX_DEFRAGGER_EXFAT_NATIVE_H
#define LINUX_DEFRAGGER_EXFAT_NATIVE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define EXFAT_EOC_MIN UINT32_C(0xfffffff8)
#define EXFAT_EOC UINT32_C(0xffffffff)
#define EXFAT_BAD_CLUSTER UINT32_C(0xfffffff7)
#define EXFAT_VOLUME_DIRTY UINT16_C(0x0002)
#define EXFAT_MEDIA_FAILURE UINT16_C(0x0004)

typedef struct { uint32_t *items; size_t count, capacity; } ExfatClusterVec;
typedef enum { EXFAT_OBJ_BITMAP, EXFAT_OBJ_UPCASE, EXFAT_OBJ_ROOT, EXFAT_OBJ_DIRECTORY, EXFAT_OBJ_FILE } ExfatObjectKind;

typedef struct {
    ExfatObjectKind kind;
    char *path;
    ExfatClusterVec clusters;
    uint64_t data_length;
    uint64_t valid_length;
    bool regular_file;
    bool directory;
    bool original_no_fat_chain;
    size_t parent_index;
    uint64_t entry_offset;
    uint32_t entry_count;
    uint64_t system_entry_offset;
    uint32_t target_start;
    uint32_t reserve_clusters;
    uint8_t source_sha256[32];
    bool have_hash;
} ExfatObject;

typedef struct { ExfatObject *items; size_t count, capacity; } ExfatObjectVec;

typedef struct {
    char *path;
    int fd;
    bool writable;
    uint64_t device_size;
    uint64_t volume_bytes;
    uint32_t bytes_per_sector;
    uint32_t sectors_per_cluster;
    uint32_t cluster_size;
    uint32_t fat_offset_sectors;
    uint32_t fat_length_sectors;
    uint32_t heap_offset_sectors;
    uint32_t cluster_count;
    uint32_t root_cluster;
    uint32_t serial;
    uint16_t revision;
    uint16_t volume_flags;
    uint8_t number_of_fats;
    uint8_t percent_in_use;
    uint64_t fat_offset;
    uint64_t fat_length;
    uint64_t heap_offset;
    uint8_t *fat;
    uint8_t *boot_regions;
} ExfatVolume;

typedef struct {
    ExfatObjectVec objects;
    uint8_t *bitmap;
    size_t bitmap_length;
    uint64_t bitmap_entry_offset;
    uint64_t upcase_entry_offset;
    uint64_t upcase_length;
    ExfatClusterVec root_clusters;
    ExfatClusterVec bitmap_clusters;
    ExfatClusterVec upcase_clusters;
    uint64_t regular_files;
    uint64_t directories;
    uint64_t fragmented_files;
    uint64_t fragmented_directories;
    bool growth_10_satisfied;
} ExfatCatalogue;

typedef struct {
    uint8_t *expected_bitmap;
    size_t bitmap_length;
    uint32_t final_cluster;
    bool growth;
} ExfatPlan;

void exfat_set_error(char **error, const char *format, ...);
uint16_t exfat_u16(const void *data, size_t offset);
uint32_t exfat_u32(const void *data, size_t offset);
uint64_t exfat_u64(const void *data, size_t offset);
void exfat_put_u16(void *data, size_t offset, uint16_t value);
void exfat_put_u32(void *data, size_t offset, uint32_t value);
uint16_t exfat_entry_checksum(const uint8_t *data, size_t bytes);
uint32_t exfat_table_checksum(const uint8_t *data, size_t bytes);
uint32_t exfat_boot_checksum(const uint8_t *region, uint32_t bytes_per_sector);

void exfat_clusters_free(ExfatClusterVec *clusters);
int exfat_clusters_push(ExfatClusterVec *clusters, uint32_t cluster);
int exfat_open_volume(const char *path, bool writable, bool allow_dirty, ExfatVolume *volume, char **error);
void exfat_close_volume(ExfatVolume *volume);
uint64_t exfat_cluster_offset(const ExfatVolume *volume, uint32_t cluster);
int exfat_read_cluster(const ExfatVolume *volume, uint32_t cluster, void *buffer, char **error);
int exfat_write_cluster(const ExfatVolume *volume, uint32_t cluster, const void *buffer, char **error);
int exfat_read_stream(const ExfatVolume *volume, const ExfatClusterVec *clusters, uint64_t length, uint8_t **data, char **error);
int exfat_chain(const ExfatVolume *volume, uint32_t first, uint64_t count, bool count_known, bool contiguous, ExfatClusterVec *clusters, char **error);
int exfat_scan(const char *path, bool allow_dirty, ExfatVolume *volume, ExfatCatalogue *catalogue, char **error);
void exfat_catalogue_free(ExfatCatalogue *catalogue);
bool exfat_allocated(const ExfatCatalogue *catalogue, uint32_t cluster);
size_t exfat_fragments(const ExfatClusterVec *clusters);

int exfat_build_plan(ExfatVolume *volume, ExfatCatalogue *catalogue, bool growth, ExfatPlan *plan, char **error);
void exfat_plan_free(ExfatPlan *plan);
int exfat_build_stage(const char *source_path, const char *stage_path, ExfatVolume *source,
                      ExfatCatalogue *catalogue, const ExfatPlan *plan,
                      bool live_updates, char **error);
int exfat_verify_stage(const char *path, ExfatCatalogue *source_catalogue,
                       const ExfatPlan *plan, bool allow_dirty, char **error);
int exfat_set_stage_boot_dirty(const char *stage_path, bool dirty, uint8_t **boot, size_t *length, char **error);

#endif
