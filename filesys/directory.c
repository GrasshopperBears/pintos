#include "filesys/directory.h"
#include <stdio.h>
#include <string.h>
#include <list.h>
#include "filesys/filesys.h"
#include "filesys/inode.h"
#include "threads/malloc.h"
#include "threads/thread.h"

/* Creates a directory with space for ENTRY_CNT entries in the
 * given SECTOR.  Returns true if successful, false on failure. */
bool
dir_create (disk_sector_t sector, size_t entry_cnt, disk_sector_t parent_sector, char *dir_name) {
	bool success = inode_create (sector, entry_cnt * sizeof (struct dir_entry), false);
	struct dir* parent;
	struct dir* curr = dir_open (inode_open (sector));
	dir_add(curr, ".", sector);
	dir_add(curr, "..", parent_sector);
	dir_close(curr);

	if (dir_name != NULL) {
		parent = dir_open (inode_open (parent_sector));
		dir_add(parent, dir_name, sector);
		dir_close(parent);
	}
	return success;
}

/* Opens and returns the directory for the given INODE, of which
 * it takes ownership.  Returns a null pointer on failure. */
struct dir *
dir_open (struct inode *inode) {
	// printf("dir open start\n");
	struct dir *dir = calloc (1, sizeof *dir);
	if (inode != NULL && dir != NULL) {
		dir->inode = inode;
		dir->pos = 0;
		return dir;
	} else {
		inode_close (inode);
		free (dir);
		return NULL;
	}
}

/* Opens the root directory and returns a directory for it.
 * Return true if successful, false on failure. */
struct dir *
dir_open_root (void) {
	return dir_open (inode_open (ROOT_DIR_SECTOR));
}

/* Opens and returns a new directory for the same inode as DIR.
 * Returns a null pointer on failure. */
struct dir *
dir_reopen (struct dir *dir) {
	return dir_open (inode_reopen (dir->inode));
}

/* Destroys DIR and frees associated resources. */
void
dir_close (struct dir *dir) {
	if (dir != NULL) {
		inode_close (dir->inode);
		free (dir);
	}
}

/* Returns the inode encapsulated by DIR. */
struct inode *
dir_get_inode (struct dir *dir) {
	return dir->inode;
}

bool
get_parent_dir(char *dir, struct dir **parent_dir) {
	bool success = false;
	char *cpy_parent_dir, *last = strrchr(dir, '/');
	int parent_dir_len = last - dir;
	struct inode *inode = NULL;
	struct dir_entry e;
	struct dir *current_dir = thread_current()->current_dir;

	if (last == dir) {
		*parent_dir = dir_open_root();
		return true;
	} else if (last == NULL) {
		*parent_dir = current_dir == NULL ? dir_open_root() : dir_reopen(current_dir);
		return true;
	}

	cpy_parent_dir = calloc(sizeof(char), parent_dir_len + 1);
	strlcpy(cpy_parent_dir, dir, parent_dir_len + 1);
	cpy_parent_dir[parent_dir_len] = '\0';

	if (lookup (current_dir, cpy_parent_dir, &e, NULL)) {
		*parent_dir = dir_open(inode_open (e.inode_sector));
		if (parent_dir != NULL)
			success = true;
	}
	
	free(cpy_parent_dir);
	return success;
}

/* Searches DIR for a file with the given NAME.
 * If successful, returns true, sets *EP to the directory entry
 * if EP is non-null, and sets *OFSP to the byte offset of the
 * directory entry if OFSP is non-null.
 * otherwise, returns false and ignores EP and OFSP. */
bool
lookup (const struct dir *dir, const char *name,
		struct dir_entry *ep, off_t *ofsp) {
	struct dir_entry e;
	size_t ofs, find_name_len;
	struct dir* curr_dir = dir_reopen(dir);
	char *slash_pos, *curr_pos, *find_name, *last = strrchr(name, '/');
	bool curr_success, found = false;
	int name_len = strlen(name);
	struct inode *inode, *symlink_inode, *real_data;

	ASSERT (dir != NULL);
	ASSERT (name != NULL);

	curr_pos = name;

	if (last != NULL && last == name && name_len == 1) {
		ep->inode_sector = ROOT_DIR_SECTOR;
		return true;
	}
	
	while (curr_pos != '\0' && curr_pos <= name + name_len) {
		slash_pos = strstr(curr_pos, "/");
		curr_success = false;

		if (slash_pos == curr_pos) {
			curr_dir = dir_open(inode_open(ROOT_DIR_SECTOR));
		} else {
			if (slash_pos == NULL) {
				find_name = curr_pos;
			} else {
				find_name_len = slash_pos - curr_pos;
				find_name = malloc(find_name_len + 1);
				strlcpy(find_name, curr_pos, find_name_len + 1);
				find_name[find_name_len] = '\0';
			}
			if (found) {
				dir_close(curr_dir);
				curr_dir = dir_open(inode_open(e.inode_sector));
			}
			for (ofs = 0; inode_read_at (curr_dir->inode, &e, sizeof e, ofs) == sizeof e; ofs += sizeof e) {
				if (e.in_use && !strcmp (find_name, e.name)) {
					if (ep != NULL)
						*ep = e;
					if (ofsp != NULL)
						*ofsp = ofs;
					curr_success = true;
					found = true;
					break;
				}
			}
			if (slash_pos != NULL)
				free(find_name);
			if (!curr_success) {		
				dir_close(curr_dir);
				return false;
			}
		}
		if (slash_pos == NULL)
			break;
		curr_pos = slash_pos + 1;
	}
	dir_close(curr_dir);
	return found;
}

/* Searches DIR for a file with the given NAME
 * and returns true if one exists, false otherwise.
 * On success, sets *INODE to an inode for the file, otherwise to
 * a null pointer.  The caller must close *INODE. */
bool
dir_lookup (const struct dir *dir, const char *name,
		struct inode **inode) {
	struct dir_entry e;
	struct inode *tmp;

	ASSERT (dir != NULL);
	ASSERT (name != NULL);

	if (lookup (dir, name, &e, NULL)) {
		tmp = inode_open (e.inode_sector);
		if (tmp->data.is_symlink) {
			*inode = inode_open(tmp->data.start);
			inode_close(tmp);
		} else
			*inode = tmp;
	}
	else
		*inode = NULL;

	return *inode != NULL;
}

/* Adds a file named NAME to DIR, which must not already contain a
 * file by that name.  The file's inode is in sector
 * INODE_SECTOR.
 * Returns true if successful, false on failure.
 * Fails if NAME is invalid (i.e. too long) or a disk or memory
 * error occurs. */
bool
dir_add (struct dir *dir, const char *name, disk_sector_t inode_sector) {
	struct dir_entry e;
	off_t ofs;
	bool success = false;

	ASSERT (dir != NULL);
	ASSERT (name != NULL);

	/* Check NAME for validity. */
	if (*name == '\0' || strlen (name) > NAME_MAX)
		return false;

	/* Check that NAME is not in use. */
	if (lookup (dir, name, NULL, NULL))
		goto done;

	/* Set OFS to offset of free slot.
	 * If there are no free slots, then it will be set to the
	 * current end-of-file.

	 * inode_read_at() will only return a short read at end of file.
	 * Otherwise, we'd need to verify that we didn't get a short
	 * read due to something intermittent such as low memory. */
	// printf("lookup passed\n");
	for (ofs = 0; inode_read_at (dir->inode, &e, sizeof e, ofs) == sizeof e;
			ofs += sizeof e)
		if (!e.in_use)
			break;

	/* Write slot. */
	e.in_use = true;
	strlcpy (e.name, name, sizeof e.name);
	e.inode_sector = inode_sector;
	success = inode_write_at (dir->inode, &e, sizeof e, ofs) == sizeof e;

done:
	return success;
}

/* Removes any entry for NAME in DIR.
 * Returns true if successful, false on failure,
 * which occurs only if there is no file with the given NAME. */
bool
dir_remove (struct dir *dir, const char *name) {
	struct dir_entry e, target_e;
	struct inode *inode = NULL, *target_inode;
	bool success = false;
	off_t ofs, target_ofs;

	ASSERT (dir != NULL);
	ASSERT (name != NULL);

	/* Find directory entry. */
	if (!lookup (dir, name, &e, &ofs))
		goto done;

	/* Open inode. */
	inode = inode_open (e.inode_sector);
	if (inode == NULL)
		goto done;
	
	if (!inode->data.is_file) {
		target_inode = inode_open(e.inode_sector);
		for (target_ofs = 0; inode_read_at (target_inode, &target_e, sizeof target_e, target_ofs) == sizeof target_e; target_ofs += sizeof target_e) {
			if (target_e.in_use && strcmp(target_e.name, ".") && strcmp(target_e.name, ".."))
				goto done;
		}
		inode_close(target_inode);
	}

	/* Erase directory entry. */
	e.in_use = false;
	if (inode_write_at (dir->inode, &e, sizeof e, ofs) != sizeof e)
		goto done;

	/* Remove inode. */
	inode_remove (inode);
	success = true;

done:
	inode_close (inode);
	return success;
}

/* Reads the next directory entry in DIR and stores the name in
 * NAME.  Returns true if successful, false if the directory
 * contains no more entries. */
bool
dir_readdir (struct dir *dir, char name[NAME_MAX + 1]) {
	struct dir_entry e;

	while (inode_read_at (dir->inode, &e, sizeof e, dir->pos) == sizeof e) {
		dir->pos += sizeof e;
		if (e.in_use && strcmp(e.name, ".") && strcmp(e.name, "..")) {
			strlcpy (name, e.name, NAME_MAX + 1);
			return true;
		}
	}
	return false;
}
