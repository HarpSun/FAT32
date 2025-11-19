/*
FAT32 parser

reference:
FAT: https://wiki.osdev.org/FAT#Implementation_Details
    
Flexible Array Member: https://en.wikipedia.org/wiki/Flexible_array_member
*/

#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <uchar.h>
#include <locale.h>
#include <time.h>
#include <math.h>
#include <assert.h>

#define _FILE_OFFSET_BITS 64
#define FUSE_USE_VERSION 32
#define _GNU_SOURCE

#include <fuse.h>


/*
FAT structure
*/
// BIOS Parameter Block
typedef struct __attribute__((packed)) {
    uint8_t boot_cmd[3];
    uint8_t oem_identifier[8];
    uint16_t num_of_bytes_per_sector;
    uint8_t num_of_sectors_per_cluster;
    uint16_t num_of_reserved_sectors;
    uint8_t num_of_fat;
    uint16_t num_of_root_dir_entries;
    uint16_t total_sectors;
    uint8_t media_descriptor_type;
    uint16_t num_of_sectors_per_fat;  // FAT12/FAT16 only
    uint16_t num_of_sectors_per_track;
    uint16_t num_of_heads;
    uint32_t num_of_hidden_sectors;
    uint32_t large_sectors_count;
} BPB;

typedef struct __attribute__((packed)) {
    uint32_t sectors_per_fat;
    uint16_t flags;
    uint16_t fat_version;
    uint32_t root_cluster_num;
    uint16_t fs_info_sector_num;
    uint16_t backup_boot_sector_num;
    uint8_t reserved[12];
    uint8_t drive_number;
    uint8_t win_nt_flags;
    uint8_t signature;
    uint32_t volume_serial_num;
    char volume_label_str[11];
    char system_ident[8];
} ExtendBootRecord;

typedef struct __attribute__((packed)) {
    uint8_t signature[4];
    uint8_t reserved[480];
    uint8_t signature2[4];
    uint32_t last_free_cluster_count;
    uint32_t last_allocted_cluster;
    uint8_t reserved2[12];
    uint8_t signature3[4];
} FSInfo;

typedef struct __attribute__((packed)) {
    uint8_t order;
    uint8_t name1[10];
    uint8_t attr;
    uint8_t long_entry_type;
    uint8_t checksum;
    uint8_t name2[12];
    uint16_t zero;
    uint8_t name3[4];
} LFN;

// standard 8.3 format
typedef struct __attribute__((packed)) {
    char file_name[11];
    uint8_t attr;
    uint8_t reserved;
    uint8_t creation_time_millisecond;
    uint16_t created_time;
    uint16_t created_date;
    uint16_t last_accessed_date;
    uint16_t high_cluster_num;
    uint16_t last_modify_time;
    uint16_t last_modify_date;
    uint16_t low_cluster_num;
    uint32_t file_size;
    uint8_t lfn_size;
    LFN lfns[];
} DirEntry;

typedef struct {
    int size;
    DirEntry* entries[];
} DirEntryList;

typedef struct {
    int size;
    uint32_t cluster_info[];
} FAT;

static int fd;
// anchor point
// easy to make navigation through file buffer
static int data_offset;
// bpb contain fs meta data, fat is index of data
// should be easily access by any function
static BPB bpb;
static ExtendBootRecord ebr;
static FAT* fat;
static FSInfo fs_info;

#define CLUSTER_END 0x0ffffff8
#define CLUSTER_BAD 0x0ffffff7
#define LFN_END_MASK 0x40
#define LFN_NUM_MASK 0x17

#define IS_DIR(entry) (entry)->attr == 0x10
    
    
void showBPB(BPB bpb)
{
    printf("--------------- BPB ---------------\n");
    printf("%x\n", bpb.boot_cmd[0]);
    printf("%x\n", bpb.boot_cmd[1]);
    printf("%x\n", bpb.boot_cmd[2]);

    printf("num_of_bytes_per_sector: %d\n", bpb.num_of_bytes_per_sector);
    printf("num_of_sectors_per_cluster: %d\n", bpb.num_of_sectors_per_cluster);
    printf("num_of_reserved_sectors: %d\n", bpb.num_of_reserved_sectors);
    printf("num_of_fat: %d\n", bpb.num_of_fat);
}

void showFSInfo(FSInfo fs)
{
    printf("--------------- FSInfo ---------------\n");
    printf("last_free_cluster_count: %d\n", fs.last_free_cluster_count);
    printf("last_allocated_cluster: %d\n", fs.last_allocted_cluster);
}

void showFAT(FAT* fat)
{
    printf("--------------- FAT ---------------\n");
    printf("size: %d\n", fat->size);
    for (int i = 0; i < 16; i++)
    {
        printf("%x ", fat->cluster_info[i]);
    }
    printf("\n");
}

void showDirEntry(DirEntry* entry)
{
    printf("--------------- DirEntry ---------------\n");
    printf("file_name: %s\n", entry->file_name);
    printf("file_size: %d\n", entry->file_size);
    printf("cluster_num: %d\n", (entry->high_cluster_num << 16) + entry->low_cluster_num);
}

//////////////////////////////////////////////////
//
//                utils
//
void strip_whitespace(char* str, char* res, int len)
{
    int end = 0;
    for (int i = len - 1; i >= 0; i--)
    {
        if (str[i] != 0x20)
        {
            end = i;
            break;
        }
    }
    memcpy(res, str, end + 1);
}

void utf16_to_utf8(char16_t* str, char* res)
{
    mbstate_t ps;
    memset(&ps, 0, sizeof(ps));

    char* offset = &res[0];
    int i = 0;
    char16_t c;
    while (c = str[i], c != 0)
    {
        char buf[4] = {0};
        c16rtomb(buf, c, &ps);
        memcpy(offset, buf, strlen(buf));
        offset += strlen(buf);
        i++;
    }
}

// only support 2 byte utf16 character, 4 byte chars like emoji is not supported
void utf8_to_utf16(char* str, char16_t* res)
{
    mbstate_t ps;
    memset(&ps, 0, sizeof(ps));

    char16_t* offset = &res[0];
    int i = 0;
    char c;
    while (c = str[i], c != 0)
    {
        // int n = mbr_size(c);
        char16_t buf;
        int n = mbrtoc16(&buf, &str[i], 4, &ps);
        memcpy(offset, &buf, 2);
        offset += 1;
        i += n;
    }
    *offset = 0;
}

size_t strlen_chr16(char16_t* str)
{
    size_t i = 0;
    while (str[i] != 0xff && str[i] != 0)
    {
        i += 1;
    }
    return i;
}

int min(int a, int b)
{
    return a > b ? b : a;
}


//////////////////////////////////////////////////
//
//                disk buffer
//
uint32_t alloc_cluster(uint32_t cluster_num)
{
    fs_info.last_allocted_cluster += 1;
    fat->cluster_info[cluster_num] = fs_info.last_allocted_cluster;
    fat->cluster_info[fs_info.last_allocted_cluster] = CLUSTER_END;
    printf("[alloc_cluster] %d %d\n", cluster_num, fs_info.last_allocted_cluster);
    return fs_info.last_allocted_cluster;
}

typedef struct {
    uint8_t* data;
    uint8_t* begin;
    int size;
    uint32_t cluster_num;
    size_t disk_offset;
    bool end;
} DiskBuffer;

typedef struct {
    int offset;
    uint32_t cluster_num;
} BufferSave;

typedef struct {
    DirEntry* entry;
    BufferSave loc;
} DirEntryWithLoc;

DiskBuffer* buffer_init(uint32_t cluster_num)
{
    DiskBuffer* buffer = malloc(sizeof(DiskBuffer));
    memset(buffer, 0, sizeof(DiskBuffer));

    size_t bytes_per_cluster = bpb.num_of_sectors_per_cluster * bpb.num_of_bytes_per_sector;
    buffer->size = bytes_per_cluster;
    buffer->cluster_num = cluster_num;
    buffer->disk_offset = data_offset + ((cluster_num) - 2) * bytes_per_cluster;
    lseek(fd, buffer->disk_offset, SEEK_SET);
    buffer->begin = malloc(bytes_per_cluster);
    buffer->data = buffer->begin;
    read(fd, buffer->begin, bytes_per_cluster);
    buffer->end = false;
    return buffer;
}

void buffer_reload(DiskBuffer* buffer, uint32_t cluster_num)
{
    if (buffer->cluster_num == cluster_num)
    {
        buffer->data = buffer->begin;
    }
    else
    {
        buffer->disk_offset = data_offset + ((cluster_num) - 2) * buffer->size;
        lseek(fd, buffer->disk_offset, SEEK_SET);
        read(fd, buffer->begin, buffer->size);
        buffer->data = buffer->begin;
        buffer->cluster_num = cluster_num;
    }
}

void buffer_sync(DiskBuffer* buffer)
{
    printf("[buffer_sync] %d\n", buffer->cluster_num);
    lseek(fd, buffer->disk_offset, SEEK_SET);
    write(fd, buffer->begin, buffer->size);
}

void fat_sync()
{
    int fat_offset = bpb.num_of_reserved_sectors * bpb.num_of_bytes_per_sector;
    int size = bpb.num_of_fat * ebr.sectors_per_fat * bpb.num_of_bytes_per_sector;
    printf("[fat_sync] %x %d\n", fat_offset, size);
    lseek(fd, fat_offset, SEEK_SET);
    write(fd, fat->cluster_info, size);
    // sync fsinfo
    lseek(fd, 512, SEEK_SET);
    write(fd, &fs_info, sizeof(FSInfo));
}


int _buffer_step(DiskBuffer* buffer, int n, bool sync)
{
    int offset = buffer->data - buffer->begin;
    if (offset + n < buffer->size)
    {
        buffer->data += n;
        return 0;
    }
    else
    {
        if (sync)
        {
            // buffer jump would lost all modification of buffer
            // if sync is set, write change to disk before jump
            buffer_sync(buffer);
        }
        
        uint32_t next_cluster = fat->cluster_info[buffer->cluster_num];
        // printf("cross cluster: %d %d\n", buffer->cluster_num, next_cluster);
        if (next_cluster >= CLUSTER_END || next_cluster == CLUSTER_BAD)
        {
            printf("[**cluster_end]\n");
            buffer->end = true;
            return -1;
        }
        else
        {
            buffer_reload(buffer, next_cluster);
            buffer->data += n + offset - buffer->size;
            return 0;
        }
    }
}

#define buffer_step(db, n) _buffer_step(db, n, false)
#define buffer_step_sync(db, n) _buffer_step(db, n, true)

BufferSave buffer_save(DiskBuffer* buffer)
{
    BufferSave s = {
        .offset = buffer->data - buffer->begin,
        .cluster_num = buffer->cluster_num,
    };
    return s;
}

void buffer_recover(DiskBuffer* buffer, BufferSave save)
{
    buffer_reload(buffer, save.cluster_num);
    buffer->data += save.offset;
}

void showDiskBuffer(DiskBuffer* buffer)
{
    printf("--------------- DiskBUffer ---------------\n");
    printf("cluster_num: %d\n", buffer->cluster_num);
    printf("disk_offset: %lx\n", buffer->disk_offset);
    for (int i = 0; i < 32; i++)
    {
        printf("%x ", buffer->data[i]);
    }
    printf("\n");
}

bool entry_end(DiskBuffer* db)
{
    return db->data[0] == 0 || db->end;
}

//////////////////////////////////////////////////
//
//                parser
//
BPB parse_bpb(uint8_t* buffer)
{
    BPB bpb;
    memcpy(&bpb, buffer, sizeof(bpb));
    return bpb;
}

ExtendBootRecord parse_extend_boot_record(uint8_t* buffer)
{
    ExtendBootRecord ebr;
    memcpy(&ebr, buffer, sizeof(ebr));
    return ebr;
}

FSInfo parse_fs_info(uint8_t* buffer)
{
    FSInfo fs;
    memcpy(&fs, buffer, sizeof(FSInfo));
    return fs;
}

void parse_lfn(LFN lfns[], char* name)
{
    char16_t utf16_name[256] = {0};
    char16_t* offset = &utf16_name[0];
    int size = lfns[0].order - LFN_END_MASK;
    for (int i = size - 1; i >= 0; i--)
    {
        LFN lfn = lfns[i];
        memcpy(offset, lfn.name1, 10);
        offset += 5;
        memcpy(offset, lfn.name2, 12);
        offset += 6;
        memcpy(offset, lfn.name3, 4);
        offset += 2;
    }
    utf16_to_utf8(utf16_name, name);
}

void parse_path(char* path, char* current_path, char* left_path)
{
    if (path[0] == '/')
    {
        path = path + 1;
    }
    
    int i = 0;
    while (path[i] != '/' && i < strlen(path))
    {
        i++;
    }

    memcpy(current_path, path, i);
    memcpy(left_path, path + 1, strlen(path) - i);
}

DirEntry* parse_dir_entry(DiskBuffer* db)
{
    if (entry_end(db))
    {
        return NULL;
    }

    // skip deleted entry
    if (db->data[0] == 0xe5)
    {
        while (db->data[0] == 0xe5)
        {
            buffer_step(db, 32);
        }
        return NULL;
    }
    
    // printf("[parse_dir_entry]\n");
    uint8_t attr = db->data[11];
    int lfn_size = 0;
    if (attr == 0x0f)
    {
        // lfns are reversed placed, so its first order byte can indicate num of lfn entry
        lfn_size = db->data[0] - LFN_END_MASK;
    }

    printf("lfn_size: %d\n", lfn_size);
    assert(lfn_size != 0);
    DirEntry* entry = malloc(sizeof(DirEntry) + lfn_size * sizeof(LFN));
    int i = 1;
    while (attr == 0x0f)
    {
        memcpy((void*)entry + 32 * i + 1, db->data, sizeof(LFN));
        buffer_step(db, 32);
        attr = db->data[11];
        i++;
    }
    entry->lfn_size = lfn_size;
    // decrease size of lfn_size and buffer which is not part of Standard DirEntry
    memcpy(entry, db->data, sizeof(DirEntry) - 1);
    buffer_step(db, 32);

    return entry;
}

DirEntryList* parse_dir_entries(DiskBuffer* db)
{
    // flexible array member
    int size = sizeof(DirEntryList) + 100 * sizeof(DirEntry*);
    DirEntryList* es = malloc(size);
    memset(es, 0, size);

    while (!entry_end(db))
    {
        printf("[parse_entry start]\n");
        DirEntry* entry = parse_dir_entry(db);
        if (entry != NULL)
        {
            es->entries[es->size] = entry;
            es->size += 1;
            // showDirEntry(entry);
        }
    }
    return es;
}

void parse_file_name(DirEntry* entry, char* file_name)
{
    printf("[parse_file_name] %s\n", entry->file_name);
    if (entry->lfn_size > 0)
    {
        parse_lfn(entry->lfns, file_name);
    }
    else
    {
        strip_whitespace(entry->file_name, file_name, 11);
    }
}

time_t parse_file_date(DirEntry* entry)
{
    struct tm tm;
    // tm year starts from 1900, fat32 year starts from 1980
    tm.tm_year = ((entry->last_modify_date & 0b1111111000000000) >> 9) + 80;
    tm.tm_mon = ((entry->last_modify_date & 0b0000000111100000) >> 5) - 1;
    tm.tm_mday  = entry->last_modify_date & 0b0000000000011111;
    // utc to zh/shanghai time
    tm.tm_hour  = ((entry->last_modify_time & 0b1111100000000000) >> 11) + 8;
    tm.tm_min   = (entry->last_modify_time & 0b0000011111100000) >> 5;
    tm.tm_sec   = entry->last_modify_time & 0b0000000000011111;
    tm.tm_isdst = -1;
    return mktime(&tm);
    // printf("%b, %b, %d\n", entry->last_modify_date, entry->created_time, entry->creation_time_millisecond);
}

void walk_fs(DiskBuffer* db)
{
    DirEntryList* es = parse_dir_entries(db);
    for (int i = 0; i < es->size; i++)
    {
        DirEntry* e = es->entries[i];
        uint32_t n = (e->high_cluster_num << 16) + e->low_cluster_num;
        
        if (IS_DIR(e))
        {
            char file_name[12];
            strip_whitespace(e->file_name, file_name, 11);
            if (strcmp(file_name, ".") == 0 || strcmp(file_name, "..") == 0)
            {
                continue;
            }
            // printf("[folder_data_offset] %x %d\n", (int)dir_offset + DATA_OFFSET(e, bpb), e.cluster_num);
            buffer_reload(db, n);
            walk_fs(db);
        }
        else
        {
            // printf("[data_offset] %p %d\n", DATA_OFFSET(e.cluster_num), e.cluster_num);
            char content[e->file_size + 512];
            buffer_reload(db, n);
            memcpy(content, db->data, bpb.num_of_bytes_per_sector);
            int i = 0;
            while (buffer_step(db, bpb.num_of_bytes_per_sector) != -1)
            {
                char* dst = &content[0] + (i + 1) * bpb.num_of_bytes_per_sector;
                memcpy(dst, db->data, bpb.num_of_bytes_per_sector);
                i += 1;
            }
            printf("content: %s\n", content);
        }
    }
}

FAT* parse_fat(uint8_t* buffer)
{
    // FAT is index of clusters, it's an array structure, every index is 4 bytes wide
    // index 0 and 1 are reserved, so root entry starts at index(cluster) 2
    int len = (bpb.num_of_fat * ebr.sectors_per_fat * bpb.num_of_bytes_per_sector) / 4;
    FAT* fat = malloc(sizeof(FAT) + len * sizeof(uint32_t));
    fat->size = len;
    memcpy(fat->cluster_info, buffer, bpb.num_of_fat * ebr.sectors_per_fat * bpb.num_of_bytes_per_sector);
    // showFAT(fat);
    
    return fat;
}

//////////////////////////////////////////////////
//
//                fuse fat32 driver 
//
DirEntryWithLoc find_entry(char* path, DiskBuffer* buffer)
{
    char current_path[256] = {0};
    char left_path[1024] = {0};
    parse_path(path, current_path, left_path);

    while (!entry_end(buffer))
    {
        // printf("[parse_entry start]\n";)
        BufferSave save = buffer_save(buffer);
        DirEntry* e = parse_dir_entry(buffer);
        if (e != NULL)
        {
            char file_name[1024] = {0};
            parse_file_name(e, file_name);
            if (strcmp(file_name, current_path) == 0)
            {
                if (strcmp(left_path, "") == 0 || strcmp(left_path, "/") == 0)
                {
                    return (DirEntryWithLoc){
                        .entry = e,
                        .loc = save,
                    };
                }
                else
                {
                    uint32_t n = (e->high_cluster_num << 16) + e->low_cluster_num;
                    buffer_reload(buffer, n);
                    return find_entry(left_path, buffer);
                }
            }
        }
    }
    
    return (DirEntryWithLoc){
        .entry = NULL,
    };
}


// /dir1/dir2  dir1/dir2 dir2
void readdir(char* path, fuse_fill_dir_t addEntry, void* buffer)
{
    // printf("path: %s %p\n", path, entry_buffer);
    DiskBuffer* disk_buffer = buffer_init(ebr.root_cluster_num);
    DirEntryList* es;
    if (strcmp(path, "") == 0 || strcmp(path, "/") == 0)
    {
        es = parse_dir_entries(disk_buffer);
    }
    else
    {
        DirEntry* e = find_entry(path, disk_buffer).entry;
        uint32_t n = (e->high_cluster_num << 16) + e->low_cluster_num;
        buffer_reload(disk_buffer, n);
        es =  parse_dir_entries(disk_buffer);
    }

    for (int i = 0; i < es->size; i++)
    {
        DirEntry* e = es->entries[i];
        char file_name[1024] = {0};
        parse_file_name(e, file_name);
        addEntry(buffer, file_name, NULL, 0);
    }
}

int callback_readdir(const char *path, void *buffer, fuse_fill_dir_t addEntry, off_t offset, struct fuse_file_info *fileInfo)
{
    printf("[readdir] %s\n", path);
    readdir((char*)path, addEntry, buffer);
    return 0;
}

// TODO: add cache for entry
int getattr(char* path, struct stat* stat)
{
    DiskBuffer* disk_buffer = buffer_init(ebr.root_cluster_num);
    DirEntry* e = find_entry(path, disk_buffer).entry;
    if (e == NULL)
    {
        return -ENOENT;
    }
    else
    {
        if (IS_DIR(e))
        {
            stat->st_mode = S_IFDIR | 0644;
        }
        else
        {
            stat->st_mode = S_IFREG | 0644;
        }
        struct timespec t = {.tv_sec = parse_file_date(e)};
        stat->st_mtim = t;
        stat->st_size = e->file_size;
        return 0;
    }
}

int callback_getattr(const char *path, struct stat *stat)
{
    printf("[getattr] %s\n", path);
    if (strcmp(path, "/") == 0)
    {
        stat->st_mode = S_IFDIR | 0755;
        return 0;
    }
    else
    {
        return getattr((char*)path, stat);
    }
}

int read_content(char* path, char* buffer, size_t size, off_t offset)
{
    DiskBuffer* disk_buffer = buffer_init(ebr.root_cluster_num);
    DirEntry* e = find_entry(path, disk_buffer).entry;
    if (e == NULL)
    {
        return 0;
    }
    else
    {
        uint32_t n = (e->high_cluster_num << 16) + e->low_cluster_num;
        buffer_reload(disk_buffer, n);
        // uint8_t* content = DATA_OFFSET(n);
        buffer_step(disk_buffer, offset);
        int s = min(e->file_size - offset, size);
        memcpy(buffer, disk_buffer->data, s);
        return s;
    }
}

int callback_read(const char *path, char *buffer, size_t size, off_t offset, struct fuse_file_info *fileInfo)
{
    printf("[read] %s %ld %ld\n", path, size, offset);
    return read_content((char*)path, buffer, size, offset);
}

int unlink_entry(const char* path)
{
    DiskBuffer* db = buffer_init(ebr.root_cluster_num);
    DirEntryWithLoc e = find_entry((char*)path, db);
    DirEntry* entry = e.entry;
    if (entry == NULL)
    {
        return -ENOENT;
    }

    buffer_recover(db, e.loc);
    // printf("[save recover]\n");
    // showDiskBuffer(db);
    // 0xe5 for deleted
    while (db->data[11] == 0x0f)
    {
        db->data[0] = 0xe5;
        buffer_step_sync(db, 32);
    }
    db->data[0] = 0xe5;
    buffer_sync(db);

    // clear fat
    uint32_t prev = (entry->high_cluster_num << 16) + entry->low_cluster_num;
    int next = fat->cluster_info[prev];
    fat->cluster_info[prev] = 0;
    printf("cluster[%d] = 0\n", prev);
    while (next < CLUSTER_END && next != CLUSTER_BAD)
    {
        prev = next;
        next = fat->cluster_info[next];
        fat->cluster_info[prev] = 0;
        printf("cluster[%d] = 0\n", prev);
    }
    fat_sync();

    return 0;
}

int callback_unlink(const char* path)
{
    printf("[unlink] %s\n", path);
    return unlink_entry(path);
}

void new_entry(char* newpath, DirEntry* entry, DiskBuffer* db)
{
    char16_t new_name[128] = { [0 ... 127] = 0xffff };
    utf8_to_utf16(newpath, new_name);
    
    size_t len = strlen_chr16(new_name);
    int lfn_num = (int)ceil((double)len / 13);

    LFN lfns[lfn_num];
    char16_t* offset = &new_name[0];
    for (int i = 0; i < lfn_num; i++)
    {
        LFN lfn = {
            .order = (i == lfn_num - 1) ? LFN_END_MASK + i + 1 : i + 1,
            // .name1[10],
            .attr = 0x0f,
            .long_entry_type = 0,
            // .checksum = entry->checksum,
            // .name2[12],
            .zero = 0,
            // .name3[4],
        };
        memcpy(lfn.name1, offset, 10);
        offset += 5;
        memcpy(lfn.name2, offset, 12);
        offset += 6;
        memcpy(lfn.name3, offset, 4);
        offset += 2;

        lfns[i] = lfn;
    }

    for (int i = lfn_num - 1; i >= 0; i--)
    {
        memcpy(db->data, &lfns[i], sizeof(LFN));
        int res = buffer_step_sync(db, 32);
        if (res == -1)
        {
            // find next cluster, modify FAT
            uint32_t n = alloc_cluster(db->cluster_num);
            buffer_reload(db, n);
        }
    }
    
    DirEntry e = {
        .file_name = {0x20},
        .attr = entry->attr,
        .reserved = entry->reserved,
        .creation_time_millisecond = entry->creation_time_millisecond,
        .created_time = entry->created_time,
        .created_date = entry->created_date,
        .last_accessed_date = entry->last_accessed_date,
        .high_cluster_num = entry->high_cluster_num,
        .last_modify_time = entry->last_modify_time,
        .last_modify_date = entry->last_accessed_date,
        .low_cluster_num = entry->low_cluster_num,
        .file_size = entry->file_size,
    };

    memcpy(db->data, &e, sizeof(DirEntry) - 1);
    buffer_sync(db);
}

int rename_entry(const char* oldpath, const char* newpath)
{
    DiskBuffer* db = buffer_init(ebr.root_cluster_num);
    DirEntryWithLoc e = find_entry((char*)oldpath, db);
    DirEntry* entry = e.entry;

    if (entry == NULL)
    {
        return -ENOENT;
    }
    
    // delte old entry
    buffer_recover(db, e.loc);
    // 0xe5 for deleted
    while (db->data[11] == 0x0f)
    {
        db->data[0] = 0xe5;
        buffer_step_sync(db, 32);
    }
    db->data[0] = 0xe5;
    buffer_sync(db);

    printf("[delete old entry success]\n");
    // create new entry
    while (!entry_end(db))
    {
        int res = buffer_step(db, 32);
        if (res == -1)
        {
            int n = alloc_cluster(db->cluster_num);
            buffer_reload(db, n);
        }
    }

    new_entry((char*)newpath + 1, entry, db);
    fat_sync();
    
    return -ENOENT;
}

int callback_rename(const char* oldpath, const char* newpath)
{
    printf("[rename] %s %s\n", oldpath, newpath);
    return rename_entry(oldpath, newpath);
}

int write_content(char* path, const char* buffer, size_t size, off_t offset)
{
    DiskBuffer* db = buffer_init(ebr.root_cluster_num);
    DirEntryWithLoc e = find_entry(path, db);
    DirEntry* entry = e.entry;
    uint32_t cluster_num = (entry->high_cluster_num << 16) + entry->low_cluster_num;
    buffer_reload(db, cluster_num);
    buffer_step(db, offset);

    int left_size = size;
    while (left_size > 0)
    {
        int n = min(size, db->size - (db->data - db->begin));
        printf("write %d %d\n", n, cluster_num);
        memcpy(db->data, buffer, n);
        buffer += n;
        buffer_step_sync(db, n);
        left_size -= n;
    }
    buffer_sync(db);

    // change file size
    buffer_recover(db, e.loc);
    while (db->data[11] == 0x0f)
    {
        buffer_step(db, 32);
    }
    uint32_t new_size = entry->file_size + (size - left_size);
    memcpy(db->data + 28, &new_size, 4);
    
    buffer_sync(db);
    
    return size - left_size;
}

int callback_write(const char *path, const char *buffer, size_t size, off_t offset, struct fuse_file_info *fileInfo)
{
    printf("[write] %s %ld %ld\n", path, size, offset);
    return write_content((char*)path, buffer, size, offset);
}

int callback_open(const char* path, struct fuse_file_info *fileInfo)
{
    printf("[open] %s\n", path);
    fileInfo->fh = 0;
    return 0;
}

// don't no usage, but is needed for write
int callback_truncate(const char* path, off_t offset)
{
    printf("[truncate] %s %ld\n", path, offset);
    return 0;
}


//////////////////////////////////////////////////
//
//                tests
//
void test_disk_buffer()
{
    DiskBuffer* db = buffer_init(2);
    for (int i = 0; i < 512 / 32 + 1; i++)
    {
        showDiskBuffer(db);
        buffer_step(db, 32);
    }
}

int init_fs()
{
     // printf("hello fs\n");
    fd = open("disk.img", O_RDWR);
    if (fd == -1)
    {
        printf("open failed! %s\n", strerror(errno));
        return -1;
    }

    const int size = 1024;
    uint8_t* buffer = malloc(sizeof(uint8_t) * size);
    int res = read(fd, buffer, size);
    if (res == -1)
    {
        printf("read failed! %s\n", strerror(errno));
        return -1;
    }
    
    bpb = parse_bpb(buffer);
    ebr = parse_extend_boot_record(buffer + 36);
    fs_info = parse_fs_info(buffer + 512);

    int fat_offset = bpb.num_of_reserved_sectors * bpb.num_of_bytes_per_sector;
    const int fat_size = bpb.num_of_fat * ebr.sectors_per_fat * bpb.num_of_bytes_per_sector;
    uint8_t* buffer2 = malloc(sizeof(uint8_t) * fat_size);
    lseek(fd, fat_offset, SEEK_SET);
    read(fd, buffer2, fat_size);
    
    fat = parse_fat(buffer2);
    free(buffer);
    free(buffer2);
    
    showBPB(bpb);
    showFSInfo(fs_info);
    printf("fat_offset: %x\n", fat_offset);
    printf("fat_size: %d\n", fat->size);

    // jump to directory entry
    data_offset = fat_offset + ebr.sectors_per_fat * bpb.num_of_fat * bpb.num_of_bytes_per_sector;
    printf("dir_offset: %x\n", data_offset);
    return 0;
}


int main(int argc, char *argv[])
{
    setlocale(LC_ALL, "en_US.UTF-8");
    
    init_fs();
    
    // test_disk_buffer();
    // test_disk_buffer2();
    // test_disk_buffer_list();
    
    // DiskBuffer* db = buffer_init(ebr.root_cluster_num);
    // walk_fs(db);

    // fuse
    static struct fuse_operations operations = {
        .getattr    = callback_getattr,
        .readdir    = callback_readdir,
        .read       = callback_read,
        .rename     = callback_rename,
        .unlink     = callback_unlink,
        .write      = callback_write,
        .open       = callback_open,
        .truncate   = callback_truncate,
    };
    return fuse_main(argc, argv, &operations, NULL);
}



