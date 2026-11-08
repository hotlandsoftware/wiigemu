/*
 * embed: - a read-only device over files baked into the DOL (make EMBED=dir,
 * packed by tools/pack_embed.py), so fopen/opendir/stat work on them like
 * on the SD card.  For GameCube runs in Dolphin, which has no GC SD adapter.
 */
#include "gemu/wii.h"
#include "embed_bin.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/iosupport.h>

typedef struct { uint32_t name, data, size; } Entry;
typedef struct { const Entry *e; uint32_t pos; } File;
typedef struct { char dir[128]; uint32_t next; } Dir;

static const uint8_t *blob(void) { return embed_bin; }
static uint32_t count(void)      { return ((const uint32_t *)blob())[1]; }
static const Entry *entry(uint32_t i) { return (const Entry *)(blob() + 8) + i; }
static const char *name_of(const Entry *e) { return (const char *)blob() + e->name; }

/* "embed:/gemu/nes/x.nes" -> "gemu/nes/x.nes" */
static const char *strip(const char *path)
{
    const char *c = strchr(path, ':');
    if (c) path = c + 1;
    while (*path == '/') path++;
    return path;
}

static const Entry *find(const char *path)
{
    path = strip(path);
    for (uint32_t i = 0; i < count(); i++)
        if (!strcmp(name_of(entry(i)), path)) return entry(i);
    return NULL;
}

static void fill_stat(struct stat *st, const Entry *e)
{
    memset(st, 0, sizeof(*st));
    st->st_mode = e ? S_IFREG | 0444 : S_IFDIR | 0555;
    st->st_size = e ? e->size : 0;
    st->st_nlink = 1;
}

static int e_open(struct _reent *r, void *fs, const char *path, int flags, int mode)
{
    (void)mode;
    const Entry *e = find(path);
    if (!e)                         { r->_errno = ENOENT; return -1; }
    if ((flags & O_ACCMODE) != O_RDONLY) { r->_errno = EROFS; return -1; }
    *(File *)fs = (File){ e, 0 };
    return (int)(intptr_t)fs;
}

static int e_close(struct _reent *r, void *fd) { (void)r; (void)fd; return 0; }

static ssize_t e_write(struct _reent *r, void *fd, const char *p, size_t n)
{
    (void)fd; (void)p; (void)n;
    r->_errno = EROFS;
    return -1;
}

static ssize_t e_read(struct _reent *r, void *fd, char *p, size_t n)
{
    (void)r;
    File *f = fd;
    uint32_t left = f->e->size - f->pos;
    if (n > left) n = left;
    memcpy(p, blob() + f->e->data + f->pos, n);
    f->pos += (uint32_t)n;
    return (ssize_t)n;
}

static off_t e_seek(struct _reent *r, void *fd, off_t pos, int dir)
{
    File *f = fd;
    off_t base = dir == SEEK_CUR ? f->pos : dir == SEEK_END ? f->e->size : 0;
    if (base + pos < 0 || base + pos > f->e->size) { r->_errno = EINVAL; return -1; }
    return f->pos = (uint32_t)(base + pos);
}

static int e_fstat(struct _reent *r, void *fd, struct stat *st)
{
    (void)r;
    fill_stat(st, ((File *)fd)->e);
    return 0;
}

/* A directory is any proper prefix of a name, at a '/'. */
static bool is_dir(const char *p)
{
    size_t n = strlen(p);
    if (!n) return true;
    for (uint32_t i = 0; i < count(); i++) {
        const char *s = name_of(entry(i));
        if (!strncmp(s, p, n) && s[n] == '/') return true;
    }
    return false;
}

static int e_stat(struct _reent *r, const char *path, struct stat *st)
{
    const Entry *e = find(path);
    if (!e && !is_dir(strip(path))) { r->_errno = ENOENT; return -1; }
    fill_stat(st, e);
    return 0;
}

static DIR_ITER *e_diropen(struct _reent *r, DIR_ITER *it, const char *path)
{
    Dir *d = it->dirStruct;
    const char *p = strip(path);
    size_t n = strlen(p);
    while (n && p[n - 1] == '/') n--;
    if (n >= sizeof(d->dir) - 1) { r->_errno = ENAMETOOLONG; return NULL; }
    memcpy(d->dir, p, n);
    d->dir[n] = '\0';
    if (!is_dir(d->dir)) { r->_errno = ENOENT; return NULL; }
    if (n) { d->dir[n] = '/'; d->dir[n + 1] = '\0'; }
    d->next = 0;
    return it;
}

static int e_dirreset(struct _reent *r, DIR_ITER *it)
{
    (void)r;
    ((Dir *)it->dirStruct)->next = 0;
    return 0;
}

/* Files directly in the directory (the browser only lists leaf folders). */
static int e_dirnext(struct _reent *r, DIR_ITER *it, char *name, struct stat *st)
{
    Dir *d = it->dirStruct;
    size_t n = strlen(d->dir);
    while (d->next < count()) {
        const Entry *e = entry(d->next++);
        const char *s = name_of(e);
        if (strncmp(s, d->dir, n) || strchr(s + n, '/')) continue;
        strcpy(name, s + n);
        if (st) fill_stat(st, e);
        return 0;
    }
    r->_errno = ENOENT;
    return -1;
}

static int e_dirclose(struct _reent *r, DIR_ITER *it) { (void)r; (void)it; return 0; }

static const devoptab_t embed_dev = {
    .name = "embed", .structSize = sizeof(File),
    .open_r = e_open, .close_r = e_close, .write_r = e_write, .read_r = e_read,
    .seek_r = e_seek, .fstat_r = e_fstat, .stat_r = e_stat,
    .dirStateSize = sizeof(Dir), .diropen_r = e_diropen, .dirreset_r = e_dirreset,
    .dirnext_r = e_dirnext, .dirclose_r = e_dirclose,
};

bool wii_embed_mount(void)
{
    if (embed_bin_size < 8 || memcmp(embed_bin, "GEMB", 4) || !count()) return false;
    return AddDevice(&embed_dev) >= 0;
}
