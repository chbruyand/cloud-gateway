/*
 * This file is part of Nuage Labs SAS's Cloud Gateway.
 *
 * Copyright (C) 2011-2017  Nuage Labs SAS
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as
 * published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * In addition, for the avoidance of any doubt, permission is granted to
 * link this program with OpenSSL and to (re)distribute the binaries
 * produced as the result of such linking.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>
 */

#include <errno.h>
#include <sys/stat.h>

#include <cgfs_async.h>
#include <cgfs_file_handler.h>
#include <cgfs_cache.h>
#include <cgfs_utils.h>

#include <cgsmclient/cgsmc_async.h>

#include <cloudutils/cloudutils.h>
#include <cloudutils/cloudutils_aio.h>
#include <cloudutils/cloudutils_file.h>

typedef enum
{
    cgfs_async_request_type_none = 0,
    cgfs_async_request_type_stat,
    cgfs_async_request_type_getattr,
    cgfs_async_request_type_open,
    cgfs_async_request_type_create_and_open,
    cgfs_async_request_type_release,
    cgfs_async_request_type_notify_write,
    cgfs_async_request_type_read,
    cgfs_async_request_type_write,
    cgfs_async_request_type_mkdir,
    cgfs_async_request_type_rmdir,
    cgfs_async_request_type_fsync,
    cgfs_async_request_type_unlink,
    cgfs_async_request_type_rename,
    cgfs_async_request_type_hardlink,
    cgfs_async_request_type_symlink,
    cgfs_async_request_type_readlink,
    cgfs_async_request_type_count
} cgfs_async_request_type;

typedef struct
{
    cgfs_data * data;
    cgfs_file_handler * fh;
    cgfs_inode * inode;
    /* the parent inode is only usable
       for requests passing a parent_ino
       (lookup / create / unlink / mkdir / rmdir / symlink / rename / link )
    */
    cgfs_inode * parent_inode;

    char * name;
    char * new_name;

    union
    {
        cgfs_async_status_cb * status_cb;
        cgfs_async_stat_cb * stat_cb;
        cgfs_async_open_cb * open_cb;
        cgfs_async_create_and_open_cb * create_and_open_cb;
        cgfs_async_read_cb * read_cb;
        cgfs_async_write_cb * write_cb;
        cgfs_async_readlink_cb * readlink_cb;
    };
    cgfs_async_error_cb * error_cb;
    void * cb_data;

    char const * const_buffer;
    char * buffer;
    size_t buffer_size;
    size_t got;
    size_t pos;

    uint64_t ino;
    uint64_t new_parent_ino;
    uid_t uid;
    gid_t gid;
    mode_t mode;
    int flags;
    int fd;

    cgfs_async_request_type type;
} cgfs_async_request;

static void cgfs_async_request_free(cgfs_async_request * this)
{
    if (this != NULL)
    {
        if (this->fh != NULL)
        {
            this->fh = NULL;
        }

        if (this->parent_inode != NULL)
        {
            cgfs_inode_release(this->parent_inode), this->parent_inode = NULL;
        }

        if (this->inode != NULL)
        {
            cgfs_inode_release(this->inode), this->inode = NULL;
        }

        CGUTILS_FREE(this->buffer);
        this->const_buffer = NULL;
        this->buffer_size = 0;

        CGUTILS_FREE(this->name);
        CGUTILS_FREE(this->new_name);

        this->data = NULL;

        this->error_cb = NULL;
        this->cb_data = NULL;

        this->ino = 0;
        this->new_parent_ino = 0;
        this->type = cgfs_async_request_type_none;
        this->mode = 0;
        this->flags = 0;
        this->fd = -1;
        this->got = 0;
        this->pos = 0;

        CGUTILS_FREE(this);
    }
}

static int cgfs_async_request_init(cgfs_data * const data,
                                   uint64_t const ino,
                                   char const * const name,
                                   cgfs_async_request_type const type,
                                   void * const cb_data,
                                   cgfs_async_error_cb * const error_cb,
                                   cgfs_async_request ** const out)
{
    int result = 0;
    cgfs_async_request * request = NULL;

    CGUTILS_ASSERT(data != NULL);
    CGUTILS_ASSERT(out != NULL);
    CGUTILS_ASSERT(type > cgfs_async_request_type_none);
    CGUTILS_ASSERT(type < cgfs_async_request_type_count);

    CGUTILS_ALLOCATE_STRUCT(request);

    if (request != NULL)
    {
        request->data = data;
        request->ino = ino;
        request->cb_data = cb_data;
        request->type = type;
        request->error_cb = error_cb;

        if (name != NULL)
        {
            request->name = cgutils_strdup(name);

            if (request->name == NULL)
            {
                result = ENOMEM;
            }
        }

        if (result != 0)
        {
            cgfs_async_request_free(request), request = NULL;
        }
    }
    else
    {
        result = ENOMEM;
    }

    *out = request;

    return result;
}

static int cgfs_utils_lookup_child(cgfs_data * const data,
                                   uint64_t const parent_ino,
                                   char const * const name,
                                   cgfs_inode ** const inode_out)
{
    CGUTILS_ASSERT(data != NULL);
    CGUTILS_ASSERT(parent_ino > 0);
    CGUTILS_ASSERT(name != NULL);
    CGUTILS_ASSERT(inode_out != NULL);

    int result = cgfs_cache_lookup_child(data->cache,
                                         parent_ino,
                                         name,
                                         inode_out);

    if (result != 0)
    {
        cgfs_inode * parent_inode = NULL;

        result = cgfs_cache_lookup(data->cache,
                                   parent_ino,
                                   &parent_inode);

        if (COMPILER_LIKELY(result == 0))
        {
            cgfs_file_handler const * const fh = cgfs_inode_get_dir_file_handler(parent_inode);

            if (COMPILER_UNLIKELY(fh != NULL))
            {
                uint64_t child_ino = 0;

                CGUTILS_ASSERT(cgfs_file_handler_get_type(fh) == cgfs_file_handler_type_dir);

                result = cgfs_file_handler_dir_get_child_ino(fh,
                                                             name,
                                                             &child_ino);

                if (result == 0)
                {
                    result = cgfs_cache_lookup(data->cache,
                                               child_ino,
                                               inode_out);

                    if (COMPILER_UNLIKELY(result != 0))
                    {
                        result = ENOENT;
                    }
                }
                else
                {
                    result = ENOENT;
                }
            }
            else
            {
                result = ENOENT;
            }

            cgfs_inode_release(parent_inode), parent_inode = NULL;
        }
        else
        {
            result = ENOENT;
        }
    }
    else
    {
        result = ENOENT;
    }

    return result;
}

static void cgfs_async_stat_callback(int const status,
                                     struct stat * st,
                                     void * const cb_data)
{
    int result = status;

    cgfs_async_request * request = cb_data;

    CGUTILS_ASSERT(request != NULL);

    if (COMPILER_LIKELY(status == 0))
    {
        bool first_root_lookup = false;
        CGUTILS_ASSERT(st != NULL);

        if (COMPILER_UNLIKELY(request->type == cgfs_async_request_type_getattr &&
                              request->ino == 1 &&
                              request->data->root_inode_number == 0 &&
                              st->st_ino >= 1))
        {
            request->data->root_inode_number = st->st_ino;
            first_root_lookup = true;
        }

        result = cgfs_cache_lookup(request->data->cache,
                                   st->st_ino,
                                   &(request->inode));

        if (COMPILER_UNLIKELY(result != 0 &&
                              result != ENOENT))
        {
            CGUTILS_WARN("Error looking up inode %"PRIu64" in cache: %d",
                         st->st_ino,
                         result);
            result = ENOENT;
        }

        if (result == ENOENT)
        {
            result = cgfs_inode_init(st,
                                     &(request->inode));

            if (COMPILER_LIKELY(result == 0))
            {
                /* this is kind of ugly, but FUSE does not issue
                   a lookup for the root inode (1). However, it does
                   issue a forget, so we need to keep the lookup counter
                   up-to-date.
                */
                if (COMPILER_UNLIKELY(first_root_lookup == true))
                {
                    cgfs_inode_inc_lookup_count(request->inode);
                }

                result = cgfs_cache_add(request->data->cache,
                                        request->inode);

                if (COMPILER_UNLIKELY(result != 0))
                {
                    CGUTILS_WARN("Error adding inode %"PRIu64" to cache: %d",
                                 cgfs_inode_get_number(request->inode),
                                 result);
                }

                result = 0;
            }
            else
            {
                CGUTILS_ERROR("Error getting inode from stat: %d",
                              result);
            }
        }

        if (COMPILER_LIKELY(result == 0))
        {
            CGUTILS_ASSERT(request->type == cgfs_async_request_type_stat ||
                           request->type == cgfs_async_request_type_getattr);
            CGUTILS_ASSERT(request->stat_cb != NULL);

            (*(request->stat_cb))(request->cb_data,
                                  request->inode);
        }
    }

    if (COMPILER_UNLIKELY(result != 0))
    {
        CGUTILS_ASSERT(request->error_cb != NULL);

        (*(request->error_cb))(result,
                               request->cb_data);
    }

    CGUTILS_FREE(st);

    cgfs_async_request_free(request), request = NULL;
}

void cgfs_async_lookup(cgfs_data * const data,
                       uint64_t parent_ino,
                       char const * const name,
                       cgfs_async_stat_cb * const cb,
                       cgfs_async_error_cb * const error_cb,
                       void * const cb_data)
{
    int result = 0;
    cgfs_inode * inode = NULL;

    CGUTILS_ASSERT(data != NULL);
    CGUTILS_ASSERT(parent_ino > 0);
    CGUTILS_ASSERT(name != NULL);
    CGUTILS_ASSERT(cb != NULL);
    CGUTILS_ASSERT(error_cb != NULL);

    parent_ino = cgfs_translate_inode_number(data,
                                             parent_ino);

    result = cgfs_utils_lookup_child(data,
                                     parent_ino,
                                     name,
                                     &inode);

    if (result == 0)
    {
        (*cb)(cb_data,
              inode);
    }
    else
    {
        cgfs_async_request * request = NULL;

        result = cgfs_async_request_init(data,
                                         parent_ino,
                                         name,
                                         cgfs_async_request_type_stat,
                                         cb_data,
                                         error_cb,
                                         &request);

        if (result == 0)
        {
            request->stat_cb = cb;

            result = cgsmc_async_lookup_child(data->cgsmc_data,
                                              request->ino,
                                              request->name,
                                              &cgfs_async_stat_callback,
                                              request);

            if (COMPILER_UNLIKELY(result != 0))
            {
                if (result != ENAMETOOLONG)
                {
                    CGUTILS_ERROR("Error looking up child %s of inode %"PRIu64": %d",
                                  request->name,
                                  request->ino,
                                  result);
                }

                cgfs_async_request_free(request), request = NULL;
            }
        }
        else
        {
            CGUTILS_ERROR("Error allocating request: %d",
                          result);
        }

        if (COMPILER_UNLIKELY(result != 0))
        {
            (*error_cb)(result,
                        cb_data);
        }
    }

    cgfs_inode_release(inode), inode = NULL;
}

void cgfs_async_getattr(cgfs_data * const data,
                        uint64_t ino,
                        cgfs_async_stat_cb * const cb,
                        cgfs_async_error_cb * const error_cb,
                        void * const cb_data)
{
    int result = 0;
    cgfs_inode * inode = NULL;

    CGUTILS_ASSERT(data != NULL);
    CGUTILS_ASSERT(ino > 0);
    CGUTILS_ASSERT(cb != NULL);
    CGUTILS_ASSERT(error_cb != NULL);

    ino = cgfs_translate_inode_number(data,
                                      ino);

    result = cgfs_cache_lookup(data->cache,
                               ino,
                               &inode);

    if (result == 0)
    {
        (*cb)(cb_data,
              inode);
    }
    else
    {
        cgfs_async_request * request = NULL;

        result = cgfs_async_request_init(data,
                                         ino,
                                         NULL,
                                         cgfs_async_request_type_getattr,
                                         cb_data,
                                         error_cb,
                                         &request);

        if (result == 0)
        {
            request->stat_cb = cb;

            result = cgsmc_async_getattr(data->cgsmc_data,
                                         request->ino,
                                         &cgfs_async_stat_callback,
                                         request);

            if (COMPILER_UNLIKELY(result != 0))
            {
                CGUTILS_ERROR("Error getting attributes of inode %"PRIu64": %d",
                              request->ino,
                              result);

                cgfs_async_request_free(request), request = NULL;
            }
        }
        else
        {
            CGUTILS_ERROR("Error allocating request: %d",
                          result);
        }

        if (COMPILER_UNLIKELY(result != 0))
        {
            (*error_cb)(result,
                        cb_data);
        }
    }

    cgfs_inode_release(inode), inode = NULL;
}

size_t cgfs_async_get_remaining_dir_entries_count(cgfs_data * const data,
                                                  uint64_t const ino,
                                                  cgfs_file_handler * const file_handler,
                                                  size_t const pos)
{
    CGUTILS_ASSERT(data != NULL);
    CGUTILS_ASSERT(ino > 0);
    CGUTILS_ASSERT(file_handler != NULL);
    CGUTILS_ASSERT(cgfs_file_handler_get_type(file_handler) == cgfs_file_handler_type_dir);
    size_t const entries_count = cgfs_file_handler_dir_get_entries_count(file_handler);
    CGUTILS_ASSERT(pos <= entries_count);

    (void) ino;
    (void) data;

    return entries_count - pos;
}

int cgfs_async_get_dir_entry(cgfs_data * const data,
                             uint64_t const ino,
                             cgfs_file_handler * const file_handler,
                             size_t const idx,
                             char const ** const name_out,
                             struct stat const ** const st_out)
{
    int result = 0;
    CGUTILS_ASSERT(data != NULL);
    CGUTILS_ASSERT(ino > 0);
    CGUTILS_ASSERT(file_handler != NULL);
    CGUTILS_ASSERT(cgfs_file_handler_get_type(file_handler) == cgfs_file_handler_type_dir);
    CGUTILS_ASSERT(name_out != NULL);
    CGUTILS_ASSERT(st_out != NULL);
    size_t const entries_count = cgfs_file_handler_dir_get_entries_count(file_handler);
    cgsmc_async_entry const * const entries = cgfs_file_handler_dir_get_entries(file_handler);

    (void) ino;
    (void) data;

    if (COMPILER_LIKELY(idx < entries_count))
    {
        cgsmc_async_entry const * const entry = &(entries[idx]);
        CGUTILS_ASSERT(entry->data != NULL);
        cgfs_inode * inode = entry->data;

        *name_out = entry->name;
        *st_out = &(inode->attr);
    }
    else
    {
        result = ENOENT;
    }

    return result;
}

void cgfs_async_releasedir(cgfs_data * const data,
                           uint64_t const ino,
                           cgfs_file_handler * file_handler)
{
    CGUTILS_ASSERT(data != NULL);
    CGUTILS_ASSERT(ino > 0);
    CGUTILS_ASSERT(file_handler != NULL);
    CGUTILS_ASSERT(cgfs_file_handler_get_type(file_handler) == cgfs_file_handler_type_dir);

    (void) data;
    (void) ino;

    cgfs_file_handler_free(file_handler), file_handler = NULL;
}

size_t cgfs_async_get_remaining_dir_entries_name_len(cgfs_data * const data,
                                                     uint64_t const ino,
                                                     cgfs_file_handler * const file_handler,
                                                     size_t const pos,
                                                     size_t const max_size)
{
    CGUTILS_ASSERT(data != NULL);
    CGUTILS_ASSERT(ino > 0);
    CGUTILS_ASSERT(file_handler != NULL);
    CGUTILS_ASSERT(cgfs_file_handler_get_type(file_handler) == cgfs_file_handler_type_dir);
    size_t const entries_count = cgfs_file_handler_dir_get_entries_count(file_handler);
    CGUTILS_ASSERT(pos <= entries_count);
    size_t result = 0;
    cgsmc_async_entry const * const entries = cgfs_file_handler_dir_get_entries(file_handler);
    CGUTILS_ASSERT(entries != NULL ||
                   entries_count == 0);


    (void) ino;
    (void) data;

    /* no need to compute more entries
       once we have reached max_size */

    for (size_t idx = pos;
         idx < entries_count &&
             result < max_size;
         idx++)
    {
        result += entries[idx].name_len;
    }

    return result;
}

static void cgfs_async_readdir_callback(int const status,
                                        cgsmc_async_entry * entries,
                                        size_t entries_count,
                                        bool const use_dir_index,
                                        void * const cb_data)
{
    int result = status;
    cgfs_async_request * request = cb_data;

    CGUTILS_ASSERT(request != NULL);

    if (COMPILER_LIKELY(status == 0))
    {
        CGUTILS_ASSERT(entries_count == 0 ||
                       entries != NULL);

        cgfs_file_handler * fh = NULL;

        result = cgfs_file_handler_create_dir(entries,
                                              entries_count,
                                              use_dir_index,
                                              request->inode,
                                              &fh);

        if (COMPILER_LIKELY(result == 0))
        {
            for (size_t idx = 0;
                 idx < entries_count &&
                     result == 0;
                 idx++)
            {
                cgsmc_async_entry * entry = &(entries[idx]);
                /* add each entry to the cache */
                cgfs_inode * inode = NULL;

                result = cgfs_cache_lookup(request->data->cache,
                                           entry->st.st_ino,
                                           &inode);

                if (result != 0)
                {
                    result = cgfs_inode_init(&(entry->st),
                                             &inode);

                    if (COMPILER_LIKELY(result == 0))
                    {
                        result = cgfs_cache_add(request->data->cache,
                                                inode);

                        if (COMPILER_UNLIKELY(result != 0))
                        {
                            CGUTILS_ERROR("Error adding inode to cache: %d",
                                         result);
                        }
                    }
                    else
                    {
                        CGUTILS_ERROR("Error getting inode from stat: %d",
                                      result);
                    }
                }

                if (COMPILER_LIKELY(result == 0))
                {
                    entry->data = inode, inode = NULL;
                }
                else
                {
                    cgfs_inode_release(inode), inode = NULL;
                }
            }

            if (COMPILER_LIKELY(result == 0))
            {
                CGUTILS_ASSERT(request->type == cgfs_async_request_type_open);
                CGUTILS_ASSERT(request->open_cb != NULL);

                if (request->inode != NULL &&
                    request->inode->dir_fh == NULL)
                {
                    request->inode->dir_fh = fh;
                }

                (*(request->open_cb))(request->cb_data,
                                      fh);
                fh = NULL;
            }
            else
            {
                cgfs_file_handler_free(fh), fh = NULL;
            }
        }
        else
        {
            CGUTILS_ERROR("Error creating file handler: %d",
                          result);
            CGUTILS_FREE(entries), entries = NULL;
        }
    }

    if (COMPILER_UNLIKELY(result != 0))
    {
        CGUTILS_ASSERT(request->error_cb != NULL);

        (*(request->error_cb))(result,
                               request->cb_data);
    }

    cgfs_async_request_free(request), request = NULL;
}

void cgfs_async_opendir(cgfs_data * const data,
                        uint64_t ino,
                        cgfs_async_open_cb * const cb,
                        cgfs_async_error_cb * const error_cb,
                        void * const cb_data)
{
    int result = 0;
    cgfs_inode * dir_inode = NULL;
    cgfs_async_request * request = NULL;

    CGUTILS_ASSERT(data != NULL);
    CGUTILS_ASSERT(ino > 0);
    CGUTILS_ASSERT(cb != NULL);
    CGUTILS_ASSERT(error_cb != NULL);

    ino = cgfs_translate_inode_number(data,
                                      ino);

    /* do we have the ino in cache ? */
    result = cgfs_cache_lookup(data->cache,
                               ino,
                               &dir_inode);

    if (result == 0)
    {
        if (COMPILER_UNLIKELY(cgfs_inode_is_dir(dir_inode) == false))
        {
            result = ENOTDIR;
        }
    }
    else if (result == ENOENT)
    {
        result = 0;
    }
    else
    {
        CGUTILS_WARN("Error retrieving inode from cache: %d",
                     result);
        result = 0;
    }

    if (COMPILER_LIKELY(result == 0))
    {
        result = cgfs_async_request_init(data,
                                         ino,
                                         NULL,
                                         cgfs_async_request_type_open,
                                         cb_data,
                                         error_cb,
                                         &request);

        if (result == 0)
        {
            request->open_cb = cb;
            request->inode = dir_inode;
            dir_inode = NULL;

            result = cgsmc_async_readdir(data->cgsmc_data,
                                         request->ino,
                                         &cgfs_async_readdir_callback,
                                         request);

            if (COMPILER_UNLIKELY(result != 0))
            {
                CGUTILS_ERROR("Error reading entries of directory/inode %"PRIu64": %d",
                              request->ino,
                              result);

                cgfs_async_request_free(request), request = NULL;
            }
        }
        else
        {
        CGUTILS_ERROR("Error allocating request: %d",
                      result);
        }
    }

    cgfs_inode_release(dir_inode), dir_inode = NULL;

    if (COMPILER_UNLIKELY(result != 0))
    {
        (*error_cb)(result,
                    cb_data);
    }
}

static void cgfs_async_release_callback(int const status,
                                        void * cb_data)
{
    cgfs_async_request * request = cb_data;

    CGUTILS_ASSERT(request != NULL);

    if (COMPILER_UNLIKELY(status != 0))
    {
        CGUTILS_ERROR("Error while releasing inode %"PRIu64": %d",
                      request->ino,
                      status);

    }

    cgfs_file_handler_free(request->fh), request->fh = NULL;
    cgfs_async_request_free(request), request = NULL;
}

static void cgfs_async_notify_of_failed_open_if_needed(cgfs_async_request const * const failed_request)
{
    CGUTILS_ASSERT(failed_request != NULL);

    if (cgfs_utils_writable_flags(failed_request->flags) == true)
    {
        cgfs_async_request * release_request = NULL;

        int result = cgfs_async_request_init(failed_request->data,
                                             failed_request->ino,
                                             NULL,
                                             cgfs_async_request_type_release,
                                             NULL,
                                             NULL,
                                             &release_request);

        if (COMPILER_LIKELY(result == 0))
        {
            result = cgsmc_async_release(failed_request->data->cgsmc_data,
                                         release_request->ino,
                                         false,
                                         &cgfs_async_release_callback,
                                         release_request);

            if (COMPILER_LIKELY(result != 0))
            {
                CGUTILS_ERROR("Error releasing inode %"PRIu64": %d",
                              failed_request->ino,
                              result);

                cgfs_async_request_free(release_request), release_request = NULL;
            }
        }
        else
        {
            CGUTILS_ERROR("Error allocating request: %d",
                          result);
        }
    }
}

static void cgfs_async_create_and_open_callback(int const status,
                                                struct stat * st,
                                                char * file_path,
                                                void * const cb_data)
{
    int result = status;
    cgfs_async_request * request = cb_data;

    CGUTILS_ASSERT(request != NULL);

    if (COMPILER_LIKELY(status == 0))
    {
        if (request->parent_inode != NULL)
        {
            cgfs_utils_update_inode_mtime(request->parent_inode);
        }

        CGUTILS_ASSERT(st != NULL);
        CGUTILS_ASSERT(file_path != NULL);

        result = cgfs_inode_init(st,
                                 &(request->inode));

        if (COMPILER_LIKELY(result == 0))
        {
            cgfs_file_handler * file_handler = NULL;

            result = cgfs_cache_add(request->data->cache,
                                    request->inode);

            if (COMPILER_UNLIKELY(result != 0))
            {
                CGUTILS_WARN("Error adding inode to cache: %d",
                             result);
            }

            result = cgfs_utils_open_file(request->inode,
                                          file_path,
                                          &(request->flags),
                                          &file_handler);

            if (COMPILER_LIKELY(result == 0))
            {
                CGUTILS_ASSERT(request->type == cgfs_async_request_type_create_and_open);
                CGUTILS_ASSERT(request->create_and_open_cb != NULL);

                (*(request->create_and_open_cb))(request->cb_data,
                                                 request->inode,
                                                 file_handler);
            }
        }
        else
        {
            CGUTILS_ERROR("Error getting inode from stat: %d",
                          result);
            CGUTILS_FREE(st);
        }

        if (COMPILER_UNLIKELY(result != 0))
        {
            cgfs_async_notify_of_failed_open_if_needed(request);
        }
    }

    CGUTILS_FREE(file_path);
    CGUTILS_FREE(st);

    if (COMPILER_UNLIKELY(result != 0))
    {
        CGUTILS_ASSERT(request->error_cb != NULL);

        (*(request->error_cb))(result,
                               request->cb_data);
    }

    cgfs_async_request_free(request), request = NULL;
}

void cgfs_async_create_and_open(cgfs_data * const data,
                                uint64_t parent,
                                char const * const name,
                                uid_t const owner,
                                gid_t const group,
                                mode_t const mode,
                                int const flags,
                                cgfs_async_create_and_open_cb * const cb,
                                cgfs_async_error_cb * const error_cb,
                                void * const cb_data)
{
    int result = 0;

    CGUTILS_ASSERT(data != NULL);
    CGUTILS_ASSERT(parent > 0);
    CGUTILS_ASSERT(name != NULL);
    CGUTILS_ASSERT(cb != NULL);
    CGUTILS_ASSERT(error_cb != NULL);

    if (COMPILER_LIKELY(cgfs_utils_check_flags_validity(flags) == true))
    {
        cgfs_inode * parent_inode = NULL;
        cgfs_async_request * request = NULL;

        parent = cgfs_translate_inode_number(data,
                                         parent);

        /* do we have the parent in cache ? */
        result = cgfs_cache_lookup(data->cache,
                                   parent,
                                   &parent_inode);

        if (result == 0)
        {
            if (COMPILER_UNLIKELY(cgfs_inode_is_dir(parent_inode) == false))
            {
                result = ENOTDIR;
            }
        }
        else if (result == ENOENT)
        {
            result = 0;
        }
        else
        {
            CGUTILS_WARN("Error retrieving inode from cache: %d",
                         result);
            result = 0;
        }

        if (COMPILER_LIKELY(result == 0))
        {
            /* TODO / FIXME would be nice to create the inode with a flag indicating
               that it is in creation, keep it in memory (separate rbtree ?)
               and queue lookup requests until it is created. */

            result = cgfs_async_request_init(data,
                                             parent,
                                             name,
                                             cgfs_async_request_type_create_and_open,
                                             cb_data,
                                             error_cb,
                                             &request);

            if (result == 0)
            {
                request->create_and_open_cb = cb;
                request->uid = owner;
                request->gid = group;
                request->mode = mode;
                request->flags = flags;
                request->parent_inode = parent_inode;
                parent_inode = NULL;

                result = cgsmc_async_create_and_open(data->cgsmc_data,
                                                     request->ino,
                                                     request->name,
                                                     request->uid,
                                                     request->gid,
                                                     request->mode,
                                                     request->flags,
                                                     &cgfs_async_create_and_open_callback,
                                                     request);

                if (COMPILER_UNLIKELY(result != 0))
                {
                    if (result != ENAMETOOLONG)
                    {
                        CGUTILS_ERROR("Error creating entry named %s in directory/inode %"PRIu64": %d",
                                      request->name,
                                      request->ino,
                                      result);
                    }

                    cgfs_async_request_free(request), request = NULL;
                }
            }
            else
            {
                CGUTILS_ERROR("Error allocating request: %d",
                              result);
            }
        }

        if (parent_inode != NULL)
        {
            cgfs_inode_release(parent_inode), parent_inode = NULL;
        }
    }
    else
    {
        result = EINVAL;
    }

    if (COMPILER_UNLIKELY(result != 0))
    {
        (*error_cb)(result,
                    cb_data);
    }
}

void cgfs_async_file_handler_release(cgfs_data * const data,
                                     uint64_t const ino,
                                     cgfs_file_handler * file_handler)
{
    CGUTILS_ASSERT(data != NULL);
    CGUTILS_ASSERT(file_handler != NULL);

    if (cgfs_file_handler_file_need_to_notify_release(file_handler) == true)
    {
        cgfs_async_request * request = NULL;

        int result = cgfs_async_request_init(data,
                                             ino,
                                             NULL,
                                             cgfs_async_request_type_release,
                                             NULL,
                                             NULL,
                                             &request);

        if (COMPILER_LIKELY(result == 0))
        {
            request->fh = file_handler;
            file_handler = NULL;
            result = cgsmc_async_release(data->cgsmc_data,
                                         request->ino,
                                         cgfs_file_handler_file_is_dirty(request->fh),
                                         &cgfs_async_release_callback,
                                         request);

            if (COMPILER_UNLIKELY(result != 0))
            {
                CGUTILS_ERROR("Error releasing inode %"PRIu64": %d",
                              request->ino,
                              result);

                cgfs_async_request_free(request), request = NULL;
            }
        }
        else
        {
            CGUTILS_ERROR("Error allocating request: %d",
                          result);
        }
    }

    if (file_handler != NULL)
    {
        cgfs_file_handler_free(file_handler), file_handler = NULL;
    }
}

static void cgfs_async_notify_write_callback(int const status,
                                             void * cb_data)
{
    cgfs_async_request * request = cb_data;

    CGUTILS_ASSERT(request != NULL);

    if (COMPILER_LIKELY(status == 0))
    {
        CGUTILS_ASSERT(request->inode != NULL);
        cgfs_inode_update_dirty_notification(request->inode);
    }
    else
    {
        CGUTILS_ERROR("Error while notifying write to inode %"PRIu64": %d",
                      request->ino,
                      status);
    }

    cgfs_async_request_free(request), request = NULL;
}

static void cgfs_async_notify_write(cgfs_data * const data,
                                    cgfs_inode * const inode)
{
    CGUTILS_ASSERT(data != NULL);
    CGUTILS_ASSERT(inode != NULL);

    cgfs_async_request * request = NULL;

    int result = cgfs_async_request_init(data,
                                         cgfs_inode_get_number(inode),
                                         NULL,
                                         cgfs_async_request_type_notify_write,
                                         NULL,
                                         NULL,
                                         &request);

    if (COMPILER_LIKELY(result == 0))
    {
        request->inode = inode;
        cgfs_inode_inc_ref_count(inode);

        result = cgsmc_async_notify_write(data->cgsmc_data,
                                          cgfs_inode_get_number(inode),
                                          &cgfs_async_notify_write_callback,
                                          request);

        if (COMPILER_UNLIKELY(result != 0))
        {
            CGUTILS_ERROR("Error notifying write to inode %"PRIu64": %d",
                          cgfs_inode_get_number(inode),
                          result);

            cgfs_async_request_free(request), request = NULL;
        }
    }
    else
    {
        CGUTILS_ERROR("Error allocating request: %d",
                      result);
    }
}


int cgfs_async_get_fd_for_writing(cgfs_data * const data,
                                  cgfs_file_handler * const file_handler,
                                  uint64_t const ino,
                                  int * const fd_out)
{
    CGUTILS_ASSERT(data != NULL);
    CGUTILS_ASSERT(file_handler != NULL);
    CGUTILS_ASSERT(ino > 0);
    CGUTILS_ASSERT(fd_out != NULL);

    int result = cgfs_file_handler_file_get_fd_for_writing(file_handler,
                                                           fd_out);

    if (COMPILER_LIKELY(result == 0))
    {
        cgfs_file_handler_file_set_dirty(file_handler);

        if (COMPILER_UNLIKELY(cgfs_file_handler_file_need_to_notify_write(data,
                                                                          file_handler) == true))
        {
            cgfs_async_notify_write(data,
                                    cgfs_file_handler_get_inode(file_handler));
        }

        cgfs_file_handler_update_mtime(file_handler);
    }
    else
    {
        CGUTILS_ERROR("Error getting FD for writing to inode %"PRIu64": %d",
                      ino,
                      result);
    }

    return result;
}

void cgfs_async_forget_inode(cgfs_data * const data,
                             uint64_t ino,
                             size_t const lookup_count)
{
    cgfs_inode * inode = NULL;
    CGUTILS_ASSERT(data != NULL);
    CGUTILS_ASSERT(ino > 0);
    CGUTILS_ASSERT(lookup_count > 0);

    ino = cgfs_translate_inode_number(data,
                                      ino);

    int result = cgfs_cache_lookup(data->cache,
                                   ino,
                                   &inode);

    if (COMPILER_LIKELY(result == 0))
    {
        cgfs_inode_dec_lookup_count(inode, lookup_count);

        if (cgfs_inode_get_lookup_count(inode) == 0)
        {
            /* The inode may be expunged from the cache now. */
            result = cgfs_cache_remove(data->cache,
                                       ino);

            if (COMPILER_UNLIKELY(result != 0))
            {
                CGUTILS_WARN("Error removing inode %"PRIu64" from cache: %d",
                             ino,
                             result);
            }
        }

        cgfs_inode_release(inode);
        inode = NULL;
    }
    else if (result == ENOENT)
    {
        /* Wow, this is not expected! */
        CGUTILS_WARN("Received a forget request for an inode (%"PRIu64") not present in cache!",
                     ino);
    }
    else
    {
        CGUTILS_WARN("Error retrieving inode %"PRIu64" from cache: %d",
                     ino,
                     result);
    }
}

static void cgfs_async_open_callback(int const status,
                                     char * file_path,
                                     void * const cb_data)
{
    int result = status;
    cgfs_async_request * request = cb_data;

    CGUTILS_ASSERT(request != NULL);

    if (COMPILER_LIKELY(status == 0))
    {
        CGUTILS_ASSERT(request->inode != NULL);
        CGUTILS_ASSERT(file_path != NULL);

        cgfs_file_handler * file_handler = NULL;

        result = cgfs_utils_open_file(request->inode,
                                      file_path,
                                      &(request->flags),
                                      &file_handler);

        if (COMPILER_LIKELY(result == 0))
        {
            time_t const now = time(NULL);

            cgfs_inode_update_atime(request->inode,
                                    now);

            CGUTILS_ASSERT(request->type == cgfs_async_request_type_open);
            CGUTILS_ASSERT(request->open_cb != NULL);

            (*(request->open_cb))(request->cb_data,
                                  file_handler);
        }

        if (COMPILER_UNLIKELY(result != 0))
        {
            cgfs_async_notify_of_failed_open_if_needed(request);
        }
    }

    CGUTILS_FREE(file_path);

    if (COMPILER_UNLIKELY(result != 0))
    {
        CGUTILS_ASSERT(request->error_cb != NULL);

        (*(request->error_cb))(result,
                               request->cb_data);
    }

    cgfs_async_request_free(request), request = NULL;
}

void cgfs_async_open(cgfs_data * const data,
                     uint64_t const ino,
                     int const flags,
                     cgfs_async_open_cb * const cb,
                     cgfs_async_error_cb * const error_cb,
                     void * const cb_data)
{
    int result = 0;

    CGUTILS_ASSERT(data != NULL);
    CGUTILS_ASSERT(ino > 0);
    CGUTILS_ASSERT(cb != NULL);
    CGUTILS_ASSERT(error_cb != NULL);

    if (COMPILER_LIKELY(cgfs_utils_check_flags_validity(flags) == true))
    {
        cgfs_inode * inode = NULL;
        cgfs_async_request * request = NULL;

        /* retrieve the inode from the cache */
        result = cgfs_cache_lookup(data->cache,
                                   ino,
                                   &inode);

        if (result == 0)
        {
            if (COMPILER_UNLIKELY(cgfs_inode_is_dir(inode) == true) &&
                cgfs_utils_writable_flags(flags) == true)
            {
                result = EISDIR;
            }
        }
        else if (result == ENOENT)
        {
            CGUTILS_ERROR("Unable to find inode %"PRIu64" in cache!!",
                          ino);
        }
        else
        {
            CGUTILS_ERROR("Error retrieving inode from cache: %d",
                          result);
        }

        if (COMPILER_LIKELY(result == 0))
        {
            result = cgfs_async_request_init(data,
                                             ino,
                                             NULL,
                                             cgfs_async_request_type_open,
                                             cb_data,
                                             error_cb,
                                             &request);

            if (COMPILER_LIKELY(result == 0))
            {
                request->open_cb = cb;
                request->inode = inode;
                inode = NULL;
                request->flags = flags;

                result = cgsmc_async_open(data->cgsmc_data,
                                          request->ino,
                                          request->flags,
                                          &cgfs_async_open_callback,
                                          request);

                if (COMPILER_UNLIKELY(result != 0))
                {
                    CGUTILS_ERROR("Error opening inode %"PRIu64": %d",
                                  request->ino,
                                  result);

                    cgfs_async_request_free(request), request = NULL;
                }
            }
            else
            {
                CGUTILS_ERROR("Error allocating request: %d",
                              result);
            }
        }

        if (inode != NULL)
        {
            cgfs_inode_release(inode), inode = NULL;
        }
    }
    else
    {
        result = EINVAL;
    }

    if (COMPILER_UNLIKELY(result != 0))
    {
        (*error_cb)(result,
                    cb_data);
    }
}

static int cgfs_async_read_event_cb(int const status,
                                    size_t const got,
                                    void * const cb_data)
{
    cgfs_async_request * request = cb_data;
    CGUTILS_ASSERT(request != NULL);

    int result = status;

    if (COMPILER_LIKELY(result == 0))
    {
        if (COMPILER_LIKELY(request->got < (SIZE_MAX - got)))
        {
            request->got += got;

            /* a return value of 0 means EOF. */
            if (request->got == request->buffer_size ||
                got == 0)
            {
                (*(request->read_cb))(request->cb_data,
                                      request->buffer,
                                      request->got);

                CGUTILS_FREE(request->buffer);
                cgfs_async_request_free(request), request = NULL;
            }
            else
            {
                result = cgutils_aio_read(request->data->aio,
                                          request->fd,
                                          request->buffer + request->got,
                                          request->buffer_size - request->got,
                                          (off_t) (request->pos + request->got),
                                          &cgfs_async_read_event_cb,
                                          request);

                if (COMPILER_UNLIKELY(result != 0))
                {
                    CGUTILS_ERROR("Error enabling AIO read from inode %"PRIu64": %d",
                                  request->ino,
                                  result);
                }
            }
        }
        else
        {
            result = E2BIG;
            CGUTILS_ERROR("Error, overflow detected");
        }
    }
    else
    {
        CGUTILS_ERROR("Error in AIO read for inode %"PRIu64": %d",
                      request->ino,
                      result);
    }

    if (COMPILER_UNLIKELY(result != 0))
    {
        (*(request->error_cb))(result,
                               request->cb_data);

        CGUTILS_FREE(request->buffer);
        cgfs_async_request_free(request), request = NULL;
    }

    return result;
}

void cgfs_async_read(cgfs_data * const data,
                     cgfs_file_handler * const file_handler,
                     uint64_t const ino,
                     size_t const size,
                     off_t const off,
                     cgfs_async_read_cb * const cb,
                     cgfs_async_error_cb * const error_cb,
                     void * const cb_data)
{
    int fd = -1;

    CGUTILS_ASSERT(data != NULL);
    CGUTILS_ASSERT(file_handler != NULL);
    CGUTILS_ASSERT(ino > 0);
    CGUTILS_ASSERT(cgfs_file_handler_get_inode_number(file_handler) == ino);
    CGUTILS_ASSERT(size > 0);
    CGUTILS_ASSERT(off >= 0);
    CGUTILS_ASSERT(cb != NULL);
    CGUTILS_ASSERT(error_cb != NULL);

    int result = cgfs_file_handler_file_get_fd_for_reading(file_handler,
                                                           &fd);

    if (COMPILER_LIKELY(result == 0))
    {
        char * buffer = NULL;
        CGUTILS_ASSERT(fd != -1);

        CGUTILS_MALLOC(buffer, size, sizeof *buffer);

        if (COMPILER_LIKELY(buffer != NULL))
        {
            /* We try to read directly. If by any chance the call doesn't block
               and returns what we need, we avoid the overhead of setting up a event-based read.
            */
            size_t got = 0;

            result = cgutils_file_pread(fd,
                                        buffer,
                                        size,
                                        off,
                                        &got);

            if (COMPILER_LIKELY(result == 0 ||
                                result == EAGAIN ||
                                result == EWOULDBLOCK ||
                                result == EINTR))
            {
                result = 0;

                /* 0 means EOF. */
                if (got == 0 ||
                    got == size)
                {
                    (*cb)(cb_data,
                          buffer,
                          got);

                    CGUTILS_FREE(buffer);
                }
                else
                {
                    cgfs_async_request * request = NULL;

                    result = cgfs_async_request_init(data,
                                                     ino,
                                                     NULL,
                                                     cgfs_async_request_type_read,
                                                     cb_data,
                                                     error_cb,
                                                     &request);

                    if (COMPILER_LIKELY(result == 0))
                    {
                        request->fd = fd;
                        request->read_cb = cb;
                        request->buffer = buffer;
                        request->buffer_size = size;
                        request->got = got;
                        request->pos = (size_t) off;

                        fd = -1;
                        buffer = NULL;
                        CGUTILS_ASSERT(request->data->aio != NULL);

                        result = cgutils_aio_read(request->data->aio,
                                                  request->fd,
                                                  request->buffer + request->got,
                                                  request->buffer_size - request->got,
                                                  (off_t) (request->pos + request->got),
                                                  &cgfs_async_read_event_cb,
                                                  request);

                        if (COMPILER_UNLIKELY(result != 0))
                        {
                            CGUTILS_ERROR("Error enabling AIO read from inode %"PRIu64": %d",
                                          ino,
                                          result);
                        }

                        if (COMPILER_UNLIKELY(result != 0))
                        {
                            cgfs_async_request_free(request), request = NULL;
                        }
                    }
                    else
                    {
                        CGUTILS_ERROR("Error allocating read request from inode %"PRIu64": %d",
                                      ino,
                                      result);
                    }
                }
            }
            else
            {
                CGUTILS_ERROR("Error reading from inode %"PRIu64": %d",
                              ino,
                              result);
            }

            CGUTILS_FREE(buffer);
        }
        else
        {
            result = ENOMEM;
            CGUTILS_ERROR("Error allocating buffer of size %zu to read from inode %"PRIu64": %d",
                          size,
                          ino,
                          result);
        }
    }
    else
    {
        CGUTILS_ERROR("Error getting FD for reading from inode %"PRIu64": %d",
                      ino,
                      result);
    }

    if (COMPILER_UNLIKELY(result != 0))
    {
        (*error_cb)(result,
                    cb_data);
    }
}

static int cgfs_async_write_event_cb(int const status,
                                     size_t const got,
                                     void * const cb_data)
{
    cgfs_async_request * request = cb_data;
    CGUTILS_ASSERT(request != NULL);

    int result = status;

    if (COMPILER_LIKELY(result == 0))
    {
        if (COMPILER_LIKELY(request->got < (SIZE_MAX - got)))
        {
            request->got += got;

            if (request->got == request->buffer_size)
            {
                CGUTILS_ASSERT(request->fh != NULL);

                cgfs_file_handler_file_refresh_inode_attributes_from_fd(request->fh);

                (*(request->write_cb))(request->cb_data,
                                       request->got);

                cgfs_async_request_free(request), request = NULL;
            }
            else
            {
                result = cgutils_aio_write(request->data->aio,
                                           request->fd,
                                           request->const_buffer + request->got,
                                           request->buffer_size - request->got,
                                           (off_t) (request->pos + request->got),
                                           &cgfs_async_write_event_cb,
                                           request);

                if (COMPILER_UNLIKELY(result != 0))
                {
                    CGUTILS_ERROR("Error enabling AIO write to inode %"PRIu64": %d",
                                  request->ino,
                                  result);
                }
            }
        }
        else
        {
            result = E2BIG;
            CGUTILS_ERROR("Error, overflow detected");
        }
    }
    else
    {
        CGUTILS_ERROR("Error in AIO write for inode %"PRIu64": %d",
                      request->ino,
                      result);
    }

    if (COMPILER_UNLIKELY(result != 0))
    {
        (*(request->error_cb))(result,
                               request->cb_data);

        cgfs_async_request_free(request), request = NULL;
    }

    return result;
}

void cgfs_async_write(cgfs_data * const data,
                      cgfs_file_handler * const file_handler,
                      uint64_t const ino,
                      char const * const buffer,
                      size_t const buffer_size,
                      off_t const off,
                      cgfs_async_write_cb * const cb,
                      cgfs_async_error_cb * const error_cb,
                      void * const cb_data)
{
    int fd = -1;
    CGUTILS_ASSERT(data != NULL);
    CGUTILS_ASSERT(file_handler != NULL);
    CGUTILS_ASSERT(ino > 0);
    CGUTILS_ASSERT(cgfs_file_handler_get_inode_number(file_handler) == ino);
    CGUTILS_ASSERT(buffer != NULL);
    CGUTILS_ASSERT(buffer_size > 0);
    CGUTILS_ASSERT(off >= 0);
    CGUTILS_ASSERT(cb != NULL);
    CGUTILS_ASSERT(error_cb != NULL);

    int result = cgfs_async_get_fd_for_writing(data,
                                               file_handler,
                                               ino,
                                               &fd);

    if (COMPILER_LIKELY(result == 0))
    {
        /* We try to write directly. If by any chance the call doesn't block
           we avoid the overhead of setting up a event-based write.
        */
        size_t written = 0;

        result = cgutils_file_lseek(fd,
                                    SEEK_SET,
                                    off);

        if (COMPILER_LIKELY(result == 0))
        {
            result = cgutils_file_write(fd,
                                        buffer,
                                        buffer_size,
                                        &written);

            if (COMPILER_LIKELY(result == 0 ||
                                result == EAGAIN ||
                                result == EWOULDBLOCK ||
                                result == EINTR))
            {
                result = 0;

                if (written == 0 ||
                    written == buffer_size)
                {
                    cgfs_file_handler_file_refresh_inode_attributes_from_fd(file_handler);

                    (*cb)(cb_data,
                          written);
                }
                else
                {
                    cgfs_async_request * request = NULL;

                    result = cgfs_async_request_init(data,
                                                     ino,
                                                     NULL,
                                                     cgfs_async_request_type_write,
                                                     cb_data,
                                                     error_cb,
                                                     &request);

                    if (COMPILER_LIKELY(result == 0))
                    {
                        request->fd = fd;
                        request->write_cb = cb;
                        request->const_buffer = buffer;
                        request->buffer_size = buffer_size;
                        request->got = written;
                        request->pos = (size_t) off + written;
                        request->fh = file_handler;
                        request->inode = cgfs_file_handler_get_inode(file_handler);
                        cgfs_inode_inc_ref_count(request->inode);

                        result = cgutils_aio_write(request->data->aio,
                                                   request->fd,
                                                   request->const_buffer + request->got,
                                                   request->buffer_size - request->got,
                                                   (off_t) (request->pos + request->got),
                                                   &cgfs_async_write_event_cb,
                                                   request);

                        if (COMPILER_UNLIKELY(result != 0))
                        {
                            CGUTILS_ERROR("Error enabling AIO write to inode %"PRIu64": %d",
                                          ino,
                                          result);
                            cgfs_async_request_free(request), request = NULL;
                        }
                    }
                    else
                    {
                        CGUTILS_ERROR("Error allocating write request to inode %"PRIu64": %d",
                                      ino,
                                      result);
                    }
                }
            }
            else
            {
                CGUTILS_ERROR("Error writing to inode %"PRIu64": %d",
                              ino,
                              result);
            }
        }
        else
        {
            CGUTILS_ERROR("Error seeking before writing to inode %"PRIu64": %d",
                          ino,
                          result);
        }
    }
    else
    {
        CGUTILS_ERROR("Error getting FD for writing to inode %"PRIu64": %d",
                      ino,
                      result);
    }

    if (COMPILER_UNLIKELY(result != 0))
    {
        (*error_cb)(result,
                    cb_data);
    }
}

static void cgfs_async_setattr_callback(int const status,
                                        void * const cb_data)
{
    int result = status;

    cgfs_async_request * request = cb_data;

    CGUTILS_ASSERT(request != NULL);

    if (COMPILER_LIKELY(status == 0))
    {
        CGUTILS_ASSERT(request->inode != NULL);

        CGUTILS_ASSERT(request->type == cgfs_async_request_type_stat);
        CGUTILS_ASSERT(request->stat_cb != NULL);

        cgfs_inode_update_ctime(request->inode);

        (*(request->stat_cb))(request->cb_data,
                              request->inode);
    }

    if (COMPILER_UNLIKELY(result != 0))
    {
        CGUTILS_ASSERT(request->error_cb != NULL);

        (*(request->error_cb))(result,
                               request->cb_data);
    }

    cgfs_async_request_free(request), request = NULL;
}

static int cgfs_async_update_fh_attributes(cgfs_file_handler * const file_handler,
                                           int const cgfs_to_set,
                                           struct stat const * const attr)
{
    int result = 0;
    int res = 0;

    CGUTILS_ASSERT(file_handler != NULL);
    CGUTILS_ASSERT(cgfs_file_handler_get_type(file_handler) == cgfs_file_handler_type_file);
    CGUTILS_ASSERT(attr != NULL);

    if (cgfs_to_set & CGFS_SET_ATTR_MODE)
    {
        res = cgfs_file_handler_file_set_mode(file_handler,
                                              attr->st_mode);

        if (COMPILER_UNLIKELY(res != 0 &&
                              result == 0))
        {
            result = res;
        }
    }

    if (cgfs_to_set & CGFS_SET_ATTR_SIZE)
    {
        res = cgfs_file_handler_file_truncate(file_handler,
                                              attr->st_size);

        if (COMPILER_UNLIKELY(res != 0 &&
                              result == 0))
        {
            result = res;
        }
    }

    if (cgfs_to_set & CGFS_SET_ATTR_ATIME)
    {
        res = cgfs_file_handler_file_set_atime(file_handler,
                                               attr->st_atime);

        if (COMPILER_UNLIKELY(res != 0 &&
                              result == 0))
        {
            result = res;
        }
    }

    if (cgfs_to_set & CGFS_SET_ATTR_MTIME)
    {
        res = cgfs_file_handler_file_set_mtime(file_handler,
                                               attr->st_mtime);

        if (COMPILER_UNLIKELY(res != 0 &&
                              result == 0))
        {
            result = res;
        }
    }

    if (cgfs_to_set & CGFS_SET_ATTR_ATIME_NOW)
    {
        time_t const now = time(NULL);

        res = cgfs_file_handler_file_set_atime(file_handler,
                                               now);

        if (COMPILER_UNLIKELY(res != 0 &&
                              result == 0))
        {
            result = res;
        }
    }

    if (cgfs_to_set & CGFS_SET_ATTR_MTIME_NOW)
    {
        time_t const now = time(NULL);

        res = cgfs_file_handler_file_set_mtime(file_handler,
                                               now);

        if (COMPILER_UNLIKELY(res != 0 &&
                              result == 0))
        {
            result = res;
        }
    }

    return result;
}

void cgfs_async_setattr(cgfs_data * const data,
                        uint64_t ino,
                        cgfs_file_handler * const file_handler,
                        struct stat const * const attr,
                        int const cgfs_to_set,
                        cgfs_async_stat_cb * const cb,
                        cgfs_async_error_cb * const error_cb,
                        void * const cb_data)
{
    int result = 0;
    cgfs_inode * inode = NULL;
    cgfs_async_request * request = NULL;
    CGUTILS_ASSERT(data != NULL);
    CGUTILS_ASSERT(ino > 0);
    CGUTILS_ASSERT(cb != NULL);
    CGUTILS_ASSERT(error_cb != NULL);

    ino = cgfs_translate_inode_number(data,
                                      ino);

    if (COMPILER_UNLIKELY(file_handler != NULL &&
                          cgfs_file_handler_get_inode(file_handler) != NULL))
    {
        inode = cgfs_file_handler_get_inode(file_handler);
        CGUTILS_ASSERT(cgfs_inode_get_number(inode) == ino);
        cgfs_inode_inc_ref_count(inode);
    }
    else
    {
        /* retrieve the inode from the cache */
        result = cgfs_cache_lookup(data->cache,
                                   ino,
                                   &inode);

        if (COMPILER_UNLIKELY(result == ENOENT))
        {
            CGUTILS_ERROR("Unable to find inode %"PRIu64" in cache!!",
                          ino);
        }
        else if (COMPILER_UNLIKELY(result != 0))
        {
            CGUTILS_ERROR("Error retrieving inode from cache: %d",
                          result);
        }
    }

    if (COMPILER_LIKELY(result == 0))
    {
        CGUTILS_ASSERT(inode != NULL);

        if (COMPILER_LIKELY(cgfs_to_set != 0))
        {
            cgfs_inode_update_attributes(inode,
                                         attr,
                                         cgfs_to_set);

            /* What about size, atime, mtime?
               If we have a file_handler, we should
               update/truncate the file in cache immediately.
            */
            if (COMPILER_UNLIKELY(file_handler != NULL &&
                                  cgfs_file_handler_get_type(file_handler) == cgfs_file_handler_type_file))
            {
                cgfs_async_update_fh_attributes(file_handler,
                                                cgfs_to_set,
                                                attr);
            }

            result = cgfs_async_request_init(data,
                                             ino,
                                             NULL,
                                             cgfs_async_request_type_stat,
                                             cb_data,
                                             error_cb,
                                             &request);

            if (COMPILER_LIKELY(result == 0))
            {
                request->stat_cb = cb;
                request->inode = inode;
                inode = NULL;

                result = cgsmc_async_setattr(data->cgsmc_data,
                                             request->ino,
                                             &(request->inode->attr),
                                             cgfs_to_set & CGFS_SET_ATTR_SIZE,
                                             &cgfs_async_setattr_callback,
                                             request);

                if (COMPILER_UNLIKELY(result != 0))
                {
                    CGUTILS_ERROR("Error updating attributes for inode %"PRIu64": %d",
                                  request->ino,
                                  result);

                    cgfs_async_request_free(request), request = NULL;
                }
            }
            else
            {
                CGUTILS_ERROR("Error allocating request: %d",
                              result);
            }
        }
        else
        {
            /* Nothing to do, just return the current inode */
            (*cb)(cb_data,
                  inode);
        }
    }

    if (inode != NULL)
    {
        cgfs_inode_release(inode), inode = NULL;
    }

    if (COMPILER_UNLIKELY(result != 0))
    {
        (*error_cb)(result,
                    cb_data);
    }
}

static void cgfs_async_mkdir_callback(int const status,
                                      struct stat * st,
                                      void * const cb_data)
{
    int result = status;
    cgfs_async_request * request = cb_data;
    CGUTILS_ASSERT(request != NULL);
    CGUTILS_ASSERT(request->type == cgfs_async_request_type_mkdir);

    if (COMPILER_LIKELY(status == 0))
    {
        CGUTILS_ASSERT(st != NULL);

        if (request->parent_inode != NULL)
        {
            cgfs_inode_update_mtime(request->parent_inode,
                                    time(NULL));
        }

        result = cgfs_cache_lookup(request->data->cache,
                                   st->st_ino,
                                   &(request->inode));

        if (COMPILER_UNLIKELY(result != 0 &&
                              result != ENOENT))
        {
            CGUTILS_WARN("Error looking up inode %"PRIu64" in cache: %d",
                         st->st_ino,
                         result);
            result = ENOENT;
        }

        if (result == ENOENT)
        {
            result = cgfs_inode_init(st,
                                     &(request->inode));

            if (COMPILER_LIKELY(result == 0))
            {
                result = cgfs_cache_add(request->data->cache,
                                        request->inode);

                if (COMPILER_UNLIKELY(result != 0))
                {
                    CGUTILS_WARN("Error adding inode %"PRIu64" to cache: %d",
                                 cgfs_inode_get_number(request->inode),
                                 result);
                }

                result = 0;
            }
            else
            {
                CGUTILS_ERROR("Error getting inode from stat: %d",
                              result);
            }
        }

        if (COMPILER_LIKELY(result == 0))
        {
            CGUTILS_ASSERT(request->stat_cb != NULL);

            (*(request->stat_cb))(request->cb_data,
                                  request->inode);
        }
    }

    if (COMPILER_UNLIKELY(result != 0))
    {
        CGUTILS_ASSERT(request->error_cb != NULL);

        (*(request->error_cb))(result,
                               request->cb_data);
    }

    CGUTILS_FREE(st);

    cgfs_async_request_free(request), request = NULL;
}

void cgfs_async_mkdir(cgfs_data * const data,
                      uint64_t parent,
                      char const * const name,
                      uid_t const owner,
                      gid_t const group,
                      mode_t const mode,
                      cgfs_async_stat_cb * const cb,
                      cgfs_async_error_cb * const error_cb,
                      void * const cb_data)
{
    int result = 0;
    cgfs_inode * parent_inode = NULL;
    cgfs_async_request * request = NULL;

    CGUTILS_ASSERT(data != NULL);
    CGUTILS_ASSERT(parent > 0);
    CGUTILS_ASSERT(name != NULL);
    CGUTILS_ASSERT(cb != NULL);
    CGUTILS_ASSERT(error_cb != NULL);

    parent = cgfs_translate_inode_number(data,
                                         parent);

    /* do we have the parent in cache ? */
    result = cgfs_cache_lookup(data->cache,
                               parent,
                               &parent_inode);

    if (result == 0)
    {
        if (COMPILER_UNLIKELY(cgfs_inode_is_dir(parent_inode) == false))
        {
            result = ENOTDIR;
        }
    }
    else if (result == ENOENT)
    {
        result = 0;
    }
    else
    {
        CGUTILS_WARN("Error retrieving inode from cache: %d",
                     result);
        result = 0;
    }

    if (COMPILER_LIKELY(result == 0))
    {
        /* TODO / FIXME would be nice to create the inode with a flag indicating
           that it is in creation, keep it in memory (separate rbtree ?)
           and queue lookup requests until it is created. */

        result = cgfs_async_request_init(data,
                                         parent,
                                         name,
                                         cgfs_async_request_type_mkdir,
                                         cb_data,
                                         error_cb,
                                         &request);

        if (result == 0)
        {
            request->stat_cb = cb;
            request->uid = owner;
            request->gid = group;
            request->mode = mode;
            request->parent_inode = parent_inode;
            parent_inode = NULL;

            result = cgsmc_async_mkdir(data->cgsmc_data,
                                       request->ino,
                                       request->name,
                                       request->uid,
                                       request->gid,
                                       request->mode,
                                       &cgfs_async_mkdir_callback,
                                       request);

            if (COMPILER_UNLIKELY(result != 0))
            {
                if (result != ENAMETOOLONG)
                {
                    CGUTILS_ERROR("Error creating directory named %s in directory/inode %"PRIu64": %d",
                                  request->name,
                                  request->ino,
                                  result);
                }

                cgfs_async_request_free(request), request = NULL;
            }
        }
        else
        {
            CGUTILS_ERROR("Error allocating request: %d",
                          result);
        }
    }

    if (parent_inode != NULL)
    {
        cgfs_inode_release(parent_inode), parent_inode = NULL;
    }

    if (COMPILER_UNLIKELY(result != 0))
    {
        (*error_cb)(result,
                    cb_data);
    }
}

static void cgfs_async_rmdir_callback(int const status,
                                      uint64_t const deleted_inode_number,
                                      void * const cb_data)
{
    int result = status;

    cgfs_async_request * request = cb_data;

    CGUTILS_ASSERT(request != NULL);

    if (COMPILER_LIKELY(status == 0))
    {
        cgfs_inode * deleted_inode = NULL;
        CGUTILS_ASSERT(request->type == cgfs_async_request_type_rmdir);

        result = cgfs_cache_lookup(request->data->cache,
                                   deleted_inode_number,
                                   &deleted_inode);

        if (result == 0)
        {
            /* removal from the cache will only happen after a forget() call has been issued. */
            cgfs_inode_decrement_link_count(deleted_inode);
            cgfs_inode_release(deleted_inode), deleted_inode = NULL;
        }
        else if (result == ENOENT)
        {
            result = 0;
        }
        else
        {
            CGUTILS_WARN("Error retrieving deleted inode %"PRIu64" from cache: %d",
                         deleted_inode_number,
                         result);
        }

        if (request->parent_inode == NULL)
        {
            result = cgfs_cache_lookup(request->data->cache,
                                       request->ino,
                                       &(request->parent_inode));

            if (COMPILER_UNLIKELY(result != 0 &&
                                  result != ENOENT))
            {
                CGUTILS_WARN("Error looking up for parent inode %"PRIu64" from the cache: %d",
                             request->ino,
                             result);
            }
        }

        if (request->parent_inode != NULL)
        {
            cgfs_inode_update_mtime(request->parent_inode,
                                    time(NULL));
        }

        CGUTILS_ASSERT(request->status_cb != NULL);

        (*(request->status_cb))(status,
                                request->cb_data);
    }

    if (COMPILER_UNLIKELY(result != 0))
    {
        CGUTILS_ASSERT(request->error_cb != NULL);

        (*(request->error_cb))(result,
                               request->cb_data);
    }

    cgfs_async_request_free(request), request = NULL;
}

void cgfs_async_rmdir(cgfs_data * const data,
                      uint64_t parent,
                      char const * const name,
                      cgfs_async_status_cb * const cb,
                      cgfs_async_error_cb * const error_cb,
                      void * const cb_data)
{
    int result = 0;
    cgfs_inode * parent_inode = NULL;
    cgfs_async_request * request = NULL;

    CGUTILS_ASSERT(data != NULL);
    CGUTILS_ASSERT(parent > 0);
    CGUTILS_ASSERT(name != NULL);
    CGUTILS_ASSERT(cb != NULL);
    CGUTILS_ASSERT(error_cb != NULL);

    parent = cgfs_translate_inode_number(data,
                                         parent);

    /* do we have the parent in cache ? */
    result = cgfs_cache_lookup(data->cache,
                               parent,
                               &parent_inode);

    if (result == 0)
    {
        if (COMPILER_UNLIKELY(cgfs_inode_is_dir(parent_inode) == false))
        {
            result = ENOTDIR;
        }
    }
    else if (result == ENOENT)
    {
        result = 0;
    }
    else
    {
        CGUTILS_WARN("Error retrieving inode from cache: %d",
                     result);
        result = 0;
    }

    if (COMPILER_LIKELY(result == 0))
    {
        result = cgfs_async_request_init(data,
                                         parent,
                                         name,
                                         cgfs_async_request_type_rmdir,
                                         cb_data,
                                         error_cb,
                                         &request);

        if (result == 0)
        {
            request->status_cb = cb;
            request->parent_inode = parent_inode;
            parent_inode = NULL;

            result = cgsmc_async_rmdir(data->cgsmc_data,
                                       request->ino,
                                       request->name,
                                       &cgfs_async_rmdir_callback,
                                       request);

            if (COMPILER_UNLIKELY(result != 0))
            {
                if (result != ENAMETOOLONG)
                {
                    CGUTILS_ERROR("Error removing directory named %s in directory/inode %"PRIu64": %d",
                                  request->name,
                                  request->ino,
                                  result);
                }

                cgfs_async_request_free(request), request = NULL;
            }
        }
        else
        {
            CGUTILS_ERROR("Error allocating request: %d",
                          result);
        }
    }

    if (parent_inode != NULL)
    {
        cgfs_inode_release(parent_inode), parent_inode = NULL;
    }

    if (COMPILER_UNLIKELY(result != 0))
    {
        (*error_cb)(result,
                    cb_data);
    }
}

void cgfs_async_statfs(cgfs_data * const data,
                       uint64_t const ino,
                       cgfs_async_statfs_cb * const cb,
                       cgfs_async_error_cb * const error_cb,
                       void * const cb_data)
{
    int result = 0;
    struct statvfs stats = (struct statvfs) { 0 };

    CGUTILS_ASSERT(data != NULL);
    CGUTILS_ASSERT(cb != NULL);
    CGUTILS_ASSERT(error_cb != NULL);

    (void) ino;

    stats.f_bsize  = cgsmc_async_get_block_size(data->cgsmc_data);

    stats.f_blocks = (CGUTILS_TYPE_MAXIMUM(typeof(stats.f_bfree)) / 1024);
    stats.f_bfree  = (CGUTILS_TYPE_MAXIMUM(typeof(stats.f_bfree)) / 1024);
    stats.f_bavail = (CGUTILS_TYPE_MAXIMUM(typeof(stats.f_bavail)) / 1024);

    stats.f_files = (CGUTILS_TYPE_MAXIMUM(typeof(stats.f_files)) / 1024);
    stats.f_ffree = (CGUTILS_TYPE_MAXIMUM(typeof(stats.f_ffree)) / 1024);
    stats.f_favail = (CGUTILS_TYPE_MAXIMUM(typeof(stats.f_favail)) / 1024);

    stats.f_namemax = cgsmc_async_get_name_max(data->cgsmc_data);

    (*cb)(cb_data,
          &stats);

    if (COMPILER_UNLIKELY(result != 0))
    {
        (*error_cb)(result,
                    cb_data);
    }
}

static void cgfs_async_fsync_notification_callback(int const status,
                                                   void * cb_data)
{
    cgfs_async_request * request = cb_data;

    CGUTILS_ASSERT(request != NULL);

    if (COMPILER_LIKELY(status == 0))
    {
        CGUTILS_ASSERT(request->inode != NULL);
        cgfs_inode_update_dirty_notification(request->inode);

        (*(request->status_cb))(status,
                                request->cb_data);
    }
    else
    {
        CGUTILS_ERROR("Error while notifying write to inode %"PRIu64" after a fsync() call: %d",
                      request->ino,
                      status);

        (*(request->error_cb))(status,
                               request->cb_data);
    }

    cgfs_async_request_free(request), request = NULL;
}

static int cgfs_async_fsync_event_cb(int const status,
                                     size_t const got,
                                     void * cb_data)
{
    cgfs_async_request * request = cb_data;
    CGUTILS_ASSERT(request != NULL);

    /* no use for a fsync operation */
    (void) got;

    int result = status;

    if (COMPILER_LIKELY(result == 0))
    {
        CGUTILS_ASSERT(request->fh != NULL);
        CGUTILS_ASSERT(request->inode != NULL);

        cgfs_file_handler_file_refresh_inode_attributes_from_fd(request->fh);

        result = cgsmc_async_notify_write(request->data->cgsmc_data,
                                          cgfs_inode_get_number(request->inode),
                                          &cgfs_async_fsync_notification_callback,
                                          request);

        if (COMPILER_UNLIKELY(result != 0))
        {
            CGUTILS_ERROR("Error notifying write after fsync to inode %"PRIu64": %d",
                          cgfs_inode_get_number(request->inode),
                          result);
        }
    }
    else
    {
        CGUTILS_ERROR("Error in AIO fsync for inode %"PRIu64": %d",
                      request->ino,
                      result);
    }

    if (COMPILER_UNLIKELY(result != 0))
    {
        (*(request->error_cb))(result,
                               request->cb_data);

        cgfs_async_request_free(request), request = NULL;
    }

    return result;
}

void cgfs_async_fsync(cgfs_data * const data,
                      cgfs_file_handler * const file_handler,
                      uint64_t const ino,
                      int const datasync,
                      cgfs_async_status_cb * const cb,
                      cgfs_async_error_cb * const error_cb,
                      void * const cb_data)
{
    int fd = -1;
    CGUTILS_ASSERT(data != NULL);
    CGUTILS_ASSERT(file_handler != NULL);
    CGUTILS_ASSERT(ino > 0);
    CGUTILS_ASSERT(cgfs_file_handler_get_inode_number(file_handler) == ino);
    CGUTILS_ASSERT(cb != NULL);
    CGUTILS_ASSERT(error_cb != NULL);

    int result = cgfs_file_handler_file_get_fd_for_writing(file_handler,
                                                           &fd);

    if (COMPILER_LIKELY(result == 0))
    {
        cgfs_async_request * request = NULL;

        result = cgfs_async_request_init(data,
                                         ino,
                                         NULL,
                                         cgfs_async_request_type_fsync,
                                         cb_data,
                                         error_cb,
                                         &request);

        if (COMPILER_LIKELY(result == 0))
        {
            request->fd = fd;
            request->status_cb = cb;
            request->fh = file_handler;
            request->inode = cgfs_file_handler_get_inode(file_handler);
            cgfs_inode_inc_ref_count(request->inode);

            result = cgutils_aio_fsync(request->data->aio,
                                       request->fd,
                                       datasync,
                                       &cgfs_async_fsync_event_cb,
                                       request);

            if (COMPILER_UNLIKELY(result != 0))
            {
                CGUTILS_ERROR("Error enabling AIO fsync to inode %"PRIu64": %d",
                              ino,
                              result);
            }

            if (COMPILER_UNLIKELY(result != 0))
            {
                cgfs_async_request_free(request), request = NULL;
            }
        }
        else
        {
            CGUTILS_ERROR("Error allocating fsync request to inode %"PRIu64": %d",
                          ino,
                          result);
        }
    }
    else
    {
        CGUTILS_ERROR("Error getting FD for writing to inode %"PRIu64": %d",
                      ino,
                      result);
    }

    if (COMPILER_UNLIKELY(result != 0))
    {
        (*error_cb)(result,
                    cb_data);
    }
}

static void cgfs_async_unlink_callback(int const status,
                                       uint64_t const unlinked_inode_number,
                                       void * const cb_data)
{
    cgfs_async_request * request = cb_data;
    CGUTILS_ASSERT(request != NULL);
    int result = status;

    if (COMPILER_LIKELY(result == 0))
    {
        cgfs_inode * parent_inode = NULL;

        result = cgfs_cache_lookup(request->data->cache,
                                   unlinked_inode_number,
                                   &(request->inode));

        if (COMPILER_UNLIKELY(result != 0 &&
                              result != ENOENT))
        {
            CGUTILS_WARN("Error looking up inode %"PRIu64" from the cache: %d",
                         unlinked_inode_number,
                         result);
        }

        if (request->inode != NULL)
        {
            CGUTILS_ASSERT(cgfs_inode_get_number(request->inode) == unlinked_inode_number);
            cgfs_inode_decrement_link_count(request->inode);
        }

        if (parent_inode == NULL)
        {
            result = cgfs_cache_lookup(request->data->cache,
                                       /* parent inode number */
                                       request->ino,
                                       &parent_inode);

            if (result != 0 &&
                result != ENOENT)
            {
                CGUTILS_WARN("Error looking up parent inode %"PRIu64" from cache: %d",
                             request->ino,
                             result);
            }
        }

        if (parent_inode != NULL)
        {
            cgfs_inode_update_mtime(parent_inode,
                                    time(NULL));
            cgfs_inode_release(parent_inode), parent_inode = NULL;
        }

        (*(request->status_cb))(0,
                                request->cb_data);
    }
    else
    {
        CGUTILS_ERROR("Error in unlink operation for inode %"PRIu64": %d",
                      request->ino,
                      result);
    }

    if (COMPILER_UNLIKELY(result != 0))
    {
        (*(request->error_cb))(result,
                               request->cb_data);
    }

    cgfs_async_request_free(request), request = NULL;
}

void cgfs_async_unlink(cgfs_data * const data,
                       uint64_t parent_ino,
                       char const * const name,
                       cgfs_async_status_cb * const cb,
                       cgfs_async_error_cb * const error_cb,
                       void * const cb_data)
{
    int result = 0;
    cgfs_async_request * request = NULL;

    CGUTILS_ASSERT(data != NULL);
    CGUTILS_ASSERT(parent_ino > 0);
    CGUTILS_ASSERT(name != NULL);
    CGUTILS_ASSERT(cb != NULL);
    CGUTILS_ASSERT(error_cb != NULL);

    parent_ino = cgfs_translate_inode_number(data,
                                             parent_ino);

    result = cgfs_async_request_init(data,
                                     parent_ino,
                                     name,
                                     cgfs_async_request_type_unlink,
                                     cb_data,
                                     error_cb,
                                     &request);

    if (COMPILER_LIKELY(result == 0))
    {
        request->status_cb = cb;

        result = cgsmc_async_unlink(data->cgsmc_data,
                                    request->ino,
                                    request->name,
                                    &cgfs_async_unlink_callback,
                                    request);

        if (COMPILER_UNLIKELY(result != 0))
        {
            if (result != ENAMETOOLONG)
            {
                CGUTILS_ERROR("Error unlinking child named %s of inode %"PRIu64": %d",
                              request->name,
                              request->ino,
                              result);
            }

            cgfs_async_request_free(request), request = NULL;
        }
    }
    else
    {
        CGUTILS_ERROR("Error allocating request: %d",
                      result);
    }

    if (COMPILER_UNLIKELY(result != 0))
    {
        (*error_cb)(result,
                    cb_data);
    }
}

static void cgfs_async_rename_callback(int const status,
                                       uint64_t const renamed_inode_number,
                                       uint64_t const deleted_inode_number,
                                       void * const cb_data)
{
    cgfs_async_request * request = cb_data;
    CGUTILS_ASSERT(request != NULL);
    int result = status;

    if (COMPILER_LIKELY(result == 0))
    {
        CGUTILS_ASSERT(request->inode == NULL);

        /* update renamed inode ctime */
        result = cgfs_cache_lookup(request->data->cache,
                                   renamed_inode_number,
                                   &(request->inode));

        if (result == 0)
        {
            cgfs_inode_update_ctime(request->inode);
            cgfs_inode_release(request->inode), request->inode = NULL;
        }
        else if (result != ENOENT)
        {
            CGUTILS_WARN("Error looking up for renamed inode %"PRIu64" from the cache: %d",
                         renamed_inode_number,
                         result);
        }

        /* update old parent mtime */
        result = cgfs_cache_lookup(request->data->cache,
                                   request->ino,
                                   (&request->inode));

        if (result == 0)
        {
            cgfs_inode_update_mtime(request->inode,
                                    time(NULL));
            cgfs_inode_release(request->inode), request->inode = NULL;
        }
        else if (result != ENOENT)
        {
            CGUTILS_WARN("Error looking up for renamed inode's old parent  %"PRIu64" from the cache: %d",
                         request->ino,
                         result);
        }

        /* if new_parent != old_parent,
           update new parent mtime. */

        if (request->ino != request->new_parent_ino)
        {
            result = cgfs_cache_lookup(request->data->cache,
                                       request->new_parent_ino,
                                       (&request->inode));

            if (result == 0)
            {
                cgfs_inode_update_mtime(request->inode,
                                        time(NULL));
                cgfs_inode_release(request->inode), request->inode = NULL;
            }
            else if (result != ENOENT)
            {
                CGUTILS_WARN("Error looking up for renamed inode's new parent  %"PRIu64" from the cache: %d",
                             request->new_parent_ino,
                             result);
            }
        }

        if (deleted_inode_number > 0)
        {
            /* An existing inode has been deleted,
               decrement its link count if needed.
               Removal from the cache will only happen after a forget call().
            */
            result = cgfs_cache_lookup(request->data->cache,
                                       deleted_inode_number,
                                       &(request->inode));

            if (result == 0)
            {
                cgfs_inode_decrement_link_count(request->inode);
                cgfs_inode_release(request->inode), request->inode = NULL;
            }
            else if (result != ENOENT)
            {
                CGUTILS_WARN("Error lookup up removed inode %"PRIu64" from the cache: %d",
                         deleted_inode_number,
                         result);
            }
        }

        (*(request->status_cb))(0,
                                request->cb_data);
    }
    else if (result != ENOTEMPTY)
    {
        CGUTILS_ERROR("Error in rename operation from %"PRIu64"->%s to %"PRIu64"->%s: %d",
                      request->ino,
                      request->name,
                      request->new_parent_ino,
                      request->new_name,
                      result);
    }

    if (COMPILER_UNLIKELY(result != 0))
    {
        (*(request->error_cb))(result,
                               request->cb_data);
    }

    cgfs_async_request_free(request), request = NULL;
}

void cgfs_async_rename(cgfs_data * const data,
                       uint64_t old_parent_ino,
                       char const * const old_name,
                       uint64_t new_parent_ino,
                       char const * const new_name,
                       cgfs_async_status_cb * const cb,
                       cgfs_async_error_cb * const error_cb,
                       void * const cb_data)
{
    int result = 0;
    cgfs_async_request * request = NULL;

    CGUTILS_ASSERT(data != NULL);
    CGUTILS_ASSERT(old_parent_ino > 0);
    CGUTILS_ASSERT(old_name != NULL);
    CGUTILS_ASSERT(new_parent_ino > 0);
    CGUTILS_ASSERT(new_name != NULL);
    CGUTILS_ASSERT(cb != NULL);
    CGUTILS_ASSERT(error_cb != NULL);

    /* if the new entry already exists, if should not be a directory if old is not
       if both are directories, the new entry should be an empty directory

       if the new entry was a file, we need to remove entry, inode, and set inodes_instances to to_delete
       then delete the existing file in cache if it exists. */

    old_parent_ino = cgfs_translate_inode_number(data,
                                                 old_parent_ino);

    new_parent_ino = cgfs_translate_inode_number(data,
                                                 new_parent_ino);

    result = cgfs_async_request_init(data,
                                     old_parent_ino,
                                     old_name,
                                     cgfs_async_request_type_rename,
                                     cb_data,
                                     error_cb,
                                     &request);

    if (COMPILER_LIKELY(result == 0))
    {
        request->status_cb = cb;
        request->new_parent_ino = new_parent_ino;
        request->new_name = cgutils_strdup(new_name);

        if (COMPILER_LIKELY(request->new_name != NULL))
        {
            result = cgsmc_async_rename(data->cgsmc_data,
                                        request->ino,
                                        request->name,
                                        request->new_parent_ino,
                                        request->new_name,
                                        &cgfs_async_rename_callback,
                                        request);

            if (COMPILER_UNLIKELY(result != 0))
            {
                if (result != ENAMETOOLONG)
                {
                    CGUTILS_ERROR("Error renaming %"PRIu64"->%s to %"PRIu64"->%s: %d",
                                  old_parent_ino,
                                  old_name,
                                  new_parent_ino,
                                  new_name,
                                  result);
                }
            }
        }
        else
        {
            result = ENOMEM;
            CGUTILS_ERROR("Error allocating memory for new name: %d",
                          result);
        }

        if (COMPILER_UNLIKELY(result != 0))
        {
            cgfs_async_request_free(request), request = NULL;
        }
    }
    else
    {
        CGUTILS_ERROR("Error allocating request: %d",
                      result);
    }

    if (COMPILER_UNLIKELY(result != 0))
    {
        (*error_cb)(result,
                    cb_data);
    }
}

static void cgfs_async_hardlink_callback(int const status,
                                         struct stat * st,
                                         void * const cb_data)
{
    cgfs_async_request * request = cb_data;
    CGUTILS_ASSERT(request != NULL);
    int result = status;

    if (COMPILER_LIKELY(result == 0))
    {
        CGUTILS_ASSERT(st != NULL);
        CGUTILS_ASSERT(request->ino == st->st_ino);
        CGUTILS_ASSERT(request->inode == NULL);

        /* update new parent mtime */
        result = cgfs_cache_lookup(request->data->cache,
                                   request->new_parent_ino,
                                   (&request->inode));

        if (result == 0)
        {
            cgfs_inode_update_mtime(request->inode,
                                    time(NULL));
            cgfs_inode_release(request->inode), request->inode = NULL;
        }
        else if (result != ENOENT)
        {
            CGUTILS_WARN("Error looking up for renamed inode's old parent  %"PRIu64" from the cache: %d",
                         request->ino,
                         result);
        }

        /* update existing inode ctime */
        result = cgfs_cache_lookup(request->data->cache,
                                   request->ino,
                                   &(request->inode));

        if (result == 0)
        {
            cgfs_inode_increment_link_count(request->inode);
            cgfs_inode_update_ctime(request->inode);
        }
        else
        {
            if (result != ENOENT)
            {
                CGUTILS_WARN("Error looking up for hardlinked inode %"PRIu64" from the cache: %d",
                             request->ino,
                             result);
            }

            result = cgfs_inode_init(st,
                                     &(request->inode));

            if (COMPILER_LIKELY(result == 0))
            {
                result = cgfs_cache_add(request->data->cache,
                                        request->inode);

                if (COMPILER_UNLIKELY(result != 0))
                {
                    CGUTILS_WARN("Error adding inode %"PRIu64" to cache: %d",
                                 request->ino,
                                 result);
                }

                result = 0;
            }
            else
            {
                CGUTILS_ERROR("Error getting inode from stat: %d",
                              result);
            }
        }

        if (COMPILER_LIKELY(result == 0))
        {
            CGUTILS_ASSERT(request->inode != NULL);
            CGUTILS_ASSERT(request->type == cgfs_async_request_type_hardlink);
            CGUTILS_ASSERT(request->stat_cb != NULL);

            (*(request->stat_cb))(request->cb_data,
                                  request->inode);
        }
    }
    else
    {
        CGUTILS_ERROR("Error hardlinking existing inode %"PRIu64" to %"PRIu64"->%s: %d",
                      request->ino,
                      request->new_parent_ino,
                      request->name,
                      result);
    }

    if (COMPILER_UNLIKELY(result != 0))
    {
        (*(request->error_cb))(result,
                               request->cb_data);
    }

    CGUTILS_FREE(st);

    cgfs_async_request_free(request), request = NULL;
}

void cgfs_async_hardlink(cgfs_data * const data,
                         uint64_t existing_ino,
                         uint64_t new_parent_ino,
                         char const * const new_name,
                         cgfs_async_stat_cb * const cb,
                         cgfs_async_error_cb * const error_cb,
                         void * const cb_data)
{
    int result = 0;
    cgfs_async_request * request = NULL;

    CGUTILS_ASSERT(data != NULL);
    CGUTILS_ASSERT(existing_ino > 0);
    CGUTILS_ASSERT(new_parent_ino > 0);
    CGUTILS_ASSERT(new_name != NULL);
    CGUTILS_ASSERT(cb != NULL);
    CGUTILS_ASSERT(error_cb != NULL);

    existing_ino = cgfs_translate_inode_number(data,
                                               existing_ino);

    new_parent_ino = cgfs_translate_inode_number(data,
                                                 new_parent_ino);

    result = cgfs_async_request_init(data,
                                     existing_ino,
                                     new_name,
                                     cgfs_async_request_type_hardlink,
                                     cb_data,
                                     error_cb,
                                     &request);

    if (COMPILER_LIKELY(result == 0))
    {
        request->stat_cb = cb;
        request->new_parent_ino = new_parent_ino;

        result = cgsmc_async_hardlink(data->cgsmc_data,
                                      request->ino,
                                      request->new_parent_ino,
                                      request->name,
                                      &cgfs_async_hardlink_callback,
                                      request);

        if (COMPILER_UNLIKELY(result != 0))
        {
            if (result != ENAMETOOLONG)
            {
                CGUTILS_ERROR("Error hardlinking %"PRIu64"->%s to existing inode %"PRIu64": %d",
                              new_parent_ino,
                              new_name,
                              request->ino,
                              result);
            }
        }

        if (COMPILER_UNLIKELY(result != 0))
        {
            cgfs_async_request_free(request), request = NULL;
        }
    }
    else
    {
        CGUTILS_ERROR("Error allocating request: %d",
                      result);
    }

    if (COMPILER_UNLIKELY(result != 0))
    {
        (*error_cb)(result,
                    cb_data);
    }
}

static void cgfs_async_symlink_callback(int const status,
                                        struct stat * st,
                                        void * const cb_data)
{
    cgfs_async_request * request = cb_data;
    CGUTILS_ASSERT(request != NULL);
    int result = status;
    CGUTILS_ASSERT(request->type == cgfs_async_request_type_symlink);

    if (COMPILER_LIKELY(result == 0))
    {
        CGUTILS_ASSERT(st != NULL);
        CGUTILS_ASSERT(request->inode == NULL);

        /* update new parent mtime */
        result = cgfs_cache_lookup(request->data->cache,
                                   request->ino,
                                   (&request->inode));

        if (result == 0)
        {
            cgfs_inode_update_mtime(request->inode,
                                    time(NULL));
            cgfs_inode_release(request->inode), request->inode = NULL;
        }
        else if (result != ENOENT)
        {
            CGUTILS_WARN("Error looking up for renamed inode's old parent %"PRIu64" from the cache: %d",
                         request->ino,
                         result);
        }

        /* create new inode */
        result = cgfs_inode_init(st,
                                 &(request->inode));

        if (COMPILER_LIKELY(result == 0))
        {
            result = cgfs_cache_add(request->data->cache,
                                    request->inode);

            if (COMPILER_UNLIKELY(result != 0))
            {
                CGUTILS_WARN("Error adding inode %"PRIu64" to cache: %d",
                             cgfs_inode_get_number(request->inode),
                             result);
            }

            result = 0;
        }
        else
        {
            CGUTILS_ERROR("Error getting inode from stat: %d",
                          result);
        }

        if (COMPILER_LIKELY(result == 0))
        {
            CGUTILS_ASSERT(request->inode != NULL);
            CGUTILS_ASSERT(request->stat_cb != NULL);

            (*(request->stat_cb))(request->cb_data,
                                  request->inode);
        }
    }
    else
    {
        CGUTILS_ERROR("Error symlinking %"PRIu64"->%s to %s: %d",
                      request->new_parent_ino,
                      request->new_name,
                      request->name,
                      result);
    }

    if (COMPILER_UNLIKELY(result != 0))
    {
        (*(request->error_cb))(result,
                               request->cb_data);
    }

    CGUTILS_FREE(st);

    cgfs_async_request_free(request), request = NULL;
}

void cgfs_async_symlink(cgfs_data * const data,
                        char const * const link_to,
                        uint64_t new_parent_ino,
                        char const * const new_name,
                        uid_t const owner,
                        gid_t const group,
                        cgfs_async_stat_cb * const cb,
                        cgfs_async_error_cb * const error_cb,
                        void * const cb_data)
{
    int result = 0;
    cgfs_async_request * request = NULL;

    CGUTILS_ASSERT(data != NULL);
    CGUTILS_ASSERT(link_to != NULL);
    CGUTILS_ASSERT(new_parent_ino > 0);
    CGUTILS_ASSERT(new_name != NULL);
    CGUTILS_ASSERT(cb != NULL);
    CGUTILS_ASSERT(error_cb != NULL);

    new_parent_ino = cgfs_translate_inode_number(data,
                                                 new_parent_ino);

    result = cgfs_async_request_init(data,
                                     new_parent_ino,
                                     link_to,
                                     cgfs_async_request_type_symlink,
                                     cb_data,
                                     error_cb,
                                     &request);

    if (COMPILER_LIKELY(result == 0))
    {
        request->stat_cb = cb;
        request->uid = owner;
        request->gid = group;

        request->new_name = cgutils_strdup(new_name);

        if (COMPILER_LIKELY(request->new_name != NULL))
        {
            result = cgsmc_async_symlink(data->cgsmc_data,
                                         request->ino,
                                         request->new_name,
                                         request->name,
                                         owner,
                                         group,
                                         &cgfs_async_symlink_callback,
                                         request);

            if (COMPILER_UNLIKELY(result != 0))
            {
                if (result != ENAMETOOLONG)
                {
                    CGUTILS_ERROR("Error symlinking %"PRIu64"->%s to %s: %d",
                                  new_parent_ino,
                                  new_name,
                                  link_to,
                                  result);
                }
            }
        }
        else
        {
            result = ENOMEM;
            CGUTILS_ERROR("Error allocating memory for symlink destination: %d",
                          result);
        }

        if (COMPILER_UNLIKELY(result != 0))
        {
            cgfs_async_request_free(request), request = NULL;
        }
    }
    else
    {
        CGUTILS_ERROR("Error allocating request: %d",
                      result);
    }

    if (COMPILER_UNLIKELY(result != 0))
    {
        (*error_cb)(result,
                    cb_data);
    }
}

static void cgfs_async_readlink_callback(int const status,
                                         char * link_to,
                                         void * const cb_data)
{
    cgfs_async_request * request = cb_data;
    CGUTILS_ASSERT(request != NULL);
    int result = status;
    CGUTILS_ASSERT(request->type == cgfs_async_request_type_readlink);

    if (COMPILER_LIKELY(result == 0))
    {
        CGUTILS_ASSERT(link_to != NULL);
        CGUTILS_ASSERT(request->readlink_cb != NULL);

        (*(request->readlink_cb))(request->cb_data,
                                  link_to);
    }
    else
    {
        CGUTILS_ERROR("Error reading link %"PRIu64": %d",
                      request->ino,
                      result);
    }

    if (COMPILER_UNLIKELY(result != 0))
    {
        (*(request->error_cb))(result,
                               request->cb_data);
    }

    CGUTILS_FREE(link_to);

    cgfs_async_request_free(request), request = NULL;
}

void cgfs_async_readlink(cgfs_data * const data,
                         uint64_t const ino,
                         cgfs_async_readlink_cb * const cb,
                         cgfs_async_error_cb * const error_cb,
                         void * const cb_data)
{
    int result = 0;
    cgfs_async_request * request = NULL;

    CGUTILS_ASSERT(data != NULL);
    CGUTILS_ASSERT(ino > 0);
    CGUTILS_ASSERT(cb != NULL);
    CGUTILS_ASSERT(error_cb != NULL);

    result = cgfs_async_request_init(data,
                                     ino,
                                     NULL,
                                     cgfs_async_request_type_readlink,
                                     cb_data,
                                     error_cb,
                                     &request);

    if (COMPILER_LIKELY(result == 0))
    {
        request->readlink_cb = cb;

        result = cgsmc_async_readlink(data->cgsmc_data,
                                      request->ino,
                                      &cgfs_async_readlink_callback,
                                      request);

        if (COMPILER_UNLIKELY(result != 0))
        {
            CGUTILS_ERROR("Error reading link %"PRIu64": %d",
                          ino,
                          result);
        }

        if (COMPILER_UNLIKELY(result != 0))
        {
            cgfs_async_request_free(request), request = NULL;
        }
    }
    else
    {
        CGUTILS_ERROR("Error allocating request: %d",
                      result);
    }

    if (COMPILER_UNLIKELY(result != 0))
    {
        (*error_cb)(result,
                    cb_data);
    }
}
