#include "filesys/filesys.h"
#include <debug.h>
#include <stdio.h>
#include <string.h>
#include "filesys/file.h"
#include "filesys/free-map.h"
#include "filesys/inode.h"
#include "filesys/directory.h"

struct dir {
  struct inode *inode;                /*!< Backing store. */
  off_t pos;                          /*!< Current position. */
};

/*! Partition that contains the file system. */
struct block *fs_device;

static void do_format(void);

/*! Initializes the file system module.
    If FORMAT is true, reformats the file system. */
void filesys_init(bool format) {
    fs_device = block_get_role(BLOCK_FILESYS);
    if (fs_device == NULL)
        PANIC("No file system device found, can't initialize file system.");

    inode_init();
    free_map_init();

    if (format) 
        do_format();

    free_map_open();
}

/*! Shuts down the file system module, writing any unwritten data to disk. */
void filesys_done(void) {
    free_map_close();
}

/*! Creates a file named NAME with the given INITIAL_SIZE.  Returns true if
    successful, false otherwise.  Fails if a file named NAME already exists,
    or if internal memory allocation fails. */
bool filesys_create(const char *name, off_t initial_size) {
    block_sector_t inode_sector = 0;
    struct dir *dir = dir_open_root();
    bool success = (dir != NULL &&
                    free_map_allocate(1, &inode_sector) &&
                    inode_create(inode_sector, initial_size) &&
                    dir_add(dir, name, inode_sector));
    if (!success && inode_sector != 0) 
        free_map_release(inode_sector, 1);
    dir_close(dir);

    return success;
}

// opens the file with the given name relative to the given directory (or
// not, if name starts with '/')
// this function has a lot of warnings, due probably to being poorly implemented
struct file * filesys_open_rel(struct dir *d_, const char *name) {
  char *saveptr, *n, *namecpy;
  struct inode *in;

  struct dir *d = dir_reopen(d_); // To avoid messing up the thread copy
  struct file *f;
  if (strlen(name) == 0)
    return NULL;

  namecpy = palloc_get_page(0);
  if (namecpy == NULL)
    return NULL;
  memcpy(namecpy, name, strlen(name)+1);
  if (name[0] == '/') {
    dir_close(d);
    d = dir_open_root();
  }
  n = strtok_r(namecpy, "/", &saveptr);
  if (n == NULL) { // it's the root directory
    f = d->inode;
    free(d);
    palloc_free_page(namecpy);
    return f;
  }
  if (dir_lookup(d, n, &in)) {
    dir_close(d);
    if (isdir(in))
      d = dir_open(in);
    else {
      // Well, then it must be a file
      if (strtok_r(NULL, "/", &saveptr)) {
	// Means we're trying to use something that's not a directory as one
        inode_close(in);
	palloc_free_page(namecpy);
	return NULL;
      }
      else {
	f = file_open(in);
	palloc_free_page(namecpy);
	return f;
      }
    }
  }
  else {
    palloc_free_page(namecpy);
    return NULL; // some part of the filename is wrong
  }
  while (n = strtok_r(NULL, "/", &saveptr)) {
    // Look for and open the relevant directory
    if(dir_lookup(d, n, &in)) {
      dir_close(d);
      if (isdir(in))
      d = dir_open(in);
    else {
      // Well, then it must be a file
      if (strtok_r(NULL, "/", &saveptr)) {
	// Means we're trying to use something that's not a directory as one
        inode_close(in);
	palloc_free_page(namecpy);
	return NULL;
      }
      else {
	f = file_open(in);
	palloc_free_page(namecpy);
	return f;
      }
    }
    }
    else {
      palloc_free_page(namecpy);
      return NULL; // some part of the filename is wrong
    }
  }
  // If we got here, then we're opening a directory!
  f = d->inode;
  free(d);
  palloc_free_page(namecpy);
  return f;
}

/*! Opens the file with the given NAME.  Returns the new file if successful
    or a null pointer otherwise.  Fails if no file named NAME exists,
    or if an internal memory allocation fails. */
struct file * filesys_open(const char *name) {
    struct dir *dir = dir_open_root();
    struct inode *inode = NULL;

    if (dir != NULL)
        dir_lookup(dir, name, &inode);
    dir_close(dir);

    return file_open(inode);
}

/*! Deletes the file named NAME.  Returns true if successful, false on failure.
    Fails if no file named NAME exists, or if an internal memory allocation
    fails. */
bool filesys_remove(const char *name) {
    struct dir *dir = dir_open_root();
    bool success = dir != NULL && dir_remove(dir, name);
    dir_close(dir);

    return success;
}

/*! Formats the file system. */
static void do_format(void) {
    printf("Formatting file system...");
    free_map_create();
    if (!dir_create(ROOT_DIR_SECTOR, 16, ROOT_DIR_SECTOR))
        PANIC("root directory creation failed");
    free_map_close();
    printf("done.\n");
}

