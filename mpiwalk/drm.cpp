#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <time.h> /* asctime / localtime */

#include <pwd.h> /* for getpwent */
#include <grp.h> /* for getgrent */
#include <errno.h>
#include <string.h>

#include <libgen.h> /* dirname */

#include "libcircle.h"
#include "dtcmp.h"
#include "bayer.h"

#include <map>
#include <string>

using namespace std;

/* TODO: these types may be encoded in files */
enum filetypes {
  TYPE_NULL    = 0,
  TYPE_UNKNOWN = 1,
  TYPE_FILE    = 2,
  TYPE_DIR     = 3,
  TYPE_LINK    = 4,
};

// getpwent getgrent to read user and group entries

/* TODO: change globals to struct */
static int verbose   = 0;
static int walk_stat = 1;

/* struct for elements in linked list */
typedef struct list_elem {
  char* file;             /* file name */
  filetypes type;         /* record type of object */
  struct stat* sb;        /* stat info */
  struct list_elem* next; /* pointer to next item */
} elem_t;

/* declare types and function prototypes for DFTW code */
typedef int (*DFTW_cb_t)(const char* fpath, const struct stat* sb, mode_t type);

/* variables to track linked list during walk */
uint64_t        list_count = 0;
static elem_t*  list_head  = NULL;
static elem_t*  list_tail  = NULL;

/** The top directory specified. */
char _DFTW_TOP_DIR[PATH_MAX];

/** The callback. */
DFTW_cb_t _DFTW_CB;

/* appends file name and stat info to linked list */
static int record_info(const char *fpath, const struct stat *sb, mode_t type)
{
  /* TODO: check that memory allocation doesn't fail */
  /* create new element to record file path, file type, and stat info */
  elem_t* elem = (elem_t*) malloc(sizeof(elem_t));

  /* copy path */
  elem->file = strdup(fpath);

  /* set file type */
  if (S_ISDIR(type)) {
    elem->type = TYPE_DIR;
  } else if (S_ISREG(type)) {
    elem->type = TYPE_FILE;
  } else if (S_ISLNK(type)) {
    elem->type = TYPE_LINK;
  } else {
    /* unknown file type */
    elem->type = TYPE_UNKNOWN;
//    printf("Unknown file type for %s (%lx)\n", fpath, (unsigned long)type);
//    fflush(stdout);
  }

  /* copy stat info */
  if (sb != NULL) {
    elem->sb = (struct stat*) malloc(sizeof(struct stat));
    memcpy(elem->sb, sb, sizeof(struct stat));
  } else {
    elem->sb = NULL;
  }

  /* append element to tail of linked list */
  elem->next = NULL;
  if (list_head == NULL) {
    list_head = elem;
  }
  if (list_tail != NULL) {
    list_tail->next = elem;
  }
  list_tail = elem;
  list_count++;

  /* To tell dftw() to continue */
  return 0;
}

/****************************************
 * Walk directory tree using stat at top level and readdir
 ***************************************/

static void DFTW_process_dir_readdir(char* dir, CIRCLE_handle* handle)
{
  /* TODO: may need to try these functions multiple times */
  DIR* dirp = opendir(dir);

  if (! dirp) {
    /* TODO: print error */
  } else {
    /* Read all directory entries */
    while (1) {
      /* read next directory entry */
      struct dirent* entry = bayer_readdir(dirp);
      if (entry == NULL) {
        break;
      }

      /* process component, unless it's "." or ".." */
      char* name = entry->d_name;
      if((strncmp(name, ".", 2)) && (strncmp(name, "..", 3))) {
        /* <dir> + '/' + <name> + '/0' */
        char newpath[CIRCLE_MAX_STRING_LEN];
        size_t len = strlen(dir) + 1 + strlen(name) + 1;
        if (len < sizeof(newpath)) {
          /* build full path to item */
          strcpy(newpath, dir);
          strcat(newpath, "/");
          strcat(newpath, name);

          #ifdef _DIRENT_HAVE_D_TYPE
            /* record info for item */
            mode_t mode;
            int have_mode = 0;
            if (entry->d_type != DT_UNKNOWN) {
              /* we can read object type from directory entry */
              have_mode = 1;
              mode = DTTOIF(entry->d_type);
              _DFTW_CB(newpath, NULL, mode);
            } else {
              /* type is unknown, we need to stat it */
              struct stat st;
              int status = bayer_lstat(newpath, &st);
              if (status == 0) {
                have_mode = 1;
                mode = st.st_mode;
                _DFTW_CB(newpath, &st, mode);
              } else {
                /* error */
              }
            }

            /* recurse into directories */
            if (have_mode && S_ISDIR(mode)) {
              handle->enqueue(newpath);
            }
          #endif
        } else {
          /* name is too long */
          printf("Path name is too long: %lu chars exceeds limit %lu\n", len, sizeof(newpath));
          fflush(stdout);
        }
      }
    }
  }

  closedir(dirp);

  return;
}

/** Call back given to initialize the dataset. */
static void DFTW_create_readdir(CIRCLE_handle* handle)
{
  char* path = _DFTW_TOP_DIR;

  /* stat top level item */
  struct stat st;
  int status = bayer_lstat(path, &st);
  if (status != 0) {
    /* TODO: print error */
    return;
  }

  /* record item info */
  _DFTW_CB(path, &st, st.st_mode);

  /* recurse into directory */
  if (S_ISDIR(st.st_mode)) {
    DFTW_process_dir_readdir(path, handle);
  }

  return;
}

/** Callback given to process the dataset. */
static void DFTW_process_readdir(CIRCLE_handle* handle)
{
  /* in this case, only items on queue are directories */
  char path[CIRCLE_MAX_STRING_LEN];
  handle->dequeue(path);
  DFTW_process_dir_readdir(path, handle);
  return;
}

/****************************************
 * Walk directory tree using stat on every object
 ***************************************/

static void DFTW_process_dir_stat(char* dir, CIRCLE_handle* handle)
{
  /* TODO: may need to try these functions multiple times */
  DIR* dirp = opendir(dir);

  if (! dirp) {
    /* TODO: print error */
  } else {
    while (1) {
      /* read next directory entry */
      struct dirent* entry = bayer_readdir(dirp);
      if (entry == NULL) {
        break;
      }
       
      /* We don't care about . or .. */
      char* name = entry->d_name;
      if ((strncmp(name, ".", 2)) && (strncmp(name, "..", 3))) {
        /* <dir> + '/' + <name> + '/0' */
        char newpath[CIRCLE_MAX_STRING_LEN];
        size_t len = strlen(dir) + 1 + strlen(name) + 1;
        if (len < sizeof(newpath)) {
          /* build full path to item */
          strcpy(newpath, dir);
          strcat(newpath, "/");
          strcat(newpath, name);

          /* add item to queue */
          handle->enqueue(newpath);
        } else {
          /* name is too long */
          printf("Path name is too long: %lu chars exceeds limit %lu\n", len, sizeof(newpath));
          fflush(stdout);
        }
      }
    }
  }

  closedir(dirp);

  return;
}

/** Call back given to initialize the dataset. */
static void DFTW_create_stat(CIRCLE_handle* handle)
{
  /* we'll call stat on every item */
  handle->enqueue(_DFTW_TOP_DIR);
}

/** Callback given to process the dataset. */
static void DFTW_process_stat(CIRCLE_handle* handle)
{
  /* get path from queue */
  char path[CIRCLE_MAX_STRING_LEN];
  handle->dequeue(path);

  /* stat item */
  struct stat st;
  int status = bayer_lstat(path, &st);
  if (status != 0) {
    /* print error */
    return;
  }

  /* TODO: filter items by stat info */

  /* record info for item */
  _DFTW_CB(path, &st, st.st_mode);

  /* recurse into directory */
  if (S_ISDIR(st.st_mode)) {
    /* TODO: check that we can recurse into directory */
    DFTW_process_dir_stat(path, handle);
  }

  return;
}

/****************************************
 * Set up and execute directory walk
 ***************************************/

void dftw(
  const char* dirpath,
  DFTW_cb_t fn)
//int (*fn)(const char* fpath, const struct stat* sb, mode_t type))
{
  /* set some global variables to do the file walk */
  _DFTW_CB = fn;
  strncpy(_DFTW_TOP_DIR, dirpath, PATH_MAX);

  /* initialize libcircle */
  CIRCLE_init(0, NULL, CIRCLE_SPLIT_EQUAL);

  /* set libcircle verbosity level */
  enum CIRCLE_loglevel loglevel = CIRCLE_LOG_WARN;
  if (verbose) {
    loglevel = CIRCLE_LOG_INFO;
  }
  CIRCLE_enable_logging(loglevel);

  /* register callbacks */
  if (walk_stat) {
    /* walk directories by calling stat on every item */
    CIRCLE_cb_create(&DFTW_create_stat);
    CIRCLE_cb_process(&DFTW_process_stat);
  } else {
    /* walk directories using file types in readdir */
    CIRCLE_cb_create(&DFTW_create_readdir);
    CIRCLE_cb_process(&DFTW_process_readdir);
  }

  /* run the libcircle job */
  CIRCLE_begin();
  CIRCLE_finalize();
}

/* create a type consisting of chars number of characters
 * immediately followed by a uint32_t */
static void create_stridtype(int chars, MPI_Datatype* dt)
{
  /* build type for string */
  MPI_Datatype dt_str;
  MPI_Type_contiguous(chars, MPI_CHAR, &dt_str);

  /* build keysat type */
  MPI_Datatype types[2];
  types[0] = dt_str;       /* file name */
  types[1] = MPI_UINT32_T; /* id */
  DTCMP_Type_create_series(2, types, dt);

  MPI_Type_free(&dt_str);
  return;
}

/* element for a linked list of name/id pairs */
typedef struct strid {
  char* name;
  uint32_t id;
  struct strid* next;
} strid_t;

/* insert specified name and id into linked list given by
 * head, tail, and count, also increase maxchars if needed */
static void strid_insert(
  const char* name,
  uint32_t id,
  strid_t** head,
  strid_t** tail,
  int* count,
  int* maxchars)
{
  /* add username and id to linked list */
  strid_t* elem = (strid_t*) malloc(sizeof(strid_t));
  elem->name = strdup(name);
  elem->id = id;
  elem->next = NULL;
  if (*head == NULL) {
    *head = elem;
  }
  if (*tail != NULL) {
    (*tail)->next = elem;
  }
  *tail = elem;
  (*count)++;

  /* increase maximum username if we need to */
  size_t len = strlen(name) + 1;
  if (*maxchars < (int)len) {
    /* round up to nearest multiple of 4 */
    size_t len4 = len / 4;
    if (len4 * 4 < len) {
      len4++;
    }
    len4 *= 4;

    *maxchars = (int)len4;
  }

  return;
}

/* copy data from linked list to array */
static void strid_serialize(strid_t* head, int chars, void* buf)
{
  char* ptr = (char*)buf;
  strid_t* current = head;
  while (current != NULL) {
    char* name  = current->name;
    uint32_t id = current->id;

    strcpy(ptr, name);
    ptr += chars;

    uint32_t* p32 = (uint32_t*) ptr;
    *p32 = id;
    ptr += 4;

    current = current->next;
  }
  return;
}

typedef struct {
  void* buf;
  uint64_t count;
  uint64_t chars;
  MPI_Datatype dt;
} buf_t;

static void init_buft(buf_t* items)
{
  /* initialize output parameters */
  items->buf = NULL;
  items->count = 0;
  items->chars = 0;
  items->dt    = MPI_DATATYPE_NULL;
}

static void free_buft(buf_t* items)
{
  if (items->buf != NULL) {
    free(items->buf);
    items->buf = NULL;
  }

  if (items->dt != MPI_DATATYPE_NULL) {
    MPI_Type_free(&(items->dt));
  }

  items->count = 0;
  items->chars = 0;

  return;
}

/* build a name-to-id map and an id-to-name map */
static void create_maps(
  const buf_t* items,
  map<string,uint32_t>& name2id,
  map<uint32_t,string>& id2name)
{
  int i;
  const char* ptr = (const char*)items->buf;
  for (i = 0; i < items->count; i++) {
    const char* name = ptr;
    ptr += items->chars;

    uint32_t id = *(uint32_t*)ptr;
    ptr += 4;

    name2id[name] = id;
    id2name[id] = name;
  }
  return;
}

/* given an id, lookup its corresponding name, returns id converted
 * to a string if no matching name is found */
static const char* get_name_from_id(uint32_t id, int chars, map<uint32_t,string>& id2name)
{
  map<uint32_t,string>::iterator it = id2name.find(id);
  if (it != id2name.end()) {
    const char* name = (*it).second.c_str();
    return name;
  } else {
    /* store id as name and return that */
    char temp[12];
    size_t len = snprintf(temp, sizeof(temp), "%d", id);
    if (len > (sizeof(temp) - 1) || len > (chars - 1)) {
      /* TODO: ERROR! */
      printf("ERROR!!!\n");
    }

    string newname = temp;
    id2name[id] = newname;

    it = id2name.find(id);
    if (it != id2name.end()) {
      const char* name = (*it).second.c_str();
      return name;
    } else {
      /* TODO: ERROR! */
      printf("ERROR!!!\n");
    }
  }
  return NULL;
}

/* delete linked list and reset head, tail, and count values */
static void strid_delete(strid_t** head, strid_t** tail, int* count)
{
  /* free memory allocated in linked list */
  strid_t* current = *head;
  while (current != NULL) {
    strid_t* next = current->next;
    free(current->name);
    free(current);
    current = next;
  }

  /* set list data structure values back to NULL */
  *head  = NULL;
  *tail  = NULL;
  *count = 0;

  return;
}

/* read user array from file system using getpwent() */
static void get_users(buf_t* items)
{
  /* initialize output parameters */
  init_buft(items);

  /* get our rank */
  int rank;
  MPI_Comm_rank(MPI_COMM_WORLD, &rank);

  /* rank 0 iterates over users with getpwent */
  strid_t* head = NULL;
  strid_t* tail = NULL;
  int count = 0;
  int chars = 0;
  if (rank == 0) {
    struct passwd* p;
    while (1) {
      /* get next user, this function can fail so we retry a few times */
      int retries = 3;
retry:
      p = getpwent();
      if (p == NULL) {
        if (errno == EIO) {
          retries--;
        } else {
          /* TODO: ERROR! */
          retries = 0;
        }
        if (retries > 0) {
          goto retry;
        }
      }

      if (p != NULL) {
        /*
        printf("User=%s Pass=%s UID=%d GID=%d Name=%s Dir=%s Shell=%s\n",
          p->pw_name, p->pw_passwd, p->pw_uid, p->pw_gid, p->pw_gecos, p->pw_dir, p->pw_shell
        );
        printf("User=%s UID=%d GID=%d\n",
          p->pw_name, p->pw_uid, p->pw_gid
        );
        */
        char* name  = p->pw_name;
        uint32_t id = p->pw_uid;
        strid_insert(name, id, &head, &tail, &count, &chars);
      } else {
        /* hit the end of the user list */
        endpwent();
        break;
      }
    }

//    printf("Max username %d, count %d\n", (int)chars, count);
  }

  /* bcast count and number of chars */
  MPI_Bcast(&count, 1, MPI_INT, 0, MPI_COMM_WORLD);
  MPI_Bcast(&chars, 1, MPI_INT, 0, MPI_COMM_WORLD);

  /* create datatype to represent a username/id pair */
  MPI_Datatype dt;
  create_stridtype(chars, &dt);

  /* get extent of type */
  MPI_Aint lb, extent;
  MPI_Type_get_extent(dt, &lb, &extent);

  /* allocate an array to hold all user names and ids */
  char* buf = NULL;
  size_t bufsize = count * extent;
  if (bufsize > 0) {
    buf = (char*) malloc(bufsize);
  }

  /* copy items from list into array */
  if (rank == 0) {
    strid_serialize(head, chars, buf);
  }

  /* broadcast the array of usernames and ids */
  MPI_Bcast(buf, count, dt, 0, MPI_COMM_WORLD);

  /* set output parameters */
  items->buf   = buf;
  items->count = (uint64_t) count;
  items->chars = (uint64_t) chars; 
  items->dt    = dt;

  /* delete the linked list */
  if (rank == 0) {
    strid_delete(&head, &tail, &count);
  }

  return;
}

/* read group array from file system using getgrent() */
static void get_groups(buf_t* items)
{
  /* initialize output parameters */
  init_buft(items);

  /* get our rank */
  int rank;
  MPI_Comm_rank(MPI_COMM_WORLD, &rank);

  /* rank 0 iterates over users with getpwent */
  strid_t* head = NULL;
  strid_t* tail = NULL;
  int count = 0;
  int chars = 0;
  if (rank == 0) {
    struct group* p;
    while (1) {
      /* get next user, this function can fail so we retry a few times */
      int retries = 3;
retry:
      p = getgrent();
      if (p == NULL) {
        if (errno == EIO || errno == EINTR) {
          retries--;
        } else {
          /* TODO: ERROR! */
          retries = 0;
        }
        if (retries > 0) {
          goto retry;
        }
      }

      if (p != NULL) {
/*
        printf("Group=%s GID=%d\n",
          p->gr_name, p->gr_gid
        );
*/
        char* name  = p->gr_name;
        uint32_t id = p->gr_gid;
        strid_insert(name, id, &head, &tail, &count, &chars);
      } else {
        /* hit the end of the group list */
        endgrent();
        break;
      }
    }

//    printf("Max groupname %d, count %d\n", chars, count);
  }

  /* bcast count and number of chars */
  MPI_Bcast(&count, 1, MPI_INT, 0, MPI_COMM_WORLD);
  MPI_Bcast(&chars, 1, MPI_INT, 0, MPI_COMM_WORLD);

  /* create datatype to represent a username/id pair */
  MPI_Datatype dt;
  create_stridtype(chars, &dt);

  /* get extent of type */
  MPI_Aint lb, extent;
  MPI_Type_get_extent(dt, &lb, &extent);

  /* allocate an array to hold all user names and ids */
  char* buf = NULL;
  size_t bufsize = count * extent;
  if (bufsize > 0) {
    buf = (char*) malloc(bufsize);
  }

  /* copy items from list into array */
  if (rank == 0) {
    strid_serialize(head, chars, buf);
  }

  /* broadcast the array of usernames and ids */
  MPI_Bcast(buf, count, dt, 0, MPI_COMM_WORLD);

  /* set output parameters */
  items->buf   = buf;
  items->count = (uint64_t) count;
  items->chars = (uint64_t) chars; 
  items->dt    = dt;

  /* delete the linked list */
  if (rank == 0) {
    strid_delete(&head, &tail, &count);
  }

  return;
}

static void create_stattype(int chars, MPI_Datatype* dt_stat)
{
  /* build type for file path */
  MPI_Datatype dt_filepath;
  MPI_Type_contiguous(chars, MPI_CHAR, &dt_filepath);

  /* build keysat type */
  int fields;
  MPI_Datatype types[8];
  if (walk_stat) {
    fields = 8;
    types[0] = dt_filepath;  /* file name */
    types[1] = MPI_UINT32_T; /* mode */
    types[2] = MPI_UINT32_T; /* uid */
    types[3] = MPI_UINT32_T; /* gid */
    types[4] = MPI_UINT32_T; /* atime */
    types[5] = MPI_UINT32_T; /* mtime */
    types[6] = MPI_UINT32_T; /* ctime */
    types[7] = MPI_UINT64_T; /* size */
  } else {
    fields = 2;
    types[0] = dt_filepath;  /* file name */
    types[1] = MPI_UINT32_T; /* file type */
  }
  DTCMP_Type_create_series(fields, types, dt_stat);

  MPI_Type_free(&dt_filepath);
  return;
}

static int convert_stat_to_dt(const elem_t* head, buf_t* items)
{
  /* initialize output params */
  items->buf   = NULL;
  items->count = 0;
  items->chars = 0;
  items->dt    = MPI_DATATYPE_NULL;

  /* get our rank and the size of comm_world */
  int rank, ranks;
  MPI_Comm_rank(MPI_COMM_WORLD, &rank);
  MPI_Comm_size(MPI_COMM_WORLD, &ranks);

  /* count number of items in list and identify longest filename */
  int max = 0;
  uint64_t count = 0;
  const elem_t* current = head;
  while (current != NULL) {
    const char* file = current->file;
    size_t len = strlen(file) + 1;
    if (len > max) {
      max = (int) len;
    }
    count++;
    current = current->next;
  }

  /* find smallest length that fits max and consists of integer
   * number of 8 byte segments */
  int max8 = max / 8;
  if (max8 * 8 < max) {
    max8++;
  }
  max8 *= 8;

  /* compute longest file path across all ranks */
  int chars;
  MPI_Allreduce(&max8, &chars, 1, MPI_INT, MPI_MAX, MPI_COMM_WORLD);

  /* nothing to do if no one has anything */
  if (chars <= 0) {
    return 0;
  }

  /* build stat type */
  MPI_Datatype dt;
  create_stattype(chars, &dt);

  /* get extent of stat type */
  MPI_Aint lb, extent;
  MPI_Type_get_extent(dt, &lb, &extent);

  /* allocate buffer */
  size_t bufsize = extent * count;
  void* buf = NULL;
  if (bufsize > 0) {
    buf = malloc(bufsize);
  }

  /* copy stat data into stat datatypes */
  char* ptr = (char*) buf;
  current = list_head;
  while (current != NULL) {
    /* get pointer to file name and stat structure */
    char* file = current->file;
    const struct stat* sb = current->sb;

    /* TODO: copy stat info via function */

    uint32_t* ptr_uint32t;
    uint64_t* ptr_uint64t;

    /* copy in file name */
    strcpy(ptr, file);
    ptr += chars;

    if (walk_stat) {
      /* copy in mode time */
      ptr_uint32t = (uint32_t*) ptr;
      *ptr_uint32t = (uint32_t) sb->st_mode;
      ptr += sizeof(uint32_t);

      /* copy in user id */
      ptr_uint32t = (uint32_t*) ptr;
      *ptr_uint32t = (uint32_t) sb->st_uid;
      ptr += sizeof(uint32_t);

      /* copy in group id */
      ptr_uint32t = (uint32_t*) ptr;
      *ptr_uint32t = (uint32_t) sb->st_gid;
      ptr += sizeof(uint32_t);

      /* copy in access time */
      ptr_uint32t = (uint32_t*) ptr;
      *ptr_uint32t = (uint32_t) sb->st_atime;
      ptr += sizeof(uint32_t);

      /* copy in modify time */
      ptr_uint32t = (uint32_t*) ptr;
      *ptr_uint32t = (uint32_t) sb->st_mtime;
      ptr += sizeof(uint32_t);

      /* copy in create time */
      ptr_uint32t = (uint32_t*) ptr;
      *ptr_uint32t = (uint32_t) sb->st_ctime;
      ptr += sizeof(uint32_t);

      /* copy in size */
      ptr_uint64t = (uint64_t*) ptr;
      *ptr_uint64t = (uint64_t) sb->st_size;
      ptr += sizeof(uint64_t);
    } else {
      /* just have the file type */
      ptr_uint32t = (uint32_t*) ptr;
      *ptr_uint32t = (uint32_t) current->type;
      ptr += sizeof(uint32_t);
    }

    /* go to next element */
    current = current->next;
  }

  /* set output params */
  items->buf   = buf;
  items->count = count;
  items->chars = (uint64_t)chars;
  items->dt    = dt;

  return 0;
}

static void read_cache_v2(
  const char* name,
  MPI_Offset* outdisp,
  MPI_File fh,
  char* datarep,
  buf_t* files,
  uint64_t* outstart,
  uint64_t* outend)
{
  MPI_Status status;

  MPI_Offset disp = *outdisp;

  /* TODO: hacky way to indicate that we just have file names */
  walk_stat = 0;

  /* get our rank */
  int rank, ranks;
  MPI_Comm_rank(MPI_COMM_WORLD, &rank);
  MPI_Comm_size(MPI_COMM_WORLD, &ranks);

  /* rank 0 reads and broadcasts header */
  uint64_t header[4];
  MPI_File_set_view(fh, disp, MPI_UINT64_T, MPI_UINT64_T, datarep, MPI_INFO_NULL);
  if (rank == 0) {
    MPI_File_read_at(fh, disp, header, 4, MPI_UINT64_T, &status);
  }
  MPI_Bcast(header, 4, MPI_UINT64_T, 0, MPI_COMM_WORLD);
  disp += 4 * 8; /* 4 consecutive uint64_t types in external32 */

  uint64_t all_count;
  *outstart     = header[0];
  *outend       = header[1];
  all_count     = header[2];
  files->chars  = header[3];

  /* compute count for each process */
  uint64_t count = all_count / ranks;
  uint64_t remainder = all_count - count * ranks;
  if (rank < remainder) {
    count++;
  }
  files->count = count;

  /* get our offset */
  uint64_t offset;
  MPI_Exscan(&count, &offset, 1, MPI_UINT64_T, MPI_SUM, MPI_COMM_WORLD);
  if (rank == 0) {
    offset = 0;
  }

  /* create file datatype and read in file info if there are any */
  if (all_count > 0 && files->chars > 0) {
    /* create types */
    create_stattype((int)files->chars,   &(files->dt));

    /* get extents */
    MPI_Aint lb_file, extent_file;
    MPI_Type_get_extent(files->dt, &lb_file, &extent_file);

    /* allocate memory to hold data */
    size_t bufsize_file = files->count * extent_file;
    if (bufsize_file > 0) {
      files->buf = (void*) malloc(bufsize_file);
    }

    /* collective read of stat info */
    MPI_File_set_view(fh, disp, files->dt, files->dt, datarep, MPI_INFO_NULL);
    MPI_Offset read_offset = disp + offset * extent_file;
    MPI_File_read_at_all(fh, read_offset, files->buf, (int)files->count, files->dt, &status);
    disp += all_count * extent_file;
  }

  *outdisp = disp;
  return;
}

static void read_cache_v3(
  const char* name,
  MPI_Offset* outdisp,
  MPI_File fh,
  char* datarep,
  buf_t* users,
  buf_t* groups,
  buf_t* files,
  uint64_t* outstart,
  uint64_t* outend)
{
  MPI_Status status;

  MPI_Offset disp = *outdisp;

  /* TODO: hacky way to indicate that we're processing stat data */
  walk_stat = 1;

  /* get our rank */
  int rank, ranks;
  MPI_Comm_rank(MPI_COMM_WORLD, &rank);
  MPI_Comm_size(MPI_COMM_WORLD, &ranks);

  /* rank 0 reads and broadcasts header */
  uint64_t header[8];
  MPI_File_set_view(fh, disp, MPI_UINT64_T, MPI_UINT64_T, datarep, MPI_INFO_NULL);
  if (rank == 0) {
    MPI_File_read_at(fh, disp, header, 8, MPI_UINT64_T, &status);
  }
  MPI_Bcast(header, 8, MPI_UINT64_T, 0, MPI_COMM_WORLD);
  disp += 8 * 8; /* 8 consecutive uint64_t types in external32 */

  uint64_t all_count;
  *outstart     = header[0];
  *outend       = header[1];
  users->count  = header[2];
  users->chars  = header[3];
  groups->count = header[4];
  groups->chars = header[5];
  all_count     = header[6];
  files->chars  = header[7];

  /* compute count for each process */
  uint64_t count = all_count / ranks;
  uint64_t remainder = all_count - count * ranks;
  if (rank < remainder) {
    count++;
  }
  files->count = count;

  /* get our offset */
  uint64_t offset;
  MPI_Exscan(&count, &offset, 1, MPI_UINT64_T, MPI_SUM, MPI_COMM_WORLD);
  if (rank == 0) {
    offset = 0;
  }

  /* read users, if any */
  if (users->count > 0 && users->chars > 0) {
    /* create type */
    create_stridtype((int)users->chars,  &(users->dt));

    /* get extent */
    MPI_Aint lb_user, extent_user;
    MPI_Type_get_extent(users->dt, &lb_user, &extent_user);

    /* allocate memory to hold data */
    size_t bufsize_user = users->count * extent_user;
    if (bufsize_user > 0) {
      users->buf = (void*) malloc(bufsize_user);
    }

    /* read data */
    MPI_File_set_view(fh, disp, users->dt, users->dt, datarep, MPI_INFO_NULL);
    if (rank == 0) {
      MPI_File_read_at(fh, disp, users->buf, (int)users->count, users->dt, &status);
    }
    MPI_Bcast(users->buf, (int)users->count, users->dt, 0, MPI_COMM_WORLD);
    disp += bufsize_user;
  }

  /* read groups, if any */
  if (groups->count > 0 && groups->chars > 0) {
    /* create type */
    create_stridtype((int)groups->chars, &(groups->dt));

    /* get extent */
    MPI_Aint lb_group, extent_group;
    MPI_Type_get_extent(groups->dt, &lb_group, &extent_group);

    /* allocate memory to hold data */
    size_t bufsize_group = groups->count * extent_group;
    if (bufsize_group > 0) {
      groups->buf = (void*) malloc(bufsize_group);
    }

    /* read data */
    MPI_File_set_view(fh, disp, groups->dt, groups->dt, datarep, MPI_INFO_NULL);
    if (rank == 0) {
      MPI_File_read_at(fh, disp, groups->buf, (int)groups->count, groups->dt, &status);
    }
    MPI_Bcast(groups->buf, (int)groups->count, groups->dt, 0, MPI_COMM_WORLD);
    disp += bufsize_group;
  }

  /* read files, if any */
  if (all_count > 0 && files->chars > 0) {
    /* create types */
    create_stattype((int)files->chars,   &(files->dt));

    /* get extents */
    MPI_Aint lb_file, extent_file;
    MPI_Type_get_extent(files->dt, &lb_file, &extent_file);

    /* allocate memory to hold data */
    size_t bufsize_file = files->count * extent_file;
    if (bufsize_file > 0) {
      files->buf = (void*) malloc(bufsize_file);
    }

    /* collective read of stat info */
    MPI_File_set_view(fh, disp, files->dt, files->dt, datarep, MPI_INFO_NULL);
    MPI_Offset read_offset = disp + offset * extent_file;
    MPI_File_read_at_all(fh, read_offset, files->buf, (int)files->count, files->dt, &status);
    disp += all_count * extent_file;
  }

  *outdisp = disp;
  return;
}

static void read_cache(
  const char* name,
  buf_t* users,
  buf_t* groups,
  buf_t* files,
  uint64_t* outstart,
  uint64_t* outend)
{
  /* intialize output variables */
  init_buft(users);
  init_buft(groups);
  init_buft(files);
  *outstart = 0;
  *outend   = 0;

  /* get our rank */
  int rank;
  MPI_Comm_rank(MPI_COMM_WORLD, &rank);

  /* inform user about file name that we're reading */
  if (verbose && rank == 0) {
    printf("Reading from cache file: %s\n", name);
    fflush(stdout);
  }

  /* open file */
  int rc;
  MPI_Status status;
  MPI_File fh;
  char datarep[] = "external32";
  int amode = MPI_MODE_RDONLY;
  rc = MPI_File_open(MPI_COMM_WORLD, (char*)name, amode, MPI_INFO_NULL, &fh);
  if (rc != MPI_SUCCESS) {
    if (rank == 0) {
      printf("Failed to open file %s", name);
    }
    return;
  }

  /* set file view */
  MPI_Offset disp = 0;

  /* rank 0 reads and broadcasts version */
  uint64_t version;
  MPI_File_set_view(fh, disp, MPI_UINT64_T, MPI_UINT64_T, datarep, MPI_INFO_NULL);
  if (rank == 0) {
    MPI_File_read_at(fh, disp, &version, 1, MPI_UINT64_T, &status);
  }
  MPI_Bcast(&version, 1, MPI_UINT64_T, 0, MPI_COMM_WORLD);
  disp += 1 * 8; /* 9 consecutive uint64_t types in external32 */

  if (version == 2) {
    read_cache_v2(name, &disp, fh, datarep, files, outstart, outend);
  } else if (version == 3) {
    read_cache_v3(name, &disp, fh, datarep, users, groups, files, outstart, outend);
  } else {
    /* TODO: unknown file format */
  }

  /* close file */
  MPI_File_close(&fh);

  return;
}

static void print_files(
  const buf_t* users,
  const buf_t* groups,
  const buf_t* files,
  map<uint32_t,string>& user_id2name,
  map<uint32_t,string>& group_id2name)
{
  /* get our rank and the size of comm_world */
  int rank, ranks;
  MPI_Comm_rank(MPI_COMM_WORLD, &rank);
  MPI_Comm_size(MPI_COMM_WORLD, &ranks);

  /* step through and print data */
  if (walk_stat) {
    int index = 0;
    char* ptr = (char*) files->buf;
    while (index < files->count) {
      /* extract stat values from function */

      /* get filename */
      char* file = ptr;
      ptr += files->chars;

      /* get mode */
      uint32_t mode = *(uint32_t*)ptr;
      ptr += sizeof(uint32_t);

      /* get uid */
      uint32_t uid = *(uint32_t*)ptr;
      ptr += sizeof(uint32_t);

      /* get gid */
      uint32_t gid = *(uint32_t*)ptr;
      ptr += sizeof(uint32_t);

      /* get access time */
      uint32_t access = *(uint32_t*)ptr;
      ptr += sizeof(uint32_t);

      /* get modify time */
      uint32_t modify = *(uint32_t*)ptr;
      ptr += sizeof(uint32_t);

      /* get create time */
      uint32_t create = *(uint32_t*)ptr;
      ptr += sizeof(uint32_t);

      /* get size */
      uint64_t size = *(uint64_t*)ptr;
      ptr += sizeof(uint64_t);

      const char* username  = get_name_from_id(uid, users->chars,  user_id2name);
      const char* groupname = get_name_from_id(gid, groups->chars, group_id2name);

      char access_s[30];
      char modify_s[30];
      char create_s[30];
      time_t access_t = (time_t) access;
      time_t modify_t = (time_t) modify;
      time_t create_t = (time_t) create;
      size_t access_rc = strftime(access_s, sizeof(access_s)-1, "%FT%T", localtime(&access_t));
      size_t modify_rc = strftime(modify_s, sizeof(modify_s)-1, "%FT%T", localtime(&modify_t));
      size_t create_rc = strftime(create_s, sizeof(create_s)-1, "%FT%T", localtime(&create_t));
      if (access_rc == 0 || modify_rc == 0 || create_rc == 0) {
        /* error */
        access_s[0] = '\0';
        modify_s[0] = '\0';
        create_s[0] = '\0';
      }

      if (index < 10 || (files->count - index) <= 10) {
        //printf("Rank %d: Mode=%lx UID=%d GUI=%d Access=%lu Modify=%lu Create=%lu Size=%lu File=%s\n",
        //  rank, mode, uid, gid, (unsigned long)access, (unsigned long)modify,
        printf("Rank %d: Mode=%lx UID=%d(%s) GUI=%d(%s) Access=%s Modify=%s Create=%s Size=%lu File=%s\n",
          rank, mode, uid, username, gid, groupname,
          access_s, modify_s, create_s, (unsigned long)size, file
        );
      } else if (index == 10) {
        printf("<snip>\n");
      }

      index++;
    }
  } else {
    int index = 0;
    char* ptr = (char*) files->buf;
    while (index < files->count) {
      /* extract stat values from function */

      /* get filename */
      char* file = ptr;
      ptr += files->chars;

      /* get type */
      uint32_t type = *(uint32_t*)ptr;
      ptr += sizeof(uint32_t);

      if (index < 10 || (files->count - index) <= 10) {
        printf("Rank %d: Type=%d File=%s\n",
          rank, type, file
        );
      } else if (index == 10) {
        printf("<snip>\n");
      }

      index++;
    }
  }

  return;
}

typedef struct rm_elem {
  char* name;
  int depth;
  filetypes type;
  int have_mode;
  mode_t mode;
  struct rm_elem* next;
} rm_elem_t;

static rm_elem_t* rm_head = NULL;
static rm_elem_t* rm_tail = NULL;

/* given path, return level within directory tree */
static int compute_depth(const char* path)
{
    /* TODO: ignore trailing '/' */

    const char* c;
    int depth = 0;
    for (c = path; *c != '\0'; c++) {
        if (*c == '/') {
            depth++;
        }
    }
    return depth;
}

static void create_list(
  const buf_t* users,
  const buf_t* groups,
  const buf_t* files,
  map<uint32_t,string>& user_id2name,
  map<uint32_t,string>& group_id2name)
{
  /* step through and print data */
  if (walk_stat) {
    int index = 0;
    char* ptr = (char*) files->buf;
    while (index < files->count) {
      /* extract stat values from function */

      /* get filename */
      char* file = ptr;
      ptr += files->chars;

      /* get mode */
      uint32_t mode = *(uint32_t*)ptr;
      ptr += sizeof(uint32_t);

      /* get uid */
      uint32_t uid = *(uint32_t*)ptr;
      ptr += sizeof(uint32_t);

      /* get gid */
      uint32_t gid = *(uint32_t*)ptr;
      ptr += sizeof(uint32_t);

      /* get access time */
      uint32_t access = *(uint32_t*)ptr;
      ptr += sizeof(uint32_t);

      /* get modify time */
      uint32_t modify = *(uint32_t*)ptr;
      ptr += sizeof(uint32_t);

      /* get create time */
      uint32_t create = *(uint32_t*)ptr;
      ptr += sizeof(uint32_t);

      /* get size */
      uint64_t size = *(uint64_t*)ptr;
      ptr += sizeof(uint64_t);

      /* create element in remove list */
      rm_elem_t* elem = (rm_elem_t*) malloc(sizeof(rm_elem_t));
      elem->name = strdup(file);
      elem->depth = compute_depth(file);
      elem->type = TYPE_FILE;
      if (S_ISDIR(mode)) {
        elem->type = TYPE_DIR;
      }
      elem->have_mode = 1;
      elem->mode = (mode_t)mode;

      /* attach element to remove list */
      if (rm_head == NULL) {
        rm_head = elem;
      }
      if (rm_tail != NULL) {
        rm_tail->next = elem;
      }
      rm_tail = elem;
      elem->next = NULL;

      index++;
    }
  } else {
    int index = 0;
    char* ptr = (char*) files->buf;
    while (index < files->count) {
      /* extract stat values from function */

      /* get filename */
      char* file = ptr;
      ptr += files->chars;

      /* get type */
      uint32_t type = *(uint32_t*)ptr;
      ptr += sizeof(uint32_t);

      /* create element in remove list */
      rm_elem_t* elem = (rm_elem_t*) malloc(sizeof(rm_elem_t));
      elem->name = strdup(file);
      elem->type = (filetypes)type;
      elem->depth = compute_depth(file);
      elem->have_mode = 0;

      /* attach element to remove list */
      if (rm_head == NULL) {
        rm_head = elem;
      }
      if (rm_tail != NULL) {
        rm_tail->next = elem;
      }
      rm_tail = elem;
      elem->next = NULL;

      index++;
    }
  }

  return;
}

static void free_list()
{
  rm_elem_t* elem = rm_head;
  while (elem != NULL) {
    if (elem->name != NULL) {
      free(elem->name);
    }
    free(elem);
    elem = elem->next;
  }
  rm_head = NULL;
  rm_tail = NULL;
}

static int rm_depth;      /* tracks current level at which to remove items */
static uint64_t rm_count; /* tracks number of items local process has removed */

static void remove_type(char type, const char* name)
{
  if (type == 'd') {
    int rc = rmdir(name);
    if (rc != 0) {
      printf("Failed to rmdir `%s' (errno=%d %s)\n", name, errno, strerror(errno));
    }
  } else if (type == 'f') {
    int rc = unlink(name);
    if (rc != 0) {
      printf("Failed to unlink `%s' (errno=%d %s)\n", name, errno, strerror(errno));
    }
  } else if (type == 'u') {
    int rc = remove(name);
    if (rc != 0) {
      printf("Failed to remove `%s' (errno=%d %s)\n", name, errno, strerror(errno));
    }
  } else {
    /* print error */
    printf("Unknown type=%c name=%s @ %s:%d\n", type, name, __FILE__, __LINE__);
  }

  return;
}

static void remove_process(CIRCLE_handle* handle)
{
  char path[CIRCLE_MAX_STRING_LEN];
  handle->dequeue(path);
  
  char item = path[0];
  char* name = &path[1];
  remove_type(item, name);
  rm_count++;

  return;
}

static void remove_create(CIRCLE_handle* handle)
{
  char path[CIRCLE_MAX_STRING_LEN];

  /* enqueues all items at rm_depth to be deleted */
  const rm_elem_t* elem = rm_head;
  while (elem != NULL) {
    if (elem->depth == rm_depth) {
      if (elem->type == TYPE_DIR) {
        path[0] = 'd';
      } else if (elem->type == TYPE_FILE || elem->type == TYPE_LINK) {
        path[0] = 'f';
      } else {
        path[0] = 'u';
      }
      size_t len = strlen(elem->name) + 2;
      if (len <= CIRCLE_MAX_STRING_LEN) {
        strcpy(&path[1], elem->name);
        handle->enqueue(path);
      } else {
        printf("Filename longer than %lu\n", (unsigned long)CIRCLE_MAX_STRING_LEN);
      }
    }
    elem = elem->next;
  }


  return;
}

/* insert all items to be removed into libcircle for
 * dynamic load balancing */
static void remove_libcircle()
{
  /* initialize libcircle */
  CIRCLE_init(0, NULL, CIRCLE_SPLIT_EQUAL | CIRCLE_CREATE_GLOBAL);

  /* set libcircle verbosity level */
  enum CIRCLE_loglevel loglevel = CIRCLE_LOG_WARN;
  if (verbose) {
//    loglevel = CIRCLE_LOG_INFO;
  }
  CIRCLE_enable_logging(loglevel);

  /* register callbacks */
  CIRCLE_cb_create(&remove_create);
  CIRCLE_cb_process(&remove_process);

  /* run the libcircle job */
  CIRCLE_begin();
  CIRCLE_finalize();

  return;
}

/* for given depth, just remove the files we know about */
static void remove_direct()
{
  /* each process directly removes its elements */
  const rm_elem_t* elem = rm_head;
  while (elem != NULL) {
      if (elem->depth == rm_depth) {
          if (elem->type == TYPE_DIR) {
            int rc = rmdir(elem->name);
            if (rc != 0) {
              printf("Failed to rmdir `%s' (errno=%d %s)\n", elem->name, errno, strerror(errno));
            }
          } else if (elem->type == TYPE_FILE || elem->type == TYPE_LINK) {
            int rc = unlink(elem->name);
            if (rc != 0) {
              printf("Failed to unlink `%s' (errno=%d %s)\n", elem->name, errno, strerror(errno));
            }
          } else {
            int rc = remove(elem->name);
            if (rc != 0) {
              printf("Failed to remove `%s' (errno=%d %s)\n", elem->name, errno, strerror(errno));
            }
          }
          rm_count++;
      }
      elem = elem->next;
  }
  return;
}

/* spread with synchronization */
  /* allreduce to get total count of items */

  /* sort by name */

  /* alltoall to determine which processes to send / recv from */

  /* alltoallv to exchange data */

  /* pt2pt with left and right neighbors to determine if they have the same dirname */

  /* delete what we can witout waiting */

  /* if my right neighbor has same dirname, send him msg when we're done */

  /* if my left neighbor has same dirname, wait for msg */


/* Bob Jenkins one-at-a-time hash: http://en.wikipedia.org/wiki/Jenkins_hash_function */
static uint32_t jenkins_one_at_a_time_hash(const char *key, size_t len)
{
  uint32_t hash, i;
  for(hash = i = 0; i < len; ++i) {
      hash += key[i];
      hash += (hash << 10);
      hash ^= (hash >> 6);
  }
  hash += (hash << 3);
  hash ^= (hash >> 11);
  hash += (hash << 15);
  return hash;
}

static int get_first_nonzero(const int* buf, int size)
{
  int i;
  for (i = 0; i < size; i++) {
    if (buf[i] != 0) {
      return i;
    }
  }
  return -1;
}

/* for given depth, evenly spread the files among processes for
 * improved load balancing */
static void remove_spread()
{
    /* get our rank and number of ranks in job */
    int rank, ranks;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &ranks);
  
    int* sendcounts = (int*) malloc(ranks * sizeof(int));
    int* sendsizes  = (int*) malloc(ranks * sizeof(int));
    int* senddisps  = (int*) malloc(ranks * sizeof(int));
    int* recvsizes  = (int*) malloc(ranks * sizeof(int));
    int* recvdisps  = (int*) malloc(ranks * sizeof(int));

    /* compute number of items we have for this depth */
    uint64_t my_count = 0;
    size_t sendbytes = 0;
    const rm_elem_t* elem = rm_head;
    while (elem != NULL) {
        if (elem->depth == rm_depth) {
            size_t len = strlen(elem->name) + 2;
            sendbytes += len;
            my_count++;
        }
        elem = elem->next;
    }

    /* compute total number of items */
    uint64_t all_count;
    MPI_Allreduce(&my_count, &all_count, 1, MPI_UINT64_T, MPI_SUM, MPI_COMM_WORLD);

    /* get our global offset */
    uint64_t offset;
    MPI_Exscan(&my_count, &offset, 1, MPI_UINT64_T, MPI_SUM, MPI_COMM_WORLD);
    if (rank == 0) {
        offset = 0;
    }

    /* compute the number that each rank should have */
    uint64_t low = all_count / (uint64_t)ranks;
    uint64_t extra = all_count - low * (uint64_t)ranks;

    /* compute number that we'll send to each rank and initialize sendsizes and offsets */
    int i;
    for (i = 0; i < ranks; i++) {
        /* compute starting element id and count for given rank */
        uint64_t start, num;
        if (i < extra) {
            num = low + 1;
            start = i * num;
        } else {
            num = low;
            start = (i - extra) * num + extra * (low + 1);
        }

        /* compute the number of items we'll send to this task */
        uint64_t sendcnt = 0;
        if (my_count > 0) {
            if (start <= offset && offset < start + num) {
                /* this rank overlaps our range,
                 * and its first element comes at or before our first element */
                sendcnt = num - (offset - start);
                if (my_count < sendcnt) {
                    /* the number the rank could receive from us
                     * is more than we have left */
                    sendcnt = my_count;
                }
            } else if (offset < start && start < offset + my_count) {
                /* this rank overlaps our range,
                 * and our first element comes strictly before its first element */
                sendcnt = my_count - (start - offset);
                if (num < sendcnt) {
                    /* the number the rank can receive from us
                     * is less than we have left */
                    sendcnt = num;
                }
            }
        }

        /* record the number of items we'll send to this task */
        sendcounts[i]  = (int) sendcnt;

        /* set sizes and displacements to 0, we'll fix this later */
        sendsizes[i] = 0;
        senddisps[i] = 0;
    }

    /* allocate space */
    char* sendbuf = NULL;
    if (sendbytes > 0) {
        sendbuf = (char*) malloc(sendbytes);
    }

    /* copy data into buffer */
    elem = rm_head;
    int dest = -1;
    int disp = 0;
    while (elem != NULL) {
        if (elem->depth == rm_depth) {
            /* get rank that we're packing data for */
            if (dest == -1) {
              dest = get_first_nonzero(sendcounts, ranks);
              if (dest == -1) {
                /* error */
              }
              /* about to copy first item for this rank,
               * record its displacement */
              senddisps[dest] = disp;
            }

            /* identify region to be sent to rank */
            char* path = sendbuf + disp;

            /* first character encodes item type */
            if (elem->type == TYPE_DIR) {
                path[0] = 'd';
            } else if (elem->type == TYPE_FILE || elem->type == TYPE_LINK) {
                path[0] = 'f';
            } else {
                path[0] = 'u';
            }

            /* now copy in the path */
            strcpy(&path[1], elem->name);

            /* add bytes to sendsizes and increase displacement */
            size_t count = strlen(elem->name) + 2;
            sendsizes[dest] += count;
            disp += count;

            /* decrement the count for this rank */
            sendcounts[dest]--;
            if (sendcounts[dest] == 0) {
              dest = -1;
            }
        }
        elem = elem->next;
    }

    /* compute displacements */
    senddisps[0] = 0;
    for (i = 1; i < ranks; i++) {
        senddisps[i] = senddisps[i-1] + sendsizes[i-1];
    }

    /* alltoall to specify incoming counts */
    MPI_Alltoall(sendsizes, 1, MPI_INT, recvsizes, 1, MPI_INT, MPI_COMM_WORLD);

    /* compute size of recvbuf and displacements */
    size_t recvbytes = 0;
    recvdisps[0] = 0;
    for (i = 0; i < ranks; i++) {
        recvbytes += recvsizes[i];
        if (i > 0) {
            recvdisps[i] = recvdisps[i-1] + recvsizes[i-1];
        }
    }

    /* allocate recvbuf */
    char* recvbuf = NULL;
    if (recvbytes > 0) {
        recvbuf = (char*) malloc(recvbytes);
    }

    /* alltoallv to send data */
    MPI_Alltoallv(
        sendbuf, sendsizes, senddisps, MPI_CHAR,
        recvbuf, recvsizes, recvdisps, MPI_CHAR, MPI_COMM_WORLD
    );

    /* delete data */
    char* item = recvbuf;
    while (item < recvbuf + recvbytes) {
        /* get item name and type */
        char type = item[0];
        char* name = &item[1];

        /* delete item */
        remove_type(type, name);
        rm_count++;

        /* go to next item */
        size_t item_size = strlen(item) + 1;
        item += item_size;
    }

    /* free memory */
    bayer_free(&recvbuf);
    bayer_free(&recvdisps);
    bayer_free(&recvsizes);
    bayer_free(&sendbuf);
    bayer_free(&senddisps);
    bayer_free(&sendsizes);
    bayer_free(&sendcounts);

    return;
}

/* for given depth, hash directory name and map to processes to
 * test whether having all files in same directory on one process
 * matters */
static void remove_map()
{
    /* get our rank and number of ranks in job */
    int rank, ranks;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &ranks);
  
    int* sendsizes  = (int*) malloc(ranks * sizeof(int));
    int* senddisps  = (int*) malloc(ranks * sizeof(int));
    int* sendoffset = (int*) malloc(ranks * sizeof(int));
    int* recvsizes  = (int*) malloc(ranks * sizeof(int));
    int* recvdisps  = (int*) malloc(ranks * sizeof(int));

    /* initialize sendsizes and offsets */
    int i;
    for (i = 0; i < ranks; i++) {
        sendsizes[i]  = 0;
        sendoffset[i] = 0;
    }

    /* compute number of bytes we'll send to each rank */
    size_t sendbytes = 0;
    const rm_elem_t* elem = rm_head;
    while (elem != NULL) {
        if (elem->depth == rm_depth) {
            /* identify a rank responsible for this item */
            char* dir = strdup(elem->name);
            dirname(dir);
            size_t dir_len = strlen(dir);
            uint32_t hash = jenkins_one_at_a_time_hash(dir, dir_len);
            int rank = (int) (hash % (uint32_t)ranks);
            free(dir);

            /* total number of bytes we'll send to each rank and the total overall */
            size_t count = strlen(elem->name) + 2;
            sendsizes[rank] += count;
            sendbytes += count;
        }
        elem = elem->next;
    }

    /* compute displacements */
    senddisps[0] = 0;
    for (i = 1; i < ranks; i++) {
        senddisps[i] = senddisps[i-1] + sendsizes[i-1];
    }

    /* allocate space */
    char* sendbuf = NULL;
    if (sendbytes > 0) {
        sendbuf = (char*) malloc(sendbytes);
    }

    /* TODO: cache results from above */
    /* copy data into buffer */
    elem = rm_head;
    while (elem != NULL) {
        if (elem->depth == rm_depth) {
            /* identify a rank responsible for this item */
            char* dir = strdup(elem->name);
            dirname(dir);
            size_t dir_len = strlen(dir);
            uint32_t hash = jenkins_one_at_a_time_hash(dir, dir_len);
            int rank = (int) (hash % (uint32_t)ranks);
            free(dir);

            /* identify region to be sent to rank */
            size_t count = strlen(elem->name) + 2;
            char* path = sendbuf + senddisps[rank] + sendoffset[rank];

            /* first character encodes item type */
            if (elem->type == TYPE_DIR) {
                path[0] = 'd';
            } else if (elem->type == TYPE_FILE || elem->type == TYPE_LINK) {
                path[0] = 'f';
            } else {
                path[0] = 'u';
            }

            /* now copy in the path */
            strcpy(&path[1], elem->name);

            /* bump up the offset for this rank */
            sendoffset[rank] += count;
        }
        elem = elem->next;
    }

    /* alltoall to specify incoming counts */
    MPI_Alltoall(sendsizes, 1, MPI_INT, recvsizes, 1, MPI_INT, MPI_COMM_WORLD);

    /* compute size of recvbuf and displacements */
    size_t recvbytes = 0;
    recvdisps[0] = 0;
    for (i = 0; i < ranks; i++) {
        recvbytes += recvsizes[i];
        if (i > 0) {
            recvdisps[i] = recvdisps[i-1] + recvsizes[i-1];
        }
    }

    /* allocate recvbuf */
    char* recvbuf = NULL;
    if (recvbytes > 0) {
        recvbuf = (char*) malloc(recvbytes);
    }

    /* alltoallv to send data */
    MPI_Alltoallv(
        sendbuf, sendsizes, senddisps, MPI_CHAR,
        recvbuf, recvsizes, recvdisps, MPI_CHAR, MPI_COMM_WORLD
    );

    /* delete data */
    char* item = recvbuf;
    while (item < recvbuf + recvbytes) {
        /* get item name and type */
        char type = item[0];
        char* name = &item[1];

        /* delete item */
        remove_type(type, name);
        rm_count++;

        /* go to next item */
        size_t item_size = strlen(item) + 1;
        item += item_size;
    }

    /* free memory */
    bayer_free(&recvbuf);
    bayer_free(&recvdisps);
    bayer_free(&recvsizes);
    bayer_free(&sendbuf);
    bayer_free(&sendoffset);
    bayer_free(&senddisps);
    bayer_free(&sendsizes);

    return;
}

/* for each depth, sort files by filename and then remove, to test
 * whether it matters to limit the number of directories each process
 * has to reference (e.g., locking) */
static void remove_sort()
{
    /* get max filename length and count number of items at this depth */
    int max_len = 0;
    uint64_t my_count = 0;
    const rm_elem_t* elem = rm_head;
    while (elem != NULL) {
        if (elem->depth == rm_depth) {
            /* identify a rank responsible for this item */
            int len = (int) strlen(elem->name) + 1;
            if (len > max_len) {
                max_len = len;
            }
            my_count++;
        }
        elem = elem->next;
    }

    /* bail out if total count is 0 */
    int64_t all_count;
    MPI_Allreduce(&my_count, &all_count, 1, MPI_UINT64_T, MPI_SUM, MPI_COMM_WORLD);
    if (all_count == 0) {
        return;
    }

    /* compute max string size */
    int chars;
    MPI_Allreduce(&max_len, &chars, 1, MPI_INT, MPI_MAX, MPI_COMM_WORLD);

    /* create key datatype (filename) and comparison op */
    MPI_Datatype dt_key;
    DTCMP_Op op_str;
    DTCMP_Str_create_ascend(chars, &dt_key, &op_str);

    /* create keysat datatype (filename + type) */
    MPI_Datatype types[2], dt_keysat;
    types[0] = dt_key;
    types[1] = MPI_CHAR;
    DTCMP_Type_create_series(2, types, &dt_keysat);

    /* allocate send buffer */
    char* sendbuf = NULL;
    int sendcount = (int) my_count;
    size_t sendbufsize = sendcount * (chars + 1);
    if (sendbufsize > 0) {
        sendbuf = (char*) malloc(sendbufsize);
    }

    /* copy data into buffer */
    elem = rm_head;
    char* ptr = sendbuf;
    while (elem != NULL) {
        if (elem->depth == rm_depth) {
            /* encode the filename first */
            strcpy(ptr, elem->name);
            ptr += chars;

            /* last character encodes item type */
            if (elem->type == TYPE_DIR) {
                ptr[0] = 'd';
            } else if (elem->type == TYPE_FILE || elem->type == TYPE_LINK) {
                ptr[0] = 'f';
            } else {
                ptr[0] = 'u';
            }
            ptr++;
        }
        elem = elem->next;
    }

    /* sort items */
    void* recvbuf;
    int recvcount;
    DTCMP_Handle handle;
    DTCMP_Sortz(
        sendbuf, sendcount, &recvbuf, &recvcount,
        dt_key, dt_keysat, op_str, DTCMP_FLAG_NONE, MPI_COMM_WORLD, &handle
    );

    /* delete data */
    int delcount = 0;
    ptr = (char*)recvbuf;
    while (delcount < recvcount) {
        /* get item name */
        char* name = ptr;
        ptr += chars;

        /* get item type */
        char type = ptr[0];
        ptr++;

        /* delete item */
        remove_type(type, name);
        rm_count++;
        delcount++;
    }

    /* free output data */
    DTCMP_Free(&handle);

    /* free our send buffer */
    bayer_free(&sendbuf);

    /* free key comparison operation */
    DTCMP_Op_free(&op_str);

    /* free datatypes */
    MPI_Type_free(&dt_keysat);
    MPI_Type_free(&dt_key);

    return;
}

/* iterate through linked list of files and set ownership, timestamps, and permissions
 * starting from deepest level and working backwards */
static void remove_files(
  const buf_t* users,
  const buf_t* groups,
  const buf_t* files,
  map<uint32_t,string>& user_id2name,
  map<uint32_t,string>& group_id2name)
{
    /* get our rank and number of ranks in job */
    int rank, ranks;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &ranks);

    create_list(users, groups, files, user_id2name, group_id2name);

    /* get max depth across all procs */
    int max_depth;
    int depth = -1;
    const rm_elem_t* elem = rm_head;
    while (elem != NULL) {
        if (elem->depth > depth) {
            depth = elem->depth;
        }
        elem = elem->next;
    }
    MPI_Allreduce(&depth, &max_depth, 1, MPI_INT, MPI_MAX, MPI_COMM_WORLD);

    /* from top to bottom, ensure all directories have write bit set */
    for (depth = 0; depth <= max_depth; depth++) {
        elem = rm_head;
        while (elem != NULL) {
            if (elem->depth == depth) {
                if (elem->type == TYPE_DIR) {
                    /* TODO: if we know the mode and if the write bit is set, skip this,
                     * and if we don't have the mode, try to delete, then set perms, then delete again */
                    if (elem->have_mode == 0 || !(elem->mode & S_IWUSR)) {
                        int rc = chmod(elem->name, S_IRWXU);
                        if (rc != 0) {
                          printf("Failed to chmod directory `%s' (errno=%d %s)\n", elem->name, errno, strerror(errno));
                        }
                    }
                }
            }
            elem = elem->next;
        }
        
        /* wait for all procs to finish before we start
         * with files at next level */
        MPI_Barrier(MPI_COMM_WORLD);
    }

    /* now remove files starting from deepest level */
    for (depth = max_depth; depth >= 0; depth--) {
        double start = MPI_Wtime();

        /* remove all files at this level */
        rm_depth = depth;
        rm_count = 0;

        remove_direct();
//        remove_spread();
//        remove_map();
//      remove_sort();
//      TODO: remove spread then sort
//        remove_libcircle();
        
        /* wait for all procs to finish before we start
         * with files at next level */
        MPI_Barrier(MPI_COMM_WORLD);

        double end = MPI_Wtime();

        if (verbose) {
            uint64_t min, max, sum;
            MPI_Allreduce(&rm_count, &min, 1, MPI_UINT64_T, MPI_MIN, MPI_COMM_WORLD);
            MPI_Allreduce(&rm_count, &max, 1, MPI_UINT64_T, MPI_MAX, MPI_COMM_WORLD);
            MPI_Allreduce(&rm_count, &sum, 1, MPI_UINT64_T, MPI_SUM, MPI_COMM_WORLD);
            double rate = 0.0;
            if (end - start > 0.0) {
              rate = (double)sum / (end - start);
            }
            double time = end - start;
            if (rank == 0) {
                printf("level=%d min=%lu max=%lu sum=%lu rate=%f secs=%f\n",
                  depth, (unsigned long)min, (unsigned long)max, (unsigned long)sum, rate, time
                );
                fflush(stdout);
            }
        }
    }

    free_list();

    return;
}

static void print_usage()
{
  printf("\n");
  printf("Usage: drm [options] <path>\n");
  printf("\n");
  printf("Options:\n");
  printf("  -c, --cache <file>  - read/write directories using cache file\n");
  printf("  -l, --lite          - walk file system without stat\n");
  printf("  -v, --verbose       - verbose output\n");
  printf("  -h, --help          - print usage\n");
  printf("\n");
  fflush(stdout);
  return;
}

int main(int argc, char **argv)
{
  /* initialize MPI */
  MPI_Init(&argc, &argv);

  /* get our rank and the size of comm_world */
  int rank, ranks;
  MPI_Comm_rank(MPI_COMM_WORLD, &rank);
  MPI_Comm_size(MPI_COMM_WORLD, &ranks);

  /* TODO: extend options
   *   - allow user to cache scan result in file
   *   - allow user to load cached scan as input
   *
   *   - allow user to filter by user, group, or filename using keyword or regex
   *   - allow user to specify time window
   *   - allow user to specify file sizes
   *
   *   - allow user to sort by different fields
   *   - allow user to group output (sum all bytes, group by user) */

  char* cachename = NULL;
  int walk = 0;

  int option_index = 0;
  static struct option long_options[] = {
    {"cache",    1, 0, 'c'},
    {"lite",     0, 0, 'l'},
    {"help",     0, 0, 'h'},
    {"verbose",  0, 0, 'v'},
    {0, 0, 0, 0}
  };

  int usage = 0;
  while (1) {
    int c = getopt_long(
      argc, argv, "c:lhv",
      long_options, &option_index
    );

    if (c == -1) {
      break;
    }

    switch (c) {
    case 'c':
      cachename = bayer_strdup(optarg, "input cache", __FILE__, __LINE__);
      break;
    case 'l':
      walk_stat = 0;
      break;
    case 'h':
      usage = 1;
      break;
    case 'v':
      verbose = 1;
      break;
    case '?':
      usage = 1;
      break;
    default:
      if (rank == 0) {
        printf("?? getopt returned character code 0%o ??\n", c);
      }
    }
  }

  /* paths to walk come after the options */
  char* target = NULL;
  if (optind < argc) {
    /* got a path to walk */
    walk = 1;

    /* get absolute path and remove ".", "..", consecutive "/",
     * and trailing "/" characters */
    char* path = argv[optind];
    target = bayer_path_strdup_abs_reduce_str(path);

    /* currently only allow one path */
    if (argc - optind > 1) {
      usage = 1;
    }
  } else {
    /* if we're not walking, we must be reading,
     * and for that we need a file */
    if (cachename == NULL) {
      usage = 1;
    }
  }

  if (usage) {
    if (rank == 0) {
      print_usage();
    }
    MPI_Finalize();
    return 0;
  }

  /* TODO: check stat fields fit within MPI types */
  // if (sizeof(st_uid) > uint64_t) error(); etc...

  /* initialize our sorting library */
  DTCMP_Init();

  uint64_t all_count = 0;
  uint64_t walk_start, walk_end;

  /* initialize users, groups, and files */
  buf_t users, groups, files;
  init_buft(&users);
  init_buft(&groups);
  init_buft(&files);

  map<string,uint32_t> user_name2id;
  map<uint32_t,string> user_id2name;
  map<string,uint32_t> group_name2id;
  map<uint32_t,string> group_id2name;

  if (walk) {
    /* we lookup users and groups first in case we can use
     * them to filter the walk */
    if (walk_stat) {
      get_users(&users);
      get_groups(&groups);
      create_maps(&users, user_name2id, user_id2name);
      create_maps(&groups, group_name2id, group_id2name);
    }

    time_t walk_start_t = time(NULL);
    if (walk_start_t == (time_t)-1) {
      /* TODO: ERROR! */
    }
    walk_start = (uint64_t) walk_start_t;

    /* report walk count, time, and rate */
    if (verbose && rank == 0) {
      char walk_s[30];
      size_t rc = strftime(walk_s, sizeof(walk_s)-1, "%FT%T", localtime(&walk_start_t));
      if (rc == 0) {
        walk_s[0] = '\0';
      }
      printf("%s: Walking directory: %s\n", walk_s, target);
      fflush(stdout);
    }

    /* walk file tree and record stat data for each file */
    double start_walk = MPI_Wtime();
    dftw(target, record_info);
    double end_walk = MPI_Wtime();

    time_t walk_end_t = time(NULL);
    if (walk_end_t == (time_t)-1) {
      /* TODO: ERROR! */
    }
    walk_end = (uint64_t) walk_end_t;

    /* convert stat structs to datatypes */
    convert_stat_to_dt(list_head, &files);

    /* free linked list */
    elem_t* current = list_head;
    while (current != NULL) {
      elem_t* next = current->next;
      free(current->file);
      if (current->sb != NULL) {
        free(current->sb);
      }
      free(current);
      current = next;
    }

    /* get total file count */
    uint64_t my_count = files.count;
    MPI_Allreduce(&my_count, &all_count, 1, MPI_UINT64_T, MPI_SUM, MPI_COMM_WORLD);

    /* report walk count, time, and rate */
    if (verbose && rank == 0) {
      double time = end_walk - start_walk;
      double rate = 0.0;
      if (time > 0.0) {
        rate = ((double)all_count) / time;
      }
      printf("Walked %lu files in %f seconds (%f files/sec)\n",
        all_count, time, rate
      );
    }
  } else {
    /* read data from cache file */
    double start_read = MPI_Wtime();
    read_cache(cachename, &users, &groups, &files, &walk_start, &walk_end);
    double end_read = MPI_Wtime();

    if (walk_stat) {
      create_maps(&users, user_name2id, user_id2name);
      create_maps(&groups, group_name2id, group_id2name);
    }

    /* get total file count */
    uint64_t my_count = files.count;
    MPI_Allreduce(&my_count, &all_count, 1, MPI_UINT64_T, MPI_SUM, MPI_COMM_WORLD);

    /* report read count, time, and rate */
    if (verbose && rank == 0) {
      double time = end_read - start_read;
      double rate = 0.0;
      if (time > 0.0) {
        rate = ((double)all_count) / time;
      }
      printf("Read %lu files in %f seconds (%f files/sec)\n",
        all_count, time, rate
      );
    }
  }

  /* TODO: remove files */
  double start_remove = MPI_Wtime();
  remove_files(&users, &groups, &files, user_id2name, group_id2name);
  double end_remove = MPI_Wtime();
  /* report read count, time, and rate */
  if (verbose && rank == 0) {
    double time = end_remove - start_remove;
    double rate = 0.0;
    if (time > 0.0) {
      rate = ((double)all_count) / time;
    }
    printf("Removed %lu files in %f seconds (%f files/sec)\n",
      all_count, time, rate
    );
  }

  /* free users, groups, and files objects */
  free_buft(&users);
  free_buft(&groups);
  free_buft(&files);

  bayer_free(&cachename);

  /* shut down the sorting library */
  DTCMP_Finalize();

  /* free target directory */
  bayer_free(&target);

  /* shut down MPI */
  MPI_Finalize();

  return 0;
}