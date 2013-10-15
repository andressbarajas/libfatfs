

#include <errno.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/queue.h>

#include <kos/fs.h>
#include <kos/mutex.h>

#include "include/fs_fat.h"

#include "fatfs.h"
#include "utils.h"
#include "fat_defs.h"
#include "dir_entry.h"

#define MAX_FAT_FILES 16

typedef struct fs_fat_fs {
    LIST_ENTRY(fs_fat_fs) entry;

    vfs_handler_t *vfsh;
    fatfs_t *fs;
    uint32_t mount_flags;
} fs_fat_fs_t;

LIST_HEAD(fat_list, fs_fat_fs);

/* Global list of mounted FAT16/FAT32 partitions */
static struct fat_list fat_fses;

/* Mutex for file handles */
static mutex_t fat_mutex;

/* File handles */
static struct {
    int           used;       /* 0 - Not Used, 1 - Used */
    int           mode;       /* O_RDONLY, O_WRONLY, O_RDWR, O_TRUNC, O_DIR, etc */
    uint32        ptr;        /* Current read position in bytes */
    dirent_t      dirent;     /* A static dirent to pass back to clients */
    node_entry_t  *node;	  /* Link to node in directory (binary) tree */
    node_entry_t  *dir;       /* Used by opendir */
    fs_fat_fs_t   *mnt;       /* Which mount instance are we using? */
} fh[MAX_FAT_FILES];

/* Open a file or directory */
static void *fs_fat_open(vfs_handler_t *vfs, const char *fn, int mode) {
    file_t fd;
    char *ufn = NULL; 
    fs_fat_fs_t *mnt = (fs_fat_fs_t *)vfs->privdata;
    node_entry_t *found = NULL;

    /* Make sure if we're going to be writing to the file that the fs is mounted
       read/write. */
    if((mode & (O_TRUNC | O_WRONLY | O_RDWR)) &&
       !(mnt->mount_flags & FS_FAT_MOUNT_READWRITE)) {
        errno = EROFS;
        return NULL;
    }
	
	/* Make sure to add the root directory to the fn */
	ufn = malloc(strlen(fn)+strlen(mnt->fs->mount)+1); 
    memset(ufn, 0, strlen(fn)+strlen(mnt->fs->mount)+1);    

    strcat(ufn, mnt->fs->mount);
    strcat(ufn, fn);

#if defined(FATFS_CACHEALL) 
    /* Find the object in question */
    found = fat_search_by_path(mnt->fs->root, ufn);
#else
	found = fat_search_by_path(mnt->fs, ufn);
#endif

    /* Handle a few errors */
    if(found == NULL && !(mode & O_CREAT)) {
        errno = ENOENT;
		free(ufn);
        return NULL;
    }
    else if(found != NULL && (mode & O_CREAT) && (mode & O_EXCL)) {
        errno = EEXIST;
		free(ufn);
        return NULL;
    }
    else if(found == NULL && (mode & O_CREAT)) {
#if defined(FATFS_CACHEALL) 
        found = create_entry(mnt->fs, mnt->fs->root, ufn, ARCHIVE);
#else
		found = create_entry(mnt->fs, ufn, ARCHIVE);
#endif
        if(found == NULL)
		{
			free(ufn);
            return NULL;
		}
    }
    else if(found != NULL && (found->Attr & READ_ONLY) && ((mode & O_WRONLY) || (mode & O_RDWR))) 
    {
		errno = EROFS;
		free(ufn);
		return NULL;
    }
    
    /* Set filesize to 0 if we set mode to O_TRUNC */
    if((mode & O_TRUNC) && ((mode & O_WRONLY) || (mode & O_RDWR)))
    {
        found->FileSize = 0;
        delete_cluster_list(mnt->fs, found);
		update_fat_entry(mnt->fs, found);
    }

    /* Find a free file handle */
    mutex_lock(&fat_mutex);

    for(fd = 0; fd < MAX_FAT_FILES; ++fd) {
        if(fh[fd].used == 0) {
            fh[fd].used = 1;
            break;
        }
    }

    if(fd >= MAX_FAT_FILES) {
        errno = ENFILE;
		free(ufn);
        mutex_unlock(&fat_mutex);
        return NULL;
    }

    /* Make sure we're not trying to open a directory for writing */
    if((found->Attr & DIRECTORY) && (mode & (O_WRONLY | O_RDWR))) {
        errno = EISDIR;
		free(ufn);
        mutex_unlock(&fat_mutex);
        return NULL;
    }

    /* Make sure if we're trying to open a directory that we have a directory */
    if((mode & O_DIR) && !(found->Attr & DIRECTORY)) {
        errno = ENOTDIR;
		free(ufn);
        mutex_unlock(&fat_mutex);
        return NULL;
    }

    /* Fill in the rest of the handle */
    fh[fd].mode = mode;
    fh[fd].ptr = 0;
    fh[fd].mnt = mnt;
    fh[fd].node = found;
    fh[fd].dir = NULL;

    mutex_unlock(&fat_mutex);
	
	free(ufn);

    return (void *)(fd + 1);
}

static int fs_fat_close(void * h) {
    file_t fd = ((file_t)h) - 1;

    mutex_lock(&fat_mutex);

    if(fd < MAX_FAT_FILES && fh[fd].mode) {
        fh[fd].used = 0;
        fh[fd].ptr = 0;
		fh[fd].mode = 0;
    }
	
#if !defined(FATFS_CACHEALL) /* Running default. Need to delete(free) node since its not part of a directory tree */
	delete_tree_entry(fh[fd].node);
	
	if(fh[fd].dir != NULL)
	{
		delete_tree_entry(fh[fd].dir);
	}
#endif

    mutex_unlock(&fat_mutex);

    return 0;
}

static ssize_t fs_fat_read(void *h, void *buf, size_t cnt) {
    file_t fd = ((file_t)h) - 1;
    fatfs_t *fs;
    unsigned char *block = NULL;
    unsigned char *bbuf = (unsigned char *)buf;
    ssize_t rv;

    mutex_lock(&fat_mutex);

    /* Check that the fd is valid */
    if(fd >= MAX_FAT_FILES || !fh[fd].used) {
        mutex_unlock(&fat_mutex);
        errno = EBADF;
        return -1;
    }

    /* Check and make sure it is not a directory */
    if(fh[fd].mode & O_DIR) {
        mutex_unlock(&fat_mutex);
        errno = EISDIR;
        return -1;
    }
    
    /* Check and make sure it is opened for reading */
    if(fh[fd].mode & O_WRONLY) {
        mutex_unlock(&fat_mutex);
        errno = EBADF;
        return -1;
    }

    /* Do we have enough left? */
    if((fh[fd].ptr + cnt) > fh[fd].node->FileSize)
    {
        cnt = fh[fd].node->FileSize - fh[fd].ptr;
    }

    fs = fh[fd].mnt->fs;
    rv = (ssize_t)cnt;
	
	/*
	printf("Number of bytes to read: %d\n", cnt);
	printf("Filesize: %d\n", fh[fd].node->FileSize);
	printf("Gonna read %d bytes starting at ptr %d\n", (int)rv, (int)fh[fd].ptr);
	*/
    if(!(block = fat_read_data(fs, fh[fd].node, (int)cnt, fh[fd].ptr))) { 
		if(block != NULL)
			free(block);
        mutex_unlock(&fat_mutex);
        errno = EBADF;
        return -1;
    }
	/*
	printf("After reading data\n");
	*/

    memcpy(bbuf, block, cnt);
    bbuf[cnt] = '\0';
    fh[fd].ptr += cnt;
	/*
	printf("After copying data and incrementing pointer\n");
*/
    /* We're done, clean up and return. */
    mutex_unlock(&fat_mutex);
    /*
	printf("Before freeing block\n");
*/
    free(block);
/*
	printf("After freeing block\n");
*/

    return rv;
}

static ssize_t fs_fat_write(void *h, const void *buf, size_t cnt)
{
    file_t fd = ((file_t)h) - 1;
    fatfs_t *fs;
    unsigned char *bbuf = NULL;
    ssize_t rv;

    mutex_lock(&fat_mutex);

    /* Check that the fd is valid */
    if(fd >= MAX_FAT_FILES || !fh[fd].used || (fh[fd].mode & O_DIR)) {
        mutex_unlock(&fat_mutex);
        errno = EBADF;
        return -1;
    }
    
    /* Check and make sure it is opened for Writing */
    if(fh[fd].mode & O_RDONLY) {
        mutex_unlock(&fat_mutex);
        errno = EBADF;
        return -1;
    }
    
    fs = fh[fd].mnt->fs;
    rv = (ssize_t)cnt;

    /* Copy the bytes we want to write */
	bbuf = malloc(sizeof(unsigned char)*(cnt+1)); 
    strncpy(bbuf, buf, cnt);
    bbuf[cnt] = '\0';
    
    /* If we set mode to O_APPEND, then make sure we write to end of file */
    if(fh[fd].mode & O_APPEND)
    {
        fh[fd].ptr = fh[fd].node->FileSize;
    }
	
    if(!fat_write_data(fs, fh[fd].node, bbuf, cnt, fh[fd].ptr)) {
        mutex_unlock(&fat_mutex);
		free(bbuf);
        errno = EBADF;
        return -1;
    }

    fh[fd].ptr += cnt;
    fh[fd].node->FileSize = (fh[fd].ptr > fh[fd].node->FileSize) ? fh[fd].ptr : fh[fd].node->FileSize; /* Increase the file size if need be(which ever is bigger) */

    /* Write it to the FAT */
    update_fat_entry(fs, fh[fd].node);

    mutex_unlock(&fat_mutex);
	
	free(bbuf);

    return rv;
}

static _off64_t fs_fat_seek64(void *h, _off64_t offset, int whence) {
    file_t fd = ((file_t)h) - 1;
    _off64_t rv;
	/*
	printf("Seek function is called\n");
*/
    mutex_lock(&fat_mutex);

    /* Check that the fd is valid */
    if(fd >= MAX_FAT_FILES || !fh[fd].used || (fh[fd].mode & O_DIR)) {
        mutex_unlock(&fat_mutex);
        errno = EBADF;
        return -1;
    }

    /* Update current position according to arguments */
    switch(whence) {
        case SEEK_SET:
            fh[fd].ptr = offset;
            break;

        case SEEK_CUR:
            fh[fd].ptr += offset;
            break;

        case SEEK_END:
            fh[fd].ptr = fh[fd].node->FileSize + offset;
            break;

        default:
            mutex_unlock(&fat_mutex);
	    errno = EINVAL;
            return -1;
    }

    rv =  (_off64_t)fh[fd].ptr;
    mutex_unlock(&fat_mutex);
	
    return rv;
}

static _off64_t fs_fat_tell64(void *h) {
    file_t fd = ((file_t)h) - 1;
    _off64_t rv;
	/*
	printf("Tell function is called\n");
    */
    mutex_lock(&fat_mutex);

    if(fd >= MAX_FAT_FILES || !fh[fd].used || (fh[fd].mode & O_DIR)) {
        mutex_unlock(&fat_mutex);
        errno = EINVAL;
        return -1;
    }

    rv = (_off64_t)fh[fd].ptr;

    mutex_unlock(&fat_mutex);
	
    return rv;
}

static uint64 fs_fat_total64(void *h) {
    file_t fd = ((file_t)h) - 1;
    size_t rv;
    /*
	printf("Total function is called\n");
	*/
    mutex_lock(&fat_mutex);

    if(fd >= MAX_FAT_FILES || !fh[fd].used || (fh[fd].mode & O_DIR)) {
        mutex_unlock(&fat_mutex);
        errno = EINVAL;
        return -1;
    }

    rv = fh[fd].node->FileSize;
    mutex_unlock(&fat_mutex);
	
    return rv;
}

static dirent_t *fs_fat_readdir(void *h) {
    file_t fd = ((file_t)h) - 1;

    mutex_lock(&fat_mutex);

    /* Check that the fd is valid */
    if(fd >= MAX_FAT_FILES || !fh[fd].used || !(fh[fd].mode & O_DIR)) {
        mutex_unlock(&fat_mutex);
        errno = EINVAL;
        return NULL;
    }

    /* Get the children of this folder if NULL */
    if(fh[fd].dir == NULL) 
    {
#if defined(FATFS_CACHEALL) 
        fh[fd].dir = fh[fd].node->Children;
#else
		fh[fd].dir = get_next_entry(fh[fd].mnt->fs, fh[fd].node, NULL);
#endif
    } 
    /* Move on to the next child */
    else  
    {
#if defined(FATFS_CACHEALL) 
        fh[fd].dir = fh[fd].dir->Next;
#else
		fh[fd].dir = get_next_entry(fh[fd].mnt->fs, fh[fd].node, fh[fd].dir);
#endif
    }
	
    /* Make sure we're not at the end of the directory */
    if(fh[fd].dir == NULL) {
        mutex_unlock(&fat_mutex);
        return NULL;
    }

    /* Fill in the static directory entry */
    fh[fd].dirent.size = fh[fd].dir->FileSize;
    memcpy(fh[fd].dirent.name, fh[fd].dir->Name, strlen(fh[fd].dir->Name));
    fh[fd].dirent.name[strlen(fh[fd].dir->Name)] = '\0';
    fh[fd].dirent.attr = fh[fd].dir->Attr;
    fh[fd].dirent.time = 0; /*inode->i_mtime; 
    fh[fd].ptr += dent->rec_len;*/

    mutex_unlock(&fat_mutex);

    return &fh[fd].dirent;
}

static int fs_fat_unlink(vfs_handler_t * vfs, const char *fn) {

	int i;
	node_entry_t *f = NULL;
	fs_fat_fs_t *mnt = (fs_fat_fs_t *)vfs->privdata;
	char *ufn = NULL;

	ufn = malloc(strlen(fn)+strlen(mnt->fs->mount)+1); 
	memset(ufn, 0, strlen(fn)+strlen(mnt->fs->mount)+1);    

    strcat(ufn, mnt->fs->mount);
    strcat(ufn, fn);

    mutex_lock(&fat_mutex);
	
#if defined(FATFS_CACHEALL) 
    /* Find the file */
    f = fat_search_by_path(mnt->fs->root, ufn);
#else
	f = fat_search_by_path(mnt->fs, ufn);
#endif
	
	free(ufn);

    if(f) {
        /* Make sure it's not in use */
		for(i=0;i<MAX_FAT_FILES; i++)
		{
			if(fh[i].used == 1 && fh[i].node == f)
			{
				errno = EBUSY;
				mutex_unlock(&fat_mutex);
				return -1;
			}
		}
		
		/* Make sure it isnt a directory(files only) */
		if(f->Attr & DIRECTORY)
		{
			errno = EISDIR;
			mutex_unlock(&fat_mutex);
			return -1;
		}
		
		/* Make sure its not Read Only */
		if(f->Attr & READ_ONLY)
		{
			errno = EROFS;
			mutex_unlock(&fat_mutex);
			return -1;
		}
       
		/* Remove it from SD card */
		delete_entry(mnt->fs, f);

		/* Remove it from directory tree */
		delete_tree_entry(f);
    }
	else /* Not found */
	{
		errno = ENOENT;
		mutex_unlock(&fat_mutex);
		return -1;
	}

    mutex_unlock(&fat_mutex);
    
	return 0;
}

static int fs_fat_mkdir(vfs_handler_t *vfs, const char *fn)
{
    char *ufn = NULL;
    fs_fat_fs_t *mnt = (fs_fat_fs_t *)vfs->privdata;
    node_entry_t *found = NULL;

	ufn = malloc(strlen(fn)+strlen(mnt->fs->mount)+1); 
    memset(ufn, 0, strlen(fn)+strlen(mnt->fs->mount)+1);    

    strcat(ufn, mnt->fs->mount);
    strcat(ufn, fn);

    /* Make sure there is a filename given */
    if(!fn) {
        errno = ENOENT;
		free(ufn);
        return -1;
    }

    /* Make sure the fs is writable */
    if(!(mnt->mount_flags & FS_FAT_MOUNT_READWRITE)) {
        errno = EROFS;
		free(ufn);
        return -1;
    }

#if defined(FATFS_CACHEALL) 
    /* Make sure the folder doesnt already exist */
    found = fat_search_by_path(mnt->fs->root, ufn);
#else
	found = fat_search_by_path(mnt->fs, ufn);
#endif

    /* Handle a few errors */
    if(found != NULL) {
        errno = EEXIST;  
		free(ufn);
        return -1;
    }

#if defined(FATFS_CACHEALL) 
    found = create_entry(mnt->fs, mnt->fs->root, ufn, DIRECTORY);
#else
	found = create_entry(mnt->fs, ufn, DIRECTORY);
#endif
 
    if(found == NULL)
	{
		free(ufn);
		return -1;
	}

#if defined(FATFS_CACHEALL) 
    if(found->Parent->Parent != NULL) /* Update parent directories(access[change] time/date) */
		update_fat_entry(mnt->fs, found->Parent);
#endif
		
	free(ufn);

    return 0;
}

static int fs_fat_rmdir(vfs_handler_t *vfs, const char *fn)
{
	int i;
	node_entry_t *f = NULL;
	char *ufn = NULL;
	fs_fat_fs_t *mnt = (fs_fat_fs_t *)vfs->privdata;
	
	ufn = malloc(strlen(fn)+strlen(mnt->fs->mount)+1); 
	memset(ufn, 0, strlen(fn)+strlen(mnt->fs->mount)+1);    

    strcat(ufn, mnt->fs->mount);
    strcat(ufn, fn);

    mutex_lock(&fat_mutex);

#if defined(FATFS_CACHEALL) 
    /* Find the folder */
    f = fat_search_by_path(mnt->fs->root, ufn);
#else
	f = fat_search_by_path(mnt->fs, ufn);
#endif
	
	free(ufn);

    if(f) {
        /* Make sure it's not in use */
		for(i=0;i<MAX_FAT_FILES; i++)
		{
			if(fh[i].used == 1 && fh[i].node == f)
			{
				errno = EBUSY;
				mutex_unlock(&fat_mutex);
				return -1;
			}
		}
		
		/* Make sure it isnt a file */
		if(f->Attr & ARCHIVE)
		{
			errno = ENOTDIR;
			mutex_unlock(&fat_mutex);
			return -1;
		}
		
		/* Make sure its not Read Only */
		if(f->Attr & READ_ONLY)
		{
			errno = EROFS;
			mutex_unlock(&fat_mutex);
			return -1;
		}
		
		/* Make sure this folder has no contents(children) */
#if defined(FATFS_CACHEALL) 
       if(f->Children != NULL) 
	   {
			errno = ENOTEMPTY;
			mutex_unlock(&fat_mutex);
			return -1;
	   }
#else
	   if(get_next_entry(mnt->fs, f, NULL) != NULL)
	   {
			errno = ENOTEMPTY;
			mutex_unlock(&fat_mutex);
			return -1;
	   }
#endif
	   
		/* Remove it from SD card */
		delete_entry(mnt->fs, f);

		/* Remove it from directory tree */
		delete_tree_entry(f);
    }
	else /* Not found */
	{
		errno = ENOENT;
		mutex_unlock(&fat_mutex);
		return -1;
	}

    mutex_unlock(&fat_mutex);
	
	return 0;
}

static int fs_fat_fcntl(void *h, int cmd, va_list ap) {
    file_t fd = ((file_t)h) - 1;
    int rv = -1;

    (void)ap;

    mutex_lock(&fat_mutex);

    if(fd >= MAX_FAT_FILES || !fh[fd].used) {
        mutex_unlock(&fat_mutex);
        errno = EBADF;
        return -1;
    }

    switch(cmd) {
        case F_GETFL:
            rv = fh[fd].mode;
            break;

        case F_SETFL:
        case F_GETFD:
        case F_SETFD:
            rv = 0;
            break;

        default:
            errno = EINVAL;
    }

    mutex_unlock(&fat_mutex);
    return rv;
}

/* This is a template that will be used for each mount */
static vfs_handler_t vh = {
    /* Name Handler */
    {
        { 0 },                  /* name */
        0,                      /* in-kernel */
        0x00010000,             /* Version 1.0 */
        NMMGR_FLAGS_NEEDSFREE,  /* We malloc each VFS struct */
        NMMGR_TYPE_VFS,         /* VFS handler */
        NMMGR_LIST_INIT         /* list */
    },

    0, NULL,                   /* no cacheing, privdata */

    fs_fat_open,               /* open */
    fs_fat_close,              /* close */
    fs_fat_read,               /* read */
    fs_fat_write,              /* write */
    NULL,             		   /* seek */
    NULL,              		   /* tell */
    NULL,            		   /* total */
    fs_fat_readdir,            /* readdir */
    NULL,                      /* ioctl */
    NULL,                      /* rename */
    fs_fat_unlink,             /* unlink(delete a file) */
    NULL,                      /* mmap */
    NULL,                      /* complete */
    NULL,                      /* stat */
    fs_fat_mkdir,              /* mkdir */
    fs_fat_rmdir,              /* rmdir */
    fs_fat_fcntl,              /* fcntl */
    NULL,                      /* poll */
    NULL,                      /* link */
    NULL,                      /* symlink */
    fs_fat_seek64,             /* seek64 */
    fs_fat_tell64,             /* tell64 */
    fs_fat_total64,            /* total64 */
    NULL                       /* readlink */
};

static int initted = 0;

/* These two functions borrow heavily from the same functions in fs_romdisk */
int fs_fat_mount(const char *mp, kos_blockdev_t *dev, uint32_t flags) {
    fatfs_t *fs;
    fs_fat_fs_t *mnt;
    vfs_handler_t *vfsh;

    if(!initted)
        return -1;

    mutex_lock(&fat_mutex);

    /* Try to initialize the filesystem */
    if(!(fs = fat_fs_init(mp, dev))) {
        mutex_unlock(&fat_mutex);
        printf("fs_fat: device does not contain a valid fatfs.\n");
        return -1;
    }

    /* Create a mount structure */
    if(!(mnt = (fs_fat_fs_t *)malloc(sizeof(fs_fat_fs_t)))) {
        printf("fs_fat: out of memory creating fs structure\n");
        fat_fs_shutdown(fs);
        mutex_unlock(&fat_mutex);
        return -1;
    }

    mnt->fs = fs;
    mnt->mount_flags = flags;

    /* Create a VFS structure */
    if(!(vfsh = (vfs_handler_t *)malloc(sizeof(vfs_handler_t)))) {
        printf("fs_fat: out of memory creating vfs handler\n");
        free(mnt);
        fat_fs_shutdown(fs);
        mutex_unlock(&fat_mutex);
        return -1;
    }

    memcpy(vfsh, &vh, sizeof(vfs_handler_t));
    strcpy(vfsh->nmmgr.pathname, mp);
    vfsh->privdata = mnt;
    mnt->vfsh = vfsh;

    /* Add it to our list */
    LIST_INSERT_HEAD(&fat_fses, mnt, entry);

    /* Register with the VFS */
    if(nmmgr_handler_add(&vfsh->nmmgr)) {
        printf("fs_fat: couldn't add fs to nmmgr\n");
        free(vfsh);
        free(mnt);
        fat_fs_shutdown(fs);
        mutex_unlock(&fat_mutex);
        return -1;
    }

    mutex_unlock(&fat_mutex);

    return 0;
}

int fs_fat_unmount(const char *mp) {
    fs_fat_fs_t *i;
    int found = 0, rv = 0;

    /* Find the fs in question */
    mutex_lock(&fat_mutex);

    LIST_FOREACH(i, &fat_fses, entry) {
        if(!strcasecmp(mp, i->vfsh->nmmgr.pathname)) {
            found = 1;
            break;
        }
    }

    if(found) {
	
#if defined(FATFS_CACHEALL) 
		/* Free the Directory tree fs->root */
		delete_directory_tree(i->fs->root);
#endif
		
		free(i->fs->mount); /* Free str mem */

        LIST_REMOVE(i, entry);

        /* XXXX: We should probably do something with open files... */
        nmmgr_handler_remove(&i->vfsh->nmmgr);
        free(i->vfsh);
        free(i);
    }
    else {
        errno = ENOENT;
        rv = -1;
    }

    mutex_unlock(&fat_mutex);
	
    return rv;
}

int fs_fat_init(void) {
    if(initted)
        return 0;

	/* Init our list of mounted entries */	
    LIST_INIT(&fat_fses);
	
	/* Reset fd's */
	memset(fh, 0, sizeof(fh));
	
	/* Init thread mutexes */
    mutex_init(&fat_mutex, MUTEX_TYPE_NORMAL);
	
    initted = 1;

    return 0;
}

int fs_fat_shutdown(void) {
    fs_fat_fs_t *i, *next;

    if(!initted)
        return 0;

    /* Clean up the mounted filesystems */
    i = LIST_FIRST(&fat_fses);
	
    while(i) {
        next = LIST_NEXT(i, entry);

        /* XXXX: We should probably do something with open files... */
        nmmgr_handler_remove(&i->vfsh->nmmgr);
        free(i->vfsh);
        free(i);

        i = next;
    }

    mutex_destroy(&fat_mutex);
    initted = 0;

    return 0;
}

int fat_partition(uint8 partition_type)
{
	if(partition_type == FAT16TYPE1
	|| partition_type == FAT16TYPE2
	|| partition_type == FAT32TYPE1
	|| partition_type == FAT32TYPE2 
	)
		return 1;
		
	return 0;
}
