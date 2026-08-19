// SPDX-License-Identifier: GPL-3.0-or-later
#include "ntfs_native.h"

#include "infiltratr/core.h"
#include "ld_runtime.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint64_t record;
    bool present;
    bool directory;
    char name[256];
} ObjectState;

typedef struct {
    ObjectState *items;
    size_t count;
    size_t capacity;
} ObjectVec;

static ObjectState *object_get(ObjectVec *vec, uint64_t record) {
    for (size_t index = 0; index < vec->count; ++index)
        if (vec->items[index].record == record) return &vec->items[index];
    if (vec->count == vec->capacity) {
        size_t next = vec->capacity == 0 ? 32U : vec->capacity * 2U;
        vec->items = ld_xrealloc(vec->items, next * sizeof(*vec->items));
        vec->capacity = next;
    }
    ObjectState *item = &vec->items[vec->count++];
    memset(item, 0, sizeof(*item)); item->record = record; return item;
}

static NtfsStream *stream_push(NtfsCatalogue *catalogue) {
    if (catalogue->count == catalogue->capacity) {
        size_t next = catalogue->capacity == 0 ? 64U : catalogue->capacity * 2U;
        catalogue->items = ld_xrealloc(catalogue->items, next * sizeof(*catalogue->items));
        catalogue->capacity = next;
    }
    NtfsStream *stream = &catalogue->items[catalogue->count++];
    memset(stream, 0, sizeof(*stream)); return stream;
}

static int copy_runs(NtfsRunVec *destination, const NtfsRunVec *source) {
    for (size_t index = 0; index < source->count; ++index) {
        NtfsRun run = source->items[index];
        if (ntfs_runs_push(destination, run.lcn, run.length, run.sparse) != 0) return -1;
    }
    return 0;
}

static void best_file_name(const uint8_t *fixed, const NtfsAttributeVec *attrs,
                           uint64_t record, char output[256]) {
    static const char *system_names[] = {
        "$MFT", "$MFTMirr", "$LogFile", "$Volume", "$AttrDef", "$Root",
        "$Bitmap", "$Boot", "$BadClus", "$Secure", "$UpCase", "$Extend"
    };
    output[0] = '\0';
    if (record < sizeof(system_names) / sizeof(system_names[0])) {
        (void)snprintf(output, 256, "%s", system_names[record]); return;
    }
    int best_score = -1;
    for (size_t index = 0; index < attrs->count; ++index) {
        const NtfsAttribute *a = &attrs->items[index];
        if (a->type != NTFS_ATTR_FILE_NAME || a->nonresident || a->length < 24U) continue;
        uint32_t value_len = ntfs_u32(fixed, a->offset + 16U);
        uint16_t value_off = ntfs_u16(fixed, a->offset + 20U);
        if (value_len < 66U || (uint32_t)value_off + value_len > a->length) continue;
        const uint8_t *value = fixed + a->offset + value_off;
        unsigned chars = value[64], ns = value[65];
        if (66U + chars * 2U > value_len) continue;
        int score = (ns == 1U || ns == 3U) ? 3 : ns == 0U ? 2 : 1;
        if (score < best_score) continue;
        size_t written = 0;
        for (unsigned c = 0; c < chars && written + 1U < 256U; ++c) {
            uint16_t ch = ntfs_u16(value + 66U, c * 2U);
            output[written++] = (ch >= 32U && ch < 127U) ? (char)ch : '?';
        }
        output[written] = '\0'; best_score = score;
    }
}

static bool desired_attribute(bool directory, const NtfsAttribute *a) {
    if (!a->nonresident) return false;
    if (directory) return a->type == NTFS_ATTR_INDEX_ALLOCATION;
    return a->type == NTFS_ATTR_DATA && a->name[0] == '\0';
}

static size_t desired_count(bool directory, const NtfsAttributeVec *attrs) {
    size_t count = 0;
    for (size_t index = 0; index < attrs->count; ++index)
        if (desired_attribute(directory, &attrs->items[index])) count++;
    return count;
}

static bool has_attribute_list(const NtfsAttributeVec *attrs) {
    for (size_t index = 0; index < attrs->count; ++index)
        if (attrs->items[index].type == NTFS_ATTR_ATTRIBUTE_LIST) return true;
    return false;
}

static uint32_t mapping_capacity(const uint8_t *fixed, size_t record_size,
                                 const NtfsAttribute *a) {
    (void)fixed;
    (void)record_size;
    if (a->run_offset > a->length) return 0;
    /* The writer does not expand MFT attributes in place.  A stream is only
       movable when its existing mapping-pairs area can hold the final run. */
    return a->length - a->run_offset;
}


static bool all_physical(const NtfsRunVec *runs) {
    for (size_t index = 0; index < runs->count; ++index) if (runs->items[index].sparse) return false;
    return true;
}

static int stream_header_nonzero(NtfsVolume *volume, const NtfsRunVec *runs,
                                 uint64_t data_size, bool *active, char **error) {
    *active = false; if (data_size == 0 || runs->count == 0) return 0;
    uint8_t header[4] = {0}; size_t take = data_size < sizeof(header) ? (size_t)data_size : sizeof(header);
    if (ntfs_read_stream(volume, runs, 0, header, take, error) != 0) return -1;
    for (size_t index = 0; index < take; ++index) if (header[index] != 0) { *active = true; break; }
    return 0;
}

static bool equal_case_ascii(const char *a, const char *b) {
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
        ++a; ++b;
    }
    return *a == '\0' && *b == '\0';
}

static bool stream_is_fragmented(const NtfsStream *stream) {
    return ntfs_fragment_count(&stream->runs) > 1U;
}

static bool object_fragmented(const NtfsCatalogue *catalogue, uint64_t owner, bool directory) {
    size_t groups = 0; bool fragmented = false; uint64_t expected_vcn = 0;
    for (size_t index = 0; index < catalogue->count; ++index) {
        const NtfsStream *s = &catalogue->items[index];
        uint64_t stream_owner = s->base_record != 0 ? s->base_record : s->record_number;
        if (stream_owner != owner) continue;
        if (s->attribute_type != (directory ? NTFS_ATTR_INDEX_ALLOCATION : NTFS_ATTR_DATA)) continue;
        if (!directory && s->attribute_name[0] != '\0') continue;
        groups++;
        if (s->lowest_vcn != expected_vcn || stream_is_fragmented(s)) fragmented = true;
        uint64_t logical = 0; for (size_t r=0;r<s->runs.count;++r) logical += s->runs.items[r].length;
        expected_vcn = s->lowest_vcn + logical;
    }
    return groups > 1U || fragmented;
}

static bool growth_object_ok(const NtfsLayout *layout, const NtfsCatalogue *catalogue,
                             uint64_t owner) {
    const NtfsStream *stream = NULL; size_t count = 0;
    for (size_t index=0; index<catalogue->count; ++index) {
        const NtfsStream *s=&catalogue->items[index]; uint64_t o=s->base_record?s->base_record:s->record_number;
        if(o==owner && s->attribute_type==NTFS_ATTR_DATA && s->attribute_name[0]=='\0') { stream=s; count++; }
    }
    if (count == 0) return true;
    if (count != 1 || stream == NULL || ntfs_fragment_count(&stream->runs) != 1U || stream->runs.items[0].sparse) return false;
    uint64_t start=stream->runs.items[0].lcn, clusters=stream->clusters;
    uint64_t reserve=clusters/10U + (clusters%10U != 0U ? 1U : 0U);
    uint64_t data_end=0U, reserve_end=0U;
    if (!infiltratr_u64_add_checked(start, clusters, &data_end) ||
        data_end > layout->bitmap_bytes*8U ||
        !infiltratr_u64_add_checked(data_end, reserve, &reserve_end))
        return false;
    for(uint64_t c=data_end; c<reserve_end; ++c) if(ntfs_bitmap_bit(layout,c)) return false;
    return true;
}

int ntfs_scan_catalogue(NtfsVolume *volume, NtfsLayout *layout,
                        NtfsCatalogue *catalogue, char **error) {
    memset(catalogue,0,sizeof(*catalogue)); catalogue->growth_10_satisfied=true;
    ObjectVec objects={0};
    uint64_t record_count=layout->mft_data_size/volume->record_size;
    if(record_count>UINT32_MAX)record_count=UINT32_MAX;
    for(uint64_t number=0; number<record_count; ++number) {
        catalogue->records_scanned++;
        uint8_t *raw=ld_xmalloc(volume->record_size),*fixed=ld_xmalloc(volume->record_size);
        if (number > UINT64_MAX / volume->record_size ||
            ntfs_read_stream(volume, &layout->mft_runs, number * volume->record_size,
                             raw, volume->record_size, error) != 0) {
            free(raw); free(fixed); free(*error); *error=NULL; catalogue->malformed_records++; continue;
        }
        if (memcmp(raw, "FILE", 4) != 0) { free(raw); free(fixed); continue; }
        if (ntfs_apply_fixups(raw, volume->record_size, volume->bytes_per_sector, fixed, error) != 0) {
            free(raw); free(fixed); free(*error); *error=NULL; catalogue->malformed_records++; continue;
        }
        if((ntfs_u16(fixed,22)&NTFS_RECORD_IN_USE)==0U) { free(raw);free(fixed);continue; }
        NtfsAttributeVec attrs={0};
        if(ntfs_parse_attributes(fixed,volume->record_size,&attrs,error)!=0) {
            free(*error); *error=NULL; catalogue->malformed_records++; free(raw);free(fixed); continue;
        }
        uint64_t base=ntfs_u64(fixed,32)&NTFS_FILE_REFERENCE_MASK;
        uint64_t owner=base?base:number; bool directory=(ntfs_u16(fixed,22)&NTFS_RECORD_DIRECTORY)!=0U;
        ObjectState *object=object_get(&objects,owner);
        char file_name[256]; best_file_name(fixed,&attrs,number,file_name);
        if(base==0){object->present=true;object->directory=directory;if(file_name[0])snprintf(object->name,sizeof(object->name),"%s",file_name);}
        bool attr_list=has_attribute_list(&attrs); size_t desired=desired_count(directory,&attrs);
        for(size_t ai=0;ai<attrs.count;++ai) {
            NtfsAttribute *a=&attrs.items[ai];
            if(!a->nonresident || (a->type!=NTFS_ATTR_DATA && a->type!=NTFS_ATTR_INDEX_ALLOCATION)) continue;
            NtfsStream *s=stream_push(catalogue);s->record_number=number;s->base_record=base;s->attribute_offset=a->offset;s->attribute_type=a->type;s->attribute_flags=a->flags;s->directory=directory;s->lowest_vcn=a->lowest_vcn;s->clusters=ntfs_run_clusters(&a->runs);s->data_size=a->data_size;s->mapping_capacity=mapping_capacity(fixed,volume->record_size,a);snprintf(s->file_name,sizeof(s->file_name),"%s",file_name);snprintf(s->attribute_name,sizeof(s->attribute_name),"%s",a->name);
            if(copy_runs(&s->runs,&a->runs)!=0){ntfs_set_error(error,"NTFS runlist allocation overflow");ntfs_attributes_free(&attrs);free(raw);free(fixed);free(objects.items);ntfs_catalogue_free(catalogue);return -1;}
            s->movable = number>=NTFS_FIRST_USER_RECORD && base==0 && !attr_list && desired==1U && desired_attribute(directory,a) && a->lowest_vcn==0 && (a->flags&(NTFS_ATTR_COMPRESSED|NTFS_ATTR_ENCRYPTED|NTFS_ATTR_SPARSE))==0U && all_physical(&a->runs) && a->data_size!=0 && s->mapping_capacity>=4U;
            if(!catalogue->hibernation_active && equal_case_ascii(file_name,"hiberfil.sys") && a->type==NTFS_ATTR_DATA && a->name[0]=='\0') {
                bool active=false;if(stream_header_nonzero(volume,&a->runs,a->data_size,&active,error)!=0){ntfs_attributes_free(&attrs);free(raw);free(fixed);free(objects.items);ntfs_catalogue_free(catalogue);return -1;}catalogue->hibernation_active=active;
            }
        }
        ntfs_attributes_free(&attrs);free(raw);free(fixed);
    }
    for(size_t index=0;index<objects.count;++index){ObjectState *o=&objects.items[index];if(!o->present)continue;bool fragmented=object_fragmented(catalogue,o->record,o->directory);if(o->directory){catalogue->directories++;if(fragmented)catalogue->fragmented_directories++;}else{catalogue->regular_files++;if(fragmented)catalogue->fragmented_files++;if(o->record>=NTFS_FIRST_USER_RECORD&&!growth_object_ok(layout,catalogue,o->record))catalogue->growth_10_satisfied=false;}}
    if(catalogue->regular_files==0 || catalogue->fragmented_directories!=0) catalogue->growth_10_satisfied=false;
    free(objects.items); return 0;
}

void ntfs_catalogue_free(NtfsCatalogue *catalogue) {
    if (catalogue == NULL) return;
    for (size_t index = 0; index < catalogue->count; ++index)
        ntfs_runs_free(&catalogue->items[index].runs);
    free(catalogue->items);
    memset(catalogue, 0, sizeof(*catalogue));
}
