#include "filesys/filesys.h"
#include <debug.h>
#include <stdio.h>
#include <string.h>
#include "filesys/file.h"
#include "filesys/free-map.h"
#include "filesys/inode.h"
#include "filesys/directory.h"
#include "filesys/fat.h"
#include "devices/disk.h"
#include "threads/thread.h"

/* The disk that contains the file system. */
struct disk *filesys_disk;

static void do_format (void);

/* Initializes the file system module.
 * If FORMAT is true, reformats the file system. */
void
filesys_init (bool format) {
	filesys_disk = disk_get (0, 1);
	if (filesys_disk == NULL)
		PANIC ("hd0:1 (hdb) not present, file system initialization failed");

	inode_init ();

#ifdef EFILESYS
	fat_init ();

	if (format)
		do_format ();

	fat_open ();
	thread_current()->current_dir = dir_open_root();
#else
	/* Original FS */
	free_map_init ();

	if (format)
		do_format ();

	free_map_open ();
#endif
}

/* Shuts down the file system module, writing any unwritten data
 * to disk. */
void
filesys_done (void) {
	/* Original FS */
#ifdef EFILESYS
	close_all_inodes();
	fat_close ();
	// dir_close(thread_current()->current_dir);
#else
	free_map_close ();
#endif
}

/* Creates a file named NAME with the given INITIAL_SIZE.
 * Returns true if successful, false otherwise.
 * Fails if a file named NAME already exists,
 * or if internal memory allocation fails. */
bool
filesys_create (const char *name, off_t initial_size) {
	disk_sector_t inode_sector = 0;
	struct dir *dir;
	bool success;
	char *last = strrchr(name, '/');

#ifdef EFILESYS
	if (!get_parent_dir(name, &dir))
		return false;
	inode_sector = fat_create_chain(0);
	success = (dir != NULL
			&& inode_sector != 0
			&& inode_create (inode_sector, initial_size, true)
			&& dir_add (dir, last == NULL ? name : last + 1, inode_sector));
	if (!success && inode_sector != 0)
		fat_remove_chain (inode_sector, 0);
#else
	success = (dir != NULL
			&& free_map_allocate (1, &inode_sector)
			&& inode_create (inode_sector, initial_size, true)
			&& dir_add (dir, name, inode_sector));
	if (!success && inode_sector != 0)
		free_map_release (inode_sector, 1);
#endif
	dir_close (dir);
	return success;
}

/* Opens the file with the given NAME.
 * Returns the new file if successful or a null pointer
 * otherwise.
 * Fails if no file named NAME exists,
 * or if an internal memory allocation fails. */
struct file *
filesys_open (const char *name) {
	struct dir *dir;
	struct inode *inode = NULL, *tmp;
	struct dir_entry e;
	char *last = strrchr(name, '/');

	if (!get_parent_dir(name, &dir))
		return false;

	if (dir != NULL) {
		if (lookup (dir, last == NULL ? name : last + 1, &e, NULL)) {
			tmp = inode_open (e.inode_sector);
			if (tmp->data.is_symlink) {
				inode = inode_open(tmp->data.start);
				inode_close(tmp);
			} else
				inode = tmp;
		}
	}
	dir_close (dir);

	return file_open (inode);
}

/* Deletes the file named NAME.
 * Returns true if successful, false on failure.
 * Fails if no file named NAME exists,
 * or if an internal memory allocation fails. */
bool
filesys_remove (const char *name) {
	struct dir *dir;
	bool success;

	if (!get_parent_dir(name, &dir))
		return false;

	success = dir != NULL && dir_remove (dir, name);
	dir_close (dir);

	return success;
}

bool
filesys_create_symlink (const char *target, const char *linkpath) {
	struct dir *link_dir, *target_dir;
	char *link_last = strrchr(linkpath, '/'), *target_last = strrchr(target, '/');
	struct inode* target_inode, *link_inode;
	bool success;
	disk_sector_t inode_sector;
	struct inode_disk *disk_inode = NULL;

	if (!get_parent_dir(linkpath, &link_dir) || !get_parent_dir(target, &target_dir))
		return false;
	
	dir_lookup (target_dir, target_last == NULL ? target : target_last + 1, &target_inode);

	inode_sector = fat_create_chain(0);
	
	disk_inode = calloc (1, sizeof *disk_inode);
	disk_inode->is_symlink = true;
	disk_inode->start = target_inode->sector;
	disk_inode->magic = INODE_MAGIC;
	disk_write (filesys_disk, inode_sector, disk_inode);
	free(disk_inode);

	success = disk_inode && dir_add (link_dir, link_last == NULL ? linkpath : link_last + 1, inode_sector);
	dir_close(link_dir);
	dir_close(target_dir);
	return success;
}

/* Formats the file system. */
static void
do_format (void) {
	struct dir* curr;
	printf ("Formatting file system...");

#ifdef EFILESYS
	/* Create FAT and save it to the disk. */
	fat_create ();
	if (!dir_create (ROOT_DIR_SECTOR, 16, ROOT_DIR_SECTOR, NULL))
		PANIC ("root directory creation failed");
	fat_close ();
#else
	free_map_create ();
	if (!dir_create (ROOT_DIR_SECTOR, 16, ROOT_DIR_SECTOR, NULL))
		PANIC ("root directory creation failed");
	free_map_close ();
#endif

	printf ("done.\n");
}
