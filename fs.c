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
} ExtendBootRecord;

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
    uint8_t file_name[11];
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
} DirEntry;

typedef struct {
    char file_name[512];
    int cluster_num;
    int file_size;
    int size;
    uint8_t attr;
} MyDirEntry;

typedef struct {
    int size;
    MyDirEntry entries[];
} DirEntryList;

typedef struct {
    int size;
    uint32_t cluster_info[];
} FAT;


// anchor point
// easy to make navigation through file buffer
static uint8_t* file_begin;
static uint8_t* fat_offset;
static uint8_t* dir_offset;
// bpb contain fs meta data, fat is index of data
// should be easily access by any function
static BPB bpb;
static ExtendBootRecord ebr;
static FAT* fat;


#define CLUSTER_END 0x0ffffff8
#define CLUSTER_BAD 0x0ffffff7
#define LFN_END_MASK 0x40
#define LFN_NUM_MASK 0x17

// root entry's cluster_num is 2
#define DATA_OFFSET(cluster_num) \
    dir_offset + ((cluster_num) - 2) * bpb.num_of_sectors_per_cluster * bpb.num_of_bytes_per_sector

#define IS_DIR(entry) (entry).attr == 0x10
    
    
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

void showDirEntry(MyDirEntry entry)
{
    printf("--------------- DirEntry ---------------\n");
    printf("file_name: %s\n", entry.file_name);
    printf("file_size: %d\n", entry.file_size);
    printf("cluster_num: %d\n", entry.cluster_num);
}


//////////////////////////////////////////////////
//
//                utils
//
bool entry_end(uint8_t* buffer)
{
    return buffer[0] == 0;
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

// auto jump to next cluster, if step would across current cluster
int step_buffer(uint8_t** buffer, size_t n)
{
    int bytes_per_cluster = bpb.num_of_bytes_per_sector * bpb.num_of_sectors_per_cluster;
    int pos = (*buffer - dir_offset) % bytes_per_cluster;
    if (pos + n < bytes_per_cluster)
    {
        *buffer += n;
        return 0;
    }
    else
    {
        int cluster_num = (*buffer - dir_offset) / bytes_per_cluster + 2;
        int v = fat->cluster_info[cluster_num];
        if (v < CLUSTER_END && v != CLUSTER_BAD)
        {
            *buffer = DATA_OFFSET(v);
            *buffer += n - (bytes_per_cluster - pos);
            return 0;
        }
        else
        {
            // printf("buffer_end");
            return -1;
        }
    }
}

//////////////////////////////////////////////////
//
//                parser
//
BPB parse_bpb(uint8_t* buffer)
{
    BPB bpb;
    memcpy(&bpb, buffer, sizeof(bpb));
    showBPB(bpb);
    return bpb;
}

ExtendBootRecord parse_extend_boot_record(uint8_t* buffer)
{
    ExtendBootRecord ebr;
    memcpy(&ebr, buffer, sizeof(ebr));
    return ebr;
}

void parse_lfn(uint8_t* buffer, MyDirEntry* entry)
{
    LFN lfn_array[20];
    int len = 0;
    
    uint8_t lfn_number;
    do {
        LFN lfn;
        memcpy(&lfn, buffer, sizeof(lfn));
        lfn_number = lfn.order & LFN_NUM_MASK;
        lfn_array[lfn_number - 1] = lfn;
        len += 1;
        buffer += 32;
    } while (lfn_number > 1);

    char16_t utf16_name[256];
    char16_t* offset = &utf16_name[0];
    for (int i = 0; i < len; i++)
    {
        LFN lfn = lfn_array[i];
        memcpy(offset, lfn.name1, 10);
        offset += 5;
        memcpy(offset, lfn.name2, 12);
        offset += 6;
        memcpy(offset, lfn.name3, 4);
        offset += 2;
    }
    
    char utf8_name[512] = {0};
    utf16_to_utf8(utf16_name, utf8_name);
    
    memcpy(entry->file_name, utf8_name, strlen(utf8_name));
    entry->size = (len + 1) * 32;
}

MyDirEntry parse_dir_entry(uint8_t* buffer)
{

    MyDirEntry e;
    
    uint8_t attr = buffer[11];
    if (attr == 0x0f)
    {
        parse_lfn(buffer, &e);
        step_buffer(&buffer, e.size - 32);
    }

    DirEntry entry;
    memcpy(&entry, buffer, sizeof(entry));

    e.cluster_num = (entry.high_cluster_num << 16) + entry.low_cluster_num;
    e.file_size = entry.file_size;
    e.attr = entry.attr;
    
    return e;
}

DirEntryList* parse_dir_entries(uint8_t* buffer)
{
    // flexible array member
    DirEntryList* es = malloc(sizeof(DirEntryList) + 100 * sizeof(MyDirEntry));
    while (!entry_end(buffer))
    {
        MyDirEntry entry = parse_dir_entry(buffer);
        showDirEntry(entry);
        step_buffer(&buffer, entry.size);
        
        es->entries[es->size] = entry;
        es->size += 1;
    }
    return es;
}

void walk_fs(MyDirEntry entry)
{
    uint8_t* offset = DATA_OFFSET(entry.cluster_num);
    DirEntryList* es =  parse_dir_entries(offset);
    for (int i = 0; i < es->size; i++)
    {
        MyDirEntry e = es->entries[i];
        if (IS_DIR(e))
        {
            if (strcmp(e.file_name, ".") == 0 || strcmp(e.file_name, "..") == 0)
            {
                continue;
            }
            // printf("[folder_data_offset] %x %d\n", (int)dir_offset + DATA_OFFSET(e, bpb), e.cluster_num);
            walk_fs(e);
        }
        else
        {
            // printf("[data_offset] %p %d\n", DATA_OFFSET(e.cluster_num), e.cluster_num);
            char content[e.file_size + 512];
            uint8_t* offset = DATA_OFFSET(e.cluster_num);
            memcpy(content, offset, bpb.num_of_bytes_per_sector);
            int i = 0;
            while (step_buffer(&offset, bpb.num_of_bytes_per_sector) != -1)
            {
                char* dst = &content[0] + (i + 1) * bpb.num_of_bytes_per_sector;
                memcpy(dst, offset, bpb.num_of_bytes_per_sector);
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
    
    return fat;
}


int main()
{
    setlocale(LC_ALL, "en_US.UTF-8");
    
    // printf("hello fs\n");
    int fd = open("disk.img", O_RDWR);
    if (fd == -1)
    {
        printf("open failed! %s\n", strerror(errno));
        return -1;
    }

    const int size = 1024 * 1000 * 1000;
    file_begin = malloc(sizeof(uint8_t) * size);
    int res = read(fd, file_begin, size);
    if (res == -1)
    {
        printf("read failed! %s\n", strerror(errno));
        return -1;
    }
    
    bpb = parse_bpb(file_begin);
    ebr = parse_extend_boot_record(&file_begin[0] + 36);

    int i = bpb.num_of_reserved_sectors * bpb.num_of_bytes_per_sector;
    fat_offset = &file_begin[i];
    printf("fat_offset: %x\n", i);
    fat = parse_fat(fat_offset);
    printf("fat_size: %d\n", fat->size);

    // jump to directory entry
    int j = ebr.sectors_per_fat * bpb.num_of_fat * bpb.num_of_bytes_per_sector + i;
    dir_offset = &file_begin[j];
    printf("dir_offset: %x\n", j);

    MyDirEntry root;
    root.cluster_num = 2;
    showDirEntry(root);
    walk_fs(root);

    // write file content
    // char data2[] = "123456789";
    // lseek(fd, dir_offset + data_offset, SEEK_SET);
    // res = write(fd, data2, sizeof(data2));
    // if (res == -1)
    // {
    //     printf("write failed! %s\n", strerror(errno));        
    // }

    // // write file size
    // lseek(fd, dir_offset + 60, SEEK_SET);
    // uint8_t size[] = {10, 0, 0, 0};
    // res = write(fd, size, sizeof(size));
}



