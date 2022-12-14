/* SPDX-License-Identifier : GPL-2.0 */

/* boot.c  -  Read and analyze ia PC/MS-DOS boot sector */

/* Written 1993 by Werner Almesberger */

/* FAT32, VFAT, Atari format support, and various fixes additions May 1998
 * by Roman Hodek <Roman.Hodek@informatik.uni-erlangen.de> */

#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <stdlib.h>

#include "common.h"
#include "dosfsck.h"
#include "io.h"
#include "boot.h"

static struct {
    __u8 media;
    char *descr;
} mediabytes[] = {
    { 0xf0, "5.25\" or 3.5\" HD floppy" },
    { 0xf8, "hard disk" },
    { 0xf9, "3,5\" 720k floppy 2s/80tr/9sec or "
        "5.25\" 1.2M floppy 2s/80tr/15sec" },
    { 0xfa, "5.25\" 320k floppy 1s/80tr/8sec" },
    { 0xfb, "3.5\" 640k floppy 2s/80tr/8sec" },
    { 0xfc, "5.25\" 180k floppy 1s/40tr/9sec" },
    { 0xfd, "5.25\" 360k floppy 2s/40tr/9sec" },
    { 0xfe, "5.25\" 160k floppy 1s/40tr/8sec" },
    { 0xff, "5.25\" 320k floppy 2s/40tr/8sec" },
};

static char *get_media_descr(unsigned char media)
{
    int i;

    for (i = 0; i < sizeof(mediabytes) / sizeof(*mediabytes); ++i) {
        if (mediabytes[i].media == media)
            return (mediabytes[i].descr);
    }

    return("undefined");
}

static void dump_boot(DOS_FS *fs, struct boot_sector *b, unsigned lss)
{
    unsigned short sectors;

    printf("\nBoot sector contents:\n");

    if (!atari_format) {
        char id[9];
        strncpy(id, (char *)b->system_id, 8);
        id[8] = 0;
        printf("System ID \"%s\"\n", id);
    }
    else {
        /* On Atari, a 24 bit serial number is stored at offset 8 of the boot
         * sector */
        printf("Serial number 0x%x\n",
                b->system_id[5] | (b->system_id[6] << 8) | (b->system_id[7] << 16));
    }
    printf("Media byte 0x%02x (%s)\n", b->media, get_media_descr(b->media));
    printf("%10d bytes per logical sector\n", GET_UNALIGNED_W(b->sector_size));
    printf("%10d bytes per cluster\n", fs->cluster_size);
    printf("%10d reserved sector%s\n", CF_LE_W(b->reserved_cnt),
            CF_LE_W(b->reserved_cnt) == 1 ? "" : "s");
    printf("First FAT starts at byte %llu (sector %llu)\n",
            (unsigned long long)fs->fat_start,
            (unsigned long long)fs->fat_start/lss);
    printf("%10d FATs, %d bit entries\n", b->nfats, fs->fat_bits);
    printf("%10d bytes per FAT (= %u sectors)\n", fs->fat_size,
            fs->fat_size/lss);
    if (!fs->root_cluster) {
        printf("Root directory starts at byte %llu (sector %llu)\n",
                (unsigned long long)fs->root_start,
                (unsigned long long)fs->root_start/lss);
        printf("%10d root directory entries\n", fs->root_entries);
    }
    else {
        printf("Root directory start at cluster %u (arbitrary size)\n",
                fs->root_cluster);
    }
    printf("Data area starts at byte %llu (sector %llu)\n",
            (unsigned long long)fs->data_start,
            (unsigned long long)fs->data_start/lss);
    printf("%10u data clusters (%llu bytes)\n", fs->clusters,
            (unsigned long long)fs->clusters * fs->cluster_size);
    printf("%u sectors/track, %u heads\n", CF_LE_W(b->sec_per_track),
            CF_LE_W(b->heads));
    printf("%10u hidden sectors\n",
            atari_format ?
            /* On Atari, the hidden field is only 16 bit wide and unused */
            (((unsigned char *)&b->hidden)[0] |
             ((unsigned char *)&b->hidden)[1] << 8) :
            CF_LE_L(b->hidden));
    sectors = GET_UNALIGNED_W(b->sectors);
    printf("%10u sectors total\n", sectors ? sectors : CF_LE_L(b->total_sect));
    printf("==========================================================\n");
}

/* check if there is difference of boot dirty flag
 * between boot and backup boot sector
 *
 * return 1 : if boot and backup boot sector is same except boot flag.
 * return 0 : if not. */
static int check_boot_flag(struct boot_sector *b, struct boot_sector *b2)
{
    b2->fat32.vi.state = b->fat32.vi.state;
    return !memcmp(b, b2, sizeof(struct boot_sector));
}

static void check_backup_boot(DOS_FS *fs, struct boot_sector *b, int lss)
{
    struct boot_sector b2;

    if (!fs->backupboot_start) {

        printf("There is no backup boot sector.\n");
        if (CF_LE_W(b->reserved_cnt) < 3) {
            printf("And there is no space for creating one!\n");
            return;
        }

        if (interactive)
            printf("1) Create one\n2) Do without a backup\n");
        else
            printf("  Auto-creating backup boot block.\n");

        if (!interactive || get_key("12", "?") == '1') {
            int bbs;
            /* The usual place for the backup boot sector is sector 6. Choose
             * that or the last reserved sector. */
            if (CF_LE_W(b->reserved_cnt) >= 7 && CF_LE_W(b->fat32.info_sector) != 6)
                bbs = 6;
            else {
                bbs = CF_LE_W(b->reserved_cnt) - 1;
                if (bbs == CF_LE_W(b->fat32.info_sector))
                    --bbs; /* this is never 0, as we checked reserved_cnt >= 3! */
            }
            fs->backupboot_start = bbs * lss;
            b->fat32.backup_boot = CT_LE_W(bbs);
            fs_write(fs->backupboot_start, sizeof(*b), b);
            fs_write((off_t)offsetof(struct boot_sector, fat32.backup_boot),
                    sizeof(b->fat32.backup_boot), &b->fat32.backup_boot);
            printf("Created backup of boot sector in sector %d\n", bbs);
            return;
        }
        else return;
    }

    fs_read(fs->backupboot_start, sizeof(b2), &b2);
    if (memcmp(b, &b2, sizeof(b2)) != 0) {
        /* there are any differences */
        __u8 *p, *q;
        int i, pos, first = 1;
        char buf[20];

        /* check if there is only difference of boot flag */
        if (check_boot_flag(b, &b2)) {
            return;
        }

        printf( "There are differences between boot sector and its backup.\n" );
        printf( "Differences: (offset:original/backup)\n  " );
        pos = 2;
        for (p = (__u8 *)b, q = (__u8 *)&b2, i = 0;
                i < sizeof(b2);
                ++p, ++q, ++i) {

            if (*p != *q) {
                sprintf(buf, "%s%u:%02x/%02x", first ? "" : ", ",
                        (unsigned)(p - (__u8 *)b), *p, *q);

                if (pos + strlen(buf) > 78)
                    printf("\n  "), pos = 2;

                printf("%s", buf);
                pos += strlen(buf);
                first = 0;
            }
        }
        printf("\n");

        if (interactive)
            printf("1) Copy original to backup\n"
                    "2) Copy backup to original\n"
                    "3) No action\n");
        else {
            printf("  Not automatically fixing this.\n");
        }

        switch (interactive ? get_key("123", "?") : '3') {
            case '1':
                fs_write(fs->backupboot_start, sizeof(*b), b);
                break;
            case '2':
                fs_write(0, sizeof(b2), &b2);
                break;
            default:
                break;
        }
    }
}

static void init_fsinfo(struct fsinfo_sector *fsinfo)
{
    memset(fsinfo, 0, sizeof(struct fsinfo_sector));
    fsinfo->magic = CT_LE_L(LEAD_SIGN);
    fsinfo->signature = CT_LE_L(STRUCT_SIGN);
    fsinfo->free_clusters = CT_LE_L(-1);
    fsinfo->next_cluster = CT_LE_L(2);
    fsinfo->boot_sign = CT_LE_W(BOOT_SIGN);
}

static void read_fsinfo(DOS_FS *fs, struct boot_sector *b, int lss)
{
    struct fsinfo_sector fsinfo;

    if (!b->fat32.info_sector) {
        printf("No FSINFO sector\n");

        if (interactive)
            printf("1) Create one\n2) Do without FSINFO\n");
        else {
            printf("  Automatically creating FSINFO.\n");
        }

        switch (interactive ? get_key("12", "?") : '1') {
            __u32 s;
            case '1':
                /* search for a free reserved sector (not boot sector and not
                 * backup boot sector) */

                for (s = 1; s < CF_LE_W(b->reserved_cnt); ++s) {
                    if (s != CF_LE_W(b->fat32.backup_boot))
                        break;
                }

                if (s > 0 && s < CF_LE_W(b->reserved_cnt)) {
                    init_fsinfo(&fsinfo);
                    fs_write((off_t)s * lss, sizeof(fsinfo), &fsinfo);
                    b->fat32.info_sector = CT_LE_W(s);
                    fs_write((off_t)offsetof(struct boot_sector, fat32.info_sector),
                            sizeof(b->fat32.info_sector), &b->fat32.info_sector);
                    if (fs->backupboot_start)
                        fs_write(fs->backupboot_start +
                                offsetof(struct boot_sector, fat32.info_sector),
                                sizeof(b->fat32.info_sector), &b->fat32.info_sector);
                }
                else {
                    printf("No free reserved sector found -- "
                            "no space for FSINFO sector!\n");
                    return;
                }

                break;

            case '2':
                return;
        }
    }

    fs->fsinfo_start = CF_LE_W(b->fat32.info_sector) * lss;
    fs_read(fs->fsinfo_start, sizeof(fsinfo), &fsinfo);

    if (fsinfo.magic != CT_LE_L(LEAD_SIGN) ||
            fsinfo.signature != CT_LE_L(STRUCT_SIGN) ||
            fsinfo.boot_sign != CT_LE_W(BOOT_SIGN)) {

        printf("FSINFO sector has bad magic number(s):\n");

        if (fsinfo.magic != CT_LE_L(LEAD_SIGN))
            printf("  Offset %llu: 0x%08x != expected 0x%08x\n",
                    (unsigned long long)offsetof(struct fsinfo_sector, magic),
                    CF_LE_L(fsinfo.magic), LEAD_SIGN);

        if (fsinfo.signature != CT_LE_L(STRUCT_SIGN))
            printf("  Offset %llu: 0x%08x != expected 0x%08x\n",
                    (unsigned long long)offsetof(struct fsinfo_sector, signature),
                    CF_LE_L(fsinfo.signature), STRUCT_SIGN);

        if (fsinfo.boot_sign != CT_LE_W(BOOT_SIGN))
            printf("  Offset %llu: 0x%04x != expected 0x%04x\n",
                    (unsigned long long)offsetof(struct fsinfo_sector, boot_sign),
                    CF_LE_W(fsinfo.boot_sign), BOOT_SIGN);

        if (interactive)
            printf("1) Correct\n2) Don't correct (FSINFO invalid then)\n");
        else
            printf("  Auto-correcting it.\n");

        if (!interactive || get_key("12", "?") == '1') {
            init_fsinfo(&fsinfo);
            fs_write(fs->fsinfo_start, sizeof(fsinfo), &fsinfo);
        }
        else
            fs->fsinfo_start = 0;
    }

    if (fs->fsinfo_start) {
        fs->free_clusters = CF_LE_L(fsinfo.free_clusters);
        fs->next_cluster = CF_LE_L(fsinfo.next_cluster);
    }
}

static inline int is_valid_media(unsigned char media)
{
    return 0xf8 <= media || media == 0xf0;
}

static int is_valid_boot(DOS_FS *fs, struct boot_sector *b)
{
    unsigned short reserved_sector;
    unsigned short sec_per_fat;
    unsigned short sector_size = GET_UNALIGNED_W(b->sector_size);
    unsigned short sectors;
    uint32_t total_sectors;

    reserved_sector = CF_LE_W(b->reserved_cnt);
    if (!reserved_sector || !b->nfats || !is_valid_media(b->media)) {
        printf("Boot fields are not valid(reserved:%d, nfat:%d)\n",
                reserved_sector, b->nfats);
        return 0;
    }

    if (b->sec_per_fat == 0 && b->fat32.sec_per_fat32 == 0) {
        printf("fat length fields are all zero\n");
        return 0;
    }

    sectors = GET_UNALIGNED_W(b->sectors);
    total_sectors = sectors ? sectors : CF_LE_L(b->total_sect);
    sec_per_fat = CF_LE_W(b->sec_per_fat) ?
        CF_LE_W(b->sec_per_fat) : CF_LE_L(b->fat32.sec_per_fat32);

    if (reserved_sector + (sec_per_fat * b->nfats) > total_sectors) {
        printf("Reserved sector value is larger than total sector\n");
        return 0;
    }

    if ((!is_power_of_2(sector_size)) ||
            (sector_size < 512) ||
            (sector_size > 4096)) {
        printf("Logical sector size is not valid.\n");
        return 0;
    }

    if (!is_power_of_2(b->sec_per_clus)) {
        printf("Cluster size is not even value.\n");
        return 0;
    }

    return 1;
}

int copy_backup_boot(DOS_FS *fs, struct boot_sector *b)
{
    struct boot_sector b2;
    unsigned short sector_size;

    sector_size = GET_UNALIGNED_W(b->sector_size);
    fs->backupboot_start = CF_LE_W(b->fat32.backup_boot) * sector_size;
    if (!fs->backupboot_start) {
        printf("Backup boot sector number is not set!\n");
        return 0;
    }

    fs_read(fs->backupboot_start, sizeof(b2), &b2);
    if (!is_valid_boot(fs, &b2)) {
        printf("Backup boot sector is not valid, too\n");
        return 0;
    }

    printf("Copy backup sector to original\n");
    memcpy(b, &b2, sizeof(b2));
    fs_write(0, sizeof(b2), &b2);
    return 1;
}

void read_boot(DOS_FS *fs)
{
    struct boot_sector b;
    unsigned total_sectors;
    unsigned short logical_sector_size, sectors;
    unsigned sec_per_fat;
    off_t data_size;
    struct volume_info *vi;

    fs_read(0, sizeof(b), &b);

    if (!is_valid_boot(fs, &b)) {
        if (!b.sec_per_fat && b.fat32.sec_per_fat32) {
            /* FAT32 */
            if (!copy_backup_boot(fs, &b)) {
                exit(EXIT_NOT_SUPPORT);
            }
        }
        else {
            exit(EXIT_NOT_SUPPORT);
        }
    }

    if (b.boot_sign != CT_LE_W(BOOT_SIGN)) {
        printf("WARN: boot sector has bad magic number:\n");
        printf("  Boot signature : 0x%08x != expected 0x%08x\n",
                CF_LE_W(b.boot_sign), CT_LE_W(BOOT_SIGN));

        if (interactive)
            printf("1) Correct\n2) Don't correct (Boot Sector invalid then)\n");
        else
            printf("  Auto-correcting it.\n");

        if (!interactive || get_key("12", "?") == '1') {
            b.boot_sign = CT_LE_W(BOOT_SIGN);
            fs_write(0, sizeof(b), &b);
        }
    }

    logical_sector_size = GET_UNALIGNED_W(b.sector_size);

    fs->cluster_size = b.sec_per_clus * logical_sector_size;
    if (!fs->cluster_size) {
        die("Cluster size is zero.");
    }

    if (b.nfats > 2) {
        printf("WARN: only 1 or 2 FATs are supported, %d FATs are not supported.\n", b.nfats);
    }

    fs->nfats = b.nfats;
    sectors = GET_UNALIGNED_W(b.sectors);
    total_sectors = sectors ? sectors : CF_LE_L(b.total_sect);
    if (verbose)
        printf("Checking we can access the last sector of the filesystem\n");

    /* Can't access last odd sector anyway, so round down */
    fs_test((off_t)((total_sectors & ~1) - 1) * (off_t)logical_sector_size,
            logical_sector_size);
    sec_per_fat = CF_LE_W(b.sec_per_fat) ?
        CF_LE_W(b.sec_per_fat) : CF_LE_L(b.fat32.sec_per_fat32);
    fs->fat_start = (off_t)CF_LE_W(b.reserved_cnt) * logical_sector_size;
    fs->root_start = ((off_t)CF_LE_W(b.reserved_cnt) + b.nfats * sec_per_fat) *
        logical_sector_size;
    fs->root_entries = GET_UNALIGNED_W(b.dir_entries);
    fs->data_start = fs->root_start +
        ROUND_TO_MULTIPLE(fs->root_entries << MSDOS_DIR_BITS, logical_sector_size);
    data_size = (off_t)total_sectors * logical_sector_size - fs->data_start;
    fs->clusters = data_size / fs->cluster_size;    /* total number of clusters */
    max_clus_num = fs->clusters + FAT_START_ENT;    /* maximum cluster no. */
    fs->root_cluster = 0;   /* indicates standard, pre-FAT32 root dir */
    fs->fsinfo_start = 0;   /* no FSINFO structure */
    fs->free_clusters = -1; /* unknown */

    if (!b.sec_per_fat && b.fat32.sec_per_fat32) {
        fs->fat_bits = 32;
        fs->root_cluster = CF_LE_L(b.fat32.root_cluster);
        if (!fs->root_cluster && fs->root_entries)
            /* M$ hasn't specified this, but it looks reasonable: If
             * root_cluster is 0 but there is a separate root dir
             * (root_entries != 0), we handle the root dir the old way. Give a
             * warning, but convertig to a root dir in a cluster chain seems
             * to complex for now... */
            printf("Warning: FAT32 root dir not in cluster chain! "
                    "Compability mode...\n");
        else if (!fs->root_cluster && !fs->root_entries) {
            die("No root directory!");
        }
        else if (fs->root_cluster && fs->root_entries)
            printf("Warning: FAT32 root dir is in a cluster chain, but "
                    "a separate root dir\n"
                    "  area is defined. Cannot fix this easily.\n");

        fs->backupboot_start = CF_LE_W(b.fat32.backup_boot) * logical_sector_size;
        check_backup_boot(fs, &b, logical_sector_size);

        read_fsinfo(fs, &b, logical_sector_size);
    }
    else if (!atari_format) {
        /* On real MS-DOS, a 16 bit FAT is used whenever there would be too
         * much clusers otherwise. */
        fs->fat_bits = (fs->clusters > MSDOS_FAT12) ? 16 : 12;
    }
    else {
        /* On Atari, things are more difficult: GEMDOS always uses 12bit FATs
         * on floppies, and always 16 bit on harddisks. */
        fs->fat_bits = 16; /* assume 16 bit FAT for now */

        /* If more clusters than fat entries in 16-bit fat, we assume
         * it's a real MSDOS FS with 12-bit fat. */
        if (fs->clusters + 2 > sec_per_fat * logical_sector_size * 8 / 16 ||
                /* if it's a floppy disk --> 12bit fat */
                device_no == 2 ||
                /* if it's a ramdisk or loopback device and has one of the usual
                 * floppy sizes -> 12bit FAT  */
                ((device_no == 1 || device_no == 7) &&
                 (total_sectors == 720 || total_sectors == 1440 ||
                  total_sectors == 2880)))
            fs->fat_bits = 12;
    }

    /* On FAT32, the high 4 bits of a FAT entry are reserved */
    fs->eff_fat_bits = (fs->fat_bits == 32) ? 28 : fs->fat_bits;
    fs->fat_size = sec_per_fat * logical_sector_size;

    fs->label = calloc(12, sizeof (__u8));
    if (fs->fat_bits == 12 || fs->fat_bits == 16) {
        vi = &b.oldfat.vi;
    } else if (fs->fat_bits == 32) {
        vi = &b.fat32.vi;
    } else {
        die("Can't find fat type.");
    }

    if (vi->extended_sig == MSDOS_EXT_SIGN)
        memmove(fs->label, vi->label, LEN_VOLUME_LABEL);
    else
        fs->label = NULL;

    fs->fat_state = vi->state;

    if (fs->clusters > ((unsigned long long)fs->fat_size * 8 / fs->fat_bits) - 2)
        die("File system has %d clusters but only space for %d FAT entries.",
                fs->clusters,
                ((unsigned long long)fs->fat_size * 8 / fs->fat_bits) - 2);

    if (!fs->root_entries && !fs->root_cluster) {
        die("Root directory has zero size");
    }

    if (fs->root_entries & (MSDOS_DPS - 1)) {
        die("Root directory (%d entries) doesn't span an integral number of "
                "sectors.", fs->root_entries);
    }

    if (logical_sector_size & (SECTOR_SIZE - 1)) {
        die("Logical sector size (%d bytes) is not a multiple of the physical "
                "sector size.", logical_sector_size);
    }

    if (verbose)
        dump_boot(fs, &b, logical_sector_size);
}

void clean_boot(DOS_FS *fs)
{
    if (fs->label)
        free_mem(fs->label);
}

/* Local Variables: */
/* tab-width: 8     */
/* End:             */
