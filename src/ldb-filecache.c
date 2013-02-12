/***
  This file is part of fusedav.

  fusedav is free software; you can redistribute it and/or modify it
  under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2 of the License, or
  (at your option) any later version.

  fusedav is distributed in the hope that it will be useful, but WITHOUT
  ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
  or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public
  License for more details.

  You should have received a copy of the GNU General Public License
  along with fusedav; if not, write to the Free Software Foundation,
  Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
***/

/* History:
 * JB 20130205: removed filename from sdata structure; its use was superceded.
 * We used to not populate the ldb cache until sync; but recently started
 * to populate on file creation, at which point cache filename goes into the
 * pdata structure.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <inttypes.h>
#include <time.h>
#include <utime.h>
#include <dirent.h>
#include <string.h>
#include <malloc.h>
#include <pthread.h>
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <sys/file.h>

#include "ldb-filecache.h"
#include "statcache.h"
#include "fusedav.h"
#include "log.h"

#include <ne_uri.h>

#define REFRESH_INTERVAL 3

// Remove filecache files older than 8 days
#define AGE_OUT_THRESHOLD 691200

// Entries for stat and file cache are in the ldb cache; fc: designates filecache entries
static const char * filecache_prefix = "fc:";

typedef int fd_t;

// Session data
struct ldb_filecache_sdata {
    fd_t fd;
    bool readable;
    bool writable;
    bool modified;
};

// FIX ME Where to find ETAG_MAX?
#define ETAG_MAX 256

// Persistent data stored in leveldb
struct ldb_filecache_pdata {
    char filename[PATH_MAX];
    char etag[ETAG_MAX + 1];
    time_t last_server_update;
};

int ldb_filecache_init(char *cache_path) {
    char path[PATH_MAX];
    snprintf(path, PATH_MAX, "%s/files", cache_path);
    if (mkdir(cache_path, 0770) == -1) {
        if (errno != EEXIST) {
            log_print(LOG_ERR, "Cache Path %s could not be created.", cache_path);
            return -1;
        }
    }
    if (mkdir(path, 0770) == -1) {
        if (errno != EEXIST) {
            log_print(LOG_ERR, "Path %s could not be created.", path);
            return -1;
        }
    }
    return 0;
}

// Allocates a new string.
static char *path2key(const char *path) {
    char *key = NULL;
    asprintf(&key, "%s%s", filecache_prefix, path);
    return key;
}

// get an entry from the ldb cache
static struct ldb_filecache_pdata *ldb_filecache_pdata_get(ldb_filecache_t *cache, const char *path) {
    struct ldb_filecache_pdata *pdata = NULL;
    char *key;
    leveldb_readoptions_t *options;
    size_t vallen;
    char *errptr = NULL;

    log_print(LOG_DEBUG, "Entered ldb_filecache_pdata_get: path=\"%s\"", path);

    key = path2key(path);

    log_print(LOG_DEBUG, "Entered ldb_filecache_pdata_get: key=\"%s\"", key);
    options = leveldb_readoptions_create();
    pdata = (struct ldb_filecache_pdata *) leveldb_get(cache, options, key, strlen(key) + 1, &vallen, &errptr);
    leveldb_readoptions_destroy(options);
    free(key);

    if (errptr != NULL) {
        log_print(LOG_ERR, "leveldb_get error: %s", errptr);
        free(errptr);
        return NULL;
    }

    if (!pdata) {
        log_print(LOG_DEBUG, "ldb_filecache_pdata_get miss on path: %s", path);
        return NULL;
    }

    if (vallen != sizeof(struct ldb_filecache_pdata)) {
        log_print(LOG_ERR, "Length %lu is not expected length %lu.", vallen, sizeof(struct ldb_filecache_pdata));
    }

    log_print(LOG_DEBUG, "Returning from ldb_filecache_pdata_get: path=%s :: cachefile=%s", path, pdata->filename);

    return pdata;
}

// deletes entry from ldb cache
int ldb_filecache_delete(ldb_filecache_t *cache, const char *path) {
    leveldb_writeoptions_t *options;
    char *key;
    int ret = 0;
    char *errptr = NULL;
    struct ldb_filecache_pdata *pdata;

    log_print(LOG_DEBUG, "ldb_filecache_delete: path (%s).", path);

    pdata = ldb_filecache_pdata_get(cache, path);

    if (pdata) log_print(LOG_DEBUG, "ldb_filecache_delete: filename (%s).", pdata->filename);
    else log_print(LOG_DEBUG, "ldb_filecache_delete: pdata NULL for (%s).", path);

    key = path2key(path);
    log_print(LOG_DEBUG, "ldb_filecache_delete: key (%s).", key);
    options = leveldb_writeoptions_create();
    leveldb_delete(cache, options, key, strlen(key) + 1, &errptr);
    leveldb_writeoptions_destroy(options);
    free(key);

    if (pdata) {
        unlink(pdata->filename);
        log_print(LOG_DEBUG, "ldb_filecache_delete: unlinking %s", pdata->filename);
        free(pdata);
    }

    if (errptr != NULL) {
        log_print(LOG_ERR, "ERROR: leveldb_delete: %s", errptr);
        free(errptr);
        ret = -1;
    }

    return ret;
}

// creates a new cache file
static int new_cache_file(const char *cache_path, char *cache_file_path, fd_t *fd) {
    snprintf(cache_file_path, PATH_MAX, "%s/files/fusedav-cache-XXXXXX", cache_path);
    log_print(LOG_DEBUG, "Using pattern %s", cache_file_path);
    if ((*fd = mkstemp(cache_file_path)) < 0) {
        log_print(LOG_ERR, "new_cache_file: Failed mkstemp: errno = %d %s", errno, strerror(errno));
        return -1;
    }

    log_print(LOG_DEBUG, "new_cache_file: mkstemp fd=%d :: %s", *fd, cache_file_path);
    return 0;
}

// adds an entry to the ldb cache
static int ldb_filecache_pdata_set(ldb_filecache_t *cache, const char *path, const struct ldb_filecache_pdata *pdata) {
    leveldb_writeoptions_t *options;
    char *errptr = NULL;
    char *key;
    int ret = -1;

    if (!pdata) {
        log_print(LOG_ERR, "ldb_filecache_pdata_set NULL pdata");
        goto finish;
    }

    log_print(LOG_DEBUG, "ldb_filecache_pdata_set: path=%s ; cachefile=%s", path, pdata->filename);

    key = path2key(path);
    options = leveldb_writeoptions_create();
    leveldb_put(cache, options, key, strlen(key) + 1, (const char *) pdata, sizeof(struct ldb_filecache_pdata), &errptr);
    leveldb_writeoptions_destroy(options);

    free(key);

    if (errptr != NULL) {
        log_print(LOG_ERR, "leveldb_set error: %s", errptr);
        free(errptr);
        goto finish;
    }

    ret = 0;

finish:

    return ret;
}

// Create a new file to write into and set values
static int create_file(struct ldb_filecache_sdata *sdata, const char *cache_path,
        ldb_filecache_t *cache, const char *path) {

    struct stat_cache_value value;
    struct ldb_filecache_pdata *pdata;

    log_print(LOG_DEBUG, "create_file: on %s", path);
    sdata->modified = true;
    sdata->writable = true;

    // Prepopulate filecache.
    pdata = malloc(sizeof(struct ldb_filecache_pdata));
    if (pdata == NULL) {
        log_print(LOG_ERR, "create_file: malloc returns NULL for pdata");
        return -1;
    }
    memset(pdata, 0, sizeof(struct ldb_filecache_pdata));
    if (new_cache_file(cache_path, pdata->filename, &sdata->fd) < 0) {
        log_print(LOG_ERR, "ldb_filecache_open: Failed on new_cache_file");
        return -1;
    }

    // Prepopulate stat cache.
    value.st.st_mode = 0660 | S_IFREG;
    value.st.st_nlink = 1;
    value.st.st_size = 0;
    value.st.st_atime = time(NULL);
    value.st.st_mtime = value.st.st_atime;
    value.st.st_ctime = value.st.st_mtime;
    value.st.st_blksize = 0;
    value.st.st_blocks = 8;
    value.st.st_uid = getuid();
    value.st.st_gid = getgid();
    value.prepopulated = false;
    stat_cache_value_set(cache, path, &value);
    log_print(LOG_DEBUG, "create_file: Updated stat cache for %d : %s : %s.", sdata->fd, path, pdata->filename);

    pdata->last_server_update = time(NULL);
    log_print(LOG_DEBUG, "create_file: Updating file cache for %d : %s : %s : timestamp %ul.", sdata->fd, path, pdata->filename, pdata->last_server_update);
    ldb_filecache_pdata_set(cache, path, pdata);
    free(pdata);

    return 0;
}

// Get a file descriptor pointing to the latest full copy of the file.
static fd_t ldb_get_fresh_fd(ne_session *session, ldb_filecache_t *cache,
        const char *cache_path, const char *path, struct ldb_filecache_pdata *pdata, int flags) {
    fd_t ret_fd = -EBADFD;
    int code;
    ne_request *req = NULL;
    int ne_ret;

    if (pdata != NULL)
        log_print(LOG_DEBUG, "ldb_get_fresh_fd: file found in cache: %s::%s", path, pdata->filename);

    // Is it usable as-is?
    // We should have guaranteed that if O_TRUNC is specified and pdata is NULL we don't get here.
    // For O_TRUNC, we just want to open a truncated cache file and not bother getting a copy from
    // the server.
    // If not O_TRUNC, but the cache file is fresh, just reuse it without going to the server.
    if (pdata != NULL && ( (flags & O_TRUNC) || ((time(NULL) - pdata->last_server_update) <= REFRESH_INTERVAL))) {
        log_print(LOG_DEBUG, "ldb_get_fresh_fd: file is fresh or being truncated: %s::%s", path, pdata->filename);
        // Open first with O_TRUNC off, if it was set
        ret_fd = open(pdata->filename, flags & ~O_TRUNC);
        if (ret_fd < 0) {
            if (flags & O_TRUNC) {
                log_print(LOG_ERR, "ldb_get_fresh_fd: open on O_TRUNC returns < 0 on \"%s\": errno: %d, %s", pdata->filename, errno, strerror(errno));
            }
            else {
                log_print(LOG_ERR, "ldb_get_fresh_fd: open on fresh file returns < 0 on \"%s\": errno: %d, %s", pdata->filename, errno, strerror(errno));
            }
            // @TODO: if we get ENOENT, then just create a new cache file and change pdata->filename to its name
        }
        if (flags & O_TRUNC) {
            log_print(LOG_DEBUG, "ldb_get_fresh_fd: acquiring exclusive file lock on fd %d:%s::%s", ret_fd, path, pdata->filename);
            if (flock(ret_fd, LOCK_SH)) {
                log_print(LOG_WARNING, "ldb_get_fresh_fd: error obtaining shared file lock on fd %d:%s::%s", ret_fd, path, pdata->filename);
            }
            log_print(LOG_DEBUG, "ldb_get_fresh_fd: acquired shared file lock on fd %d:%s::%s", ret_fd, path, pdata->filename);
            log_print(LOG_DEBUG, "ldb_get_fresh_fd: truncating fd %d:%s::%s", ret_fd, path, pdata->filename);
            if (ftruncate(ret_fd, 0)) {
                log_print(LOG_WARNING, "ldb_get_fresh_fd: ftruncate failed; errno %d %s -- %d:%s::%s", errno, strerror(errno), ret_fd, path, pdata->filename);
            }
            log_print(LOG_DEBUG, "ldb_get_fresh_fd: releasing shared file lock on fd %d:%s::%s", ret_fd, path, pdata->filename);
            if (flock(ret_fd, LOCK_UN)) {
                log_print(LOG_WARNING, "ldb_get_fresh_fd: error releasing shared file lock on fd %d:%s::%s", ret_fd, path, pdata->filename);
            }
        }
        else {
            log_print(LOG_DEBUG, "ldb_get_fresh_fd: O_TRUNC not specified on fd %d:%s::%s", ret_fd, path, pdata->filename);
        }

        // We're done; no need to access the server...
        goto finish;
    }

    req = ne_request_create(session, "GET", path);
    if (!req) {
        log_print(LOG_ERR, "ldb_get_fresh_fd: Failed ne_request_create on GET on %s", path);
        goto finish;
    }

    // If we have stale cache data, set a header to aim for a 304.
    if (pdata)
        ne_add_request_header(req, "If-None-Match", pdata->etag);

    do {
        ne_ret = ne_begin_request(req);
        if (ne_ret != NE_OK) {
            log_print(LOG_ERR, "ldb_get_fresh_fd: ne_begin_request is not NE_OK: %d %s",
                ne_ret, ne_get_error(session));
            goto finish;
        }

        // If we get a 304, the cache file has the same contents as the file on the server, so
        // just open the cache file without bothering to re-GET the contents from the server.
        // If we get a 200, the cache file is stale and we need to update its contents from
        // the server.
        // We should not get a 404 here; either the open included O_CREAT and we create a new
        // file, or the getattr/get_stat calls in fusedav.c should have detected the file was
        // missing and handled it there.
        code = ne_get_status(req)->code;
        if (code == 304) {
            log_print(LOG_DEBUG, "Got 304 on %s with etag %s", path, pdata->etag);

            // Gobble up any remaining data in the response.
            ne_discard_response(req);

            if (pdata != NULL) {
                // Mark the cache item as revalidated at the current time.
                pdata->last_server_update = time(NULL);
                log_print(LOG_DEBUG, "ldb_get_fresh_fd: Updating file cache on 304 for %s : %s : timestamp: %ul.", path, pdata->filename, pdata->last_server_update);
                ldb_filecache_pdata_set(cache, path, pdata);

                ret_fd = open(pdata->filename, flags);
                if (ret_fd < 0) {
                    log_print(LOG_ERR, "ldb_get_fresh_fd: open for 304 on %s with flags %x and etag %s returns < 0: errno: %d, %s", pdata->filename, flags, pdata->etag, errno, strerror(errno));
                    // @TODO: if we get ENOENT, then just create a new cache file and change pdata->filename to its name
                }
                else {
                    log_print(LOG_DEBUG, "ldb_get_fresh_fd: open for 304 on %s with flags %x succeeded; fd %d", pdata->filename, flags, ret_fd);
                }
            }
            else {
                log_print(LOG_WARNING, "ldb_get_fresh_fd: Got 304 without If-None-Match");
            }
        }
        else if (code == 200) {
            // Archive the old temp file path for unlinking after replacement.
            char old_filename[PATH_MAX];
            bool unlink_old = false;
            const char *etag = NULL;

            if (pdata == NULL) {
                pdata = malloc(sizeof(struct ldb_filecache_pdata));
                if (pdata == NULL) {
                    log_print(LOG_ERR, "ldb_get_fresh_fd: malloc returns NULL for pdata");
                    ne_end_request(req);
                    goto finish;
                }
                memset(pdata, 0, sizeof(struct ldb_filecache_pdata));
            }
            else {
                strncpy(old_filename, pdata->filename, PATH_MAX);
                unlink_old = true;
            }

            // Fill in ETag.
            etag = ne_get_response_header(req, "ETag");
            if (etag != NULL) {
                log_print(LOG_DEBUG, "Got ETag: %s", etag);
                strncpy(pdata->etag, etag, ETAG_MAX);
                pdata->etag[ETAG_MAX] = '\0'; // length of etag is ETAG_MAX + 1 to accomodate this null terminator
            }
            else {
                log_print(LOG_DEBUG, "Got no ETag in response.");
                pdata->etag[0] = '\0';
            }

            // Create a new temp file and read the file content into it.
            // @TODO: Set proper flags? Or enforce in fusedav.c?
            if (new_cache_file(cache_path, pdata->filename, &ret_fd) < 0) {
                log_print(LOG_ERR, "ldb_get_fresh_fd: new_cache_file returns < 0");
                ne_end_request(req);
                goto finish;
            }
            ne_read_response_to_fd(req, ret_fd);

            // Point the persistent cache to the new file content.
            pdata->last_server_update = time(NULL);
            log_print(LOG_DEBUG, "ldb_get_fresh_fd: Updating file cache on 200 for %s : %s : timestamp: %ul.", path, pdata->filename, pdata->last_server_update);
            ldb_filecache_pdata_set(cache, path, pdata);

            // Unlink the old cache file, which the persistent cache
            // no longer references. This will cause the file to be
            // deleted once no more file descriptors reference it.
            if (unlink_old) {
                unlink(old_filename);
                log_print(LOG_DEBUG, "ldb_get_fresh_fd: 200: unlink old filename %s", old_filename);
            }
        }
        else if (code == 404) {
            log_print(LOG_WARNING, "ldb_get_fresh_fd: File expected to exist returns 404.");
            ret_fd = -ENOENT;
        }
        else {
            // Not sure what to do here; goto finish, or try the loop another time?
            log_print(LOG_WARNING, "ldb_get_fresh_fd: returns %d; expected 304 or 200", code);
        }

        ne_ret = ne_end_request(req);
    } while (ne_ret == NE_RETRY);

    finish:
        if (req != NULL)
            ne_request_destroy(req);
        return ret_fd;
}

// top-level open call
int ldb_filecache_open(char *cache_path, ldb_filecache_t *cache, const char *path, struct fuse_file_info *info) {
    ne_session *session;
    struct ldb_filecache_pdata *pdata = NULL;
    struct ldb_filecache_sdata *sdata = NULL;
    int ret = -EBADF;
    int flags = info->flags;

    log_print(LOG_DEBUG, "ldb_filecache_open: %s", path);

    if (!(session = session_get(1))) {
        ret = -EIO;
        log_print(LOG_ERR, "ldb_filecache_open: Failed to get session");
        goto fail;
    }

    // Allocate and zero-out a session data structure.
    sdata = malloc(sizeof(struct ldb_filecache_sdata));
    if (sdata == NULL) {
        log_print(LOG_ERR, "ldb_filecache_open: Failed to malloc sdata");
        goto fail;
    }
    memset(sdata, 0, sizeof(struct ldb_filecache_sdata));

    // If open is called twice, both times with O_CREAT, fuse does not pass O_CREAT
    // the second time. (Unlike on a linux file system, where the second time open
    // is called with O_CREAT, the flag is there but is ignored.) So O_CREAT here
    // means new file.

    // If O_TRUNC is called, it is possible that there is no entry in the filecache.
    // I believe the use-case for this is: prior to conversion to fusedav, a file
    // was on the server. After conversion to fusedav, on first access, it is not
    // in the cache, so we need to create a new cache file for it (or it has aged
    // out of the cache.) If it is in the cache, we let ldb_get_fresh_fd handle it.

    pdata = ldb_filecache_pdata_get(cache, path);
    if ((flags & O_CREAT) || ((flags & O_TRUNC) && (pdata == NULL))) {
        if ((flags & O_CREAT) && (pdata != NULL)) {
            // This will orphan the previous filecache file
            log_print(LOG_WARNING, "ldb_filecache_open: creating a file that already has a cache entry: %s", path);
        }
        ret = create_file(sdata, cache_path, cache, path);
        if (ret < 0) {
            log_print(LOG_ERR, "ldb_filecache_open: Failed on create for %s", path);
            goto fail;
        }
    }
    else {
        // Get a file descriptor pointing to a guaranteed-fresh file.
        sdata->fd = ldb_get_fresh_fd(session, cache, cache_path, path, pdata, flags);
        if (sdata->fd < 0) {
            log_print(LOG_ERR, "ldb_filecache_open: Failed on ldb_get_fresh_fd on %s", path);
            ret = sdata->fd;
            goto fail;
        }
    }

    if (flags & O_RDONLY || flags & O_RDWR) sdata->readable = 1;
    if (flags & O_WRONLY || flags & O_RDWR) sdata->writable = 1;

    if (sdata->fd >= 0) {
        log_print(LOG_DEBUG, "Setting fd to session data structure with fd %d for %s.", sdata->fd, path);
        info->fh = (uint64_t) sdata;
        ret = 0;
        goto finish;
    }

fail:
    log_print(LOG_ERR, "No valid fd set for path %s. Setting fh structure to NULL.", path);
    info->fh = (uint64_t) NULL;

    if (sdata != NULL)
        free(sdata);

    if (pdata != NULL)
        free(pdata);

finish:
    return ret;
}

// top-level read call
ssize_t ldb_filecache_read(struct fuse_file_info *info, char *buf, size_t size, ne_off_t offset) {
    struct ldb_filecache_sdata *sdata = (struct ldb_filecache_sdata *)info->fh;
    ssize_t ret = -1;

    log_print(LOG_DEBUG, "ldb_filecache_read: fd=%d", sdata->fd);

    if ((ret = pread(sdata->fd, buf, size, offset)) < 0) {
        ret = -errno;
        log_print(LOG_ERR, "ldb_filecache_read: error %d; %d %s %d %ld", ret, sdata->fd, buf, size, offset);
        goto finish;
    }

finish:

    // ret is bytes read, or error
    log_print(LOG_DEBUG, "Done reading.");

    return ret;
}

// top-level write call
ssize_t ldb_filecache_write(struct fuse_file_info *info, const char *buf, size_t size, ne_off_t offset) {
    struct ldb_filecache_sdata *sdata = (struct ldb_filecache_sdata *)info->fh;
    ssize_t ret = -1;

    log_print(LOG_DEBUG, "ldb_filecache_write: fd=%d", sdata->fd);

    if (!sdata->writable) {
        errno = EBADF;
        ret = 0;
        log_print(LOG_DEBUG, "ldb_filecache_write: not writable");
        goto finish;
    }

    if ((ret = pwrite(sdata->fd, buf, size, offset)) < 0) {
        ret = -errno;
        log_print(LOG_ERR, "ldb_filecache_write: error %d %d %s::%d %d %ld", ret, errno, strerror(errno), sdata->fd, size, offset);
        goto finish;
    }

    sdata->modified = true;

finish:

    // ret is bytes written

    return ret;
}

// close the file
static int ldb_filecache_close(struct ldb_filecache_sdata *sdata) {
    int ret = -1;

    log_print(LOG_DEBUG, "ldb_filecache_close: fd (%d).", sdata->fd);

    if (sdata->fd >= 0)
        ret = close(sdata->fd);

    log_print(LOG_DEBUG, "ldb_filecache_close: close returns %d %s", ret, strerror(ret));

    if (sdata != NULL)
        free(sdata);

    return 0;
}

// top-level close/release call
int ldb_filecache_release(ldb_filecache_t *cache, const char *path, struct fuse_file_info *info) {
    struct ldb_filecache_sdata *sdata = (struct ldb_filecache_sdata *)info->fh;
    int ret = -1;

    assert(sdata);

    log_print(LOG_DEBUG, "ldb_filecache_release: %s : %d", path, sdata->fd);

    if ((ret = ldb_filecache_sync(cache, path, info, true)) < 0) {
        log_print(LOG_ERR, "ldb_filecache_release: ldb_filecache_sync returns error %d", ret);
        goto finish;
    }

    log_print(LOG_DEBUG, "Done syncing file (%s) for release, calling ldb_filecache_close.", path);

    ret = 0;

finish:

    // close, even on error
    ldb_filecache_close(sdata);

    log_print(LOG_DEBUG, "ldb_filecache_release: Done releasing file (%s).", path);

    return ret;
}

/* PUT's from fd to URI */
/* Our modification to include etag support on put */
static int ne_put_return_etag(ne_session *session, const char *path, int fd, char *etag)
{
    ne_request *req;
    struct stat st;
    int ret;
    const char *value;

    log_print(LOG_DEBUG, "enter: ne_put_return_etag(,%s,,)", path);

    if (fstat(fd, &st)) {
        int errnum = errno;
        char buf[200];
        char msg[256];
        char *error;

        error = ne_strerror(errnum, buf, sizeof buf);
        sprintf(msg, "Could not determine file size: %s", error);
        ne_set_error(session, msg);
        return NE_ERROR;
    }

    req = ne_request_create(session, "PUT", path);

    ne_lock_using_resource(req, path, 0);
    ne_lock_using_parent(req, path);

    ne_set_request_body_fd(req, fd, 0, st.st_size);

    ret = ne_request_dispatch(req);

    if (ret != NE_OK) {
        log_print(LOG_WARNING, "ne_put_return_etag: ne_request_dispatch returns error (%d:%s: fd=%d)", ret, ne_get_error(session), fd);
    }

    if (ret == NE_OK && ne_get_status(req)->klass != 2) {
        ret = NE_ERROR;
    }

    // We continue to PUT the file if etag happens to be NULL; it just
    // means ultimately that it won't trigger a 304 on next access
    if (ret == NE_OK && etag != NULL) {
        value = ne_get_response_header(req, "etag");
        if (value) {
            strncpy(etag, value, ETAG_MAX);
            etag[ETAG_MAX] = '\0';
        }
        log_print(LOG_DEBUG, "PUT returns etag: %s", etag);
    }
    else {
        if (etag != NULL) etag[0] = '\0';
    }
    ne_request_destroy(req);

    return ret;
}

// top-level sync call
int ldb_filecache_sync(ldb_filecache_t *cache, const char *path, struct fuse_file_info *info, bool do_put) {
    struct ldb_filecache_sdata *sdata = (struct ldb_filecache_sdata *)info->fh;
    int ret = -1;
    struct ldb_filecache_pdata *pdata = NULL;
    ne_session *session;
    struct stat_cache_value value;
    char *local_etag;

    assert(sdata);

    log_print(LOG_DEBUG, "ldb_filecache_sync(%s, fd=%d)", path, sdata->fd);

    // @TODO Get a non-blocking flock here; if we get here from someplace other than release, it might be useful
    if (flock(sdata->fd, LOCK_EX)) {
        log_print(LOG_WARNING, "ldb_filecache_release: error acquiring exclusive file lock on fd %d", sdata->fd);
    }

    log_print(LOG_DEBUG, "ldb_filecache_sync: Checking if file (%s) was writable.", path);
    if (!sdata->writable) {
        // errno = EBADF; why?
        ret = 0;
        log_print(LOG_DEBUG, "ldb_filecache_sync: not writable");
        goto finish;
    }

    // Write this data to the persistent cache and update the file cache
    pdata = ldb_filecache_pdata_get(cache, path);
    if (pdata == NULL) {
        log_print(LOG_NOTICE, "ldb_filecache_sync(%s, fd=%d): pdata is NULL; probably unlink intervened", path, sdata->fd);
        // @TODO Create a new pdata here and populate; get a new cache file
    }
    else {
        log_print(LOG_DEBUG, "ldb_filecache_sync(%s, fd=%d): cachefile=%s", path, sdata->fd, pdata->filename);
    }

    if (do_put) {
        log_print(LOG_DEBUG, "ldb_filecache_sync: Checking if file (%s) was modified.", path);
        if (!sdata->modified) {
            ret = 0;
            log_print(LOG_DEBUG, "ldb_filecache_sync: not modified");
            goto finish;
        }

        log_print(LOG_DEBUG, "ldb_filecache_sync: Seeking fd=%d", sdata->fd);
        if (lseek(sdata->fd, 0, SEEK_SET) == (ne_off_t)-1) {
            log_print(LOG_ERR, "ldb_filecache_sync: failed lseek :: %d %d %s", sdata->fd, errno, strerror(errno));
            ret = -1;
            goto finish;
        }

        log_print(LOG_DEBUG, "Getting libneon session.");
        if (!(session = session_get(1))) {
            errno = EIO;
            ret = -1;
            log_print(LOG_ERR, "ldb_filecache_sync: failed session");
            goto finish;
        }

        log_print(LOG_DEBUG, "About to PUT file (%s, fd=%d).", path, sdata->fd);

        if (pdata) local_etag = pdata->etag;
        else local_etag = NULL;
        if (ne_put_return_etag(session, path, sdata->fd, local_etag)) {
            log_print(LOG_ERR, "ldb_filecache_sync: ne_put PUT failed: %s: fd=%d", ne_get_error(session), sdata->fd);
            errno = ENOENT;
            ret = -1;
            goto finish;
        }

        if (pdata) {
            log_print(LOG_DEBUG, "ldb_filecache_sync: PUT successful: %s : %s : timestamp: %ul: etag = %s", path, pdata->filename, pdata->last_server_update, pdata->etag);
        }
        else {
            log_print(LOG_DEBUG, "ldb_filecache_sync: PUT successful: %s", path);
        }

        // If the PUT succeeded, the file isn't locally modified.
        sdata->modified = false;
    }
    else {
        // If we don't PUT the file, we don't have an etag, so zero it out
        if (pdata) strncpy(pdata->etag, "", 1);
    }

    if (pdata) {
        // Point the persistent cache to the new file content.
        pdata->last_server_update = time(NULL);
        log_print(LOG_DEBUG, "ldb_filecache_sync: Updating file cache for %s : %s : timestamp: %ul", path, pdata->filename, pdata->last_server_update);
        ldb_filecache_pdata_set(cache, path, pdata);
    }

    // Update stat cache.
    // @TODO: Use actual mode.
    value.st.st_mode = 0660 | S_IFREG;
    value.st.st_nlink = 1;
    value.st.st_size = lseek(sdata->fd, 0, SEEK_END);
    value.st.st_atime = time(NULL);
    value.st.st_mtime = value.st.st_atime;
    value.st.st_ctime = value.st.st_mtime;
    value.st.st_blksize = 0;
    value.st.st_blocks = 8;
    value.st.st_uid = getuid();
    value.st.st_gid = getgid();
    value.prepopulated = false;
    stat_cache_value_set(cache, path, &value);

    ret = 0;

finish:

    if (pdata) free(pdata);

    log_print(LOG_DEBUG, "ldb_filecache_release: releasing exclusive file lock on fd %d", sdata->fd);
    if (flock(sdata->fd, LOCK_UN)) {
        log_print(LOG_WARNING, "ldb_filecache_sync: error releasing exclusive file lock on fd %d", sdata->fd);
    }

    log_print(LOG_DEBUG, "ldb_filecache_sync: Done syncing file (%s, fd=%d).", path, sdata->fd);

    return ret;
}

// top-level truncate call
int ldb_filecache_truncate(struct fuse_file_info *info, ne_off_t s) {
    struct ldb_filecache_sdata *sdata = (struct ldb_filecache_sdata *)info->fh;
    int ret = -1;

    if ((ret = ftruncate(sdata->fd, s)) < 0) {
        log_print(LOG_ERR, "ldb_filecache_truncate: error on ftruncate %d", ret);
    }

    return ret;
}

// Does *not* allocate a new string.
static const char *key2path(const char *key) {
    char *prefix;
    prefix = strstr(key, filecache_prefix);
    // Looking for "fc:" (filecache_prefix) at the beginning of the key
    if (prefix == key) {
        return key + 3;
    }
    return NULL;
}

<<<<<<< HEAD
static int cleanup_orphans(const char *cache_path, time_t stamped_time) {
    struct dirent *diriter;
    DIR *dir;
    char cachefile_path[PATH_MAX]; // path to file in the cache
    char filecache_path[PATH_MAX]; // path to the file cache itself
    int ret = 0;
    int visited = 0;
    int unlinked = 0;

    snprintf(filecache_path, PATH_MAX, "%s/files", cache_path);
    if ((dir = opendir(filecache_path)) == NULL) {
        log_print(LOG_WARNING, "cleanup_orphans: Can't open filecache directory %s", filecache_path);
        return -1;
    }
=======
    log_print(LOG_DEBUG, "ldb_filecache_fd(%s)", path);

    pdata = ldb_filecache_pdata_get(cache, path);
    if (!pdata) return -1;
    log_print(LOG_DEBUG, "ldb_filecache_fd(cachefile = %s)", pdata->filename);
>>>>>>> 39886006139e57ea34b4fca101ddea3e5fdfe139

    while ((diriter = readdir(dir)) != NULL) {
        struct stat stbuf;
        snprintf(cachefile_path, PATH_MAX , "%s/%s", filecache_path, diriter->d_name) ;
        if (stat(cachefile_path, &stbuf) == -1)
        {
            log_print(LOG_NOTICE, "cleanup_orphans: Unable to stat file: %s", cachefile_path);
            --ret;
            continue;
        }

<<<<<<< HEAD
        if ((stbuf.st_mode & S_IFMT ) == S_IFDIR) {
            // We don't expect directories, but skip them
            if (!cachefile_path[strlen(cachefile_path) - 1] == '.') {
                log_print(LOG_NOTICE, "cleanup_orphans: unexpected directory in filecache: %s", cachefile_path);
                --ret;
            }
            else {
                log_print(LOG_DEBUG, "cleanup_orphans: found . or .. directory: %s", cachefile_path);
            }
            continue;
        }
        else {
            ++visited;
            if (stbuf.st_mtime < stamped_time) {
                if (unlink(cachefile_path)) {
                    log_print(LOG_NOTICE, "cleanup_orphans: failed to unlink %s: %d %s", cachefile_path, errno, strerror(errno));
                    --ret;
                }
                log_print(LOG_DEBUG, "cleanup_orphans: unlinked %s", cachefile_path);
                ++unlinked;
            }
            else {
                log_print(LOG_DEBUG, "cleanup_orphans: didn't unlink %s: %d %d", cachefile_path, stamped_time, stbuf.st_mtime);
            }
        }
    }
    log_print(LOG_INFO, "cleanup_orphans: visited %d files, unlinked %d, and had %d issues", visited, unlinked, ret);

    // ret is effectively the number of unexpected issues we encountered
    return ret;
}

void ldb_filecache_cleanup(ldb_filecache_t *cache, const char *cache_path) {
    leveldb_iterator_t *iter = NULL;
    leveldb_readoptions_t *options;
    const struct ldb_filecache_pdata *pdata;
    size_t klen;
    const char *iterkey;
    const char *path;
    char fname[PATH_MAX];
    time_t starttime;
    int ret;
    //int fd;
    int cached_files = 0;
    int unlinked_files = 0;
    int skipped_files = 0;
    int issues = 0;
    int pruned_files = 0;

    log_print(LOG_DEBUG, "enter: ldb_filecache_cleanup(cache %p)", cache);

    options = leveldb_readoptions_create();
    iter = leveldb_create_iterator(cache, options);
    leveldb_readoptions_destroy(options);

    leveldb_iter_seek(iter, filecache_prefix, 3);

    starttime = time(NULL);

    while (leveldb_iter_valid(iter)) {
        // We need the key to get the path in case we need to remove the entry from the filecache
        iterkey = leveldb_iter_key(iter, &klen);
        path = key2path(iterkey);
        // if path is null, we've gone past the filecache entries
        if (path == NULL) break;
        pdata = (const struct ldb_filecache_pdata *)leveldb_iter_value(iter, &klen);
        log_print(LOG_DEBUG, "ldb_filecache_cleanup: Visiting %s", path);
        if (pdata) {
            ++cached_files;
            // We delete the entry, making pdata invalid, before we might need the filename to unlink,
            // so store it in fname
            strncpy(fname, pdata->filename, PATH_MAX);

            // If the cache file doesn't exist, delete the etnry from the level_db cache
            ret = access(fname, F_OK);
            if (ret) {
                ret = ldb_filecache_delete(cache, path);
                if (ret) {
                    log_print(LOG_WARNING, "ldb_filecache_cleanup: after access failed, failed to remove entry for \"%s\" from ldb cache", path);
                    ++issues;
                }
                else {
                    ++pruned_files;
                }
            }
            else if (starttime - pdata->last_server_update > AGE_OUT_THRESHOLD) {
                //log_print(LOG_DEBUG, "ldb_filecache_cleanup: acquiring exclusive file lock on fd %d:%s::%s", fd, path, pdata->filename);
                //fd = open (fname, O_RDWR);
                //if (flock(fd, LOCK_EX | LOCK_NB)) {
                    //if (errno == EWOULDBLOCK) {
                        //log_print(LOG_NOTICE, "ldb_filecache_cleanup: skipping %d:%s::%s; file still open", fd, path, pdata->filename);
                    //}
                    //else {
                        //log_print(LOG_WARNING, "ldb_filecache_cleanup: error obtaining exclusive file lock on fd %d:%s::%s", fd, path, pdata->filename);
                    //}
                    //++skipped_files;
                    //close(fd);
                //}
                //else {
                    //if (flock(fd, LOCK_UN)) {
                        //log_print(LOG_NOTICE, "ldb_filecache_cleanup: error releasing exclusive file lock on fd %d:%s::%s", fd, path, pdata->filename);
                    //}
                    //close(fd);
                log_print(LOG_INFO, "ldb_filecache_cleanup: Unlinking %s", fname);
                ret = ldb_filecache_delete(cache, path);
                if (ret) {
                    log_print(LOG_WARNING, "ldb_filecache_cleanup: failed to remove entry for \"%s\" from ldb cache", path);
                    log_print(LOG_INFO, "ldb_filecache_cleanup: failed to remove entry \"%s\" from ldb cache", fname);
                    ++issues;
                }
                ret = unlink(fname);
                if (ret) {
                    log_print(LOG_NOTICE, "ldb_filecache_cleanup: failed to unlink %s from ldb cache", fname);
                    ++issues;
                }
                else {
                    ++unlinked_files;
                }
                //}
            }
            else {
                // put a timestamp on the file
                ret = utime(fname, NULL);
                if (ret) {
                    log_print(LOG_NOTICE, "ldb_filecache_cleanup: failed to update timestamp on \"%s\" for \"%s\" from ldb cache: %d - %s", fname, path, errno, strerror(errno));
                }
            }
        }
        else {
            char *base;
            // One of the entries in the db is <path>/files directory itself; ignore this, it's not an error
            // Find the last slash
            base = strrchr(path, '/');
            // if found, move past it
            if (base) ++base;
            // ... if base is NULL(because slash was not found) or it does not equal files, then we have an error
            // (If it does equal files, we have the directory as an entry in the filecache, and we want to ignore it.)
            if (!base || strcmp(base, "files")) {
                log_print(LOG_WARNING, "ldb_filecache_cleanup: pulled NULL pdata out of cache for %s:%s %s", path, iterkey, base);
            }
            else {
                log_print(LOG_DEBUG, "ldb_filecache_cleanup: NULL in cache is directory %s", path);
            }
        }
        leveldb_iter_next(iter);
    }

    leveldb_iter_destroy(iter);

    log_print(LOG_INFO, "ldb_filecache_cleanup: visited %d cache entries; unlinked %d, skipped %d, pruned %d, had %d issues", cached_files, unlinked_files, skipped_files, pruned_files, issues);

    // check filestamps on each file in directory
    ret = cleanup_orphans(cache_path, starttime);
    if (ret) {
        log_print(LOG_NOTICE, "ldb_filecache_cleanup: issues cleaning orphans");
    }
}

int ldb_filecache_fd(ldb_filecache_t *cache, const char *path) {
    int fd;
    struct ldb_filecache_pdata *pdata = NULL;

    log_print(LOG_DEBUG, "ldb_filecache_fd(%s)", path);

    pdata = ldb_filecache_pdata_get(cache, path);
    if (!pdata) return -1;
    log_print(LOG_DEBUG, "ldb_filecache_fd(cachefile = %s)", pdata->filename);

    fd = open(pdata->filename, O_RDONLY);
    return fd;
}

=======
>>>>>>> 39886006139e57ea34b4fca101ddea3e5fdfe139
int ldb_filecache_pdata_move(ldb_filecache_t *cache, const char *old_path, const char *new_path) {
    struct ldb_filecache_pdata *pdata = NULL;
    int ret = -1;

    pdata = ldb_filecache_pdata_get(cache, old_path);

    if (pdata == NULL) {
        log_print(LOG_DEBUG, "ldb_filecache_pdata_move: Path %s does not exist.", old_path);
        goto finish;
    }

    pdata->last_server_update = time(NULL);

    log_print(LOG_DEBUG, "ldb_filecache_pdata_move: Update last_server_update on %s: timestamp: %ul", pdata->filename, pdata->last_server_update);

    if (ldb_filecache_pdata_set(cache, new_path, pdata) < 0) {
        log_print(LOG_ERR, "ldb_filecache_pdata_move: Moving entry from path %s to %s failed. Could not write new entry.", old_path, new_path);
        goto finish;
    }

    ldb_filecache_delete(cache, old_path);

finish:
    if (pdata != NULL)
        free(pdata);
    return ret;
}
