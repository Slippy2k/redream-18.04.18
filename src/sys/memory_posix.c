#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include "core/assert.h"
#include "core/list.h"
#include "core/string.h"
#include "sys/filesystem.h"
#include "sys/memory.h"

static const int MAX_SHMEM = 128;

typedef struct {
  char filename[PATH_MAX];
  int handle;
  list_node_t free_it;
} shmem_t;

static bool s_initialized;
static shmem_t s_shmem[MAX_SHMEM];
static list_t s_free_shmem;

static mode_t access_to_mode_flags(page_access_t access) {
  switch (access) {
    case ACC_READONLY:
      return S_IREAD;
    case ACC_READWRITE:
      return S_IREAD | S_IWRITE;
    default:
      return 0;
  }
}

static int access_to_open_flags(page_access_t access) {
  switch (access) {
    case ACC_READONLY:
      return O_RDONLY;
    case ACC_READWRITE:
      return O_RDWR;
    default:
      return 0;
  }
}

static int access_to_protect_flags(page_access_t access) {
  switch (access) {
    case ACC_READONLY:
      return PROT_READ;
    case ACC_READWRITE:
      return PROT_READ | PROT_WRITE;
    case ACC_READWRITEEXEC:
      return PROT_READ | PROT_WRITE | PROT_EXEC;
    default:
      return PROT_NONE;
  }
}

size_t get_page_size() {
  return getpagesize();
}

size_t get_allocation_granularity() {
  return get_page_size();
}

bool protect_pages(void *ptr, size_t size, page_access_t access) {
  int prot = access_to_protect_flags(access);
  return mprotect(ptr, size, prot) == 0;
}

bool reserve_pages(void *ptr, size_t size) {
  // NOTE mmap with MAP_FIXED will overwrite existing mappings, making it hard
  // to detect that a section of memory has already been mmap'd. however, mmap
  // without MAP_FIXED will obey the address parameter only if an existing
  // mapping does not already exist, else it will map it to a new address.
  // knowing this, an existing mapping can be detected by not using MAP_FIXED,
  // and comparing the returned mapped address with the requested address
  void *res =
      mmap(ptr, size, PROT_NONE, MAP_ANON | MAP_NORESERVE | MAP_PRIVATE, -1, 0);

  if (res == MAP_FAILED) {
    return false;
  }

  if (res != ptr) {
    // mapping was successful. however, it was made at a different address
    // than requested, meaning the requested address has already been mapped
    munmap(res, size);
    return false;
  }

  return true;
}

bool release_pages(void *ptr, size_t size) {
  return munmap(ptr, size) == 0;
}

static void init_shared_memory_entries() {
  if (s_initialized) {
    return;
  }

  s_initialized = true;

  // add all entries to free list
  for (int i = 0; i < MAX_SHMEM; i++) {
    shmem_t *shmem = &s_shmem[i];
    list_add(&s_free_shmem, &shmem->free_it);
  }
}

shmem_handle_t create_shared_memory(const char *filename, size_t size,
                                    page_access_t access) {
  init_shared_memory_entries();

  // find unused shmem entry (wrapper for both shmem object name and file
  // handle)
  shmem_t *shmem = list_first_entry(&s_free_shmem, shmem_t, free_it);
  CHECK_NOTNULL(shmem);

  // make sure the shared memory object doesn't already exist
  shm_unlink(filename);

  // create the shared memory object and open a file handle to it
  int oflag = access_to_open_flags(access);
  mode_t mode = access_to_mode_flags(access);
  int handle = shm_open(filename, oflag | O_CREAT | O_EXCL, mode);
  if (handle == -1) {
    return NULL;
  }

  // resize it
  int res = ftruncate(handle, size);
  if (res == -1) {
    shm_unlink(filename);
    return NULL;
  }

  // update entry, remove from free list
  strncpy(shmem->filename, filename, sizeof(shmem->filename));
  shmem->handle = handle;
  list_remove(&s_free_shmem, &shmem->free_it);

  return (shmem_handle_t)shmem;
}

bool map_shared_memory(shmem_handle_t handle, size_t offset, void *start,
                       size_t size, page_access_t access) {
  init_shared_memory_entries();

  shmem_t *shmem = (shmem_t *)handle;

  int prot = access_to_protect_flags(access);
  void *ptr =
      mmap(start, size, prot, MAP_SHARED | MAP_FIXED, shmem->handle, offset);

  return ptr != MAP_FAILED;
}

bool unmap_shared_memory(shmem_handle_t handle, void *start, size_t size) {
  return munmap(start, size) == 0;
}

bool destroy_shared_memory(shmem_handle_t handle) {
  init_shared_memory_entries();

  shmem_t *shmem = (shmem_t *)handle;

  int res1 = close(shmem->handle);
  int res2 = shm_unlink(shmem->filename);

  // add back to free list
  list_add(&s_free_shmem, &shmem->free_it);

  return res1 == 0 && res2 == 0;
}
