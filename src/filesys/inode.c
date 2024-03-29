#include "filesys/inode.h"
#include <list.h>
#include <debug.h>
#include <round.h>
#include <string.h>
#include "filesys/filesys.h"
#include "filesys/free-map.h"
#include "filesys/cache.h"
#include "threads/malloc.h"
#include "threads/synch.h"

/*! Identifies an inode. */
/* This isn't actually used for anything, so I'm going to mangle it to
   identify directories */
#define INODE_MAGIC 0x494e4f44

/*! On-disk inode.
    Must be exactly BLOCK_SECTOR_SIZE bytes long. */
struct inode_disk {
  block_sector_t sectors[126]; /* Array of sectors; however, this has to be
				  doubly indirect to allow for larger files */
    off_t length;                       /*!< File size in bytes. */
    unsigned magic;                     /*!< Magic number. */
  //uint32_t unused[125];               /*!< Not used. */
};

/*! Returns the number of sectors to allocate for an inode SIZE
    bytes long. */
static inline size_t bytes_to_sectors(off_t size) {
    return DIV_ROUND_UP(size, BLOCK_SECTOR_SIZE);
}

/*! In-memory inode. */
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

// self-explanatory
bool isdir(struct inode *n) {
  return !(n->data.magic - INODE_MAGIC - 1);
}

/*! Returns the block device sector that contains byte offset POS
    within INODE.
    Returns -1 if INODE does not contain data for a byte at offset
    POS. */
static block_sector_t byte_to_sector(const struct inode *inode, off_t pos) {
    ASSERT(inode != NULL);
    if (pos < inode->data.length) {
      //    return inode->data.start + pos / BLOCK_SECTOR_SIZE; {
      // Unfortunately there's some indirection here; we have to read the
      // corresponding sector into memory!
      block_sector_t tmp[128];
      block_sector_t map = inode->data.sectors[pos / (128 * BLOCK_SECTOR_SIZE)];
      block_read(fs_device, map, tmp);
      return tmp[(pos / BLOCK_SECTOR_SIZE) % 128];
    }
    else
        return -1;
}

/*! List of open inodes, so that opening a single inode twice
    returns the same `struct inode'. */
static struct list open_inodes;

/*! Initializes the inode module. */
void inode_init(void) {
    list_init(&open_inodes);
}

/*! Initializes an inode with LENGTH bytes of data and
    writes the new inode to sector SECTOR on the file system
    device.
    Returns true if successful.
    Returns false if memory or disk allocation fails. */
bool inode_create(block_sector_t sector, off_t length) {
    struct inode_disk *disk_inode = NULL;
    bool success = false;

    ASSERT(length >= 0);

    /* If this assertion fails, the inode structure is not exactly
       one sector in size, and you should fix that. */
    ASSERT(sizeof *disk_inode == BLOCK_SECTOR_SIZE);

    disk_inode = calloc(1, sizeof *disk_inode);
    if (disk_inode != NULL) {
        size_t sectors = bytes_to_sectors(length);
        disk_inode->length = length;
        disk_inode->magic = INODE_MAGIC;
	size_t i, j;
	for (i = 0; sectors > 0; ++i) {
	  // Get a block to hold (real) sector pointers
	  if (free_map_allocate(1, disk_inode->sectors+i)) {
	    // Get some memory to put these things in
	    block_sector_t *tmp = (block_sector_t *)malloc(512);
	    for (j = 0; j < 128 && sectors > 0; ++j, --sectors) {
	      // Get a block and fill it with zeroes
	      if (free_map_allocate(1, tmp+j)) {
		static char zeros[BLOCK_SECTOR_SIZE];
		block_write(fs_device, tmp[j], zeros);
	      }
	      else {
		// fail out, freeing all blocks
		for (success = true; j;) {
		  --j;
		  free_map_release(tmp[j], 1);
		}
		// Yes, I'm misusing "success" here. Success being true
		// indicates failure
	      }
	    }
	    if (!success) {
	      block_write(fs_device, disk_inode->sectors[i], tmp);
	    }
	    free(tmp);
	  }
	  else {
	    --i;
	    success = true;
	  }
	  if (success) {
	    for (++i; i;) {
	      --i;
	      free_map_release(disk_inode->sectors[i], 1);
	    }
	  }
	}
	// Finally, actually write the disk inode
	if (success) {
	  success = false;
	}
	else {
	  success = true;
	  block_write(fs_device, sector, disk_inode);
	}
	
        free(disk_inode);
    }
    return success;
}

/*! Reads an inode from SECTOR
    and returns a `struct inode' that contains it.
    Returns a null pointer if memory allocation fails. */
struct inode * inode_open(block_sector_t sector) {
    struct list_elem *e;
    struct inode *inode;

    /* Check whether this inode is already open. */
    for (e = list_begin(&open_inodes); e != list_end(&open_inodes);
         e = list_next(e)) {
        inode = list_entry(e, struct inode, elem);
        if (inode->sector == sector) {
            inode_reopen(inode);
            return inode; 
        }
    }

    /* Allocate memory. */
    inode = malloc(sizeof *inode);
    if (inode == NULL)
        return NULL;

    /* Initialize. */
    list_push_front(&open_inodes, &inode->elem);
    inode->sector = sector;
    inode->open_cnt = 1;
    inode->deny_write_cnt = 0;
    inode->removed = false;
    lock_init(&inode->extend_lock);
    block_read(fs_device, inode->sector, &inode->data);
    return inode;
}

/*! Reopens and returns INODE. */
struct inode * inode_reopen(struct inode *inode) {
    if (inode != NULL)
        inode->open_cnt++;
    return inode;
}

/*! Returns INODE's inode number. */
block_sector_t inode_get_inumber(const struct inode *inode) {
    return inode->sector;
}

/*! Closes INODE and writes it to disk.
    If this was the last reference to INODE, frees its memory.
    If INODE was also a removed inode, frees its blocks. */
void inode_close(struct inode *inode) {
    /* Ignore null pointer. */
    if (inode == NULL)
        return;

    /* Release resources if this was the last opener. */
    if (--inode->open_cnt == 0) {
        /* Remove from inode list and release lock. */
        list_remove(&inode->elem);
 
        /* Deallocate blocks if removed. */
        if (inode->removed) {
            
            //free_map_release(inode->data.start,
            //                 bytes_to_sectors(inode->data.length)); 
	    // We now have to deallocate _all_ sectors in our doubly
	    // indirect lists

	    size_t sectors = bytes_to_sectors(inode->data.length);
	    int i, j;
	    for (i = 0; sectors; ++i) {
	      // Read the appropriate thing into memory
	      block_sector_t tmp[128];
	      block_sector_t map = inode->data.sectors[i];
	      block_read(fs_device, map, tmp);
	      for (j = 0; j < 128 && sectors; ++i, --sectors) {
	        free_map_release(tmp[i], 1);
	      }
	      free_map_release(map,1);
	    }
	    free_map_release(inode->sector, 1);
        }

        free(inode); 
    }
}

/*! Marks INODE to be deleted when it is closed by the last caller who
    has it open. */
void inode_remove(struct inode *inode) {
    ASSERT(inode != NULL);
    inode->removed = true;
}

/*! Reads SIZE bytes from INODE into BUFFER, starting at position OFFSET.
   Returns the number of bytes actually read, which may be less
   than SIZE if an error occurs or end of file is reached. */
off_t inode_read_at(struct inode *inode, void *buffer_, off_t size, off_t offset) {
    uint8_t *buffer = buffer_;
    off_t bytes_read = 0;

    while (size > 0) {
        /* Disk sector to read, starting byte offset within sector. */
        block_sector_t sector_idx = byte_to_sector (inode, offset);
        int sector_ofs = offset % BLOCK_SECTOR_SIZE;

        /* Bytes left in inode, bytes left in sector, lesser of the two. */
        off_t inode_left = inode_length(inode) - offset;
        int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
        int min_left = inode_left < sector_left ? inode_left : sector_left;

        /* Number of bytes to actually copy out of this sector. */
        int chunk_size = size < min_left ? size : min_left;
        if (chunk_size <= 0)
            break;

        /* Read from cache */
        cache_read(sector_idx, sector_ofs, buffer + bytes_read, chunk_size);

        /* Request read-ahead */
        if (sector_ofs + size >= BLOCK_SECTOR_SIZE)
            cache_prefetch(sector_idx + 1);

        /* Advance. */
        size -= chunk_size;
        offset += chunk_size;
        bytes_read += chunk_size;
    }

    return bytes_read;
}

/*! Writes SIZE bytes from BUFFER into INODE, starting at OFFSET.
    Returns the number of bytes actually written, which may be
    less than SIZE if an error occurs or there is not enough
    space on the disk) */
off_t inode_write_at(struct inode *inode, const void *buffer_, off_t size, off_t offset) {
    const uint8_t *buffer = buffer_;
    off_t bytes_written = 0;
    static char zeros[BLOCK_SECTOR_SIZE];

    if (inode->deny_write_cnt)
        return 0;

    lock_acquire(&inode->extend_lock);
    if (offset + size > inode->data.length) {
      // Extend the file appropriately

      // An interesting approach (with respect to synchronization) is to
      // write a modified byte_to_sector() that extends the file, and only
      // extend up to offset in this loop
      int ext_sectors = bytes_to_sectors(offset + size) - bytes_to_sectors(inode->data.length);
      if (inode->data.length % BLOCK_SECTOR_SIZE)
	inode->data.length += BLOCK_SECTOR_SIZE - (inode->data.length % BLOCK_SECTOR_SIZE);
      int i = (inode->data.length) / (128 * BLOCK_SECTOR_SIZE);
      int j = ((inode->data.length) / BLOCK_SECTOR_SIZE) % 128;
      block_sector_t *tmp = (block_sector_t *)malloc(512);
      // Deal with the current indirect block if appropriate
      if (j) {
	block_read(fs_device, inode->data.sectors[i], tmp);
	for (; ext_sectors && j<128; ++j, --ext_sectors) {
	  if (free_map_allocate(1, tmp+j)) {
	    inode->data.length += BLOCK_SECTOR_SIZE;
	    block_write(fs_device, tmp[j], zeros);
	  }
	  else {
	    ext_sectors = 0;
	    break; // No, we don't really have to do anything more
	  }
	}
	block_write(fs_device, inode->data.sectors[i], tmp);
	++i;
      }
      for (; ext_sectors; ++i) {
	if (free_map_allocate(1, inode->data.sectors+i)) {
	  for (j = 0; ext_sectors && j<128; ++j, --ext_sectors) {
	    if (free_map_allocate(1, tmp+j)) {
	      inode->data.length += BLOCK_SECTOR_SIZE;
	      block_write(fs_device, tmp[j], zeros);
	    }
	    else {
	      ext_sectors = 0;
	      break;
	    }
	  }
	  block_write(fs_device, inode->data.sectors[i], tmp);
	}
	else {
	  ext_sectors = 0;
	  break;
	}
	if (ext_sectors == 0 && j == 0) {
	  free_map_release(tmp[j], 1);
	}
      }
      free(tmp);
      inode->data.length = offset + size;
      // This is the synchronization of the file extension with reads; the new
      // length is not written until the block is deallocated. This may need
      // to change with buffer cache enabled (in this case we need to not
      // write the new length back to the buffer cache until we're done
      // writing data)
      block_write(fs_device, inode->sector, &inode->data);
    }
    lock_release(&inode->extend_lock);

    while (size > 0) {
        /* Sector to write, starting byte offset within sector. */
        block_sector_t sector_idx = byte_to_sector(inode, offset);
        int sector_ofs = offset % BLOCK_SECTOR_SIZE;

        /* Bytes left in inode, bytes left in sector, lesser of the two. */
        off_t inode_left = inode_length(inode) - offset;
        int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
        int min_left = inode_left < sector_left ? inode_left : sector_left;

        /* Number of bytes to actually write into this sector. */
        int chunk_size = size < min_left ? size : min_left;
        if (chunk_size <= 0)
            break;

        /* Write to cache */
        cache_write(sector_idx, sector_ofs, buffer + bytes_written, chunk_size,
                    (sector_ofs > 0 || chunk_size < sector_left));

        /* Advance. */
        size -= chunk_size;
        offset += chunk_size;
        bytes_written += chunk_size;
    }

    return bytes_written;
}

/*! Disables writes to INODE.
    May be called at most once per inode opener. */
void inode_deny_write (struct inode *inode) {
    inode->deny_write_cnt++;
    ASSERT(inode->deny_write_cnt <= inode->open_cnt);
}

/*! Re-enables writes to INODE.
    Must be called once by each inode opener who has called
    inode_deny_write() on the inode, before closing the inode. */
void inode_allow_write (struct inode *inode) {
    ASSERT(inode->deny_write_cnt > 0);
    ASSERT(inode->deny_write_cnt <= inode->open_cnt);
    inode->deny_write_cnt--;
}

/*! Returns the length, in bytes, of INODE's data. */
off_t inode_length(const struct inode *inode) {
    return inode->data.length;
}

