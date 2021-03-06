
#ifndef _FAT_DIR_ENTRY_H_
#define _FAT_DIR_ENTRY_H_

__BEGIN_DECLS

#include "fat_defs.h"

/* List of attributes the following structures may take */
#define READ_ONLY    0x01 
#define HIDDEN       0x02 
#define SYSTEM       0x04 
#define VOLUME_ID    0x08 
#define DIRECTORY    0x10 
#define ARCHIVE      0x20
#define LONGFILENAME 0x0F

/* Shortname offsets */
#define FILENAME  0x00
#define EXTENSION 0x08
#define ATTRIBUTE 0x0B
#define RESERVED  0x0C
#define CREATIONTIME 0x0E
#define CREATIONDATE 0x10
#define LASTACCESSDATE 0x12
#define STARTCLUSTERHI 0x14
#define LASTWRITETIME 0x16
#define LASTWRITEDATE 0x18
#define STARTCLUSTERLOW 0x1A
#define FILESIZE  0x1C

/* Long file offsets */
#define ORDER     0x00
#define FNPART1   0x01
#define CHECKSUM  0x0D
#define FNPART2   0x0E
#define FNPART3   0x1C

/* FSInfo Sector offsets */
#define FREECOUNT 0x1E8
#define NEXTFREE  0x1EC

/* Other */
#define ENTRYSIZE 32       /* Size of an entry whether it be a lfn or just a regular file entry */
#define DELETED   0xE5     /* The first byte of a deleted entry */
#define EMPTY     0        /* Shows an empty entry */

typedef struct fatfs fatfs_t;

typedef struct fat_long_fn_dir_entry fat_lfn_entry_t;

/* FNPart1, FNPart2, and FNPart3 are unicode characters */
struct fat_long_fn_dir_entry 
{
	unsigned char Order;        /* The order of this entry in the sequence of long file name entries. There can be many of these entry stacked to make a really long filename 
	                               The specification says that the last entry value(Order) will be ORed with 0x40(01000000) and it is the mark for last entry
								   0x80 is marked when LFN entry is deleted
								*/
	unsigned char FNPart1[10];  /* The first 5, 2-byte characters of this entry. */
	unsigned char Attr;         /* Should only have the value 0x0F (Specifying that it is a long name entry and not an actual file entry) */
	unsigned char Res;          /* Reserved */
	unsigned char Checksum;     /* Checksum */
	unsigned char FNPart2[12];  /* The next 6, 2-byte characters of this entry. */
	unsigned short Cluster;     /* Unused. Always 0 */
	unsigned char FNPart3[4];   /* The final 2, 2-byte characters of this entry. */
} __attribute__((packed));

typedef struct fat_dir_entry fat_dir_entry_t;

struct fat_dir_entry 
{
    unsigned char FileName[9];  /* Represent the filename. The first character in array can hold special values. See http://www.tavi.co.uk/phobos/fat.html */
    unsigned char Ext[4];       /* Indicate the filename extension.  Note that the dot used to separate the filename and the filename extension is implied, and is not actually stored anywhere */
    unsigned char Attr;         /* Provides information about the file and its permissions. Hex numbers are bit offsets in this variable. 0x01 = 00000001
	                                                                                        0x01(0000 0001) - Read-Only
	                                                                                        0x02(0000 0002) - Hidden File
																							0x04(0000 0100) - System File 
																							0x08(0000 1000) - Contains the disk's volume label, instead of describing a file
																							0x10(0001 0000) - This is a subdirectory
																							0x20(0010 0000) - Archive flag. Set when file is modified
																							0x40(0100 0000) - Not used. Must be zero
																							0x80(1000 0000) - Not used. Must be zero
*/
    unsigned char Res;          /* Reserved : http://en.wikipedia.org/wiki/File_Allocation_Table#DIR_OFS_0Ch . Bit 4 [0x10] means lowercase extension and bit 3[0x08] lowercase basename, which allows for combinations such as "example.TXT" or "HELLO.txt" but not "Mixed.txt" */
    unsigned char CrtTimeTenth; /* Creation time in tenths of a second.  */
    unsigned short CrtTime;     /* Taken from http://www.tavi.co.uk/phobos/fat.html#file_time
	                               <------- 0x17 --------> <------- 0x16 -------->
								   15 14 13 12 11 10 09 08 07 06 05 04 03 02 01 00
									h  h  h  h  h  m  m  m  m  m  m  x  x  x  x  x
								   
							       hhhhh - Indicates the binary number of hours (0-23)
                                   mmmmmm - Indicates the binary number of minutes (0-59)
                                   xxxxx - Indicates the binary number of two-second periods (0-29), representing seconds 0 to 58.
                                */								   
    unsigned short CrtDate;     /* Taken from http://www.tavi.co.uk/phobos/fat.html#file_date
							   <------- 0x19 --------> <------- 0x18 -------->
									15 14 13 12 11 10 09 08 07 06 05 04 03 02 01 00
									y  y  y  y  y  y  y  m  m  m  m  d  d  d  d  d

                                        yyyyyyy - Indicates the binary year offset from 1980 (0-119), representing the years 1980 to 2099
                                        mmmm - Indicates the binary month number (1-12)
                                        ddddd - Indicates the binary day number (1-31) 
                                */	
    unsigned short LstAccDate;  /* Same format as CrtDate above */
    unsigned short FstClusHI;   /* The high 16 bits of this entry's first cluster number. For FAT 12 and FAT 16 this is always zero. */
    unsigned short WrtTime;     /* Same format as CrtTime above */
    unsigned short WrtDate;     /* Same format as CrtDate above */
    unsigned short FstClusLO;   /* The low 16 bits of this entry's first cluster number. Use this number to find the first cluster for this entry. */
    unsigned int FileSize;      /* The size of the file in bytes. This should be 0 if the file type is a folder */
};

typedef struct node_entry node_entry_t;

struct node_entry {
	unsigned char *Name;               /* Holds name of file/folder */
	unsigned char *ShortName;          /* Holds the short name (entry). No two files can have the same short name. Can be equal to Name. */
	unsigned char Attr;                /* Holds the attributes of entry */
	unsigned int FileSize;             /* Holds the size of the file */
	unsigned int Location[2];          /* Location in FAT Table. Location[0]: Sector, Location[1]: Byte in that sector */
	unsigned int StartCluster;		   /* First cluster that belongs to this file/folder */
	unsigned int EndCluster;		   /* The last cluster that belongs to this file/folder */
	
	unsigned int CurrCluster;          /* The current cluster that is being used by read/write (files only) */
	unsigned int NumCluster;           /* The number(space/spot) of the cluster	if a file had an array of cluster numbers */
};

/* Prototypes */
int generate_and_write_entry(fatfs_t *fat, char *filename, node_entry_t *newfile, node_entry_t *parent);

void delete_struct_entry(node_entry_t * node);
void delete_cluster_list(fatfs_t *fat, node_entry_t *file);

unsigned int allocate_cluster(fatfs_t *fat, unsigned int start_cluster);

void update_sd_entry(fatfs_t *fat, node_entry_t *file);
void delete_sd_entry(fatfs_t *fat, node_entry_t *file);

int fat_read_data(fatfs_t *fat, node_entry_t *file, unsigned char **buf, int cnt, int ptr);
int fat_write_data(fatfs_t *fat, node_entry_t *file, unsigned char *buf, int count, int ptr);

node_entry_t *fat_search_by_path(fatfs_t *fat, const char *fn);
node_entry_t *search_directory(fatfs_t *fat, node_entry_t *node, const char *fn);
node_entry_t *browse_sector(fatfs_t *fat, unsigned int sector_loc, unsigned int ptr, const char *fn);
node_entry_t *get_next_entry(fatfs_t *fat, node_entry_t *dir, node_entry_t *last_entry);
node_entry_t *create_entry(fatfs_t *fat, const char *fn, unsigned char attr);

__END_DECLS
#endif /* _FAT_DIR_ENTRY_H_ */
