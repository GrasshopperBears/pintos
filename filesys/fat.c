#include "filesys/fat.h"
#include "devices/disk.h"
#include "filesys/filesys.h"
#include "threads/malloc.h"
#include "threads/synch.h"
#include <stdio.h>
#include <string.h>
#include <bitmap.h>

/* Should be less than DISK_SECTOR_SIZE */
struct fat_boot {
	unsigned int magic;
	unsigned int sectors_per_cluster; /* Fixed to 1 */
	unsigned int total_sectors;
	unsigned int fat_start;
	unsigned int fat_sectors; /* Size of FAT in sectors. */
	unsigned int root_dir_cluster;
};

/* FAT FS */
struct fat_fs {
	struct fat_boot bs;
	unsigned int *fat;
	unsigned int fat_length;
	disk_sector_t data_start;
	cluster_t last_clst;
	struct lock write_lock;
	// bool *fat_map;
	struct bitmap *map;
};

static struct fat_fs *fat_fs;

void fat_boot_create (void);
void fat_fs_init (void);

void
fat_init (void) {
	fat_fs = calloc (1, sizeof (struct fat_fs));
	if (fat_fs == NULL)
		PANIC ("FAT init failed");

	// Read boot sector from the disk
	unsigned int *bounce = malloc (DISK_SECTOR_SIZE);
	if (bounce == NULL)
		PANIC ("FAT init failed");
	disk_read (filesys_disk, FAT_BOOT_SECTOR, bounce);
	memcpy (&fat_fs->bs, bounce, sizeof (fat_fs->bs));
	free (bounce);

	// Extract FAT info
	if (fat_fs->bs.magic != FAT_MAGIC)
		fat_boot_create ();
	fat_fs_init ();
}

void
fat_open (void) {
	fat_fs->fat = calloc (fat_fs->fat_length, sizeof (cluster_t));
	if (fat_fs->fat == NULL)
		PANIC ("FAT load failed");

	// Load FAT directly from the disk
	uint8_t *buffer = (uint8_t *) fat_fs->fat;
	off_t bytes_read = 0;
	off_t bytes_left = sizeof (fat_fs->fat);
	const off_t fat_size_in_bytes = fat_fs->fat_length * sizeof (cluster_t);
	for (unsigned i = 0; i < fat_fs->bs.fat_sectors; i++) {
		bytes_left = fat_size_in_bytes - bytes_read;
		if (bytes_left >= DISK_SECTOR_SIZE) {
			disk_read (filesys_disk, fat_fs->bs.fat_start + i,
			           buffer + bytes_read);
			bytes_read += DISK_SECTOR_SIZE;
		} else {
			uint8_t *bounce = malloc (DISK_SECTOR_SIZE);
			if (bounce == NULL)
				PANIC ("FAT load failed");
			disk_read (filesys_disk, fat_fs->bs.fat_start + i, bounce);
			memcpy (buffer + bytes_read, bounce, bytes_left);
			bytes_read += bytes_left;
			free (bounce);
		}
	}
}

void
fat_close (void) {
	// Write FAT boot sector
	uint8_t *bounce = calloc (1, DISK_SECTOR_SIZE);
	if (bounce == NULL)
		PANIC ("FAT close failed");
	memcpy (bounce, &fat_fs->bs, sizeof (fat_fs->bs));
	disk_write (filesys_disk, FAT_BOOT_SECTOR, bounce);
	free (bounce);

	// Write FAT directly to the disk
	uint8_t *buffer = (uint8_t *) fat_fs->fat;
	off_t bytes_wrote = 0;
	off_t bytes_left = sizeof (fat_fs->fat);
	const off_t fat_size_in_bytes = fat_fs->fat_length * sizeof (cluster_t);
	for (unsigned i = 0; i < fat_fs->bs.fat_sectors; i++) {
		bytes_left = fat_size_in_bytes - bytes_wrote;
		if (bytes_left >= DISK_SECTOR_SIZE) {
			disk_write (filesys_disk, fat_fs->bs.fat_start + i,
			            buffer + bytes_wrote);
			bytes_wrote += DISK_SECTOR_SIZE;
		} else {
			bounce = calloc (1, DISK_SECTOR_SIZE);
			if (bounce == NULL)
				PANIC ("FAT close failed");
			memcpy (bounce, buffer + bytes_wrote, bytes_left);
			disk_write (filesys_disk, fat_fs->bs.fat_start + i, bounce);
			bytes_wrote += bytes_left;
			free (bounce);
		}
	}
}

void
fat_create (void) {
	// Create FAT boot
	fat_boot_create ();
	fat_fs_init ();

	// Create FAT table
	fat_fs->fat = calloc (fat_fs->fat_length, sizeof (cluster_t));
	if (fat_fs->fat == NULL)
		PANIC ("FAT creation failed");

	// Set up ROOT_DIR_CLST
	fat_put (ROOT_DIR_CLUSTER, EOChain);

	// Set up fat as chain, and EOChain empty block
	for (cluster_t i = 2; i < fat_fs->fat_length; i++) {
		fat_put(i, i+1);
	}

	fat_put(fat_fs->fat_length, EOChain);

	for (cluster_t i = fat_fs->data_start; i <= fat_fs->last_clst; i++) {
		fat_put(i, EOChain);
	}

	// Set up fat_map
	// fat_fs->fat_map = calloc (fat_fs->bs.total_sectors - fat_fs->bs.fat_sectors - 1, sizeof (bool));

	// for (int i = fat_fs->data_start; i <= fat_fs->last_clst; i++) {
	// 	fat_fs->fat_map[i] = false;
	// }

	// print_fat();

	// for (int i = fat_fs->data_start; i <= fat_fs->data_start + 20; i++) {
	// 	printf("%d ", i);
	// 	printf(fat_fs->fat_map[i] ? "true\n" : "false\n");
	// }
	
	//bitamp set up
	bitmap_set_all(fat_fs->map, false);

	// fat_create_chain(0);
	// fat_create_chain(158);
	// fat_create_chain(159);
	// fat_create_chain(160);
	// fat_create_chain(0);
	// fat_create_chain(162);

	// for (int i = 5; i < 300; i++) {
	// 	bitmap_mark (fat_fs->map, i);
	// }
	
	// for (int i = 0; i < 1000; i++) {
	// 	printf("%d", bitmap_test (fat_fs->map, i));
	// }
	// print_fat();
	// print_map();

	// printf("remove chain \n");

	// fat_remove_chain(158, 0);
	// print_fat();
	// print_map();

	// Fill up ROOT_DIR_CLUSTER region with 0
	uint8_t *buf = calloc (1, DISK_SECTOR_SIZE);
	if (buf == NULL)
		PANIC ("FAT create failed due to OOM");
	disk_write (filesys_disk, cluster_to_sector (ROOT_DIR_CLUSTER), buf);
	free (buf);
}

void
fat_boot_create (void) {
	unsigned int fat_sectors =
	    (disk_size (filesys_disk) - 1)
	    / (DISK_SECTOR_SIZE / sizeof (cluster_t) * SECTORS_PER_CLUSTER + 1) + 1;
	fat_fs->bs = (struct fat_boot){
	    .magic = FAT_MAGIC,
	    .sectors_per_cluster = SECTORS_PER_CLUSTER,
	    .total_sectors = disk_size (filesys_disk),
	    .fat_start = 1,
	    .fat_sectors = fat_sectors,
	    .root_dir_cluster = ROOT_DIR_CLUSTER,
	};
}

void
fat_fs_init (void) {
	/* TODO: Your code goes here. */

	//fat_fs->fat = NULL;
	fat_fs->fat_length = fat_fs->bs.fat_sectors;
	fat_fs->data_start = fat_fs->bs.fat_start + fat_fs->bs.fat_sectors;
	fat_fs->last_clst = fat_fs->bs.total_sectors - 1;
	// fat_fs->fat_map = NULL;
	fat_fs->map = bitmap_create(disk_size (filesys_disk));
	lock_init(&fat_fs->write_lock);
}

/*----------------------------------------------------------------------------*/
/* FAT handling                                                               */
/*----------------------------------------------------------------------------*/

cluster_t
fat_get_free (void) {

	// for (int i = fat_fs->data_start; i <= fat_fs->last_clst; i++) {
	// 	if (fat_fs->fat_map[i] == false)
	// 		return (cluster_t) i;
	// }

	for (int i = fat_fs->data_start; i <= fat_fs->last_clst; i++) {
		if (bitmap_test(fat_fs->map, i) == 0) {
			// printf("fat get free ");
			// printf("%d\n", i);
			return (cluster_t) i;
		}
	}

	return NULL;
}

/* Add a cluster to the chain.
 * If CLST is 0, start a new chain.
 * Returns 0 if fails to allocate a new cluster. */
cluster_t
fat_create_chain (cluster_t clst) {
	/* TODO: Your code goes here. */

	cluster_t new = fat_get_free ();

		if (fat_get_free == NULL)
			return 0;

	if (clst == 0) {
		
		// fat_fs->fat_map[new] = true;
		// printf("bitmap mark : %d", new);
		bitmap_mark(fat_fs->map, (int) new);

		// cluster_t new_next = fat_get_free ();
		// if (new_next != NULL)
		// 	fat_put(new, new_next);

		return new;
	}
	else {

		// fat_fs->fat_map[new] = true;
		
		bitmap_mark(fat_fs->map, (int) new);
		fat_put(clst, new);

		return new;
	}
}

/* Remove the chain of clusters starting from CLST.
 * If PCLST is 0, assume CLST as the start of the chain. */
void
fat_remove_chain (cluster_t clst, cluster_t pclst) {
	/* TODO: Your code goes here. */
	cluster_t cur;
	cluster_t next;

	if (bitmap_test(fat_fs->map, (int) clst) == 0)
		return;

	if (pclst == 0) {		
		cur = clst;

		while (cur != EOChain) {
			next = fat_get(cur);
			fat_put(cur, EOChain);
			// fat_fs->fat_map[cur] = false;
			bitmap_reset(fat_fs->map, (int) cur);
			cur = next;
		}
	}
	else {
		cur = pclst;

		while (cur != EOChain) {
			next = fat_get(cur);
			fat_put(cur, EOChain);
			// fat_fs->fat_map[cur] = false;
			
			bitmap_reset(fat_fs->map, (int) cur);
			cur = next;			
		}
	}
}

/* Update a value in the FAT table. */
void
fat_put (cluster_t clst, cluster_t val) {
	/* TODO: Your code goes here. */

	fat_fs->fat[clst] = val;
}

/* Fetch a value in the FAT table. */
cluster_t
fat_get (cluster_t clst) {
	/* TODO: Your code goes here. */

	return fat_fs->fat[clst];
}

/* Covert a cluster # to a sector number. */
disk_sector_t
cluster_to_sector (cluster_t clst) {
	/* TODO: Your code goes here. */

	return (disk_sector_t) clst;
}

void print_fat (void) {
	
	for (cluster_t i = 0; i < fat_fs->fat_length + 50; i++) {
		printf("%d %d\n", i, fat_get(i));
	}
}

void print_map (void) {
	
	for (cluster_t i = fat_fs->data_start; i < fat_fs->data_start + 50; i++) {
		printf("%d %d\n", i, bitmap_test(fat_fs->map, (int) i));
	}
}