#include "filesys/directory.h"
#include <stdio.h>
#include <string.h>
#include <list.h>
#include "filesys/filesys.h"
#include "filesys/inode.h"
#include "threads/malloc.h"
#include "threads/synch.h"

/*! A directory. */
struct dir {
    struct inode *inode;                /*!< Backing store. */
    off_t pos;                          /*!< Current position. */
};

/*! A single directory entry. */
struct dir_entry {
    block_sector_t inode_sector;        /*!< Sector number of header. */
    char name[NAME_MAX + 1];            /*!< Null terminated file name. */
    bool in_use;                        /*!< In use or free? */
};

struct inode_disk {
  block_sector_t sectors[126]; /* Array of sectors; however, this has to be
				  doubly indirect to allow for larger files */
    off_t length;                       /*!< File size in bytes. */
    unsigned magic;                     /*!< Magic number. */
  //uint32_t unused[125];               /*!< Not used. */
};

struct inode {
    struct list_elem elem;              /*!< Element in inode list. */
    block_sector_t sector;              /*!< Sector number of disk location. */
    int open_cnt;                       /*!< Number of openers. */
    bool removed
;                       /*!< True if deleted, false otherwise. */
    int deny_write_cnt;                 /*!< 0: writes ok, >0: deny writes. */
    struct inode_disk data;             /*!< Inode content. */
  struct lock extend_lock; // lock used to synchronize extends
};

/*! Creates a directory with space for ENTRY_CNT entries in the
    given SECTOR.  Returns true if successful, false on failure. */
bool dir_create(block_sector_t sector, size_t entry_cnt, block_sector_t parent) {
    bool ret = inode_create(sector, entry_cnt * sizeof(struct dir_entry));
    if (!ret)
      return false;
    struct inode_disk d;
    block_read(fs_device, sector, &d);
    d.magic++;
    block_write(fs_device, sector, &d);

    // Open the directory and add the . and .. special files
    struct dir *dir = dir_open(inode_open(sector));
    dir_add(dir, ".", sector);
    dir_add(dir, "..", parent);
    dir_close(dir);
    return true;
}

/*! Opens and returns the directory for the given INODE, of which
    it takes ownership.  Returns a null pointer on failure. */
struct dir * dir_open(struct inode *inode) {
    struct dir *dir = calloc(1, sizeof(*dir));
    if (inode != NULL && dir != NULL && isdir(inode) && (!inode->removed)) {
        dir->inode = inode;
        dir->pos = 0;
        return dir;
    }
    else {
        inode_close(inode);
        free(dir);
        return NULL; 
    }
}

/*! Opens the root directory and returns a directory for it.
    Return true if successful, false on failure. */
struct dir * dir_open_root(void) {
    return dir_open(inode_open(ROOT_DIR_SECTOR));
}

/* Opens and returns a new directory for the same inode as DIR.
   Returns a null pointer on failure. */
struct dir * dir_reopen(struct dir *dir) {
    return dir_open(inode_reopen(dir->inode));
}

/*! Destroys DIR and frees associated resources. */
void dir_close(struct dir *dir) {
    if (dir != NULL) {
        inode_close(dir->inode);
        free(dir);
    }
}

/*! Returns the inode encapsulated by DIR. */
struct inode * dir_get_inode(struct dir *dir) {
    return dir->inode;
}

/*! Searches DIR for a file with the given NAME.
    If successful, returns true, sets *EP to the directory entry
    if EP is non-null, and sets *OFSP to the byte offset of the
    directory entry if OFSP is non-null.
    otherwise, returns false and ignores EP and OFSP. */
static bool lookup(const struct dir *dir, const char *name,
                   struct dir_entry *ep, off_t *ofsp) {
    struct dir_entry e;
    size_t ofs;

    ASSERT(dir != NULL);
    ASSERT(name != NULL);

    for (ofs = 0; inode_read_at(dir->inode, &e, sizeof(e), ofs) == sizeof(e);
         ofs += sizeof(e)) {
        if (e.in_use && !strcmp(name, e.name)) {
            if (ep != NULL)
                *ep = e;
            if (ofsp != NULL)
                *ofsp = ofs;
            return true;
        }
    }
    return false;
}

/*! Searches DIR for a file with the given NAME and returns true if one exists,
    false otherwise.  On success, sets *INODE to an inode for the file,
    otherwise to a null pointer.  The caller must close *INODE. */
bool dir_lookup(const struct dir *dir, const char *name, struct inode **inode) {
    struct dir_entry e;

    ASSERT(dir != NULL);
    ASSERT(name != NULL);

    if (lookup(dir, name, &e, NULL))
        *inode = inode_open(e.inode_sector);
    else
        *inode = NULL;

    return *inode != NULL;
}

/*! Adds a file named NAME to DIR, which must not already contain a file by
    that name.  The file's inode is in sector INODE_SECTOR.
    Returns true if successful, false on failure.
    Fails if NAME is invalid (i.e. too long) or a disk or memory
    error occurs. */
bool dir_add(struct dir *dir, const char *name, block_sector_t inode_sector) {
    struct dir_entry e;
    off_t ofs;
    bool success = false;

    ASSERT(dir != NULL);
    ASSERT(name != NULL);

    /* Check NAME for validity. */
    if (*name == '\0' || strlen(name) > NAME_MAX)
        return false;

    lock_acquire(&dir->inode->extend_lock);

    /* Check that NAME is not in use. */
    if (lookup(dir, name, NULL, NULL))
        goto done;

    /* Set OFS to offset of free slot.
       If there are no free slots, then it will be set to the
       current end-of-file.
     
       inode_read_at() will only return a short read at end of file.
       Otherwise, we'd need to verify that we didn't get a short
       read due to something intermittent such as low memory. */
    for (ofs = 0; inode_read_at(dir->inode, &e, sizeof(e), ofs) == sizeof(e);
         ofs += sizeof(e)) {
        if (!e.in_use)
            break;
    }

    /* Write slot. */
    e.in_use = true;
    strlcpy(e.name, name, sizeof e.name);
    e.inode_sector = inode_sector;
    lock_release(&dir->inode->extend_lock);
    success = inode_write_at(dir->inode, &e, sizeof(e), ofs) == sizeof(e);

done:
    if (lock_held_by_current_thread(&dir->inode->extend_lock))
      lock_release(&dir->inode->extend_lock);
    return success;
}

/*! Removes any entry for NAME in DIR.  Returns true if successful, false on
    failure, which occurs only if there is no file with the given NAME. */
bool dir_remove(struct dir *dir, const char *name) {
    struct dir_entry e;
    struct inode *inode = NULL;
    bool success = false;
    off_t ofs;

    ASSERT(dir != NULL);
    ASSERT(name != NULL);

    if (strcmp(name, ".") ^ strcmp(name, "..")) {
      return false; // Can't let you do that
    }

    lock_acquire(&dir->inode->extend_lock);

    /* Find directory entry. */
    if (!lookup(dir, name, &e, &ofs))
        goto done;

    /* Open inode. */
    inode = inode_open(e.inode_sector);
    if (inode == NULL)
        goto done;

    if (isdir(inode)) {
      // ruh roh :)
      struct dir *d = dir_open(inode_reopen(inode));
      char n[NAME_MAX + 1];
      if (dir_readdir(d,n)) {
	// Directory isn't empty
	dir_close(d);
	inode_close(inode);
	lock_release(&dir->inode->extend_lock);
	return false;
      }
      dir_close(d);
    }

    /* Erase directory entry. */
    e.in_use = false;
    lock_release(&dir->inode->extend_lock);
    if (inode_write_at(dir->inode, &e, sizeof(e), ofs) != sizeof(e))
        goto done;

    /* Remove inode. */
    inode_remove(inode);
    success = true;

done:
    if (lock_held_by_current_thread(&dir->inode->extend_lock))
      lock_release(&dir->inode->extend_lock);
    inode_close(inode);
    return success;
}

/*! Reads the next directory entry in DIR and stores the name in NAME.  Returns
    true if successful, false if the directory contains no more entries. */
bool dir_readdir(struct dir *dir, char name[NAME_MAX + 1]) {
    struct dir_entry e;
   
    if (dir->inode->removed)
      return false;
    
    while (inode_read_at(dir->inode, &e, sizeof(e), dir->pos) == sizeof(e)) {
        dir->pos += sizeof(e);
        if (e.in_use && strcmp(e.name, ".") && strcmp(e.name, "..")) {
            strlcpy(name, e.name, NAME_MAX + 1);
            return true;
        } 
    }
    return false;
}

