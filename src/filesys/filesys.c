#include "filesys/filesys.h"
#include <debug.h>
#include <stdio.h>
#include <string.h>
#include "filesys/file.h"
#include "filesys/free-map.h"
#include "filesys/inode.h"
#include "filesys/directory.h"
#include "filesys/cache.h"

#include <list.h>

struct dir {
  struct inode *inode;                /*!< Backing store. */
  off_t pos;                          /*!< Current position. */
};
struct inode_disk {
  uint32_t sectors[126]; /* Array of sectors; however, this has to be
				  doubly indirect to allow for larger files */
    off_t length;                       /*!< File size in bytes. */
    unsigned magic;                     /*!< Magic number. */
  //uint32_t unused[125];               /*!< Not used. */
};
struct inode {
    struct list_elem elem;              /*!< Element in inode list. */
    uint32_t sector;              /*!< Sector number of disk location. */
    int open_cnt;                       /*!< Number of openers. */
    bool removed;                       /*!< True if deleted, false otherwise. */
    int deny_write_cnt;                 /*!< 0: writes ok, >0: deny writes. */
    struct inode_disk data;             /*!< Inode content. */
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
    cache_flush();
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

struct file {
    struct inode *inode;        /*!< File's inode. */
    off_t pos;                  /*!< Current position. */
    bool deny_write;            /*!< Has file_deny_write() been called? */
};

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
  if (namecpy == NULL) {
    return NULL;
  }
  memcpy(namecpy, name, strlen(name)+1);
  if (name[0] == '/') {
    dir_close(d);
    d = dir_open_root();
  }
  n = strtok_r(namecpy, "/", &saveptr);
  if (n == NULL) { // it's the root directory
    f = file_open(inode_reopen(d->inode));
    dir_close(d);
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
    dir_close(d);
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
      dir_close(d);
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
      dir_close(d);
      palloc_free_page(namecpy);
      return NULL; // some part of the filename is wrong
    }
  }
  // If we got here, then we're opening a directory!
  f = file_open(inode_reopen(d->inode));
  dir_close(d);
  palloc_free_page(namecpy);
  return f;
}

// Similar to the above, except we instead create the directory if possible;
// this changes some of the code logic, because we _don't_ want to be able
// to find the file!
// Returns false if failed for any reason
bool filesys_mkdir_rel(struct dir *d_, const char *name) {
  char *saveptr, *n, *namecpy;
  struct inode *in;
  
  struct dir *d = dir_reopen(d_); // To avoid messing up the thread copy
  struct file *f;
  if (strlen(name) == 0)
    return false;

  namecpy = palloc_get_page(0);
  if (namecpy == NULL)
    return false;
  memcpy(namecpy, name, strlen(name)+1);
  if (name[0] == '/') {
    dir_close(d);
    d = dir_open_root();
  }
  
  n = strtok_r(namecpy, "/", &saveptr);
  if (n == NULL) {
    // mkdir("/") shouldn't work
    dir_close(d);
    palloc_free_page(namecpy);
    return false;
  }
  while(n) {
    if (dir_lookup(d, n, &in)) {
      if (isdir(in)) {
	dir_close(d);
	d = dir_open(in);
      }
      else {
	// If there's a file _anywhere_ then it should fail
	dir_close(d);
	palloc_free_page(namecpy);
	return false;
      }
    }
    else {
      // See whether this was the last token
      if (strtok_r(NULL, "/", &saveptr) == NULL) {
	// Make a new directory and add it to d
	uint32_t sec;
	free_map_allocate(1, &sec);
	dir_create(sec, 16, d->inode->sector);
	dir_add(d, n, sec);
	dir_close(d);
	palloc_free_page(namecpy);
	return true;
      }
      else {
	dir_close(d);
	palloc_free_page(namecpy);
	return false;
      }
    }
    n=strtok_r(NULL, "/", &saveptr);
  }
  dir_close(d);
  palloc_free_page(namecpy);
  return false;
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

bool filesys_remove_rel(struct dir *d_, const char *name) {
  char *saveptr, *n, *namecpy;
  struct inode *in;
  struct dir *d = dir_reopen(d_), *d2; // To avoid messing up the thread copy
  struct file *f;
  if (strlen(name) == 0)
    return false;

  namecpy = palloc_get_page(0);

  if (namecpy == NULL) {
    return false;
  }
  memcpy(namecpy, name, strlen(name)+1);
  if (name[0] == '/') {
    dir_close(d);
    d = dir_open_root();
  }
  
  n = strtok_r(namecpy, "/", &saveptr);
  if (n == NULL) {
    palloc_free_page(namecpy);
    return false;
  }
  while(n) {
    if (dir_lookup(d, n, &in)) {
      if (isdir(in)) {
	dir_close(d);
	d = dir_open(in);
      }
      else {
	// check whether it's the last token
	if (strtok_r(NULL, "/", &saveptr)) {
	  // if not then fail out
	  dir_close(d);
	  if(d2)
	    dir_close(d2);
	  palloc_free_page(namecpy);
	  return false;
	}
	else {
	  // if so, try to remove the file
	  bool suc = dir_remove(d, n);
	  dir_close(d);
	  if(d2)
	    dir_close(d2);
	  palloc_free_page(namecpy);
	  return suc;
	}
      }
    }
    else {
      // fail out
      dir_close(d);
      if(d2)
	dir_close(d2);
      palloc_free_page(namecpy);
      return false;
    }
    nt=strtok_r(NULL, "/", &saveptr);
    if (nt) {
      n = nt;
    }
    else {
      break;
    }
    n=strtok_r(NULL, "/", &saveptr);
  }
  printf("wuh?\n");
  dir_close(d);
  palloc_free_page(namecpy);
  return false;
}

bool filesys_create_rel(struct dir *d_, const char *name, unsigned int size) {
  char *saveptr, *n, *namecpy;
  struct inode *in;
  
  struct dir *d = dir_reopen(d_); // To avoid messing up the thread copy
  struct file *f;
  if (strlen(name) == 0)
    return false;

  namecpy = palloc_get_page(0);
  if (namecpy == NULL)
    return false;
  memcpy(namecpy, name, strlen(name)+1);
  if (name[0] == '/') {
    dir_close(d);
    d = dir_open_root();
  }
  
  n = strtok_r(namecpy, "/", &saveptr);
  if (n == NULL) {
    // mkdir("/") shouldn't work
    palloc_free_page(namecpy);
    return false;
  }
  while(n) {
    if (dir_lookup(d, n, &in)) {
      if (isdir(in)) {
	dir_close(d);
	d = dir_open(in);
      }
      else {
	// If there's a file _anywhere_ then it should fail
	palloc_free_page(namecpy);
	return false;
      }
    }
    else {
      // See whether this was the last token
      if (strtok_r(NULL, "/", &saveptr) == NULL) {
	// Make a new directory and add it to d
	uint32_t sec;
	free_map_allocate(1, &sec);
	if ((sec != NULL) && inode_create(sec, size)) {
	  dir_add(d, n, sec);
	  palloc_free_page(namecpy);
	  return true;
	}
	else {
	  if(sec != NULL)
	    free_map_release(sec, 1);
	  palloc_free_page(namecpy);
	  return false;
	}
      }
      else {
	palloc_free_page(namecpy);
	return false;
      }
    }
    n=strtok_r(NULL, "/", &saveptr);
  }
  palloc_free_page(namecpy);
  return false;
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

