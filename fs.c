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
typedef struct {
    uint8_t boot_cmd[3];
    uint8_t oem_identifier[8];
    int num_of_bytes_per_sector;
    int num_of_bytes_per_cluster;
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
    char file_name[512];
    uint8_t attr;
    uint8_t creation_time;
    int cluster_num;
    int file_size;
    int size;
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
// bpb contain fs meta data, fat is index of data
// should be easily access by any function
static BPB bpb;
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
    printf("sectors_per_fat: %d\n", bpb.sectors_per_fat);
}

void showDirEntry(DirEntry entry)
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

void utf16_to_utf8(char16_t str[], char res[], size_t len)
{
    mbstate_t ps;
    memset(&ps, 0, sizeof(ps));

    // char res[100] = {0};
    char* offset = &res[0];
    for (int i = 0; i < len; i++) 
    {
        char buf[4] = {0};
        c16rtomb(buf, str[i], &ps);
        memcpy(offset, buf, strlen(buf));
        offset += strlen(buf);
    }
}

// auto jump to next cluster, if step would across current cluster
int step_buffer(uint8_t** buffer, size_t n)
{
    int pos = (*buffer - dir_offset) % bpb.num_of_bytes_per_cluster;
    if (pos + n < 512)
    {
        *buffer += n;
        return 0;
    }
    else
    {
        int cluster_num = (*buffer - dir_offset) / bpb.num_of_bytes_per_cluster + 2;
        int v = fat->cluster_info[cluster_num];
        if (v < CLUSTER_END && v != CLUSTER_BAD)
        {
            *buffer = DATA_OFFSET(v);
            *buffer += n - (bpb.num_of_bytes_per_cluster - pos);
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
    bpb.num_of_bytes_per_cluster = bpb.num_of_bytes_per_sector * bpb.num_of_sectors_per_cluster;
    
    showBPB(bpb);
    return bpb;
}

void parse_lfn(uint8_t* buffer, char res[])
{
    // skip order byte
    buffer += 1;
    
    char16_t name[13] = {0};
    int j = 0;
    for (int i = 0; i < 5; i++)
    {
        if (buffer[0] != 0xff)
        {
            name[j] = little_endian_to_int(buffer, 2);
            j += 1;
        }
        buffer += 2;
    }
    // skip attr, long entry type, chekcusm
    buffer += 3;
    
    for (int i = 0; i < 6; i++)
    {
        if (buffer[0] != 0xff)
        {
            name[j] = little_endian_to_int(buffer, 2);
            j += 1;
        }
        buffer += 2;
    }
    // always zero
    buffer += 2;
    
    for (int i = 0; i < 2; i++)
    {
        if (buffer[0] != 0xff)
        {
            name[j] = little_endian_to_int(buffer, 2);
            j += 1;
        }
        buffer += 2;
    }

    utf16_to_utf8(name, res, j);
}

void parse_lfns(uint8_t* buffer, DirEntry* entry)
{
    char file_name[512] = {0};
    uint8_t b = buffer[0];
    uint8_t lfn_num = b & LFN_NUM_MASK;

    // lfn is stored in reverse order, make it hard to concat it together
    // by store all lfn location in a chian, we can easily parse and concat it from start
    int len = 1;
    uint8_t* lfn_chain[20];
    lfn_chain[0] = buffer;
    while (lfn_num > 1)
    {
        buffer += 32;
        lfn_num = buffer[0] & LFN_NUM_MASK;
        // printf("lfn_num: %d\n", lfn_num);
        lfn_chain[len] = buffer;
        len += 1;
    }
    
    for (int i = len - 1; i >= 0; i--)
    {
        char res[14] = {0};
        parse_lfn(lfn_chain[i], res); 
        strcat(file_name, res);
    }
    memcpy(entry->file_name, file_name, strlen(file_name));
    entry->size = (len + 1) * 32;
}

DirEntry parse_dir_entry(uint8_t* buffer)
{
    DirEntry entry = {
        .file_name = {0}
    };
    
    uint8_t attr = buffer[11];
    if (attr == 0x0f)
    {
        parse_lfns(buffer, &entry);
        step_buffer(&buffer, entry.size - 32);
        // since there is lfn, standard8.3 file name can be ignored
        step_buffer(&buffer, 11);
    }
    else
    {
        // standard8.3, parse file name
        int j = 0;
        for (int i = 0; i < 11; i++)
        {
            uint8_t c = buffer[0];
            // ignore spaces
            if (c != 0x20)
            {
                entry.file_name[j] = c;
                j += 1;
            }
            step_buffer(&buffer, 1);
        }    
    }
    
    entry.attr = buffer[0];
    // ignore create time for now
    buffer += 9;

    int high = little_endian_to_int(buffer, 2);
    buffer += 6;

    int low = little_endian_to_int(buffer, 2);
    buffer += 2;

    int cluster_num = (high << 16) + low;
    entry.cluster_num = cluster_num;
    entry.file_size = little_endian_to_int(buffer, 4);
    buffer += 4;
    return entry;
}

DirEntryList* parse_dir_entries(uint8_t* buffer, DirEntry parent)
{
    // flexible array member
    DirEntryList* es = malloc(sizeof(DirEntryList) + 100 * sizeof(DirEntry));
    while (!entry_end(buffer))
    {
        DirEntry entry = parse_dir_entry(buffer);
        showDirEntry(entry);
        step_buffer(&buffer, entry.size);
        
        es->entries[es->size] = entry;
        es->size += 1;
    }
    return es;
}

void walk_fs(DirEntry entry)
{
    uint8_t* offset = DATA_OFFSET(entry.cluster_num);
    DirEntryList* es =  parse_dir_entries(offset, entry);
    for (int i = 0; i < es->size; i++)
    {
        DirEntry e = es->entries[i];
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

    int i = bpb.num_of_reserved_sectors * bpb.num_of_bytes_per_sector;
    fat_offset = &file_begin[i];
    printf("fat_offset: %x\n", i);
    fat = parse_fat(fat_offset);
    printf("fat_size: %d\n", fat->size);

    // jump to directory entry
    int j = bpb.sectors_per_fat * bpb.num_of_fat * bpb.num_of_bytes_per_sector + i;
    dir_offset = &file_begin[j];
    printf("dir_offset: %x\n", j);

    DirEntry root;
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



