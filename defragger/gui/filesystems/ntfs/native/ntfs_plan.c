// SPDX-License-Identifier: GPL-3.0-or-later
#include "ntfs_native.h"

#include "ld_io.h"
#include "ld_runtime.h"
#include "ld_stop.h"

#include <errno.h>
#include <fcntl.h>
#include <openssl/evp.h>
#include <openssl/sha.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/file.h>
#include <unistd.h>

typedef struct {
    NtfsStream *stream;
    uint64_t span;
} PlanItem;

typedef struct { uint64_t start, length; } FreeRun;
typedef struct { FreeRun *items; size_t count, capacity; } FreeVec;

static int sql_error(sqlite3 *db, char **error, const char *action) {
    ntfs_set_error(error, "%s: %s", action, sqlite3_errmsg(db)); return -1;
}
static int sql_exec(sqlite3 *db, const char *sql, char **error) {
    char *message=NULL; int code=sqlite3_exec(db,sql,NULL,NULL,&message);
    if(code!=SQLITE_OK){ntfs_set_error(error,"NTFS plan database: %s",message?message:sqlite3_errmsg(db));sqlite3_free(message);return -1;}return 0;
}
static void placement_push(NtfsPlacementVec *vec,NtfsPlacement item){if(vec->count==vec->capacity){size_t n=vec->capacity?vec->capacity*2U:32U;vec->items=ld_xrealloc(vec->items,n*sizeof(*vec->items));vec->capacity=n;}vec->items[vec->count++]=item;}
void ntfs_placements_free(NtfsPlacementVec *p){if(!p)return;free(p->items);memset(p,0,sizeof(*p));}
static void free_push(FreeVec *vec,uint64_t start,uint64_t length){if(!length)return;if(vec->count==vec->capacity){size_t n=vec->capacity?vec->capacity*2U:32U;vec->items=ld_xrealloc(vec->items,n*sizeof(*vec->items));vec->capacity=n;}vec->items[vec->count++]=(FreeRun){start,length};}
static int cmp_plan_item(const void *left,const void *right){const PlanItem *a=left,*b=right;if(a->stream->directory!=b->stream->directory)return a->stream->directory?-1:1;int name=strcasecmp(a->stream->file_name,b->stream->file_name);if(name)return name;if(a->stream->record_number<b->stream->record_number)return -1;if(a->stream->record_number>b->stream->record_number)return 1;if(a->stream->attribute_offset<b->stream->attribute_offset)return -1;if(a->stream->attribute_offset>b->stream->attribute_offset)return 1;return 0;}
static uint64_t stream_span(const NtfsStream *s,bool growth){uint64_t reserve=growth&&s->attribute_type==NTFS_ATTR_DATA?(s->clusters*10U+99U)/100U:0U;return s->clusters+reserve;}
static void set_stream_free(NtfsLayout *layout,const NtfsStream *s){for(size_t r=0;r<s->runs.count;++r){NtfsRun run=s->runs.items[r];if(run.sparse)continue;for(uint64_t c=0;c<run.length;++c)ntfs_bitmap_set(layout,run.lcn+c,false);}}
static void collect_free(const NtfsLayout *layout,uint64_t total,FreeVec *free_runs){uint64_t upper=total>0?total-1U:0U;bool active=false;uint64_t start=0;for(uint64_t c=1;c<upper;++c){bool free=!ntfs_bitmap_bit(layout,c);if(free&&!active){active=true;start=c;}else if(!free&&active){free_push(free_runs,start,c-start);active=false;}}if(active)free_push(free_runs,start,upper-start);}

static int select_best_fit(PlanItem *items,size_t count,uint64_t target,bool *selected,
                           uint64_t *filled,char **error){
    *filled = 0;
    if (target == 0) return 0;
    if (target > SIZE_MAX / sizeof(int32_t) - 1U) {ntfs_set_error(error,"NTFS low-layout target exceeds addressable memory");return -1;}
    uint64_t bytes=(target+1U)*sizeof(int32_t);if(bytes>NTFS_SUBSET_MEMORY_LIMIT){ntfs_set_error(error,"NTFS low-layout planning would exceed the fixed 256 MiB memory safety limit");return -1;}
    int32_t *choice=ld_xmalloc((size_t)bytes);for(uint64_t s=0;s<=target;++s)choice[s]=-1;choice[0]=-2;
    for(size_t i=0;i<count;++i){uint64_t span=items[i].span;if(span==0||span>target)continue;for(uint64_t sum=target;;--sum){if(sum>=span&&choice[sum]<0&&choice[sum-span]!=-1)choice[sum]=(int32_t)i;if(sum==span)break;}}
    uint64_t best=target;while(best>0&&choice[best]==-1)best--;
    uint64_t remaining=best;while(remaining){int32_t index=choice[remaining];if(index<0||(size_t)index>=count||items[index].span>remaining){free(choice);ntfs_set_error(error,"NTFS low-layout subset recovery failed");return -1;}selected[index]=true;remaining-=items[index].span;}
    *filled=best;free(choice);return 0;
}

static uint64_t stream_owner(const NtfsStream *stream) {
    return stream->base_record != 0 ? stream->base_record : stream->record_number;
}

static bool primary_object_stream(const NtfsStream *stream) {
    if (stream->directory) return stream->attribute_type == NTFS_ATTR_INDEX_ALLOCATION;
    return stream->attribute_type == NTFS_ATTR_DATA && stream->attribute_name[0] == '\0';
}

static size_t logical_stream_parts(const NtfsCatalogue *catalogue, const NtfsStream *stream) {
    size_t count = 0;
    uint64_t owner = stream_owner(stream);
    for (size_t i = 0; i < catalogue->count; ++i) {
        const NtfsStream *candidate = &catalogue->items[i];
        if (stream_owner(candidate) != owner || candidate->attribute_type != stream->attribute_type) continue;
        if (strcmp(candidate->attribute_name, stream->attribute_name) != 0) continue;
        count++;
    }
    return count;
}

static bool preserved_primary_is_contiguous(const NtfsCatalogue *catalogue, const NtfsStream *stream) {
    if (!primary_object_stream(stream) || stream->lowest_vcn != 0) return false;
    if (logical_stream_parts(catalogue, stream) != 1U) return false;
    return ntfs_fragment_count(&stream->runs) <= 1U;
}

static int reserve_fixed_growth_stream(NtfsLayout *layout, const NtfsStream *stream,
                                       FreeVec *reserved, char **error) {
    if (stream->directory || stream->attribute_type != NTFS_ATTR_DATA ||
        stream->attribute_name[0] != '\0') return 0;
    if ((stream->attribute_flags & (NTFS_ATTR_COMPRESSED | NTFS_ATTR_ENCRYPTED | NTFS_ATTR_SPARSE)) != 0U ||
        stream->runs.count != 1U || stream->runs.items[0].sparse) {
        ntfs_set_error(error,
                       "NTFS primary stream in MFT record %llu cannot safely receive a 10%% reserve while preserved in place",
                       (unsigned long long)stream->record_number);
        return -1;
    }
    uint64_t end = stream->runs.items[0].lcn + stream->runs.items[0].length;
    uint64_t reserve = (stream->clusters * 10U + 99U) / 100U;
    if (reserve == 0) return 0;
    if (end > layout->bitmap_bytes * 8U || reserve > layout->bitmap_bytes * 8U - end) {
        ntfs_set_error(error,
                       "NTFS preserved stream in MFT record %llu has no room for its 10%% growth reserve",
                       (unsigned long long)stream->record_number);
        return -1;
    }
    for (uint64_t c = end; c < end + reserve; ++c) {
        if (ntfs_bitmap_bit(layout, c)) {
            ntfs_set_error(error,
                           "NTFS preserved stream in MFT record %llu cannot obtain its 10%% growth reserve because cluster %llu is fixed allocated",
                           (unsigned long long)stream->record_number, (unsigned long long)c);
            return -1;
        }
    }
    free_push(reserved, end, reserve);
    for (uint64_t c = end; c < end + reserve; ++c) ntfs_bitmap_set(layout, c, true);
    return 0;
}

int ntfs_plan_layout(NtfsLayout *layout,NtfsCatalogue *catalogue,uint64_t total_clusters,bool growth,NtfsPlacementVec *placements,char **error){
    memset(placements,0,sizeof(*placements));
    if(catalogue->hibernation_active){ntfs_set_error(error,"NTFS hibernation image is active; resume and shut down Windows fully before raw mutation");return -1;}
    size_t movable=0;
    for(size_t i=0;i<catalogue->count;++i){
        NtfsStream *s=&catalogue->items[i];
        if(s->movable&&s->clusters){movable++;continue;}
        if(s->record_number<NTFS_FIRST_USER_RECORD||s->clusters==0||
           (s->attribute_type!=NTFS_ATTR_DATA&&s->attribute_type!=NTFS_ATTR_INDEX_ALLOCATION))continue;
        /* Named ADS and other non-primary user streams are deliberately left
           byte-for-byte in place. They are not part of the canonical primary
           file/directory layout and must not veto the whole volume. */
        if(!primary_object_stream(s)){placements->fixed_streams++;continue;}
        if(!preserved_primary_is_contiguous(catalogue,s)){
            ntfs_set_error(error,
                           "NTFS primary stream in MFT record %llu is split or fragmented in an unsupported layout; it cannot be safely preserved in place",
                           (unsigned long long)s->record_number);
            return -1;
        }
        placements->fixed_streams++;
    }
    if(movable==0){ntfs_set_error(error,"NTFS has no supported movable user streams");return -1;}
    PlanItem *all=ld_xmalloc(movable*sizeof(*all));size_t w=0;
    for(size_t i=0;i<catalogue->count;++i)if(catalogue->items[i].movable&&catalogue->items[i].clusters){all[w].stream=&catalogue->items[i];all[w].span=stream_span(all[w].stream,growth);set_stream_free(layout,all[w].stream);w++;}
    qsort(all,movable,sizeof(*all),cmp_plan_item);
    FreeVec reserved={0};
    if(growth){
        for(size_t i=0;i<catalogue->count;++i){
            NtfsStream *s=&catalogue->items[i];
            if(s->movable||s->record_number<NTFS_FIRST_USER_RECORD||s->clusters==0||!primary_object_stream(s))continue;
            if(reserve_fixed_growth_stream(layout,s,&reserved,error)!=0)goto fail_before_free;
        }
    }
    FreeVec free_runs={0};
    collect_free(layout,total_clusters,&free_runs);
    PlanItem *remaining=ld_xmalloc(movable*sizeof(*remaining));memcpy(remaining,all,movable*sizeof(*remaining));size_t remaining_count=movable;uint64_t total_span=0;for(size_t i=0;i<remaining_count;++i)total_span+=remaining[i].span;
    bool have_start=false;
    for(size_t fr=0;fr<free_runs.count&&remaining_count; ++fr){uint64_t run_start=free_runs.items[fr].start,capacity=free_runs.items[fr].length;if(capacity==0)continue;if(!have_start){placements->envelope_start=run_start;have_start=true;}
        if(total_span<=capacity){uint64_t cursor=run_start;for(size_t i=0;i<remaining_count;++i){NtfsStream *s=remaining[i].stream;uint64_t reserve=remaining[i].span-s->clusters;placement_push(placements,(NtfsPlacement){s->record_number,s->attribute_offset,cursor,s->clusters,reserve});cursor+=remaining[i].span;}placements->envelope_end=cursor;remaining_count=0;total_span=0;break;}
        bool *selected=calloc(remaining_count,sizeof(*selected));if(!selected){ntfs_set_error(error,"allocating NTFS subset planner failed");goto fail;}uint64_t filled=0;if(select_best_fit(remaining,remaining_count,capacity,selected,&filled,error)<0){free(selected);goto fail;}
        uint64_t cursor=run_start;for(size_t i=0;i<remaining_count;++i)if(selected[i]){NtfsStream *s=remaining[i].stream;uint64_t reserve=remaining[i].span-s->clusters;placement_push(placements,(NtfsPlacement){s->record_number,s->attribute_offset,cursor,s->clusters,reserve});cursor+=remaining[i].span;}if(cursor!=run_start+filled){free(selected);ntfs_set_error(error,"NTFS low-layout planner produced an inconsistent best-fit run");goto fail;}placements->envelope_end=cursor;placements->fixed_slack_clusters+=capacity-filled;
        size_t out=0;for(size_t i=0;i<remaining_count;++i)if(!selected[i])remaining[out++]=remaining[i];free(selected);remaining_count=out;total_span-=filled;
    }
    if(remaining_count){ntfs_set_error(error,"NTFS has insufficient legal free clusters for the canonical layout");goto fail;}
    for(size_t i=0;i<placements->count;++i){NtfsPlacement *p=&placements->items[i];for(uint64_t c=0;c<p->clusters;++c)ntfs_bitmap_set(layout,p->start+c,true);for(uint64_t c=0;c<p->reserve;++c)ntfs_bitmap_set(layout,p->start+p->clusters+c,false);}
    for(size_t i=0;i<reserved.count;++i)for(uint64_t c=0;c<reserved.items[i].length;++c)ntfs_bitmap_set(layout,reserved.items[i].start+c,false);
    free(all);free(remaining);free(free_runs.items);free(reserved.items);return 0;
fail:
    for(size_t i=0;i<reserved.count;++i)for(uint64_t c=0;c<reserved.items[i].length;++c)ntfs_bitmap_set(layout,reserved.items[i].start+c,false);
    free(all);free(remaining);free(free_runs.items);free(reserved.items);ntfs_placements_free(placements);return -1;
fail_before_free:
    for(size_t i=0;i<reserved.count;++i)for(uint64_t c=0;c<reserved.items[i].length;++c)ntfs_bitmap_set(layout,reserved.items[i].start+c,false);
    free(all);free(reserved.items);ntfs_placements_free(placements);return -1;
}

static NtfsPlacement *find_placement(const NtfsPlacementVec *placements,uint64_t record,uint32_t offset){for(size_t i=0;i<placements->count;++i)if(placements->items[i].record_number==record&&placements->items[i].attribute_offset==offset)return &placements->items[i];return NULL;}
static int stream_digest(NtfsVolume *volume, const NtfsStream *stream,
                         uint8_t digest[SHA256_DIGEST_LENGTH], char **error) {
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (ctx == NULL || EVP_DigestInit_ex(ctx, EVP_sha256(), NULL) != 1) {
        EVP_MD_CTX_free(ctx);
        ntfs_set_error(error, "initializing NTFS payload digest failed");
        return -1;
    }
    uint8_t *buffer = ld_xmalloc(volume->cluster_size);
    for (size_t r = 0; r < stream->runs.count; ++r) {
        NtfsRun run = stream->runs.items[r];
        if (run.sparse) {
            free(buffer);
            EVP_MD_CTX_free(ctx);
            ntfs_set_error(error, "sparse NTFS stream entered native writer");
            return -1;
        }
        for (uint64_t c = 0; c < run.length; ++c) {
            ssize_t got = ld_pread_full(volume->fd, buffer, volume->cluster_size,
                                        (run.lcn + c) * volume->cluster_size);
            if (got < 0 || (size_t)got != volume->cluster_size ||
                EVP_DigestUpdate(ctx, buffer, volume->cluster_size) != 1) {
                free(buffer);
                EVP_MD_CTX_free(ctx);
                ntfs_set_error(error, "reading NTFS payload for verification failed");
                return -1;
            }
        }
    }
    free(buffer);
    unsigned int digest_length = 0;
    if (EVP_DigestFinal_ex(ctx, digest, &digest_length) != 1 ||
        digest_length != SHA256_DIGEST_LENGTH) {
        EVP_MD_CTX_free(ctx);
        ntfs_set_error(error, "finalizing NTFS payload digest failed");
        return -1;
    }
    EVP_MD_CTX_free(ctx);
    return 0;
}

int ntfs_create_plan_db(const char *path, NtfsVolume *volume, NtfsLayout *layout,
                        NtfsCatalogue *catalogue, const NtfsPlacementVec *placements,
                        bool growth, sqlite3 **db, char **error) {
    (void)unlink(path);
    if (sqlite3_open_v2(path, db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL) != SQLITE_OK)
        return sql_error(*db, error, "opening NTFS plan database");
    if (sql_exec(*db,
                 "PRAGMA journal_mode=WAL;PRAGMA synchronous=FULL;"
                 "CREATE TABLE streams(record INTEGER,attr INTEGER,type INTEGER,clusters INTEGER,target INTEGER,reserve INTEGER,sha BLOB,PRIMARY KEY(record,attr));"
                 "CREATE TABLE fixed_primary(record INTEGER,attr INTEGER,clusters INTEGER,start INTEGER,reserve INTEGER,sha BLOB,PRIMARY KEY(record,attr));"
                 "CREATE TABLE blocks(old INTEGER PRIMARY KEY,target INTEGER UNIQUE,placed INTEGER DEFAULT 0);"
                 "CREATE TABLE metadata(key TEXT PRIMARY KEY,value BLOB);BEGIN IMMEDIATE",
                 error) != 0) return -1;

    sqlite3_stmt *ins_stream = NULL, *ins_fixed = NULL, *ins_block = NULL, *ins_meta = NULL;
    if (sqlite3_prepare_v2(*db, "INSERT INTO streams VALUES (?,?,?,?,?,?,?)", -1, &ins_stream, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(*db, "INSERT INTO fixed_primary VALUES (?,?,?,?,?,?)", -1, &ins_fixed, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(*db, "INSERT INTO blocks(old,target) VALUES (?,?)", -1, &ins_block, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(*db, "INSERT INTO metadata VALUES (?,?)", -1, &ins_meta, NULL) != SQLITE_OK) {
        sql_error(*db, error, "preparing NTFS plan database");
        goto fail;
    }

    for (size_t i = 0; i < catalogue->count; ++i) {
        NtfsStream *stream = &catalogue->items[i];
        if (!stream->movable || !stream->clusters) continue;
        NtfsPlacement *placement = find_placement(placements, stream->record_number,
                                                  stream->attribute_offset);
        if (placement == NULL) {
            ntfs_set_error(error, "NTFS planner omitted a movable stream");
            goto fail;
        }
        uint8_t digest[SHA256_DIGEST_LENGTH];
        if (stream_digest(volume, stream, digest, error) != 0) goto fail;
        sqlite3_reset(ins_stream);
        sqlite3_bind_int64(ins_stream, 1, (sqlite3_int64)stream->record_number);
        sqlite3_bind_int(ins_stream, 2, (int)stream->attribute_offset);
        sqlite3_bind_int(ins_stream, 3, (int)stream->attribute_type);
        sqlite3_bind_int64(ins_stream, 4, (sqlite3_int64)stream->clusters);
        sqlite3_bind_int64(ins_stream, 5, (sqlite3_int64)placement->start);
        sqlite3_bind_int64(ins_stream, 6, (sqlite3_int64)placement->reserve);
        sqlite3_bind_blob(ins_stream, 7, digest, SHA256_DIGEST_LENGTH, SQLITE_TRANSIENT);
        if (sqlite3_step(ins_stream) != SQLITE_DONE) {
            sql_error(*db, error, "recording NTFS stream plan");
            goto fail;
        }
        uint64_t target = placement->start;
        for (size_t r = 0; r < stream->runs.count; ++r) {
            NtfsRun run = stream->runs.items[r];
            for (uint64_t c = 0; c < run.length; ++c) {
                sqlite3_reset(ins_block);
                sqlite3_bind_int64(ins_block, 1, (sqlite3_int64)(run.lcn + c));
                sqlite3_bind_int64(ins_block, 2, (sqlite3_int64)target++);
                if (sqlite3_step(ins_block) != SQLITE_DONE) {
                    sql_error(*db, error, "recording NTFS cluster permutation");
                    goto fail;
                }
            }
        }
        if (target != placement->start + placement->clusters) {
            ntfs_set_error(error, "NTFS stream cluster count changed during planning");
            goto fail;
        }
    }

    /* Preserve the exact contract for every unsupported-but-safe primary user
       file stream.  These rows are never rewritten; they exist so verification
       proves that their mapping and payload stayed byte-for-byte fixed and, for
       Growth Defrag, that the planner's exact 10% post-file reserve is free. */
    for (size_t i = 0; i < catalogue->count; ++i) {
        NtfsStream *stream = &catalogue->items[i];
        if (stream->movable || stream->record_number < NTFS_FIRST_USER_RECORD ||
            stream->clusters == 0 || stream->directory || !primary_object_stream(stream)) continue;
        if (stream->runs.count != 1U || stream->runs.items[0].sparse) {
            ntfs_set_error(error,
                           "NTFS fixed primary stream in MFT record %llu changed shape before plan persistence",
                           (unsigned long long)stream->record_number);
            goto fail;
        }
        uint64_t reserve = growth ? (stream->clusters * 10U + 99U) / 100U : 0U;
        uint64_t start_cluster = stream->runs.items[0].lcn;
        uint8_t digest[SHA256_DIGEST_LENGTH];
        if (stream_digest(volume, stream, digest, error) != 0) goto fail;
        sqlite3_reset(ins_fixed);
        sqlite3_bind_int64(ins_fixed, 1, (sqlite3_int64)stream->record_number);
        sqlite3_bind_int(ins_fixed, 2, (int)stream->attribute_offset);
        sqlite3_bind_int64(ins_fixed, 3, (sqlite3_int64)stream->clusters);
        sqlite3_bind_int64(ins_fixed, 4, (sqlite3_int64)start_cluster);
        sqlite3_bind_int64(ins_fixed, 5, (sqlite3_int64)reserve);
        sqlite3_bind_blob(ins_fixed, 6, digest, SHA256_DIGEST_LENGTH, SQLITE_TRANSIENT);
        if (sqlite3_step(ins_fixed) != SQLITE_DONE) {
            sql_error(*db, error, "recording NTFS fixed primary stream");
            goto fail;
        }
    }

    sqlite3_reset(ins_meta);
    sqlite3_bind_text(ins_meta, 1, "bitmap", -1, SQLITE_STATIC);
    sqlite3_bind_blob(ins_meta, 2, layout->bitmap, (int)layout->bitmap_bytes, SQLITE_TRANSIENT);
    if (sqlite3_step(ins_meta) != SQLITE_DONE) {
        sql_error(*db, error, "recording NTFS final bitmap");
        goto fail;
    }
    if (sql_exec(*db, "COMMIT", error) != 0) goto fail;
    sqlite3_finalize(ins_stream);
    sqlite3_finalize(ins_fixed);
    sqlite3_finalize(ins_block);
    sqlite3_finalize(ins_meta);
    return 0;

fail:
    (void)sqlite3_exec(*db, "ROLLBACK", NULL, NULL, NULL);
    sqlite3_finalize(ins_stream);
    sqlite3_finalize(ins_fixed);
    sqlite3_finalize(ins_block);
    sqlite3_finalize(ins_meta);
    return -1;
}
int ntfs_open_plan_db(const char *path,sqlite3 **db,char **error){if(sqlite3_open_v2(path,db,SQLITE_OPEN_READWRITE,NULL)!=SQLITE_OK)return sql_error(*db,error,"opening NTFS plan database");return 0;}
uint64_t ntfs_plan_move_count(sqlite3 *db,char **error){sqlite3_stmt *s=NULL;if(sqlite3_prepare_v2(db,"SELECT COUNT(*) FROM blocks WHERE old<>target",-1,&s,NULL)!=SQLITE_OK){sql_error(db,error,"reading NTFS move count");return UINT64_MAX;}uint64_t n=sqlite3_step(s)==SQLITE_ROW?(uint64_t)sqlite3_column_int64(s,0):UINT64_MAX;sqlite3_finalize(s);return n;}

static int copy_cluster(int fd,uint32_t size,uint64_t source,uint64_t target,char **error){uint8_t *buffer=ld_xmalloc(size);ssize_t got=ld_pread_full(fd,buffer,size,source*size);if(got<0||(size_t)got!=size){free(buffer);ntfs_set_error(error,"short NTFS source-cluster read");return -1;}ssize_t wrote=ld_pwrite_full(fd,buffer,size,target*size);free(buffer);if(wrote<0||(size_t)wrote!=size){ntfs_set_error(error,"short NTFS target-cluster write");return -1;}return 0;}
int ntfs_permute_stage(const char *stage,sqlite3 *db,uint32_t cluster_size,uint64_t move_count,char **error){int fd=open(stage,O_RDWR|O_CLOEXEC);if(fd<0){ntfs_set_error(error,"cannot open NTFS stage for relocation: %s",strerror(errno));return -1;}sqlite3_stmt *term=NULL,*pred=NULL,*mark=NULL,*unplaced=NULL;if(sqlite3_prepare_v2(db,"SELECT b.target FROM blocks b LEFT JOIN blocks s ON s.old=b.target WHERE b.old<>b.target AND s.old IS NULL ORDER BY b.target",-1,&term,NULL)!=SQLITE_OK||sqlite3_prepare_v2(db,"SELECT old FROM blocks WHERE target=? AND old<>target AND placed=0",-1,&pred,NULL)!=SQLITE_OK||sqlite3_prepare_v2(db,"UPDATE blocks SET placed=1 WHERE old=?",-1,&mark,NULL)!=SQLITE_OK||sqlite3_prepare_v2(db,"SELECT old FROM blocks WHERE old<>target AND placed=0 LIMIT 1",-1,&unplaced,NULL)!=SQLITE_OK){close(fd);return sql_error(db,error,"preparing NTFS cluster permutation");}uint64_t placed=0;int state;while((state=sqlite3_step(term))==SQLITE_ROW){uint64_t free_cluster=(uint64_t)sqlite3_column_int64(term,0);while(1){sqlite3_reset(pred);sqlite3_clear_bindings(pred);sqlite3_bind_int64(pred,1,(sqlite3_int64)free_cluster);int ps=sqlite3_step(pred);if(ps==SQLITE_DONE)break;if(ps!=SQLITE_ROW){sql_error(db,error,"reading NTFS cluster predecessor");goto fail;}uint64_t old=(uint64_t)sqlite3_column_int64(pred,0);if(copy_cluster(fd,cluster_size,old,free_cluster,error)!=0)goto fail;sqlite3_reset(mark);sqlite3_clear_bindings(mark);sqlite3_bind_int64(mark,1,(sqlite3_int64)old);if(sqlite3_step(mark)!=SQLITE_DONE){sql_error(db,error,"marking NTFS cluster placement");goto fail;}placed++;free_cluster=old;if((placed%8192U)==0U&&ld_stop_requested()){ntfs_set_error(error,"stop requested before NTFS source commit");goto fail;}}}if(state!=SQLITE_DONE){sql_error(db,error,"reading NTFS terminal clusters");goto fail;}while(1){sqlite3_reset(unplaced);int us=sqlite3_step(unplaced);if(us==SQLITE_DONE)break;if(us!=SQLITE_ROW){sql_error(db,error,"reading NTFS relocation cycle");goto fail;}uint64_t start=(uint64_t)sqlite3_column_int64(unplaced,0),free_cluster=start;uint8_t *saved=ld_xmalloc(cluster_size);ssize_t got=ld_pread_full(fd,saved,cluster_size,start*cluster_size);if(got<0||(size_t)got!=cluster_size){free(saved);ntfs_set_error(error,"short NTFS cycle read");goto fail;}while(1){sqlite3_reset(pred);sqlite3_clear_bindings(pred);sqlite3_bind_int64(pred,1,(sqlite3_int64)free_cluster);int ps=sqlite3_step(pred);if(ps!=SQLITE_ROW){free(saved);ntfs_set_error(error,"broken NTFS relocation cycle");goto fail;}uint64_t old=(uint64_t)sqlite3_column_int64(pred,0);if(old==start){ssize_t wrote=ld_pwrite_full(fd,saved,cluster_size,free_cluster*cluster_size);free(saved);if(wrote<0||(size_t)wrote!=cluster_size){ntfs_set_error(error,"short NTFS cycle close write");goto fail;}sqlite3_reset(mark);sqlite3_clear_bindings(mark);sqlite3_bind_int64(mark,1,(sqlite3_int64)start);if(sqlite3_step(mark)!=SQLITE_DONE){sql_error(db,error,"marking NTFS cycle completion");goto fail;}placed++;break;}if(copy_cluster(fd,cluster_size,old,free_cluster,error)!=0){free(saved);goto fail;}sqlite3_reset(mark);sqlite3_clear_bindings(mark);sqlite3_bind_int64(mark,1,(sqlite3_int64)old);if(sqlite3_step(mark)!=SQLITE_DONE){free(saved);sql_error(db,error,"marking NTFS cycle placement");goto fail;}placed++;free_cluster=old;}}
    if(placed!=move_count||fsync(fd)!=0){ntfs_set_error(error,"NTFS cluster permutation did not complete durably");goto fail;}sqlite3_finalize(term);sqlite3_finalize(pred);sqlite3_finalize(mark);sqlite3_finalize(unplaced);close(fd);return 0;
fail:sqlite3_finalize(term);sqlite3_finalize(pred);sqlite3_finalize(mark);sqlite3_finalize(unplaced);close(fd);return -1;}

int ntfs_apply_stage_metadata(const char *stage,sqlite3 *db,bool allow_dirty,char **error){NtfsVolume volume;NtfsLayout layout;if(ntfs_open_volume(stage,true,&volume,error)!=0)return -1;if(ntfs_read_layout(&volume,allow_dirty,&layout,error)!=0){ntfs_close_volume(&volume);return -1;}sqlite3_stmt *streams=NULL;if(sqlite3_prepare_v2(db,"SELECT record,attr,clusters,target FROM streams ORDER BY record,attr",-1,&streams,NULL)!=SQLITE_OK){sql_error(db,error,"reading NTFS stream metadata plan");goto fail;}int state;while((state=sqlite3_step(streams))==SQLITE_ROW){uint64_t record=(uint64_t)sqlite3_column_int64(streams,0);uint32_t attr_off=(uint32_t)sqlite3_column_int(streams,1);uint64_t clusters=(uint64_t)sqlite3_column_int64(streams,2),target=(uint64_t)sqlite3_column_int64(streams,3);uint8_t *raw=NULL,*fixed=NULL;if(ntfs_read_record(&volume,&layout.mft_runs,record,&raw,&fixed,error)!=0)goto fail_stream;NtfsAttributeVec attrs={0};if(ntfs_parse_attributes(fixed,volume.record_size,&attrs,error)!=0){free(raw);free(fixed);goto fail_stream;}NtfsAttribute *wanted=NULL;for(size_t i=0;i<attrs.count;++i)if(attrs.items[i].offset==attr_off){wanted=&attrs.items[i];break;}if(!wanted||!wanted->nonresident||wanted->run_offset>=wanted->length){ntfs_set_error(error,"NTFS planned attribute moved or changed before metadata commit");ntfs_attributes_free(&attrs);free(raw);free(fixed);goto fail_stream;}size_t capacity=wanted->length-wanted->run_offset;uint8_t *mapping=fixed+wanted->offset+wanted->run_offset;memset(mapping,0,capacity);size_t used=0;if(ntfs_encode_single_run(target,clusters,mapping,capacity,&used,error)!=0){ntfs_attributes_free(&attrs);free(raw);free(fixed);goto fail_stream;}(void)used;ntfs_put_u64(fixed,wanted->offset+24U,wanted->lowest_vcn+clusters-1U);ntfs_put_u64(fixed,wanted->offset+40U,clusters*volume.cluster_size);if(ntfs_prepare_fixups(fixed,volume.record_size,volume.bytes_per_sector,raw,error)!=0||ntfs_write_record(&volume,&layout.mft_runs,record,raw,error)!=0){ntfs_attributes_free(&attrs);free(raw);free(fixed);goto fail_stream;}ntfs_attributes_free(&attrs);free(raw);free(fixed);continue;fail_stream:sqlite3_finalize(streams);goto fail;}
    if(state!=SQLITE_DONE){sql_error(db,error,"reading NTFS stream metadata plan");sqlite3_finalize(streams);goto fail;}sqlite3_finalize(streams);
    sqlite3_stmt *meta=NULL;if(sqlite3_prepare_v2(db,"SELECT value FROM metadata WHERE key='bitmap'",-1,&meta,NULL)!=SQLITE_OK||sqlite3_step(meta)!=SQLITE_ROW){sqlite3_finalize(meta);sql_error(db,error,"reading NTFS final bitmap");goto fail;}const void *blob=sqlite3_column_blob(meta,0);int bytes=sqlite3_column_bytes(meta,0);if(blob==NULL||bytes<0||(size_t)bytes!=layout.bitmap_bytes){sqlite3_finalize(meta);ntfs_set_error(error,"NTFS final bitmap has the wrong size");goto fail;}memcpy(layout.bitmap,blob,layout.bitmap_bytes);sqlite3_finalize(meta);if(ntfs_write_bitmap(&volume,&layout,error)!=0||fsync(volume.fd)!=0){if(error&&*error==NULL)ntfs_set_error(error,"syncing NTFS stage metadata failed");goto fail;}ntfs_layout_free(&layout);ntfs_close_volume(&volume);return 0;
fail:ntfs_layout_free(&layout);ntfs_close_volume(&volume);return -1;}



static int workspace_digest(const uint8_t *data, size_t length,
                            uint8_t digest[SHA256_DIGEST_LENGTH], char **error) {
    unsigned int digest_length = 0;
    if (EVP_Digest(data, length, digest, &digest_length, EVP_sha256(), NULL) != 1 ||
        digest_length != SHA256_DIGEST_LENGTH) {
        ntfs_set_error(error, "computing NTFS workspace cluster digest failed");
        return -1;
    }
    return 0;
}

int ntfs_prepare_workspace_map(sqlite3 *db, uint64_t workspace_start,
                               uint64_t workspace_clusters, char **error) {
    if (workspace_clusters == 0 ||
        workspace_start > UINT64_MAX - workspace_clusters) {
        ntfs_set_error(error, "NTFS terminal workspace geometry is invalid");
        return -1;
    }
    if (sql_exec(db,
                 "CREATE TABLE IF NOT EXISTS workspace("
                 "old INTEGER PRIMARY KEY,target INTEGER NOT NULL UNIQUE,"
                 "slot INTEGER NOT NULL UNIQUE,sha BLOB);"
                 "BEGIN IMMEDIATE;DELETE FROM workspace",
                 error) != 0) return -1;
    sqlite3_stmt *select = NULL, *insert = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT old,target FROM blocks WHERE old<>target ORDER BY old", -1,
            &select, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(db,
            "INSERT INTO workspace(old,target,slot,sha) VALUES (?,?,?,NULL)", -1,
            &insert, NULL) != SQLITE_OK) {
        sql_error(db, error, "preparing NTFS terminal workspace map");
        goto fail;
    }
    uint64_t index = 0;
    while (sqlite3_step(select) == SQLITE_ROW) {
        if (index >= workspace_clusters) {
            ntfs_set_error(error, "NTFS terminal workspace is smaller than the relocation set");
            goto fail;
        }
        uint64_t old_cluster = (uint64_t)sqlite3_column_int64(select, 0);
        uint64_t target_cluster = (uint64_t)sqlite3_column_int64(select, 1);
        uint64_t slot = workspace_start + index;
        sqlite3_reset(insert);
        sqlite3_clear_bindings(insert);
        sqlite3_bind_int64(insert, 1, (sqlite3_int64)old_cluster);
        sqlite3_bind_int64(insert, 2, (sqlite3_int64)target_cluster);
        sqlite3_bind_int64(insert, 3, (sqlite3_int64)slot);
        if (sqlite3_step(insert) != SQLITE_DONE) {
            sql_error(db, error, "recording NTFS terminal workspace map");
            goto fail;
        }
        index++;
    }
    if (index != workspace_clusters) {
        ntfs_set_error(error,
                       "NTFS terminal workspace map contains %llu clusters; expected %llu",
                       (unsigned long long)index,
                       (unsigned long long)workspace_clusters);
        goto fail;
    }
    sqlite3_finalize(select);
    sqlite3_finalize(insert);
    if (sql_exec(db, "COMMIT;PRAGMA wal_checkpoint(FULL)", error) != 0) return -1;
    return 0;
fail:
    sqlite3_finalize(select);
    sqlite3_finalize(insert);
    (void)sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
    return -1;
}

static int workspace_copy_rows(const char *device, sqlite3 *db, uint32_t cluster_size,
                               const char *query, bool stop_aware,
                               bool record_digest, char **error) {
    int fd = open(device, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        ntfs_set_error(error, "cannot open NTFS source for terminal-workspace I/O: %s",
                       strerror(errno));
        return -1;
    }
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        ntfs_set_error(error, "cannot lock NTFS source for terminal-workspace I/O: %s",
                       strerror(errno));
        close(fd);
        return -1;
    }
    sqlite3_stmt *rows = NULL, *update = NULL;
    if (sqlite3_prepare_v2(db, query, -1, &rows, NULL) != SQLITE_OK) {
        close(fd);
        return sql_error(db, error, "reading NTFS terminal workspace map");
    }
    if (record_digest) {
        if (sql_exec(db, "BEGIN IMMEDIATE", error) != 0) {
            sqlite3_finalize(rows); close(fd); return -1;
        }
        if (sqlite3_prepare_v2(db, "UPDATE workspace SET sha=? WHERE old=?", -1,
                              &update, NULL) != SQLITE_OK) {
            sql_error(db, error, "preparing NTFS workspace digest update");
            (void)sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
            sqlite3_finalize(rows); close(fd); return -1;
        }
    }
    uint8_t *buffer = ld_xmalloc(cluster_size);
    uint64_t copied = 0;
    int result = 0;
    while (sqlite3_step(rows) == SQLITE_ROW) {
        if (stop_aware && ld_stop_requested()) { result = -2; break; }
        uint64_t source_cluster = (uint64_t)sqlite3_column_int64(rows, 0);
        uint64_t target_cluster = (uint64_t)sqlite3_column_int64(rows, 1);
        uint64_t source_offset = source_cluster * (uint64_t)cluster_size;
        uint64_t target_offset = target_cluster * (uint64_t)cluster_size;
        ssize_t got = ld_pread_full(fd, buffer, cluster_size, source_offset);
        if (got < 0 || (size_t)got != cluster_size) {
            ntfs_set_error(error, "short read during NTFS terminal-workspace relocation");
            result = -1; break;
        }
        if (record_digest) {
            uint8_t digest[SHA256_DIGEST_LENGTH];
            if (workspace_digest(buffer, cluster_size, digest, error) != 0) {
                result = -1; break;
            }
            sqlite3_reset(update);
            sqlite3_clear_bindings(update);
            sqlite3_bind_blob(update, 1, digest, SHA256_DIGEST_LENGTH, SQLITE_TRANSIENT);
            sqlite3_bind_int64(update, 2, (sqlite3_int64)source_cluster);
            if (sqlite3_step(update) != SQLITE_DONE) {
                sql_error(db, error, "recording NTFS workspace cluster digest");
                result = -1; break;
            }
        }
        ssize_t wrote = ld_pwrite_full(fd, buffer, cluster_size, target_offset);
        if (wrote < 0 || (size_t)wrote != cluster_size) {
            ntfs_set_error(error, "short write during NTFS terminal-workspace relocation");
            result = -1; break;
        }
        copied++;
    }
    if (result == 0 && fsync(fd) != 0) {
        ntfs_set_error(error, "syncing NTFS terminal-workspace relocation failed: %s",
                       strerror(errno));
        result = -1;
    }
    if (record_digest) {
        if (result == 0) {
            if (sql_exec(db, "COMMIT;PRAGMA wal_checkpoint(FULL)", error) != 0) result = -1;
        } else {
            (void)sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
        }
    }
    sqlite3_finalize(update);
    sqlite3_finalize(rows);
    free(buffer);
    close(fd);
    (void)copied;
    return result;
}

int ntfs_stage_workspace(const char *device, sqlite3 *db, uint32_t cluster_size,
                         char **error) {
    return workspace_copy_rows(device, db, cluster_size,
                               "SELECT old,slot FROM workspace ORDER BY old",
                               true, true, error);
}

int ntfs_place_workspace(const char *device, sqlite3 *db, uint32_t cluster_size,
                         bool stop_aware, char **error) {
    return workspace_copy_rows(device, db, cluster_size,
                               "SELECT slot,target FROM workspace ORDER BY old",
                               stop_aware, false, error);
}

int ntfs_restore_workspace(const char *device, sqlite3 *db, uint32_t cluster_size,
                           char **error) {
    return workspace_copy_rows(device, db, cluster_size,
                               "SELECT slot,old FROM workspace ORDER BY old",
                               false, false, error);
}

int ntfs_verify_workspace(const char *device, sqlite3 *db, uint32_t cluster_size,
                          char **error) {
    int fd = open(device, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        ntfs_set_error(error, "cannot open NTFS source to verify terminal workspace: %s",
                       strerror(errno));
        return -1;
    }
    sqlite3_stmt *rows = NULL;
    if (sqlite3_prepare_v2(db, "SELECT slot,sha FROM workspace ORDER BY old", -1,
                          &rows, NULL) != SQLITE_OK) {
        close(fd);
        return sql_error(db, error, "reading NTFS workspace verification map");
    }
    uint8_t *buffer = ld_xmalloc(cluster_size);
    uint64_t verified = 0;
    int result = 0;
    while (sqlite3_step(rows) == SQLITE_ROW) {
        uint64_t slot = (uint64_t)sqlite3_column_int64(rows, 0);
        const void *expected = sqlite3_column_blob(rows, 1);
        int expected_bytes = sqlite3_column_bytes(rows, 1);
        if (expected == NULL || expected_bytes != SHA256_DIGEST_LENGTH) {
            ntfs_set_error(error, "NTFS terminal workspace is not fully checksummed");
            result = -1; break;
        }
        ssize_t got = ld_pread_full(fd, buffer, cluster_size,
                                    slot * (uint64_t)cluster_size);
        uint8_t actual[SHA256_DIGEST_LENGTH];
        if (got < 0 || (size_t)got != cluster_size ||
            workspace_digest(buffer, cluster_size, actual, error) != 0) {
            if (error != NULL && *error == NULL)
                ntfs_set_error(error, "short read while verifying NTFS terminal workspace");
            result = -1; break;
        }
        if (memcmp(actual, expected, SHA256_DIGEST_LENGTH) != 0) {
            ntfs_set_error(error, "NTFS terminal workspace checksum mismatch at cluster %llu",
                           (unsigned long long)slot);
            result = -1; break;
        }
        verified++;
    }
    sqlite3_finalize(rows);
    free(buffer);
    close(fd);
    if (result == 0 && verified == 0) {
        ntfs_set_error(error, "NTFS terminal workspace verification found no staged clusters");
        result = -1;
    }
    return result;
}

static NtfsStream *find_stream(NtfsCatalogue *catalogue,uint64_t record,uint32_t attr){for(size_t i=0;i<catalogue->count;++i)if(catalogue->items[i].record_number==record&&catalogue->items[i].attribute_offset==attr)return &catalogue->items[i];return NULL;}
int ntfs_verify_stage(const char *stage, sqlite3 *db, bool growth,
                      bool allow_dirty, char **error) {
    NtfsVolume volume;
    NtfsLayout layout;
    NtfsCatalogue catalogue;
    if (ntfs_open_volume(stage, false, &volume, error) != 0) return -1;
    if (ntfs_read_layout(&volume, allow_dirty, &layout, error) != 0) {
        ntfs_close_volume(&volume);
        return -1;
    }
    if (ntfs_scan_catalogue(&volume, &layout, &catalogue, error) != 0) {
        ntfs_layout_free(&layout);
        ntfs_close_volume(&volume);
        return -1;
    }

    int result = -1;
    sqlite3_stmt *streams = NULL;
    sqlite3_stmt *fixed = NULL;
    sqlite3_stmt *covered = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT record,attr,clusters,target,reserve,sha FROM streams",
            -1, &streams, NULL) != SQLITE_OK) {
        sql_error(db, error, "preparing NTFS verification");
        goto done;
    }

    int state;
    while ((state = sqlite3_step(streams)) == SQLITE_ROW) {
        uint64_t record = (uint64_t)sqlite3_column_int64(streams, 0);
        uint32_t attr = (uint32_t)sqlite3_column_int(streams, 1);
        uint64_t clusters = (uint64_t)sqlite3_column_int64(streams, 2);
        uint64_t target = (uint64_t)sqlite3_column_int64(streams, 3);
        uint64_t reserve = (uint64_t)sqlite3_column_int64(streams, 4);
        NtfsStream *stream = find_stream(&catalogue, record, attr);
        if (stream == NULL || stream->runs.count != 1U || stream->runs.items[0].sparse ||
            stream->runs.items[0].lcn != target || stream->runs.items[0].length != clusters) {
            ntfs_set_error(error,
                           "NTFS canonical mapping verification failed for MFT record %llu",
                           (unsigned long long)record);
            goto final;
        }
        uint8_t digest[SHA256_DIGEST_LENGTH];
        if (stream_digest(&volume, stream, digest, error) != 0) goto final;
        const void *stored = sqlite3_column_blob(streams, 5);
        if (stored == NULL || sqlite3_column_bytes(streams, 5) != SHA256_DIGEST_LENGTH ||
            memcmp(stored, digest, SHA256_DIGEST_LENGTH) != 0) {
            ntfs_set_error(error,
                           "NTFS payload checksum changed for MFT record %llu",
                           (unsigned long long)record);
            goto final;
        }
        if (target > volume.total_clusters || clusters > volume.total_clusters - target ||
            reserve > volume.total_clusters - target - clusters) {
            ntfs_set_error(error,
                           "NTFS growth reserve for MFT record %llu extends beyond the filesystem",
                           (unsigned long long)record);
            goto final;
        }
        for (uint64_t c = 0; c < reserve; ++c) {
            if (ntfs_bitmap_bit(&layout, target + clusters + c)) {
                ntfs_set_error(error,
                               "NTFS growth-reserve cluster %llu is allocated after MFT record %llu",
                               (unsigned long long)(target + clusters + c),
                               (unsigned long long)record);
                goto final;
            }
        }
    }
    if (state != SQLITE_DONE) {
        sql_error(db, error, "reading NTFS verification plan");
        goto final;
    }

    if (sqlite3_prepare_v2(db,
            "SELECT record,attr,clusters,start,reserve,sha FROM fixed_primary",
            -1, &fixed, NULL) != SQLITE_OK) {
        sql_error(db, error, "preparing NTFS fixed-stream verification");
        goto final;
    }
    while ((state = sqlite3_step(fixed)) == SQLITE_ROW) {
        uint64_t record = (uint64_t)sqlite3_column_int64(fixed, 0);
        uint32_t attr = (uint32_t)sqlite3_column_int(fixed, 1);
        uint64_t clusters = (uint64_t)sqlite3_column_int64(fixed, 2);
        uint64_t start_cluster = (uint64_t)sqlite3_column_int64(fixed, 3);
        uint64_t reserve = (uint64_t)sqlite3_column_int64(fixed, 4);
        NtfsStream *stream = find_stream(&catalogue, record, attr);
        if (stream == NULL || stream->directory || !primary_object_stream(stream) ||
            stream->runs.count != 1U || stream->runs.items[0].sparse ||
            stream->runs.items[0].lcn != start_cluster ||
            stream->runs.items[0].length != clusters) {
            ntfs_set_error(error,
                           "NTFS preserved primary stream in MFT record %llu changed mapping",
                           (unsigned long long)record);
            goto final;
        }
        uint8_t digest[SHA256_DIGEST_LENGTH];
        if (stream_digest(&volume, stream, digest, error) != 0) goto final;
        const void *stored = sqlite3_column_blob(fixed, 5);
        if (stored == NULL || sqlite3_column_bytes(fixed, 5) != SHA256_DIGEST_LENGTH ||
            memcmp(stored, digest, SHA256_DIGEST_LENGTH) != 0) {
            ntfs_set_error(error,
                           "NTFS preserved primary payload changed for MFT record %llu",
                           (unsigned long long)record);
            goto final;
        }
        if (start_cluster > volume.total_clusters || clusters > volume.total_clusters - start_cluster ||
            reserve > volume.total_clusters - start_cluster - clusters) {
            ntfs_set_error(error,
                           "NTFS preserved 10%% reserve for MFT record %llu extends beyond the filesystem",
                           (unsigned long long)record);
            goto final;
        }
        for (uint64_t c = 0; c < reserve; ++c) {
            if (ntfs_bitmap_bit(&layout, start_cluster + clusters + c)) {
                ntfs_set_error(error,
                               "NTFS preserved 10%% reserve cluster %llu is allocated after MFT record %llu",
                               (unsigned long long)(start_cluster + clusters + c),
                               (unsigned long long)record);
                goto final;
            }
        }
    }
    if (state != SQLITE_DONE) {
        sql_error(db, error, "reading NTFS fixed-stream verification plan");
        goto final;
    }

    if (catalogue.malformed_records != 0) {
        ntfs_set_error(error, "NTFS verification found malformed MFT records");
        goto final;
    }

    if (growth) {
        if (sqlite3_prepare_v2(db,
                "SELECT 1 FROM streams WHERE record=? AND attr=? "
                "UNION ALL SELECT 1 FROM fixed_primary WHERE record=? AND attr=? LIMIT 1",
                -1, &covered, NULL) != SQLITE_OK) {
            sql_error(db, error, "preparing NTFS growth-contract coverage verification");
            goto final;
        }
        for (size_t i = 0; i < catalogue.count; ++i) {
            NtfsStream *stream = &catalogue.items[i];
            if (stream->record_number < NTFS_FIRST_USER_RECORD || stream->clusters == 0 ||
                stream->directory || !primary_object_stream(stream)) continue;
            sqlite3_reset(covered);
            sqlite3_clear_bindings(covered);
            sqlite3_bind_int64(covered, 1, (sqlite3_int64)stream->record_number);
            sqlite3_bind_int(covered, 2, (int)stream->attribute_offset);
            sqlite3_bind_int64(covered, 3, (sqlite3_int64)stream->record_number);
            sqlite3_bind_int(covered, 4, (int)stream->attribute_offset);
            int covered_state = sqlite3_step(covered);
            if (covered_state != SQLITE_ROW) {
                if (covered_state != SQLITE_DONE)
                    sql_error(db, error, "checking NTFS growth-contract coverage");
                else
                    ntfs_set_error(error,
                                   "NTFS growth plan omitted primary stream in MFT record %llu attribute 0x%x",
                                   (unsigned long long)stream->record_number,
                                   stream->attribute_offset);
                goto final;
            }
        }
    }

    result = 0;
final:
    sqlite3_finalize(streams);
    sqlite3_finalize(fixed);
    sqlite3_finalize(covered);
done:
    ntfs_catalogue_free(&catalogue);
    ntfs_layout_free(&layout);
    ntfs_close_volume(&volume);
    return result;
}

