/*
   Copyright (c) 2006-2011 Gluster, Inc. <http://www.gluster.com>
   This file is part of GlusterFS.

   GlusterFS is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published
   by the Free Software Foundation; either version 3 of the License,
   or (at your option) any later version.

   GlusterFS is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see
   <http://www.gnu.org/licenses/>.
*/

#include <sys/wait.h>
#include "fuse-bridge.h"

static int gf_fuse_conn_err_log;
static int gf_fuse_xattr_enotsup_log;

void fini (xlator_t *this_xl);

fuse_fd_ctx_t *
__fuse_fd_ctx_check_n_create (xlator_t *this, fd_t *fd)
{
        uint64_t       val    = 0;
        int32_t        ret    = 0;
        fuse_fd_ctx_t *fd_ctx = NULL;

        ret = __fd_ctx_get (fd, this, &val);

        fd_ctx = (fuse_fd_ctx_t *)(unsigned long) val;

        if (fd_ctx == NULL) {
                fd_ctx = GF_CALLOC (1, sizeof (*fd_ctx),
                                    gf_fuse_mt_fd_ctx_t);

                ret = __fd_ctx_set (fd, this,
                                    (uint64_t)(unsigned long)fd_ctx);
                if (ret < 0) {
                        gf_log ("glusterfs-fuse", GF_LOG_DEBUG,
                                "fd-ctx-set failed");
                        GF_FREE (fd_ctx);
                        fd_ctx = NULL;
                }
        }

        return fd_ctx;
}

fuse_fd_ctx_t *
fuse_fd_ctx_check_n_create (xlator_t *this, fd_t *fd)
{
        fuse_fd_ctx_t *fd_ctx = NULL;

        if ((fd == NULL) || (this == NULL)) {
                goto out;
        }

        LOCK (&fd->lock);
        {
                fd_ctx = __fuse_fd_ctx_check_n_create (this, fd);
        }
        UNLOCK (&fd->lock);

out:
        return fd_ctx;
}


fuse_fd_ctx_t *
fuse_fd_ctx_get (xlator_t *this, fd_t *fd)
{
        fuse_fd_ctx_t *fdctx = NULL;
        uint64_t       value = 0;
        int            ret   = 0;

        ret = fd_ctx_get (fd, this, &value);
        if (ret < 0) {
                goto out;
        }

        fdctx = (fuse_fd_ctx_t *) (unsigned long)value;

out:
        return fdctx;
}


/*
 * iov_out should contain a fuse_out_header at zeroth position.
 * The error value of this header is sent to kernel.
 */
static int
send_fuse_iov (xlator_t *this, fuse_in_header_t *finh, struct iovec *iov_out,
               int count)
{
        fuse_private_t *priv = NULL;
        struct fuse_out_header *fouh = NULL;
        int res, i;

        if (!this || !finh || !iov_out) {
                gf_log ("send_fuse_iov", GF_LOG_ERROR,"Invalid arguments");
                return -1;
        }
        priv = this->private;

        fouh = iov_out[0].iov_base;
        iov_out[0].iov_len = sizeof (*fouh);
        fouh->len = 0;
        for (i = 0; i < count; i++)
                fouh->len += iov_out[i].iov_len;
        fouh->unique = finh->unique;

        res = writev (priv->fd, iov_out, count);

        if (res == -1)
                return errno;
        if (res != fouh->len)
                return EINVAL;

        if (priv->fuse_dump_fd != -1) {
                char w = 'W';

                pthread_mutex_lock (&priv->fuse_dump_mutex);
                res = write (priv->fuse_dump_fd, &w, 1);
                if (res != -1)
                        res = writev (priv->fuse_dump_fd, iov_out, count);
                pthread_mutex_unlock (&priv->fuse_dump_mutex);

                if (res == -1)
                        gf_log ("glusterfs-fuse", GF_LOG_ERROR,
                                "failed to dump fuse message (W): %s",
                                strerror (errno));
        }

        return 0;
}

static int
send_fuse_data (xlator_t *this, fuse_in_header_t *finh, void *data, size_t size)
{
        struct fuse_out_header fouh = {0, };
        struct iovec iov_out[2];

        fouh.error = 0;
        iov_out[0].iov_base = &fouh;
        iov_out[1].iov_base = data;
        iov_out[1].iov_len = size;

        return send_fuse_iov (this, finh, iov_out, 2);
}

#define send_fuse_obj(this, finh, obj) \
        send_fuse_data (this, finh, obj, sizeof (*(obj)))


static void
fuse_invalidate (xlator_t *this, uint64_t fuse_ino)
{
        struct fuse_out_header             *fouh   = NULL;
        struct fuse_notify_inval_entry_out *fnieo  = NULL;
        fuse_private_t                     *priv   = NULL;
        dentry_t                           *dentry = NULL;
        inode_t                            *inode  = NULL;
        size_t                              nlen   = 0;
        int                                 rv     = 0;

        char inval_buf[INVAL_BUF_SIZE] = {0,};

        fouh  = (struct fuse_out_header *)inval_buf;
        fnieo = (struct fuse_notify_inval_entry_out *)(fouh + 1);

        priv = this->private;
        if (priv->revchan_out == -1)
                return;

        fouh->unique = 0;
        fouh->error = FUSE_NOTIFY_INVAL_ENTRY;

        inode = fuse_ino_to_inode (fuse_ino, this);

        list_for_each_entry (dentry, &inode->dentry_list, inode_list) {
                nlen = strlen (dentry->name);
                fouh->len = sizeof (*fouh) + sizeof (*fnieo) + nlen + 1;
                fnieo->parent = inode_to_fuse_nodeid (dentry->parent);

                fnieo->namelen = nlen;
                strcpy (inval_buf + sizeof (*fouh) + sizeof (*fnieo), dentry->name);

                rv = write (priv->revchan_out, inval_buf, fouh->len);
                if (rv != fouh->len) {
                        gf_log ("glusterfs-fuse", GF_LOG_ERROR,
                                "kernel notification daemon defunct");

                        close (priv->fd);
                        break;
                }

                gf_log ("glusterfs-fuse", GF_LOG_TRACE, "INVALIDATE entry: "
                        "%"PRIu64"/%s", fnieo->parent, dentry->name);
        }
}

int
send_fuse_err (xlator_t *this, fuse_in_header_t *finh, int error)
{
        struct fuse_out_header fouh = {0, };
        struct iovec iov_out;

        fouh.error = -error;
        iov_out.iov_base = &fouh;

        return send_fuse_iov (this, finh, &iov_out, 1);
}


static int
fuse_entry_cbk (call_frame_t *frame, void *cookie, xlator_t *this,
                int32_t op_ret, int32_t op_errno,
                inode_t *inode, struct iatt *buf, dict_t *xdata)
{
        fuse_state_t          *state        = NULL;
        fuse_in_header_t      *finh         = NULL;
        struct fuse_entry_out  feo          = {0, };
        fuse_private_t        *priv         = NULL;
        inode_t               *linked_inode = NULL;

        priv = this->private;
        state = frame->root->state;
        finh = state->finh;

        if (!op_ret && __is_root_gfid (state->loc.inode->gfid)) {
                buf->ia_ino = 1;
        }

        if (op_ret == 0) {
                gf_log ("glusterfs-fuse", GF_LOG_TRACE,
                        "%"PRIu64": %s() %s => %"PRId64,
                        frame->root->unique, gf_fop_list[frame->root->op],
                        state->loc.path, buf->ia_ino);

                buf->ia_blksize = this->ctx->page_size;
                gf_fuse_stat2attr (buf, &feo.attr);

                if (!buf->ia_ino) {
                        gf_log ("glusterfs-fuse", GF_LOG_WARNING,
                                "%"PRIu64": %s() %s returning inode 0",
                                frame->root->unique,
                                gf_fop_list[frame->root->op], state->loc.path);
                }

                linked_inode = inode_link (inode, state->loc.parent,
                                           state->loc.name, buf);

                if (linked_inode != inode) {
                }

                inode_lookup (linked_inode);

                feo.nodeid = inode_to_fuse_nodeid (linked_inode);

                inode_unref (linked_inode);

                feo.entry_valid =
                        calc_timeout_sec (priv->entry_timeout);
                feo.entry_valid_nsec =
                        calc_timeout_nsec (priv->entry_timeout);
                feo.attr_valid =
                        calc_timeout_sec (priv->attribute_timeout);
                feo.attr_valid_nsec =
                        calc_timeout_nsec (priv->attribute_timeout);

#if FUSE_KERNEL_MINOR_VERSION >= 9
                priv->proto_minor >= 9 ?
                send_fuse_obj (this, finh, &feo) :
                send_fuse_data (this, finh, &feo,
                                FUSE_COMPAT_ENTRY_OUT_SIZE);
#else
                send_fuse_obj (this, finh, &feo);
#endif
        } else {
                gf_log ("glusterfs-fuse",
                        (op_errno == ENOENT ? GF_LOG_TRACE : GF_LOG_WARNING),
                        "%"PRIu64": %s() %s => -1 (%s)", frame->root->unique,
                        gf_fop_list[frame->root->op], state->loc.path,
                        strerror (op_errno));
                send_fuse_err (this, state->finh, op_errno);
        }

        free_fuse_state (state);
        STACK_DESTROY (frame->root);
        return 0;
}


static int
fuse_newentry_cbk (call_frame_t *frame, void *cookie, xlator_t *this,
                   int32_t op_ret, int32_t op_errno,
                   inode_t *inode, struct iatt *buf, struct iatt *preparent,
                   struct iatt *postparent, dict_t *xdata)
{
        fuse_entry_cbk (frame, cookie, this, op_ret, op_errno, inode, buf,
                        xdata);
        return 0;
}


static int
fuse_lookup_cbk (call_frame_t *frame, void *cookie, xlator_t *this,
                 int32_t op_ret, int32_t op_errno,
                 inode_t *inode, struct iatt *stat, dict_t *dict,
                 struct iatt *postparent)
{
        fuse_state_t            *state = NULL;
        call_frame_t            *prev = NULL;
        inode_table_t           *itable = NULL;

        state = frame->root->state;
        prev  = cookie;

        if (op_ret == -1 && state->is_revalidate == 1) {
                itable = state->itable;
                inode_unref (state->loc.inode);
                state->loc.inode = inode_new (itable);
                state->is_revalidate = 2;
                if (uuid_is_null (state->gfid))
                        uuid_generate (state->gfid);
                fuse_gfid_set (state);

                STACK_WIND (frame, fuse_lookup_cbk,
                            prev->this, prev->this->fops->lookup,
                            &state->loc, state->xdata);
                return 0;
        }

        fuse_entry_cbk (frame, cookie, this, op_ret, op_errno, inode, stat,
                        dict);
        return 0;
}

void
fuse_lookup_resume (fuse_state_t *state)
{
        if (!state->loc.parent && !state->loc.inode) {
                gf_log ("fuse", GF_LOG_ERROR, "failed to resolve path %s",
                        state->loc.path);
                send_fuse_err (state->this, state->finh, ENOENT);
                free_fuse_state (state);
                return;
        }

        if (state->loc.inode) {
                gf_log ("glusterfs-fuse", GF_LOG_TRACE,
                        "%"PRIu64": LOOKUP %s(%s)", state->finh->unique,
                        state->loc.path, uuid_utoa (state->loc.inode->gfid));
                state->is_revalidate = 1;
        } else {
                gf_log ("glusterfs-fuse", GF_LOG_TRACE,
                        "%"PRIu64": LOOKUP %s", state->finh->unique,
                        state->loc.path);
                state->loc.inode = inode_new (state->loc.parent->table);
                if (uuid_is_null (state->gfid))
                        uuid_generate (state->gfid);
                fuse_gfid_set (state);
        }

        FUSE_FOP (state, fuse_lookup_cbk, GF_FOP_LOOKUP,
                  lookup, &state->loc, state->xdata);
}

static void
fuse_lookup (xlator_t *this, fuse_in_header_t *finh, void *msg)
{
        char         *name      = msg;
        fuse_state_t *state     = NULL;

        GET_STATE (this, finh, state);

        (void) fuse_resolve_entry_init (state, &state->resolve,
                                        finh->nodeid, name);

        fuse_resolve_and_resume (state, fuse_lookup_resume);
}


static void
fuse_forget (xlator_t *this, fuse_in_header_t *finh, void *msg)

{
        struct fuse_forget_in *ffi = msg;

        inode_t      *fuse_inode;

        if (finh->nodeid == 1) {
                GF_FREE (finh);
                return;
        }

        gf_log ("glusterfs-fuse", GF_LOG_TRACE,
                "%"PRIu64": FORGET %"PRIu64"/%"PRIu64,
                finh->unique, finh->nodeid, ffi->nlookup);

        fuse_inode = fuse_ino_to_inode (finh->nodeid, this);

        inode_forget (fuse_inode, ffi->nlookup);
        inode_unref (fuse_inode);

        GF_FREE (finh);
}


static int
fuse_truncate_cbk (call_frame_t *frame, void *cookie, xlator_t *this,
                   int32_t op_ret, int32_t op_errno, struct iatt *prebuf,
                   struct iatt *postbuf, dict_t *xdata)
{
        fuse_state_t     *state;
        fuse_in_header_t *finh;
        fuse_private_t   *priv = NULL;
        struct fuse_attr_out fao;

        priv  = this->private;
        state = frame->root->state;
        finh  = state->finh;

        if (op_ret == 0) {
                gf_log ("glusterfs-fuse", GF_LOG_TRACE,
                        "%"PRIu64": %s() %s => %"PRId64, frame->root->unique,
                        gf_fop_list[frame->root->op],
                        state->loc.path ? state->loc.path : "ERR",
                        prebuf->ia_ino);

                postbuf->ia_blksize = this->ctx->page_size;
                gf_fuse_stat2attr (postbuf, &fao.attr);

                fao.attr_valid = calc_timeout_sec (priv->attribute_timeout);
                fao.attr_valid_nsec =
                  calc_timeout_nsec (priv->attribute_timeout);

#if FUSE_KERNEL_MINOR_VERSION >= 9
                priv->proto_minor >= 9 ?
                send_fuse_obj (this, finh, &fao) :
                send_fuse_data (this, finh, &fao,
                                FUSE_COMPAT_ATTR_OUT_SIZE);
#else
                send_fuse_obj (this, finh, &fao);
#endif
        } else {
                gf_log ("glusterfs-fuse", GF_LOG_WARNING,
                        "%"PRIu64": %s() %s => -1 (%s)", frame->root->unique,
                        gf_fop_list[frame->root->op],
                        state->loc.path ? state->loc.path : "ERR",
                        strerror (op_errno));

                send_fuse_err (this, finh, op_errno);
        }

        free_fuse_state (state);
        STACK_DESTROY (frame->root);

        return 0;
}


static int
fuse_attr_cbk (call_frame_t *frame, void *cookie, xlator_t *this,
               int32_t op_ret, int32_t op_errno, struct iatt *buf, dict_t *xdata)
{
        fuse_state_t     *state;
        fuse_in_header_t *finh;
        fuse_private_t   *priv = NULL;
        struct fuse_attr_out fao;

        priv  = this->private;
        state = frame->root->state;
        finh  = state->finh;

        if (op_ret == 0) {
                gf_log ("glusterfs-fuse", GF_LOG_TRACE,
                        "%"PRIu64": %s() %s => %"PRId64, frame->root->unique,
                        gf_fop_list[frame->root->op],
                        state->loc.path ? state->loc.path : "ERR",
                        buf->ia_ino);

                buf->ia_blksize = this->ctx->page_size;
                gf_fuse_stat2attr (buf, &fao.attr);

                fao.attr_valid = calc_timeout_sec (priv->attribute_timeout);
                fao.attr_valid_nsec =
                  calc_timeout_nsec (priv->attribute_timeout);

#if FUSE_KERNEL_MINOR_VERSION >= 9
                priv->proto_minor >= 9 ?
                send_fuse_obj (this, finh, &fao) :
                send_fuse_data (this, finh, &fao,
                                FUSE_COMPAT_ATTR_OUT_SIZE);
#else
                send_fuse_obj (this, finh, &fao);
#endif
        } else {
                GF_LOG_OCCASIONALLY ( gf_fuse_conn_err_log, "glusterfs-fuse", 
                                      GF_LOG_WARNING, 
                                      "%"PRIu64": %s() %s => -1 (%s)", 
                                      frame->root->unique,
                                      gf_fop_list[frame->root->op],
                                      state->loc.path ? state->loc.path : "ERR",
                                      strerror (op_errno));

                send_fuse_err (this, finh, op_errno);
        }

        free_fuse_state (state);
        STACK_DESTROY (frame->root);

        return 0;
}


static int
fuse_root_lookup_cbk (call_frame_t *frame, void *cookie, xlator_t *this,
                      int32_t op_ret, int32_t op_errno,
                      inode_t *inode, struct iatt *stat, dict_t *dict,
                      struct iatt *postparent)
{
        fuse_attr_cbk (frame, cookie, this, op_ret, op_errno, stat, dict);

        return 0;
}

void
fuse_getattr_resume (fuse_state_t *state)
{
        if (!state->loc.inode) {
                gf_log ("glusterfs-fuse", GF_LOG_ERROR,
                        "%"PRIu64": GETATTR %"PRIu64" (%s) resolution failed",
                        state->finh->unique, state->finh->nodeid,
			uuid_utoa (state->resolve.gfid));
                send_fuse_err (state->this, state->finh, ENOENT);
                free_fuse_state (state);
                return;
        }

	if (!IA_ISDIR (state->loc.inode->ia_type)) {
		state->fd = fd_lookup (state->loc.inode, state->finh->pid);
	}

        if (!state->fd) {
                gf_log ("glusterfs-fuse", GF_LOG_TRACE,
                        "%"PRIu64": GETATTR %"PRIu64" (%s)",
                        state->finh->unique, state->finh->nodeid,
                        state->loc.path);

                FUSE_FOP (state, fuse_attr_cbk, GF_FOP_STAT,
                          stat, &state->loc, state->xdata);
        } else {

                gf_log ("glusterfs-fuse", GF_LOG_TRACE,
                        "%"PRIu64": FGETATTR %"PRIu64" (%s/%p)",
                        state->finh->unique, state->finh->nodeid,
                        state->loc.path, state->fd);

                FUSE_FOP (state, fuse_attr_cbk, GF_FOP_FSTAT,
                          fstat, state->fd, state->xdata);
        }
}

static void
fuse_getattr (xlator_t *this, fuse_in_header_t *finh, void *msg)
{
        fuse_state_t *state;
        int32_t       ret = -1;

        GET_STATE (this, finh, state);

        if (finh->nodeid == 1) {
                state->gfid[15] = 1;

                ret = fuse_loc_fill (&state->loc, state, finh->nodeid, 0, NULL);
                if (ret < 0) {
                        gf_log ("glusterfs-fuse", GF_LOG_WARNING,
                                "%"PRIu64": GETATTR on / (fuse_loc_fill() failed)",
                                finh->unique);
                        send_fuse_err (this, finh, ENOENT);
                        free_fuse_state (state);
                        return;
                }

                fuse_gfid_set (state);

                FUSE_FOP (state, fuse_root_lookup_cbk, GF_FOP_LOOKUP,
                          lookup, &state->loc, state->xdata);
                return;
        }

        fuse_resolve_inode_init (state, &state->resolve, state->finh->nodeid);

        fuse_resolve_and_resume (state, fuse_getattr_resume);
}


static int32_t
fuse_fd_inherit_directio (xlator_t *this, fd_t *fd, struct fuse_open_out *foo)
{
        int32_t        ret    = 0;
        fuse_fd_ctx_t *fdctx  = NULL, *tmp_fdctx = NULL;
        fd_t          *tmp_fd = NULL;

        GF_VALIDATE_OR_GOTO_WITH_ERROR ("glusterfs-fuse", this, out, ret,
                                        -EINVAL);
        GF_VALIDATE_OR_GOTO_WITH_ERROR ("glusterfs-fuse", fd, out, ret,
                                        -EINVAL);
        GF_VALIDATE_OR_GOTO_WITH_ERROR ("glusterfs-fuse", foo, out, ret,
                                        -EINVAL);

        fdctx = fuse_fd_ctx_check_n_create (this, fd);
        if (!fdctx) {
                ret = -ENOMEM;
                goto out;
        }

        tmp_fd = fd_lookup (fd->inode, 0);
        if (tmp_fd) {
                tmp_fdctx = fuse_fd_ctx_get (this, fd);
                if (tmp_fdctx) {
                        foo->open_flags &= ~FOPEN_DIRECT_IO;
                        foo->open_flags |= (tmp_fdctx->open_flags
                                            & FOPEN_DIRECT_IO);
                }
        }

        fdctx->open_flags |= (foo->open_flags & FOPEN_DIRECT_IO);

        if (tmp_fd != NULL) {
                fd_unref (tmp_fd);
        }

        ret = 0;
out:
        return ret;
}


static int
fuse_fd_cbk (call_frame_t *frame, void *cookie, xlator_t *this,
             int32_t op_ret, int32_t op_errno, fd_t *fd, dict_t *xdata)
{
        fuse_state_t         *state  = NULL;
        fuse_in_header_t     *finh   = NULL;
        fuse_private_t       *priv   = NULL;
        int32_t               ret    = 0;
        struct fuse_open_out  foo    = {0, };

        priv = this->private;
        state = frame->root->state;
        finh = state->finh;

        if (op_ret >= 0) {
                foo.fh = (uintptr_t) fd;
                foo.open_flags = 0;

                if (!IA_ISDIR (fd->inode->ia_type)) {
                        if (((priv->direct_io_mode == 2)
                             && ((state->flags & O_ACCMODE) != O_RDONLY))
                            || (priv->direct_io_mode == 1))
                                foo.open_flags |= FOPEN_DIRECT_IO;
#ifdef GF_DARWIN_HOST_OS
                                /* In Linux: by default, buffer cache
                                 * is purged upon open, setting
                                 * FOPEN_KEEP_CACHE implies no-purge
                                 *
                                 * In MacFUSE: by default, buffer cache
                                 * is left intact upon open, setting
                                 * FOPEN_PURGE_UBC implies purge
                                 *
                                 * [[Interesting...]]
                                 */
                                foo.open_flags |= FOPEN_PURGE_UBC;
#endif
                }

                gf_log ("glusterfs-fuse", GF_LOG_TRACE,
                        "%"PRIu64": %s() %s => %p", frame->root->unique,
                        gf_fop_list[frame->root->op], state->loc.path, fd);

                ret = fuse_fd_inherit_directio (this, fd, &foo);
                if (ret < 0) {
                        op_errno = -ret;
                        gf_log ("glusterfs-fuse", GF_LOG_WARNING,
                                "cannot inherit direct-io values from fds "
                                "already opened");
                        goto err;
                }

                if (send_fuse_obj (this, finh, &foo) == ENOENT) {
                        gf_log ("glusterfs-fuse", GF_LOG_DEBUG,
                                "open(%s) got EINTR", state->loc.path);
                        gf_fd_put (priv->fdtable, state->fd_no);
                        goto out;
                }

                fd_bind (fd);
        } else {
        err:
                gf_log ("glusterfs-fuse", GF_LOG_WARNING,
                        "%"PRIu64": %s() %s => -1 (%s)", frame->root->unique,
                        gf_fop_list[frame->root->op], state->loc.path,
                        strerror (op_errno));

                send_fuse_err (this, finh, op_errno);
                gf_fd_put (priv->fdtable, state->fd_no);
        }
out:
        free_fuse_state (state);
        STACK_DESTROY (frame->root);
        return 0;
}


static void
fuse_do_truncate (fuse_state_t *state, size_t size)
{
        if (state->fd) {
                FUSE_FOP (state, fuse_truncate_cbk, GF_FOP_FTRUNCATE,
                          ftruncate, state->fd, size, state->xdata);
        } else {
                FUSE_FOP (state, fuse_truncate_cbk, GF_FOP_TRUNCATE,
                          truncate, &state->loc, size, state->xdata);
        }

        return;
}


static int
fuse_setattr_cbk (call_frame_t *frame, void *cookie, xlator_t *this,
                  int32_t op_ret, int32_t op_errno,
                  struct iatt *statpre, struct iatt *statpost, dict_t *xdata)
{
        fuse_state_t     *state;
        fuse_in_header_t *finh;
        fuse_private_t   *priv = NULL;
        struct fuse_attr_out fao;

        int op_done = 0;

        priv  = this->private;
        state = frame->root->state;
        finh  = state->finh;

        if (op_ret == 0) {
                gf_log ("glusterfs-fuse", GF_LOG_TRACE,
                        "%"PRIu64": %s() %s => %"PRId64, frame->root->unique,
                        gf_fop_list[frame->root->op],
                        state->loc.path ? state->loc.path : "ERR",
                        statpost->ia_ino);

                statpost->ia_blksize = this->ctx->page_size;
                gf_fuse_stat2attr (statpost, &fao.attr);

                fao.attr_valid = calc_timeout_sec (priv->attribute_timeout);
                fao.attr_valid_nsec =
                        calc_timeout_nsec (priv->attribute_timeout);

                if (state->truncate_needed) {
                        fuse_do_truncate (state, state->size);
                } else {
#if FUSE_KERNEL_MINOR_VERSION >= 9
                        priv->proto_minor >= 9 ?
                        send_fuse_obj (this, finh, &fao) :
                        send_fuse_data (this, finh, &fao,
                                        FUSE_COMPAT_ATTR_OUT_SIZE);
#else
                        send_fuse_obj (this, finh, &fao);
#endif
                        op_done = 1;
                }
        } else {
                gf_log ("glusterfs-fuse", GF_LOG_WARNING,
                        "%"PRIu64": %s() %s => -1 (%s)", frame->root->unique,
                        gf_fop_list[frame->root->op],
                        state->loc.path ? state->loc.path : "ERR",
                        strerror (op_errno));

                send_fuse_err (this, finh, op_errno);
                op_done = 1;
        }

        if (op_done) {
                free_fuse_state (state);
        }

        STACK_DESTROY (frame->root);

        return 0;
}


static int32_t
fattr_to_gf_set_attr (int32_t valid)
{
        int32_t gf_valid = 0;

        if (valid & FATTR_MODE)
                gf_valid |= GF_SET_ATTR_MODE;

        if (valid & FATTR_UID)
                gf_valid |= GF_SET_ATTR_UID;

        if (valid & FATTR_GID)
                gf_valid |= GF_SET_ATTR_GID;

        if (valid & FATTR_ATIME)
                gf_valid |= GF_SET_ATTR_ATIME;

        if (valid & FATTR_MTIME)
                gf_valid |= GF_SET_ATTR_MTIME;

        if (valid & FATTR_SIZE)
                gf_valid |= GF_SET_ATTR_SIZE;

        return gf_valid;
}


#define FATTR_MASK   (FATTR_SIZE                        \
                      | FATTR_UID | FATTR_GID           \
                      | FATTR_ATIME | FATTR_MTIME       \
                      | FATTR_MODE)

void
fuse_setattr_resume (fuse_state_t *state)
{
        if (!state->fd && !state->loc.inode) {
                gf_log ("glusterfs-fuse", GF_LOG_ERROR,
                        "%"PRIu64": SETATTR %"PRIu64" (%s) resolution failed",
                        state->finh->unique, state->finh->nodeid,
			uuid_utoa (state->resolve.gfid));
                send_fuse_err (state->this, state->finh, ENOENT);
                free_fuse_state (state);
                return;
        }

        gf_log ("glusterfs-fuse", GF_LOG_TRACE,
                "%"PRIu64": SETATTR (%"PRIu64")%s", state->finh->unique,
                state->finh->nodeid, state->loc.path);

#ifdef GF_TEST_FFOP
        /* this is for calls like 'fchmod()' */
        if (!state->fd)
                state->fd = fd_lookup (state->loc.inode, state->finh->pid);
#endif /* GF_TEST_FFOP */

        if ((state->valid & (FATTR_MASK)) != FATTR_SIZE) {
                if (state->fd &&
                    !((state->valid & FATTR_ATIME) ||
                      (state->valid & FATTR_MTIME))) {
                        /*
                            there is no "futimes" call, so don't send
                            fsetattr if ATIME or MTIME is set
                         */

                        FUSE_FOP (state, fuse_setattr_cbk, GF_FOP_FSETATTR,
                                  fsetattr, state->fd, &state->attr,
                                  fattr_to_gf_set_attr (state->valid),
                                  state->xdata);
                } else {
                        FUSE_FOP (state, fuse_setattr_cbk, GF_FOP_SETATTR,
                                  setattr, &state->loc, &state->attr,
                                  fattr_to_gf_set_attr (state->valid),
                                  state->xdata);
                }
        } else {
                fuse_do_truncate (state, state->size);
        }

}

static void
fuse_setattr (xlator_t *this, fuse_in_header_t *finh, void *msg)
{
        struct fuse_setattr_in *fsi = msg;

        fuse_private_t  *priv = NULL;
        fuse_state_t *state = NULL;

        GET_STATE (this, finh, state);

        if (fsi->valid & FATTR_FH &&
            !(fsi->valid & (FATTR_ATIME|FATTR_MTIME))) {
                /* We need no loc if kernel sent us an fd and
                 * we are not fiddling with times */
                state->fd = FH_TO_FD (fsi->fh);
		fuse_resolve_fd_init (state, &state->resolve, state->fd);
	} else {
		fuse_resolve_inode_init (state, &state->resolve, finh->nodeid);
	}

        /*
         * This is just stub code demonstrating how to retrieve
         * lock_owner in setattr, according to the FUSE proto.
         * We do not make use of ATM. Its purpose is supporting
         * mandatory locking, but getting that right is further
         * down the road. Cf.
         *
         * http://thread.gmane.org/gmane.comp.file-systems.fuse.devel/
         * 4962/focus=4982
         *
         * http://git.kernel.org/?p=linux/kernel/git/torvalds/
         * linux-2.6.git;a=commit;h=v2.6.23-5896-gf333211
         */
        priv = this->private;
#if FUSE_KERNEL_MINOR_VERSION >= 9
        if (priv->proto_minor >= 9 && fsi->valid & FATTR_LOCKOWNER)
                state->lk_owner = fsi->lock_owner;
#endif

        state->valid = fsi->valid;

        if ((fsi->valid & (FATTR_MASK)) != FATTR_SIZE) {
                if (fsi->valid & FATTR_SIZE) {
                        state->size            = fsi->size;
                        state->truncate_needed = _gf_true;
                }

                state->attr.ia_size  = fsi->size;
                state->attr.ia_atime = fsi->atime;
                state->attr.ia_mtime = fsi->mtime;
                state->attr.ia_atime_nsec = fsi->atimensec;
                state->attr.ia_mtime_nsec = fsi->mtimensec;

                state->attr.ia_prot = ia_prot_from_st_mode (fsi->mode);
                state->attr.ia_uid  = fsi->uid;
                state->attr.ia_gid  = fsi->gid;
        } else {
                state->size = fsi->size;
        }

        fuse_resolve_and_resume (state, fuse_setattr_resume);
}


static int
fuse_err_cbk (call_frame_t *frame, void *cookie, xlator_t *this,
              int32_t op_ret, int32_t op_errno, dict_t *xdata)
{
        fuse_state_t *state = frame->root->state;
        fuse_in_header_t *finh = state->finh;

        if (op_ret == 0) {
                gf_log ("glusterfs-fuse", GF_LOG_TRACE,
                        "%"PRIu64": %s() %s => 0", frame->root->unique,
                        gf_fop_list[frame->root->op],
                        state->loc.path ? state->loc.path : "ERR");

                send_fuse_err (this, finh, 0);
        } else {
                gf_log ("glusterfs-fuse", GF_LOG_WARNING,
                        "%"PRIu64": %s() %s => -1 (%s)",
                        frame->root->unique,
                        gf_fop_list[frame->root->op],
                        state->loc.path ? state->loc.path : "ERR",
                        strerror (op_errno));

                send_fuse_err (this, finh, op_errno);
        }

        free_fuse_state (state);
        STACK_DESTROY (frame->root);

        return 0;
}


static int
fuse_fsync_cbk (call_frame_t *frame, void *cookie, xlator_t *this,
                int32_t op_ret, int32_t op_errno, struct iatt *prebuf,
                struct iatt *postbuf, dict_t *xdata)
{
        return fuse_err_cbk (frame, cookie, this, op_ret, op_errno, xdata);
}


static int
fuse_setxattr_cbk (call_frame_t *frame, void *cookie, xlator_t *this,
                   int32_t op_ret, int32_t op_errno, dict_t *xdata)
{
        if (op_ret == -1 && op_errno == ENOTSUP)
                GF_LOG_OCCASIONALLY (gf_fuse_xattr_enotsup_log,
                                     "glusterfs-fuse", GF_LOG_CRITICAL,
                                     "extended attribute not supported "
                                     "by the backend storage");

        return fuse_err_cbk (frame, cookie, this, op_ret, op_errno, xdata);
}


static int
fuse_unlink_cbk (call_frame_t *frame, void *cookie, xlator_t *this,
                 int32_t op_ret, int32_t op_errno, struct iatt *preparent,
                 struct iatt *postparent, dict_t *xdata)
{
        fuse_state_t     *state = NULL;
        fuse_in_header_t *finh = NULL;

        state = frame->root->state;
        finh = state->finh;

        if (op_ret == 0)
                inode_unlink (state->loc.inode, state->loc.parent,
                              state->loc.name);

        if (op_ret == 0) {
                gf_log ("glusterfs-fuse", GF_LOG_TRACE,
                        "%"PRIu64": %s() %s => 0", frame->root->unique,
                        gf_fop_list[frame->root->op], state->loc.path);

                send_fuse_err (this, finh, 0);
        } else {
                gf_log ("glusterfs-fuse",
                        op_errno == ENOTEMPTY ? GF_LOG_DEBUG : GF_LOG_WARNING,
                        "%"PRIu64": %s() %s => -1 (%s)", frame->root->unique,
                        gf_fop_list[frame->root->op], state->loc.path,
                        strerror (op_errno));

                send_fuse_err (this, finh, op_errno);
        }

        free_fuse_state (state);
        STACK_DESTROY (frame->root);

        return 0;
}


void
fuse_access_resume (fuse_state_t *state)
{
        if (!state->loc.inode) {
                gf_log ("glusterfs-fuse", GF_LOG_ERROR,
                        "%"PRIu64": ACCESS %"PRIu64" (%s) resolution failed",
                        state->finh->unique, state->finh->nodeid,
			uuid_utoa (state->resolve.gfid));
                send_fuse_err (state->this, state->finh, ENOENT);
                free_fuse_state (state);
                return;
        }

        gf_log ("glusterfs-fuse", GF_LOG_TRACE,
                "%"PRIu64" ACCESS %s/%"PRIu64" mask=%d",
                state->finh->unique, state->loc.path,
                state->finh->nodeid, state->mask);

        FUSE_FOP (state, fuse_err_cbk, GF_FOP_ACCESS, access,
                  &state->loc, state->mask, state->xdata);
}


static void
fuse_access (xlator_t *this, fuse_in_header_t *finh, void *msg)
{
        struct fuse_access_in *fai = msg;
        fuse_state_t *state = NULL;

        GET_STATE (this, finh, state);

        fuse_resolve_inode_init (state, &state->resolve, finh->nodeid);

        state->mask = fai->mask;

        fuse_resolve_and_resume (state, fuse_access_resume);

        return;
}


static int
fuse_readlink_cbk (call_frame_t *frame, void *cookie, xlator_t *this,
                   int32_t op_ret, int32_t op_errno, const char *linkname,
                   struct iatt *buf, dict_t *xdata)
{
        fuse_state_t     *state = NULL;
        fuse_in_header_t *finh = NULL;

        state = frame->root->state;
        finh = state->finh;

        if (op_ret > 0) {
                ((char *)linkname)[op_ret] = '\0';

                gf_log ("glusterfs-fuse", GF_LOG_TRACE,
                        "%"PRIu64": %s => %s", frame->root->unique,
                        state->loc.path, linkname);

                send_fuse_data (this, finh, (void *)linkname, op_ret + 1);
        } else {
                gf_log ("glusterfs-fuse", GF_LOG_WARNING,
                        "%"PRIu64": %s => -1 (%s)", frame->root->unique,
                        state->loc.path, strerror (op_errno));

                send_fuse_err (this, finh, op_errno);
        }

        free_fuse_state (state);
        STACK_DESTROY (frame->root);

        return 0;
}


void
fuse_readlink_resume (fuse_state_t *state)
{
        if (!state->loc.inode) {
                gf_log ("glusterfs-fuse", GF_LOG_ERROR,
                        "READLINK %"PRIu64" (%s) resolution failed",
                        state->finh->unique, uuid_utoa (state->resolve.gfid));
                send_fuse_err (state->this, state->finh, ENOENT);
                free_fuse_state (state);
                return;
        }

        gf_log ("glusterfs-fuse", GF_LOG_TRACE,
                "%"PRIu64" READLINK %s/%s", state->finh->unique,
                state->loc.path, uuid_utoa (state->loc.inode->gfid));

        FUSE_FOP (state, fuse_readlink_cbk, GF_FOP_READLINK,
                  readlink, &state->loc, 4096, state->xdata);
}


static void
fuse_readlink (xlator_t *this, fuse_in_header_t *finh, void *msg)
{
        fuse_state_t *state = NULL;

        GET_STATE (this, finh, state);

	fuse_resolve_inode_init (state, &state->resolve, finh->nodeid);

        fuse_resolve_and_resume (state, fuse_readlink_resume);

        return;
}


void
fuse_mknod_resume (fuse_state_t *state)
{
        if (!state->loc.parent) {
                gf_log ("glusterfs-fuse", GF_LOG_ERROR,
			"MKNOD %"PRId64"/%s (%s/%s) resolution failed",
                        state->finh->nodeid, state->resolve.bname,
			uuid_utoa (state->resolve.gfid), state->resolve.bname);
                send_fuse_err (state->this, state->finh, ENOENT);
                free_fuse_state (state);
                return;
        }

        if (state->resolve.op_errno == ENOENT) {
                state->resolve.op_ret = 0;
                state->resolve.op_errno = 0;
        }

        if (state->loc.inode) {
                gf_log (state->this->name, GF_LOG_DEBUG, "inode already present");
                inode_unref (state->loc.inode);
		state->loc.inode = NULL;
        }

        state->loc.inode = inode_new (state->loc.parent->table);

        gf_log ("glusterfs-fuse", GF_LOG_TRACE,
                "%"PRIu64": MKNOD %s", state->finh->unique,
                state->loc.path);

        FUSE_FOP (state, fuse_newentry_cbk, GF_FOP_MKNOD,
                  mknod, &state->loc, state->mode, state->rdev, state->umask,
                  state->xdata);
}


static void
fuse_mknod (xlator_t *this, fuse_in_header_t *finh, void *msg)
{
        struct fuse_mknod_in *fmi = msg;
        char         *name = (char *)(fmi + 1);

        fuse_state_t   *state = NULL;
        fuse_private_t *priv = NULL;
        int32_t         ret = -1;

        priv = this->private;
#if FUSE_KERNEL_MINOR_VERSION >= 12
        if (priv->proto_minor < 12)
                name = (char *)msg + FUSE_COMPAT_MKNOD_IN_SIZE;
#endif

        GET_STATE (this, finh, state);

        uuid_generate (state->gfid);

	fuse_resolve_entry_init (state, &state->resolve, finh->nodeid, name);

        state->mode = fmi->mode;
        state->rdev = fmi->rdev;

        priv = this->private;
#if FUSE_KERNEL_MINOR_VERSION >=12
        if (priv->proto_minor >= 12)
                state->mode &= ~fmi->umask;
        if (priv->proto_minor >= 12 && priv->acl) {
                state->xdata = dict_new ();
                if (!state->xdata) {
                        gf_log ("glusterfs-fuse", GF_LOG_WARNING,
                                "MKNOD Failed to allocate a param dictionary");
                        send_fuse_err (this, finh, ENOMEM);
                        free_fuse_state (state);
                        return;
                }
                state->umask = fmi->umask;
                ret = dict_set_int16 (state->xdata, "umask", fmi->umask);
                if (ret < 0) {
                        gf_log ("glusterfs-fuse", GF_LOG_WARNING,
                                "MKNOD Failed adding umask to request");
                        dict_destroy (state->xdata);
                        send_fuse_err (this, finh, ENOMEM);
                        free_fuse_state (state);
                        return;
                }
                ret = dict_set_int16 (state->xdata, "mode", fmi->mode);
                if (ret < 0) {
                        gf_log ("glusterfs-fuse", GF_LOG_WARNING,
                                "MKNOD Failed adding mode to request");
                        dict_destroy (state->xdata);
                        send_fuse_err (this, finh, ENOMEM);
                        free_fuse_state (state);
                        return;
                }
        }
#endif

        fuse_resolve_and_resume (state, fuse_mknod_resume);

        return;
}


void
fuse_mkdir_resume (fuse_state_t *state)
{
        if (!state->loc.parent) {
                gf_log ("glusterfs-fuse", GF_LOG_ERROR,
			"MKDIR %"PRId64" (%s/%s) resolution failed",
                        state->finh->nodeid, uuid_utoa (state->resolve.gfid),
			state->resolve.bname);
                send_fuse_err (state->this, state->finh, ENOENT);
                free_fuse_state (state);
                return;
        }

        if (state->resolve.op_errno == ENOENT) {
                state->resolve.op_ret = 0;
                state->resolve.op_errno = 0;
        }

        if (state->loc.inode) {
                gf_log (state->this->name, GF_LOG_DEBUG, "inode already present");
                inode_unref (state->loc.inode);
		state->loc.inode = NULL;
        }

        state->loc.inode = inode_new (state->loc.parent->table);

        gf_log ("glusterfs-fuse", GF_LOG_TRACE,
                "%"PRIu64": MKDIR %s", state->finh->unique,
                state->loc.path);

        FUSE_FOP (state, fuse_newentry_cbk, GF_FOP_MKDIR,
                  mkdir, &state->loc, state->mode, state->umask, state->xdata);
}


static void
fuse_mkdir (xlator_t *this, fuse_in_header_t *finh, void *msg)
{
        struct fuse_mkdir_in *fmi = msg;
        char *name = (char *)(fmi + 1);
        fuse_private_t *priv = NULL;

        fuse_state_t *state;
        int32_t ret = -1;

        GET_STATE (this, finh, state);

        uuid_generate (state->gfid);

	fuse_resolve_entry_init (state, &state->resolve, finh->nodeid, name);

        state->mode = fmi->mode;

        priv = this->private;
#if FUSE_KERNEL_MINOR_VERSION >=12
        if (priv->proto_minor >= 12)
                state->mode &= ~fmi->umask;
        if (priv->proto_minor >= 12 && priv->acl) {
                state->xdata = dict_new ();
                if (!state->xdata) {
                        gf_log ("glusterfs-fuse", GF_LOG_WARNING,
                                "MKDIR Failed to allocate a param dictionary");
                        send_fuse_err (this, finh, ENOMEM);
                        free_fuse_state (state);
                        return;
                }
                state->umask = fmi->umask;
                ret = dict_set_int16 (state->xdata, "umask", fmi->umask);
                if (ret < 0) {
                        gf_log ("glusterfs-fuse", GF_LOG_WARNING,
                                "MKDIR Failed adding umask to request");
                        dict_destroy (state->xdata);
                        send_fuse_err (this, finh, ENOMEM);
                        free_fuse_state (state);
                        return;
                }
                ret = dict_set_int16 (state->xdata, "mode", fmi->mode);
                if (ret < 0) {
                        gf_log ("glusterfs-fuse", GF_LOG_WARNING,
                                "MKDIR Failed adding mode to request");
                        dict_destroy (state->xdata);
                        send_fuse_err (this, finh, ENOMEM);
                        free_fuse_state (state);
                        return;
                }
        }
#endif

        fuse_resolve_and_resume (state, fuse_mkdir_resume);

        return;
}


void
fuse_unlink_resume (fuse_state_t *state)
{
        if (!state->loc.parent || !state->loc.inode) {
                gf_log ("glusterfs-fuse", GF_LOG_ERROR,
			"UNLINK %"PRId64" (%s/%s) resolution failed",
			state->finh->nodeid, uuid_utoa (state->resolve.gfid),
			state->resolve.bname);
                send_fuse_err (state->this, state->finh, ENOENT);
                free_fuse_state (state);
                return;
        }

        gf_log ("glusterfs-fuse", GF_LOG_TRACE,
                "%"PRIu64": UNLINK %s", state->finh->unique,
                state->loc.path);

        FUSE_FOP (state, fuse_unlink_cbk, GF_FOP_UNLINK,
                  unlink, &state->loc, 0, state->xdata);
}


static void
fuse_unlink (xlator_t *this, fuse_in_header_t *finh, void *msg)
{
        char         *name = msg;
        fuse_state_t *state = NULL;

        GET_STATE (this, finh, state);

	fuse_resolve_entry_init (state, &state->resolve, finh->nodeid, name);

        fuse_resolve_and_resume (state, fuse_unlink_resume);

        return;
}

void
fuse_rmdir_resume (fuse_state_t *state)
{
        if (!state->loc.parent || !state->loc.inode) {
                gf_log ("glusterfs-fuse", GF_LOG_ERROR,
			"RMDIR %"PRId64" (%s/%s) resolution failed",
			state->finh->nodeid, uuid_utoa (state->resolve.gfid),
			state->resolve.bname);
                send_fuse_err (state->this, state->finh, ENOENT);
                free_fuse_state (state);
                return;
        }

        gf_log ("glusterfs-fuse", GF_LOG_TRACE,
                "%"PRIu64": RMDIR %s", state->finh->unique,
                state->loc.path);

        FUSE_FOP (state, fuse_unlink_cbk, GF_FOP_RMDIR,
                  rmdir, &state->loc, 0, state->xdata);
}


static void
fuse_rmdir (xlator_t *this, fuse_in_header_t *finh, void *msg)
{
        char         *name = msg;
        fuse_state_t *state = NULL;

        GET_STATE (this, finh, state);

	fuse_resolve_entry_init (state, &state->resolve, finh->nodeid, name);

        fuse_resolve_and_resume (state, fuse_rmdir_resume);

        return;
}


void
fuse_symlink_resume (fuse_state_t *state)
{
        if (!state->loc.parent) {
                gf_log ("glusterfs-fuse", GF_LOG_ERROR,
			"SYMLINK %"PRId64" (%s/%s) -> %s resolution failed",
                        state->finh->nodeid, uuid_utoa (state->resolve.gfid),
			state->resolve.bname, state->name);
                send_fuse_err (state->this, state->finh, ENOENT);
                free_fuse_state (state);
                return;
        }

        if (state->resolve.op_errno == ENOENT) {
                state->resolve.op_ret = 0;
                state->resolve.op_errno = 0;
        }

        if (state->loc.inode) {
                gf_log (state->this->name, GF_LOG_DEBUG, "inode already present");
                inode_unref (state->loc.inode);
		state->loc.inode = NULL;
        }

        state->loc.inode = inode_new (state->loc.parent->table);

        gf_log ("glusterfs-fuse", GF_LOG_TRACE,
                "%"PRIu64": SYMLINK %s -> %s", state->finh->unique,
                state->loc.path, state->name);

        FUSE_FOP (state, fuse_newentry_cbk, GF_FOP_SYMLINK,
                  symlink, state->name, &state->loc, state->umask, state->xdata);
}


static void
fuse_symlink (xlator_t *this, fuse_in_header_t *finh, void *msg)
{
        char         *name = msg;
        char         *linkname = name + strlen (name) + 1;
        fuse_state_t *state = NULL;

        GET_STATE (this, finh, state);

        uuid_generate (state->gfid);

	fuse_resolve_entry_init (state, &state->resolve, finh->nodeid, name);

        state->name = gf_strdup (linkname);

        fuse_resolve_and_resume (state, fuse_symlink_resume);

        return;
}


int
fuse_rename_cbk (call_frame_t *frame, void *cookie, xlator_t *this,
                 int32_t op_ret, int32_t op_errno, struct iatt *buf,
                 struct iatt *preoldparent, struct iatt *postoldparent,
                 struct iatt *prenewparent, struct iatt *postnewparent,
                 dict_t *xdata)
{
        fuse_state_t     *state = NULL;
        fuse_in_header_t *finh = NULL;

        state = frame->root->state;
        finh  = state->finh;

        if (op_ret == 0) {
                gf_log ("glusterfs-fuse", GF_LOG_TRACE,
                        "%"PRIu64": %s -> %s => 0 (buf->ia_ino=%"PRId64")",
                        frame->root->unique, state->loc.path, state->loc2.path,
                        buf->ia_ino);

                {
                        /* ugly ugly - to stay blind to situation where
                           rename happens on a new inode
                        */
                        buf->ia_type = state->loc.inode->ia_type;
                }
                buf->ia_blksize = this->ctx->page_size;

                inode_rename (state->loc.parent->table,
                              state->loc.parent, state->loc.name,
                              state->loc2.parent, state->loc2.name,
                              state->loc.inode, buf);

                send_fuse_err (this, finh, 0);
        } else {
                gf_log ("glusterfs-fuse", GF_LOG_WARNING,
                        "%"PRIu64": %s -> %s => -1 (%s)", frame->root->unique,
                        state->loc.path, state->loc2.path,
                        strerror (op_errno));
                send_fuse_err (this, finh, op_errno);
        }

        free_fuse_state (state);
        STACK_DESTROY (frame->root);
        return 0;
}

void
fuse_rename_resume (fuse_state_t *state)
{
        char loc_uuid[64]  = {0,};
        char loc2_uuid[64] = {0,};

        if (!state->loc.parent || !state->loc.inode) {
                gf_log ("glusterfs-fuse", GF_LOG_ERROR,
                        "RENAME %"PRIu64" %s/%s -> %s/%s src resolution failed",
			state->finh->unique,
			uuid_utoa_r (state->resolve.gfid, loc_uuid),
			state->resolve.bname,
			uuid_utoa_r (state->resolve2.gfid, loc2_uuid),
			state->resolve2.bname);

                send_fuse_err (state->this, state->finh, ENOENT);
                free_fuse_state (state);
                return;
        }

        if (!state->loc2.parent) {
                gf_log ("glusterfs-fuse", GF_LOG_ERROR,
                        "RENAME %"PRIu64" %s/%s -> %s/%s dst resolution failed",
			state->finh->unique,
			uuid_utoa_r (state->resolve.gfid, loc_uuid),
			state->resolve.bname,
			uuid_utoa_r (state->resolve2.gfid, loc2_uuid),
			state->resolve2.bname);

                send_fuse_err (state->this, state->finh, ENOENT);
                free_fuse_state (state);
                return;
        }

        state->resolve.op_ret = 0;
        state->resolve2.op_ret = 0;

        gf_log ("glusterfs-fuse", GF_LOG_TRACE,
                "%"PRIu64": RENAME `%s (%s)' -> `%s (%s)'",
                state->finh->unique, state->loc.path, loc_uuid,
                state->loc2.path, loc2_uuid);

        FUSE_FOP (state, fuse_rename_cbk, GF_FOP_RENAME,
                  rename, &state->loc, &state->loc2, state->xdata);
}


static void
fuse_rename (xlator_t *this, fuse_in_header_t *finh, void *msg)
{
        struct fuse_rename_in  *fri = msg;
        char *oldname = (char *)(fri + 1);
        char *newname = oldname + strlen (oldname) + 1;
        fuse_state_t *state = NULL;

        GET_STATE (this, finh, state);

	fuse_resolve_entry_init (state, &state->resolve, finh->nodeid, oldname);

	fuse_resolve_entry_init (state, &state->resolve2, fri->newdir, newname);

        fuse_resolve_and_resume (state, fuse_rename_resume);

        return;
}


void
fuse_link_resume (fuse_state_t *state)
{
        if (!state->loc2.inode || !state->loc.parent) {
                gf_log ("glusterfs-fuse", GF_LOG_WARNING,
                        "fuse_loc_fill() failed %"PRIu64": LINK %s %s",
                        state->finh->unique, state->loc2.path, state->loc.path);
                send_fuse_err (state->this, state->finh, ENOENT);
                free_fuse_state (state);
                return;
        }

        state->resolve.op_ret = 0;
        state->resolve2.op_ret = 0;

	if (state->loc.inode) {
		inode_unref (state->loc.inode);
		state->loc.inode = NULL;
	}
	state->loc.inode = inode_ref (state->loc2.inode);

        gf_log ("glusterfs-fuse", GF_LOG_TRACE,
                "%"PRIu64": LINK() %s -> %s",
                state->finh->unique, state->loc2.path,
                state->loc.path);

        FUSE_FOP (state, fuse_newentry_cbk, GF_FOP_LINK,
                  link, &state->loc2, &state->loc, state->xdata);
}


static void
fuse_link (xlator_t *this, fuse_in_header_t *finh, void *msg)
{
        struct fuse_link_in *fli = msg;
        char         *name = (char *)(fli + 1);
        fuse_state_t *state = NULL;

        GET_STATE (this, finh, state);

	fuse_resolve_inode_init (state, &state->resolve2, fli->oldnodeid);

	fuse_resolve_entry_init (state, &state->resolve, finh->nodeid, name);

        fuse_resolve_and_resume (state, fuse_link_resume);

        return;
}


static int
fuse_create_cbk (call_frame_t *frame, void *cookie, xlator_t *this,
                 int32_t op_ret, int32_t op_errno,
                 fd_t *fd, inode_t *inode, struct iatt *buf,
                 struct iatt *preparent, struct iatt *postparent, dict_t *xdata)
{
        fuse_state_t           *state        = NULL;
        fuse_in_header_t       *finh         = NULL;
        fuse_private_t         *priv         = NULL;
        struct fuse_out_header  fouh         = {0, };
        struct fuse_entry_out   feo          = {0, };
        struct fuse_open_out    foo          = {0, };
        struct iovec            iov_out[3];
        inode_t                *linked_inode = NULL;

        state    = frame->root->state;
        priv     = this->private;
        finh     = state->finh;
        foo.open_flags = 0;

        if (op_ret >= 0) {
                foo.fh = (uintptr_t) fd;

                if (((priv->direct_io_mode == 2)
                     && ((state->flags & O_ACCMODE) != O_RDONLY))
                    || (priv->direct_io_mode == 1))
                        foo.open_flags |= FOPEN_DIRECT_IO;

                gf_log ("glusterfs-fuse", GF_LOG_TRACE,
                        "%"PRIu64": %s() %s => %p (ino=%"PRId64")",
                        frame->root->unique, gf_fop_list[frame->root->op],
                        state->loc.path, fd, buf->ia_ino);

                buf->ia_blksize = this->ctx->page_size;
                gf_fuse_stat2attr (buf, &feo.attr);

                linked_inode = inode_link (inode, state->loc.parent,
                                           state->loc.name, buf);

                if (linked_inode != inode) {
                        /*
                           VERY racy code (if used anywhere else)
                           -- don't do this without understanding
                        */
                        inode_unref (fd->inode);
                        fd->inode = inode_ref (linked_inode);
                }

                inode_lookup (linked_inode);

                inode_unref (linked_inode);

                feo.nodeid = inode_to_fuse_nodeid (linked_inode);

                feo.entry_valid = calc_timeout_sec (priv->entry_timeout);
                feo.entry_valid_nsec = calc_timeout_nsec (priv->entry_timeout);
                feo.attr_valid = calc_timeout_sec (priv->attribute_timeout);
                feo.attr_valid_nsec =
                  calc_timeout_nsec (priv->attribute_timeout);

                fouh.error = 0;
                iov_out[0].iov_base = &fouh;
                iov_out[1].iov_base = &feo;
#if FUSE_KERNEL_MINOR_VERSION >= 9
                iov_out[1].iov_len = priv->proto_minor >= 9 ?
                                     sizeof (feo) :
                                     FUSE_COMPAT_ENTRY_OUT_SIZE;
#else
                iov_out[1].iov_len = sizeof (feo);
#endif
                iov_out[2].iov_base = &foo;
                iov_out[2].iov_len = sizeof (foo);
                if (send_fuse_iov (this, finh, iov_out, 3) == ENOENT) {
                        gf_log ("glusterfs-fuse", GF_LOG_DEBUG,
                                "create(%s) got EINTR", state->loc.path);
                        inode_forget (inode, 1);
                        gf_fd_put (priv->fdtable, state->fd_no);
                        goto out;
                }

                fd_bind (fd);
        } else {
                gf_log ("glusterfs-fuse", GF_LOG_WARNING,
                        "%"PRIu64": %s => -1 (%s)", finh->unique,
                        state->loc.path, strerror (op_errno));
                send_fuse_err (this, finh, op_errno);
                gf_fd_put (priv->fdtable, state->fd_no);
        }
out:
        free_fuse_state (state);
        STACK_DESTROY (frame->root);

        return 0;
}


void
fuse_create_resume (fuse_state_t *state)
{
        fd_t           *fd   = NULL;
        fuse_private_t *priv = NULL;

        if (!state->loc.parent) {
                gf_log ("glusterfs-fuse", GF_LOG_WARNING,
                        "%"PRIu64" CREATE %s/%s resolution failed",
                        state->finh->unique, uuid_utoa (state->resolve.gfid),
			state->resolve.bname);
                send_fuse_err (state->this, state->finh, ENOENT);
                free_fuse_state (state);
                return;
        }

        if (state->resolve.op_errno == ENOENT) {
                state->resolve.op_ret = 0;
                state->resolve.op_errno = 0;
        }

        if (state->loc.inode) {
                gf_log (state->this->name, GF_LOG_DEBUG,
                        "inode already present");
                inode_unref (state->loc.inode);
        }

        state->loc.inode = inode_new (state->loc.parent->table);

        fd = fd_create (state->loc.inode, state->finh->pid);

        priv = state->this->private;

        state->fd_no = gf_fd_unused_get (priv->fdtable, fd);

        state->fd = fd_ref (fd);
        fd->flags = state->flags;

        gf_log ("glusterfs-fuse", GF_LOG_TRACE,
                "%"PRIu64": CREATE %s", state->finh->unique,
                state->loc.path);

        FUSE_FOP (state, fuse_create_cbk, GF_FOP_CREATE,
                  create, &state->loc, state->flags, state->mode,
                  state->umask, fd, state->xdata);

}


static void
fuse_create (xlator_t *this, fuse_in_header_t *finh, void *msg)
{
#if FUSE_KERNEL_MINOR_VERSION >= 12
        struct fuse_create_in *fci = msg;
#else
        struct fuse_open_in *fci = msg;
#endif
        char         *name = (char *)(fci + 1);

        fuse_private_t        *priv = NULL;
        fuse_state_t *state = NULL;
        int32_t       ret = -1;

        priv = this->private;
#if FUSE_KERNEL_MINOR_VERSION >= 12
        if (priv->proto_minor < 12)
                name = (char *)((struct fuse_open_in *)msg + 1);
#endif

        GET_STATE (this, finh, state);

        uuid_generate (state->gfid);

	fuse_resolve_entry_init (state, &state->resolve, finh->nodeid, name);

        state->mode = fci->mode;
        state->flags = fci->flags;

        priv = this->private;
#if FUSE_KERNEL_MINOR_VERSION >=12
        if (priv->proto_minor >= 12)
                state->mode &= ~fci->umask;
        if (priv->proto_minor >= 12 && priv->acl) {
                state->xdata = dict_new ();
                if (!state->xdata) {
                        gf_log ("glusterfs-fuse", GF_LOG_WARNING,
                                "CREATE Failed to allocate a param dictionary");
                        send_fuse_err (this, finh, ENOMEM);
                        free_fuse_state (state);
                        return;
                }
                state->umask = fci->umask;
                ret = dict_set_int16 (state->xdata, "umask", fci->umask);
                if (ret < 0) {
                        gf_log ("glusterfs-fuse", GF_LOG_WARNING,
                                "CREATE Failed adding umask to request");
                        dict_destroy (state->xdata);
                        send_fuse_err (this, finh, ENOMEM);
                        free_fuse_state (state);
                        return;
                }
                ret = dict_set_int16 (state->xdata, "mode", fci->mode);
                if (ret < 0) {
                        gf_log ("glusterfs-fuse", GF_LOG_WARNING,
                                "CREATE Failed adding mode to request");
                        dict_destroy (state->xdata);
                        send_fuse_err (this, finh, ENOMEM);
                        free_fuse_state (state);
                        return;
                }
        }
#endif

        fuse_resolve_and_resume (state, fuse_create_resume);

        return;
}


void
fuse_open_resume (fuse_state_t *state)
{
        fd_t           *fd   = NULL;
        fuse_private_t *priv = NULL;

        if (!state->loc.inode) {
                gf_log ("glusterfs-fuse", GF_LOG_ERROR,
                        "%"PRIu64": OPEN %s resolution failed",
                        state->finh->unique, uuid_utoa (state->resolve.gfid));

                send_fuse_err (state->this, state->finh, ENOENT);
                free_fuse_state (state);
                return;
        }

        fd = fd_create (state->loc.inode, state->finh->pid);
        if (!fd) {
                gf_log ("fuse", GF_LOG_ERROR,
                        "fd is NULL");
                send_fuse_err (state->this, state->finh, ENOENT);
                free_fuse_state (state);
                return;
        }

        priv = state->this->private;

        state->fd_no = gf_fd_unused_get (priv->fdtable, fd);
        state->fd = fd_ref (fd);
        fd->flags = state->flags;

        gf_log ("glusterfs-fuse", GF_LOG_TRACE,
                "%"PRIu64": OPEN %s", state->finh->unique,
                state->loc.path);

        FUSE_FOP (state, fuse_fd_cbk, GF_FOP_OPEN,
                  open, &state->loc, state->flags, fd, state->xdata);
}


static void
fuse_open (xlator_t *this, fuse_in_header_t *finh, void *msg)
{
        struct fuse_open_in *foi = msg;
        fuse_state_t *state = NULL;

        GET_STATE (this, finh, state);

	fuse_resolve_inode_init (state, &state->resolve, finh->nodeid);

        state->flags = foi->flags;

        fuse_resolve_and_resume (state, fuse_open_resume);

        return;
}


static int
fuse_readv_cbk (call_frame_t *frame, void *cookie, xlator_t *this,
                int32_t op_ret, int32_t op_errno,
                struct iovec *vector, int32_t count,
                struct iatt *stbuf, struct iobref *iobref, dict_t *xdata)
{
        fuse_state_t *state = NULL;
        fuse_in_header_t *finh = NULL;
        struct fuse_out_header fouh = {0, };
        struct iovec *iov_out = NULL;

        state = frame->root->state;
        finh = state->finh;

        if (op_ret >= 0) {
                gf_log ("glusterfs-fuse", GF_LOG_TRACE,
                        "%"PRIu64": READ => %d/%"GF_PRI_SIZET",%"PRId64"/%"PRId64,
                        frame->root->unique,
                        op_ret, state->size, state->off, stbuf->ia_size);

                iov_out = GF_CALLOC (count + 1, sizeof (*iov_out),
                                     gf_fuse_mt_iovec);
                if (iov_out) {
                        fouh.error = 0;
                        iov_out[0].iov_base = &fouh;
                        memcpy (iov_out + 1, vector, count * sizeof (*iov_out));
                        send_fuse_iov (this, finh, iov_out, count + 1);
                        GF_FREE (iov_out);
                } else
                        send_fuse_err (this, finh, ENOMEM);
        } else {
                gf_log ("glusterfs-fuse", GF_LOG_WARNING,
                        "%"PRIu64": READ => %d (%s)", frame->root->unique,
                        op_ret, strerror (op_errno));

                send_fuse_err (this, finh, op_errno);
        }

        free_fuse_state (state);
        STACK_DESTROY (frame->root);

        return 0;
}

void
fuse_readv_resume (fuse_state_t *state)
{
        gf_log ("glusterfs-fuse", GF_LOG_TRACE,
                "%"PRIu64": READ (%p, size=%zu, offset=%"PRIu64")",
                state->finh->unique, state->fd, state->size, state->off);

        FUSE_FOP (state, fuse_readv_cbk, GF_FOP_READ, readv, state->fd,
                  state->size, state->off, state->io_flags, state->xdata);
}

static void
fuse_readv (xlator_t *this, fuse_in_header_t *finh, void *msg)
{
        struct fuse_read_in *fri = msg;

        fuse_private_t  *priv = NULL;
        fuse_state_t *state = NULL;
        fd_t         *fd = NULL;

        GET_STATE (this, finh, state);

        fd = FH_TO_FD (fri->fh);
        state->fd = fd;

	fuse_resolve_fd_init (state, &state->resolve, fd);

        /* See comment by similar code in fuse_settatr */
        priv = this->private;
#if FUSE_KERNEL_MINOR_VERSION >= 9
        if (priv->proto_minor >= 9 && fri->read_flags & FUSE_READ_LOCKOWNER)
                state->lk_owner = fri->lock_owner;
#endif

        state->size = fri->size;
        state->off = fri->offset;
        /* lets ignore 'fri->read_flags', but just consider 'fri->flags' */
        state->io_flags = fri->flags;

        fuse_resolve_and_resume (state, fuse_readv_resume);
}


static int
fuse_writev_cbk (call_frame_t *frame, void *cookie, xlator_t *this,
                 int32_t op_ret, int32_t op_errno,
                 struct iatt *stbuf, struct iatt *postbuf, dict_t *xdata)
{
        fuse_state_t *state = NULL;
        fuse_in_header_t *finh = NULL;
        struct fuse_write_out fwo = {0, };

        state = frame->root->state;
        finh = state->finh;

        if (op_ret >= 0) {
                gf_log ("glusterfs-fuse", GF_LOG_TRACE,
                        "%"PRIu64": WRITE => %d/%"GF_PRI_SIZET",%"PRId64"/%"PRId64,
                        frame->root->unique,
                        op_ret, state->size, state->off, stbuf->ia_size);

                fwo.size = op_ret;
                send_fuse_obj (this, finh, &fwo);
        } else {
                gf_log ("glusterfs-fuse", GF_LOG_WARNING,
                        "%"PRIu64": WRITE => -1 (%s)", frame->root->unique,
                        strerror (op_errno));

                send_fuse_err (this, finh, op_errno);
        }

        free_fuse_state (state);
        STACK_DESTROY (frame->root);

        return 0;
}

void
fuse_write_resume (fuse_state_t *state)
{
        struct iobref *iobref = NULL;
        struct iobuf  *iobuf = NULL;


        iobref = iobref_new ();
        if (!iobref) {
                gf_log ("glusterfs-fuse", GF_LOG_ERROR,
                        "%"PRIu64": WRITE iobref allocation failed",
                        state->finh->unique);
                send_fuse_err (state->this, state->finh, ENOMEM);

                free_fuse_state (state);
                return;
        }

        iobuf = ((fuse_private_t *) (state->this->private))->iobuf;
        iobref_add (iobref, iobuf);

        gf_log ("glusterfs-fuse", GF_LOG_TRACE,
                "%"PRIu64": WRITE (%p, size=%"PRId64", offset=%"PRId64")",
                state->finh->unique, state->fd, state->size, state->off);

        FUSE_FOP (state, fuse_writev_cbk, GF_FOP_WRITE, writev, state->fd,
                  &state->vector, 1, state->off, state->io_flags, iobref,
                  state->xdata);

        iobref_unref (iobref);
}

static void
fuse_write (xlator_t *this, fuse_in_header_t *finh, void *msg)
{
        /* WRITE is special, metadata is attached to in_header,
         * and msg is the payload as-is.
         */
        struct fuse_write_in *fwi = (struct fuse_write_in *)
                                      (finh + 1);

        fuse_private_t  *priv = NULL;
        fuse_state_t    *state = NULL;
        fd_t            *fd = NULL;

        priv = this->private;

        GET_STATE (this, finh, state);
        fd          = FH_TO_FD (fwi->fh);
        state->fd   = fd;
        state->size = fwi->size;
        state->off  = fwi->offset;

        /* lets ignore 'fwi->write_flags', but just consider 'fwi->flags' */
        state->io_flags = fwi->flags;
        /* TODO: may need to handle below flag
           (fwi->write_flags & FUSE_WRITE_CACHE);
        */


	fuse_resolve_fd_init (state, &state->resolve, fd);

        /* See comment by similar code in fuse_settatr */
        priv = this->private;
#if FUSE_KERNEL_MINOR_VERSION >= 9
        if (priv->proto_minor >= 9 && fwi->write_flags & FUSE_WRITE_LOCKOWNER)
                state->lk_owner = fwi->lock_owner;
#endif

        state->vector.iov_base = msg;
        state->vector.iov_len  = fwi->size;

        fuse_resolve_and_resume (state, fuse_write_resume);

        return;
}


void
fuse_flush_resume (fuse_state_t *state)
{
        FUSE_FOP (state, fuse_err_cbk, GF_FOP_FLUSH,
                  flush, state->fd, state->xdata);
}


static void
fuse_flush (xlator_t *this, fuse_in_header_t *finh, void *msg)
{
        struct fuse_flush_in *ffi = msg;

        fuse_state_t *state = NULL;
        fd_t         *fd = NULL;

        GET_STATE (this, finh, state);
        fd = FH_TO_FD (ffi->fh);
        state->fd = fd;

	fuse_resolve_fd_init (state, &state->resolve, fd);

        state->lk_owner = ffi->lock_owner;

        gf_log ("glusterfs-fuse", GF_LOG_TRACE,
                "%"PRIu64": FLUSH %p", finh->unique, fd);

        fuse_resolve_and_resume (state, fuse_flush_resume);

        return;
}


static void
fuse_release (xlator_t *this, fuse_in_header_t *finh, void *msg)
{
        struct fuse_release_in *fri    = msg;
        fd_t                   *new_fd = NULL;
        fd_t                   *fd     = NULL;
        uint64_t                val    = 0;
        int                     ret    = 0;
        fuse_state_t           *state  = NULL;
        fuse_fd_ctx_t          *fdctx  = NULL;
        fuse_private_t         *priv   = NULL;

        GET_STATE (this, finh, state);
        fd = FH_TO_FD (fri->fh);
        state->fd = fd;

        priv = this->private;

        gf_log ("glusterfs-fuse", GF_LOG_TRACE,
                "%"PRIu64": RELEASE %p", finh->unique, state->fd);

        ret = fd_ctx_del (fd, this, &val);
        if (!ret) {
                fdctx = (fuse_fd_ctx_t *)(unsigned long)val;
                if (fdctx) {
                        new_fd = fdctx->fd;
                        if (new_fd) {
                                fd_unref (new_fd);
                        }

                        GF_FREE (fdctx);
                }
        }
        fd_unref (fd);

        state->fd = NULL;

        gf_fdptr_put (priv->fdtable, fd);

        send_fuse_err (this, finh, 0);

        free_fuse_state (state);
        return;
}


void
fuse_fsync_resume (fuse_state_t *state)
{
        gf_log ("glusterfs-fuse", GF_LOG_TRACE,
                "%"PRIu64": FSYNC %p", state->finh->unique,
		state->fd);

        /* fsync_flags: 1 means "datasync" (no defines for this) */
        FUSE_FOP (state, fuse_fsync_cbk, GF_FOP_FSYNC,
                  fsync, state->fd, (state->flags & 1), state->xdata);
}


static void
fuse_fsync (xlator_t *this, fuse_in_header_t *finh, void *msg)
{
        struct fuse_fsync_in *fsi = msg;

        fuse_state_t *state = NULL;
        fd_t         *fd = NULL;

        GET_STATE (this, finh, state);
        fd = FH_TO_FD (fsi->fh);
        state->fd = fd;

	fuse_resolve_fd_init (state, &state->resolve, fd);

        state->flags = fsi->fsync_flags;
        fuse_resolve_and_resume (state, fuse_fsync_resume);
        return;
}


void
fuse_opendir_resume (fuse_state_t *state)
{
        fd_t           *fd   = NULL;
        fuse_private_t *priv = NULL;

        priv = state->this->private;

        if (!state->loc.inode) {
                gf_log ("glusterfs-fuse", GF_LOG_WARNING,
                        "%"PRIu64": OPENDIR (%s) resolution failed",
                        state->finh->unique, uuid_utoa (state->resolve.gfid));
                send_fuse_err (state->this, state->finh, ENOENT);
                free_fuse_state (state);
                return;
        }

        fd = fd_create (state->loc.inode, state->finh->pid);
        state->fd = fd_ref (fd);
        state->fd_no = gf_fd_unused_get (priv->fdtable, fd);

        gf_log ("glusterfs-fuse", GF_LOG_TRACE,
                "%"PRIu64": OPENDIR %s", state->finh->unique,
                state->loc.path);

        FUSE_FOP (state, fuse_fd_cbk, GF_FOP_OPENDIR,
                  opendir, &state->loc, fd, state->xdata);
}


static void
fuse_opendir (xlator_t *this, fuse_in_header_t *finh, void *msg)
{
        /*
        struct fuse_open_in *foi = msg;
         */

        fuse_state_t *state = NULL;

        GET_STATE (this, finh, state);

        fuse_resolve_inode_init (state, &state->resolve, finh->nodeid);

        fuse_resolve_and_resume (state, fuse_opendir_resume);
}


unsigned char
d_type_from_stat (struct iatt *buf)
{
        unsigned char d_type;

        if (IA_ISLNK (buf->ia_type)) {
                d_type = DT_LNK;

        } else if (IA_ISDIR (buf->ia_type)) {
                d_type = DT_DIR;

        } else if (IA_ISFIFO (buf->ia_type)) {
                d_type = DT_FIFO;

        } else if (IA_ISSOCK (buf->ia_type)) {
                d_type = DT_SOCK;

        } else if (IA_ISCHR (buf->ia_type)) {
                d_type = DT_CHR;

        } else if (IA_ISBLK (buf->ia_type)) {
                d_type = DT_BLK;

        } else if (IA_ISREG (buf->ia_type)) {
                d_type = DT_REG;

        } else {
                d_type = DT_UNKNOWN;
        }

        return d_type;
}


static int
fuse_readdir_cbk (call_frame_t *frame, void *cookie, xlator_t *this,
                  int32_t op_ret, int32_t op_errno, gf_dirent_t *entries,
                  dict_t *xdata)
{
        fuse_state_t *state = NULL;
        fuse_in_header_t *finh = NULL;
        int           size = 0;
        char         *buf = NULL;
        gf_dirent_t  *entry = NULL;
        struct fuse_dirent *fde = NULL;

        state = frame->root->state;
        finh  = state->finh;

        if (op_ret < 0) {
                gf_log ("glusterfs-fuse", GF_LOG_WARNING,
                        "%"PRIu64": READDIR => -1 (%s)", frame->root->unique,
                        strerror (op_errno));

                send_fuse_err (this, finh, op_errno);
                goto out;
        }

        gf_log ("glusterfs-fuse", GF_LOG_TRACE,
                "%"PRIu64": READDIR => %d/%"GF_PRI_SIZET",%"PRId64,
                frame->root->unique, op_ret, state->size, state->off);

        list_for_each_entry (entry, &entries->list, list) {
                size += FUSE_DIRENT_ALIGN (FUSE_NAME_OFFSET +
                                           strlen (entry->d_name));
        }

        buf = GF_CALLOC (1, size, gf_fuse_mt_char);
        if (!buf) {
                gf_log ("glusterfs-fuse", GF_LOG_DEBUG,
                        "%"PRIu64": READDIR => -1 (%s)", frame->root->unique,
                        strerror (ENOMEM));
                send_fuse_err (this, finh, ENOMEM);
                goto out;
        }

        size = 0;
        list_for_each_entry (entry, &entries->list, list) {
                fde = (struct fuse_dirent *)(buf + size);
                fde->ino = entry->d_ino;
                fde->off = entry->d_off;
                fde->type = entry->d_type;
                fde->namelen = strlen (entry->d_name);
                strncpy (fde->name, entry->d_name, fde->namelen);
                size += FUSE_DIRENT_SIZE (fde);
        }

        send_fuse_data (this, finh, buf, size);

        /* TODO: */
        /* gf_link_inodes_from_dirent (this, state->fd->inode, entries); */

out:
        free_fuse_state (state);
        STACK_DESTROY (frame->root);
        if (buf)
                GF_FREE (buf);
        return 0;

}


void
fuse_readdir_resume (fuse_state_t *state)
{
        gf_log ("glusterfs-fuse", GF_LOG_TRACE,
                "%"PRIu64": READDIR (%p, size=%zu, offset=%"PRId64")",
                state->finh->unique, state->fd, state->size, state->off);

        FUSE_FOP (state, fuse_readdir_cbk, GF_FOP_READDIR,
                  readdir, state->fd, state->size, state->off, state->xdata);
}


static void
fuse_readdir (xlator_t *this, fuse_in_header_t *finh, void *msg)
{
        struct fuse_read_in *fri = msg;

        fuse_state_t *state = NULL;
        fd_t         *fd = NULL;

        GET_STATE (this, finh, state);
        state->size = fri->size;
        state->off = fri->offset;
        fd = FH_TO_FD (fri->fh);
        state->fd = fd;

	fuse_resolve_fd_init (state, &state->resolve, fd);

        fuse_resolve_and_resume (state, fuse_readdir_resume);
}


static void
fuse_releasedir (xlator_t *this, fuse_in_header_t *finh, void *msg)
{
        struct fuse_release_in *fri    = msg;
        fd_t                   *new_fd = NULL;
        uint64_t                val    = 0;
        int                     ret    = 0;
        fuse_state_t           *state  = NULL;
        fuse_fd_ctx_t          *fdctx  = NULL;
        fuse_private_t         *priv   = NULL;

        GET_STATE (this, finh, state);
        state->fd = FH_TO_FD (fri->fh);

        priv = this->private;

        gf_log ("glusterfs-fuse", GF_LOG_TRACE,
                "%"PRIu64": RELEASEDIR %p", finh->unique, state->fd);

        ret = fd_ctx_del (state->fd, this, &val);

        if (!ret) {
                fdctx = (fuse_fd_ctx_t *)(unsigned long)val;
                if (fdctx) {
                        new_fd = fdctx->fd;
                        if (new_fd) {
                                fd_unref (new_fd);
                        }

                        GF_FREE (fdctx);
                }
        }

        fd_unref (state->fd);

        gf_fdptr_put (priv->fdtable, state->fd);

        state->fd = NULL;

        send_fuse_err (this, finh, 0);

        free_fuse_state (state);

        return;
}

void
fuse_fsyncdir_resume (fuse_state_t *state)
{
        FUSE_FOP (state, fuse_err_cbk, GF_FOP_FSYNCDIR,
                  fsyncdir, state->fd, (state->flags & 1), state->xdata);

}

static void
fuse_fsyncdir (xlator_t *this, fuse_in_header_t *finh, void *msg)
{
        struct fuse_fsync_in *fsi = msg;

        fuse_state_t *state = NULL;
        fd_t         *fd = NULL;

        fd = FH_TO_FD (fsi->fh);

        GET_STATE (this, finh, state);
        state->fd = fd;

	fuse_resolve_fd_init (state, &state->resolve, fd);

        state->flags = fsi->fsync_flags;
        fuse_resolve_and_resume (state, fuse_fsyncdir_resume);

        return;
}


static int
fuse_statfs_cbk (call_frame_t *frame, void *cookie, xlator_t *this,
                 int32_t op_ret, int32_t op_errno, struct statvfs *buf,
                 dict_t *xdata)
{
        fuse_state_t *state = NULL;
        fuse_in_header_t *finh = NULL;
        fuse_private_t   *priv = NULL;
        struct fuse_statfs_out fso = {{0, }, };

        state = frame->root->state;
        priv  = this->private;
        finh  = state->finh;

        if (op_ret == 0) {
#ifndef GF_DARWIN_HOST_OS
                /* MacFUSE doesn't respect anyof these tweaks */
                buf->f_blocks *= buf->f_frsize;
                buf->f_blocks /= this->ctx->page_size;

                buf->f_bavail *= buf->f_frsize;
                buf->f_bavail /= this->ctx->page_size;

                buf->f_bfree *= buf->f_frsize;
                buf->f_bfree /= this->ctx->page_size;

                buf->f_frsize = buf->f_bsize =this->ctx->page_size;
#endif /* GF_DARWIN_HOST_OS */
                fso.st.bsize   = buf->f_bsize;
                fso.st.frsize  = buf->f_frsize;
                fso.st.blocks  = buf->f_blocks;
                fso.st.bfree   = buf->f_bfree;
                fso.st.bavail  = buf->f_bavail;
                fso.st.files   = buf->f_files;
                fso.st.ffree   = buf->f_ffree;
                fso.st.namelen = buf->f_namemax;

                priv->proto_minor >= 4 ?
                send_fuse_obj (this, finh, &fso) :
                send_fuse_data (this, finh, &fso, FUSE_COMPAT_STATFS_SIZE);
        } else {
                gf_log ("glusterfs-fuse", GF_LOG_WARNING,
                        "%"PRIu64": ERR => -1 (%s)", frame->root->unique,
                        strerror (op_errno));
                send_fuse_err (this, finh, op_errno);
        }

        free_fuse_state (state);
        STACK_DESTROY (frame->root);

        return 0;
}


void
fuse_statfs_resume (fuse_state_t *state)
{
        if (!state->loc.inode) {
                gf_log ("glusterfs-fuse", GF_LOG_WARNING,
                        "%"PRIu64": STATFS (%s) resolution fail",
                        state->finh->unique, uuid_utoa (state->resolve.gfid));

                send_fuse_err (state->this, state->finh, ENOENT);
                free_fuse_state (state);
                return;
        }

        gf_log ("glusterfs-fuse", GF_LOG_TRACE,
                "%"PRIu64": STATFS", state->finh->unique);

        FUSE_FOP (state, fuse_statfs_cbk, GF_FOP_STATFS,
                  statfs, &state->loc, state->xdata);
}


static void
fuse_statfs (xlator_t *this, fuse_in_header_t *finh, void *msg)
{
        fuse_state_t *state = NULL;

        GET_STATE (this, finh, state);

	fuse_resolve_inode_init (state, &state->resolve, finh->nodeid);

	fuse_resolve_and_resume (state, fuse_statfs_resume);
}


void
fuse_setxattr_resume (fuse_state_t *state)
{
        if (!state->loc.inode) {
                gf_log ("glusterfs-fuse", GF_LOG_WARNING,
                        "%"PRIu64": SETXATTR %s/%"PRIu64" (%s) "
                        "resolution failed",
                        state->finh->unique, uuid_utoa (state->resolve.gfid),
			state->finh->nodeid, state->name);
                send_fuse_err (state->this, state->finh, ENOENT);
                free_fuse_state (state);
                return;
        }

#ifdef GF_TEST_FFOP
        state->fd = fd_lookup (state->loc.inode, state->finh->pid);
#endif /* GF_TEST_FFOP */

        if (state->fd) {
                gf_log ("glusterfs-fuse", GF_LOG_TRACE,
                        "%"PRIu64": SETXATTR %p/%"PRIu64" (%s)", state->finh->unique,
                        state->fd, state->finh->nodeid, state->name);

                FUSE_FOP (state, fuse_setxattr_cbk, GF_FOP_FSETXATTR,
                          fsetxattr, state->fd, state->xattr, state->flags,
                          state->xdata);
        } else {
                gf_log ("glusterfs-fuse", GF_LOG_TRACE,
                        "%"PRIu64": SETXATTR %s/%"PRIu64" (%s)", state->finh->unique,
                        state->loc.path, state->finh->nodeid, state->name);

                FUSE_FOP (state, fuse_setxattr_cbk, GF_FOP_SETXATTR,
                          setxattr, &state->loc, state->xattr, state->flags,
                          state->xdata);
        }
}


static void
fuse_setxattr (xlator_t *this, fuse_in_header_t *finh, void *msg)
{
        struct fuse_setxattr_in *fsi = msg;
        char         *name = (char *)(fsi + 1);
        char         *value = name + strlen (name) + 1;
        struct fuse_private *priv = NULL;

        fuse_state_t *state = NULL;
        char         *dict_value = NULL;
        int32_t       ret = -1;
        char *newkey = NULL;

        priv = this->private;

#ifdef GF_DARWIN_HOST_OS
        if (fsi->position) {
                gf_log ("glusterfs-fuse", GF_LOG_WARNING,
                        "%"PRIu64": SETXATTR %s/%"PRIu64" (%s):"
                        "refusing positioned setxattr",
                        finh->unique, state->loc.path, finh->nodeid, name);
                send_fuse_err (this, finh, EINVAL);
                FREE (finh);
                return;
        }
#endif

        if (!priv->acl) {
                if ((strcmp (name, "system.posix_acl_access") == 0) ||
                    (strcmp (name, "system.posix_acl_default") == 0)) {
                        send_fuse_err (this, finh, EOPNOTSUPP);
                        GF_FREE (finh);
                        return;
                }
        }

#ifdef DISABLE_SELINUX
        if (!strncmp (name, "security.", 9)) {
                send_fuse_err (this, finh, EOPNOTSUPP);
                GF_FREE (finh);
                return;
        }
#endif

        /* Check if the command is for changing the log
           level of process or specific xlator */
        ret = is_gf_log_command (this, name, value);
        if (ret >= 0) {
                send_fuse_err (this, finh, ret);
                GF_FREE (finh);
                return;
        }

        if (!strcmp ("inode-invalidate", name)) {
                gf_log ("fuse", GF_LOG_TRACE,
                        "got request to invalidate %"PRIu64, finh->nodeid);
                send_fuse_err (this, finh, 0);
                fuse_invalidate (this, finh->nodeid);
                GF_FREE (finh);
                return;
        }

        if (!strcmp (GFID_XATTR_KEY, name)) {
                send_fuse_err (this, finh, EPERM);
                GF_FREE (finh);
                return;
        }

        GET_STATE (this, finh, state);
        state->size = fsi->size;

	fuse_resolve_inode_init (state, &state->resolve, finh->nodeid);

        state->xattr = get_new_dict ();
        if (!state->xattr) {
                gf_log ("glusterfs-fuse", GF_LOG_ERROR,
                        "%"PRIu64": SETXATTR dict allocation failed",
                        finh->unique);

                send_fuse_err (this, finh, ENOMEM);
                free_fuse_state (state);
                return;
        }

        ret = fuse_flip_xattr_ns (priv, name, &newkey);
        if (ret) {
                send_fuse_err (this, finh, ENOMEM);
                free_fuse_state (state);
                return;
        }

        if (fsi->size > 0) {
                dict_value = memdup (value, fsi->size);
        } else {
                gf_log (THIS->name, GF_LOG_ERROR, "value size zero");
                dict_value = NULL;
        }
        dict_set (state->xattr, newkey,
                  data_from_dynptr ((void *)dict_value, fsi->size));
        dict_ref (state->xattr);

        state->flags = fsi->flags;
        state->name = newkey;

        fuse_resolve_and_resume (state, fuse_setxattr_resume);

        return;
}


static void
send_fuse_xattr (xlator_t *this, fuse_in_header_t *finh, const char *value,
                 size_t size, size_t expected)
{
        struct fuse_getxattr_out fgxo;

        /* linux kernel limits the size of xattr value to 64k */
        if (size > GLUSTERFS_XATTR_LEN_MAX)
                send_fuse_err (this, finh, E2BIG);
        else if (expected) {
                /* if callback for getxattr and asks for value */
                if (size > expected)
                        /* reply would be bigger than
                         * what was asked by kernel */
                        send_fuse_err (this, finh, ERANGE);
                else
                        send_fuse_data (this, finh, (void *)value, size);
        } else {
                fgxo.size = size;
                send_fuse_obj (this, finh, &fgxo);
        }
}


static int
fuse_xattr_cbk (call_frame_t *frame, void *cookie, xlator_t *this,
                int32_t op_ret, int32_t op_errno, dict_t *dict, dict_t *xdata)
{
        int             need_to_free_dict = 0;
        char           *value = "";
        fuse_state_t   *state = NULL;
        fuse_in_header_t *finh = NULL;
        data_t         *value_data = NULL;
        int             ret = -1;
        int32_t         len = 0;
        data_pair_t    *trav = NULL;

        state = frame->root->state;
        finh  = state->finh;

        if (op_ret >= 0) {
                gf_log ("glusterfs-fuse", GF_LOG_TRACE,
                        "%"PRIu64": %s() %s => %d", frame->root->unique,
                        gf_fop_list[frame->root->op], state->loc.path, op_ret);

                /* if successful */
                if (state->name) {
                        /* if callback for getxattr */
                        value_data = dict_get (dict, state->name);
                        if (value_data) {

                                ret = value_data->len; /* Don't return the value for '\0' */
                                value = value_data->data;

                                send_fuse_xattr (this, finh, value, ret, state->size);
                                /* if(ret >...)...else if...else */
                        } else {
                                send_fuse_err (this, finh, ENODATA);
                        } /* if(value_data)...else */
                } else {
                        /* if callback for listxattr */
                        trav = dict->members_list;
                        while (trav) {
                                len += strlen (trav->key) + 1;
                                trav = trav->next;
                        } /* while(trav) */
                        value = alloca (len + 1);
                        if (!value)
                                goto out;
                        len = 0;
                        trav = dict->members_list;
                        while (trav) {
                                strcpy (value + len, trav->key);
                                value[len + strlen (trav->key)] = '\0';
                                len += strlen (trav->key) + 1;
                                trav = trav->next;
                        } /* while(trav) */
                        send_fuse_xattr (this, finh, value, len, state->size);
                } /* if(state->name)...else */
        } else {
                /* if failure - no need to check if listxattr or getxattr */
                if (op_errno != ENODATA) {
                        if (op_errno == ENOTSUP) {
                                GF_LOG_OCCASIONALLY (gf_fuse_xattr_enotsup_log,
                                                     "glusterfs-fuse",
                                                     GF_LOG_ERROR,
                                                     "extended attribute not "
                                                     "supported by the backend "
                                                     "storage");
                        } else {
                                gf_log ("glusterfs-fuse", GF_LOG_WARNING,
                                        "%"PRIu64": %s(%s) %s => -1 (%s)",
                                        frame->root->unique,
                                        gf_fop_list[frame->root->op], state->name,
                                        state->loc.path, strerror (op_errno));
                        }
                } else {
                        gf_log ("glusterfs-fuse", GF_LOG_DEBUG,
                                "%"PRIu64": %s(%s) %s => -1 (%s)",
                                frame->root->unique,
                                gf_fop_list[frame->root->op], state->name,
                                state->loc.path, strerror (op_errno));
                } /* if(op_errno!= ENODATA)...else */

                send_fuse_err (this, finh, op_errno);
        } /* if(op_ret>=0)...else */

out:
        if (need_to_free_dict)
                dict_unref (dict);

        free_fuse_state (state);
        STACK_DESTROY (frame->root);

        return 0;
}


void
fuse_getxattr_resume (fuse_state_t *state)
{
        if (!state->loc.inode) {
                gf_log ("glusterfs-fuse", GF_LOG_WARNING,
                        "%"PRIu64": GETXATTR %s/%"PRIu64" (%s) "
			"resolution failed",
                        state->finh->unique,
			uuid_utoa (state->resolve.gfid),
			state->finh->nodeid, state->name);

                send_fuse_err (state->this, state->finh, ENOENT);
                free_fuse_state (state);
                return;
        }

#ifdef GF_TEST_FFOP
        state->fd = fd_lookup (state->loc.inode, state->finh->pid);
#endif /* GF_TEST_FFOP */

        if (state->fd) {
                gf_log ("glusterfs-fuse", GF_LOG_TRACE,
                        "%"PRIu64": GETXATTR %p/%"PRIu64" (%s)", state->finh->unique,
                        state->fd, state->finh->nodeid, state->name);

                FUSE_FOP (state, fuse_xattr_cbk, GF_FOP_FGETXATTR,
                          fgetxattr, state->fd, state->name, state->xdata);
        } else {
                gf_log ("glusterfs-fuse", GF_LOG_TRACE,
                        "%"PRIu64": GETXATTR %s/%"PRIu64" (%s)", state->finh->unique,
                        state->loc.path, state->finh->nodeid, state->name);

                FUSE_FOP (state, fuse_xattr_cbk, GF_FOP_GETXATTR,
                          getxattr, &state->loc, state->name, state->xdata);
        }
}


static void
fuse_getxattr (xlator_t *this, fuse_in_header_t *finh, void *msg)
{
        struct fuse_getxattr_in *fgxi = msg;
        char         *name = (char *)(fgxi + 1);

        fuse_state_t *state = NULL;
        struct fuse_private *priv = NULL;
        int rv = 0;
        char *newkey = NULL;

        priv = this->private;

#ifdef GF_DARWIN_HOST_OS
        if (fgxi->position) {
                /* position can be used only for
                 * resource fork queries which we
                 * don't support anyway... so handling
                 * it separately is just sort of a
                 * matter of aesthetics, not strictly
                 * necessary.
                 */

                gf_log ("glusterfs-fuse", GF_LOG_WARNING,
                        "%"PRIu64": GETXATTR %s/%"PRIu64" (%s):"
                        "refusing positioned getxattr",
                        finh->unique, state->loc.path, finh->nodeid, name);
                send_fuse_err (this, finh, EINVAL);
                FREE (finh);
                return;
        }
#endif

        if (!priv->acl) {
                if ((strcmp (name, "system.posix_acl_access") == 0) ||
                    (strcmp (name, "system.posix_acl_default") == 0)) {
                        send_fuse_err (this, finh, ENOTSUP);
                        GF_FREE (finh);
                        return;
                }
        }

#ifdef DISABLE_SELINUX
        if (!strncmp (name, "security.", 9)) {
                send_fuse_err (this, finh, ENODATA);
                GF_FREE (finh);
                return;
        }
#endif

        GET_STATE (this, finh, state);

	fuse_resolve_inode_init (state, &state->resolve, finh->nodeid);

        rv = fuse_flip_xattr_ns (priv, name, &newkey);
        if (rv) {
                send_fuse_err (this, finh, ENOMEM);
                free_fuse_state (state);
                goto out;
        }

        state->size = fgxi->size;
        state->name = newkey;

        fuse_resolve_and_resume (state, fuse_getxattr_resume);
 out:
        return;
}


void
fuse_listxattr_resume (fuse_state_t *state)
{
        if (!state->loc.inode) {
                gf_log ("glusterfs-fuse", GF_LOG_WARNING,
                        "%"PRIu64": LISTXATTR %s/%"PRIu64
                        "resolution failed", state->finh->unique,
			uuid_utoa (state->resolve.gfid), state->finh->nodeid);

                send_fuse_err (state->this, state->finh, ENOENT);
                free_fuse_state (state);
                return;
        }

#ifdef GF_TEST_FFOP
        state->fd = fd_lookup (state->loc.inode, state->finh->pid);
#endif /* GF_TEST_FFOP */

        if (state->fd) {
                gf_log ("glusterfs-fuse", GF_LOG_TRACE,
                        "%"PRIu64": LISTXATTR %p/%"PRIu64, state->finh->unique,
                        state->fd, state->finh->nodeid);

                FUSE_FOP (state, fuse_xattr_cbk, GF_FOP_FGETXATTR,
                          fgetxattr, state->fd, NULL, state->xdata);
        } else {
                gf_log ("glusterfs-fuse", GF_LOG_TRACE,
                        "%"PRIu64": LISTXATTR %s/%"PRIu64, state->finh->unique,
                        state->loc.path, state->finh->nodeid);

                FUSE_FOP (state, fuse_xattr_cbk, GF_FOP_GETXATTR,
                          getxattr, &state->loc, NULL, state->xdata);
        }
}


static void
fuse_listxattr (xlator_t *this, fuse_in_header_t *finh, void *msg)
{
        struct fuse_getxattr_in *fgxi = msg;
        fuse_state_t *state = NULL;

        GET_STATE (this, finh, state);

	fuse_resolve_inode_init (state, &state->resolve, finh->nodeid);

        state->size = fgxi->size;

        fuse_resolve_and_resume (state, fuse_listxattr_resume);

        return;
}


void
fuse_removexattr_resume (fuse_state_t *state)
{
         if (!state->loc.inode) {
                gf_log ("glusterfs-fuse", GF_LOG_DEBUG,
                        "%"PRIu64": REMOVEXATTR %s/%"PRIu64" (%s) "
			"resolution failed",
                        state->finh->unique, uuid_utoa (state->resolve.gfid),
			state->finh->nodeid, state->name);

                send_fuse_err (state->this, state->finh, ENOENT);
                free_fuse_state (state);
                return;
        }

#ifdef GF_TEST_FFOP
        state->fd = fd_lookup (state->loc.inode, state->finh->pid);
#endif /* GF_TEST_FFOP */

        if (state->fd) {
                gf_log ("glusterfs-fuse", GF_LOG_TRACE,
                        "%"PRIu64": REMOVEXATTR %p/%"PRIu64" (%s)", state->finh->unique,
                        state->fd, state->finh->nodeid, state->name);

                FUSE_FOP (state, fuse_err_cbk, GF_FOP_FREMOVEXATTR,
                          fremovexattr, state->fd, state->name, state->xdata);
        } else {
                gf_log ("glusterfs-fuse", GF_LOG_TRACE,
                        "%"PRIu64": REMOVEXATTR %s/%"PRIu64" (%s)", state->finh->unique,
                        state->loc.path, state->finh->nodeid, state->name);

                FUSE_FOP (state, fuse_err_cbk, GF_FOP_REMOVEXATTR,
                          removexattr, &state->loc, state->name, state->xdata);
        }
}


static void
fuse_removexattr (xlator_t *this, fuse_in_header_t *finh, void *msg)
{
        char *name = msg;

        fuse_state_t *state = NULL;
        fuse_private_t *priv = NULL;
        int32_t       ret = -1;
        char *newkey = NULL;

        if (!strcmp (GFID_XATTR_KEY, name)) {
                send_fuse_err (this, finh, EPERM);
                GF_FREE (finh);
                return;
        }

        priv = this->private;

        GET_STATE (this, finh, state);

	fuse_resolve_inode_init (state, &state->resolve, finh->nodeid);

        ret = fuse_flip_xattr_ns (priv, name, &newkey);
        if (ret) {
                send_fuse_err (this, finh, ENOMEM);
                free_fuse_state (state);
                return;
        }

        state->name = newkey;

        fuse_resolve_and_resume (state, fuse_removexattr_resume);
        return;
}


static int gf_fuse_lk_enosys_log;

static int
fuse_getlk_cbk (call_frame_t *frame, void *cookie, xlator_t *this,
                int32_t op_ret, int32_t op_errno, struct gf_flock *lock,
                dict_t *xdata)
{
        fuse_state_t *state = NULL;

        state = frame->root->state;
        struct fuse_lk_out flo = {{0, }, };

        if (op_ret == 0) {
                gf_log ("glusterfs-fuse", GF_LOG_TRACE,
                        "%"PRIu64": ERR => 0", frame->root->unique);
                flo.lk.type = lock->l_type;
                flo.lk.pid  = lock->l_pid;
                if (lock->l_type == F_UNLCK)
                        flo.lk.start = flo.lk.end = 0;
                else {
                        flo.lk.start = lock->l_start;
                        flo.lk.end = lock->l_len ?
                                     (lock->l_start + lock->l_len - 1) :
                                     OFFSET_MAX;
                }
                send_fuse_obj (this, state->finh, &flo);
        } else {
                if (op_errno == ENOSYS) {
                        gf_fuse_lk_enosys_log++;
                        if (!(gf_fuse_lk_enosys_log % GF_UNIVERSAL_ANSWER)) {
                                gf_log ("glusterfs-fuse", GF_LOG_ERROR,
                                        "GETLK not supported. loading "
                                        "'features/posix-locks' on server side "
                                        "will add GETLK support.");
                        }
                } else {
                        gf_log ("glusterfs-fuse", GF_LOG_WARNING,
                                "%"PRIu64": ERR => -1 (%s)",
                                frame->root->unique, strerror (op_errno));
                }
                send_fuse_err (this, state->finh, op_errno);
        }

        free_fuse_state (state);
        STACK_DESTROY (frame->root);

        return 0;
}


void
fuse_getlk_resume (fuse_state_t *state)
{
        gf_log ("glusterfs-fuse", GF_LOG_TRACE,
                "%"PRIu64": GETLK %p", state->finh->unique, state->fd);

        FUSE_FOP (state, fuse_getlk_cbk, GF_FOP_LK,
                  lk, state->fd, F_GETLK, &state->lk_lock, state->xdata);
}


static void
fuse_getlk (xlator_t *this, fuse_in_header_t *finh, void *msg)
{
        struct fuse_lk_in *fli = msg;

        fuse_state_t *state = NULL;
        fd_t         *fd = NULL;

        fd = FH_TO_FD (fli->fh);
        GET_STATE (this, finh, state);
        state->fd = fd;

	fuse_resolve_fd_init (state, &state->resolve, fd);

        convert_fuse_file_lock (&fli->lk, &state->lk_lock,
                                fli->owner);

        state->lk_owner = fli->owner;

        fuse_resolve_and_resume (state, fuse_getlk_resume);

        return;
}


static int
fuse_setlk_cbk (call_frame_t *frame, void *cookie, xlator_t *this,
                int32_t op_ret, int32_t op_errno, struct gf_flock *lock,
                dict_t *xdata)
{
        uint32_t      op    = 0;
        fuse_state_t *state = NULL;

        state = frame->root->state;
        op    = state->finh->opcode;

        if (op_ret == 0) {
                gf_log ("glusterfs-fuse", GF_LOG_TRACE,
                        "%"PRIu64": ERR => 0", frame->root->unique);
                fd_lk_insert_and_merge (state->fd,
                                        (op == FUSE_SETLK) ? F_SETLK : F_SETLKW,
                                        &state->lk_lock);

                send_fuse_err (this, state->finh, 0);
        } else {
                if (op_errno == ENOSYS) {
                        gf_fuse_lk_enosys_log++;
                        if (!(gf_fuse_lk_enosys_log % GF_UNIVERSAL_ANSWER)) {
                                gf_log ("glusterfs-fuse", GF_LOG_ERROR,
                                        "SETLK not supported. loading "
                                        "'features/posix-locks' on server side "
                                        "will add SETLK support.");
                        }
                } else if (op_errno == EAGAIN) {
                        gf_log ("glusterfs-fuse", GF_LOG_DEBUG,
                                "Returning EAGAIN Flock: "
                                "start=%llu, len=%llu, pid=%llu, lk-owner=%s",
                                (unsigned long long) state->lk_lock.l_start,
                                (unsigned long long) state->lk_lock.l_len,
                                (unsigned long long) state->lk_lock.l_pid,
                                lkowner_utoa (&frame->root->lk_owner));
                } else  {
                        gf_log ("glusterfs-fuse", GF_LOG_WARNING,
                                "%"PRIu64": ERR => -1 (%s)",
                                frame->root->unique, strerror (op_errno));
                }

                send_fuse_err (this, state->finh, op_errno);
        }

        free_fuse_state (state);
        STACK_DESTROY (frame->root);

        return 0;
}


void
fuse_setlk_resume (fuse_state_t *state)
{
        gf_log ("glusterfs-fuse", GF_LOG_TRACE,
                "%"PRIu64": SETLK%s %p", state->finh->unique,
                state->finh->opcode == FUSE_SETLK ? "" : "W", state->fd);

        FUSE_FOP (state, fuse_setlk_cbk, GF_FOP_LK, lk, state->fd,
                  state->finh->opcode == FUSE_SETLK ? F_SETLK : F_SETLKW,
                  &state->lk_lock, state->xdata);
}


static void
fuse_setlk (xlator_t *this, fuse_in_header_t *finh, void *msg)
{
        struct fuse_lk_in *fli = msg;

        fuse_state_t *state = NULL;
        fd_t         *fd = NULL;

        fd = FH_TO_FD (fli->fh);
        GET_STATE (this, finh, state);
        state->finh = finh;
        state->fd = fd;

	fuse_resolve_fd_init (state, &state->resolve, fd);

        convert_fuse_file_lock (&fli->lk, &state->lk_lock,
                                fli->owner);

        state->lk_owner = fli->owner;

        fuse_resolve_and_resume (state, fuse_setlk_resume);

        return;
}


static void *
notify_kernel_loop (void *data)
{
        xlator_t               *this = NULL;
        fuse_private_t         *priv = NULL;
        struct fuse_out_header *fouh = NULL;
        int                     rv   = 0;

        char inval_buf[INVAL_BUF_SIZE] = {0,};

        this = data;
        priv = this->private;

        for (;;) {
                rv = read (priv->revchan_in, inval_buf, sizeof (*fouh));
                if (rv != sizeof (*fouh))
                        break;
                fouh = (struct fuse_out_header *)inval_buf;
                rv = read (priv->revchan_in, inval_buf + sizeof (*fouh),
                           fouh->len - sizeof (*fouh));
                if (rv != fouh->len - sizeof (*fouh))
                        break;
                rv = write (priv->fd, inval_buf, fouh->len);
                if (rv != fouh->len && !(rv == -1 && errno == ENOENT))
                        break;
        }

        close (priv->revchan_in);
        close (priv->revchan_out);

        gf_log ("glusterfs-fuse", GF_LOG_INFO,
                "kernel notifier loop terminated");

        return NULL;
}


static void
fuse_init (xlator_t *this, fuse_in_header_t *finh, void *msg)
{
        struct fuse_init_in  *fini      = msg;
        struct fuse_init_out  fino      = {0,};
        fuse_private_t       *priv      = NULL;
        int                   ret       = 0;
        int                   pfd[2]    = {0,};
        pthread_t             messenger;

        priv = this->private;

        if (priv->init_recvd) {
                gf_log ("glusterfs-fuse", GF_LOG_ERROR,
                        "got INIT after first message");

                close (priv->fd);
                goto out;
        }

        priv->init_recvd = 1;

        if (fini->major != FUSE_KERNEL_VERSION) {
                gf_log ("glusterfs-fuse", GF_LOG_ERROR,
                        "unsupported FUSE protocol version %d.%d",
                        fini->major, fini->minor);

                close (priv->fd);
                goto out;
        }
        priv->proto_minor = fini->minor;

        fino.major = FUSE_KERNEL_VERSION;
        fino.minor = FUSE_KERNEL_MINOR_VERSION;
        fino.max_readahead = 1 << 17;
        fino.max_write = 1 << 17;
        fino.flags = FUSE_ASYNC_READ | FUSE_POSIX_LOCKS;
#if FUSE_KERNEL_MINOR_VERSION >= 12
        if (fini->minor >= 12) {
            /* let fuse leave the umask processing to us, so that it does not
             * break extended POSIX ACL defaults on server */
            fino.flags |= FUSE_DONT_MASK;
        }
#endif
#if FUSE_KERNEL_MINOR_VERSION >= 9
        if (fini->minor >= 6 /* fuse_init_in has flags */ &&
            fini->flags & FUSE_BIG_WRITES) {
                /* no need for direct I/O mode by default if big writes are supported */
                if (priv->direct_io_mode == 2)
                        priv->direct_io_mode = 0;
                fino.flags |= FUSE_BIG_WRITES;
        }

        /* Used for 'reverse invalidation of inode' */
        if (fini->minor >= 12) {
                if (pipe(pfd) == -1) {
                        gf_log ("glusterfs-fuse", GF_LOG_ERROR,
                                "cannot create pipe pair (%s)",
                                strerror(errno));

                        close (priv->fd);
                        goto out;
                }
                priv->revchan_in  = pfd[0];
                priv->revchan_out = pfd[1];
                ret = pthread_create (&messenger, NULL, notify_kernel_loop,
                                      this);
                if (ret != 0) {
                        gf_log ("glusterfs-fuse", GF_LOG_ERROR,
                                "failed to start messenger daemon (%s)",
                                strerror(errno));

                        close (priv->fd);
                        goto out;
                }
                priv->reverse_fuse_thread_started = _gf_true;
        }
        if (fini->minor >= 13) {
                /* these values seemed to work fine during testing */
                fino.max_background = 64;
                fino.congestion_threshold = 48;
        }
        if (fini->minor < 9)
                *priv->msg0_len_p = sizeof(*finh) + FUSE_COMPAT_WRITE_IN_SIZE;
#endif
        ret = send_fuse_obj (this, finh, &fino);
        if (ret == 0)
                gf_log ("glusterfs-fuse", GF_LOG_INFO,
                        "FUSE inited with protocol versions:"
                        " glusterfs %d.%d kernel %d.%d",
                        FUSE_KERNEL_VERSION, FUSE_KERNEL_MINOR_VERSION,
                        fini->major, fini->minor);
        else {
                gf_log ("glusterfs-fuse", GF_LOG_ERROR,
                        "FUSE init failed (%s)", strerror (ret));

                close (priv->fd);
        }

 out:
        GF_FREE (finh);
}


static void
fuse_enosys (xlator_t *this, fuse_in_header_t *finh, void *msg)
{
        send_fuse_err (this, finh, ENOSYS);

        GF_FREE (finh);
}


static void
fuse_destroy (xlator_t *this, fuse_in_header_t *finh, void *msg)
{
        send_fuse_err (this, finh, 0);

        GF_FREE (finh);
}



struct fuse_first_lookup {
        pthread_mutex_t  mutex;
        pthread_cond_t   cond;
        char             fin;
};

int
fuse_first_lookup_cbk (call_frame_t *frame, void *cookie, xlator_t *this,
                       int32_t op_ret, int32_t op_errno,
                       inode_t *inode, struct iatt *buf, dict_t *xattr,
                       struct iatt *postparent)
{
        struct fuse_first_lookup *stub = NULL;

        stub = frame->local;

        if (op_ret == 0) {
                gf_log (this->name, GF_LOG_TRACE,
                        "first lookup on root succeeded.");
        } else {
                gf_log (this->name, GF_LOG_DEBUG,
                        "first lookup on root failed.");
        }

        pthread_mutex_lock (&stub->mutex);
        {
                stub->fin = 1;
                pthread_cond_broadcast (&stub->cond);
        }
        pthread_mutex_unlock (&stub->mutex);

        return 0;
}


int
fuse_first_lookup (xlator_t *this)
{
        fuse_private_t            *priv = NULL;
        loc_t                      loc = {0, };
        call_frame_t              *frame = NULL;
        xlator_t                  *xl = NULL;
        dict_t                    *dict = NULL;
        struct fuse_first_lookup   stub;
        uuid_t                     gfid;
        int                        ret;

        priv = this->private;

        loc.path = "/";
        loc.name = "";
        loc.inode = fuse_ino_to_inode (1, this);
        uuid_copy (loc.gfid, loc.inode->gfid);
        loc.parent = NULL;

        dict = dict_new ();
        frame = create_frame (this, this->ctx->pool);
        frame->root->type = GF_OP_TYPE_FOP;

        xl = priv->active_subvol;

        pthread_mutex_init (&stub.mutex, NULL);
        pthread_cond_init (&stub.cond, NULL);
        stub.fin = 0;

        frame->local = &stub;

        memset (gfid, 0, 16);
        gfid[15] = 1;
        ret = dict_set_static_bin (dict, "gfid-req", gfid, 16);
        if (ret)
                gf_log (xl->name, GF_LOG_ERROR, "failed to set 'gfid-req'");

        STACK_WIND (frame, fuse_first_lookup_cbk, xl, xl->fops->lookup,
                    &loc, dict);
        dict_unref (dict);

        pthread_mutex_lock (&stub.mutex);
        {
                while (!stub.fin) {
                        pthread_cond_wait (&stub.cond, &stub.mutex);
                }
        }
        pthread_mutex_unlock (&stub.mutex);

        pthread_mutex_destroy (&stub.mutex);
        pthread_cond_destroy (&stub.cond);

        frame->local = NULL;
        STACK_DESTROY (frame->root);

        return 0;
}


int
fuse_nameless_lookup (xlator_t *xl, uuid_t gfid, loc_t *loc)
{
        int          ret          = -1;
        dict_t      *xattr_req    = NULL;
        struct iatt  iatt         = {0, };
        inode_t     *linked_inode = NULL;

        if ((loc == NULL) || (xl == NULL)) {
                goto out;
        }

        if (loc->inode == NULL) {
                loc->inode = inode_new (xl->itable);
                if (loc->inode == NULL) {
                        goto out;
                }
        }

        uuid_copy (loc->gfid, gfid);

        xattr_req = dict_new ();
        if (xattr_req == NULL) {
                goto out;
        }

        ret = syncop_lookup (xl, loc, xattr_req, &iatt, NULL, NULL);
        if (ret < 0) {
                goto out;
        }

        linked_inode = inode_link (loc->inode, NULL, NULL, &iatt);
        inode_unref (loc->inode);
        loc->inode = linked_inode;

        ret = 0;
out:
        if (xattr_req != NULL) {
                dict_unref (xattr_req);
        }

        return ret;
}


int
fuse_migrate_fd (xlator_t *this, fd_t *fd, xlator_t *old_subvol,
                 xlator_t *new_subvol)
{
        int            ret                = -1;
        loc_t          loc                = {0, };
        char           create_in_progress = 0;
        inode_t       *old_inode          = NULL;
        int            flags              = 0;

        /* could've used pthread_cond_wait, but that requires a cond variable to
         * be mainted for each fd and that is a bit too much overhead.
         */
        do {
                LOCK (&fd->inode->lock);
                {
                        if (uuid_is_null (fd->inode->gfid)) {
                                create_in_progress = 1;
                        } else {
                                create_in_progress = 0;
                        }
                }
                UNLOCK (&fd->inode->lock);

                if (create_in_progress) {
                        gf_log ("glusterfs-fuse", GF_LOG_INFO,
                                "create call on fd (%p) is in progress, "
                                "hence waiting", fd);
                        sleep (1);
                }

        } while (create_in_progress);

        if (fd->inode->table->xl == old_subvol) {
                ret = syncop_fsync (old_subvol, fd);
                if (ret < 0) {
                        gf_log ("glusterfs-fuse", GF_LOG_WARNING,
                                "syncop_fsync failed (%s)", strerror (errno));
                }
        } else {
                gf_log ("glusterfs-fuse", GF_LOG_DEBUG, "fd (%p) was not "
                        "migrated during previous graph switch", fd);
        }

        loc.path = "";
        loc.name = NULL;

        loc.inode = inode_find (new_subvol->itable, fd->inode->gfid);

        if (loc.inode == NULL) {
                ret = fuse_nameless_lookup (new_subvol, fd->inode->gfid, &loc);
                if (ret < 0) {
                        gf_log ("glusterfs-fuse", GF_LOG_WARNING,
                                "name-less lookup of gfid (%s) failed (%s)",
                                uuid_utoa (fd->inode->gfid), strerror (errno));
                        goto out;
                }

        }

        old_inode = fd->inode;

        inode_ref (loc.inode);

        LOCK (&fd->inode->lock);
        {
                list_del_init (&fd->inode_list);
        }
        UNLOCK (&fd->inode->lock);

        LOCK (&fd->lock);
        {
                fd->inode = loc.inode;
        }
        UNLOCK (&fd->lock);

        if (IA_ISDIR (fd->inode->ia_type)) {
                ret = syncop_opendir (new_subvol, &loc, fd);
        } else {
                flags = fd->flags & ~(O_CREAT | O_EXCL);
                ret = syncop_open (new_subvol, &loc, flags, fd);
        }

        if (ret < 0) {
                gf_log ("glusterfs-fuse", GF_LOG_WARNING,
                        "open on gfid (%s) failed (%s)",
                        uuid_utoa (fd->inode->gfid), strerror (errno));
                goto out;
        }

        fd_bind (fd);

        ret = 0;
out:
        if (loc.inode != NULL) {
                inode_unref (loc.inode);
        }

        if (old_inode != NULL) {
                inode_unref (old_inode);
        }

        return ret;
}


int
fuse_handle_opened_fds (xlator_t *this, xlator_t *old_subvol,
                        xlator_t *new_subvol)
{
        fuse_private_t *priv      = NULL;
        fdentry_t      *fdentries = NULL;
        uint32_t        count     = 0;
        fdtable_t      *fdtable   = NULL;
        int             i         = 0;
        fd_t           *fd        = NULL;
        int32_t         ret       = 0;
        fuse_fd_ctx_t  *fdctx     = NULL;

        priv = this->private;

        fdtable = priv->fdtable;

        fdentries = gf_fd_fdtable_copy_all_fds (fdtable, &count);
        if (fdentries != NULL) {
                for (i = 0; i < count; i++) {
                        fd = fdentries[i].fd;
                        if (fd == NULL)
                                continue;

                        ret = fuse_migrate_fd (this, fd, old_subvol,
                                               new_subvol);

                        fdctx = fuse_fd_ctx_check_n_create (this, fd);
                        if (fdctx) {
                                if (ret < 0) {
                                        fdctx->migration_failed = 1;
                                } else {
                                        fdctx->migration_failed = 0;
                                }
                        }
                }

                for (i = 0; i < count ; i++) {
                        fd = fdentries[i].fd;
                        if (fd)
                                fd_unref (fd);
                }

                GF_FREE (fdentries);
        }

        return 0;
}


static int
fuse_handle_blocked_locks (xlator_t *this, xlator_t *old_subvol,
                           xlator_t *new_subvol)
{
        return 0;
}


static int
fuse_graph_switch_task (void *data)
{
        fuse_graph_switch_args_t *args = NULL;

        args = data;
        if (args == NULL) {
                goto out;
        }

        /* don't change the order of handling open fds and blocked locks, since
         * the act of opening files also reacquires granted locks in new graph.
         */
        fuse_handle_opened_fds (args->this, args->old_subvol, args->new_subvol);

        fuse_handle_blocked_locks (args->this, args->old_subvol,
                                   args->new_subvol);

out:
        return 0;
}


fuse_graph_switch_args_t *
fuse_graph_switch_args_alloc (void)
{
        fuse_graph_switch_args_t *args = NULL;

        args = GF_CALLOC (1, sizeof (*args), gf_fuse_mt_graph_switch_args_t);
        if (args == NULL) {
                goto out;
        }

out:
        return args;
}


void
fuse_graph_switch_args_destroy (fuse_graph_switch_args_t *args)
{
        if (args == NULL) {
                goto out;
        }

        GF_FREE (args);
out:
        return;
}


int
fuse_handle_graph_switch (xlator_t *this, xlator_t *old_subvol,
                          xlator_t *new_subvol)
{
        call_frame_t             *frame = NULL;
        int32_t                   ret   = -1;
        fuse_graph_switch_args_t *args  = NULL;

        frame = create_frame (this, this->ctx->pool);
        if (frame == NULL) {
                goto out;
        }

        args = fuse_graph_switch_args_alloc ();
        if (args == NULL) {
                goto out;
        }

        args->this = this;
        args->old_subvol = old_subvol;
        args->new_subvol = new_subvol;

        ret = synctask_new (this->ctx->env, fuse_graph_switch_task, NULL, frame,
                            args);
        if (ret == -1) {
                gf_log (this->name, GF_LOG_WARNING, "starting sync-task to "
                        "handle graph switch failed");
                goto out;
        }

        ret = 0;
out:
        if (args != NULL) {
                fuse_graph_switch_args_destroy (args);
        }

        if (frame != NULL) {
                STACK_DESTROY (frame->root);
        }

        return ret;
}


int
fuse_graph_sync (xlator_t *this)
{
        fuse_private_t *priv                = NULL;
        int             need_first_lookup   = 0;
        int             ret                 = 0;
        xlator_t       *old_subvol          = NULL, *new_subvol = NULL;
        uint64_t        winds_on_old_subvol = 0;

        priv = this->private;

        pthread_mutex_lock (&priv->sync_mutex);
        {
                if (!priv->next_graph)
                        goto unlock;

                old_subvol = priv->active_subvol;
                new_subvol = priv->active_subvol = priv->next_graph->top;
                priv->next_graph = NULL;
                need_first_lookup = 1;

                while (!priv->event_recvd) {
                        ret = pthread_cond_wait (&priv->sync_cond,
                                                 &priv->sync_mutex);
                        if (ret != 0) {
			          gf_log (this->name, GF_LOG_DEBUG,
					  "timedwait returned non zero value "
					  "ret: %d errno: %d", ret, errno);
				  break;
			}
                }
        }
unlock:
        pthread_mutex_unlock (&priv->sync_mutex);

        if (need_first_lookup) {
                fuse_first_lookup (this);
        }

        if ((old_subvol != NULL) && (new_subvol != NULL)) {
                fuse_handle_graph_switch (this, old_subvol, new_subvol);

                pthread_mutex_lock (&priv->sync_mutex);
                {
                        old_subvol->switched = 1;
                        winds_on_old_subvol = old_subvol->winds;
                }
                pthread_mutex_unlock (&priv->sync_mutex);

                if (winds_on_old_subvol == 0) {
                        xlator_notify (old_subvol, GF_EVENT_PARENT_DOWN,
                                       old_subvol, NULL);
                }
        }

        return 0;
}

int
fuse_get_mount_status (xlator_t *this)
{
        int             kid_status = -1;
        pid_t           kid_pid = -1;
        fuse_private_t *priv = this->private;
        int             our_status = -1;

        if (read(priv->status_pipe[0],&kid_status, sizeof(kid_status)) < 0) {
                gf_log (this->name, GF_LOG_ERROR, "could not get mount status");
                goto out;
        }
        gf_log (this->name, GF_LOG_DEBUG, "mount status is %d", kid_status);

        if (read(priv->status_pipe[0],&kid_pid, sizeof(kid_pid)) < 0) {
                gf_log (this->name, GF_LOG_ERROR, "could not get mount PID");
                goto out;
        }
        gf_log (this->name, GF_LOG_DEBUG, "mount PID is %d", kid_pid);

        (void)waitpid(kid_pid,NULL,0);
        our_status = kid_status;

out:
        close(priv->status_pipe[0]);
        close(priv->status_pipe[1]);
        return our_status;
}

static void *
fuse_thread_proc (void *data)
{
        char                     *mount_point = NULL;
        xlator_t                 *this = NULL;
        fuse_private_t           *priv = NULL;
        ssize_t                   res = 0;
        struct iobuf             *iobuf = NULL;
        fuse_in_header_t         *finh;
        struct iovec              iov_in[2];
        void                     *msg = NULL;
        const size_t              msg0_size = sizeof (*finh) + 128;
        fuse_handler_t          **fuse_ops = NULL;
        struct pollfd             pfd[2] = {{0,}};
        gf_boolean_t              mount_finished = _gf_false;

        this = data;
        priv = this->private;
        fuse_ops = priv->fuse_ops;

        THIS = this;

        iov_in[0].iov_len = sizeof (*finh) + sizeof (struct fuse_write_in);
        iov_in[1].iov_len = ((struct iobuf_pool *)this->ctx->iobuf_pool)
                              ->default_page_size;
        priv->msg0_len_p = &iov_in[0].iov_len;

        for (;;) {
                /* THIS has to be reset here */
                THIS = this;

                if (!mount_finished) {
                        memset(pfd,0,sizeof(pfd));
                        pfd[0].fd = priv->status_pipe[0];
                        pfd[0].events = POLLIN | POLLHUP | POLLERR;
                        pfd[1].fd = priv->fd;
                        pfd[1].events = POLLIN | POLLHUP | POLLERR;
                        if (poll(pfd,2,-1) < 0) {
                                gf_log (this->name, GF_LOG_ERROR,
                                        "poll error %s", strerror(errno));
                                break;
                        }
                        if (pfd[0].revents & POLLIN) {
                                if (fuse_get_mount_status(this) != 0) {
                                        break;
                                }
                                mount_finished = _gf_true;
                        }
                        else if (pfd[0].revents) {
                                gf_log (this->name, GF_LOG_ERROR,
                                        "mount pipe closed without status");
                                break;
                        }
                        if (!pfd[1].revents) {
                                continue;
                        }
                }

                /*
                 * We don't want to block on readv while we're still waiting
                 * for mount status.  That means we only want to get here if
                 * mount_status is true (meaning that our wait completed
                 * already) or if we already called poll(2) on priv->fd to
                 * make sure it's ready.
                 */

                if (priv->init_recvd)
                        fuse_graph_sync (this);

                /* TODO: This place should always get maximum supported buffer
                   size from 'fuse', which is as of today 128KB. If we bring in
                   support for higher block sizes support, then we should be
                   changing this one too */
                iobuf = iobuf_get (this->ctx->iobuf_pool);

                /* Add extra 128 byte to the first iov so that it can
                 * accommodate "ordinary" non-write requests. It's not
                 * guaranteed to be big enough, as SETXATTR and namespace
                 * operations with very long names may grow behind it,
                 * but it's good enough in most cases (and we can handle
                 * rest via realloc).
                 */
                iov_in[0].iov_base = GF_CALLOC (1, msg0_size,
                                                gf_fuse_mt_iov_base);

                if (!iobuf || !iov_in[0].iov_base) {
                        gf_log (this->name, GF_LOG_ERROR,
                                "Out of memory");
                        if (iobuf)
                                iobuf_unref (iobuf);
                        GF_FREE (iov_in[0].iov_base);
                        sleep (10);
                        continue;
                }

                iov_in[1].iov_base = iobuf->ptr;

                res = readv (priv->fd, iov_in, 2);

                if (res == -1) {
                        if (errno == ENODEV || errno == EBADF) {
                                gf_log ("glusterfs-fuse", GF_LOG_DEBUG,
                                        "terminating upon getting %s when "
                                        "reading /dev/fuse",
                                        errno == ENODEV ? "ENODEV" : "EBADF");

                                break;
                        }
                        if (errno != EINTR) {
                                gf_log ("glusterfs-fuse", GF_LOG_WARNING,
                                        "read from /dev/fuse returned -1 (%s)",
                                        strerror (errno));
                        }

                        goto cont_err;
                }
                if (res < sizeof (finh)) {
                        gf_log ("glusterfs-fuse", GF_LOG_WARNING,
                                "short read on /dev/fuse");
                        break;
                }

                finh = (fuse_in_header_t *)iov_in[0].iov_base;

                if (res != finh->len
#ifdef GF_DARWIN_HOST_OS
                    /* work around fuse4bsd/MacFUSE msg size miscalculation bug,
                     * that is, payload size is not taken into account for
                     * buffered writes
                     */
                    && !(finh->opcode == FUSE_WRITE &&
                         finh->len == sizeof(*finh) + sizeof(struct fuse_write_in) &&
                         res == finh->len + ((struct fuse_write_in *)(finh + 1))->size)
#endif
                   ) {
                        gf_log ("glusterfs-fuse", GF_LOG_WARNING,
                                "inconsistent read on /dev/fuse");
                        break;
                }

                priv->iobuf = iobuf;

                if (finh->opcode == FUSE_WRITE)
                        msg = iov_in[1].iov_base;
                else {
                        if (res > msg0_size) {
                                iov_in[0].iov_base =
                                  GF_REALLOC (iov_in[0].iov_base, res);
                                if (iov_in[0].iov_base)
                                        finh = (fuse_in_header_t *)
                                                 iov_in[0].iov_base;
                                else {
                                        gf_log ("glusterfs-fuse", GF_LOG_ERROR,
                                                "Out of memory");
                                        send_fuse_err (this, finh, ENOMEM);

                                        goto cont_err;
                                }
                        }

                        if (res > iov_in[0].iov_len)
                                memcpy (iov_in[0].iov_base + iov_in[0].iov_len,
                                        iov_in[1].iov_base,
                                        res - iov_in[0].iov_len);

                        msg = finh + 1;
                }
                if (priv->uid_map_root &&
                    finh->uid == priv->uid_map_root)
                        finh->uid = 0;

#ifdef GF_DARWIN_HOST_OS
                if (finh->opcode >= FUSE_OP_HIGH)
                        /* turn down MacFUSE specific messages */
                        fuse_enosys (this, finh, msg);
                else
#endif
                fuse_ops[finh->opcode] (this, finh, msg);

                iobuf_unref (iobuf);
                continue;

 cont_err:
                iobuf_unref (iobuf);
                GF_FREE (iov_in[0].iov_base);
        }

        /*
         * We could be in all sorts of states with respect to iobuf and iov_in
         * by the time we get here, and it's just not worth untangling them if
         * we're about to kill ourselves anyway.
         */

        if (dict_get (this->options, ZR_MOUNTPOINT_OPT))
                mount_point = data_to_str (dict_get (this->options,
                                                     ZR_MOUNTPOINT_OPT));
        if (mount_point) {
                gf_log (this->name, GF_LOG_INFO,
                        "unmounting %s", mount_point);
        }

        /* Kill the whole process, not just this thread. */
        kill (getpid(), SIGTERM);
        return NULL;
}


int32_t
fuse_itable_dump (xlator_t  *this)
{
        if (!this)
                 return -1;

        gf_proc_dump_add_section("xlator.mount.fuse.itable");
        inode_table_dump(this->itable, "xlator.mount.fuse.itable");

	return 0;
}

int32_t
fuse_priv_dump (xlator_t  *this)
{
        fuse_private_t  *private = NULL;

        if (!this)
                return -1;

        private = this->private;

        if (!private)
                return -1;

        gf_proc_dump_add_section("xlator.mount.fuse.priv");

        gf_proc_dump_write("fd", "%d", private->fd);
        gf_proc_dump_write("proto_minor", "%u",
                            private->proto_minor);
        gf_proc_dump_write("volfile", "%s",
                            private->volfile?private->volfile:"None");
        gf_proc_dump_write("volfile_size", "%d",
                            private->volfile_size);
        gf_proc_dump_write("mount_point", "%s",
                            private->mount_point);
        gf_proc_dump_write("iobuf", "%u",
                            private->iobuf);
        gf_proc_dump_write("fuse_thread_started", "%d",
                            (int)private->fuse_thread_started);
        gf_proc_dump_write("direct_io_mode", "%d",
                            private->direct_io_mode);
        gf_proc_dump_write("entry_timeout", "%lf",
                            private->entry_timeout);
        gf_proc_dump_write("attribute_timeout", "%lf",
                            private->attribute_timeout);
        gf_proc_dump_write("init_recvd", "%d",
                            (int)private->init_recvd);
        gf_proc_dump_write("strict_volfile_check", "%d",
                            (int)private->strict_volfile_check);
        gf_proc_dump_write("reverse_thread_started", "%d",
                           (int)private->reverse_fuse_thread_started);

        return 0;
}


int
fuse_graph_setup (xlator_t *this, glusterfs_graph_t *graph)
{
        inode_table_t  *itable = NULL;
        int             ret = 0;
        fuse_private_t *priv = NULL;

        priv = this->private;

        /* handle the case of more than one CHILD_UP on same graph */
        if (priv->active_subvol == graph->top)
                return 0; /* This is a valid case */

        if (graph->used)
                return 0;

        graph->used = 1;

        itable = inode_table_new (0, graph->top);
        if (!itable)
                return -1;

        ((xlator_t *)graph->top)->itable = itable;

        pthread_mutex_lock (&priv->sync_mutex);
        {
                priv->next_graph = graph;
                priv->event_recvd = 0;

                pthread_cond_signal (&priv->sync_cond);
        }
        pthread_mutex_unlock (&priv->sync_mutex);

        gf_log ("fuse", GF_LOG_INFO, "switched to graph %d",
                ((graph) ? graph->id : 0));

        return ret;
}


int
notify (xlator_t *this, int32_t event, void *data, ...)
{
        int32_t             ret     = 0;
        fuse_private_t     *private = NULL;
        glusterfs_graph_t  *graph = NULL;

        private = this->private;

        graph = data;

        gf_log ("fuse", GF_LOG_DEBUG, "got event %d on graph %d",
                event, ((graph) ? graph->id : 0));

        switch (event)
        {
        case GF_EVENT_GRAPH_NEW:
                break;

        case GF_EVENT_CHILD_UP:
        case GF_EVENT_CHILD_DOWN:
        case GF_EVENT_CHILD_CONNECTING:
        {
                if (graph) {
                        ret = fuse_graph_setup (this, graph);
                        if (ret)
                                gf_log (this->name, GF_LOG_WARNING,
                                        "failed to setup the graph");
                }

                if ((event == GF_EVENT_CHILD_UP)
                    || (event == GF_EVENT_CHILD_DOWN)) {
                        pthread_mutex_lock (&private->sync_mutex);
                        {
                                private->event_recvd = 1;
                                pthread_cond_broadcast (&private->sync_cond);
                        }
                        pthread_mutex_unlock (&private->sync_mutex);
                }

                if (!private->fuse_thread_started) {
                        private->fuse_thread_started = 1;

                        ret = pthread_create (&private->fuse_thread, NULL,
                                              fuse_thread_proc, this);
                        if (ret != 0) {
                                gf_log (this->name, GF_LOG_DEBUG,
                                        "pthread_create() failed (%s)",
                                        strerror (errno));
                                break;
                        }
                }

                break;
        }

        case GF_EVENT_AUTH_FAILED:
        {
                /* Authentication failure is an error and glusterfs should stop */
               gf_log (this->name, GF_LOG_ERROR, "Server authenication failed. Shutting down.");
               fini (this);
               break;
        }

        default:
                break;
        }

        return ret;
}

int32_t
mem_acct_init (xlator_t *this)
{
        int     ret = -1;

        if (!this)
                return ret;

        ret = xlator_mem_acct_init (this, gf_fuse_mt_end + 1);

        if (ret != 0) {
                gf_log (this->name, GF_LOG_ERROR, "Memory accounting init"
                                "failed");
                return ret;
        }

        return ret;
}


static fuse_handler_t *fuse_std_ops[FUSE_OP_HIGH] = {
        [FUSE_INIT]        = fuse_init,
        [FUSE_DESTROY]     = fuse_destroy,
        [FUSE_LOOKUP]      = fuse_lookup,
        [FUSE_FORGET]      = fuse_forget,
        [FUSE_GETATTR]     = fuse_getattr,
        [FUSE_SETATTR]     = fuse_setattr,
        [FUSE_OPENDIR]     = fuse_opendir,
        [FUSE_READDIR]     = fuse_readdir,
        [FUSE_RELEASEDIR]  = fuse_releasedir,
        [FUSE_ACCESS]      = fuse_access,
        [FUSE_READLINK]    = fuse_readlink,
        [FUSE_MKNOD]       = fuse_mknod,
        [FUSE_MKDIR]       = fuse_mkdir,
        [FUSE_UNLINK]      = fuse_unlink,
        [FUSE_RMDIR]       = fuse_rmdir,
        [FUSE_SYMLINK]     = fuse_symlink,
        [FUSE_RENAME]      = fuse_rename,
        [FUSE_LINK]        = fuse_link,
        [FUSE_CREATE]      = fuse_create,
        [FUSE_OPEN]        = fuse_open,
        [FUSE_READ]        = fuse_readv,
        [FUSE_WRITE]       = fuse_write,
        [FUSE_FLUSH]       = fuse_flush,
        [FUSE_RELEASE]     = fuse_release,
        [FUSE_FSYNC]       = fuse_fsync,
        [FUSE_FSYNCDIR]    = fuse_fsyncdir,
        [FUSE_STATFS]      = fuse_statfs,
        [FUSE_SETXATTR]    = fuse_setxattr,
        [FUSE_GETXATTR]    = fuse_getxattr,
        [FUSE_LISTXATTR]   = fuse_listxattr,
        [FUSE_REMOVEXATTR] = fuse_removexattr,
        [FUSE_GETLK]       = fuse_getlk,
        [FUSE_SETLK]       = fuse_setlk,
        [FUSE_SETLKW]      = fuse_setlk,
};


static fuse_handler_t *fuse_dump_ops[FUSE_OP_HIGH] = {
};


static void
fuse_dumper (xlator_t *this, fuse_in_header_t *finh, void *msg)
{
        fuse_private_t *priv = NULL;
        struct iovec diov[3];
        char r = 'R';
        int ret = 0;

        priv = this->private;

        diov[0].iov_base = &r;
        diov[0].iov_len  = 1;
        diov[1].iov_base = finh;
        diov[1].iov_len  = sizeof (*finh);
        diov[2].iov_base = msg;
        diov[2].iov_len  = finh->len - sizeof (*finh);

        pthread_mutex_lock (&priv->fuse_dump_mutex);
        ret = writev (priv->fuse_dump_fd, diov, 3);
        pthread_mutex_unlock (&priv->fuse_dump_mutex);
        if (ret == -1)
                gf_log ("glusterfs-fuse", GF_LOG_ERROR,
                        "failed to dump fuse message (R): %s",
                        strerror (errno));

        return priv->fuse_ops0[finh->opcode] (this, finh, msg);
}


int
init (xlator_t *this_xl)
{
        int                ret = 0;
        dict_t            *options = NULL;
        char              *value_string = NULL;
        cmd_args_t        *cmd_args = NULL;
        char              *fsname = NULL;
        fuse_private_t    *priv = NULL;
        struct stat        stbuf = {0,};
        int                i = 0;
        int                xl_name_allocated = 0;
        int                fsname_allocated = 0;
        glusterfs_ctx_t   *ctx = NULL;
        gf_boolean_t       sync_mtab = _gf_false;
        char              *mnt_args = NULL;

        if (this_xl == NULL)
                return -1;

        if (this_xl->options == NULL)
                return -1;

        ctx = glusterfs_ctx_get ();
        if (!ctx)
                return -1;

        options = this_xl->options;

        if (this_xl->name == NULL) {
                this_xl->name = gf_strdup ("fuse");
                if (!this_xl->name) {
                        gf_log ("glusterfs-fuse", GF_LOG_ERROR,
                                "Out of memory");

                        goto cleanup_exit;
                }
                xl_name_allocated = 1;
        }

        priv = GF_CALLOC (1, sizeof (*priv), gf_fuse_mt_fuse_private_t);
        if (!priv) {
                gf_log ("glusterfs-fuse", GF_LOG_ERROR,
                        "Out of memory");

                goto cleanup_exit;
        }
        this_xl->private = (void *) priv;
        priv->mount_point = NULL;
        priv->fd = -1;
        priv->revchan_in = -1;
        priv->revchan_out = -1;

        /* get options from option dictionary */
        ret = dict_get_str (options, ZR_MOUNTPOINT_OPT, &value_string);
        if (ret == -1 || value_string == NULL) {
                gf_log ("fuse", GF_LOG_ERROR,
                        "Mandatory option 'mountpoint' is not specified.");
                goto cleanup_exit;
        }

        if (stat (value_string, &stbuf) != 0) {
                if (errno == ENOENT) {
                        gf_log (this_xl->name, GF_LOG_ERROR,
                                "%s %s does not exist",
                                ZR_MOUNTPOINT_OPT, value_string);
                } else if (errno == ENOTCONN) {
                        gf_log (this_xl->name, GF_LOG_ERROR,
                                "Mountpoint %s seems to have a stale "
                                "mount, run 'umount %s' and try again.",
                                value_string, value_string);
                } else {
                        gf_log (this_xl->name, GF_LOG_DEBUG,
                                "%s %s : stat returned %s",
                                ZR_MOUNTPOINT_OPT,
                                value_string, strerror (errno));
                }
                goto cleanup_exit;
        }

        if (S_ISDIR (stbuf.st_mode) == 0) {
                gf_log (this_xl->name, GF_LOG_ERROR,
                        "%s %s is not a directory",
                        ZR_MOUNTPOINT_OPT, value_string);
                goto cleanup_exit;
        }
        priv->mount_point = gf_strdup (value_string);
        if (!priv->mount_point) {
                gf_log ("glusterfs-fuse", GF_LOG_ERROR,
                        "Out of memory");

                goto cleanup_exit;
        }

        ret = dict_get_double (options, "attribute-timeout",
                               &priv->attribute_timeout);
        if (ret != 0)
                priv->attribute_timeout = 1.0; /* default */

        ret = dict_get_double (options, "entry-timeout",
                               &priv->entry_timeout);
        if (ret != 0)
                priv->entry_timeout = 1.0; /* default */

        ret = dict_get_int32 (options, "client-pid",
                              &priv->client_pid);
        if (ret == 0)
                priv->client_pid_set = _gf_true;

        ret = dict_get_uint32 (options, "uid-map-root",
                               &priv->uid_map_root);
        if (ret != 0)
                priv->uid_map_root = 0;

        priv->direct_io_mode = 2;
        ret = dict_get_str (options, ZR_DIRECT_IO_OPT, &value_string);
        if (ret == 0) {
                ret = gf_string2boolean (value_string, &priv->direct_io_mode);
                GF_ASSERT (ret == 0);
        }

        priv->strict_volfile_check = 0;
        ret = dict_get_str (options, ZR_STRICT_VOLFILE_CHECK, &value_string);
        if (ret == 0) {
                ret = gf_string2boolean (value_string,
                                         &priv->strict_volfile_check);
                GF_ASSERT (ret == 0);
        }

        priv->acl = 0;
        ret = dict_get_str (options, "acl", &value_string);
        if (ret == 0) {
                ret = gf_string2boolean (value_string, &priv->acl);
                GF_ASSERT (ret == 0);
        }
        if (priv->uid_map_root)
                priv->acl = 1;

        priv->read_only = 0;
        ret = dict_get_str (options, "read-only", &value_string);
        if (ret == 0) {
                ret = gf_string2boolean (value_string, &priv->read_only);
                GF_ASSERT (ret == 0);
        }

        priv->fuse_dump_fd = -1;
        ret = dict_get_str (options, "dump-fuse", &value_string);
        if (ret == 0) {
                ret = unlink (value_string);
                if (ret != -1 || errno == ENOENT)
                        ret = open (value_string, O_RDWR|O_CREAT|O_EXCL,
                                    S_IRUSR|S_IWUSR);
                if (ret == -1) {
                        gf_log ("glusterfs-fuse", GF_LOG_ERROR,
                                "cannot open fuse dump file %s",
                                 value_string);

                        goto cleanup_exit;
                }
                priv->fuse_dump_fd = ret;
        }

        sync_mtab = _gf_false;
        ret = dict_get_str (options, "sync-mtab", &value_string);
        if (ret == 0) {
                ret = gf_string2boolean (value_string,
                                         &sync_mtab);
                GF_ASSERT (ret == 0);
        }

        cmd_args = &this_xl->ctx->cmd_args;
        fsname = cmd_args->volfile;
        if (!fsname && cmd_args->volfile_server) {
                if (cmd_args->volfile_id) {
                        fsname = GF_MALLOC (
                                   strlen (cmd_args->volfile_server) + 1 +
                                   strlen (cmd_args->volfile_id) + 1,
                                   gf_fuse_mt_fuse_private_t);
                        if (!fsname) {
                                gf_log ("glusterfs-fuse", GF_LOG_ERROR,
                                        "Out of memory");
                                goto cleanup_exit;
                        }
                        fsname_allocated = 1;
                        strcpy (fsname, cmd_args->volfile_server);
                        strcat (fsname, ":");
                        strcat (fsname, cmd_args->volfile_id);
                } else
                        fsname = cmd_args->volfile_server;
        }
        if (!fsname)
                fsname = "glusterfs";

        priv->fdtable = gf_fd_fdtable_alloc ();
        if (priv->fdtable == NULL) {
                gf_log ("glusterfs-fuse", GF_LOG_ERROR, "Out of memory");
                goto cleanup_exit;
        }

        gf_asprintf (&mnt_args, "%s%sallow_other,max_read=131072",
                     priv->read_only ? "ro," : "",
                     priv->acl ? "" : "default_permissions,");
        if (!mnt_args)
                goto cleanup_exit;

        if (pipe(priv->status_pipe) < 0) {
                gf_log (this_xl->name, GF_LOG_ERROR,
                        "could not create pipe to separate mount process");
                goto cleanup_exit;
        }

        priv->fd = gf_fuse_mount (priv->mount_point, fsname, mnt_args,
                                  sync_mtab ? &ctx->mtab_pid : NULL,
                                  priv->status_pipe[1]);
        if (priv->fd == -1)
                goto cleanup_exit;

        pthread_mutex_init (&priv->fuse_dump_mutex, NULL);
        pthread_cond_init (&priv->sync_cond, NULL);
        pthread_mutex_init (&priv->sync_mutex, NULL);
        priv->event_recvd = 0;

        for (i = 0; i < FUSE_OP_HIGH; i++) {
                if (!fuse_std_ops[i])
                        fuse_std_ops[i] = fuse_enosys;
                if (!fuse_dump_ops[i])
                        fuse_dump_ops[i] = fuse_dumper;
        }
        priv->fuse_ops = fuse_std_ops;
        if (priv->fuse_dump_fd != -1) {
                priv->fuse_ops0 = priv->fuse_ops;
                priv->fuse_ops  = fuse_dump_ops;
        }

        if (fsname_allocated)
                GF_FREE (fsname);
        GF_FREE (mnt_args);
        return 0;

cleanup_exit:
        if (xl_name_allocated)
                GF_FREE (this_xl->name);
        if (fsname_allocated)
                GF_FREE (fsname);
        if (priv) {
                GF_FREE (priv->mount_point);
                if (priv->fd != -1)
                        close (priv->fd);
                if (priv->fuse_dump_fd != -1)
                        close (priv->fuse_dump_fd);
                GF_FREE (priv);
        }
        if (mnt_args)
                GF_FREE (mnt_args);
        return -1;
}


void
fini (xlator_t *this_xl)
{
        fuse_private_t *priv = NULL;
        char *mount_point = NULL;

        if (this_xl == NULL)
                return;

        if ((priv = this_xl->private) == NULL)
                return;

        if (dict_get (this_xl->options, ZR_MOUNTPOINT_OPT))
                mount_point = data_to_str (dict_get (this_xl->options,
                                                     ZR_MOUNTPOINT_OPT));
        if (mount_point != NULL) {
                gf_log (this_xl->name, GF_LOG_INFO,
                        "Unmounting '%s'.", mount_point);

                gf_fuse_unmount (mount_point, priv->fd);
                close (priv->fuse_dump_fd);
                dict_del (this_xl->options, ZR_MOUNTPOINT_OPT);
        }
        /* Process should terminate once fuse xlator is finished.
         * Required for AUTH_FAILED event.
         */
        raise (SIGTERM);
}

struct xlator_fops fops = {
};

struct xlator_cbks cbks = {
};


struct xlator_dumpops dumpops = {
        .priv  = fuse_priv_dump,
        .inode = fuse_itable_dump,
};

struct volume_options options[] = {
        { .key  = {"direct-io-mode"},
          .type = GF_OPTION_TYPE_BOOL
        },
        { .key  = {ZR_MOUNTPOINT_OPT, "mount-point"},
          .type = GF_OPTION_TYPE_PATH
        },
        { .key  = {ZR_DUMP_FUSE, "fuse-dumpfile"},
          .type = GF_OPTION_TYPE_PATH
        },
        { .key  = {ZR_ATTR_TIMEOUT_OPT},
          .type = GF_OPTION_TYPE_DOUBLE
        },
        { .key  = {ZR_ENTRY_TIMEOUT_OPT},
          .type = GF_OPTION_TYPE_DOUBLE
        },
        { .key  = {ZR_STRICT_VOLFILE_CHECK},
          .type = GF_OPTION_TYPE_BOOL
        },
        { .key  = {"client-pid"},
          .type = GF_OPTION_TYPE_INT
        },
        { .key  = {"uid-map-root"},
          .type = GF_OPTION_TYPE_INT
        },
        { .key  = {"sync-mtab"},
          .type = GF_OPTION_TYPE_BOOL
        },
        { .key = {"read-only"},
          .type = GF_OPTION_TYPE_BOOL
        },
        { .key = {NULL} },
};
