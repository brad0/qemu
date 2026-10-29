/*
 * QEMU Guest Agent FreeBSD-specific command implementations
 *
 * Copyright (c) Virtuozzo International GmbH.
 *
 * Authors:
 *  Alexander Ivanov  <alexander.ivanov@virtuozzo.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "qga-qapi-commands.h"
#include "qapi/error.h"
#include "commands-common.h"
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <paths.h>
#include <ufs/ffs/fs.h>
#include <devstat.h>

static int ufssuspend_fd = -1;
static int ufssuspend_cnt;

int64_t qmp_guest_fsfreeze_do_freeze_list(bool has_mountpoints,
                                          strList *mountpoints,
                                          FsMountList mounts,
                                          Error **errp)
{
    int ret;
    strList *list;
    struct FsMount *mount;

    if (ufssuspend_fd != -1) {
        error_setg(errp, "filesystems have already frozen");
        return -1;
    }

    ufssuspend_cnt = 0;
    ufssuspend_fd = qemu_open(_PATH_UFSSUSPEND, O_RDWR, errp);
    if (ufssuspend_fd == -1) {
        return -1;
    }

    QTAILQ_FOREACH_REVERSE(mount, &mounts, next) {
        /*
         * To issue fsfreeze in the reverse order of mounts, check if the
         * mount is listed in the list here
         */
        if (has_mountpoints) {
            for (list = mountpoints; list; list = list->next) {
                if (g_str_equal(list->value, mount->dirname)) {
                    break;
                }
            }
            if (!list) {
                continue;
            }
        }

        /* Only UFS supports suspend */
        if (!g_str_equal(mount->devtype, "ufs")) {
            continue;
        }

        ret = ioctl(ufssuspend_fd, UFSSUSPEND, &mount->fsid);
        if (ret == -1) {
            /*
             * ioctl returns EBUSY for all the FS except the first one
             * that was suspended
             */
            if (errno == EBUSY) {
                continue;
            }
            error_setg_errno(errp, errno, "failed to freeze %s",
                             mount->dirname);
            goto error;
        }
        ufssuspend_cnt++;
    }
    return ufssuspend_cnt;
error:
    close(ufssuspend_fd);
    ufssuspend_fd = -1;
    return -1;
}

/*
 * We don't need to call UFSRESUME ioctl because all the frozen FS
 * are thawed on /dev/ufssuspend closing.
 */
int qmp_guest_fsfreeze_do_thaw(Error **errp)
{
    int ret = ufssuspend_cnt;
    ufssuspend_cnt = 0;
    if (ufssuspend_fd != -1) {
        close(ufssuspend_fd);
        ufssuspend_fd = -1;
    }
    return ret;
}

static uint64_t bintime_to_msec(const struct bintime *bt)
{
    return (uint64_t)bt->sec * 1000ULL + (((bt->frac >> 32) * 1000ULL) >> 32);
}

static GuestDiskStatsInfo *
guest_diskstats_info_new(const struct devstat *dev)
{
    GuestDiskStatsInfo *diskstatinfo;
    GuestDiskStats *diskstat;

    diskstatinfo = g_new0(GuestDiskStatsInfo, 1);
    diskstatinfo->name = g_strdup_printf("%s%d", dev->device_name,
                                         dev->unit_number);
    /*
     * devstat does not expose Linux-style major/minor numbers.  Report the
     * devstat device number and unit number in these mandatory QAPI fields.
     */
    diskstatinfo->major = dev->device_number;
    diskstatinfo->minor = dev->unit_number;

    diskstat = g_new0(GuestDiskStats, 1);
    diskstat->has_read_ios = true;
    diskstat->read_ios = dev->operations[DEVSTAT_READ];
    diskstat->has_read_sectors = true;
    diskstat->read_sectors = dev->bytes[DEVSTAT_READ] / BDRV_SECTOR_SIZE;
    diskstat->has_read_ticks = true;
    diskstat->read_ticks = bintime_to_msec(&dev->duration[DEVSTAT_READ]);

    diskstat->has_write_ios = true;
    diskstat->write_ios = dev->operations[DEVSTAT_WRITE];
    diskstat->has_write_sectors = true;
    diskstat->write_sectors = dev->bytes[DEVSTAT_WRITE] / BDRV_SECTOR_SIZE;
    diskstat->has_write_ticks = true;
    diskstat->write_ticks = bintime_to_msec(&dev->duration[DEVSTAT_WRITE]);

    diskstat->has_discard_ios = true;
    diskstat->discard_ios = dev->operations[DEVSTAT_FREE];
    diskstat->has_discard_sectors = true;
    diskstat->discard_sectors = dev->bytes[DEVSTAT_FREE] / BDRV_SECTOR_SIZE;
    diskstat->has_discard_ticks = true;
    diskstat->discard_ticks = bintime_to_msec(&dev->duration[DEVSTAT_FREE]);

    diskstat->has_ios_pgr = true;
    diskstat->ios_pgr = dev->start_count - dev->end_count;

    diskstat->has_total_ticks = true;
    diskstat->total_ticks = bintime_to_msec(&dev->busy_time);

    diskstatinfo->stats = diskstat;
    return diskstatinfo;
}

GuestDiskStatsInfoList *qmp_guest_get_diskstats(Error **errp)
{
    GuestDiskStatsInfoList *head = NULL, **tail = &head;
    struct devinfo dinfo = { 0 };
    struct statinfo stats = { .dinfo = &dinfo };
    struct device_selection *dev_select = NULL;
    struct devstat_match matches[] = {
        {
            .match_fields = DEVSTAT_MATCH_TYPE,
            .device_type = DEVSTAT_TYPE_DIRECT,
            .num_match_categories = 1,
        },
    };
    int num_selected = 0;
    int num_selections = 0;
    long select_generation = 0;
    int i;

    if (devstat_checkversion(NULL) == -1) {
        error_setg(errp, "%s", devstat_errbuf);
        return NULL;
    }

    if (devstat_getdevs(NULL, &stats) == -1) {
        error_setg(errp, "%s", devstat_errbuf);
        goto error;
    }

    if (devstat_selectdevs(&dev_select, &num_selected, &num_selections,
                           &select_generation, stats.dinfo->generation,
                           stats.dinfo->devices, stats.dinfo->numdevs,
                           matches, ARRAY_SIZE(matches), NULL, 0,
                           DS_SELECT_ONLY,
                           stats.dinfo->numdevs, 0) == -1) {
        error_setg(errp, "%s", devstat_errbuf);
        goto error;
    }

    for (i = 0; i < num_selections; i++) {
        const struct devstat *dev;

        if (dev_select[i].selected == 0) {
            continue;
        }

        dev = &stats.dinfo->devices[dev_select[i].position];
        QAPI_LIST_APPEND(tail, guest_diskstats_info_new(dev));
    }

    free(stats.dinfo->mem_ptr);
    free(dev_select);
    return head;

error:
    qapi_free_GuestDiskStatsInfoList(head);
    free(stats.dinfo->mem_ptr);
    free(dev_select);
    return NULL;
}
