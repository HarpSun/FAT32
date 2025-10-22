/*

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



/*
FAT structure
*/
// BIOS Parameter Block
typedef struct {
    uint8_t boot_cmd[3];
    uint8_t oem_identifier[8];
    int num_of_bytes_per_sector;
    int num_of_sectors_per_cluster;
    int num_of_reserved_sectors;
    int num_of_fat;
    int num_of_root_dir_entries;
    int total_sectors;
    uint8_t media_descriptor_type;
    int num_of_sectors_per_fat;  // FAT12/FAT16 only
    int num_of_sectors_per_track;
    int num_of_heads;
    int num_of_hidden_sectors;
    int large_sectors_count;
    int sectors_per_fat;
} BPB;


typedef struct {
    int size;
    int cluster[];
} ClusterChain;
    
typedef struct {
    char file_name[32];
    uint8_t attr;
    uint8_t creation_time;
    int cluster_num;
    int file_size;
    ClusterChain* cluster_chain;
} DirEntry;

typedef struct {
    int size;
    DirEntry entries[];
} DirEntryList;

typedef struct {
    int size;
    int cluster_info[];
} FAT;


// anchor point
// easy to make navigation through file buffer
static uint8_t* file_begin;
static uint8_t* fat_offset;
static uint8_t* dir_offset;
// bpb contain fs meta data, should be easily access by any function
static BPB bpb;


// root entry's cluster_num is 2
#define DATA_OFFSET(cluster_num) \
    ((cluster_num) - 2) * bpb.num_of_sectors_per_cluster * bpb.num_of_bytes_per_sector

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
    printf("sectors_per_fat: %d\n", bpb.sectors_per_fat);
}

void showDirEntry(DirEntry entry)
{
    printf("--------------- DirEntry ---------------\n");
    printf("file_name: %s\n", entry.file_name);
    printf("file_size: %d\n", entry.file_size);
    printf("cluster_num: %d\n", entry.cluster_num);
    printf("cluster_len: %d\n", entry.cluster_chain->size);
    for (int i = 0; i < entry.cluster_chain->size; i++)
    {
        printf("cluster_num: %d\n", entry.cluster_chain->cluster[i]);
    }
}

int little_endian_to_int(uint8_t* bytes, size_t len)
{
    int res = 0;
    for (int i = 0; i < len; i++)
    {
        res += bytes[i] << (i * 8);
    }
    return res;
}

bool entry_end(uint8_t* buffer)
{
    return buffer[0] == 0;
}

int min(int a, int b)
{
    return a > b ? b : a;
}


BPB parse_bpb(uint8_t* buffer)
{
    BPB bpb;

    memcpy(bpb.boot_cmd, buffer, 3);
    buffer += 3;
    
    memcpy(bpb.oem_identifier, buffer, 8);
    buffer += 8;

    bpb.num_of_bytes_per_sector = little_endian_to_int(buffer, 2);
    buffer += 2;

    bpb.num_of_sectors_per_cluster = buffer[0];
    buffer += 1;

    bpb.num_of_reserved_sectors = little_endian_to_int(buffer, 2);
    buffer += 2;

    bpb.num_of_fat = buffer[0];
    buffer += 1;

    buffer += 19;
    bpb.sectors_per_fat = little_endian_to_int(buffer, 4);
    
    showBPB(bpb);
    return bpb;
}

ClusterChain* parse_cluster_chain(int cluster_num, FAT* fat)
{
    int v = fat->cluster_info[cluster_num];
    // v >= 0x0ffffff8: cluster end
    // v == 0x0ffffff7: bad_cluster
    // otherwise:       cluster_num
    int chain[1024];
    int len = 0;
    while (v < 0x0ffffff8 && v != 0x0ffffff7)
    {
        chain[len] = v;
        len += 1;
        v = fat->cluster_info[v];
        // printf("cluster index: %x\n", v);
    }

    ClusterChain* c = malloc(sizeof(ClusterChain) + sizeof(int) * len);
    c->size = len;
    memcpy(c->cluster, chain, len * sizeof(int));
    return c;
}

DirEntry parse_dir_entry(uint8_t* buffer, FAT* fat)
{
    // ignore long file name for now
    buffer += 32;

    DirEntry entry;
    memcpy(entry.file_name, buffer, 11);
    buffer += 11;

    entry.attr = buffer[0];
    // ignore create time for now
    buffer += 9;

    int high = little_endian_to_int(buffer, 2);
    buffer += 6;

    int low = little_endian_to_int(buffer, 2);
    buffer += 2;

    int cluster_num = (high << 16) + low;
    entry.cluster_num = cluster_num;
    
    entry.cluster_chain = parse_cluster_chain(cluster_num, fat);
    
    entry.file_size = little_endian_to_int(buffer, 4);
    buffer += 4;
    return entry;
}

DirEntryList* parse_dir_entries(uint8_t* buffer, FAT* fat, DirEntry parent)
{
    // flexible array member
    DirEntryList* es = malloc(sizeof(DirEntryList) + 100 * sizeof(DirEntry));
    int left_cluster = parent.cluster_chain->size;
    int i = 0;
    while (!entry_end(buffer))
    {
        DirEntry entry = parse_dir_entry(buffer, fat);
        showDirEntry(entry);
        buffer += 64;
        es->entries[es->size] = entry;
        es->size += 1;

        // 512 / 64 = 8, so one cluster can contain 8 entries
        if (es->size % 8 == 0 && left_cluster > 0)
        {
            // buffer jump to new cluster
            int n = parent.cluster_chain->cluster[i];
            buffer = dir_offset + DATA_OFFSET(n);
            i += 1;
            left_cluster -= 1;
        }
    }
    return es;
}

// pass entry directly?
void show_dir_entry(DirEntryList* es, FAT* fat)
{
    // DirEntryList* es =  parse_dir_entries(offset2);
    for (int i = 0; i < es->size; i++)
    {
        DirEntry e = es->entries[i];
        if (IS_DIR(e))
        {
            // printf("[folder_data_offset] %x %d\n", (int)dir_offset + DATA_OFFSET(e, bpb), e.cluster_num);
            uint8_t* offset = dir_offset + DATA_OFFSET(e.cluster_num);
            // jump over . and .. entry
            offset += 64;
            show_dir_entry(parse_dir_entries(offset, fat, e), fat);
        }
        else
        {
            // printf("[data_offset] %d %d\n", DATA_OFFSET(e.cluster_num), e.cluster_num);
            int size = (1 + e.cluster_chain->size) * bpb.num_of_bytes_per_sector;
            char content[size];
            uint8_t* offset = dir_offset + DATA_OFFSET(e.cluster_num);
            int left_cluster = e.cluster_chain->size;
            memcpy(content, offset, bpb.num_of_bytes_per_sector);
            int i = 0;
            while (left_cluster > 0)
            {
                int n = e.cluster_chain->cluster[i];
                offset = dir_offset + DATA_OFFSET(n);
                char* dst = &content[0] + (i + 1) * bpb.num_of_bytes_per_sector;
                memcpy(dst, offset, bpb.num_of_bytes_per_sector);
                
                i += 1;
                left_cluster -= 1;
            }
            
            printf("content: %s\n", content);
        }
    }
}

FAT* parse_fat(uint8_t* buffer)
{
    // FAT is index of clusters, it's an array structure, every index is 4 bytes wide
    // index 0 and 1 are reserved, so root entry starts at index(cluster) 2
    int len = (bpb.num_of_fat * bpb.sectors_per_fat * bpb.num_of_bytes_per_sector) / 4;
    FAT* fat = malloc(sizeof(FAT) + len * sizeof(int));
    fat->size = 0;

    int v;
    while (v = little_endian_to_int(buffer, 4), v != 0)
    {
        fat->cluster_info[fat->size] = v;
        fat->size += 1;
        buffer += 4;
    }
    return fat;
}


int main()
{
    // printf("hello fs\n");
    int fd = open("disk.img", O_RDWR);
    if (fd == -1)
    {
        printf("open failed! %s\n", strerror(errno));
    }

    const int size = 1024 * 1000 * 1000;
    file_begin = malloc(sizeof(uint8_t) * size);
    int res = read(fd, file_begin, size);
    if (res == -1)
    {
        printf("read failed! %s\n", strerror(errno));
    }
    
    bpb = parse_bpb(file_begin);

    int i = bpb.num_of_reserved_sectors * bpb.num_of_bytes_per_sector;
    fat_offset = &file_begin[i];
    printf("fat_offset: %x\n", i);
    FAT* fat = parse_fat(fat_offset);
    printf("fat_size: %d\n", fat->size);

    // jump to directory entry
    int j = bpb.sectors_per_fat * bpb.num_of_fat * bpb.num_of_bytes_per_sector + i;
    dir_offset = &file_begin[j];
    printf("dir_offset: %x\n", j);

    DirEntry root;
    root.cluster_num = 2;
    root.cluster_chain = parse_cluster_chain(root.cluster_num, fat);
    showDirEntry(root);
    DirEntryList* es =  parse_dir_entries(dir_offset, fat, root);
    show_dir_entry(es, fat);
    

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



