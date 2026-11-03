/*
 * resmgr.c - the monitor's files under /dev/phila. See resmgr.h.
 *
 * Each open() gets its own snapshot, taken on the first read, so a reader
 * that reads in small pieces still sees one consistent picture. The thread
 * that serves a request runs at the priority of the client that sent it,
 * as every QNX resource manager does by default, so a low-priority display
 * never borrows the alarm path's priority.
 *
 * SPDX-License-Identifier: MIT
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

struct phila_ocb;
#define IOFUNC_OCB_T struct phila_ocb
#include <sys/iofunc.h> /* first, so the resource-manager types use our OCB */
#include <sys/dispatch.h>

#include "resmgr.h"

struct phila_ocb {
    iofunc_ocb_t base; /* must come first */
    char *text;
    size_t len;
};

static const char *const NAMES[PFILE_COUNT] = {"/dev/phila/status", "/dev/phila/alarms", "/dev/phila/latency",
                                               "/dev/phila/threads", "/dev/phila/control"};

static dispatch_t *g_dpp;
static iofunc_attr_t g_attr[PFILE_COUNT];
static resmgr_connect_funcs_t g_connect;
static resmgr_io_funcs_t g_io;
static pfile_render_fn g_render;
static pfile_control_fn g_control;

static IOFUNC_OCB_T *ocb_calloc(resmgr_context_t *ctp, IOFUNC_ATTR_T *attr)
{
    (void)ctp;
    (void)attr;
    return calloc(1, sizeof(struct phila_ocb));
}

static void ocb_free(IOFUNC_OCB_T *ocb)
{
    free(ocb->text);
    free(ocb);
}

static iofunc_funcs_t g_ocb_funcs = {_IOFUNC_NFUNCS, ocb_calloc, ocb_free, NULL, NULL, NULL};
static iofunc_mount_t g_mount = {.funcs = &g_ocb_funcs};

static pfile_t which_file(const IOFUNC_OCB_T *ocb)
{
    for (int i = 0; i < PFILE_COUNT; i++)
        if (ocb->base.attr == &g_attr[i]) return (pfile_t)i;
    return PFILE_COUNT;
}

static int io_read(resmgr_context_t *ctp, io_read_t *msg, IOFUNC_OCB_T *ocb)
{
    int st = iofunc_read_verify(ctp, msg, &ocb->base, NULL);
    if (st != EOK) return st;
    if ((msg->i.xtype & _IO_XTYPE_MASK) != _IO_XTYPE_NONE) return ENOSYS;
    pfile_t f = which_file(ocb);
    if (f == PFILE_COUNT || f == PFILE_CONTROL) return EBADF;
    if (ocb->text == NULL) {
        ocb->text = g_render(f, &ocb->len);
        if (ocb->text == NULL) return ENOMEM;
    }
    size_t off = (size_t)ocb->base.offset;
    size_t n = off < ocb->len ? ocb->len - off : 0;
    if (n > _IO_READ_GET_NBYTES(msg)) n = _IO_READ_GET_NBYTES(msg);
    _IO_SET_READ_NBYTES(ctp, n);
    if (n == 0) return _RESMGR_NPARTS(0);
    SETIOV(ctp->iov, ocb->text + off, n);
    ocb->base.offset += n;
    return _RESMGR_NPARTS(1);
}

static int io_write(resmgr_context_t *ctp, io_write_t *msg, IOFUNC_OCB_T *ocb)
{
    int st = iofunc_write_verify(ctp, msg, &ocb->base, NULL);
    if (st != EOK) return st;
    if ((msg->i.xtype & _IO_XTYPE_MASK) != _IO_XTYPE_NONE) return ENOSYS;
    if (which_file(ocb) != PFILE_CONTROL) return EBADF;
    size_t n = _IO_WRITE_GET_NBYTES(msg);
    char line[256];
    if (n >= sizeof line) return EMSGSIZE;
    if (resmgr_msgget(ctp, line, n, sizeof msg->i) != (ssize_t)n) return EIO;
    line[n] = '\0';
    while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r' || line[n - 1] == ' ')) line[--n] = '\0';
    st = g_control(line);
    if (st != EOK) return st;
    _IO_SET_WRITE_NBYTES(ctp, _IO_WRITE_GET_NBYTES(msg));
    return EOK;
}

int phila_resmgr_attach(pfile_render_fn render, pfile_control_fn control)
{
    g_render = render;
    g_control = control;
    if ((g_dpp = dispatch_create()) == NULL) return -1;
    iofunc_func_init(_RESMGR_CONNECT_NFUNCS, &g_connect, _RESMGR_IO_NFUNCS, &g_io);
    g_io.read = io_read;
    g_io.write = io_write;

    resmgr_attr_t rattr;
    memset(&rattr, 0, sizeof rattr);
    rattr.nparts_max = 1;
    rattr.msg_max_size = 2048;
    for (int i = 0; i < PFILE_COUNT; i++) {
        mode_t mode = i == PFILE_CONTROL ? (S_IFNAM | 0200) : (S_IFNAM | 0444);
        iofunc_attr_init(&g_attr[i], mode, NULL, NULL);
        g_attr[i].mount = &g_mount;
        if (resmgr_attach(g_dpp, &rattr, NAMES[i], _FTYPE_ANY, 0, &g_connect, &g_io, &g_attr[i]) == -1) return -1;
    }
    return 0;
}

void phila_resmgr_run(void)
{
    dispatch_context_t *ctp = dispatch_context_alloc(g_dpp);
    if (ctp == NULL) {
        perror("phila-monitord: dispatch_context_alloc");
        return;
    }
    for (;;) {
        if (dispatch_block(ctp) == NULL) continue;
        dispatch_handler(ctp);
    }
}
