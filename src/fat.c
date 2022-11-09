/* SPDX-License-Identifier : GPL-2.0 */

/* fat.c  -  Read/write access to the FAT */

/* Written 1993 by Werner Almesberger */

/* FAT32, VFAT, Atari format support, and various fixes additions May 1998
 * by Roman Hodek <Roman.Hodek@informatik.uni-erlangen.de> */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "common.h"
#include "dosfsck.h"
#include "io.h"
#include "check.h"
#include "fat.h"

static void get_fat_entry(FAT_ENTRY *entry, void *fat, uint32_t cluster, DOS_FS *fs)
{
    unsigned char *ptr;

    switch (fs->fat_bits) {
        case 12:
            ptr = &((unsigned char *) fat)[cluster * 3 / 2];
            entry->value = 0xfff & (cluster & 1 ? (ptr[0] >> 4) | (ptr[1] << 4) :
                    (ptr[0] | ptr[1] << 8));
            break;
        case 16:
            entry->value = CF_LE_W(((unsigned short *)fat)[cluster]);
            break;
        case 32:
            /* According to M$, the high 4 bits of a FAT32 entry are reserved and
             * are not part of the cluster number. So we cut them off. */
            {
                uint32_t e = CF_LE_L(((unsigned int *)fat)[cluster]);
                entry->value = e & 0xfffffff;
                entry->reserved = e >> 28;
            }
            break;
        default:
            die("Bad FAT entry size: %d bits.", fs->fat_bits);
    }

    entry->owner = NULL;
}

/*
 * allocate FAT_ENTRY array in DOS_FS,
 * read first/second FAT and compare both.
 * check cluster out of range and set it free
 */
void read_fat(DOS_FS *fs)
{
    int eff_size;
    uint32_t i;
    void *first, *second = NULL;
    int first_ok, second_ok;

    /* 2 == FAT_START_ENT */
    eff_size = ((fs->clusters + 2ULL) * fs->fat_bits + 7) / 8ULL;
    first = alloc(eff_size);
    fs_read(fs->fat_start, eff_size, first);

    /* TODO: handle in case that fs->nfats is bigger than 2 */
    if (fs->nfats > 2) {
        printf("Not support filesystem that have more than 2 FATs\n");
        exit(1);
    }

    if (fs->nfats > 1) {
        second = alloc(eff_size);
        fs_read(fs->fat_start + fs->fat_size, eff_size, second);
    }

    if (second && memcmp(first, second, eff_size) != 0) {
        FAT_ENTRY first_media, second_media;

        get_fat_entry(&first_media, first, 0, fs);
        get_fat_entry(&second_media, second, 0, fs);
        first_ok = (first_media.value & FAT_EXTD(fs)) == FAT_EXTD(fs);
        second_ok = (second_media.value & FAT_EXTD(fs)) == FAT_EXTD(fs);

        if (first_ok && !second_ok) {
            printf("FATs differ - using first FAT.\n");
            fs_write(fs->fat_start + fs->fat_size, eff_size, first);
        }

        if (!first_ok && second_ok) {
            printf("FATs differ - using second FAT.\n");
            fs_write(fs->fat_start, eff_size, second);
            memcpy(first, second, eff_size);
        }

        if (first_ok && second_ok) {
            if (interactive) {
                printf("FATs differ but appear to be intact. Use which FAT ?\n"
                        "1) Use first FAT\n"
                        "2) Use second FAT\n");
                if (get_key("12", "?") == '1') {
                    fs_write(fs->fat_start + fs->fat_size, eff_size, first);
                } else {
                    fs_write(fs->fat_start, eff_size, second);
                    memcpy(first, second, eff_size);
                }
            }
            else {
                printf("FATs differ but appear to be intact. Using first "
                        "FAT.\n");
                fs_write(fs->fat_start + fs->fat_size, eff_size, first);
            }
        }

        if (!first_ok && !second_ok) {
            printf("Both FATs appear to be corrupt. Giving up.\n");
            exit(1);
        }
    }

    if (second) {
        free(second);
    }

    fs->fat = qalloc(&mem_queue, sizeof(FAT_ENTRY) * (fs->clusters + 2ULL));
    for (i = 0; i < FAT_START_ENT; i++) {
        get_fat_entry(&fs->fat[i], first, i, fs);
    }

    for (i = FAT_START_ENT; i < fs->clusters + 2; i++) {
        get_fat_entry(&fs->fat[i], first, i, fs);

        if (fs->fat[i].value >= fs->clusters + 2 &&
                (fs->fat[i].value < FAT_MIN_BAD(fs))) {
            printf("Cluster %u out of range (%u > %u). Setting to EOF.\n",
                    i, fs->fat[i].value, fs->clusters + 2 - 1);
            set_fat(fs, i, -1);
        }
    }

    free(first);
}

void get_fat(DOS_FS *fs, uint32_t cluster, FAT_ENTRY *fatent)
{
    *fatent = fs->fat[cluster];
}

void set_fat(DOS_FS *fs, uint32_t cluster, uint32_t new)
{
    unsigned char data[4];
    int size;
    loff_t offs;

    if ((int32_t)new == -1)
        new = FAT_EOF(fs);
    else if ((int32_t)new == -2)
        new = FAT_BAD(fs);

    switch (fs->fat_bits) {
        case 12:
            offs = fs->fat_start + cluster * 3 / 2;
            if (cluster & 1) {
                data[0] = ((new & 0xf) << 4) | (fs->fat[cluster - 1].value >> 8);
                data[1] = new >> 4;
            }
            else {
                data[0] = new & 0xff;
                data[1] = (new >> 8) | (cluster == fs->clusters - 1 ? 0 :
                        (0xff & fs->fat[cluster + 1].value) << 4);
            }
            size = 2;
            break;
        case 16:
            offs = fs->fat_start + cluster * 2;
            *(uint16_t *)data = CT_LE_W(new);
            size = 2;
            break;
        case 32:
            offs = fs->fat_start + cluster * 4;
            /* According to M$, the high 4 bits of a FAT32 entry are reserved and
             * are not part of the cluster number. So we never touch them. */
            *(uint32_t *)data =
                CT_LE_L((new & 0xfffffff) | (fs->fat[cluster].reserved << 28));
            size = 4;
            break;
        default:
            die("Bad FAT entry size: %d bits.", fs->fat_bits);
    }

    fs->fat[cluster].value = new;
    fs_write(offs, size, &data);
    if (fs->nfats > 1) {
        fs_write(offs + fs->fat_size, size, &data);
    }
}

int bad_cluster(DOS_FS *fs, uint32_t cluster)
{
    return FAT_IS_BAD(fs, fs->fat[cluster].value);
}

uint32_t next_cluster(DOS_FS *fs, uint32_t cluster)
{
    uint32_t value;

    value = fs->fat[cluster].value;
    if (FAT_IS_BAD(fs, value))
        die("Internal error: next_cluster on bad cluster");

    return FAT_IS_EOF(fs, value) ? -1 : value;
}

loff_t cluster_start(DOS_FS *fs, uint32_t cluster)
{
    return fs->data_start +
        ((loff_t)cluster - 2) * (unsigned long long)fs->cluster_size;
}

void set_owner(DOS_FS *fs, uint32_t cluster, DOS_FILE *owner)
{
    if (owner && fs->fat[cluster].owner)
        die("Internal error: attempt to change file owner");

    fs->fat[cluster].owner = owner;
}

DOS_FILE *get_owner(DOS_FS *fs, uint32_t cluster)
{
    return fs->fat[cluster].owner;
}

void fix_bad(DOS_FS *fs)
{
    uint32_t i;

    if (verbose)
        printf("Checking for bad clusters.\n");

    for (i = 2; i < fs->clusters + 2; i++) {
        if (!get_owner(fs, i) && !FAT_IS_BAD(fs, fs->fat[i].value))
            if (!fs_test(cluster_start(fs, i), fs->cluster_size)) {
                printf("Cluster %u is unreadable.\n", i);
                set_fat(fs, i, -2);
            }
    }
}

void reclaim_free(DOS_FS *fs)
{
    int reclaimed;
    uint32_t i;

    if (verbose)
        printf("Checking for unused clusters.\n");

    reclaimed = 0;
    for (i = FAT_START_ENT; i < fs->clusters + FAT_START_ENT; i++) {
        if (!get_owner(fs, i) && fs->fat[i].value &&
                !FAT_IS_BAD(fs, fs->fat[i].value)) {
            set_fat(fs, i, 0);
            reclaimed++;
        }
    }

    if (reclaimed)
        printf("Reclaimed %d unused cluster%s (%llu bytes).\n", reclaimed,
                reclaimed == 1 ?  "" : "s",
                (unsigned long long)reclaimed * fs->cluster_size);
}

/* set owner of cluster which is orphan to dummy owner.
 * if it has circular chain, then break it */
static void tag_free(DOS_FS *fs, DOS_FILE *ptr)
{
    DOS_FILE *owner;
    uint32_t prev;
    uint32_t i, walk;

    for (i = FAT_START_ENT; i < fs->clusters + FAT_START_ENT; i++) {
        if (fs->fat[i].value && !FAT_IS_BAD(fs, fs->fat[i].value) &&
                !get_owner(fs, i) && !fs->fat[i].prev) {
            prev = 0;
            for (walk = i; walk > 0 && walk != -1;
                    walk = next_cluster(fs, walk)) {
                if (!(owner = get_owner(fs, walk)))
                    set_owner(fs, walk, ptr);
                else if (owner != ptr)
                    /* TODO: need to analyze what it mean, which case? */
                    die("Internal error: free chain collides with file");
                else {
                    set_fat(fs, prev, -1);
                    break;
                }
                prev = walk;
            }
        }
    }
}

void reclaim_file(DOS_FS *fs)
{
    DOS_FILE dummy;
    int reclaimed, files, changed;
    uint32_t i, next, walk;

    if (verbose)
        printf("Reclaiming unconnected clusters.\n");

    /* initialize FAT_ENTRY's 'prev' (reference count) field */
    for (i = FAT_START_ENT; i < fs->clusters + FAT_START_ENT; i++)
        fs->fat[i].prev = 0;

    /* set 'prev' field to find first cluster of chain, later */
    for (i = FAT_START_ENT; i < fs->clusters + FAT_START_ENT; i++) {
        next = fs->fat[i].value;
        if (!get_owner(fs, i) && next && next < fs->clusters + FAT_START_ENT) {
            /* i and next cluster is linked, but does not have owner */
            if (get_owner(fs, next) || !fs->fat[next].value ||
                    FAT_IS_BAD(fs, fs->fat[next].value))
                set_fat(fs, i, -1);
            else
                fs->fat[next].prev++;
        }
    }

    do {
        tag_free(fs, &dummy);
        changed = 0;

        /* check orphan cluster's onwer again.
         * And if it has, then set cluster to EOF.
         * if it is changed then check again */
        for (i = FAT_START_ENT; i < fs->clusters + FAT_START_ENT; i++) {
            if (fs->fat[i].value && !FAT_IS_BAD(fs, fs->fat[i].value) &&
                    !get_owner(fs, i)) {

                if (!fs->fat[fs->fat[i].value].prev--)
                    die("Internal error: prev going below zero");

                set_fat(fs, i, -1);
                changed = 1;
                printf("Broke cycle at cluster %u in free chain.\n", i);
                break;
            }
        }
    } while (changed);

    files = reclaimed = 0;
    for (i = FAT_START_ENT; i < fs->clusters + FAT_START_ENT; i++) {
        /* if i(cluster) is the first cluster of orphaned cluster chain,
         * make entry in root directory and set i to start cluster */
        if (get_owner(fs, i) == &dummy && !fs->fat[i].prev) {
            DIR_ENT de;
            loff_t offset;
            files++;
            offset = alloc_rootdir_entry(fs, &de, "FSCK%04dREC");
            de.start = CT_LE_W(i & 0xffff);

            if (fs->fat_bits == 32)
                de.starthi = CT_LE_W(i >> 16);

            for (walk = i; walk > 0 && walk != -1;
                    walk = next_cluster(fs, walk)) {
                de.size = CT_LE_L(CF_LE_L(de.size) + fs->cluster_size);
                reclaimed++;
            }

            fs_write(offset, sizeof(DIR_ENT), &de);
        }
    }

    if (reclaimed)
        printf("Reclaimed %d unused cluster%s (%llu bytes) in %d chain%s.\n",
                reclaimed, reclaimed == 1 ? "" : "s",
                (unsigned long long)reclaimed * fs->cluster_size, files,
                files == 1 ? "" : "s");
}

uint32_t update_free(DOS_FS *fs)
{
    uint32_t i;
    uint32_t free = 0;
    int do_set = 0;

    /* is it possible to calclulate free count in previous routine?
     * (in reclaim_free or reclaim_file) */
    for (i = FAT_START_ENT; i < fs->clusters + FAT_START_ENT; i++)
        if (!get_owner(fs, i) && !FAT_IS_BAD(fs, fs->fat[i].value))
            ++free;

    if (!fs->fsinfo_start)
        return free;

    if (verbose)
        printf("Checking free cluster summary.\n");

    if (fs->free_clusters >= 0) {
        if (free != fs->free_clusters) {
            printf("Free cluster summary wrong (%u vs. really %u)\n",
                    fs->free_clusters, free);
            if (interactive)
                printf("1) Correct\n"
                        "2) Don't correct\n");
            else
                printf("  Auto-correcting.\n");

            if (!interactive || get_key("12", "?") == '1')
                do_set = 1;
        }
    }
    else {
        printf("Free cluster summary uninitialized (should be %u)\n", free);
        if (interactive) {
            printf("1) Set it\n"
                    "2) Leave it uninitialized\n");
        }
        else {
            printf("  Auto-setting.\n");
        }

        if (!interactive || get_key("12", "?") == '1')
            do_set = 1;
    }

    if (do_set) {
        fs->free_clusters = free;
        free = CT_LE_L(free);
        fs_write(fs->fsinfo_start + offsetof(struct fsinfo_sector, free_clusters),
                sizeof(free), &free);
    }

    return free;
}

/* Local Variables: */
/* tab-width: 8     */
/* End:             */