
#ifndef _FAT_FATFS_H_
#define _FAT_FATFS_H_

#include <sys/cdefs.h>
__BEGIN_DECLS

#include <kos/blockdev.h>

struct fatfs;
typedef struct fatfs fatfs_t;

fatfs_t *fat_fs_init(const char *mp, kos_blockdev_t *bd);
void fat_fs_shutdown(fatfs_t  *fs);

void read_fat_table(fatfs_t *fat, unsigned char **table); 
void write_fat_table(fatfs_t *fat, unsigned char *table); 

__END_DECLS

#endif /* _FAT_FATFS_H_ */
