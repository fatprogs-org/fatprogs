/* SPDX-License-Identifier : GPL-2.0 */

/* io.c  -  Virtual disk input/output */

/* Written 1993 by Werner Almesberger */

/*
 * Thu Feb 26 01:15:36 CET 1998: Martin Schulze <joey@infodrom.north.de>
 *	Fixed nasty bug that caused every file with a name like
 *	xxxxxxxx.xxx to be treated as bad name that needed to be fixed.
 */

/* FAT32, VFAT, Atari format support, and various fixes additions May 1998
 * by Roman Hodek <Roman.Hodek@informatik.uni-erlangen.de> */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fd.h>

#include "dosfsck.h"
#include "common.h"
#include "io.h"

typedef struct _change {
    void *data;
    loff_t pos;
    int size;
    struct _change *next;
} CHANGE;

static CHANGE *changes, *last;
static int fd, did_change = 0;

unsigned device_no;

#ifdef __DJGPP__
#include "volume.h"	/* DOS lowlevel disk access functions */
#undef llseek
static loff_t llseek(int fd, loff_t offset, int whence)
{
    if ((whence != SEEK_SET) || (fd == 4711))
        return -1; /* only those supported */
    return VolumeSeek(offset);
}
#define open OpenVolume
#define close CloseVolume
#define read(a,b,c) ReadVolume(b,c)
#define write(a,b,c) WriteVolume(b,c)
#endif

void fs_open(char *path, int rw)
{
    struct stat stbuf;

    if ((fd = open(path, rw ? O_RDWR : O_RDONLY)) < 0)
        pdie("open %s", path);

    changes = last = NULL;
    did_change = 0;

#ifndef _DJGPP_
    if (fstat(fd, &stbuf) < 0)
        pdie("fstat %s", path);

    device_no = S_ISBLK(stbuf.st_mode) ? (stbuf.st_rdev >> 8) & 0xff : 0;
#else
    if (IsWorkingOnImageFile()) {
        if (fstat(GetVolumeHandle(), &stbuf) < 0)
            pdie("fstat image %s", path);
        device_no = 0;
    }
    else {
        /* return 2 for floppy, 1 for ramdisk, 7 for loopback  */
        /* used by boot.c in Atari mode: floppy always FAT12,  */
        /* loopback / ramdisk only FAT12 if usual floppy size, */
        /* harddisk always FAT16 on Atari... */
        device_no = (GetVolumeHandle() < 2) ? 2 : 1;
        /* telling "floppy" for A:/B:, "ramdisk" for the rest */
    }
#endif
}

void fs_read(loff_t pos, int size, void *data)
{
    CHANGE *walk;
    int got;

    if (llseek(fd, pos, SEEK_SET) != pos)
        pdie("Seek to %lld(%d,%s)", pos, __LINE__, __func__);

    if ((got = read(fd, data, size)) < 0)
        die("Got %d bytes instead of %d at %lld(%d,%s)",
                got, size, pos, __LINE__, __func__);

    if (got != size)
        die("Got %d bytes instead of %d at %lld", got, size, pos);

    for (walk = changes; walk; walk = walk->next) {
        if (pos + size < walk->pos) {
            break;
        }

        /* in case that new read area overlap with previous read data */
        if (walk->pos < pos + size && walk->pos + walk->size > pos) {
            if (walk->pos < pos)
                memcpy(data, (char *)walk->data + pos - walk->pos,
                        min(size, walk->size - pos + walk->pos));
            else
                memcpy((char *)data + walk->pos - pos, walk->data,
                        min(walk->size, size + pos - walk->pos));
        }
    }
}

int fs_test(loff_t pos, int size)
{
    void *scratch;
    int okay;

    if (llseek(fd, pos, SEEK_SET) != pos)
        pdie("Seek to %lld(%d,%s)", pos, __LINE__, __func__);

    scratch = alloc_mem(size);
    okay = read(fd, scratch, size) == size;
    free_mem(scratch);
    return okay;
}

static CHANGE *merge_change(CHANGE *old, CHANGE *new)
{
    CHANGE *merge;
    int size;

    merge = alloc_mem(sizeof(CHANGE));

    merge->pos = min(old->pos, new->pos);
    merge->size = max(old->pos + old->size, new->pos + new->size) -
        min(old->pos, new->pos);
    merge->data = alloc_mem(merge->size);

    /*
     * new :         |--------|
     * old : |----------|
     */
    if (merge->pos == old->pos) {
        size = new->pos - old->pos;
        memcpy(merge->data, old->data, size);
        memcpy(merge->data + size, new->data, new->size);
    }
    /*
     * new : |--------|
     * old:        |----------|
     */
    else {
        size = (old->pos + old->size) - (new->pos + new->size);
        memcpy(merge->data, new->data, new->size);
        memcpy(merge->data + new->size, old->data + (old->size - size), size);
    }
    return merge;
}

static void free_change(CHANGE *del)
{
    if (del) {
        free_mem(del->data);
        free_mem(del);
    }
}

static void add_change_list(CHANGE *prev, CHANGE *new, CHANGE *next)
{
    if (prev) {
        new->next = next;
        prev->next = new;
    }
    else {
        new->next = changes;
        changes = new;
    }

    if (!next) {
        if (last == prev)
            last = new;
    }
}

static void del_change_list(CHANGE *prev, CHANGE *del)
{
    if (prev) {
        prev->next = del->next;
    }
    else {
        changes = del->next;
    }

    if (last == del) {
        last = prev;
    }
}

void print_changes(void)
{
    CHANGE *walk;
    CHANGE *next = NULL;
    int i;

    printf("Wrong data in CHANGES list : ");
    if (changes == NULL)
        printf("None");
    else
        printf("\n");

    for (i = 0, walk = changes; walk; walk = walk->next, i++) {
        next = walk->next;
        if (!next) {
            break;
        }

        if ((walk->pos >= next->pos) || (walk->pos + walk->size > next->pos)) {
            printf("%5d : pos %8ld, size %8d\n", i, walk->pos, walk->size);
            printf("%5d : pos %8ld, size %8d\n", i + 1, next->pos, next->size);
        }
    }
}

void fs_write(loff_t pos, int size, void *data)
{
    CHANGE *new;
    CHANGE *walk;
    CHANGE *prev;
    CHANGE *merge;
    int did;

    if (write_immed) {
        did_change = 1;
        if (llseek(fd, pos, SEEK_SET) != pos)
            pdie("Seek to %lld(%d,%s)", pos, __LINE__, __func__);

        if ((did = write(fd, data, size)) == size)
            return;
        if (did < 0)
            pdie("Write %d bytes at %lld(%d,%s)", size, pos, __LINE__, __func__);

        die("Wrote %d bytes instead of %d at %lld", did, size, pos);
    }

    new = alloc_mem(sizeof(CHANGE));
    new->pos = pos;
    memcpy(new->data = alloc_mem(new->size = size), data, size);
    new->next = NULL;

    /* for first entry */
    if (!last) {
        changes = new;
        last = new;
        return;
    }

    prev = NULL;
    for (walk = changes; walk; prev = walk, walk = walk->next) {
        if (pos >= walk->pos + walk->size) {
            CHANGE *next;
            /* new  :           |--------|
             * walk : |--------|
             * Include when walk is last entry of list.
             * -> add new */
            next = walk->next;
            if (!next || ((pos + size) <= next->pos)) {
                add_change_list(walk, new, next);
                break;
            }
            continue;
        }

        /* new : |--------|
         * walk:           |---------|
         * Include walk is first entry of list.
         * -> add new */
        if (pos + size <= walk->pos) {
            add_change_list(prev, new, walk);
            break;
        }

        if (pos <= walk->pos) {
            /* new : |-------------------------|
             * walk:     |--------|      |--------|
             * -> use new & delete walk */
            if (pos + size >= walk->pos + walk->size) {
                add_change_list(prev, new, walk);
                del_change_list(new, walk);

                free_change(walk);

                /* walk->next's area also may be overlapped by new. Check next. */
                continue;
            }
            /* new : |--------|
             * walk: |----------------|
             * -> use walk & copy new data, delete new */
            else if (pos == walk->pos) {
                memcpy(walk->data, new->data, new->size);
                free_change(new);
                break;
            }
            /* new : |--------|
             * walk:        |---------|
             * -> merge & delete walk/new */
            else {
                merge = merge_change(walk, new);
                add_change_list(prev, merge, walk);
                del_change_list(merge, walk);

                free_change(walk);
                free_change(new);
                break;
            }
        }
        else {
            /* new :         |--------|
             * walk: |----------|
             * -> merge & delete walk/new */
            if (pos + size > walk->pos + walk->size) {
                merge = merge_change(walk, new);
                add_change_list(prev, merge, walk);
                del_change_list(merge, walk);

                free_change(walk);
                free_change(new);
                break;
            }
            /* new :         |--------|
             * walk: |-----------------|
             * -> user walk & delete new */
            else {
                memcpy(walk->data + (pos - walk->pos), new->data, new->size);
                free_change(new);
                break;
            }
        }
    }
}

static void fs_flush(void)
{
    CHANGE *this;
    int size;

    while (changes) {
        this = changes;
        changes = changes->next;
        if (llseek(fd, this->pos, SEEK_SET) != this->pos)
            fprintf(stderr, "Seek to %lld failed: %s\n  Did not write %d bytes.\n",
                    (long long)this->pos, strerror(errno), this->size);
        else if ((size = write(fd, this->data, this->size)) < 0)
            fprintf(stderr, "Writing %d bytes at %lld failed: %s\n",
                    this->size, (long long)this->pos, strerror(errno));
        else if (size != this->size)
            fprintf(stderr, "Wrote %d bytes instead of %d bytes at %lld.\n",
                    size, this->size, (long long)this->pos);
        free_mem(this->data);
        free_mem(this);
    }
}

int fs_close(int write)
{
    CHANGE *next;
    int changed;

    changed = !!changes;
    if (write)
        fs_flush();
    else
        while (changes) {
            next = changes->next;
            free_mem(changes->data);
            free_mem(changes);
            changes = next;
        }

    if (close(fd) < 0)
        pdie("closing file system");

    return changed || did_change;
}

int fs_changed(void)
{
    return !!changes || did_change;
}

/* Local Variables: */
/* tab-width: 8     */
/* End:             */
