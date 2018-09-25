/*
 * qemu_hotplug.c: QEMU device hotplug management
 *
 * Copyright (C) 2006-2016 Red Hat, Inc.
 * Copyright (C) 2006 Daniel P. Berrange
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library.  If not, see
 * <http://www.gnu.org/licenses/>.
 *
 * Author: Daniel P. Berrange <berrange@redhat.com>
 */


#include <config.h>

#include "qemu_hotplug.h"
#include "qemu_hotplugpriv.h"
#include "qemu_alias.h"
#include "qemu_capabilities.h"
#include "qemu_domain.h"
#include "qemu_domain_address.h"
#include "qemu_command.h"
#include "qemu_hostdev.h"
#include "qemu_interface.h"
#include "qemu_process.h"
#include "qemu_security.h"
#include "qemu_block.h"
#include "domain_audit.h"
#include "netdev_bandwidth_conf.h"
#include "domain_nwfilter.h"
#include "virlog.h"
#include "datatypes.h"
#include "virerror.h"
#include "viralloc.h"
#include "virpci.h"
#include "virfile.h"
#include "virprocess.h"
#include "qemu_cgroup.h"
#include "locking/domain_lock.h"
#include "virnetdev.h"
#include "virnetdevbridge.h"
#include "virnetdevtap.h"
#include "virnetdevopenvswitch.h"
#include "virnetdevmidonet.h"
#include "device_conf.h"
#include "virstoragefile.h"
#include "virstring.h"
#include "virtime.h"

#define VIR_FROM_THIS VIR_FROM_QEMU

VIR_LOG_INIT("qemu.qemu_hotplug");

#define CHANGE_MEDIA_TIMEOUT 5000

/* Wait up to 5 seconds for device removal to finish. */
unsigned long long qemuDomainRemoveDeviceWaitTime = 1000ull * 5;


/**
 * qemuHotplugPrepareDiskAccess:
 * @driver: qemu driver struct
 * @vm: domain object
 * @disk: disk to prepare
 * @overridesrc: Source different than @disk->src when necessary
 * @teardown: Teardown the disk instead of adding it to a vm
 *
 * Setup the locks, cgroups and security permissions on a disk of a VM.
 * If @overridesrc is specified the source struct is used instead of the
 * one present in @disk. If @teardown is true, then the labels and cgroups
 * are removed instead.
 *
 * Returns 0 on success and -1 on error. Reports libvirt error.
 */
static int
qemuHotplugPrepareDiskAccess(virQEMUDriverPtr driver,
                             virDomainObjPtr vm,
                             virDomainDiskDefPtr disk,
                             virStorageSourcePtr overridesrc,
                             bool teardown)
{
    virQEMUDriverConfigPtr cfg = virQEMUDriverGetConfig(driver);
    int ret = -1;
    virStorageSourcePtr origsrc = NULL;
    virErrorPtr orig_err = NULL;

    if (overridesrc) {
        origsrc = disk->src;
        disk->src = overridesrc;
    }

    /* just tear down the disk access */
    if (teardown) {
        virErrorPreserveLast(&orig_err);
        ret = 0;
        goto rollback_cgroup;
    }

    if (virDomainLockDiskAttach(driver->lockManager, cfg->uri,
                                vm, disk) < 0)
        goto cleanup;

    if (qemuDomainNamespaceSetupDisk(vm, disk->src) < 0)
        goto rollback_lock;

    if (qemuSecuritySetDiskLabel(driver, vm, disk) < 0)
        goto rollback_namespace;

    if (qemuSetupDiskCgroup(vm, disk) < 0)
        goto rollback_label;

    ret = 0;
    goto cleanup;

 rollback_cgroup:
    if (qemuTeardownDiskCgroup(vm, disk) < 0)
        VIR_WARN("Unable to tear down cgroup access on %s",
                 NULLSTR(virDomainDiskGetSource(disk)));
 rollback_label:
    if (qemuSecurityRestoreDiskLabel(driver, vm, disk) < 0)
        VIR_WARN("Unable to restore security label on %s",
                 NULLSTR(virDomainDiskGetSource(disk)));

 rollback_namespace:
    if (qemuDomainNamespaceTeardownDisk(vm, disk->src) < 0)
        VIR_WARN("Unable to remove /dev entry for %s",
                 NULLSTR(virDomainDiskGetSource(disk)));

 rollback_lock:
    if (virDomainLockDiskDetach(driver->lockManager, vm, disk) < 0)
        VIR_WARN("Unable to release lock on %s",
                 NULLSTR(virDomainDiskGetSource(disk)));

 cleanup:
    if (origsrc)
        disk->src = origsrc;

    virErrorRestore(&orig_err);

    virObjectUnref(cfg);

    return ret;
}


static int
qemuHotplugWaitForTrayEject(virDomainObjPtr vm,
                            virDomainDiskDefPtr disk)
{
    unsigned long long now;
    int rc;

    if (virTimeMillisNow(&now) < 0)
        return -1;

    while (disk->tray_status != VIR_DOMAIN_DISK_TRAY_OPEN) {
        if ((rc = virDomainObjWaitUntil(vm, now + CHANGE_MEDIA_TIMEOUT)) < 0)
            return -1;

        if (rc > 0) {
            /* the caller called qemuMonitorEjectMedia which usually reports an
             * error. Report the failure in an off-chance that it didn't. */
            if (virGetLastErrorCode() == VIR_ERR_OK) {
                virReportError(VIR_ERR_OPERATION_FAILED,
                               _("timed out waiting to open tray of '%s'"),
                               disk->dst);
            }
            return -1;
        }
    }

    return 0;
}


/**
 * qemuDomainChangeMediaLegacy:
 * @driver: qemu driver structure
 * @vm: domain definition
 * @disk: disk definition to change the source of
 * @newsrc: new disk source to change to
 * @force: force the change of media
 *
 * Change the media in an ejectable device to the one described by
 * @newsrc. This function also removes the old source from the
 * shared device table if appropriate. Note that newsrc is consumed
 * on success and the old source is freed on success.
 *
 * Returns 0 on success, -1 on error and reports libvirt error
 */
static int
qemuDomainChangeMediaLegacy(virQEMUDriverPtr driver,
                            virDomainObjPtr vm,
                            virDomainDiskDefPtr disk,
                            virStorageSourcePtr newsrc,
                            bool force)
{
    int ret = -1, rc;
    char *driveAlias = NULL;
    qemuDomainObjPrivatePtr priv = vm->privateData;
    qemuDomainDiskPrivatePtr diskPriv = QEMU_DOMAIN_DISK_PRIVATE(disk);
    const char *format = NULL;
    char *sourcestr = NULL;

    if (!disk->info.alias) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("missing disk device alias name for %s"), disk->dst);
        goto cleanup;
    }

    if (!(driveAlias = qemuAliasDiskDriveFromDisk(disk)))
        goto cleanup;

    qemuDomainObjEnterMonitor(driver, vm);
    rc = qemuMonitorEjectMedia(priv->mon, driveAlias, force);
    if (qemuDomainObjExitMonitor(driver, vm) < 0)
        goto cleanup;

    /* If the tray is present and tray change event is supported wait for it to open. */
    if (!force && diskPriv->tray &&
        virQEMUCapsGet(priv->qemuCaps, QEMU_CAPS_DEVICE_TRAY_MOVED)) {
        rc = qemuHotplugWaitForTrayEject(vm, disk);
        if (rc < 0)
            goto cleanup;

        /* re-issue ejection command to pop out the media */
        qemuDomainObjEnterMonitor(driver, vm);
        rc = qemuMonitorEjectMedia(priv->mon, driveAlias, false);
        if (qemuDomainObjExitMonitor(driver, vm) < 0 || rc < 0)
            goto cleanup;

    } else  {
        /* otherwise report possible errors from the attempt to eject the media*/
        if (rc < 0)
            goto cleanup;
    }

    if (!virStorageSourceIsEmpty(newsrc)) {
        if (qemuGetDriveSourceString(newsrc, NULL, &sourcestr) < 0)
            goto cleanup;

        if (virStorageSourceGetActualType(newsrc) != VIR_STORAGE_TYPE_DIR)
            format = virStorageFileFormatTypeToString(newsrc->format);

        qemuDomainObjEnterMonitor(driver, vm);
        rc = qemuMonitorChangeMedia(priv->mon,
                                    driveAlias,
                                    sourcestr,
                                    format);
        if (qemuDomainObjExitMonitor(driver, vm) < 0)
            goto cleanup;
    }

    if (rc < 0)
        goto cleanup;

    ret = 0;

 cleanup:
    VIR_FREE(driveAlias);
    VIR_FREE(sourcestr);
    return ret;
}


/**
 * qemuHotplugAttachManagedPR:
 * @driver: QEMU driver object
 * @vm: domain object
 * @src: new disk source to be attached to @vm
 * @asyncJob: asynchronous job identifier
 *
 * Checks if it's needed to start qemu-pr-helper and add the corresponding
 * pr-manager-helper object.
 *
 * Returns: 0 on success, -1 on error.
 */
static int
qemuHotplugAttachManagedPR(virQEMUDriverPtr driver,
                           virDomainObjPtr vm,
                           virStorageSourcePtr src,
                           qemuDomainAsyncJob asyncJob)
{
    qemuDomainObjPrivatePtr priv = vm->privateData;
    virJSONValuePtr props = NULL;
    bool daemonStarted = false;
    int ret = -1;
    int rc;

    if (priv->prDaemonRunning ||
        !virStorageSourceChainHasManagedPR(src))
        return 0;

    if (!(props = qemuBuildPRManagedManagerInfoProps(priv)))
        return -1;

    if (qemuProcessStartManagedPRDaemon(vm) < 0)
        goto cleanup;

    daemonStarted = true;

    if (qemuDomainObjEnterMonitorAsync(driver, vm, asyncJob) < 0)
        goto cleanup;

    rc = qemuMonitorAddObject(priv->mon, &props, NULL);

    if (qemuDomainObjExitMonitor(driver, vm) < 0 || rc < 0)
        goto cleanup;

    ret = 0;

 cleanup:
    if (ret < 0 && daemonStarted)
        qemuProcessKillManagedPRDaemon(vm);
    virJSONValueFree(props);
    return ret;
}


/**
 * qemuHotplugRemoveManagedPR:
 * @driver: QEMU driver object
 * @vm: domain object
 * @asyncJob: asynchronous job identifier
 *
 * Removes the managed PR object from @vm if the configuration does not require
 * it any more.
 */
static int
qemuHotplugRemoveManagedPR(virQEMUDriverPtr driver,
                           virDomainObjPtr vm,
                           qemuDomainAsyncJob asyncJob)
{
    qemuDomainObjPrivatePtr priv = vm->privateData;
    virErrorPtr orig_err;
    int ret = -1;

    if (!priv->prDaemonRunning ||
        virDomainDefHasManagedPR(vm->def))
        return 0;

    virErrorPreserveLast(&orig_err);

    if (qemuDomainObjEnterMonitorAsync(driver, vm, asyncJob) < 0)
        goto cleanup;
    ignore_value(qemuMonitorDelObject(priv->mon, qemuDomainGetManagedPRAlias()));
    if (qemuDomainObjExitMonitor(driver, vm) < 0)
        goto cleanup;

    qemuProcessKillManagedPRDaemon(vm);

    ret = 0;
 cleanup:
    virErrorRestore(&orig_err);
    return ret;
}


struct _qemuHotplugDiskSourceData {
    qemuBlockStorageSourceAttachDataPtr *backends;
    size_t nbackends;

    /* disk copy-on-read object */
    virJSONValuePtr corProps;
    char *corAlias;
};
typedef struct _qemuHotplugDiskSourceData qemuHotplugDiskSourceData;
typedef qemuHotplugDiskSourceData *qemuHotplugDiskSourceDataPtr;


static void
qemuHotplugDiskSourceDataFree(qemuHotplugDiskSourceDataPtr data)
{
    size_t i;

    if (!data)
        return;

    virJSONValueFree(data->corProps);
    VIR_FREE(data->corAlias);

    for (i = 0; i < data->nbackends; i++)
        qemuBlockStorageSourceAttachDataFree(data->backends[i]);

    VIR_FREE(data);
}


/**
 * qemuDomainRemoveDiskStorageSourcePrepareData:
 * @src: disk source structure
 * @driveAlias: Alias of the -drive backend, the pointer is always consumed
 *
 * Prepare qemuBlockStorageSourceAttachDataPtr for detaching a single source
 * from a VM. If @driveAlias is NULL -blockdev is assumed.
 */
static qemuBlockStorageSourceAttachDataPtr
qemuHotplugRemoveStorageSourcePrepareData(virStorageSourcePtr src,
                                          char *driveAlias)

{
    qemuDomainStorageSourcePrivatePtr srcpriv = QEMU_DOMAIN_STORAGE_SOURCE_PRIVATE(src);
    qemuBlockStorageSourceAttachDataPtr data;
    qemuBlockStorageSourceAttachDataPtr ret = NULL;

    if (VIR_ALLOC(data) < 0)
        goto cleanup;

    if (driveAlias) {
        VIR_STEAL_PTR(data->driveAlias, driveAlias);
        data->driveAdded = true;
    } else {
        data->formatNodeName = src->nodeformat;
        data->formatAttached = true;
        data->storageNodeName = src->nodestorage;
        data->storageAttached = true;
    }

    if (src->pr &&
        !virStoragePRDefIsManaged(src->pr) &&
        VIR_STRDUP(data->prmgrAlias, src->pr->mgralias) < 0)
        goto cleanup;

    if (VIR_STRDUP(data->tlsAlias, src->tlsAlias) < 0)
        goto cleanup;

    if (srcpriv) {
        if (srcpriv->secinfo &&
            srcpriv->secinfo->type == VIR_DOMAIN_SECRET_INFO_TYPE_AES &&
            VIR_STRDUP(data->authsecretAlias, srcpriv->secinfo->s.aes.alias) < 0)
            goto cleanup;

        if (srcpriv->encinfo &&
            srcpriv->encinfo->type == VIR_DOMAIN_SECRET_INFO_TYPE_AES &&
            VIR_STRDUP(data->encryptsecretAlias, srcpriv->encinfo->s.aes.alias) < 0)
            goto cleanup;
    }

    VIR_STEAL_PTR(ret, data);

 cleanup:
    VIR_FREE(driveAlias);
    qemuBlockStorageSourceAttachDataFree(data);
    return ret;
}


static qemuHotplugDiskSourceDataPtr
qemuHotplugDiskSourceRemovePrepare(virDomainDiskDefPtr disk,
                                   virStorageSourcePtr src,
                                   virQEMUCapsPtr qemuCaps)
{
    qemuDomainDiskPrivatePtr diskPriv = QEMU_DOMAIN_DISK_PRIVATE(disk);
    qemuBlockStorageSourceAttachDataPtr backend = NULL;
    qemuHotplugDiskSourceDataPtr data = NULL;
    qemuHotplugDiskSourceDataPtr ret = NULL;
    char *drivealias = NULL;
    virStorageSourcePtr n;

    if (VIR_ALLOC(data) < 0)
        return NULL;

    if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_BLOCKDEV)) {
        if (VIR_STRDUP(data->corAlias, diskPriv->nodeCopyOnRead) < 0)
            goto cleanup;

        for (n = src; virStorageSourceIsBacking(n); n = n->backingStore) {
            if (!(backend = qemuHotplugRemoveStorageSourcePrepareData(n, NULL)))
                goto cleanup;

            if (VIR_APPEND_ELEMENT(data->backends, data->nbackends, backend) < 0)
                goto cleanup;
        }
    } else {
        if (!(drivealias = qemuAliasDiskDriveFromDisk(disk)))
            goto cleanup;

        if (!(backend = qemuHotplugRemoveStorageSourcePrepareData(src,
                                                                  drivealias)))
            goto cleanup;

        if (VIR_APPEND_ELEMENT(data->backends, data->nbackends, backend) < 0)
            goto cleanup;
    }

    VIR_STEAL_PTR(ret, data);

 cleanup:
    qemuBlockStorageSourceAttachDataFree(backend);
    qemuHotplugDiskSourceDataFree(data);
    return ret;
}


/**
 * qemuHotplugDiskSourceAttachPrepare:
 * @disk: disk to generate attachment data for
 * @src: disk source to prepare attachment
 * @qemuCaps: capabilities of the qemu process
 *
 * Prepares and returns qemuHotplugDiskSourceData structure filled with all data
 * which will fully attach the source backend of the disk to a given VM.
 */
static qemuHotplugDiskSourceDataPtr
qemuHotplugDiskSourceAttachPrepare(virDomainDiskDefPtr disk,
                                   virStorageSourcePtr src,
                                   virQEMUCapsPtr qemuCaps)
{
    qemuBlockStorageSourceAttachDataPtr backend = NULL;
    qemuHotplugDiskSourceDataPtr data;
    qemuHotplugDiskSourceDataPtr ret = NULL;
    virStorageSourcePtr savesrc = NULL;
    virStorageSourcePtr n;

    if (VIR_ALLOC(data) < 0)
        return NULL;

    if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_BLOCKDEV)) {
        if (disk->copy_on_read == VIR_TRISTATE_SWITCH_ON &&
            !(data->corProps = qemuBlockStorageGetCopyOnReadProps(disk)))
            goto cleanup;

        for (n = src; virStorageSourceIsBacking(n); n = n->backingStore) {
            if (!(backend = qemuBlockStorageSourceAttachPrepareBlockdev(n)))
                goto cleanup;

            if (qemuBuildStorageSourceAttachPrepareCommon(n, backend, qemuCaps) < 0)
                goto cleanup;

            if (VIR_APPEND_ELEMENT(data->backends, data->nbackends, backend) < 0)
                goto cleanup;
        }
    } else {
        VIR_STEAL_PTR(savesrc, disk->src);
        disk->src = src;

        if (!(backend = qemuBuildStorageSourceAttachPrepareDrive(disk, qemuCaps)))
            goto cleanup;

        VIR_STEAL_PTR(disk->src, savesrc);

        if (qemuBuildStorageSourceAttachPrepareCommon(src, backend, qemuCaps) < 0)
            goto cleanup;

        if (VIR_APPEND_ELEMENT(data->backends, data->nbackends, backend) < 0)
            goto cleanup;
    }

    VIR_STEAL_PTR(ret, data);

 cleanup:
    if (savesrc)
        VIR_STEAL_PTR(disk->src, savesrc);

    qemuBlockStorageSourceAttachDataFree(backend);
    qemuHotplugDiskSourceDataFree(data);
    return ret;
}


/**
 * qemuHotplugDiskSourceAttach:
 * @mon: monitor object
 * @data: disk backend data object describing what to remove
 *
 * Attach a disk source backend with all relevant pieces. Caller must enter the
 * monitor context for @mon.
 */
static int
qemuHotplugDiskSourceAttach(qemuMonitorPtr mon,
                            qemuHotplugDiskSourceDataPtr data)
{
    size_t i;

    for (i = data->nbackends; i > 0; i--) {
        if (qemuBlockStorageSourceAttachApply(mon, data->backends[i - 1]) < 0)
            return -1;
    }

    if (data->corProps &&
        qemuMonitorAddObject(mon, &data->corProps, &data->corAlias) < 0)
        return -1;

    return 0;
}


/**
 * qemuHotplugDiskSourceRemove:
 * @mon: monitor object
 * @data: disk backend data object describing what to remove
 *
 * Remove a disk source backend with all relevant pieces. This function
 * preserves the error which was set prior to calling it. Caller must enter the
 * monitor context for @mon.
 */
static void
qemuHotplugDiskSourceRemove(qemuMonitorPtr mon,
                            qemuHotplugDiskSourceDataPtr data)

{
    size_t i;

    if (data->corAlias)
        ignore_value(qemuMonitorDelObject(mon, data->corAlias));

    for (i = 0; i < data->nbackends; i++)
        qemuBlockStorageSourceAttachRollback(mon, data->backends[i]);
}


/**
 * qemuDomainChangeMediaBlockdev:
 * @driver: qemu driver structure
 * @vm: domain definition
 * @disk: disk definition to change the source of
 * @oldsrc: old source definition
 * @newsrc: new disk source to change to
 * @force: force the change of media
 *
 * Change the media in an ejectable device to the one described by
 * @newsrc. This function also removes the old source from the
 * shared device table if appropriate. Note that newsrc is consumed
 * on success and the old source is freed on success.
 *
 * Returns 0 on success, -1 on error and reports libvirt error
 */
static int
qemuDomainChangeMediaBlockdev(virQEMUDriverPtr driver,
                              virDomainObjPtr vm,
                              virDomainDiskDefPtr disk,
                              virStorageSourcePtr oldsrc,
                              virStorageSourcePtr newsrc,
                              bool force)
{
    qemuDomainObjPrivatePtr priv = vm->privateData;
    qemuDomainDiskPrivatePtr diskPriv = QEMU_DOMAIN_DISK_PRIVATE(disk);
    qemuHotplugDiskSourceDataPtr newbackend = NULL;
    qemuHotplugDiskSourceDataPtr oldbackend = NULL;
    char *nodename = NULL;
    int rc;
    int ret = -1;

    if (!virStorageSourceIsEmpty(oldsrc) &&
        !(oldbackend = qemuHotplugDiskSourceRemovePrepare(disk, oldsrc,
                                                          priv->qemuCaps)))
        goto cleanup;

    if (!virStorageSourceIsEmpty(newsrc)) {
        if (!(newbackend = qemuHotplugDiskSourceAttachPrepare(disk, newsrc,
                                                              priv->qemuCaps)))
            goto cleanup;

        if (qemuDomainDiskGetBackendAlias(disk, priv->qemuCaps, &nodename) < 0)
            goto cleanup;
    }

    if (diskPriv->tray && disk->tray_status != VIR_DOMAIN_DISK_TRAY_OPEN) {
        qemuDomainObjEnterMonitor(driver, vm);
        rc = qemuMonitorBlockdevTrayOpen(priv->mon, diskPriv->qomName, force);
        if (qemuDomainObjExitMonitor(driver, vm) < 0 || rc < 0)
            goto cleanup;

        if (!force && qemuHotplugWaitForTrayEject(vm, disk) < 0)
            goto cleanup;
    }

    qemuDomainObjEnterMonitor(driver, vm);

    rc = qemuMonitorBlockdevMediumRemove(priv->mon, diskPriv->qomName);

    if (rc == 0 && oldbackend)
        qemuHotplugDiskSourceRemove(priv->mon, oldbackend);

    if (newbackend && nodename) {
        if (rc == 0)
            rc = qemuHotplugDiskSourceAttach(priv->mon, newbackend);

        if (rc == 0)
            rc = qemuMonitorBlockdevMediumInsert(priv->mon, diskPriv->qomName,
                                                 nodename);
    }

    if (rc == 0)
        rc = qemuMonitorBlockdevTrayClose(priv->mon, diskPriv->qomName);

    if (qemuDomainObjExitMonitor(driver, vm) < 0 || rc < 0)
        goto cleanup;

    ret = 0;

 cleanup:
    qemuHotplugDiskSourceDataFree(newbackend);
    qemuHotplugDiskSourceDataFree(oldbackend);
    VIR_FREE(nodename);
    return ret;
}


/**
 * qemuDomainChangeEjectableMedia:
 * @driver: qemu driver structure
 * @vm: domain definition
 * @disk: disk definition to change the source of
 * @newsrc: new disk source to change to
 * @force: force the change of media
 *
 * Change the media in an ejectable device to the one described by
 * @newsrc. This function also removes the old source from the
 * shared device table if appropriate. Note that newsrc is consumed
 * on success and the old source is freed on success.
 *
 * Returns 0 on success, -1 on error and reports libvirt error
 */
int
qemuDomainChangeEjectableMedia(virQEMUDriverPtr driver,
                               virDomainObjPtr vm,
                               virDomainDiskDefPtr disk,
                               virStorageSourcePtr newsrc,
                               bool force)
{
    virQEMUDriverConfigPtr cfg = virQEMUDriverGetConfig(driver);
    qemuDomainObjPrivatePtr priv = vm->privateData;
    virStorageSourcePtr oldsrc = disk->src;
    int ret = -1;
    int rc;

    disk->src = newsrc;

    if (qemuDomainPrepareDiskSource(disk, priv, cfg) < 0)
        goto cleanup;

    if (qemuHotplugPrepareDiskAccess(driver, vm, disk, newsrc, false) < 0)
        goto cleanup;

    if (qemuHotplugAttachManagedPR(driver, vm, newsrc, QEMU_ASYNC_JOB_NONE) < 0)
        goto cleanup;

    if (virQEMUCapsGet(priv->qemuCaps, QEMU_CAPS_BLOCKDEV))
        rc = qemuDomainChangeMediaBlockdev(driver, vm, disk, oldsrc, newsrc, force);
    else
        rc = qemuDomainChangeMediaLegacy(driver, vm, disk, newsrc, force);

    virDomainAuditDisk(vm, oldsrc, newsrc, "update", rc >= 0);

    if (rc < 0) {
        ignore_value(qemuHotplugPrepareDiskAccess(driver, vm, disk, newsrc, true));
        goto cleanup;
    }

    /* remove the old source from shared device list */
    disk->src = oldsrc;
    ignore_value(qemuRemoveSharedDisk(driver, disk, vm->def->name));
    ignore_value(qemuHotplugPrepareDiskAccess(driver, vm, disk, oldsrc, true));

    /* media was changed, so we can remove the old media definition now */
    virStorageSourceFree(oldsrc);
    oldsrc = NULL;
    disk->src = newsrc;

    ignore_value(qemuHotplugRemoveManagedPR(driver, vm, QEMU_ASYNC_JOB_NONE));

    ret = 0;

 cleanup:
    if (oldsrc)
        disk->src = oldsrc;

    virObjectUnref(cfg);
    return ret;
}


/**
 * qemuDomainAttachDiskGeneric:
 *
 * Attaches disk to a VM. This function aggregates common code for all bus types.
 * In cases when the VM crashed while adding the disk, -2 is returned. */
static int
qemuDomainAttachDiskGeneric(virQEMUDriverPtr driver,
                            virDomainObjPtr vm,
                            virDomainDiskDefPtr disk)
{
    int ret = -1;
    qemuDomainObjPrivatePtr priv = vm->privateData;
    qemuHotplugDiskSourceDataPtr diskdata = NULL;
    char *devstr = NULL;
    virQEMUDriverConfigPtr cfg = virQEMUDriverGetConfig(driver);

    if (qemuHotplugPrepareDiskAccess(driver, vm, disk, NULL, false) < 0)
        goto cleanup;

    if (qemuAssignDeviceDiskAlias(vm->def, disk, priv->qemuCaps) < 0)
        goto error;

    if (qemuDomainPrepareDiskSource(disk, priv, cfg) < 0)
        goto error;

    if (!(diskdata = qemuHotplugDiskSourceAttachPrepare(disk, disk->src,
                                                        priv->qemuCaps)))
        goto error;

    if (!(devstr = qemuBuildDiskDeviceStr(vm->def, disk, 0, priv->qemuCaps)))
        goto error;

    if (VIR_REALLOC_N(vm->def->disks, vm->def->ndisks + 1) < 0)
        goto error;

    if (qemuHotplugAttachManagedPR(driver, vm, disk->src, QEMU_ASYNC_JOB_NONE) < 0)
        goto error;

    qemuDomainObjEnterMonitor(driver, vm);

    if (qemuHotplugDiskSourceAttach(priv->mon, diskdata) < 0)
        goto exit_monitor;

    if (qemuMonitorAddDevice(priv->mon, devstr) < 0)
        goto exit_monitor;

    if (qemuDomainObjExitMonitor(driver, vm) < 0) {
        ret = -2;
        goto error;
    }

    virDomainAuditDisk(vm, NULL, disk->src, "attach", true);

    virDomainDiskInsertPreAlloced(vm->def, disk);
    ret = 0;

 cleanup:
    qemuHotplugDiskSourceDataFree(diskdata);
    qemuDomainSecretDiskDestroy(disk);
    VIR_FREE(devstr);
    virObjectUnref(cfg);
    return ret;

 exit_monitor:
    qemuHotplugDiskSourceRemove(priv->mon, diskdata);

    if (qemuDomainObjExitMonitor(driver, vm) < 0)
        ret = -2;
    if (qemuHotplugRemoveManagedPR(driver, vm, QEMU_ASYNC_JOB_NONE) < 0)
        ret = -2;

    virDomainAuditDisk(vm, NULL, disk->src, "attach", false);

 error:
    ignore_value(qemuHotplugPrepareDiskAccess(driver, vm, disk, NULL, true));
    goto cleanup;
}


static int
qemuDomainAttachVirtioDiskDevice(virQEMUDriverPtr driver,
                                 virDomainObjPtr vm,
                                 virDomainDiskDefPtr disk)
{
    virDomainDeviceDef dev = { VIR_DOMAIN_DEVICE_DISK, { .disk = disk } };
    bool releaseaddr = false;
    int rv;

    if (qemuDomainEnsureVirtioAddress(&releaseaddr, vm, &dev, disk->dst) < 0)
        return -1;

    if ((rv = qemuDomainAttachDiskGeneric(driver, vm, disk)) < 0) {
        if (rv == -1 && releaseaddr)
            qemuDomainReleaseDeviceAddress(vm, &disk->info, disk->dst);

        return -1;
    }

    return 0;
}


int qemuDomainAttachControllerDevice(virQEMUDriverPtr driver,
                                     virDomainObjPtr vm,
                                     virDomainControllerDefPtr controller)
{
    int ret = -1;
    const char* type = virDomainControllerTypeToString(controller->type);
    char *devstr = NULL;
    qemuDomainObjPrivatePtr priv = vm->privateData;
    virDomainDeviceDef dev = { VIR_DOMAIN_DEVICE_CONTROLLER,
                               { .controller = controller } };
    bool releaseaddr = false;

    if (controller->type != VIR_DOMAIN_CONTROLLER_TYPE_SCSI) {
        virReportError(VIR_ERR_OPERATION_UNSUPPORTED,
                       _("'%s' controller cannot be hot plugged."),
                       virDomainControllerTypeToString(controller->type));
        return -1;
    }

    /* default idx would normally be set by virDomainDefPostParse(),
     * which isn't called in the case of live attach of a single
     * device.
     */
    if (controller->idx == -1)
       controller->idx = virDomainControllerFindUnusedIndex(vm->def,
                                                            controller->type);

    if (virDomainControllerFind(vm->def, controller->type, controller->idx) >= 0) {
        virReportError(VIR_ERR_OPERATION_FAILED,
                       _("target %s:%d already exists"),
                       type, controller->idx);
        return -1;
    }

    if (qemuDomainEnsureVirtioAddress(&releaseaddr, vm, &dev, "controller") < 0)
        return -1;

    if (qemuAssignDeviceControllerAlias(vm->def, priv->qemuCaps, controller) < 0)
        goto cleanup;

    if (qemuBuildControllerDevStr(vm->def, controller, priv->qemuCaps, &devstr, NULL) < 0)
        goto cleanup;

    if (!devstr)
        goto cleanup;

    if (VIR_REALLOC_N(vm->def->controllers, vm->def->ncontrollers+1) < 0)
        goto cleanup;

    qemuDomainObjEnterMonitor(driver, vm);
    ret = qemuMonitorAddDevice(priv->mon, devstr);
    if (qemuDomainObjExitMonitor(driver, vm) < 0) {
        releaseaddr = false;
        ret = -1;
        goto cleanup;
    }

    if (ret == 0)
        virDomainControllerInsertPreAlloced(vm->def, controller);

 cleanup:
    if (ret != 0 && releaseaddr)
        qemuDomainReleaseDeviceAddress(vm, &controller->info, NULL);

    VIR_FREE(devstr);
    return ret;
}

static virDomainControllerDefPtr
qemuDomainFindOrCreateSCSIDiskController(virQEMUDriverPtr driver,
                                         virDomainObjPtr vm,
                                         int controller)
{
    size_t i;
    virDomainControllerDefPtr cont;
    qemuDomainObjPrivatePtr priv = vm->privateData;
    int model = -1;

    for (i = 0; i < vm->def->ncontrollers; i++) {
        cont = vm->def->controllers[i];

        if (cont->type != VIR_DOMAIN_CONTROLLER_TYPE_SCSI)
            continue;

        if (cont->idx == controller)
            return cont;

        /* Because virDomainHostdevAssignAddress called during
         * virDomainHostdevDefPostParse cannot add a new controller
         * it will assign a controller index to a controller that doesn't
         * exist leaving this code to perform the magic of adding the
         * controller. Because that code would be attempting to add a
         * SCSI disk to an existing controller, let's save the model
         * of the "last" SCSI controller we find so that if we end up
         * creating a controller below it uses the same controller model. */
        model = cont->model;
    }

    /* No SCSI controller present, for backward compatibility we
     * now hotplug a controller */
    if (VIR_ALLOC(cont) < 0)
        return NULL;
    cont->type = VIR_DOMAIN_CONTROLLER_TYPE_SCSI;
    cont->idx = controller;
    if (model == VIR_DOMAIN_CONTROLLER_MODEL_SCSI_DEFAULT)
        cont->model = qemuDomainGetSCSIControllerModel(vm->def, cont, priv->qemuCaps);
    else
        cont->model = model;

    VIR_INFO("No SCSI controller present, hotplugging one model=%s",
             virDomainControllerModelSCSITypeToString(cont->model));
    if (qemuDomainAttachControllerDevice(driver, vm, cont) < 0) {
        VIR_FREE(cont);
        return NULL;
    }

    if (!virDomainObjIsActive(vm)) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("guest unexpectedly quit"));
        /* cont doesn't need freeing here, since the reference
         * now held in def->controllers */
        return NULL;
    }

    return cont;
}


static int
qemuDomainAttachSCSIDisk(virQEMUDriverPtr driver,
                         virDomainObjPtr vm,
                         virDomainDiskDefPtr disk)
{
    size_t i;

    /* We should have an address already, so make sure */
    if (disk->info.type != VIR_DOMAIN_DEVICE_ADDRESS_TYPE_DRIVE) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("unexpected disk address type %s"),
                       virDomainDeviceAddressTypeToString(disk->info.type));
        return -1;
    }

    /* Let's make sure the disk has a controller defined and loaded before
     * trying to add it. The controller used by the disk must exist before a
     * qemu command line string is generated.
     *
     * Ensure that the given controller and all controllers with a smaller index
     * exist; there must not be any missing index in between.
     */
    for (i = 0; i <= disk->info.addr.drive.controller; i++) {
        if (!qemuDomainFindOrCreateSCSIDiskController(driver, vm, i))
            return -1;
    }

    if (qemuDomainAttachDiskGeneric(driver, vm, disk) < 0)
        return -1;

    return 0;
}


static int
qemuDomainAttachUSBMassStorageDevice(virQEMUDriverPtr driver,
                                     virDomainObjPtr vm,
                                     virDomainDiskDefPtr disk)
{
    qemuDomainObjPrivatePtr priv = vm->privateData;

    if (virDomainUSBAddressEnsure(priv->usbaddrs, &disk->info) < 0)
        return -1;

    if (qemuDomainAttachDiskGeneric(driver, vm, disk) < 0) {
        virDomainUSBAddressRelease(priv->usbaddrs, &disk->info);
        return -1;
    }

    return 0;
}


static int
qemuDomainAttachDeviceDiskLiveInternal(virQEMUDriverPtr driver,
                                       virDomainObjPtr vm,
                                       virDomainDeviceDefPtr dev)
{
    size_t i;
    virDomainDiskDefPtr disk = dev->data.disk;
    virDomainDiskDefPtr orig_disk = NULL;
    int ret = -1;

    if (virDomainDiskTranslateSourcePool(disk) < 0)
        goto cleanup;

    if (qemuAddSharedDevice(driver, dev, vm->def->name) < 0)
        goto cleanup;

    if (qemuSetUnprivSGIO(dev) < 0)
        goto cleanup;

    if (qemuDomainDetermineDiskChain(driver, vm, disk, true) < 0)
        goto cleanup;

    switch ((virDomainDiskDevice) disk->device)  {
    case VIR_DOMAIN_DISK_DEVICE_CDROM:
    case VIR_DOMAIN_DISK_DEVICE_FLOPPY:
        if (!(orig_disk = virDomainDiskFindByBusAndDst(vm->def,
                                                       disk->bus, disk->dst))) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("No device with bus '%s' and target '%s'. "
                             "cdrom and floppy device hotplug isn't supported "
                             "by libvirt"),
                           virDomainDiskBusTypeToString(disk->bus),
                           disk->dst);
            goto cleanup;
        }

        if (qemuDomainChangeEjectableMedia(driver, vm, orig_disk,
                                           disk->src, false) < 0)
            goto cleanup;

        disk->src = NULL;
        ret = 0;
        break;

    case VIR_DOMAIN_DISK_DEVICE_DISK:
    case VIR_DOMAIN_DISK_DEVICE_LUN:
        for (i = 0; i < vm->def->ndisks; i++) {
            if (virDomainDiskDefCheckDuplicateInfo(vm->def->disks[i], disk) < 0)
                goto cleanup;
        }

        switch ((virDomainDiskBus) disk->bus) {
        case VIR_DOMAIN_DISK_BUS_USB:
            if (disk->device == VIR_DOMAIN_DISK_DEVICE_LUN) {
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                               _("disk device='lun' is not supported for usb bus"));
                break;
            }
            ret = qemuDomainAttachUSBMassStorageDevice(driver, vm, disk);
            break;

        case VIR_DOMAIN_DISK_BUS_VIRTIO:
            ret = qemuDomainAttachVirtioDiskDevice(driver, vm, disk);
            break;

        case VIR_DOMAIN_DISK_BUS_SCSI:
            ret = qemuDomainAttachSCSIDisk(driver, vm, disk);
            break;

        case VIR_DOMAIN_DISK_BUS_IDE:
        case VIR_DOMAIN_DISK_BUS_FDC:
        case VIR_DOMAIN_DISK_BUS_XEN:
        case VIR_DOMAIN_DISK_BUS_UML:
        case VIR_DOMAIN_DISK_BUS_SATA:
        case VIR_DOMAIN_DISK_BUS_SD:
            /* Note that SD card hotplug support should be added only once
             * they support '-device' (don't require -drive only).
             * See also: qemuDiskBusNeedsDriveArg */
        case VIR_DOMAIN_DISK_BUS_LAST:
            virReportError(VIR_ERR_OPERATION_UNSUPPORTED,
                           _("disk bus '%s' cannot be hotplugged."),
                           virDomainDiskBusTypeToString(disk->bus));
        }
        break;

    case VIR_DOMAIN_DISK_DEVICE_LAST:
        break;
    }

 cleanup:
    if (ret != 0)
        ignore_value(qemuRemoveSharedDevice(driver, dev, vm->def->name));
    return ret;
}


/**
 * qemuDomainAttachDeviceDiskLive:
 * @driver: qemu driver struct
 * @vm: domain object
 * @dev: device to attach (expected type is DISK)
 *
 * Attach a new disk or in case of cdroms/floppies change the media in the drive.
 * This function handles all the necessary steps to attach a new storage source
 * to the VM.
 */
int
qemuDomainAttachDeviceDiskLive(virQEMUDriverPtr driver,
                               virDomainObjPtr vm,
                               virDomainDeviceDefPtr dev)
{
    return qemuDomainAttachDeviceDiskLiveInternal(driver, vm, dev);
}


static void
qemuDomainNetDeviceVportRemove(virDomainNetDefPtr net)
{
    virNetDevVPortProfilePtr vport = virDomainNetGetActualVirtPortProfile(net);
    const char *brname;

    if (!vport)
        return;

    if (vport->virtPortType == VIR_NETDEV_VPORT_PROFILE_MIDONET) {
        ignore_value(virNetDevMidonetUnbindPort(vport));
    } else if (vport->virtPortType == VIR_NETDEV_VPORT_PROFILE_OPENVSWITCH) {
        brname = virDomainNetGetActualBridgeName(net);
        ignore_value(virNetDevOpenvswitchRemovePort(brname, net->ifname));
    }
}


int
qemuDomainAttachNetDevice(virQEMUDriverPtr driver,
                          virDomainObjPtr vm,
                          virDomainNetDefPtr net)
{
    qemuDomainObjPrivatePtr priv = vm->privateData;
    virDomainDeviceDef dev = { VIR_DOMAIN_DEVICE_NET, { .net = net } };
    virErrorPtr originalError = NULL;
    char **tapfdName = NULL;
    int *tapfd = NULL;
    size_t tapfdSize = 0;
    char **vhostfdName = NULL;
    int *vhostfd = NULL;
    size_t vhostfdSize = 0;
    size_t queueSize = 0;
    char *nicstr = NULL;
    char *netstr = NULL;
    int ret = -1;
    bool releaseaddr = false;
    bool iface_connected = false;
    virDomainNetType actualType;
    virNetDevBandwidthPtr actualBandwidth;
    virQEMUDriverConfigPtr cfg = virQEMUDriverGetConfig(driver);
    virDomainCCWAddressSetPtr ccwaddrs = NULL;
    size_t i;
    char *charDevAlias = NULL;
    bool charDevPlugged = false;
    bool netdevPlugged = false;
    char *netdev_name;

    /* preallocate new slot for device */
    if (VIR_REALLOC_N(vm->def->nets, vm->def->nnets + 1) < 0)
        goto cleanup;

    /* If appropriate, grab a physical device from the configured
     * network's pool of devices, or resolve bridge device name
     * to the one defined in the network definition.
     */
    if (virDomainNetAllocateActualDevice(vm->def, net) < 0)
        goto cleanup;

    actualType = virDomainNetGetActualType(net);

    /* Currently only TAP/macvtap devices supports multiqueue. */
    if (net->driver.virtio.queues > 0 &&
        !(actualType == VIR_DOMAIN_NET_TYPE_NETWORK ||
          actualType == VIR_DOMAIN_NET_TYPE_BRIDGE ||
          actualType == VIR_DOMAIN_NET_TYPE_DIRECT ||
          actualType == VIR_DOMAIN_NET_TYPE_ETHERNET ||
          actualType == VIR_DOMAIN_NET_TYPE_VHOSTUSER)) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("Multiqueue network is not supported for: %s"),
                       virDomainNetTypeToString(actualType));
        return -1;
    }

    /* and only TAP devices support nwfilter rules */
    if (net->filter &&
        !(actualType == VIR_DOMAIN_NET_TYPE_NETWORK ||
          actualType == VIR_DOMAIN_NET_TYPE_BRIDGE ||
          actualType == VIR_DOMAIN_NET_TYPE_ETHERNET)) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("filterref is not supported for "
                         "network interfaces of type %s"),
                       virDomainNetTypeToString(actualType));
        return -1;
    }

    if (qemuAssignDeviceNetAlias(vm->def, net, -1) < 0)
        goto cleanup;

    switch (actualType) {
    case VIR_DOMAIN_NET_TYPE_BRIDGE:
    case VIR_DOMAIN_NET_TYPE_NETWORK:
        tapfdSize = vhostfdSize = net->driver.virtio.queues;
        if (!tapfdSize)
            tapfdSize = vhostfdSize = 1;
        queueSize = tapfdSize;
        if (VIR_ALLOC_N(tapfd, tapfdSize) < 0)
            goto cleanup;
        memset(tapfd, -1, sizeof(*tapfd) * tapfdSize);
        if (VIR_ALLOC_N(vhostfd, vhostfdSize) < 0)
            goto cleanup;
        memset(vhostfd, -1, sizeof(*vhostfd) * vhostfdSize);
        if (qemuInterfaceBridgeConnect(vm->def, driver, net,
                                       tapfd, &tapfdSize) < 0)
            goto cleanup;
        iface_connected = true;
        if (qemuInterfaceOpenVhostNet(vm->def, net, vhostfd, &vhostfdSize) < 0)
            goto cleanup;
        break;

    case VIR_DOMAIN_NET_TYPE_DIRECT:
        tapfdSize = vhostfdSize = net->driver.virtio.queues;
        if (!tapfdSize)
            tapfdSize = vhostfdSize = 1;
        queueSize = tapfdSize;
        if (VIR_ALLOC_N(tapfd, tapfdSize) < 0)
            goto cleanup;
        memset(tapfd, -1, sizeof(*tapfd) * tapfdSize);
        if (VIR_ALLOC_N(vhostfd, vhostfdSize) < 0)
            goto cleanup;
        memset(vhostfd, -1, sizeof(*vhostfd) * vhostfdSize);
        if (qemuInterfaceDirectConnect(vm->def, driver, net,
                                       tapfd, tapfdSize,
                                       VIR_NETDEV_VPORT_PROFILE_OP_CREATE) < 0)
            goto cleanup;
        iface_connected = true;
        if (qemuInterfaceOpenVhostNet(vm->def, net, vhostfd, &vhostfdSize) < 0)
            goto cleanup;
        break;

    case VIR_DOMAIN_NET_TYPE_ETHERNET:
        tapfdSize = vhostfdSize = net->driver.virtio.queues;
        if (!tapfdSize)
            tapfdSize = vhostfdSize = 1;
        queueSize = tapfdSize;
        if (VIR_ALLOC_N(tapfd, tapfdSize) < 0)
            goto cleanup;
        memset(tapfd, -1, sizeof(*tapfd) * tapfdSize);
        if (VIR_ALLOC_N(vhostfd, vhostfdSize) < 0)
            goto cleanup;
        memset(vhostfd, -1, sizeof(*vhostfd) * vhostfdSize);
        if (qemuInterfaceEthernetConnect(vm->def, driver, net,
                                         tapfd, tapfdSize) < 0)
            goto cleanup;
        iface_connected = true;
        if (qemuInterfaceOpenVhostNet(vm->def, net, vhostfd, &vhostfdSize) < 0)
            goto cleanup;
        break;

    case VIR_DOMAIN_NET_TYPE_HOSTDEV:
        /* This is really a "smart hostdev", so it should be attached
         * as a hostdev (the hostdev code will reach over into the
         * netdev-specific code as appropriate), then also added to
         * the nets list (see cleanup:) if successful.
         */
        ret = qemuDomainAttachHostDevice(driver, vm,
                                         virDomainNetGetActualHostdev(net));
        goto cleanup;
        break;

    case VIR_DOMAIN_NET_TYPE_VHOSTUSER:
        queueSize = net->driver.virtio.queues;
        if (!queueSize)
            queueSize = 1;
        if (!qemuDomainSupportsNicdev(vm->def, net)) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           "%s", _("Nicdev support unavailable"));
            goto cleanup;
        }

        if (!(charDevAlias = qemuAliasChardevFromDevAlias(net->info.alias)))
            goto cleanup;

        if (virNetDevOpenvswitchGetVhostuserIfname(net->data.vhostuser->data.nix.path,
                                                   &net->ifname) < 0)
            goto cleanup;

        break;

    case VIR_DOMAIN_NET_TYPE_USER:
        /* No preparation needed. */
        break;

    case VIR_DOMAIN_NET_TYPE_SERVER:
    case VIR_DOMAIN_NET_TYPE_CLIENT:
    case VIR_DOMAIN_NET_TYPE_MCAST:
    case VIR_DOMAIN_NET_TYPE_INTERNAL:
    case VIR_DOMAIN_NET_TYPE_UDP:
    case VIR_DOMAIN_NET_TYPE_LAST:
        virReportError(VIR_ERR_OPERATION_UNSUPPORTED,
                       _("hotplug of interface type of %s is not implemented yet"),
                       virDomainNetTypeToString(actualType));
        goto cleanup;
    }

    /* Set device online immediately */
    if (qemuInterfaceStartDevice(net) < 0)
        goto cleanup;

    /* Set bandwidth or warn if requested and not supported. */
    actualBandwidth = virDomainNetGetActualBandwidth(net);
    if (actualBandwidth) {
        if (virNetDevSupportBandwidth(actualType)) {
            if (virNetDevBandwidthSet(net->ifname, actualBandwidth, false,
                                      !virDomainNetTypeSharesHostView(net)) < 0)
                goto cleanup;
        } else {
            VIR_WARN("setting bandwidth on interfaces of "
                     "type '%s' is not implemented yet",
                     virDomainNetTypeToString(actualType));
        }
    }

    if (net->mtu &&
        virNetDevSetMTU(net->ifname, net->mtu) < 0)
        goto cleanup;

    for (i = 0; i < tapfdSize; i++) {
        if (qemuSecuritySetTapFDLabel(driver->securityManager,
                                      vm->def, tapfd[i]) < 0)
            goto cleanup;
    }

    if (qemuDomainIsS390CCW(vm->def) &&
        virQEMUCapsGet(priv->qemuCaps, QEMU_CAPS_CCW)) {
        net->info.type = VIR_DOMAIN_DEVICE_ADDRESS_TYPE_CCW;
        if (!(ccwaddrs = virDomainCCWAddressSetCreateFromDomain(vm->def)))
            goto cleanup;
        if (virDomainCCWAddressAssign(&net->info, ccwaddrs,
                                      !net->info.addr.ccw.assigned) < 0)
            goto cleanup;
    } else if (virQEMUCapsGet(priv->qemuCaps, QEMU_CAPS_VIRTIO_S390)) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("virtio-s390 net device cannot be hotplugged."));
        goto cleanup;
    } else if (qemuDomainEnsurePCIAddress(vm, &dev, driver) < 0) {
        goto cleanup;
    }

    releaseaddr = true;

    if (VIR_ALLOC_N(tapfdName, tapfdSize) < 0 ||
        VIR_ALLOC_N(vhostfdName, vhostfdSize) < 0)
        goto cleanup;

    for (i = 0; i < tapfdSize; i++) {
        if (virAsprintf(&tapfdName[i], "fd-%s%zu", net->info.alias, i) < 0)
            goto cleanup;
    }

    for (i = 0; i < vhostfdSize; i++) {
        if (virAsprintf(&vhostfdName[i], "vhostfd-%s%zu", net->info.alias, i) < 0)
            goto cleanup;
    }

    if (!(netstr = qemuBuildHostNetStr(net, driver,
                                       tapfdName, tapfdSize,
                                       vhostfdName, vhostfdSize)))
        goto cleanup;

    qemuDomainObjEnterMonitor(driver, vm);

    if (actualType == VIR_DOMAIN_NET_TYPE_VHOSTUSER) {
        if (qemuMonitorAttachCharDev(priv->mon, charDevAlias, net->data.vhostuser) < 0) {
            ignore_value(qemuDomainObjExitMonitor(driver, vm));
            virDomainAuditNet(vm, NULL, net, "attach", false);
            goto cleanup;
        }
        charDevPlugged = true;
    }

    if (qemuMonitorAddNetdev(priv->mon, netstr,
                             tapfd, tapfdName, tapfdSize,
                             vhostfd, vhostfdName, vhostfdSize) < 0) {
        ignore_value(qemuDomainObjExitMonitor(driver, vm));
        virDomainAuditNet(vm, NULL, net, "attach", false);
        goto try_remove;
    }
    netdevPlugged = true;

    if (qemuDomainObjExitMonitor(driver, vm) < 0)
        goto cleanup;

    for (i = 0; i < tapfdSize; i++)
        VIR_FORCE_CLOSE(tapfd[i]);
    for (i = 0; i < vhostfdSize; i++)
        VIR_FORCE_CLOSE(vhostfd[i]);

    if (!(nicstr = qemuBuildNicDevStr(vm->def, net, 0,
                                      queueSize, priv->qemuCaps)))
        goto try_remove;

    qemuDomainObjEnterMonitor(driver, vm);
    if (qemuMonitorAddDevice(priv->mon, nicstr) < 0) {
        ignore_value(qemuDomainObjExitMonitor(driver, vm));
        virDomainAuditNet(vm, NULL, net, "attach", false);
        goto try_remove;
    }
    if (qemuDomainObjExitMonitor(driver, vm) < 0)
        goto cleanup;

    /* set link state */
    if (net->linkstate == VIR_DOMAIN_NET_INTERFACE_LINK_STATE_DOWN) {
        if (!net->info.alias) {
            virReportError(VIR_ERR_OPERATION_FAILED, "%s",
                           _("device alias not found: cannot set link state to down"));
        } else {
            qemuDomainObjEnterMonitor(driver, vm);

            if (qemuMonitorSetLink(priv->mon, net->info.alias, VIR_DOMAIN_NET_INTERFACE_LINK_STATE_DOWN) < 0) {
                ignore_value(qemuDomainObjExitMonitor(driver, vm));
                virDomainAuditNet(vm, NULL, net, "attach", false);
                goto try_remove;
            }

            if (qemuDomainObjExitMonitor(driver, vm) < 0)
                goto cleanup;
        }
        /* link set to down */
    }

    virDomainAuditNet(vm, NULL, net, "attach", true);

    ret = 0;

 cleanup:
    if (!ret) {
        vm->def->nets[vm->def->nnets++] = net;
    } else {
        if (releaseaddr)
            qemuDomainReleaseDeviceAddress(vm, &net->info, NULL);

        if (iface_connected) {
            virErrorPreserveLast(&originalError);
            virDomainConfNWFilterTeardown(net);
            virErrorRestore(&originalError);

            if (virDomainNetGetActualType(net) == VIR_DOMAIN_NET_TYPE_DIRECT) {
                ignore_value(virNetDevMacVLanDeleteWithVPortProfile(
                                 net->ifname, &net->mac,
                                 virDomainNetGetActualDirectDev(net),
                                 virDomainNetGetActualDirectMode(net),
                                 virDomainNetGetActualVirtPortProfile(net),
                                 cfg->stateDir));
            }

            qemuDomainNetDeviceVportRemove(net);
        }

        virDomainNetRemoveHostdev(vm->def, net);

        virDomainNetReleaseActualDevice(vm->def, net);
    }

    VIR_FREE(nicstr);
    VIR_FREE(netstr);
    for (i = 0; tapfd && i < tapfdSize; i++) {
        VIR_FORCE_CLOSE(tapfd[i]);
        if (tapfdName)
            VIR_FREE(tapfdName[i]);
    }
    VIR_FREE(tapfd);
    VIR_FREE(tapfdName);
    for (i = 0; vhostfd && i < vhostfdSize; i++) {
        VIR_FORCE_CLOSE(vhostfd[i]);
        if (vhostfdName)
            VIR_FREE(vhostfdName[i]);
    }
    VIR_FREE(vhostfd);
    VIR_FREE(vhostfdName);
    VIR_FREE(charDevAlias);
    virObjectUnref(cfg);
    virDomainCCWAddressSetFree(ccwaddrs);

    return ret;

 try_remove:
    if (!virDomainObjIsActive(vm))
        goto cleanup;

    virErrorPreserveLast(&originalError);
    if (virAsprintf(&netdev_name, "host%s", net->info.alias) >= 0) {
        qemuDomainObjEnterMonitor(driver, vm);
        if (charDevPlugged &&
            qemuMonitorDetachCharDev(priv->mon, charDevAlias) < 0)
            VIR_WARN("Failed to remove associated chardev %s", charDevAlias);
        if (netdevPlugged &&
            qemuMonitorRemoveNetdev(priv->mon, netdev_name) < 0)
            VIR_WARN("Failed to remove network backend for netdev %s",
                     netdev_name);
        ignore_value(qemuDomainObjExitMonitor(driver, vm));
        VIR_FREE(netdev_name);
    }
    virErrorRestore(&originalError);
    goto cleanup;
}


static int
qemuDomainAttachHostPCIDevice(virQEMUDriverPtr driver,
                              virDomainObjPtr vm,
                              virDomainHostdevDefPtr hostdev)
{
    qemuDomainObjPrivatePtr priv = vm->privateData;
    virDomainDeviceDef dev = { VIR_DOMAIN_DEVICE_HOSTDEV,
                               { .hostdev = hostdev } };
    virDomainDeviceInfoPtr info = hostdev->info;
    int ret;
    char *devstr = NULL;
    int configfd = -1;
    char *configfd_name = NULL;
    bool releaseaddr = false;
    bool teardowncgroup = false;
    bool teardownlabel = false;
    bool teardowndevice = false;
    int backend;
    virQEMUDriverConfigPtr cfg = virQEMUDriverGetConfig(driver);
    unsigned int flags = 0;

    if (VIR_REALLOC_N(vm->def->hostdevs, vm->def->nhostdevs + 1) < 0)
        goto cleanup;

    if (!cfg->relaxedACS)
        flags |= VIR_HOSTDEV_STRICT_ACS_CHECK;
    if (qemuHostdevPreparePCIDevices(driver, vm->def->name, vm->def->uuid,
                                     &hostdev, 1, priv->qemuCaps, flags) < 0)
        goto cleanup;

    /* this could have been changed by qemuHostdevPreparePCIDevices */
    backend = hostdev->source.subsys.u.pci.backend;

    switch ((virDomainHostdevSubsysPCIBackendType)backend) {
    case VIR_DOMAIN_HOSTDEV_PCI_BACKEND_VFIO:
        if (!virQEMUCapsGet(priv->qemuCaps, QEMU_CAPS_DEVICE_VFIO_PCI)) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("VFIO PCI device assignment is not "
                             "supported by this version of qemu"));
            goto error;
        }
        break;

    case VIR_DOMAIN_HOSTDEV_PCI_BACKEND_DEFAULT:
    case VIR_DOMAIN_HOSTDEV_PCI_BACKEND_KVM:
        break;

    case VIR_DOMAIN_HOSTDEV_PCI_BACKEND_XEN:
    case VIR_DOMAIN_HOSTDEV_PCI_BACKEND_TYPE_LAST:
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("QEMU does not support device assignment mode '%s'"),
                       virDomainHostdevSubsysPCIBackendTypeToString(backend));
        goto error;
        break;
    }

    /* Temporarily add the hostdev to the domain definition. This is needed
     * because qemuDomainAdjustMaxMemLock() requires the hostdev to be already
     * part of the domain definition, but other functions like
     * qemuAssignDeviceHostdevAlias() used below expect it *not* to be there.
     * A better way to handle this would be nice */
    vm->def->hostdevs[vm->def->nhostdevs++] = hostdev;
    if (qemuDomainAdjustMaxMemLock(vm) < 0) {
        vm->def->hostdevs[--(vm->def->nhostdevs)] = NULL;
        goto error;
    }
    vm->def->hostdevs[--(vm->def->nhostdevs)] = NULL;

    if (qemuDomainNamespaceSetupHostdev(vm, hostdev) < 0)
        goto error;
    teardowndevice = true;

    if (qemuSetupHostdevCgroup(vm, hostdev) < 0)
        goto error;
    teardowncgroup = true;

    if (qemuSecuritySetHostdevLabel(driver, vm, hostdev) < 0)
        goto error;
    if (backend != VIR_DOMAIN_HOSTDEV_PCI_BACKEND_VFIO)
        teardownlabel = true;

    if (qemuAssignDeviceHostdevAlias(vm->def, &info->alias, -1) < 0)
        goto error;

    if (qemuDomainIsPSeries(vm->def)) {
        /* Isolation groups are only relevant for pSeries guests */
        if (qemuDomainFillDeviceIsolationGroup(vm->def, &dev) < 0)
            goto error;
    }

    if (qemuDomainEnsurePCIAddress(vm, &dev, driver) < 0)
        goto error;
    releaseaddr = true;
    if (backend != VIR_DOMAIN_HOSTDEV_PCI_BACKEND_VFIO) {
        configfd = qemuOpenPCIConfig(hostdev);
        if (configfd >= 0) {
            if (virAsprintf(&configfd_name, "fd-%s", info->alias) < 0)
                goto error;
        }
    }

    if (!virDomainObjIsActive(vm)) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("guest unexpectedly quit during hotplug"));
        goto error;
    }

    if (!(devstr = qemuBuildPCIHostdevDevStr(vm->def, hostdev, 0,
                                             configfd_name, priv->qemuCaps)))
        goto error;

    qemuDomainObjEnterMonitor(driver, vm);
    ret = qemuMonitorAddDeviceWithFd(priv->mon, devstr,
                                     configfd, configfd_name);
    if (qemuDomainObjExitMonitor(driver, vm) < 0)
        goto error;

    virDomainAuditHostdev(vm, hostdev, "attach", ret == 0);
    if (ret < 0)
        goto error;

    vm->def->hostdevs[vm->def->nhostdevs++] = hostdev;

    VIR_FREE(devstr);
    VIR_FREE(configfd_name);
    VIR_FORCE_CLOSE(configfd);
    virObjectUnref(cfg);

    return 0;

 error:
    if (teardowncgroup && qemuTeardownHostdevCgroup(vm, hostdev) < 0)
        VIR_WARN("Unable to remove host device cgroup ACL on hotplug fail");
    if (teardownlabel &&
        qemuSecurityRestoreHostdevLabel(driver, vm, hostdev) < 0)
        VIR_WARN("Unable to restore host device labelling on hotplug fail");
    if (teardowndevice &&
        qemuDomainNamespaceTeardownHostdev(vm, hostdev) < 0)
        VIR_WARN("Unable to remove host device from /dev");

    if (releaseaddr)
        qemuDomainReleaseDeviceAddress(vm, info, NULL);

    qemuHostdevReAttachPCIDevices(driver, vm->def->name, &hostdev, 1);

    VIR_FREE(devstr);
    VIR_FREE(configfd_name);
    VIR_FORCE_CLOSE(configfd);

 cleanup:
    virObjectUnref(cfg);
    return -1;
}


void
qemuDomainDelTLSObjects(virQEMUDriverPtr driver,
                        virDomainObjPtr vm,
                        qemuDomainAsyncJob asyncJob,
                        const char *secAlias,
                        const char *tlsAlias)
{
    qemuDomainObjPrivatePtr priv = vm->privateData;
    virErrorPtr orig_err;

    if (!tlsAlias && !secAlias)
        return;

    virErrorPreserveLast(&orig_err);

    if (qemuDomainObjEnterMonitorAsync(driver, vm, asyncJob) < 0)
        goto cleanup;

    if (tlsAlias)
        ignore_value(qemuMonitorDelObject(priv->mon, tlsAlias));

    if (secAlias)
        ignore_value(qemuMonitorDelObject(priv->mon, secAlias));

    ignore_value(qemuDomainObjExitMonitor(driver, vm));

 cleanup:
    virErrorRestore(&orig_err);
}


int
qemuDomainAddTLSObjects(virQEMUDriverPtr driver,
                        virDomainObjPtr vm,
                        qemuDomainAsyncJob asyncJob,
                        virJSONValuePtr *secProps,
                        virJSONValuePtr *tlsProps)
{
    qemuDomainObjPrivatePtr priv = vm->privateData;
    virErrorPtr orig_err;
    char *secAlias = NULL;

    if (!tlsProps && !secProps)
        return 0;

    if (qemuDomainObjEnterMonitorAsync(driver, vm, asyncJob) < 0)
        return -1;

    if (secProps && *secProps &&
        qemuMonitorAddObject(priv->mon, secProps, &secAlias) < 0)
        goto error;

    if (tlsProps &&
        qemuMonitorAddObject(priv->mon, tlsProps, NULL) < 0)
        goto error;

    VIR_FREE(secAlias);

    return qemuDomainObjExitMonitor(driver, vm);

 error:
    virErrorPreserveLast(&orig_err);
    ignore_value(qemuDomainObjExitMonitor(driver, vm));
    virErrorRestore(&orig_err);
    qemuDomainDelTLSObjects(driver, vm, asyncJob, secAlias, NULL);
    VIR_FREE(secAlias);

    return -1;
}


int
qemuDomainGetTLSObjects(virQEMUCapsPtr qemuCaps,
                        qemuDomainSecretInfoPtr secinfo,
                        const char *tlsCertdir,
                        bool tlsListen,
                        bool tlsVerify,
                        const char *alias,
                        virJSONValuePtr *tlsProps,
                        virJSONValuePtr *secProps)
{
    const char *secAlias = NULL;

    if (secinfo) {
        if (qemuBuildSecretInfoProps(secinfo, secProps) < 0)
            return -1;

        secAlias = secinfo->s.aes.alias;
    }

    if (qemuBuildTLSx509BackendProps(tlsCertdir, tlsListen, tlsVerify,
                                     alias, secAlias, qemuCaps, tlsProps) < 0)
        return -1;

    return 0;
}


static int
qemuDomainAddChardevTLSObjects(virQEMUDriverPtr driver,
                               virDomainObjPtr vm,
                               virDomainChrSourceDefPtr dev,
                               char *devAlias,
                               char *charAlias,
                               char **tlsAlias,
                               const char **secAlias)
{
    int ret = -1;
    virQEMUDriverConfigPtr cfg = virQEMUDriverGetConfig(driver);
    qemuDomainObjPrivatePtr priv = vm->privateData;
    qemuDomainChrSourcePrivatePtr chrSourcePriv;
    qemuDomainSecretInfoPtr secinfo = NULL;
    virJSONValuePtr tlsProps = NULL;
    virJSONValuePtr secProps = NULL;

    /* NB: This may alter haveTLS based on cfg */
    qemuDomainPrepareChardevSourceTLS(dev, cfg);

    if (dev->type != VIR_DOMAIN_CHR_TYPE_TCP ||
        dev->data.tcp.haveTLS != VIR_TRISTATE_BOOL_YES) {
        ret = 0;
        goto cleanup;
    }

    if (qemuDomainSecretChardevPrepare(cfg, priv, devAlias, dev) < 0)
        goto cleanup;

    if ((chrSourcePriv = QEMU_DOMAIN_CHR_SOURCE_PRIVATE(dev)))
        secinfo = chrSourcePriv->secinfo;

    if (secinfo)
        *secAlias = secinfo->s.aes.alias;

    if (!(*tlsAlias = qemuAliasTLSObjFromSrcAlias(charAlias)))
        goto cleanup;

    if (qemuDomainGetTLSObjects(priv->qemuCaps, secinfo,
                                cfg->chardevTLSx509certdir,
                                dev->data.tcp.listen,
                                cfg->chardevTLSx509verify,
                                *tlsAlias, &tlsProps, &secProps) < 0)
        goto cleanup;
    dev->data.tcp.tlscreds = true;

    if (qemuDomainAddTLSObjects(driver, vm, QEMU_ASYNC_JOB_NONE,
                                &secProps, &tlsProps) < 0)
        goto cleanup;

    ret = 0;

 cleanup:
    virJSONValueFree(tlsProps);
    virJSONValueFree(secProps);
    virObjectUnref(cfg);

    return ret;
}


static int
qemuDomainDelChardevTLSObjects(virQEMUDriverPtr driver,
                               virDomainObjPtr vm,
                               virDomainChrSourceDefPtr dev,
                               const char *inAlias)
{
    int ret = -1;
    virQEMUDriverConfigPtr cfg = virQEMUDriverGetConfig(driver);
    qemuDomainObjPrivatePtr priv = vm->privateData;
    char *tlsAlias = NULL;
    char *secAlias = NULL;

    if (dev->type != VIR_DOMAIN_CHR_TYPE_TCP ||
        dev->data.tcp.haveTLS != VIR_TRISTATE_BOOL_YES) {
        ret = 0;
        goto cleanup;
    }

    if (!(tlsAlias = qemuAliasTLSObjFromSrcAlias(inAlias)))
        goto cleanup;

    /* Best shot at this as the secinfo is destroyed after process launch
     * and this path does not recreate it. Thus, if the config has the
     * secret UUID and we have a serial TCP chardev, then formulate a
     * secAlias which we'll attempt to destroy. */
    if (cfg->chardevTLSx509secretUUID &&
        !(secAlias = qemuDomainGetSecretAESAlias(inAlias, false)))
        goto cleanup;

    qemuDomainObjEnterMonitor(driver, vm);

    ignore_value(qemuMonitorDelObject(priv->mon, tlsAlias));
    if (secAlias)
        ignore_value(qemuMonitorDelObject(priv->mon, secAlias));

    if (qemuDomainObjExitMonitor(driver, vm) < 0)
        goto cleanup;

    ret = 0;

 cleanup:
    VIR_FREE(tlsAlias);
    VIR_FREE(secAlias);
    virObjectUnref(cfg);
    return ret;
}


int qemuDomainAttachRedirdevDevice(virQEMUDriverPtr driver,
                                   virDomainObjPtr vm,
                                   virDomainRedirdevDefPtr redirdev)
{
    int ret = -1;
    qemuDomainObjPrivatePtr priv = vm->privateData;
    virDomainDefPtr def = vm->def;
    char *charAlias = NULL;
    char *devstr = NULL;
    bool chardevAdded = false;
    char *tlsAlias = NULL;
    const char *secAlias = NULL;
    bool need_release = false;
    virErrorPtr orig_err;

    if (qemuAssignDeviceRedirdevAlias(def, redirdev, -1) < 0)
        goto cleanup;

    if (!(charAlias = qemuAliasChardevFromDevAlias(redirdev->info.alias)))
        goto cleanup;

    if ((virDomainUSBAddressEnsure(priv->usbaddrs, &redirdev->info)) < 0)
        goto cleanup;
    need_release = true;

    if (!(devstr = qemuBuildRedirdevDevStr(def, redirdev, priv->qemuCaps)))
        goto cleanup;

    if (VIR_REALLOC_N(def->redirdevs, def->nredirdevs+1) < 0)
        goto cleanup;

    if (qemuDomainAddChardevTLSObjects(driver, vm, redirdev->source,
                                       redirdev->info.alias, charAlias,
                                       &tlsAlias, &secAlias) < 0)
        goto audit;

    qemuDomainObjEnterMonitor(driver, vm);

    if (qemuMonitorAttachCharDev(priv->mon,
                                 charAlias,
                                 redirdev->source) < 0)
        goto exit_monitor;
    chardevAdded = true;

    if (qemuMonitorAddDevice(priv->mon, devstr) < 0)
        goto exit_monitor;

    if (qemuDomainObjExitMonitor(driver, vm) < 0)
        goto audit;

    def->redirdevs[def->nredirdevs++] = redirdev;
    ret = 0;
 audit:
    virDomainAuditRedirdev(vm, redirdev, "attach", ret == 0);
 cleanup:
    if (ret < 0 && need_release)
        qemuDomainReleaseDeviceAddress(vm, &redirdev->info, NULL);
    VIR_FREE(tlsAlias);
    VIR_FREE(charAlias);
    VIR_FREE(devstr);
    return ret;

 exit_monitor:
    virErrorPreserveLast(&orig_err);
    /* detach associated chardev on error */
    if (chardevAdded)
        ignore_value(qemuMonitorDetachCharDev(priv->mon, charAlias));
    ignore_value(qemuDomainObjExitMonitor(driver, vm));
    virErrorRestore(&orig_err);
    qemuDomainDelTLSObjects(driver, vm, QEMU_ASYNC_JOB_NONE,
                            secAlias, tlsAlias);
    goto audit;
}

static int
qemuDomainChrPreInsert(virDomainDefPtr vmdef,
                       virDomainChrDefPtr chr)
{
    if (chr->deviceType == VIR_DOMAIN_CHR_DEVICE_TYPE_CONSOLE &&
        chr->targetType == VIR_DOMAIN_CHR_CONSOLE_TARGET_TYPE_SERIAL) {
        virReportError(VIR_ERR_OPERATION_UNSUPPORTED, "%s",
                       _("attaching serial console is not supported"));
        return -1;
    }

    if (virDomainChrFind(vmdef, chr)) {
        virReportError(VIR_ERR_OPERATION_INVALID, "%s",
                       _("chardev already exists"));
        return -1;
    }

    if (virDomainChrPreAlloc(vmdef, chr) < 0)
        return -1;

    /* Due to historical reasons, the first console is an alias to the
     * first serial device (if such exists). If this is the case, we need to
     * create an object for the first console as well.
     */
    if (vmdef->nserials == 0 && vmdef->nconsoles == 0 &&
        chr->deviceType == VIR_DOMAIN_CHR_DEVICE_TYPE_SERIAL) {
        if (!vmdef->consoles && VIR_ALLOC(vmdef->consoles) < 0)
            return -1;

        /* We'll be dealing with serials[0] directly, so NULL is fine here. */
        if (!(vmdef->consoles[0] = virDomainChrDefNew(NULL))) {
            VIR_FREE(vmdef->consoles);
            return -1;
        }
        vmdef->nconsoles++;
    }
    return 0;
}

static void
qemuDomainChrInsertPreAlloced(virDomainDefPtr vmdef,
                              virDomainChrDefPtr chr)
{
    virDomainChrInsertPreAlloced(vmdef, chr);
    if (vmdef->nserials == 1 && vmdef->nconsoles == 0 &&
        chr->deviceType == VIR_DOMAIN_CHR_DEVICE_TYPE_SERIAL) {
        vmdef->nconsoles = 1;

        /* Create an console alias for the serial port */
        vmdef->consoles[0]->deviceType = VIR_DOMAIN_CHR_DEVICE_TYPE_CONSOLE;
        vmdef->consoles[0]->targetType = VIR_DOMAIN_CHR_CONSOLE_TARGET_TYPE_SERIAL;
    }
}

static void
qemuDomainChrInsertPreAllocCleanup(virDomainDefPtr vmdef,
                                   virDomainChrDefPtr chr)
{
    /* Remove the stub console added by qemuDomainChrPreInsert */
    if (vmdef->nserials == 0 && vmdef->nconsoles == 1 &&
        chr->deviceType == VIR_DOMAIN_CHR_DEVICE_TYPE_SERIAL) {
        virDomainChrDefFree(vmdef->consoles[0]);
        VIR_FREE(vmdef->consoles);
        vmdef->nconsoles = 0;
    }
}

int
qemuDomainChrInsert(virDomainDefPtr vmdef,
                    virDomainChrDefPtr chr)
{
    if (qemuDomainChrPreInsert(vmdef, chr) < 0) {
        qemuDomainChrInsertPreAllocCleanup(vmdef, chr);
        return -1;
    }
    qemuDomainChrInsertPreAlloced(vmdef, chr);
    return 0;
}

virDomainChrDefPtr
qemuDomainChrRemove(virDomainDefPtr vmdef,
                    virDomainChrDefPtr chr)
{
    virDomainChrDefPtr ret;
    bool removeCompat;

    if (chr->deviceType == VIR_DOMAIN_CHR_DEVICE_TYPE_CONSOLE &&
        chr->targetType == VIR_DOMAIN_CHR_CONSOLE_TARGET_TYPE_SERIAL) {
        virReportError(VIR_ERR_OPERATION_INVALID, "%s",
                       _("detaching serial console is not supported"));
        return NULL;
    }

    /* Due to some crazy backcompat stuff, the first serial device is an alias
     * to the first console too. If this is the case, the definition must be
     * duplicated as first console device. */
    removeCompat = vmdef->nserials && vmdef->nconsoles &&
        vmdef->consoles[0]->deviceType == VIR_DOMAIN_CHR_DEVICE_TYPE_CONSOLE &&
        vmdef->consoles[0]->targetType == VIR_DOMAIN_CHR_CONSOLE_TARGET_TYPE_SERIAL &&
        virDomainChrEquals(vmdef->serials[0], chr);

    if (!(ret = virDomainChrRemove(vmdef, chr))) {
        virReportError(VIR_ERR_INVALID_ARG, "%s",
                       _("device not present in domain configuration"));
            return NULL;
    }

    if (removeCompat)
        VIR_DELETE_ELEMENT(vmdef->consoles, 0, vmdef->nconsoles);

    return ret;
}

/* Returns  1 if the address will need to be released later,
 *         -1 on error
 *          0 otherwise
 */
static int
qemuDomainAttachChrDeviceAssignAddr(virDomainObjPtr vm,
                                    virDomainChrDefPtr chr,
                                    virQEMUDriverPtr driver)
{
    virDomainDefPtr def = vm->def;
    qemuDomainObjPrivatePtr priv = vm->privateData;
    virDomainDeviceDef dev = { VIR_DOMAIN_DEVICE_CHR, { .chr = chr } };

    if (chr->deviceType == VIR_DOMAIN_CHR_DEVICE_TYPE_CONSOLE &&
        chr->targetType == VIR_DOMAIN_CHR_CONSOLE_TARGET_TYPE_VIRTIO) {
        if (virDomainVirtioSerialAddrAutoAssign(def, &chr->info, true) < 0)
            return -1;
        return 0;

    } else if (chr->deviceType == VIR_DOMAIN_CHR_DEVICE_TYPE_SERIAL &&
               chr->targetType == VIR_DOMAIN_CHR_SERIAL_TARGET_TYPE_PCI) {
        if (qemuDomainEnsurePCIAddress(vm, &dev, driver) < 0)
            return -1;
        return 1;

    } else if (chr->deviceType == VIR_DOMAIN_CHR_DEVICE_TYPE_SERIAL &&
               chr->targetType == VIR_DOMAIN_CHR_SERIAL_TARGET_TYPE_USB) {
        if (virDomainUSBAddressEnsure(priv->usbaddrs, &chr->info) < 0)
            return -1;
        return 1;

    } else if (chr->deviceType == VIR_DOMAIN_CHR_DEVICE_TYPE_CHANNEL &&
               chr->targetType == VIR_DOMAIN_CHR_CHANNEL_TARGET_TYPE_VIRTIO) {
        if (virDomainVirtioSerialAddrAutoAssign(def, &chr->info, false) < 0)
            return -1;
        return 0;
    }

    if (chr->info.type == VIR_DOMAIN_DEVICE_ADDRESS_TYPE_VIRTIO_SERIAL ||
        chr->info.type == VIR_DOMAIN_DEVICE_ADDRESS_TYPE_PCI) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("Unsupported address type for character device"));
        return -1;
    }

    return 0;
}

int qemuDomainAttachChrDevice(virQEMUDriverPtr driver,
                              virDomainObjPtr vm,
                              virDomainChrDefPtr chr)
{
    int ret = -1, rc;
    qemuDomainObjPrivatePtr priv = vm->privateData;
    virErrorPtr orig_err;
    virDomainDefPtr vmdef = vm->def;
    char *devstr = NULL;
    virDomainChrSourceDefPtr dev = chr->source;
    char *charAlias = NULL;
    bool chardevAttached = false;
    bool teardowncgroup = false;
    bool teardowndevice = false;
    bool teardownlabel = false;
    char *tlsAlias = NULL;
    const char *secAlias = NULL;
    bool need_release = false;

    if (chr->deviceType == VIR_DOMAIN_CHR_DEVICE_TYPE_CHANNEL &&
        qemuDomainPrepareChannel(chr, priv->channelTargetDir) < 0)
        goto cleanup;

    if (qemuAssignDeviceChrAlias(vmdef, chr, -1) < 0)
        goto cleanup;

    if ((rc = qemuDomainAttachChrDeviceAssignAddr(vm, chr, driver)) < 0)
        goto cleanup;
    if (rc == 1)
        need_release = true;

    if (qemuDomainNamespaceSetupChardev(vm, chr) < 0)
        goto cleanup;
    teardowndevice = true;

    if (qemuSecuritySetChardevLabel(driver, vm, chr) < 0)
        goto cleanup;
    teardownlabel = true;

    if (qemuSetupChardevCgroup(vm, chr) < 0)
        goto cleanup;
    teardowncgroup = true;

    if (qemuBuildChrDeviceStr(&devstr, vmdef, chr, priv->qemuCaps) < 0)
        goto cleanup;

    if (!(charAlias = qemuAliasChardevFromDevAlias(chr->info.alias)))
        goto cleanup;

    if (qemuDomainChrPreInsert(vmdef, chr) < 0)
        goto cleanup;

    if (qemuDomainAddChardevTLSObjects(driver, vm, dev,
                                       chr->info.alias, charAlias,
                                       &tlsAlias, &secAlias) < 0)
        goto audit;

    qemuDomainObjEnterMonitor(driver, vm);

    if (qemuMonitorAttachCharDev(priv->mon, charAlias, chr->source) < 0)
        goto exit_monitor;
    chardevAttached = true;

    if (qemuMonitorAddDevice(priv->mon, devstr) < 0)
        goto exit_monitor;

    if (qemuDomainObjExitMonitor(driver, vm) < 0)
        goto audit;

    qemuDomainChrInsertPreAlloced(vmdef, chr);
    ret = 0;
 audit:
    virDomainAuditChardev(vm, NULL, chr, "attach", ret == 0);
 cleanup:
    if (ret < 0) {
        if (virDomainObjIsActive(vm))
            qemuDomainChrInsertPreAllocCleanup(vmdef, chr);
        if (need_release)
            qemuDomainReleaseDeviceAddress(vm, &chr->info, NULL);
        if (teardowncgroup && qemuTeardownChardevCgroup(vm, chr) < 0)
            VIR_WARN("Unable to remove chr device cgroup ACL on hotplug fail");
        if (teardownlabel && qemuSecurityRestoreChardevLabel(driver, vm, chr) < 0)
            VIR_WARN("Unable to restore security label on char device");
        if (teardowndevice && qemuDomainNamespaceTeardownChardev(vm, chr) < 0)
            VIR_WARN("Unable to remove chr device from /dev");
    }
    VIR_FREE(tlsAlias);
    VIR_FREE(charAlias);
    VIR_FREE(devstr);
    return ret;

 exit_monitor:
    virErrorPreserveLast(&orig_err);
    /* detach associated chardev on error */
    if (chardevAttached)
        qemuMonitorDetachCharDev(priv->mon, charAlias);
    ignore_value(qemuDomainObjExitMonitor(driver, vm));
    virErrorRestore(&orig_err);

    qemuDomainDelTLSObjects(driver, vm, QEMU_ASYNC_JOB_NONE,
                            secAlias, tlsAlias);
    goto audit;
}


int
qemuDomainAttachRNGDevice(virQEMUDriverPtr driver,
                          virDomainObjPtr vm,
                          virDomainRNGDefPtr rng)
{
    qemuDomainObjPrivatePtr priv = vm->privateData;
    virDomainDeviceDef dev = { VIR_DOMAIN_DEVICE_RNG, { .rng = rng } };
    virErrorPtr orig_err;
    char *devstr = NULL;
    char *charAlias = NULL;
    char *objAlias = NULL;
    char *tlsAlias = NULL;
    const char *secAlias = NULL;
    bool releaseaddr = false;
    bool teardowncgroup = false;
    bool teardowndevice = false;
    bool chardevAdded = false;
    virJSONValuePtr props = NULL;
    int ret = -1;

    if (qemuAssignDeviceRNGAlias(vm->def, rng) < 0)
        goto cleanup;

    /* preallocate space for the device definition */
    if (VIR_REALLOC_N(vm->def->rngs, vm->def->nrngs + 1) < 0)
        goto cleanup;

    if (qemuDomainEnsureVirtioAddress(&releaseaddr, vm, &dev, "rng") < 0)
        return -1;

    if (qemuDomainNamespaceSetupRNG(vm, rng) < 0)
        goto cleanup;
    teardowndevice = true;

    if (qemuSetupRNGCgroup(vm, rng) < 0)
        goto cleanup;
    teardowncgroup = true;

    /* build required metadata */
    if (!(devstr = qemuBuildRNGDevStr(vm->def, rng, priv->qemuCaps)))
        goto cleanup;

    if (qemuBuildRNGBackendProps(rng, priv->qemuCaps, &props) < 0)
        goto cleanup;

    if (!(charAlias = qemuAliasChardevFromDevAlias(rng->info.alias)))
        goto cleanup;

    if (rng->backend == VIR_DOMAIN_RNG_BACKEND_EGD) {
        if (qemuDomainAddChardevTLSObjects(driver, vm,
                                           rng->source.chardev,
                                           rng->info.alias, charAlias,
                                           &tlsAlias, &secAlias) < 0)
            goto audit;
    }

    qemuDomainObjEnterMonitor(driver, vm);

    if (rng->backend == VIR_DOMAIN_RNG_BACKEND_EGD &&
        qemuMonitorAttachCharDev(priv->mon, charAlias,
                                 rng->source.chardev) < 0)
        goto exit_monitor;
    chardevAdded = true;

    if (qemuMonitorAddObject(priv->mon, &props, &objAlias) < 0)
        goto exit_monitor;

    if (qemuMonitorAddDevice(priv->mon, devstr) < 0)
        goto exit_monitor;

    if (qemuDomainObjExitMonitor(driver, vm) < 0) {
        releaseaddr = false;
        goto cleanup;
    }

    VIR_APPEND_ELEMENT_INPLACE(vm->def->rngs, vm->def->nrngs, rng);

    ret = 0;

 audit:
    virDomainAuditRNG(vm, NULL, rng, "attach", ret == 0);
 cleanup:
    virJSONValueFree(props);
    if (ret < 0) {
        if (releaseaddr)
            qemuDomainReleaseDeviceAddress(vm, &rng->info, NULL);
        if (teardowncgroup && qemuTeardownRNGCgroup(vm, rng) < 0)
            VIR_WARN("Unable to remove RNG device cgroup ACL on hotplug fail");
        if (teardowndevice && qemuDomainNamespaceTeardownRNG(vm, rng) < 0)
            VIR_WARN("Unable to remove chr device from /dev");
    }

    VIR_FREE(tlsAlias);
    VIR_FREE(charAlias);
    VIR_FREE(objAlias);
    VIR_FREE(devstr);
    return ret;

 exit_monitor:
    virErrorPreserveLast(&orig_err);
    if (objAlias)
        ignore_value(qemuMonitorDelObject(priv->mon, objAlias));
    if (rng->backend == VIR_DOMAIN_RNG_BACKEND_EGD && chardevAdded)
        ignore_value(qemuMonitorDetachCharDev(priv->mon, charAlias));
    if (qemuDomainObjExitMonitor(driver, vm) < 0)
        releaseaddr = false;
    virErrorRestore(&orig_err);

    qemuDomainDelTLSObjects(driver, vm, QEMU_ASYNC_JOB_NONE,
                            secAlias, tlsAlias);
    goto audit;
}


/**
 * qemuDomainAttachMemory:
 * @driver: qemu driver data
 * @vm: VM object
 * @mem: Definition of the memory device to be attached. @mem is always consumed
 *
 * Attaches memory device described by @mem to domain @vm.
 *
 * Returns 0 on success -1 on error.
 */
int
qemuDomainAttachMemory(virQEMUDriverPtr driver,
                       virDomainObjPtr vm,
                       virDomainMemoryDefPtr mem)
{
    qemuDomainObjPrivatePtr priv = vm->privateData;
    virErrorPtr orig_err;
    virQEMUDriverConfigPtr cfg = virQEMUDriverGetConfig(driver);
    unsigned long long oldmem = virDomainDefGetMemoryTotal(vm->def);
    unsigned long long newmem = oldmem + mem->size;
    char *devstr = NULL;
    char *objalias = NULL;
    bool objAdded = false;
    bool teardownlabel = false;
    bool teardowncgroup = false;
    bool teardowndevice = false;
    virJSONValuePtr props = NULL;
    virObjectEventPtr event;
    int id;
    int ret = -1;

    qemuDomainMemoryDeviceAlignSize(vm->def, mem);

    if (qemuDomainDefValidateMemoryHotplug(vm->def, priv->qemuCaps, mem) < 0)
        goto cleanup;

    if (qemuDomainAssignMemoryDeviceSlot(vm->def, mem) < 0)
        goto cleanup;

    /* in cases where we are using a VM with aliases generated according to the
     * index of the memory device we need to keep continue using that scheme */
    if (qemuAssignDeviceMemoryAlias(vm->def, mem, priv->memAliasOrderMismatch) < 0)
        goto cleanup;

    if (virAsprintf(&objalias, "mem%s", mem->info.alias) < 0)
        goto cleanup;

    if (!(devstr = qemuBuildMemoryDeviceStr(mem)))
        goto cleanup;

    if (qemuBuildMemoryBackendProps(&props, objalias, cfg,
                                    priv->qemuCaps, vm->def, mem, NULL, true) < 0)
        goto cleanup;

    if (qemuProcessBuildDestroyMemoryPaths(driver, vm, mem, true) < 0)
        goto cleanup;

    if (qemuDomainNamespaceSetupMemory(vm, mem) < 0)
        goto cleanup;
    teardowndevice = true;

    if (qemuSetupMemoryDevicesCgroup(vm, mem) < 0)
        goto cleanup;
    teardowncgroup = true;

    if (qemuSecuritySetMemoryLabel(driver, vm, mem) < 0)
        goto cleanup;
    teardownlabel = true;

    if (virDomainMemoryInsert(vm->def, mem) < 0)
        goto cleanup;

    if (qemuDomainAdjustMaxMemLock(vm) < 0)
        goto removedef;

    qemuDomainObjEnterMonitor(driver, vm);
    if (qemuMonitorAddObject(priv->mon, &props, NULL) < 0)
        goto exit_monitor;
    objAdded = true;

    if (qemuMonitorAddDevice(priv->mon, devstr) < 0)
        goto exit_monitor;

    if (qemuDomainObjExitMonitor(driver, vm) < 0) {
        /* we shouldn't touch mem now, as the def might be freed */
        mem = NULL;
        goto audit;
    }

    event = virDomainEventDeviceAddedNewFromObj(vm, objalias);
    virObjectEventStateQueue(driver->domainEventState, event);

    /* fix the balloon size */
    ignore_value(qemuProcessRefreshBalloonState(driver, vm, QEMU_ASYNC_JOB_NONE));

    /* mem is consumed by vm->def */
    mem = NULL;

    /* this step is best effort, removing the device would be so much trouble */
    ignore_value(qemuDomainUpdateMemoryDeviceInfo(driver, vm,
                                                  QEMU_ASYNC_JOB_NONE));

    ret = 0;

 audit:
    virDomainAuditMemory(vm, oldmem, newmem, "update", ret == 0);
 cleanup:
    if (mem && ret < 0) {
        if (teardowncgroup && qemuTeardownMemoryDevicesCgroup(vm, mem) < 0)
            VIR_WARN("Unable to remove memory device cgroup ACL on hotplug fail");
        if (teardownlabel && qemuSecurityRestoreMemoryLabel(driver, vm, mem) < 0)
            VIR_WARN("Unable to restore security label on memdev");
        if (teardowndevice &&
            qemuDomainNamespaceTeardownMemory(vm, mem) <  0)
            VIR_WARN("Unable to remove memory device from /dev");
    }

    virJSONValueFree(props);
    virObjectUnref(cfg);
    VIR_FREE(devstr);
    VIR_FREE(objalias);
    virDomainMemoryDefFree(mem);
    return ret;

 exit_monitor:
    virErrorPreserveLast(&orig_err);
    if (objAdded)
        ignore_value(qemuMonitorDelObject(priv->mon, objalias));
    if (qemuDomainObjExitMonitor(driver, vm) < 0)
        mem = NULL;

    if (objAdded && mem)
        ignore_value(qemuProcessDestroyMemoryBackingPath(driver, vm, mem));

    virErrorRestore(&orig_err);
    if (!mem)
        goto audit;

 removedef:
    if ((id = virDomainMemoryFindByDef(vm->def, mem)) >= 0)
        mem = virDomainMemoryRemove(vm->def, id);
    else
        mem = NULL;

    /* reset the mlock limit */
    virErrorPreserveLast(&orig_err);
    ignore_value(qemuDomainAdjustMaxMemLock(vm));
    virErrorRestore(&orig_err);

    goto audit;
}


static int
qemuDomainAttachHostUSBDevice(virQEMUDriverPtr driver,
                              virDomainObjPtr vm,
                              virDomainHostdevDefPtr hostdev)
{
    qemuDomainObjPrivatePtr priv = vm->privateData;
    char *devstr = NULL;
    bool added = false;
    bool teardowncgroup = false;
    bool teardownlabel = false;
    bool teardowndevice = false;
    int ret = -1;

    if (virDomainUSBAddressEnsure(priv->usbaddrs, hostdev->info) < 0)
        return -1;

    if (qemuHostdevPrepareUSBDevices(driver, vm->def->name, &hostdev, 1, 0) < 0)
        goto cleanup;

    added = true;

    if (qemuDomainNamespaceSetupHostdev(vm, hostdev) < 0)
        goto cleanup;
    teardowndevice = true;

    if (qemuSetupHostdevCgroup(vm, hostdev) < 0)
        goto cleanup;
    teardowncgroup = true;

    if (qemuSecuritySetHostdevLabel(driver, vm, hostdev) < 0)
        goto cleanup;
    teardownlabel = true;

    if (qemuAssignDeviceHostdevAlias(vm->def, &hostdev->info->alias, -1) < 0)
        goto cleanup;
    if (!(devstr = qemuBuildUSBHostdevDevStr(vm->def, hostdev, priv->qemuCaps)))
        goto cleanup;

    if (VIR_REALLOC_N(vm->def->hostdevs, vm->def->nhostdevs+1) < 0)
        goto cleanup;

    qemuDomainObjEnterMonitor(driver, vm);
    ret = qemuMonitorAddDevice(priv->mon, devstr);
    if (qemuDomainObjExitMonitor(driver, vm) < 0) {
        ret = -1;
        goto cleanup;
    }
    virDomainAuditHostdev(vm, hostdev, "attach", ret == 0);
    if (ret < 0)
        goto cleanup;

    vm->def->hostdevs[vm->def->nhostdevs++] = hostdev;

    ret = 0;
 cleanup:
    if (ret < 0) {
        if (teardowncgroup && qemuTeardownHostdevCgroup(vm, hostdev) < 0)
            VIR_WARN("Unable to remove host device cgroup ACL on hotplug fail");
        if (teardownlabel &&
            qemuSecurityRestoreHostdevLabel(driver, vm, hostdev) < 0)
            VIR_WARN("Unable to restore host device labelling on hotplug fail");
        if (teardowndevice &&
            qemuDomainNamespaceTeardownHostdev(vm, hostdev) < 0)
            VIR_WARN("Unable to remove host device from /dev");
        if (added)
            qemuHostdevReAttachUSBDevices(driver, vm->def->name, &hostdev, 1);
        virDomainUSBAddressRelease(priv->usbaddrs, hostdev->info);
    }
    VIR_FREE(devstr);
    return ret;
}


static int
qemuDomainAttachHostSCSIDevice(virQEMUDriverPtr driver,
                               virDomainObjPtr vm,
                               virDomainHostdevDefPtr hostdev)
{
    size_t i;
    int ret = -1;
    qemuDomainObjPrivatePtr priv = vm->privateData;
    virErrorPtr orig_err;
    char *devstr = NULL;
    char *drvstr = NULL;
    char *drivealias = NULL;
    char *secobjAlias = NULL;
    bool teardowncgroup = false;
    bool teardownlabel = false;
    bool teardowndevice = false;
    bool driveAdded = false;
    virJSONValuePtr secobjProps = NULL;
    virDomainHostdevSubsysSCSIPtr scsisrc = &hostdev->source.subsys.u.scsi;
    qemuDomainSecretInfoPtr secinfo = NULL;

    /* Let's make sure the disk has a controller defined and loaded before
     * trying to add it. The controller used by the disk must exist before a
     * qemu command line string is generated.
     *
     * Ensure that the given controller and all controllers with a smaller index
     * exist; there must not be any missing index in between.
     */
    for (i = 0; i <= hostdev->info->addr.drive.controller; i++) {
        if (!qemuDomainFindOrCreateSCSIDiskController(driver, vm, i))
            return -1;
    }

    if (qemuHostdevPrepareSCSIDevices(driver, vm->def->name, &hostdev, 1) < 0)
        return -1;

    if (qemuDomainNamespaceSetupHostdev(vm, hostdev) < 0)
        goto cleanup;
    teardowndevice = true;

    if (qemuSetupHostdevCgroup(vm, hostdev) < 0)
        goto cleanup;
    teardowncgroup = true;

    if (qemuSecuritySetHostdevLabel(driver, vm, hostdev) < 0)
        goto cleanup;
    teardownlabel = true;

    if (qemuAssignDeviceHostdevAlias(vm->def, &hostdev->info->alias, -1) < 0)
        goto cleanup;

    if (qemuDomainSecretHostdevPrepare(priv, hostdev) < 0)
        goto cleanup;

    if (scsisrc->protocol == VIR_DOMAIN_HOSTDEV_SCSI_PROTOCOL_TYPE_ISCSI) {
        qemuDomainStorageSourcePrivatePtr srcPriv =
            QEMU_DOMAIN_STORAGE_SOURCE_PRIVATE(scsisrc->u.iscsi.src);
        if (srcPriv)
            secinfo = srcPriv->secinfo;
    }

    if (secinfo && secinfo->type == VIR_DOMAIN_SECRET_INFO_TYPE_AES) {
        if (qemuBuildSecretInfoProps(secinfo, &secobjProps) < 0)
            goto cleanup;
    }

    if (!(drvstr = qemuBuildSCSIHostdevDrvStr(hostdev, priv->qemuCaps)))
        goto cleanup;

    if (!(drivealias = qemuAliasFromHostdev(hostdev)))
        goto cleanup;

    if (!(devstr = qemuBuildSCSIHostdevDevStr(vm->def, hostdev)))
        goto cleanup;

    if (VIR_REALLOC_N(vm->def->hostdevs, vm->def->nhostdevs + 1) < 0)
        goto cleanup;

    qemuDomainObjEnterMonitor(driver, vm);

    if (secobjProps &&
        qemuMonitorAddObject(priv->mon, &secobjProps, &secobjAlias) < 0)
        goto exit_monitor;

    if (qemuMonitorAddDrive(priv->mon, drvstr) < 0)
        goto exit_monitor;
    driveAdded = true;

    if (qemuMonitorAddDevice(priv->mon, devstr) < 0)
        goto exit_monitor;

    if (qemuDomainObjExitMonitor(driver, vm) < 0)
        goto cleanup;

    virDomainAuditHostdev(vm, hostdev, "attach", true);

    vm->def->hostdevs[vm->def->nhostdevs++] = hostdev;

    ret = 0;

 cleanup:
    if (ret < 0) {
        qemuHostdevReAttachSCSIDevices(driver, vm->def->name, &hostdev, 1);
        if (teardowncgroup && qemuTeardownHostdevCgroup(vm, hostdev) < 0)
            VIR_WARN("Unable to remove host device cgroup ACL on hotplug fail");
        if (teardownlabel &&
            qemuSecurityRestoreHostdevLabel(driver, vm, hostdev) < 0)
            VIR_WARN("Unable to restore host device labelling on hotplug fail");
        if (teardowndevice &&
            qemuDomainNamespaceTeardownHostdev(vm, hostdev) < 0)
            VIR_WARN("Unable to remove host device from /dev");
    }
    qemuDomainSecretHostdevDestroy(hostdev);
    virJSONValueFree(secobjProps);
    VIR_FREE(secobjAlias);
    VIR_FREE(drivealias);
    VIR_FREE(drvstr);
    VIR_FREE(devstr);
    return ret;

 exit_monitor:
    virErrorPreserveLast(&orig_err);
    if (driveAdded && qemuMonitorDriveDel(priv->mon, drivealias) < 0) {
        VIR_WARN("Unable to remove drive %s (%s) after failed "
                 "qemuMonitorAddDevice",
                 drvstr, devstr);
    }
    if (secobjAlias)
        ignore_value(qemuMonitorDelObject(priv->mon, secobjAlias));
    ignore_value(qemuDomainObjExitMonitor(driver, vm));
    virErrorRestore(&orig_err);

    virDomainAuditHostdev(vm, hostdev, "attach", false);

    goto cleanup;
}

static int
qemuDomainAttachSCSIVHostDevice(virQEMUDriverPtr driver,
                                virDomainObjPtr vm,
                                virDomainHostdevDefPtr hostdev)
{
    int ret = -1;
    qemuDomainObjPrivatePtr priv = vm->privateData;
    virDomainDeviceDef dev = { VIR_DOMAIN_DEVICE_HOSTDEV,
                               { .hostdev = hostdev } };
    virDomainCCWAddressSetPtr ccwaddrs = NULL;
    char *vhostfdName = NULL;
    int vhostfd = -1;
    char *devstr = NULL;
    bool teardowncgroup = false;
    bool teardownlabel = false;
    bool teardowndevice = false;
    bool releaseaddr = false;

    if (qemuHostdevPrepareSCSIVHostDevices(driver, vm->def->name, &hostdev, 1) < 0)
        return -1;

    if (qemuDomainNamespaceSetupHostdev(vm, hostdev) < 0)
        goto cleanup;
    teardowndevice = true;

    if (qemuSetupHostdevCgroup(vm, hostdev) < 0)
        goto cleanup;
    teardowncgroup = true;

    if (qemuSecuritySetHostdevLabel(driver, vm, hostdev) < 0)
        goto cleanup;
    teardownlabel = true;

    if (virSCSIVHostOpenVhostSCSI(&vhostfd) < 0)
        goto cleanup;

    if (virAsprintf(&vhostfdName, "vhostfd-%d", vhostfd) < 0)
        goto cleanup;

    if (hostdev->info->type == VIR_DOMAIN_DEVICE_ADDRESS_TYPE_NONE) {
        if (qemuDomainIsS390CCW(vm->def) &&
            virQEMUCapsGet(priv->qemuCaps, QEMU_CAPS_CCW))
            hostdev->info->type = VIR_DOMAIN_DEVICE_ADDRESS_TYPE_CCW;
    }

    if (hostdev->info->type == VIR_DOMAIN_DEVICE_ADDRESS_TYPE_NONE ||
        hostdev->info->type == VIR_DOMAIN_DEVICE_ADDRESS_TYPE_PCI) {
        if (qemuDomainEnsurePCIAddress(vm, &dev, driver) < 0)
            goto cleanup;
    } else if (hostdev->info->type == VIR_DOMAIN_DEVICE_ADDRESS_TYPE_CCW) {
        if (!(ccwaddrs = virDomainCCWAddressSetCreateFromDomain(vm->def)))
            goto cleanup;
        if (virDomainCCWAddressAssign(hostdev->info, ccwaddrs,
                                      !hostdev->info->addr.ccw.assigned) < 0)
            goto cleanup;
    }
    releaseaddr = true;

    if (qemuAssignDeviceHostdevAlias(vm->def, &hostdev->info->alias, -1) < 0)
        goto cleanup;

    if (!(devstr = qemuBuildSCSIVHostHostdevDevStr(vm->def,
                                                   hostdev,
                                                   priv->qemuCaps,
                                                   vhostfdName)))
        goto cleanup;

    if (VIR_REALLOC_N(vm->def->hostdevs, vm->def->nhostdevs + 1) < 0)
        goto cleanup;

    qemuDomainObjEnterMonitor(driver, vm);

    ret = qemuMonitorAddDeviceWithFd(priv->mon, devstr, vhostfd, vhostfdName);

    if (qemuDomainObjExitMonitor(driver, vm) < 0 || ret < 0)
        goto audit;

    vm->def->hostdevs[vm->def->nhostdevs++] = hostdev;
    ret = 0;

 audit:
    virDomainAuditHostdev(vm, hostdev, "attach", (ret == 0));

 cleanup:
    if (ret < 0) {
        if (teardowncgroup && qemuTeardownHostdevCgroup(vm, hostdev) < 0)
            VIR_WARN("Unable to remove host device cgroup ACL on hotplug fail");
        if (teardownlabel &&
            qemuSecurityRestoreHostdevLabel(driver, vm, hostdev) < 0)
            VIR_WARN("Unable to restore host device labelling on hotplug fail");
        if (teardowndevice &&
            qemuDomainNamespaceTeardownHostdev(vm, hostdev) < 0)
            VIR_WARN("Unable to remove host device from /dev");
        if (releaseaddr)
            qemuDomainReleaseDeviceAddress(vm, hostdev->info, NULL);
    }

    virDomainCCWAddressSetFree(ccwaddrs);

    VIR_FORCE_CLOSE(vhostfd);
    VIR_FREE(vhostfdName);
    VIR_FREE(devstr);
    return ret;
}


static int
qemuDomainAttachMediatedDevice(virQEMUDriverPtr driver,
                               virDomainObjPtr vm,
                               virDomainHostdevDefPtr hostdev)
{
    int ret = -1;
    char *devstr = NULL;
    bool added = false;
    bool teardowncgroup = false;
    bool teardownlabel = false;
    bool teardowndevice = false;
    qemuDomainObjPrivatePtr priv = vm->privateData;
    virDomainDeviceDef dev = { VIR_DOMAIN_DEVICE_HOSTDEV,
                                { .hostdev = hostdev } };

    switch (hostdev->source.subsys.u.mdev.model) {
    case VIR_MDEV_MODEL_TYPE_VFIO_PCI:
        if (qemuDomainEnsurePCIAddress(vm, &dev, driver) < 0)
            return -1;
        break;
    case VIR_MDEV_MODEL_TYPE_VFIO_CCW:
    case VIR_MDEV_MODEL_TYPE_LAST:
        break;
    }

    if (qemuHostdevPrepareMediatedDevices(driver,
                                          vm->def->name,
                                          &hostdev,
                                          1) < 0)
        goto cleanup;
    added = true;

    if (qemuDomainNamespaceSetupHostdev(vm, hostdev) < 0)
        goto cleanup;
    teardowndevice = true;

    if (qemuSetupHostdevCgroup(vm, hostdev) < 0)
        goto cleanup;
    teardowncgroup = true;

    if (qemuSecuritySetHostdevLabel(driver, vm, hostdev) < 0)
        goto cleanup;
    teardownlabel = true;

    if (qemuAssignDeviceHostdevAlias(vm->def, &hostdev->info->alias, -1) < 0)
        goto cleanup;

    if (!(devstr = qemuBuildHostdevMediatedDevStr(vm->def, hostdev,
                                                  priv->qemuCaps)))
        goto cleanup;

    if (VIR_REALLOC_N(vm->def->hostdevs, vm->def->nhostdevs + 1) < 0)
        goto cleanup;

    qemuDomainObjEnterMonitor(driver, vm);
    ret = qemuMonitorAddDevice(priv->mon, devstr);
    if (qemuDomainObjExitMonitor(driver, vm) < 0) {
        ret = -1;
        goto cleanup;
    }

    virDomainAuditHostdev(vm, hostdev, "attach", ret == 0);
    if (ret < 0)
        goto cleanup;

    VIR_APPEND_ELEMENT_INPLACE(vm->def->hostdevs, vm->def->nhostdevs, hostdev);
    ret = 0;
 cleanup:
    if (ret < 0) {
        if (teardowncgroup && qemuTeardownHostdevCgroup(vm, hostdev) < 0)
            VIR_WARN("Unable to remove host device cgroup ACL on hotplug fail");
        if (teardownlabel &&
            qemuSecurityRestoreHostdevLabel(driver, vm, hostdev) < 0)
            VIR_WARN("Unable to restore host device labelling on hotplug fail");
        if (teardowndevice &&
            qemuDomainNamespaceTeardownHostdev(vm, hostdev) < 0)
            VIR_WARN("Unable to remove host device from /dev");
        if (added)
            qemuHostdevReAttachMediatedDevices(driver,
                                               vm->def->name,
                                               &hostdev,
                                               1);
        qemuDomainReleaseDeviceAddress(vm, hostdev->info, NULL);
    }
    VIR_FREE(devstr);
    return ret;
}


int
qemuDomainAttachHostDevice(virQEMUDriverPtr driver,
                           virDomainObjPtr vm,
                           virDomainHostdevDefPtr hostdev)
{
    if (hostdev->mode != VIR_DOMAIN_HOSTDEV_MODE_SUBSYS) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("hotplug is not supported for hostdev mode '%s'"),
                       virDomainHostdevModeTypeToString(hostdev->mode));
        return -1;
    }

    switch (hostdev->source.subsys.type) {
    case VIR_DOMAIN_HOSTDEV_SUBSYS_TYPE_PCI:
        if (qemuDomainAttachHostPCIDevice(driver, vm,
                                          hostdev) < 0)
            goto error;
        break;

    case VIR_DOMAIN_HOSTDEV_SUBSYS_TYPE_USB:
        if (qemuDomainAttachHostUSBDevice(driver, vm,
                                          hostdev) < 0)
            goto error;
        break;

    case VIR_DOMAIN_HOSTDEV_SUBSYS_TYPE_SCSI:
        if (qemuDomainAttachHostSCSIDevice(driver, vm,
                                           hostdev) < 0)
            goto error;
        break;

    case VIR_DOMAIN_HOSTDEV_SUBSYS_TYPE_SCSI_HOST:
        if (qemuDomainAttachSCSIVHostDevice(driver, vm, hostdev) < 0)
            goto error;
        break;
    case VIR_DOMAIN_HOSTDEV_SUBSYS_TYPE_MDEV:
        if (qemuDomainAttachMediatedDevice(driver, vm, hostdev) < 0)
            goto error;
        break;

    default:
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("hotplug is not supported for hostdev subsys type '%s'"),
                       virDomainHostdevSubsysTypeToString(hostdev->source.subsys.type));
        goto error;
    }

    return 0;

 error:
    return -1;
}


int
qemuDomainAttachShmemDevice(virQEMUDriverPtr driver,
                            virDomainObjPtr vm,
                            virDomainShmemDefPtr shmem)
{
    int ret = -1;
    char *shmstr = NULL;
    char *charAlias = NULL;
    char *memAlias = NULL;
    bool release_backing = false;
    bool release_address = true;
    virErrorPtr orig_err = NULL;
    virJSONValuePtr props = NULL;
    qemuDomainObjPrivatePtr priv = vm->privateData;
    virDomainDeviceDef dev = { VIR_DOMAIN_DEVICE_SHMEM, { .shmem = shmem } };

    switch ((virDomainShmemModel)shmem->model) {
    case VIR_DOMAIN_SHMEM_MODEL_IVSHMEM_PLAIN:
    case VIR_DOMAIN_SHMEM_MODEL_IVSHMEM_DOORBELL:
        break;

    case VIR_DOMAIN_SHMEM_MODEL_IVSHMEM:
        virReportError(VIR_ERR_OPERATION_UNSUPPORTED,
                       _("live attach of shmem model '%s' is not supported"),
                       virDomainShmemModelTypeToString(shmem->model));
        ATTRIBUTE_FALLTHROUGH;
    case VIR_DOMAIN_SHMEM_MODEL_LAST:
        return -1;
    }

    if (qemuAssignDeviceShmemAlias(vm->def, shmem, -1) < 0)
        return -1;

    if (qemuDomainPrepareShmemChardev(shmem) < 0)
        return -1;

    if (VIR_REALLOC_N(vm->def->shmems, vm->def->nshmems + 1) < 0)
        return -1;

    if ((shmem->info.type == VIR_DOMAIN_DEVICE_ADDRESS_TYPE_NONE ||
         shmem->info.type == VIR_DOMAIN_DEVICE_ADDRESS_TYPE_PCI) &&
        (qemuDomainEnsurePCIAddress(vm, &dev, driver) < 0))
        return -1;

    if (!(shmstr = qemuBuildShmemDevStr(vm->def, shmem, priv->qemuCaps)))
        goto cleanup;

    if (shmem->server.enabled) {
        if (virAsprintf(&charAlias, "char%s", shmem->info.alias) < 0)
            goto cleanup;
    } else {
        if (!(props = qemuBuildShmemBackendMemProps(shmem)))
            goto cleanup;

    }

    qemuDomainObjEnterMonitor(driver, vm);

    if (shmem->server.enabled) {
        if (qemuMonitorAttachCharDev(priv->mon, charAlias,
                                     &shmem->server.chr) < 0)
            goto exit_monitor;
    } else {
        if (qemuMonitorAddObject(priv->mon, &props, &memAlias) < 0)
            goto exit_monitor;
    }

    release_backing = true;

    if (qemuMonitorAddDevice(priv->mon, shmstr) < 0)
        goto exit_monitor;

    if (qemuDomainObjExitMonitor(driver, vm) < 0) {
        release_address = false;
        goto cleanup;
    }

    /* Doing a copy here just so the pointer doesn't get nullified
     * because we need it in the audit function */
    VIR_APPEND_ELEMENT_COPY_INPLACE(vm->def->shmems, vm->def->nshmems, shmem);

    ret = 0;
    release_address = false;

 audit:
    virDomainAuditShmem(vm, shmem, "attach", ret == 0);

 cleanup:
    if (release_address)
        qemuDomainReleaseDeviceAddress(vm, &shmem->info, NULL);

    virJSONValueFree(props);
    VIR_FREE(memAlias);
    VIR_FREE(charAlias);
    VIR_FREE(shmstr);

    return ret;

 exit_monitor:
    virErrorPreserveLast(&orig_err);
    if (release_backing) {
        if (shmem->server.enabled)
            ignore_value(qemuMonitorDetachCharDev(priv->mon, charAlias));
        else
            ignore_value(qemuMonitorDelObject(priv->mon, memAlias));
    }

    if (qemuDomainObjExitMonitor(driver, vm) < 0)
        release_address = false;

    virErrorRestore(&orig_err);

    goto audit;
}


int
qemuDomainAttachWatchdog(virQEMUDriverPtr driver,
                         virDomainObjPtr vm,
                         virDomainWatchdogDefPtr watchdog)
{
    int ret = -1;
    qemuDomainObjPrivatePtr priv = vm->privateData;
    virDomainDeviceDef dev = { VIR_DOMAIN_DEVICE_WATCHDOG, { .watchdog = watchdog } };
    virDomainWatchdogAction actualAction = watchdog->action;
    const char *actionStr = NULL;
    char *watchdogstr = NULL;
    bool releaseAddress = false;
    int rv;

    if (vm->def->watchdog) {
        virReportError(VIR_ERR_OPERATION_INVALID, "%s",
                       _("domain already has a watchdog"));
        return -1;
    }

    if (qemuAssignDeviceWatchdogAlias(watchdog) < 0)
        return -1;

    if (!(watchdogstr = qemuBuildWatchdogDevStr(vm->def, watchdog, priv->qemuCaps)))
        return -1;

    if (watchdog->model == VIR_DOMAIN_WATCHDOG_MODEL_I6300ESB) {
        if (qemuDomainEnsurePCIAddress(vm, &dev, driver) < 0)
            goto cleanup;
        releaseAddress = true;
    } else {
        virReportError(VIR_ERR_OPERATION_UNSUPPORTED,
                       _("hotplug of watchdog of model %s is not supported"),
                       virDomainWatchdogModelTypeToString(watchdog->model));
        goto cleanup;
    }

    /* QEMU doesn't have a 'dump' action; we tell qemu to 'pause', then
       libvirt listens for the watchdog event, and we perform the dump
       ourselves. so convert 'dump' to 'pause' for the qemu cli */
    if (actualAction == VIR_DOMAIN_WATCHDOG_ACTION_DUMP)
        actualAction = VIR_DOMAIN_WATCHDOG_ACTION_PAUSE;

    actionStr = virDomainWatchdogActionTypeToString(actualAction);

    qemuDomainObjEnterMonitor(driver, vm);

    rv = qemuMonitorSetWatchdogAction(priv->mon, actionStr);

    if (rv >= 0)
        rv = qemuMonitorAddDevice(priv->mon, watchdogstr);

    if (qemuDomainObjExitMonitor(driver, vm) < 0) {
        releaseAddress = false;
        goto cleanup;
    }

    if (rv < 0)
        goto cleanup;

    releaseAddress = false;
    vm->def->watchdog = watchdog;
    ret = 0;

 cleanup:
    if (releaseAddress)
        qemuDomainReleaseDeviceAddress(vm, &watchdog->info, NULL);
    VIR_FREE(watchdogstr);
    return ret;
}


int
qemuDomainAttachInputDevice(virQEMUDriverPtr driver,
                            virDomainObjPtr vm,
                            virDomainInputDefPtr input)
{
    int ret = -1;
    char *devstr = NULL;
    qemuDomainObjPrivatePtr priv = vm->privateData;
    virDomainDeviceDef dev = { VIR_DOMAIN_DEVICE_INPUT,
                               { .input = input } };
    virErrorPtr originalError = NULL;
    bool releaseaddr = false;
    bool teardowndevice = false;
    bool teardownlabel = false;
    bool teardowncgroup = false;

    if (input->bus != VIR_DOMAIN_INPUT_BUS_USB &&
        input->bus != VIR_DOMAIN_INPUT_BUS_VIRTIO) {
        virReportError(VIR_ERR_OPERATION_UNSUPPORTED,
                       _("input device on bus '%s' cannot be hot plugged."),
                       virDomainInputBusTypeToString(input->bus));
        return -1;
    }

    if (input->bus == VIR_DOMAIN_INPUT_BUS_VIRTIO) {
        if (qemuDomainEnsureVirtioAddress(&releaseaddr, vm, &dev, "input") < 0)
            return -1;
    } else if (input->bus == VIR_DOMAIN_INPUT_BUS_USB) {
        if (virDomainUSBAddressEnsure(priv->usbaddrs, &input->info) < 0)
            goto cleanup;
        releaseaddr = true;
    }

    if (qemuAssignDeviceInputAlias(vm->def, input, -1) < 0)
        goto cleanup;

    if (qemuBuildInputDevStr(&devstr, vm->def, input, priv->qemuCaps) < 0)
        goto cleanup;

    if (qemuDomainNamespaceSetupInput(vm, input) < 0)
        goto cleanup;
    teardowndevice = true;

    if (qemuSetupInputCgroup(vm, input) < 0)
        goto cleanup;
    teardowncgroup = true;

    if (qemuSecuritySetInputLabel(vm, input) < 0)
        goto cleanup;
    teardownlabel = true;

    if (VIR_REALLOC_N(vm->def->inputs, vm->def->ninputs + 1) < 0)
        goto cleanup;

    qemuDomainObjEnterMonitor(driver, vm);
    if (qemuMonitorAddDevice(priv->mon, devstr) < 0)
        goto exit_monitor;

    if (qemuDomainObjExitMonitor(driver, vm) < 0) {
        releaseaddr = false;
        goto cleanup;
    }

    VIR_APPEND_ELEMENT_COPY_INPLACE(vm->def->inputs, vm->def->ninputs, input);

    ret = 0;

 audit:
    virDomainAuditInput(vm, input, "attach", ret == 0);

 cleanup:
    if (ret < 0) {
        virErrorPreserveLast(&originalError);
        if (teardownlabel)
            qemuSecurityRestoreInputLabel(vm, input);
        if (teardowncgroup)
            qemuTeardownInputCgroup(vm, input);
        if (teardowndevice)
            qemuDomainNamespaceTeardownInput(vm, input);
        if (releaseaddr)
            qemuDomainReleaseDeviceAddress(vm, &input->info, NULL);
        virErrorRestore(&originalError);
    }

    VIR_FREE(devstr);
    return ret;

 exit_monitor:
    if (qemuDomainObjExitMonitor(driver, vm) < 0) {
        releaseaddr = false;
        goto cleanup;
    }
    goto audit;
}


int
qemuDomainAttachVsockDevice(virQEMUDriverPtr driver,
                            virDomainObjPtr vm,
                            virDomainVsockDefPtr vsock)
{
    qemuDomainVsockPrivatePtr vsockPriv = (qemuDomainVsockPrivatePtr)vsock->privateData;
    qemuDomainObjPrivatePtr priv = vm->privateData;
    virDomainDeviceDef dev = { VIR_DOMAIN_DEVICE_VSOCK,
                               { .vsock = vsock } };
    virErrorPtr originalError = NULL;
    const char *fdprefix = "vsockfd";
    bool releaseaddr = false;
    char *fdname = NULL;
    char *devstr = NULL;
    int ret = -1;

    if (vm->def->vsock) {
        virReportError(VIR_ERR_OPERATION_UNSUPPORTED, "%s",
                       _("the domain already has a vsock device"));
        return -1;
    }

    if (qemuDomainEnsureVirtioAddress(&releaseaddr, vm, &dev, "vsock") < 0)
        return -1;

    if (qemuAssignDeviceVsockAlias(vsock) < 0)
        goto cleanup;

    if (qemuProcessOpenVhostVsock(vsock) < 0)
        goto cleanup;

    if (virAsprintf(&fdname, "%s%u", fdprefix, vsockPriv->vhostfd) < 0)
        goto cleanup;

    if (!(devstr = qemuBuildVsockDevStr(vm->def, vsock, priv->qemuCaps, fdprefix)))
        goto cleanup;

    qemuDomainObjEnterMonitor(driver, vm);
    if (qemuMonitorAddDeviceWithFd(priv->mon, devstr, vsockPriv->vhostfd, fdname) < 0)
        goto exit_monitor;

    if (qemuDomainObjExitMonitor(driver, vm) < 0) {
        releaseaddr = false;
        goto cleanup;
    }

    VIR_STEAL_PTR(vm->def->vsock, vsock);

    ret = 0;

 cleanup:
    if (ret < 0) {
        virErrorPreserveLast(&originalError);
        if (releaseaddr)
            qemuDomainReleaseDeviceAddress(vm, &vsock->info, NULL);
        virErrorRestore(&originalError);
    }

    VIR_FREE(devstr);
    VIR_FREE(fdname);
    return ret;

 exit_monitor:
    if (qemuDomainObjExitMonitor(driver, vm) < 0)
        releaseaddr = false;
    goto cleanup;
}


static int
qemuDomainChangeNetBridge(virDomainObjPtr vm,
                          virDomainNetDefPtr olddev,
                          virDomainNetDefPtr newdev)
{
    int ret = -1;
    const char *oldbridge = virDomainNetGetActualBridgeName(olddev);
    const char *newbridge = virDomainNetGetActualBridgeName(newdev);

    if (!oldbridge || !newbridge) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s", _("Missing bridge name"));
        goto cleanup;
    }

    VIR_DEBUG("Change bridge for interface %s: %s -> %s",
              olddev->ifname, oldbridge, newbridge);

    if (virNetDevExists(newbridge) != 1) {
        virReportError(VIR_ERR_OPERATION_FAILED,
                       _("bridge %s doesn't exist"), newbridge);
        goto cleanup;
    }

    if (oldbridge) {
        ret = virNetDevBridgeRemovePort(oldbridge, olddev->ifname);
        virDomainAuditNet(vm, olddev, NULL, "detach", ret == 0);
        if (ret < 0) {
            /* warn but continue - possibly the old network
             * had been destroyed and reconstructed, leaving the
             * tap device orphaned.
             */
            VIR_WARN("Unable to detach device %s from bridge %s",
                     olddev->ifname, oldbridge);
        }
    }

    ret = virNetDevBridgeAddPort(newbridge, olddev->ifname);
    virDomainAuditNet(vm, NULL, newdev, "attach", ret == 0);
    if (ret < 0) {
        ret = virNetDevBridgeAddPort(oldbridge, olddev->ifname);
        virDomainAuditNet(vm, NULL, olddev, "attach", ret == 0);
        if (ret < 0) {
            virReportError(VIR_ERR_OPERATION_FAILED,
                           _("unable to recover former state by adding port "
                             "to bridge %s"), oldbridge);
        }
        goto cleanup;
    }
    /* caller will replace entire olddev with newdev in domain nets list */
    ret = 0;
 cleanup:
    return ret;
}

static int
qemuDomainChangeNetFilter(virDomainObjPtr vm,
                          virDomainNetDefPtr olddev,
                          virDomainNetDefPtr newdev)
{
    /* make sure this type of device supports filters. */
    switch (virDomainNetGetActualType(newdev)) {
    case VIR_DOMAIN_NET_TYPE_ETHERNET:
    case VIR_DOMAIN_NET_TYPE_BRIDGE:
    case VIR_DOMAIN_NET_TYPE_NETWORK:
        break;
    case VIR_DOMAIN_NET_TYPE_USER:
    case VIR_DOMAIN_NET_TYPE_VHOSTUSER:
    case VIR_DOMAIN_NET_TYPE_SERVER:
    case VIR_DOMAIN_NET_TYPE_CLIENT:
    case VIR_DOMAIN_NET_TYPE_MCAST:
    case VIR_DOMAIN_NET_TYPE_INTERNAL:
    case VIR_DOMAIN_NET_TYPE_DIRECT:
    case VIR_DOMAIN_NET_TYPE_HOSTDEV:
    case VIR_DOMAIN_NET_TYPE_UDP:
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("filters not supported on interfaces of type %s"),
                       virDomainNetTypeToString(virDomainNetGetActualType(newdev)));
        return -1;
    case VIR_DOMAIN_NET_TYPE_LAST:
    default:
        virReportEnumRangeError(virDomainNetType,
                                virDomainNetGetActualType(newdev));
        return -1;
    }

    virDomainConfNWFilterTeardown(olddev);

    if (newdev->filter &&
        virDomainConfNWFilterInstantiate(vm->def->name,
                                         vm->def->uuid, newdev, false) < 0) {
        virErrorPtr errobj;

        virReportError(VIR_ERR_OPERATION_FAILED,
                       _("failed to add new filter rules to '%s' "
                         "- attempting to restore old rules"),
                       olddev->ifname);
        virErrorPreserveLast(&errobj);
        ignore_value(virDomainConfNWFilterInstantiate(vm->def->name,
                                                      vm->def->uuid, olddev, false));
        virErrorRestore(&errobj);
        return -1;
    }
    return 0;
}

int qemuDomainChangeNetLinkState(virQEMUDriverPtr driver,
                                 virDomainObjPtr vm,
                                 virDomainNetDefPtr dev,
                                 int linkstate)
{
    int ret = -1;
    qemuDomainObjPrivatePtr priv = vm->privateData;

    if (!dev->info.alias) {
        virReportError(VIR_ERR_OPERATION_FAILED, "%s",
                       _("can't change link state: device alias not found"));
        return -1;
    }

    VIR_DEBUG("dev: %s, state: %d", dev->info.alias, linkstate);

    qemuDomainObjEnterMonitor(driver, vm);

    ret = qemuMonitorSetLink(priv->mon, dev->info.alias, linkstate);
    if (ret < 0)
        goto cleanup;

    /* modify the device configuration */
    dev->linkstate = linkstate;

 cleanup:
    if (qemuDomainObjExitMonitor(driver, vm) < 0)
        return -1;

    return ret;
}

int
qemuDomainChangeNet(virQEMUDriverPtr driver,
                    virDomainObjPtr vm,
                    virDomainDeviceDefPtr dev)
{
    virDomainNetDefPtr newdev = dev->data.net;
    virDomainNetDefPtr *devslot = NULL;
    virDomainNetDefPtr olddev;
    virDomainNetType oldType, newType;
    bool needReconnect = false;
    bool needBridgeChange = false;
    bool needFilterChange = false;
    bool needLinkStateChange = false;
    bool needReplaceDevDef = false;
    bool needBandwidthSet = false;
    bool needCoalesceChange = false;
    bool needVlanUpdate = false;
    int ret = -1;
    int changeidx = -1;

    if ((changeidx = virDomainNetFindIdx(vm->def, newdev)) < 0)
        goto cleanup;
    devslot = &vm->def->nets[changeidx];
    olddev = *devslot;

    oldType = virDomainNetGetActualType(olddev);
    if (oldType == VIR_DOMAIN_NET_TYPE_HOSTDEV) {
        /* no changes are possible to a type='hostdev' interface */
        virReportError(VIR_ERR_OPERATION_UNSUPPORTED,
                       _("cannot change config of '%s' network type"),
                       virDomainNetTypeToString(oldType));
        goto cleanup;
    }

    /* Check individual attributes for changes that can't be done to a
     * live netdev. These checks *mostly* go in order of the
     * declarations in virDomainNetDef in order to assure nothing is
     * omitted. (exceptiong where noted in comments - in particular,
     * some things require that a new "actual device" be allocated
     * from the network driver first, but we delay doing that until
     * after we've made as many other checks as possible)
     */

    /* type: this can change (with some restrictions), but the actual
     * type of the new device connection isn't known until after we
     * allocate the "actual" device.
     */

    if (virMacAddrCmp(&olddev->mac, &newdev->mac)) {
        char oldmac[VIR_MAC_STRING_BUFLEN], newmac[VIR_MAC_STRING_BUFLEN];

        virReportError(VIR_ERR_OPERATION_UNSUPPORTED,
                       _("cannot change network interface mac address "
                         "from %s to %s"),
                       virMacAddrFormat(&olddev->mac, oldmac),
                       virMacAddrFormat(&newdev->mac, newmac));
        goto cleanup;
    }

    if (STRNEQ_NULLABLE(olddev->model, newdev->model)) {
        virReportError(VIR_ERR_OPERATION_UNSUPPORTED,
                       _("cannot modify network device model from %s to %s"),
                       olddev->model ? olddev->model : "(default)",
                       newdev->model ? newdev->model : "(default)");
        goto cleanup;
    }

    if (olddev->model && STREQ(olddev->model, "virtio") &&
        (olddev->driver.virtio.name != newdev->driver.virtio.name ||
         olddev->driver.virtio.txmode != newdev->driver.virtio.txmode ||
         olddev->driver.virtio.ioeventfd != newdev->driver.virtio.ioeventfd ||
         olddev->driver.virtio.event_idx != newdev->driver.virtio.event_idx ||
         olddev->driver.virtio.queues != newdev->driver.virtio.queues ||
         olddev->driver.virtio.rx_queue_size != newdev->driver.virtio.rx_queue_size ||
         olddev->driver.virtio.tx_queue_size != newdev->driver.virtio.tx_queue_size ||
         olddev->driver.virtio.host.csum != newdev->driver.virtio.host.csum ||
         olddev->driver.virtio.host.gso != newdev->driver.virtio.host.gso ||
         olddev->driver.virtio.host.tso4 != newdev->driver.virtio.host.tso4 ||
         olddev->driver.virtio.host.tso6 != newdev->driver.virtio.host.tso6 ||
         olddev->driver.virtio.host.ecn != newdev->driver.virtio.host.ecn ||
         olddev->driver.virtio.host.ufo != newdev->driver.virtio.host.ufo ||
         olddev->driver.virtio.host.mrg_rxbuf != newdev->driver.virtio.host.mrg_rxbuf ||
         olddev->driver.virtio.guest.csum != newdev->driver.virtio.guest.csum ||
         olddev->driver.virtio.guest.tso4 != newdev->driver.virtio.guest.tso4 ||
         olddev->driver.virtio.guest.tso6 != newdev->driver.virtio.guest.tso6 ||
         olddev->driver.virtio.guest.ecn != newdev->driver.virtio.guest.ecn ||
         olddev->driver.virtio.guest.ufo != newdev->driver.virtio.guest.ufo)) {
        virReportError(VIR_ERR_OPERATION_UNSUPPORTED, "%s",
                       _("cannot modify virtio network device driver attributes"));
        goto cleanup;
    }

    /* data: this union will be examined later, after allocating new actualdev */
    /* virtPortProfile: will be examined later, after allocating new actualdev */

    if (olddev->tune.sndbuf_specified != newdev->tune.sndbuf_specified ||
        olddev->tune.sndbuf != newdev->tune.sndbuf) {
        needReconnect = true;
    }

    if (STRNEQ_NULLABLE(olddev->script, newdev->script)) {
        virReportError(VIR_ERR_OPERATION_UNSUPPORTED, "%s",
                       _("cannot modify network device script attribute"));
        goto cleanup;
    }

    /* ifname: check if it's set in newdev. If not, retain the autogenerated one */
    if (!newdev->ifname && VIR_STRDUP(newdev->ifname, olddev->ifname) < 0)
        goto cleanup;
    if (STRNEQ_NULLABLE(olddev->ifname, newdev->ifname)) {
        virReportError(VIR_ERR_OPERATION_UNSUPPORTED, "%s",
                       _("cannot modify network device tap name"));
        goto cleanup;
    }

    /* info: Nothing is allowed to change. First fill the missing newdev->info
     * from olddev and then check for changes.
     */
    /* if pci addr is missing or is invalid we overwrite it from olddev */
    if (newdev->info.type == VIR_DOMAIN_DEVICE_ADDRESS_TYPE_NONE ||
        !virDomainDeviceAddressIsValid(&newdev->info,
                                       newdev->info.type)) {
        newdev->info.type = olddev->info.type;
        newdev->info.addr = olddev->info.addr;
    }
    if (olddev->info.type != newdev->info.type) {
        virReportError(VIR_ERR_OPERATION_UNSUPPORTED, "%s",
                       _("cannot modify network device address type"));
    }
    if (!virPCIDeviceAddressEqual(&olddev->info.addr.pci,
                                  &newdev->info.addr.pci)) {
        virReportError(VIR_ERR_OPERATION_UNSUPPORTED, "%s",
                       _("cannot modify network device guest PCI address"));
        goto cleanup;
    }
    /* grab alias from olddev if not set in newdev */
    if (!newdev->info.alias &&
        VIR_STRDUP(newdev->info.alias, olddev->info.alias) < 0)
        goto cleanup;

    /* device alias is checked already in virDomainDefCompatibleDevice */

    if (newdev->info.rombar == VIR_TRISTATE_BOOL_ABSENT)
        newdev->info.rombar = olddev->info.rombar;
    if (olddev->info.rombar != newdev->info.rombar) {
        virReportError(VIR_ERR_OPERATION_UNSUPPORTED, "%s",
                       _("cannot modify network device rom bar setting"));
        goto cleanup;
    }

    if (!newdev->info.romfile &&
        VIR_STRDUP(newdev->info.romfile, olddev->info.romfile) < 0)
        goto cleanup;
    if (STRNEQ_NULLABLE(olddev->info.romfile, newdev->info.romfile)) {
        virReportError(VIR_ERR_OPERATION_UNSUPPORTED, "%s",
                       _("cannot modify network rom file"));
        goto cleanup;
    }

    if (newdev->info.bootIndex == 0)
        newdev->info.bootIndex = olddev->info.bootIndex;
    if (olddev->info.bootIndex != newdev->info.bootIndex) {
        virReportError(VIR_ERR_OPERATION_UNSUPPORTED, "%s",
                       _("cannot modify network device boot index setting"));
        goto cleanup;
    }

    if (newdev->info.romenabled == VIR_TRISTATE_BOOL_ABSENT)
        newdev->info.romenabled = olddev->info.romenabled;
    if (olddev->info.romenabled != newdev->info.romenabled) {
        virReportError(VIR_ERR_OPERATION_UNSUPPORTED, "%s",
                       _("cannot modify network device rom enabled setting"));
        goto cleanup;
    }
    /* (end of device info checks) */

    if (STRNEQ_NULLABLE(olddev->filter, newdev->filter) ||
        !virNWFilterHashTableEqual(olddev->filterparams, newdev->filterparams)) {
        needFilterChange = true;
    }

    /* bandwidth can be modified, and will be checked later */
    /* vlan can be modified, and will be checked later */
    /* linkstate can be modified */

    if (olddev->mtu != newdev->mtu) {
        virReportError(VIR_ERR_OPERATION_UNSUPPORTED, "%s",
                       _("cannot modify MTU"));
        goto cleanup;
    }

    /* allocate new actual device to compare to old - we will need to
     * free it if we fail for any reason
     */
    if (newdev->type == VIR_DOMAIN_NET_TYPE_NETWORK &&
        virDomainNetAllocateActualDevice(vm->def, newdev) < 0) {
        goto cleanup;
    }

    newType = virDomainNetGetActualType(newdev);

    if (newType == VIR_DOMAIN_NET_TYPE_HOSTDEV) {
        /* can't turn it into a type='hostdev' interface */
        virReportError(VIR_ERR_OPERATION_UNSUPPORTED,
                       _("cannot change network interface type to '%s'"),
                       virDomainNetTypeToString(newType));
        goto cleanup;
    }

    if (olddev->type == newdev->type && oldType == newType) {

        /* if type hasn't changed, check the relevant fields for the type */
        switch (newdev->type) {
        case VIR_DOMAIN_NET_TYPE_USER:
            break;

        case VIR_DOMAIN_NET_TYPE_ETHERNET:
            break;

        case VIR_DOMAIN_NET_TYPE_SERVER:
        case VIR_DOMAIN_NET_TYPE_CLIENT:
        case VIR_DOMAIN_NET_TYPE_MCAST:
        case VIR_DOMAIN_NET_TYPE_UDP:
            if (STRNEQ_NULLABLE(olddev->data.socket.address,
                                newdev->data.socket.address) ||
                olddev->data.socket.port != newdev->data.socket.port) {
                needReconnect = true;
            }
            break;

        case VIR_DOMAIN_NET_TYPE_NETWORK:
            if (STRNEQ(olddev->data.network.name, newdev->data.network.name)) {
                if (virDomainNetGetActualVirtPortProfile(newdev))
                    needReconnect = true;
                else
                    needBridgeChange = true;
            }
            /* other things handled in common code directly below this switch */
            break;

        case VIR_DOMAIN_NET_TYPE_BRIDGE:
            /* all handled in bridge name checked in common code below */
            break;

        case VIR_DOMAIN_NET_TYPE_INTERNAL:
            if (STRNEQ_NULLABLE(olddev->data.internal.name,
                                newdev->data.internal.name)) {
                needReconnect = true;
            }
            break;

        case VIR_DOMAIN_NET_TYPE_DIRECT:
            /* all handled in common code directly below this switch */
            break;

        case VIR_DOMAIN_NET_TYPE_VHOSTUSER:
        case VIR_DOMAIN_NET_TYPE_HOSTDEV:
            virReportError(VIR_ERR_OPERATION_UNSUPPORTED,
                           _("unable to change config on '%s' network type"),
                           virDomainNetTypeToString(newdev->type));
            goto cleanup;
        case VIR_DOMAIN_NET_TYPE_LAST:
        default:
            virReportEnumRangeError(virDomainNetType, newdev->type);
            goto cleanup;
        }
    } else {
        /* interface type has changed. There are a few special cases
         * where this can only require a minor (or even no) change,
         * but in most cases we need to do a full reconnection.
         *
         * If we switch (in either direction) between type='bridge'
         * and type='network' (for a traditional managed virtual
         * network that uses a host bridge, i.e. forward
         * mode='route|nat'), we just need to change the bridge.
         */
        if ((oldType == VIR_DOMAIN_NET_TYPE_NETWORK &&
             newType == VIR_DOMAIN_NET_TYPE_BRIDGE) ||
            (oldType == VIR_DOMAIN_NET_TYPE_BRIDGE &&
             newType == VIR_DOMAIN_NET_TYPE_NETWORK)) {

            needBridgeChange = true;

        } else if (oldType == VIR_DOMAIN_NET_TYPE_DIRECT &&
                   newType == VIR_DOMAIN_NET_TYPE_DIRECT) {

            /* this is the case of switching from type='direct' to
             * type='network' for a network that itself uses direct
             * (macvtap) devices. If the physical device and mode are
             * the same, this doesn't require any actual setup
             * change. If the physical device or mode *does* change,
             * that will be caught in the common section below */

        } else {

            /* for all other combinations, we'll need a full reconnect */
            needReconnect = true;

        }
    }

    /* now several things that are in multiple (but not all)
     * different types, and can be safely compared even for those
     * cases where they don't apply to a particular type.
     */
    if (STRNEQ_NULLABLE(virDomainNetGetActualBridgeName(olddev),
                        virDomainNetGetActualBridgeName(newdev))) {
        if (virDomainNetGetActualVirtPortProfile(newdev))
            needReconnect = true;
        else
            needBridgeChange = true;
    }

    if (STRNEQ_NULLABLE(virDomainNetGetActualDirectDev(olddev),
                        virDomainNetGetActualDirectDev(newdev)) ||
        virDomainNetGetActualDirectMode(olddev) != virDomainNetGetActualDirectMode(newdev) ||
        !virNetDevVPortProfileEqual(virDomainNetGetActualVirtPortProfile(olddev),
                                    virDomainNetGetActualVirtPortProfile(newdev))) {
        needReconnect = true;
    }

    if (!virNetDevVlanEqual(virDomainNetGetActualVlan(olddev),
                             virDomainNetGetActualVlan(newdev))) {
        needVlanUpdate = true;
    }

    if (olddev->linkstate != newdev->linkstate)
        needLinkStateChange = true;

    if (!virNetDevBandwidthEqual(virDomainNetGetActualBandwidth(olddev),
                                 virDomainNetGetActualBandwidth(newdev)))
        needBandwidthSet = true;

    if (!!olddev->coalesce != !!newdev->coalesce ||
        (olddev->coalesce && newdev->coalesce &&
         memcmp(olddev->coalesce, newdev->coalesce,
                sizeof(*olddev->coalesce))))
        needCoalesceChange = true;

    /* FINALLY - actually perform the required actions */

    if (needReconnect) {
        virReportError(VIR_ERR_OPERATION_UNSUPPORTED,
                       _("unable to change config on '%s' network type"),
                       virDomainNetTypeToString(newdev->type));
        goto cleanup;
    }

    if (needBandwidthSet) {
        virNetDevBandwidthPtr newb = virDomainNetGetActualBandwidth(newdev);

        if (newb) {
            if (virNetDevBandwidthSet(newdev->ifname, newb, false,
                                      !virDomainNetTypeSharesHostView(newdev)) < 0)
                goto cleanup;
        } else {
            /*
             * virNetDevBandwidthSet() doesn't clear any existing
             * setting unless something new is being set.
             */
            virNetDevBandwidthClear(newdev->ifname);
        }
        needReplaceDevDef = true;
    }

    if (needBridgeChange) {
        if (qemuDomainChangeNetBridge(vm, olddev, newdev) < 0)
            goto cleanup;
        /* we successfully switched to the new bridge, and we've
         * determined that the rest of newdev is equivalent to olddev,
         * so move newdev into place */
        needReplaceDevDef = true;
    }

    if (needFilterChange) {
        if (qemuDomainChangeNetFilter(vm, olddev, newdev) < 0)
            goto cleanup;
        /* we successfully switched to the new filter, and we've
         * determined that the rest of newdev is equivalent to olddev,
         * so move newdev into place */
        needReplaceDevDef = true;
    }

    if (needCoalesceChange) {
        if (virNetDevSetCoalesce(newdev->ifname, newdev->coalesce, true) < 0)
            goto cleanup;
        needReplaceDevDef = true;
    }

    if (needLinkStateChange &&
        qemuDomainChangeNetLinkState(driver, vm, olddev, newdev->linkstate) < 0) {
        goto cleanup;
    }

    if (needVlanUpdate) {
        if (virNetDevOpenvswitchUpdateVlan(newdev->ifname, &newdev->vlan) < 0)
            goto cleanup;
        needReplaceDevDef = true;
    }

    if (needReplaceDevDef) {
        /* the changes above warrant replacing olddev with newdev in
         * the domain's nets list.
         */

        /* this function doesn't work with HOSTDEV networks yet, thus
         * no need to change the pointer in the hostdev structure */
        virDomainNetReleaseActualDevice(vm->def, olddev);
        virDomainNetDefFree(olddev);
        /* move newdev into the nets list, and NULL it out from the
         * virDomainDeviceDef that we were given so that the caller
         * won't delete it on return.
         */
        *devslot = newdev;
        newdev = dev->data.net = NULL;
        dev->type = VIR_DOMAIN_DEVICE_NONE;
    }

    ret = 0;
 cleanup:
    /* When we get here, we will be in one of these two states:
     *
     * 1) newdev has been moved into the domain's list of nets and
     *    newdev set to NULL, and dev->data.net will be NULL (and
     *    dev->type is NONE). olddev will have been completely
     *    released and freed. (aka success) In this case no extra
     *    cleanup is needed.
     *
     * 2) newdev has *not* been moved into the domain's list of nets,
     *    and dev->data.net == newdev (and dev->type == NET). In this *
     *    case, we need to at least release the "actual device" from *
     *    newdev (the caller will free dev->data.net a.k.a. newdev, and
     *    the original olddev is still in used)
     *
     * Note that case (2) isn't necessarily a failure. It may just be
     * that the changes were minor enough that we didn't need to
     * replace the entire device object.
     */
    if (newdev)
        virDomainNetReleaseActualDevice(vm->def, newdev);

    return ret;
}

static virDomainGraphicsDefPtr
qemuDomainFindGraphics(virDomainObjPtr vm,
                       virDomainGraphicsDefPtr dev)
{
    size_t i;

    for (i = 0; i < vm->def->ngraphics; i++) {
        if (vm->def->graphics[i]->type == dev->type)
            return vm->def->graphics[i];
    }

    return NULL;
}

int
qemuDomainFindGraphicsIndex(virDomainDefPtr def,
                            virDomainGraphicsDefPtr dev)
{
    size_t i;

    for (i = 0; i < def->ngraphics; i++) {
        if (def->graphics[i]->type == dev->type)
            return i;
    }

    return -1;
}

int
qemuDomainChangeGraphics(virQEMUDriverPtr driver,
                         virDomainObjPtr vm,
                         virDomainGraphicsDefPtr dev)
{
    virDomainGraphicsDefPtr olddev = qemuDomainFindGraphics(vm, dev);
    virQEMUDriverConfigPtr cfg = virQEMUDriverGetConfig(driver);
    const char *type = virDomainGraphicsTypeToString(dev->type);
    size_t i;
    int ret = -1;

    if (!olddev) {
        virReportError(VIR_ERR_DEVICE_MISSING,
                       _("cannot find existing graphics device to modify of "
                         "type '%s'"), type);
        goto cleanup;
    }

    if (dev->nListens != olddev->nListens) {
        virReportError(VIR_ERR_OPERATION_UNSUPPORTED,
                       _("cannot change the number of listen addresses "
                         "on '%s' graphics"), type);
        goto cleanup;
    }

    for (i = 0; i < dev->nListens; i++) {
        virDomainGraphicsListenDefPtr newlisten = &dev->listens[i];
        virDomainGraphicsListenDefPtr oldlisten = &olddev->listens[i];

        if (newlisten->type != oldlisten->type) {
            virReportError(VIR_ERR_OPERATION_UNSUPPORTED,
                           _("cannot change the type of listen address "
                             "on '%s' graphics"), type);
            goto cleanup;
        }

        switch (newlisten->type) {
        case VIR_DOMAIN_GRAPHICS_LISTEN_TYPE_ADDRESS:
            if (STRNEQ_NULLABLE(newlisten->address, oldlisten->address)) {
                virReportError(VIR_ERR_OPERATION_UNSUPPORTED,
                               _("cannot change listen address setting "
                                 "on '%s' graphics"), type);
                goto cleanup;
            }

            break;

        case VIR_DOMAIN_GRAPHICS_LISTEN_TYPE_NETWORK:
            if (STRNEQ_NULLABLE(newlisten->network, oldlisten->network)) {
                virReportError(VIR_ERR_OPERATION_UNSUPPORTED,
                               _("cannot change listen address setting "
                                 "on '%s' graphics"), type);
                goto cleanup;
            }

            break;

        case VIR_DOMAIN_GRAPHICS_LISTEN_TYPE_SOCKET:
            if (STRNEQ_NULLABLE(newlisten->socket, oldlisten->socket)) {
                virReportError(VIR_ERR_OPERATION_UNSUPPORTED,
                               _("cannot change listen socket setting "
                                 "on '%s' graphics"), type);
                goto cleanup;
            }
            break;

        case VIR_DOMAIN_GRAPHICS_LISTEN_TYPE_NONE:
        case VIR_DOMAIN_GRAPHICS_LISTEN_TYPE_LAST:
            /* nada */
            break;
        }
    }

    switch (dev->type) {
    case VIR_DOMAIN_GRAPHICS_TYPE_VNC:
        if ((olddev->data.vnc.autoport != dev->data.vnc.autoport) ||
            (!dev->data.vnc.autoport &&
             (olddev->data.vnc.port != dev->data.vnc.port))) {
            virReportError(VIR_ERR_OPERATION_UNSUPPORTED, "%s",
                           _("cannot change port settings on vnc graphics"));
            goto cleanup;
        }
        if (STRNEQ_NULLABLE(olddev->data.vnc.keymap, dev->data.vnc.keymap)) {
            virReportError(VIR_ERR_OPERATION_UNSUPPORTED, "%s",
                           _("cannot change keymap setting on vnc graphics"));
            goto cleanup;
        }

        /* If a password lifetime was, or is set, or action if connected has
         * changed, then we must always run, even if new password matches
         * old password */
        if (olddev->data.vnc.auth.expires ||
            dev->data.vnc.auth.expires ||
            olddev->data.vnc.auth.connected != dev->data.vnc.auth.connected ||
            STRNEQ_NULLABLE(olddev->data.vnc.auth.passwd,
                            dev->data.vnc.auth.passwd)) {
            VIR_DEBUG("Updating password on VNC server %p %p",
                      dev->data.vnc.auth.passwd, cfg->vncPassword);
            ret = qemuDomainChangeGraphicsPasswords(driver, vm,
                                                    VIR_DOMAIN_GRAPHICS_TYPE_VNC,
                                                    &dev->data.vnc.auth,
                                                    cfg->vncPassword,
                                                    QEMU_ASYNC_JOB_NONE);
            if (ret < 0)
                goto cleanup;

            /* Steal the new dev's  char * reference */
            VIR_FREE(olddev->data.vnc.auth.passwd);
            olddev->data.vnc.auth.passwd = dev->data.vnc.auth.passwd;
            dev->data.vnc.auth.passwd = NULL;
            olddev->data.vnc.auth.validTo = dev->data.vnc.auth.validTo;
            olddev->data.vnc.auth.expires = dev->data.vnc.auth.expires;
            olddev->data.vnc.auth.connected = dev->data.vnc.auth.connected;
        } else {
            ret = 0;
        }
        break;

    case VIR_DOMAIN_GRAPHICS_TYPE_SPICE:
        if ((olddev->data.spice.autoport != dev->data.spice.autoport) ||
            (!dev->data.spice.autoport &&
             (olddev->data.spice.port != dev->data.spice.port)) ||
            (!dev->data.spice.autoport &&
             (olddev->data.spice.tlsPort != dev->data.spice.tlsPort))) {
            virReportError(VIR_ERR_OPERATION_UNSUPPORTED, "%s",
                           _("cannot change port settings on spice graphics"));
            goto cleanup;
        }
        if (STRNEQ_NULLABLE(olddev->data.spice.keymap,
                            dev->data.spice.keymap)) {
            virReportError(VIR_ERR_OPERATION_UNSUPPORTED, "%s",
                            _("cannot change keymap setting on spice graphics"));
            goto cleanup;
        }

        /* We must reset the password if it has changed but also if:
         * - password lifetime is or was set
         * - the requested action has changed
         * - the action is "disconnect"
         */
        if (olddev->data.spice.auth.expires ||
            dev->data.spice.auth.expires ||
            olddev->data.spice.auth.connected != dev->data.spice.auth.connected ||
            dev->data.spice.auth.connected ==
            VIR_DOMAIN_GRAPHICS_AUTH_CONNECTED_DISCONNECT ||
            STRNEQ_NULLABLE(olddev->data.spice.auth.passwd,
                            dev->data.spice.auth.passwd)) {
            VIR_DEBUG("Updating password on SPICE server %p %p",
                      dev->data.spice.auth.passwd, cfg->spicePassword);
            ret = qemuDomainChangeGraphicsPasswords(driver, vm,
                                                    VIR_DOMAIN_GRAPHICS_TYPE_SPICE,
                                                    &dev->data.spice.auth,
                                                    cfg->spicePassword,
                                                    QEMU_ASYNC_JOB_NONE);

            if (ret < 0)
                goto cleanup;

            /* Steal the new dev's char * reference */
            VIR_FREE(olddev->data.spice.auth.passwd);
            olddev->data.spice.auth.passwd = dev->data.spice.auth.passwd;
            dev->data.spice.auth.passwd = NULL;
            olddev->data.spice.auth.validTo = dev->data.spice.auth.validTo;
            olddev->data.spice.auth.expires = dev->data.spice.auth.expires;
            olddev->data.spice.auth.connected = dev->data.spice.auth.connected;
        } else {
            VIR_DEBUG("Not updating since password didn't change");
            ret = 0;
        }
        break;

    case VIR_DOMAIN_GRAPHICS_TYPE_SDL:
    case VIR_DOMAIN_GRAPHICS_TYPE_RDP:
    case VIR_DOMAIN_GRAPHICS_TYPE_DESKTOP:
    case VIR_DOMAIN_GRAPHICS_TYPE_EGL_HEADLESS:
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("unable to change config on '%s' graphics type"), type);
        break;
    case VIR_DOMAIN_GRAPHICS_TYPE_LAST:
    default:
        virReportEnumRangeError(virDomainGraphicsType, dev->type);
        break;
    }

 cleanup:
    virObjectUnref(cfg);
    return ret;
}


static int qemuComparePCIDevice(virDomainDefPtr def ATTRIBUTE_UNUSED,
                                virDomainDeviceDefPtr device ATTRIBUTE_UNUSED,
                                virDomainDeviceInfoPtr info1,
                                void *opaque)
{
    virDomainDeviceInfoPtr info2 = opaque;

    if (info1->type != VIR_DOMAIN_DEVICE_ADDRESS_TYPE_PCI ||
        info2->type != VIR_DOMAIN_DEVICE_ADDRESS_TYPE_PCI)
        return 0;

    if (info1->addr.pci.domain == info2->addr.pci.domain &&
        info1->addr.pci.bus == info2->addr.pci.bus &&
        info1->addr.pci.slot == info2->addr.pci.slot &&
        info1->addr.pci.function != info2->addr.pci.function)
        return -1;
    return 0;
}

static bool qemuIsMultiFunctionDevice(virDomainDefPtr def,
                                      virDomainDeviceInfoPtr dev)
{
    if (dev->type != VIR_DOMAIN_DEVICE_ADDRESS_TYPE_PCI)
        return false;

    if (virDomainDeviceInfoIterate(def, qemuComparePCIDevice, dev) < 0)
        return true;
    return false;
}


static int
qemuDomainRemoveDiskDevice(virQEMUDriverPtr driver,
                           virDomainObjPtr vm,
                           virDomainDiskDefPtr disk)
{
    qemuHotplugDiskSourceDataPtr diskbackend = NULL;
    virDomainDeviceDef dev;
    virObjectEventPtr event;
    size_t i;
    qemuDomainObjPrivatePtr priv = vm->privateData;
    int ret = -1;

    VIR_DEBUG("Removing disk %s from domain %p %s",
              disk->info.alias, vm, vm->def->name);

    if (!(diskbackend = qemuHotplugDiskSourceRemovePrepare(disk, disk->src,
                                                           priv->qemuCaps)))
        return -1;

    for (i = 0; i < vm->def->ndisks; i++) {
        if (vm->def->disks[i] == disk) {
            virDomainDiskRemove(vm->def, i);
            break;
        }
    }

    qemuDomainObjEnterMonitor(driver, vm);

    qemuHotplugDiskSourceRemove(priv->mon, diskbackend);

    if (qemuDomainObjExitMonitor(driver, vm) < 0)
        goto cleanup;

    virDomainAuditDisk(vm, disk->src, NULL, "detach", true);

    event = virDomainEventDeviceRemovedNewFromObj(vm, disk->info.alias);
    virObjectEventStateQueue(driver->domainEventState, event);

    qemuDomainReleaseDeviceAddress(vm, &disk->info, virDomainDiskGetSource(disk));

    /* tear down disk security access */
    qemuHotplugPrepareDiskAccess(driver, vm, disk, NULL, true);

    dev.type = VIR_DOMAIN_DEVICE_DISK;
    dev.data.disk = disk;
    ignore_value(qemuRemoveSharedDevice(driver, &dev, vm->def->name));
    virDomainUSBAddressRelease(priv->usbaddrs, &disk->info);

    if (qemuHotplugRemoveManagedPR(driver, vm, QEMU_ASYNC_JOB_NONE) < 0)
        goto cleanup;

    ret = 0;

 cleanup:
    qemuHotplugDiskSourceDataFree(diskbackend);
    virDomainDiskDefFree(disk);
    return ret;
}


static int
qemuDomainRemoveControllerDevice(virQEMUDriverPtr driver,
                                 virDomainObjPtr vm,
                                 virDomainControllerDefPtr controller)
{
    virObjectEventPtr event;
    size_t i;

    VIR_DEBUG("Removing controller %s from domain %p %s",
              controller->info.alias, vm, vm->def->name);

    event = virDomainEventDeviceRemovedNewFromObj(vm, controller->info.alias);
    virObjectEventStateQueue(driver->domainEventState, event);

    for (i = 0; i < vm->def->ncontrollers; i++) {
        if (vm->def->controllers[i] == controller) {
            virDomainControllerRemove(vm->def, i);
            break;
        }
    }

    qemuDomainReleaseDeviceAddress(vm, &controller->info, NULL);
    virDomainControllerDefFree(controller);
    return 0;
}


static int
qemuDomainRemoveMemoryDevice(virQEMUDriverPtr driver,
                             virDomainObjPtr vm,
                             virDomainMemoryDefPtr mem)
{
    qemuDomainObjPrivatePtr priv = vm->privateData;
    unsigned long long oldmem = virDomainDefGetMemoryTotal(vm->def);
    unsigned long long newmem = oldmem - mem->size;
    virObjectEventPtr event;
    char *backendAlias = NULL;
    int rc;
    int idx;

    VIR_DEBUG("Removing memory device %s from domain %p %s",
              mem->info.alias, vm, vm->def->name);

    if (virAsprintf(&backendAlias, "mem%s", mem->info.alias) < 0)
        return -1;

    qemuDomainObjEnterMonitor(driver, vm);
    rc = qemuMonitorDelObject(priv->mon, backendAlias);
    if (qemuDomainObjExitMonitor(driver, vm) < 0)
        rc = -1;

    VIR_FREE(backendAlias);

    virDomainAuditMemory(vm, oldmem, newmem, "update", rc == 0);
    if (rc < 0)
        return -1;

    event = virDomainEventDeviceRemovedNewFromObj(vm, mem->info.alias);
    virObjectEventStateQueue(driver->domainEventState, event);

    if ((idx = virDomainMemoryFindByDef(vm->def, mem)) >= 0)
        virDomainMemoryRemove(vm->def, idx);

    if (qemuSecurityRestoreMemoryLabel(driver, vm, mem) < 0)
        VIR_WARN("Unable to restore security label on memdev");

    if (qemuTeardownMemoryDevicesCgroup(vm, mem) < 0)
        VIR_WARN("Unable to remove memory device cgroup ACL");

    if (qemuDomainNamespaceTeardownMemory(vm, mem) <  0)
        VIR_WARN("Unable to remove memory device from /dev");

    if (qemuProcessDestroyMemoryBackingPath(driver, vm, mem) < 0)
        VIR_WARN("Unable to destroy memory backing path");

    virDomainMemoryDefFree(mem);

    /* fix the balloon size */
    ignore_value(qemuProcessRefreshBalloonState(driver, vm, QEMU_ASYNC_JOB_NONE));

    /* decrease the mlock limit after memory unplug if necessary */
    ignore_value(qemuDomainAdjustMaxMemLock(vm));

    return 0;
}


static void
qemuDomainRemovePCIHostDevice(virQEMUDriverPtr driver,
                              virDomainObjPtr vm,
                              virDomainHostdevDefPtr hostdev)
{
    qemuHostdevReAttachPCIDevices(driver, vm->def->name, &hostdev, 1);
    qemuDomainReleaseDeviceAddress(vm, hostdev->info, NULL);
}

static void
qemuDomainRemoveUSBHostDevice(virQEMUDriverPtr driver,
                              virDomainObjPtr vm,
                              virDomainHostdevDefPtr hostdev)
{
    qemuHostdevReAttachUSBDevices(driver, vm->def->name, &hostdev, 1);
    qemuDomainReleaseDeviceAddress(vm, hostdev->info, NULL);
}

static void
qemuDomainRemoveSCSIHostDevice(virQEMUDriverPtr driver,
                               virDomainObjPtr vm,
                               virDomainHostdevDefPtr hostdev)
{
    qemuHostdevReAttachSCSIDevices(driver, vm->def->name, &hostdev, 1);
}

static void
qemuDomainRemoveSCSIVHostDevice(virQEMUDriverPtr driver,
                                virDomainObjPtr vm,
                                virDomainHostdevDefPtr hostdev)
{
    qemuHostdevReAttachSCSIVHostDevices(driver, vm->def->name, &hostdev, 1);
}


static void
qemuDomainRemoveMediatedDevice(virQEMUDriverPtr driver,
                               virDomainObjPtr vm,
                               virDomainHostdevDefPtr hostdev)
{
    qemuHostdevReAttachMediatedDevices(driver, vm->def->name, &hostdev, 1);
    qemuDomainReleaseDeviceAddress(vm, hostdev->info, NULL);
}


static int
qemuDomainRemoveHostDevice(virQEMUDriverPtr driver,
                           virDomainObjPtr vm,
                           virDomainHostdevDefPtr hostdev)
{
    virQEMUDriverConfigPtr cfg = virQEMUDriverGetConfig(driver);
    virDomainNetDefPtr net = NULL;
    virObjectEventPtr event;
    size_t i;
    int ret = -1;
    qemuDomainObjPrivatePtr priv = vm->privateData;
    char *drivealias = NULL;
    char *objAlias = NULL;
    bool is_vfio = false;

    VIR_DEBUG("Removing host device %s from domain %p %s",
              hostdev->info->alias, vm, vm->def->name);

    if (hostdev->source.subsys.type == VIR_DOMAIN_HOSTDEV_SUBSYS_TYPE_PCI) {
        int backend = hostdev->source.subsys.u.pci.backend;
        is_vfio = backend == VIR_DOMAIN_HOSTDEV_PCI_BACKEND_VFIO;
    }

    if (hostdev->source.subsys.type == VIR_DOMAIN_HOSTDEV_SUBSYS_TYPE_SCSI) {
        virDomainHostdevSubsysSCSIPtr scsisrc = &hostdev->source.subsys.u.scsi;
        virDomainHostdevSubsysSCSIiSCSIPtr iscsisrc = &scsisrc->u.iscsi;

        if (!(drivealias = qemuAliasFromHostdev(hostdev)))
            goto cleanup;

        /* Look for the markers that the iSCSI hostdev was added with a
         * secret object to manage the username/password. If present, let's
         * attempt to remove the object as well. */
        if (scsisrc->protocol == VIR_DOMAIN_HOSTDEV_SCSI_PROTOCOL_TYPE_ISCSI &&
            virQEMUCapsGet(priv->qemuCaps, QEMU_CAPS_ISCSI_PASSWORD_SECRET) &&
            qemuDomainStorageSourceHasAuth(iscsisrc->src)) {
            if (!(objAlias = qemuDomainGetSecretAESAlias(hostdev->info->alias, false)))
                goto cleanup;
        }

        qemuDomainObjEnterMonitor(driver, vm);
        qemuMonitorDriveDel(priv->mon, drivealias);

        /* If it fails, then so be it - it was a best shot */
        if (objAlias)
            ignore_value(qemuMonitorDelObject(priv->mon, objAlias));

        if (qemuDomainObjExitMonitor(driver, vm) < 0)
            goto cleanup;
    }

    event = virDomainEventDeviceRemovedNewFromObj(vm, hostdev->info->alias);
    virObjectEventStateQueue(driver->domainEventState, event);

    if (hostdev->parent.type == VIR_DOMAIN_DEVICE_NET) {
        net = hostdev->parent.data.net;

        for (i = 0; i < vm->def->nnets; i++) {
            if (vm->def->nets[i] == net) {
                virDomainNetRemove(vm->def, i);
                break;
            }
        }
    }

    for (i = 0; i < vm->def->nhostdevs; i++) {
        if (vm->def->hostdevs[i] == hostdev) {
            virDomainHostdevRemove(vm->def, i);
            break;
        }
    }

    virDomainAuditHostdev(vm, hostdev, "detach", true);

    if (!is_vfio &&
        qemuSecurityRestoreHostdevLabel(driver, vm, hostdev) < 0)
        VIR_WARN("Failed to restore host device labelling");

    if (qemuTeardownHostdevCgroup(vm, hostdev) < 0)
        VIR_WARN("Failed to remove host device cgroup ACL");

    if (qemuDomainNamespaceTeardownHostdev(vm, hostdev) < 0)
        VIR_WARN("Unable to remove host device from /dev");

    switch ((virDomainHostdevSubsysType)hostdev->source.subsys.type) {
    case VIR_DOMAIN_HOSTDEV_SUBSYS_TYPE_PCI:
        qemuDomainRemovePCIHostDevice(driver, vm, hostdev);
        /* QEMU might no longer need to lock as much memory, eg. we just
         * detached the last VFIO device, so adjust the limit here */
        if (qemuDomainAdjustMaxMemLock(vm) < 0)
            VIR_WARN("Failed to adjust locked memory limit");
        break;
    case VIR_DOMAIN_HOSTDEV_SUBSYS_TYPE_USB:
        qemuDomainRemoveUSBHostDevice(driver, vm, hostdev);
        break;
    case VIR_DOMAIN_HOSTDEV_SUBSYS_TYPE_SCSI:
        qemuDomainRemoveSCSIHostDevice(driver, vm, hostdev);
        break;
    case VIR_DOMAIN_HOSTDEV_SUBSYS_TYPE_SCSI_HOST:
        qemuDomainRemoveSCSIVHostDevice(driver, vm, hostdev);
        break;
    case VIR_DOMAIN_HOSTDEV_SUBSYS_TYPE_MDEV:
        qemuDomainRemoveMediatedDevice(driver, vm, hostdev);
        break;
    case VIR_DOMAIN_HOSTDEV_SUBSYS_TYPE_LAST:
        break;
    }

    virDomainHostdevDefFree(hostdev);

    if (net) {
        virDomainNetReleaseActualDevice(vm->def, net);
        virDomainNetDefFree(net);
    }

    ret = 0;

 cleanup:
    VIR_FREE(drivealias);
    VIR_FREE(objAlias);
    virObjectUnref(cfg);
    return ret;
}


static int
qemuDomainRemoveNetDevice(virQEMUDriverPtr driver,
                          virDomainObjPtr vm,
                          virDomainNetDefPtr net)
{
    virQEMUDriverConfigPtr cfg = virQEMUDriverGetConfig(driver);
    qemuDomainObjPrivatePtr priv = vm->privateData;
    virObjectEventPtr event;
    char *hostnet_name = NULL;
    char *charDevAlias = NULL;
    size_t i;
    int ret = -1;
    int actualType = virDomainNetGetActualType(net);

    if (actualType == VIR_DOMAIN_NET_TYPE_HOSTDEV) {
        /* this function handles all hostdev and netdev cleanup */
        ret = qemuDomainRemoveHostDevice(driver, vm,
                                         virDomainNetGetActualHostdev(net));
        goto cleanup;
    }

    VIR_DEBUG("Removing network interface %s from domain %p %s",
              net->info.alias, vm, vm->def->name);

    if (virAsprintf(&hostnet_name, "host%s", net->info.alias) < 0 ||
        !(charDevAlias = qemuAliasChardevFromDevAlias(net->info.alias)))
        goto cleanup;


    qemuDomainObjEnterMonitor(driver, vm);
    if (qemuMonitorRemoveNetdev(priv->mon, hostnet_name) < 0) {
        if (qemuDomainObjExitMonitor(driver, vm) < 0)
            goto cleanup;
        virDomainAuditNet(vm, net, NULL, "detach", false);
        goto cleanup;
    }

    if (actualType == VIR_DOMAIN_NET_TYPE_VHOSTUSER) {
        /* vhostuser has a chardev too */
        if (qemuMonitorDetachCharDev(priv->mon, charDevAlias) < 0) {
            /* well, this is a messy situation. Guest visible PCI device has
             * been removed, netdev too but chardev not. The best seems to be
             * to just ignore the error and carry on.
             */
        }
    }

    if (qemuDomainObjExitMonitor(driver, vm) < 0)
        goto cleanup;

    virDomainAuditNet(vm, net, NULL, "detach", true);

    event = virDomainEventDeviceRemovedNewFromObj(vm, net->info.alias);
    virObjectEventStateQueue(driver->domainEventState, event);

    for (i = 0; i < vm->def->nnets; i++) {
        if (vm->def->nets[i] == net) {
            virDomainNetRemove(vm->def, i);
            break;
        }
    }

    qemuDomainReleaseDeviceAddress(vm, &net->info, NULL);
    virDomainConfNWFilterTeardown(net);

    if (cfg->macFilter && (net->ifname != NULL)) {
        ignore_value(ebtablesRemoveForwardAllowIn(driver->ebtables,
                                                  net->ifname,
                                                  &net->mac));
    }

    if (actualType == VIR_DOMAIN_NET_TYPE_DIRECT) {
        ignore_value(virNetDevMacVLanDeleteWithVPortProfile(
                         net->ifname, &net->mac,
                         virDomainNetGetActualDirectDev(net),
                         virDomainNetGetActualDirectMode(net),
                         virDomainNetGetActualVirtPortProfile(net),
                         cfg->stateDir));
    }

    qemuDomainNetDeviceVportRemove(net);

    virDomainNetReleaseActualDevice(vm->def, net);
    virDomainNetDefFree(net);
    ret = 0;

 cleanup:
    virObjectUnref(cfg);
    VIR_FREE(charDevAlias);
    VIR_FREE(hostnet_name);
    return ret;
}


static int
qemuDomainRemoveChrDevice(virQEMUDriverPtr driver,
                          virDomainObjPtr vm,
                          virDomainChrDefPtr chr)
{
    virObjectEventPtr event;
    char *charAlias = NULL;
    qemuDomainObjPrivatePtr priv = vm->privateData;
    int ret = -1;
    int rc;

    VIR_DEBUG("Removing character device %s from domain %p %s",
              chr->info.alias, vm, vm->def->name);

    if (!(charAlias = qemuAliasChardevFromDevAlias(chr->info.alias)))
        goto cleanup;

    qemuDomainObjEnterMonitor(driver, vm);
    rc = qemuMonitorDetachCharDev(priv->mon, charAlias);

    if (qemuDomainObjExitMonitor(driver, vm) < 0)
        goto cleanup;

    if (rc == 0 &&
        qemuDomainDelChardevTLSObjects(driver, vm, chr->source, charAlias) < 0)
        goto cleanup;

    virDomainAuditChardev(vm, chr, NULL, "detach", rc == 0);

    if (rc < 0)
        goto cleanup;

    if (qemuTeardownChardevCgroup(vm, chr) < 0)
        VIR_WARN("Failed to remove chr device cgroup ACL");

    if (qemuSecurityRestoreChardevLabel(driver, vm, chr) < 0)
        VIR_WARN("Unable to restore security label on char device");

    if (qemuDomainNamespaceTeardownChardev(vm, chr) < 0)
        VIR_WARN("Unable to remove chr device from /dev");

    event = virDomainEventDeviceRemovedNewFromObj(vm, chr->info.alias);
    virObjectEventStateQueue(driver->domainEventState, event);

    qemuDomainReleaseDeviceAddress(vm, &chr->info, NULL);
    qemuDomainChrRemove(vm->def, chr);
    virDomainChrDefFree(chr);
    ret = 0;

 cleanup:
    VIR_FREE(charAlias);
    return ret;
}


static int
qemuDomainRemoveRNGDevice(virQEMUDriverPtr driver,
                          virDomainObjPtr vm,
                          virDomainRNGDefPtr rng)
{
    virObjectEventPtr event;
    char *charAlias = NULL;
    char *objAlias = NULL;
    qemuDomainObjPrivatePtr priv = vm->privateData;
    ssize_t idx;
    int ret = -1;
    int rc;

    VIR_DEBUG("Removing RNG device %s from domain %p %s",
              rng->info.alias, vm, vm->def->name);


    if (virAsprintf(&objAlias, "obj%s", rng->info.alias) < 0)
        goto cleanup;

    if (!(charAlias = qemuAliasChardevFromDevAlias(rng->info.alias)))
        goto cleanup;

    qemuDomainObjEnterMonitor(driver, vm);

    rc = qemuMonitorDelObject(priv->mon, objAlias);

    if (qemuDomainObjExitMonitor(driver, vm) < 0)
        goto cleanup;

    if (rng->backend == VIR_DOMAIN_RNG_BACKEND_EGD &&
        rc == 0 &&
        qemuDomainDelChardevTLSObjects(driver, vm, rng->source.chardev,
                                       charAlias) < 0)
        goto cleanup;

    virDomainAuditRNG(vm, rng, NULL, "detach", rc == 0);

    if (rc < 0)
        goto cleanup;

    if (qemuTeardownRNGCgroup(vm, rng) < 0)
        VIR_WARN("Failed to remove RNG device cgroup ACL");

    if (qemuDomainNamespaceTeardownRNG(vm, rng) < 0)
        VIR_WARN("Unable to remove RNG device from /dev");

    event = virDomainEventDeviceRemovedNewFromObj(vm, rng->info.alias);
    virObjectEventStateQueue(driver->domainEventState, event);

    if ((idx = virDomainRNGFind(vm->def, rng)) >= 0)
        virDomainRNGRemove(vm->def, idx);
    qemuDomainReleaseDeviceAddress(vm, &rng->info, NULL);
    virDomainRNGDefFree(rng);
    ret = 0;

 cleanup:
    VIR_FREE(charAlias);
    VIR_FREE(objAlias);
    return ret;
}


static int
qemuDomainRemoveShmemDevice(virQEMUDriverPtr driver,
                            virDomainObjPtr vm,
                            virDomainShmemDefPtr shmem)
{
    int rc;
    int ret = -1;
    ssize_t idx = -1;
    char *charAlias = NULL;
    char *memAlias = NULL;
    qemuDomainObjPrivatePtr priv = vm->privateData;
    virObjectEventPtr event = NULL;

    VIR_DEBUG("Removing shmem device %s from domain %p %s",
              shmem->info.alias, vm, vm->def->name);

    if (shmem->server.enabled) {
        if (virAsprintf(&charAlias, "char%s", shmem->info.alias) < 0)
            return -1;
    } else {
        if (virAsprintf(&memAlias, "shmmem-%s", shmem->info.alias) < 0)
            return -1;
    }

    qemuDomainObjEnterMonitor(driver, vm);

    if (shmem->server.enabled)
        rc = qemuMonitorDetachCharDev(priv->mon, charAlias);
    else
        rc = qemuMonitorDelObject(priv->mon, memAlias);

    if (qemuDomainObjExitMonitor(driver, vm) < 0)
        goto cleanup;

    virDomainAuditShmem(vm, shmem, "detach", rc == 0);

    if (rc < 0)
        goto cleanup;

    event = virDomainEventDeviceRemovedNewFromObj(vm, shmem->info.alias);
    virObjectEventStateQueue(driver->domainEventState, event);

    if ((idx = virDomainShmemDefFind(vm->def, shmem)) >= 0)
        virDomainShmemDefRemove(vm->def, idx);
    qemuDomainReleaseDeviceAddress(vm, &shmem->info, NULL);
    virDomainShmemDefFree(shmem);

    ret = 0;
 cleanup:
    VIR_FREE(charAlias);
    VIR_FREE(memAlias);

    return ret;
}


static int
qemuDomainRemoveWatchdog(virQEMUDriverPtr driver,
                         virDomainObjPtr vm,
                         virDomainWatchdogDefPtr watchdog)
{
    virObjectEventPtr event = NULL;

    VIR_DEBUG("Removing watchdog %s from domain %p %s",
              watchdog->info.alias, vm, vm->def->name);

    event = virDomainEventDeviceRemovedNewFromObj(vm, watchdog->info.alias);
    virObjectEventStateQueue(driver->domainEventState, event);
    qemuDomainReleaseDeviceAddress(vm, &watchdog->info, NULL);
    virDomainWatchdogDefFree(vm->def->watchdog);
    vm->def->watchdog = NULL;
    return 0;
}


static int
qemuDomainRemoveInputDevice(virDomainObjPtr vm,
                            virDomainInputDefPtr dev)
{
    qemuDomainObjPrivatePtr priv = vm->privateData;
    virQEMUDriverPtr driver = priv->driver;
    virObjectEventPtr event = NULL;
    size_t i;

    VIR_DEBUG("Removing input device %s from domain %p %s",
              dev->info.alias, vm, vm->def->name);

    event = virDomainEventDeviceRemovedNewFromObj(vm, dev->info.alias);
    virObjectEventStateQueue(driver->domainEventState, event);
    for (i = 0; i < vm->def->ninputs; i++) {
        if (vm->def->inputs[i] == dev)
            break;
    }
    qemuDomainReleaseDeviceAddress(vm, &dev->info, NULL);
    if (qemuSecurityRestoreInputLabel(vm, dev) < 0)
        VIR_WARN("Unable to restore security label on input device");

    if (qemuTeardownInputCgroup(vm, dev) < 0)
        VIR_WARN("Unable to remove input device cgroup ACL");

    if (qemuDomainNamespaceTeardownInput(vm, dev) < 0)
        VIR_WARN("Unable to remove input device from /dev");

    virDomainInputDefFree(vm->def->inputs[i]);
    VIR_DELETE_ELEMENT(vm->def->inputs, i, vm->def->ninputs);
    return 0;
}


static int
qemuDomainRemoveVsockDevice(virDomainObjPtr vm,
                            virDomainVsockDefPtr dev)
{
    qemuDomainObjPrivatePtr priv = vm->privateData;
    virQEMUDriverPtr driver = priv->driver;
    virObjectEventPtr event = NULL;

    VIR_DEBUG("Removing vsock device %s from domain %p %s",
              dev->info.alias, vm, vm->def->name);

    event = virDomainEventDeviceRemovedNewFromObj(vm, dev->info.alias);
    virObjectEventStateQueue(driver->domainEventState, event);
    qemuDomainReleaseDeviceAddress(vm, &dev->info, NULL);
    virDomainVsockDefFree(vm->def->vsock);
    vm->def->vsock = NULL;
    return 0;
}


static int
qemuDomainRemoveRedirdevDevice(virQEMUDriverPtr driver,
                               virDomainObjPtr vm,
                               virDomainRedirdevDefPtr dev)
{
    qemuDomainObjPrivatePtr priv = vm->privateData;
    virObjectEventPtr event;
    char *charAlias = NULL;
    ssize_t idx;
    int ret = -1;

    VIR_DEBUG("Removing redirdev device %s from domain %p %s",
              dev->info.alias, vm, vm->def->name);

    if (!(charAlias = qemuAliasChardevFromDevAlias(dev->info.alias)))
        goto cleanup;

    qemuDomainObjEnterMonitor(driver, vm);
    /* DeviceDel from Detach may remove chardev,
     * so we cannot rely on return status to delete TLS chardevs.
     */
    ignore_value(qemuMonitorDetachCharDev(priv->mon, charAlias));

    if (qemuDomainObjExitMonitor(driver, vm) < 0)
        goto cleanup;

    if (qemuDomainDelChardevTLSObjects(driver, vm, dev->source, charAlias) < 0)
        goto cleanup;

    virDomainAuditRedirdev(vm, dev, "detach", true);

    event = virDomainEventDeviceRemovedNewFromObj(vm, dev->info.alias);
    virObjectEventStateQueue(driver->domainEventState, event);

    if ((idx = virDomainRedirdevDefFind(vm->def, dev)) >= 0)
        virDomainRedirdevDefRemove(vm->def, idx);
    qemuDomainReleaseDeviceAddress(vm, &dev->info, NULL);
    virDomainRedirdevDefFree(dev);

    ret = 0;

 cleanup:
    VIR_FREE(charAlias);
    return ret;
}


int
qemuDomainRemoveDevice(virQEMUDriverPtr driver,
                       virDomainObjPtr vm,
                       virDomainDeviceDefPtr dev)
{
    int ret = -1;
    switch ((virDomainDeviceType)dev->type) {
    case VIR_DOMAIN_DEVICE_DISK:
        ret = qemuDomainRemoveDiskDevice(driver, vm, dev->data.disk);
        break;
    case VIR_DOMAIN_DEVICE_CONTROLLER:
        ret = qemuDomainRemoveControllerDevice(driver, vm, dev->data.controller);
        break;
    case VIR_DOMAIN_DEVICE_NET:
        ret = qemuDomainRemoveNetDevice(driver, vm, dev->data.net);
        break;
    case VIR_DOMAIN_DEVICE_HOSTDEV:
        ret = qemuDomainRemoveHostDevice(driver, vm, dev->data.hostdev);
        break;

    case VIR_DOMAIN_DEVICE_CHR:
        ret = qemuDomainRemoveChrDevice(driver, vm, dev->data.chr);
        break;
    case VIR_DOMAIN_DEVICE_RNG:
        ret = qemuDomainRemoveRNGDevice(driver, vm, dev->data.rng);
        break;

    case VIR_DOMAIN_DEVICE_MEMORY:
        ret = qemuDomainRemoveMemoryDevice(driver, vm, dev->data.memory);
        break;

    case VIR_DOMAIN_DEVICE_SHMEM:
        ret = qemuDomainRemoveShmemDevice(driver, vm, dev->data.shmem);
        break;

    case VIR_DOMAIN_DEVICE_INPUT:
        ret = qemuDomainRemoveInputDevice(vm, dev->data.input);
        break;

    case VIR_DOMAIN_DEVICE_REDIRDEV:
        ret = qemuDomainRemoveRedirdevDevice(driver, vm, dev->data.redirdev);
        break;

    case VIR_DOMAIN_DEVICE_WATCHDOG:
        ret = qemuDomainRemoveWatchdog(driver, vm, dev->data.watchdog);
        break;

    case VIR_DOMAIN_DEVICE_VSOCK:
        ret = qemuDomainRemoveVsockDevice(vm, dev->data.vsock);
        break;

    case VIR_DOMAIN_DEVICE_NONE:
    case VIR_DOMAIN_DEVICE_LEASE:
    case VIR_DOMAIN_DEVICE_FS:
    case VIR_DOMAIN_DEVICE_SOUND:
    case VIR_DOMAIN_DEVICE_VIDEO:
    case VIR_DOMAIN_DEVICE_GRAPHICS:
    case VIR_DOMAIN_DEVICE_HUB:
    case VIR_DOMAIN_DEVICE_SMARTCARD:
    case VIR_DOMAIN_DEVICE_MEMBALLOON:
    case VIR_DOMAIN_DEVICE_NVRAM:
    case VIR_DOMAIN_DEVICE_TPM:
    case VIR_DOMAIN_DEVICE_PANIC:
    case VIR_DOMAIN_DEVICE_IOMMU:
    case VIR_DOMAIN_DEVICE_LAST:
        virReportError(VIR_ERR_OPERATION_UNSUPPORTED,
                       _("don't know how to remove a %s device"),
                       virDomainDeviceTypeToString(dev->type));
        break;
    }
    return ret;
}


static void
qemuDomainMarkDeviceAliasForRemoval(virDomainObjPtr vm,
                                    const char *alias)
{
    qemuDomainObjPrivatePtr priv = vm->privateData;

    memset(&priv->unplug, 0, sizeof(priv->unplug));

    if (!virQEMUCapsGet(priv->qemuCaps, QEMU_CAPS_DEVICE_DEL_EVENT))
        return;

    priv->unplug.alias = alias;
}


static void
qemuDomainMarkDeviceForRemoval(virDomainObjPtr vm,
                               virDomainDeviceInfoPtr info)

{
    qemuDomainMarkDeviceAliasForRemoval(vm, info->alias);
}


static void
qemuDomainResetDeviceRemoval(virDomainObjPtr vm)
{
    qemuDomainObjPrivatePtr priv = vm->privateData;
    priv->unplug.alias = NULL;
}

/* Returns:
 *  -1 Unplug of the device failed
 *
 *   0 DEVICE_DELETED event is supported and removal of the device did not
 *     finish in qemuDomainRemoveDeviceWaitTime
 *
 *   1 when the caller is responsible for finishing the device removal:
 *      - DEVICE_DELETED event is unsupported
 *      - DEVICE_DELETED event arrived before the timeout time
 *      - we failed to reliably wait for the event and thus use fallback behavior
 */
static int
qemuDomainWaitForDeviceRemoval(virDomainObjPtr vm)
{
    qemuDomainObjPrivatePtr priv = vm->privateData;
    unsigned long long until;
    int rc;

    if (!virQEMUCapsGet(priv->qemuCaps, QEMU_CAPS_DEVICE_DEL_EVENT))
        return 1;

    if (virTimeMillisNow(&until) < 0)
        return 1;
    until += qemuDomainRemoveDeviceWaitTime;

    while (priv->unplug.alias) {
        if ((rc = virDomainObjWaitUntil(vm, until)) == 1)
            return 0;

        if (rc < 0) {
            VIR_WARN("Failed to wait on unplug condition for domain '%s' "
                     "device '%s'", vm->def->name, priv->unplug.alias);
            return 1;
        }
    }

    if (priv->unplug.status == QEMU_DOMAIN_UNPLUGGING_DEVICE_STATUS_GUEST_REJECTED) {
        virReportError(VIR_ERR_OPERATION_FAILED, "%s",
                       _("unplug of device was rejected by the guest"));
        return -1;
    }

    return 1;
}

/* Returns:
 *  true    there was a thread waiting for devAlias to be removed and this
 *          thread will take care of finishing the removal
 *  false   the thread that started the removal is already gone and delegate
 *          finishing the removal to a new thread
 */
bool
qemuDomainSignalDeviceRemoval(virDomainObjPtr vm,
                              const char *devAlias,
                              qemuDomainUnpluggingDeviceStatus status)
{
    qemuDomainObjPrivatePtr priv = vm->privateData;

    if (STREQ_NULLABLE(priv->unplug.alias, devAlias)) {
        VIR_DEBUG("Removal of device '%s' continues in waiting thread", devAlias);
        qemuDomainResetDeviceRemoval(vm);
        priv->unplug.status = status;
        virDomainObjBroadcast(vm);
        return true;
    }
    return false;
}


static int
qemuDomainDetachVirtioDiskDevice(virQEMUDriverPtr driver,
                                 virDomainObjPtr vm,
                                 virDomainDiskDefPtr detach,
                                 bool async)
{
    int ret = -1;
    qemuDomainObjPrivatePtr priv = vm->privateData;

    if (qemuIsMultiFunctionDevice(vm->def, &detach->info)) {
        virReportError(VIR_ERR_OPERATION_FAILED,
                       _("cannot hot unplug multifunction PCI device: %s"),
                       detach->dst);
        goto cleanup;
    }

    if (!async)
        qemuDomainMarkDeviceForRemoval(vm, &detach->info);

    qemuDomainObjEnterMonitor(driver, vm);
    if (qemuMonitorDelDevice(priv->mon, detach->info.alias) < 0) {
        if (qemuDomainObjExitMonitor(driver, vm) < 0)
            goto cleanup;
        virDomainAuditDisk(vm, detach->src, NULL, "detach", false);
        goto cleanup;
    }
    if (qemuDomainObjExitMonitor(driver, vm) < 0)
        goto cleanup;

    if (async) {
        ret = 0;
    } else {
        if ((ret = qemuDomainWaitForDeviceRemoval(vm)) == 1)
            ret = qemuDomainRemoveDiskDevice(driver, vm, detach);
    }

 cleanup:
    if (!async)
        qemuDomainResetDeviceRemoval(vm);
    return ret;
}

static int
qemuDomainDetachDiskDevice(virQEMUDriverPtr driver,
                           virDomainObjPtr vm,
                           virDomainDiskDefPtr detach,
                           bool async)
{
    int ret = -1;
    qemuDomainObjPrivatePtr priv = vm->privateData;

    if (qemuDomainDiskBlockJobIsActive(detach))
        goto cleanup;

    if (!async)
        qemuDomainMarkDeviceForRemoval(vm, &detach->info);

    qemuDomainObjEnterMonitor(driver, vm);
    if (qemuMonitorDelDevice(priv->mon, detach->info.alias) < 0) {
        if (qemuDomainObjExitMonitor(driver, vm) < 0)
            goto cleanup;
        virDomainAuditDisk(vm, detach->src, NULL, "detach", false);
        goto cleanup;
    }
    if (qemuDomainObjExitMonitor(driver, vm) < 0)
        goto cleanup;

    if (async) {
        ret = 0;
    } else {
        if ((ret = qemuDomainWaitForDeviceRemoval(vm)) == 1)
            ret = qemuDomainRemoveDiskDevice(driver, vm, detach);
    }

 cleanup:
    if (!async)
        qemuDomainResetDeviceRemoval(vm);
    return ret;
}

static int
qemuFindDisk(virDomainDefPtr def, const char *dst)
{
    size_t i;

    for (i = 0; i < def->ndisks; i++) {
        if (STREQ(def->disks[i]->dst, dst))
            return i;
    }

    return -1;
}

int
qemuDomainDetachDeviceDiskLive(virQEMUDriverPtr driver,
                               virDomainObjPtr vm,
                               virDomainDeviceDefPtr dev,
                               bool async)
{
    virDomainDiskDefPtr disk;
    int ret = -1;
    int idx;

    if ((idx = qemuFindDisk(vm->def, dev->data.disk->dst)) < 0) {
        virReportError(VIR_ERR_OPERATION_FAILED,
                       _("disk %s not found"), dev->data.disk->dst);
        return -1;
    }
    disk = vm->def->disks[idx];

    switch (disk->device) {
    case VIR_DOMAIN_DISK_DEVICE_DISK:
    case VIR_DOMAIN_DISK_DEVICE_LUN:
        if (disk->bus == VIR_DOMAIN_DISK_BUS_VIRTIO)
            ret = qemuDomainDetachVirtioDiskDevice(driver, vm, disk, async);
        else if (disk->bus == VIR_DOMAIN_DISK_BUS_SCSI ||
                 disk->bus == VIR_DOMAIN_DISK_BUS_USB)
            ret = qemuDomainDetachDiskDevice(driver, vm, disk, async);
        else
            virReportError(VIR_ERR_OPERATION_UNSUPPORTED, "%s",
                           _("This type of disk cannot be hot unplugged"));
        break;
    default:
        virReportError(VIR_ERR_OPERATION_UNSUPPORTED,
                       _("disk device type '%s' cannot be detached"),
                       virDomainDiskDeviceTypeToString(disk->device));
        break;
    }

    return ret;
}


static bool qemuDomainDiskControllerIsBusy(virDomainObjPtr vm,
                                           virDomainControllerDefPtr detach)
{
    size_t i;
    virDomainDiskDefPtr disk;
    virDomainHostdevDefPtr hostdev;

    for (i = 0; i < vm->def->ndisks; i++) {
        disk = vm->def->disks[i];
        if (disk->info.type != VIR_DOMAIN_DEVICE_ADDRESS_TYPE_DRIVE)
            /* the disk does not use disk controller */
            continue;

        /* check whether the disk uses this type controller */
        if (disk->bus == VIR_DOMAIN_DISK_BUS_IDE &&
            detach->type != VIR_DOMAIN_CONTROLLER_TYPE_IDE)
            continue;
        if (disk->bus == VIR_DOMAIN_DISK_BUS_FDC &&
            detach->type != VIR_DOMAIN_CONTROLLER_TYPE_FDC)
            continue;
        if (disk->bus == VIR_DOMAIN_DISK_BUS_SCSI &&
            detach->type != VIR_DOMAIN_CONTROLLER_TYPE_SCSI)
            continue;

        if (disk->info.addr.drive.controller == detach->idx)
            return true;
    }

    for (i = 0; i < vm->def->nhostdevs; i++) {
        hostdev = vm->def->hostdevs[i];
        if (!virHostdevIsSCSIDevice(hostdev) ||
            detach->type != VIR_DOMAIN_CONTROLLER_TYPE_SCSI)
            continue;
        if (hostdev->info->addr.drive.controller == detach->idx)
            return true;
    }

    return false;
}

static bool qemuDomainControllerIsBusy(virDomainObjPtr vm,
                                       virDomainControllerDefPtr detach)
{
    switch (detach->type) {
    case VIR_DOMAIN_CONTROLLER_TYPE_IDE:
    case VIR_DOMAIN_CONTROLLER_TYPE_FDC:
    case VIR_DOMAIN_CONTROLLER_TYPE_SCSI:
        return qemuDomainDiskControllerIsBusy(vm, detach);

    case VIR_DOMAIN_CONTROLLER_TYPE_SATA:
    case VIR_DOMAIN_CONTROLLER_TYPE_VIRTIO_SERIAL:
    case VIR_DOMAIN_CONTROLLER_TYPE_CCID:
    default:
        /* libvirt does not support sata controller, and does not support to
         * detach virtio and smart card controller.
         */
        return true;
    }
}

int qemuDomainDetachControllerDevice(virQEMUDriverPtr driver,
                                     virDomainObjPtr vm,
                                     virDomainDeviceDefPtr dev,
                                     bool async)
{
    int idx, ret = -1;
    virDomainControllerDefPtr detach = NULL;
    qemuDomainObjPrivatePtr priv = vm->privateData;

    if ((idx = virDomainControllerFind(vm->def,
                                       dev->data.controller->type,
                                       dev->data.controller->idx)) < 0) {
        virReportError(VIR_ERR_DEVICE_MISSING,
                       _("controller %s:%d not found"),
                       virDomainControllerTypeToString(dev->data.controller->type),
                       dev->data.controller->idx);
        goto cleanup;
    }

    detach = vm->def->controllers[idx];

    if (detach->info.type != VIR_DOMAIN_DEVICE_ADDRESS_TYPE_PCI &&
        detach->info.type != VIR_DOMAIN_DEVICE_ADDRESS_TYPE_CCW &&
        detach->info.type != VIR_DOMAIN_DEVICE_ADDRESS_TYPE_VIRTIO_S390) {
        virReportError(VIR_ERR_OPERATION_FAILED,
                       _("device with '%s' address cannot be detached"),
                       virDomainDeviceAddressTypeToString(detach->info.type));
        goto cleanup;
    }

    if (!virDomainDeviceAddressIsValid(&detach->info, detach->info.type)) {
        virReportError(VIR_ERR_OPERATION_FAILED,
                       _("device with invalid '%s' address cannot be detached"),
                       virDomainDeviceAddressTypeToString(detach->info.type));
        goto cleanup;
    }

    if (qemuIsMultiFunctionDevice(vm->def, &detach->info)) {
        virReportError(VIR_ERR_OPERATION_FAILED,
                       _("cannot hot unplug multifunction PCI device: %s"),
                       dev->data.disk->dst);
        goto cleanup;
    }

    if (qemuDomainControllerIsBusy(vm, detach)) {
        virReportError(VIR_ERR_OPERATION_FAILED, "%s",
                       _("device cannot be detached: device is busy"));
        goto cleanup;
    }

    if (!async)
        qemuDomainMarkDeviceForRemoval(vm, &detach->info);

    qemuDomainObjEnterMonitor(driver, vm);
    if (qemuMonitorDelDevice(priv->mon, detach->info.alias)) {
        ignore_value(qemuDomainObjExitMonitor(driver, vm));
        goto cleanup;
    }
    if (qemuDomainObjExitMonitor(driver, vm) < 0)
        goto cleanup;

    if (async) {
        ret = 0;
    } else {
        if ((ret = qemuDomainWaitForDeviceRemoval(vm)) == 1)
            ret = qemuDomainRemoveControllerDevice(driver, vm, detach);
    }

 cleanup:
    if (!async)
        qemuDomainResetDeviceRemoval(vm);
    return ret;
}

static int
qemuDomainDetachHostPCIDevice(virQEMUDriverPtr driver,
                              virDomainObjPtr vm,
                              virDomainHostdevDefPtr detach,
                              bool async)
{
    qemuDomainObjPrivatePtr priv = vm->privateData;
    virDomainHostdevSubsysPCIPtr pcisrc = &detach->source.subsys.u.pci;
    int ret;

    if (qemuIsMultiFunctionDevice(vm->def, detach->info)) {
        virReportError(VIR_ERR_OPERATION_FAILED,
                       _("cannot hot unplug multifunction PCI device: %.4x:%.2x:%.2x.%.1x"),
                       pcisrc->addr.domain, pcisrc->addr.bus,
                       pcisrc->addr.slot, pcisrc->addr.function);
        return -1;
    }

    if (!async)
        qemuDomainMarkDeviceForRemoval(vm, detach->info);

    qemuDomainObjEnterMonitor(driver, vm);
    ret = qemuMonitorDelDevice(priv->mon, detach->info->alias);
    if (qemuDomainObjExitMonitor(driver, vm) < 0)
        ret = -1;

    return ret;
}

static int
qemuDomainDetachHostUSBDevice(virQEMUDriverPtr driver,
                              virDomainObjPtr vm,
                              virDomainHostdevDefPtr detach,
                              bool async)
{
    qemuDomainObjPrivatePtr priv = vm->privateData;
    int ret;

    if (!detach->info->alias) {
        virReportError(VIR_ERR_OPERATION_FAILED,
                       "%s", _("device cannot be detached without a device alias"));
        return -1;
    }

    if (!async)
        qemuDomainMarkDeviceForRemoval(vm, detach->info);

    qemuDomainObjEnterMonitor(driver, vm);
    ret = qemuMonitorDelDevice(priv->mon, detach->info->alias);
    if (qemuDomainObjExitMonitor(driver, vm) < 0)
        ret = -1;

    return ret;
}

static int
qemuDomainDetachHostSCSIDevice(virQEMUDriverPtr driver,
                               virDomainObjPtr vm,
                               virDomainHostdevDefPtr detach,
                               bool async)
{
    qemuDomainObjPrivatePtr priv = vm->privateData;
    int ret = -1;

    if (!detach->info->alias) {
        virReportError(VIR_ERR_OPERATION_FAILED,
                       "%s", _("device cannot be detached without a device alias"));
        return -1;
    }

    if (!async)
        qemuDomainMarkDeviceForRemoval(vm, detach->info);

    qemuDomainObjEnterMonitor(driver, vm);
    ret = qemuMonitorDelDevice(priv->mon, detach->info->alias);

    if (qemuDomainObjExitMonitor(driver, vm) < 0)
        return -1;

    return ret;
}

static int
qemuDomainDetachSCSIVHostDevice(virQEMUDriverPtr driver,
                                virDomainObjPtr vm,
                                virDomainHostdevDefPtr detach,
                                bool async)
{
    qemuDomainObjPrivatePtr priv = vm->privateData;
    int ret = -1;

    if (!detach->info->alias) {
        virReportError(VIR_ERR_OPERATION_FAILED,
                       "%s", _("device cannot be detached without a device alias"));
        return -1;
    }

    if (!async)
        qemuDomainMarkDeviceForRemoval(vm, detach->info);

    qemuDomainObjEnterMonitor(driver, vm);
    ret = qemuMonitorDelDevice(priv->mon, detach->info->alias);

    if (qemuDomainObjExitMonitor(driver, vm) < 0)
        return -1;

    return ret;
}


static int
qemuDomainDetachMediatedDevice(virQEMUDriverPtr driver,
                               virDomainObjPtr vm,
                               virDomainHostdevDefPtr detach,
                               bool async)
{
    int ret = -1;
    qemuDomainObjPrivatePtr priv = vm->privateData;

    if (!detach->info->alias) {
        virReportError(VIR_ERR_OPERATION_FAILED, "%s",
                       _("device cannot be detached without a device alias"));
        return -1;
    }

    if (!async)
        qemuDomainMarkDeviceForRemoval(vm, detach->info);

    qemuDomainObjEnterMonitor(driver, vm);
    ret = qemuMonitorDelDevice(priv->mon, detach->info->alias);
    if (qemuDomainObjExitMonitor(driver, vm) < 0)
        ret = -1;

    return ret;
}


static int
qemuDomainDetachThisHostDevice(virQEMUDriverPtr driver,
                               virDomainObjPtr vm,
                               virDomainHostdevDefPtr detach,
                               bool async)
{
    int ret = -1;

    if (qemuAssignDeviceHostdevAlias(vm->def, &detach->info->alias, -1) < 0)
        return -1;

    switch (detach->source.subsys.type) {
    case VIR_DOMAIN_HOSTDEV_SUBSYS_TYPE_PCI:
        ret = qemuDomainDetachHostPCIDevice(driver, vm, detach, async);
        break;
    case VIR_DOMAIN_HOSTDEV_SUBSYS_TYPE_USB:
        ret = qemuDomainDetachHostUSBDevice(driver, vm, detach, async);
        break;
    case VIR_DOMAIN_HOSTDEV_SUBSYS_TYPE_SCSI:
        ret = qemuDomainDetachHostSCSIDevice(driver, vm, detach, async);
        break;
    case VIR_DOMAIN_HOSTDEV_SUBSYS_TYPE_SCSI_HOST:
        ret = qemuDomainDetachSCSIVHostDevice(driver, vm, detach, async);
        break;
    case VIR_DOMAIN_HOSTDEV_SUBSYS_TYPE_MDEV:
        ret = qemuDomainDetachMediatedDevice(driver, vm, detach, async);
        break;
    default:
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("hot unplug is not supported for hostdev subsys type '%s'"),
                       virDomainHostdevSubsysTypeToString(detach->source.subsys.type));
        return -1;
    }

    if (ret < 0) {
        if (virDomainObjIsActive(vm))
            virDomainAuditHostdev(vm, detach, "detach", false);
    } else if (!async &&
               (ret = qemuDomainWaitForDeviceRemoval(vm)) == 1) {
        ret = qemuDomainRemoveHostDevice(driver, vm, detach);
    }

    if (!async)
        qemuDomainResetDeviceRemoval(vm);

    return ret;
}

/* search for a hostdev matching dev and detach it */
int qemuDomainDetachHostDevice(virQEMUDriverPtr driver,
                               virDomainObjPtr vm,
                               virDomainDeviceDefPtr dev,
                               bool async)
{
    virDomainHostdevDefPtr hostdev = dev->data.hostdev;
    virDomainHostdevSubsysPtr subsys = &hostdev->source.subsys;
    virDomainHostdevSubsysUSBPtr usbsrc = &subsys->u.usb;
    virDomainHostdevSubsysPCIPtr pcisrc = &subsys->u.pci;
    virDomainHostdevSubsysSCSIPtr scsisrc = &subsys->u.scsi;
    virDomainHostdevSubsysMediatedDevPtr mdevsrc = &subsys->u.mdev;
    virDomainHostdevDefPtr detach = NULL;
    int idx;

    if (hostdev->mode != VIR_DOMAIN_HOSTDEV_MODE_SUBSYS) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("hot unplug is not supported for hostdev mode '%s'"),
                       virDomainHostdevModeTypeToString(hostdev->mode));
        return -1;
    }

    idx = virDomainHostdevFind(vm->def, hostdev, &detach);

    if (idx < 0) {
        switch (subsys->type) {
        case VIR_DOMAIN_HOSTDEV_SUBSYS_TYPE_PCI:
            virReportError(VIR_ERR_DEVICE_MISSING,
                           _("host pci device %.4x:%.2x:%.2x.%.1x not found"),
                           pcisrc->addr.domain, pcisrc->addr.bus,
                           pcisrc->addr.slot, pcisrc->addr.function);
            break;
        case VIR_DOMAIN_HOSTDEV_SUBSYS_TYPE_USB:
            if (usbsrc->bus && usbsrc->device) {
                virReportError(VIR_ERR_DEVICE_MISSING,
                               _("host usb device %03d.%03d not found"),
                               usbsrc->bus, usbsrc->device);
            } else {
                virReportError(VIR_ERR_DEVICE_MISSING,
                               _("host usb device vendor=0x%.4x product=0x%.4x not found"),
                               usbsrc->vendor, usbsrc->product);
            }
            break;
        case VIR_DOMAIN_HOSTDEV_SUBSYS_TYPE_SCSI: {
            if (scsisrc->protocol ==
                VIR_DOMAIN_HOSTDEV_SCSI_PROTOCOL_TYPE_ISCSI) {
                virDomainHostdevSubsysSCSIiSCSIPtr iscsisrc = &scsisrc->u.iscsi;
                virReportError(VIR_ERR_DEVICE_MISSING,
                               _("host scsi iSCSI path %s not found"),
                               iscsisrc->src->path);
            } else {
                 virDomainHostdevSubsysSCSIHostPtr scsihostsrc =
                     &scsisrc->u.host;
                 virReportError(VIR_ERR_DEVICE_MISSING,
                                _("host scsi device %s:%u:%u.%llu not found"),
                                scsihostsrc->adapter, scsihostsrc->bus,
                                scsihostsrc->target, scsihostsrc->unit);
            }
            break;
        }
        case VIR_DOMAIN_HOSTDEV_SUBSYS_TYPE_MDEV:
            virReportError(VIR_ERR_DEVICE_MISSING,
                           _("mediated device '%s' not found"),
                           mdevsrc->uuidstr);
            break;
        case VIR_DOMAIN_HOSTDEV_SUBSYS_TYPE_SCSI_HOST:
            break;
        default:
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("unexpected hostdev type %d"), subsys->type);
            break;
        }
        return -1;
    }

    /* If this is a network hostdev, we need to use the higher-level detach
     * function so that mac address / virtualport are reset
     */
    if (detach->parent.type == VIR_DOMAIN_DEVICE_NET)
        return qemuDomainDetachNetDevice(driver, vm, &detach->parent, async);
    else
        return qemuDomainDetachThisHostDevice(driver, vm, detach, async);
}


int
qemuDomainDetachShmemDevice(virQEMUDriverPtr driver,
                            virDomainObjPtr vm,
                            virDomainShmemDefPtr dev,
                            bool async)
{
    int ret = -1;
    ssize_t idx = -1;
    virDomainShmemDefPtr shmem = NULL;
    qemuDomainObjPrivatePtr priv = vm->privateData;

    if ((idx = virDomainShmemDefFind(vm->def, dev)) < 0) {
        virReportError(VIR_ERR_DEVICE_MISSING,
                       _("model '%s' shmem device not present "
                         "in domain configuration"),
                       virDomainShmemModelTypeToString(dev->model));
        return -1;
    }

    shmem = vm->def->shmems[idx];

    switch ((virDomainShmemModel)shmem->model) {
    case VIR_DOMAIN_SHMEM_MODEL_IVSHMEM_PLAIN:
    case VIR_DOMAIN_SHMEM_MODEL_IVSHMEM_DOORBELL:
        break;

    case VIR_DOMAIN_SHMEM_MODEL_IVSHMEM:
        virReportError(VIR_ERR_OPERATION_UNSUPPORTED,
                       _("live detach of shmem model '%s' is not supported"),
                       virDomainShmemModelTypeToString(shmem->model));
        ATTRIBUTE_FALLTHROUGH;
    case VIR_DOMAIN_SHMEM_MODEL_LAST:
        return -1;
    }

    if (!async)
        qemuDomainMarkDeviceForRemoval(vm, &shmem->info);

    qemuDomainObjEnterMonitor(driver, vm);
    if (qemuMonitorDelDevice(priv->mon, shmem->info.alias) < 0) {
        ignore_value(qemuDomainObjExitMonitor(driver, vm));
        goto cleanup;
    }
    if (qemuDomainObjExitMonitor(driver, vm) < 0)
        goto cleanup;

    if (async) {
        ret = 0;
    } else {
        if ((ret = qemuDomainWaitForDeviceRemoval(vm)) == 1)
            ret = qemuDomainRemoveShmemDevice(driver, vm, shmem);
    }

 cleanup:
    if (!async)
        qemuDomainResetDeviceRemoval(vm);
    return ret;
}


int
qemuDomainDetachWatchdog(virQEMUDriverPtr driver,
                         virDomainObjPtr vm,
                         virDomainWatchdogDefPtr dev,
                         bool async)
{
    int ret = -1;
    virDomainWatchdogDefPtr watchdog = vm->def->watchdog;
    qemuDomainObjPrivatePtr priv = vm->privateData;

    if (!watchdog) {
        virReportError(VIR_ERR_DEVICE_MISSING, "%s",
                       _("watchdog device not present in domain configuration"));
        return -1;
    }

    /* While domains can have up to one watchdog, the one supplied by the user
     * doesn't necessarily match the one domain has. Refuse to detach in such
     * case. */
    if (!(watchdog->model == dev->model &&
          watchdog->action == dev->action &&
          virDomainDeviceInfoAddressIsEqual(&dev->info, &watchdog->info))) {
        virReportError(VIR_ERR_DEVICE_MISSING,
                       _("model '%s' watchdog device not present "
                         "in domain configuration"),
                       virDomainWatchdogModelTypeToString(watchdog->model));
        return -1;
    }

    if (watchdog->model != VIR_DOMAIN_WATCHDOG_MODEL_I6300ESB) {
        virReportError(VIR_ERR_OPERATION_UNSUPPORTED,
                       _("hot unplug of watchdog of model %s is not supported"),
                       virDomainWatchdogModelTypeToString(watchdog->model));
        return -1;
    }

    if (!async)
        qemuDomainMarkDeviceForRemoval(vm, &watchdog->info);

    qemuDomainObjEnterMonitor(driver, vm);
    if (qemuMonitorDelDevice(priv->mon, watchdog->info.alias) < 0) {
        ignore_value(qemuDomainObjExitMonitor(driver, vm));
        goto cleanup;
    }
    if (qemuDomainObjExitMonitor(driver, vm) < 0)
        goto cleanup;

    if (async) {
        ret = 0;
    } else {
        if ((ret = qemuDomainWaitForDeviceRemoval(vm)) == 1)
            ret = qemuDomainRemoveWatchdog(driver, vm, watchdog);
    }

 cleanup:
    if (!async)
        qemuDomainResetDeviceRemoval(vm);
    return ret;
}


int
qemuDomainDetachRedirdevDevice(virQEMUDriverPtr driver,
                               virDomainObjPtr vm,
                               virDomainRedirdevDefPtr dev,
                               bool async)
{
    int ret = -1;
    qemuDomainObjPrivatePtr priv = vm->privateData;
    virDomainRedirdevDefPtr tmpRedirdevDef;
    ssize_t idx;

    if ((idx = virDomainRedirdevDefFind(vm->def, dev)) < 0) {
        virReportError(VIR_ERR_OPERATION_INVALID, "%s",
                       _("no matching redirdev was not found"));
        return -1;
    }

    tmpRedirdevDef = vm->def->redirdevs[idx];

    if (!tmpRedirdevDef->info.alias) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("alias not set for redirdev device"));
        return -1;
    }

    if (!async)
        qemuDomainMarkDeviceForRemoval(vm, &tmpRedirdevDef->info);

    qemuDomainObjEnterMonitor(driver, vm);
    if (qemuMonitorDelDevice(priv->mon, tmpRedirdevDef->info.alias) < 0) {
        ignore_value(qemuDomainObjExitMonitor(driver, vm));
        goto cleanup;
    }
    if (qemuDomainObjExitMonitor(driver, vm) < 0)
        goto cleanup;

    if (async) {
        ret = 0;
    } else {
        if ((ret = qemuDomainWaitForDeviceRemoval(vm)) == 1)
            ret = qemuDomainRemoveRedirdevDevice(driver, vm, tmpRedirdevDef);
    }

 cleanup:
    if (!async)
        qemuDomainResetDeviceRemoval(vm);
    return ret;
}


int
qemuDomainDetachNetDevice(virQEMUDriverPtr driver,
                          virDomainObjPtr vm,
                          virDomainDeviceDefPtr dev,
                          bool async)
{
    int detachidx, ret = -1;
    virDomainNetDefPtr detach = NULL;
    qemuDomainObjPrivatePtr priv = vm->privateData;

    if ((detachidx = virDomainNetFindIdx(vm->def, dev->data.net)) < 0)
        goto cleanup;

    detach = vm->def->nets[detachidx];

    if (virDomainNetGetActualType(detach) == VIR_DOMAIN_NET_TYPE_HOSTDEV) {
        ret = qemuDomainDetachThisHostDevice(driver, vm,
                                             virDomainNetGetActualHostdev(detach),
                                             async);
        goto cleanup;
    }

    if (qemuIsMultiFunctionDevice(vm->def, &detach->info)) {
        virReportError(VIR_ERR_OPERATION_FAILED,
                       _("cannot hot unplug multifunction PCI device: %s"),
                       dev->data.disk->dst);
        goto cleanup;
    }

    if (!detach->info.alias) {
        if (qemuAssignDeviceNetAlias(vm->def, detach, -1) < 0)
            goto cleanup;
    }

    if (virDomainNetGetActualBandwidth(detach) &&
        virNetDevSupportBandwidth(virDomainNetGetActualType(detach)) &&
        virNetDevBandwidthClear(detach->ifname) < 0)
        VIR_WARN("cannot clear bandwidth setting for device : %s",
                 detach->ifname);

    /* deactivate the tap/macvtap device on the host, which could also
     * affect the parent device (e.g. macvtap passthrough mode sets
     * the parent device offline)
     */
    ignore_value(qemuInterfaceStopDevice(detach));

    if (!async)
        qemuDomainMarkDeviceForRemoval(vm, &detach->info);

    qemuDomainObjEnterMonitor(driver, vm);
    if (qemuMonitorDelDevice(priv->mon, detach->info.alias) < 0) {
        if (qemuDomainObjExitMonitor(driver, vm) < 0)
            goto cleanup;
        virDomainAuditNet(vm, detach, NULL, "detach", false);
        goto cleanup;
    }
    if (qemuDomainObjExitMonitor(driver, vm) < 0)
        goto cleanup;

    if (async) {
        ret = 0;
    } else {
        if ((ret = qemuDomainWaitForDeviceRemoval(vm)) == 1)
            ret = qemuDomainRemoveNetDevice(driver, vm, detach);
    }

 cleanup:
    if (!async)
        qemuDomainResetDeviceRemoval(vm);
    return ret;
}

int
qemuDomainChangeGraphicsPasswords(virQEMUDriverPtr driver,
                                  virDomainObjPtr vm,
                                  int type,
                                  virDomainGraphicsAuthDefPtr auth,
                                  const char *defaultPasswd,
                                  int asyncJob)
{
    qemuDomainObjPrivatePtr priv = vm->privateData;
    time_t now = time(NULL);
    const char *expire;
    char *validTo = NULL;
    const char *connected = NULL;
    const char *password;
    int ret = -1;
    virQEMUDriverConfigPtr cfg = virQEMUDriverGetConfig(driver);

    if (!auth->passwd && !defaultPasswd) {
        ret = 0;
        goto cleanup;
    }
    password = auth->passwd ? auth->passwd : defaultPasswd;

    if (auth->connected)
        connected = virDomainGraphicsAuthConnectedTypeToString(auth->connected);

    if (qemuDomainObjEnterMonitorAsync(driver, vm, asyncJob) < 0)
        goto cleanup;
    ret = qemuMonitorSetPassword(priv->mon, type, password, connected);

    if (ret == -2) {
        if (type != VIR_DOMAIN_GRAPHICS_TYPE_VNC) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("Graphics password only supported for VNC"));
            ret = -1;
        } else {
            ret = qemuMonitorSetVNCPassword(priv->mon, password);
        }
    }
    if (ret != 0)
        goto end_job;

    if (password[0] == '\0' ||
        (auth->expires && auth->validTo <= now)) {
        expire = "now";
    } else if (auth->expires) {
        if (virAsprintf(&validTo, "%lu", (unsigned long)auth->validTo) < 0)
            goto end_job;
        expire = validTo;
    } else {
        expire = "never";
    }

    ret = qemuMonitorExpirePassword(priv->mon, type, expire);

    if (ret == -2) {
        /* XXX we could fake this with a timer */
        if (auth->expires) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("Expiry of passwords is not supported"));
            ret = -1;
        } else {
            ret = 0;
        }
    }

 end_job:
    if (qemuDomainObjExitMonitor(driver, vm) < 0)
        ret = -1;
 cleanup:
    VIR_FREE(validTo);
    virObjectUnref(cfg);
    return ret;
}

int qemuDomainAttachLease(virQEMUDriverPtr driver,
                          virDomainObjPtr vm,
                          virDomainLeaseDefPtr lease)
{
    int ret = -1;
    virQEMUDriverConfigPtr cfg = virQEMUDriverGetConfig(driver);

    if (virDomainLeaseInsertPreAlloc(vm->def) < 0)
        goto cleanup;

    if (virDomainLockLeaseAttach(driver->lockManager, cfg->uri,
                                 vm, lease) < 0) {
        virDomainLeaseInsertPreAlloced(vm->def, NULL);
        goto cleanup;
    }

    virDomainLeaseInsertPreAlloced(vm->def, lease);
    ret = 0;

 cleanup:
    virObjectUnref(cfg);
    return ret;
}

int qemuDomainDetachLease(virQEMUDriverPtr driver,
                          virDomainObjPtr vm,
                          virDomainLeaseDefPtr lease)
{
    virDomainLeaseDefPtr det_lease;
    int idx;

    if ((idx = virDomainLeaseIndex(vm->def, lease)) < 0) {
        virReportError(VIR_ERR_INVALID_ARG,
                       _("Lease %s in lockspace %s does not exist"),
                       lease->key, NULLSTR(lease->lockspace));
        return -1;
    }

    if (virDomainLockLeaseDetach(driver->lockManager, vm, lease) < 0)
        return -1;

    det_lease = virDomainLeaseRemoveAt(vm->def, idx);
    virDomainLeaseDefFree(det_lease);
    return 0;
}

int qemuDomainDetachChrDevice(virQEMUDriverPtr driver,
                              virDomainObjPtr vm,
                              virDomainChrDefPtr chr,
                              bool async)
{
    int ret = -1;
    qemuDomainObjPrivatePtr priv = vm->privateData;
    virDomainDefPtr vmdef = vm->def;
    virDomainChrDefPtr tmpChr;
    char *devstr = NULL;

    if (!(tmpChr = virDomainChrFind(vmdef, chr))) {
        virReportError(VIR_ERR_DEVICE_MISSING,
                       _("chr type '%s' device not present "
                         "in domain configuration"),
                       virDomainChrDeviceTypeToString(chr->deviceType));
        goto cleanup;
    }

    if (!tmpChr->info.alias && qemuAssignDeviceChrAlias(vmdef, tmpChr, -1) < 0)
        goto cleanup;

    sa_assert(tmpChr->info.alias);

    if (qemuBuildChrDeviceStr(&devstr, vmdef, chr, priv->qemuCaps) < 0)
        goto cleanup;

    if (!async)
        qemuDomainMarkDeviceForRemoval(vm, &tmpChr->info);

    qemuDomainObjEnterMonitor(driver, vm);
    if (devstr && qemuMonitorDelDevice(priv->mon, tmpChr->info.alias) < 0) {
        ignore_value(qemuDomainObjExitMonitor(driver, vm));
        goto cleanup;
    }
    if (qemuDomainObjExitMonitor(driver, vm) < 0)
        goto cleanup;

    if (async) {
        ret = 0;
    } else {
        if ((ret = qemuDomainWaitForDeviceRemoval(vm)) == 1)
            ret = qemuDomainRemoveChrDevice(driver, vm, tmpChr);
    }

 cleanup:
    if (!async)
        qemuDomainResetDeviceRemoval(vm);
    VIR_FREE(devstr);
    return ret;
}


int
qemuDomainDetachRNGDevice(virQEMUDriverPtr driver,
                          virDomainObjPtr vm,
                          virDomainRNGDefPtr rng,
                          bool async)
{
    qemuDomainObjPrivatePtr priv = vm->privateData;
    ssize_t idx;
    virDomainRNGDefPtr tmpRNG;
    int rc;
    int ret = -1;

    if ((idx = virDomainRNGFind(vm->def, rng)) < 0) {
        virReportError(VIR_ERR_DEVICE_MISSING,
                       _("model '%s' RNG device not present "
                         "in domain configuration"),
                       virDomainRNGBackendTypeToString(rng->model));
        return -1;
    }

    tmpRNG = vm->def->rngs[idx];

    if (!tmpRNG->info.alias) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("alias not set for RNG device"));
        return -1;
    }

    if (!async)
        qemuDomainMarkDeviceForRemoval(vm, &tmpRNG->info);

    qemuDomainObjEnterMonitor(driver, vm);
    rc = qemuMonitorDelDevice(priv->mon, tmpRNG->info.alias);
    if (qemuDomainObjExitMonitor(driver, vm) || rc < 0)
        goto cleanup;

    if (async) {
        ret = 0;
    } else {
        if ((ret = qemuDomainWaitForDeviceRemoval(vm)) == 1)
            ret = qemuDomainRemoveRNGDevice(driver, vm, tmpRNG);
    }

 cleanup:
    if (!async)
        qemuDomainResetDeviceRemoval(vm);
    return ret;
}


int
qemuDomainDetachMemoryDevice(virQEMUDriverPtr driver,
                             virDomainObjPtr vm,
                             virDomainMemoryDefPtr memdef,
                             bool async)
{
    qemuDomainObjPrivatePtr priv = vm->privateData;
    virDomainMemoryDefPtr mem;
    int idx;
    int rc;
    int ret = -1;

    qemuDomainMemoryDeviceAlignSize(vm->def, memdef);

    if ((idx = virDomainMemoryFindByDef(vm->def, memdef)) < 0) {
        virReportError(VIR_ERR_DEVICE_MISSING,
                       _("model '%s' memory device not present "
                         "in the domain configuration"),
                       virDomainMemoryModelTypeToString(memdef->model));
        return -1;
    }

    mem = vm->def->mems[idx];

    if (!mem->info.alias) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("alias for the memory device was not found"));
        return -1;
    }

    if (!async)
        qemuDomainMarkDeviceForRemoval(vm, &mem->info);

    qemuDomainObjEnterMonitor(driver, vm);
    rc = qemuMonitorDelDevice(priv->mon, mem->info.alias);
    if (qemuDomainObjExitMonitor(driver, vm) < 0 || rc < 0)
        goto cleanup;

    if (async) {
        ret = 0;
    } else {
        if ((ret = qemuDomainWaitForDeviceRemoval(vm)) == 1)
            ret = qemuDomainRemoveMemoryDevice(driver, vm, mem);
    }

 cleanup:
    if (!async)
        qemuDomainResetDeviceRemoval(vm);
    return ret;
}


static int
qemuDomainRemoveVcpu(virQEMUDriverPtr driver,
                     virDomainObjPtr vm,
                     unsigned int vcpu)
{
    qemuDomainObjPrivatePtr priv = vm->privateData;
    virDomainVcpuDefPtr vcpuinfo = virDomainDefGetVcpu(vm->def, vcpu);
    qemuDomainVcpuPrivatePtr vcpupriv = QEMU_DOMAIN_VCPU_PRIVATE(vcpuinfo);
    int oldvcpus = virDomainDefGetVcpus(vm->def);
    unsigned int nvcpus = vcpupriv->vcpus;
    virErrorPtr save_error = NULL;
    size_t i;

    if (qemuDomainRefreshVcpuInfo(driver, vm, QEMU_ASYNC_JOB_NONE, false) < 0)
        return -1;

    /* validation requires us to set the expected state prior to calling it */
    for (i = vcpu; i < vcpu + nvcpus; i++) {
        vcpuinfo = virDomainDefGetVcpu(vm->def, i);
        vcpuinfo->online = false;
    }

    if (qemuDomainValidateVcpuInfo(vm) < 0) {
        /* rollback vcpu count if the setting has failed */
        virDomainAuditVcpu(vm, oldvcpus, oldvcpus - nvcpus, "update", false);

        for (i = vcpu; i < vcpu + nvcpus; i++) {
            vcpuinfo = virDomainDefGetVcpu(vm->def, i);
            vcpuinfo->online = true;
        }
        return -1;
    }

    virDomainAuditVcpu(vm, oldvcpus, oldvcpus - nvcpus, "update", true);

    virErrorPreserveLast(&save_error);

    for (i = vcpu; i < vcpu + nvcpus; i++)
        ignore_value(virCgroupDelThread(priv->cgroup, VIR_CGROUP_THREAD_VCPU, i));

    virErrorRestore(&save_error);

    return 0;
}


void
qemuDomainRemoveVcpuAlias(virQEMUDriverPtr driver,
                          virDomainObjPtr vm,
                          const char *alias)
{
    virDomainVcpuDefPtr vcpu;
    qemuDomainVcpuPrivatePtr vcpupriv;
    size_t i;

    for (i = 0; i < virDomainDefGetVcpusMax(vm->def); i++) {
        vcpu = virDomainDefGetVcpu(vm->def, i);
        vcpupriv = QEMU_DOMAIN_VCPU_PRIVATE(vcpu);

        if (STREQ_NULLABLE(alias, vcpupriv->alias)) {
            qemuDomainRemoveVcpu(driver, vm, i);
            return;
        }
    }
}


static int
qemuDomainHotplugDelVcpu(virQEMUDriverPtr driver,
                         virQEMUDriverConfigPtr cfg,
                         virDomainObjPtr vm,
                         unsigned int vcpu)
{
    virDomainVcpuDefPtr vcpuinfo = virDomainDefGetVcpu(vm->def, vcpu);
    qemuDomainVcpuPrivatePtr vcpupriv = QEMU_DOMAIN_VCPU_PRIVATE(vcpuinfo);
    int oldvcpus = virDomainDefGetVcpus(vm->def);
    unsigned int nvcpus = vcpupriv->vcpus;
    int rc;
    int ret = -1;

    if (!vcpupriv->alias) {
        virReportError(VIR_ERR_OPERATION_UNSUPPORTED,
                       _("vcpu '%u' can't be unplugged"), vcpu);
        return -1;
    }

    qemuDomainMarkDeviceAliasForRemoval(vm, vcpupriv->alias);

    qemuDomainObjEnterMonitor(driver, vm);

    rc = qemuMonitorDelDevice(qemuDomainGetMonitor(vm), vcpupriv->alias);

    if (qemuDomainObjExitMonitor(driver, vm) < 0)
        goto cleanup;

    if (rc < 0) {
        virDomainAuditVcpu(vm, oldvcpus, oldvcpus - nvcpus, "update", false);
        goto cleanup;
    }

    if ((rc = qemuDomainWaitForDeviceRemoval(vm)) <= 0) {
        if (rc == 0)
            virReportError(VIR_ERR_OPERATION_FAILED, "%s",
                           _("vcpu unplug request timed out"));

        goto cleanup;
    }

    if (qemuDomainRemoveVcpu(driver, vm, vcpu) < 0)
        goto cleanup;

    qemuDomainVcpuPersistOrder(vm->def);

    if (virDomainSaveStatus(driver->xmlopt, cfg->stateDir, vm, driver->caps) < 0)
        goto cleanup;

    ret = 0;

 cleanup:
    qemuDomainResetDeviceRemoval(vm);
    return ret;
}


static int
qemuDomainHotplugAddVcpu(virQEMUDriverPtr driver,
                         virQEMUDriverConfigPtr cfg,
                         virDomainObjPtr vm,
                         unsigned int vcpu)
{
    virJSONValuePtr vcpuprops = NULL;
    virDomainVcpuDefPtr vcpuinfo = virDomainDefGetVcpu(vm->def, vcpu);
    qemuDomainVcpuPrivatePtr vcpupriv = QEMU_DOMAIN_VCPU_PRIVATE(vcpuinfo);
    unsigned int nvcpus = vcpupriv->vcpus;
    bool newhotplug = qemuDomainSupportsNewVcpuHotplug(vm);
    int ret = -1;
    int rc;
    int oldvcpus = virDomainDefGetVcpus(vm->def);
    size_t i;

    if (newhotplug) {
        if (virAsprintf(&vcpupriv->alias, "vcpu%u", vcpu) < 0)
            goto cleanup;

        if (!(vcpuprops = qemuBuildHotpluggableCPUProps(vcpuinfo)))
            goto cleanup;
    }

    qemuDomainObjEnterMonitor(driver, vm);

    if (newhotplug) {
        rc = qemuMonitorAddDeviceArgs(qemuDomainGetMonitor(vm), vcpuprops);
        vcpuprops = NULL;
    } else {
        rc = qemuMonitorSetCPU(qemuDomainGetMonitor(vm), vcpu, true);
    }

    if (qemuDomainObjExitMonitor(driver, vm) < 0)
        goto cleanup;

    virDomainAuditVcpu(vm, oldvcpus, oldvcpus + nvcpus, "update", rc == 0);

    if (rc < 0)
        goto cleanup;

    /* start outputting of the new XML element to allow keeping unpluggability */
    if (newhotplug)
        vm->def->individualvcpus = true;

    if (qemuDomainRefreshVcpuInfo(driver, vm, QEMU_ASYNC_JOB_NONE, false) < 0)
        goto cleanup;

    /* validation requires us to set the expected state prior to calling it */
    for (i = vcpu; i < vcpu + nvcpus; i++) {
        vcpuinfo = virDomainDefGetVcpu(vm->def, i);
        vcpupriv = QEMU_DOMAIN_VCPU_PRIVATE(vcpuinfo);

        vcpuinfo->online = true;

        if (vcpupriv->tid > 0 &&
            qemuProcessSetupVcpu(vm, i) < 0)
            goto cleanup;
    }

    if (qemuDomainValidateVcpuInfo(vm) < 0)
        goto cleanup;

    qemuDomainVcpuPersistOrder(vm->def);

    if (virDomainSaveStatus(driver->xmlopt, cfg->stateDir, vm, driver->caps) < 0)
        goto cleanup;

    ret = 0;

 cleanup:
    virJSONValueFree(vcpuprops);
    return ret;
}


/**
 * qemuDomainSelectHotplugVcpuEntities:
 *
 * @def: domain definition
 * @nvcpus: target vcpu count
 * @enable: set to true if vcpus should be enabled
 *
 * Tries to find which vcpu entities need to be enabled or disabled to reach
 * @nvcpus. This function works in order of the legacy hotplug but is able to
 * skip over entries that are added out of order.
 *
 * Returns the bitmap of vcpus to modify on success, NULL on error.
 */
static virBitmapPtr
qemuDomainSelectHotplugVcpuEntities(virDomainDefPtr def,
                                    unsigned int nvcpus,
                                    bool *enable)
{
    virBitmapPtr ret = NULL;
    virDomainVcpuDefPtr vcpu;
    qemuDomainVcpuPrivatePtr vcpupriv;
    unsigned int maxvcpus = virDomainDefGetVcpusMax(def);
    unsigned int curvcpus = virDomainDefGetVcpus(def);
    ssize_t i;

    if (!(ret = virBitmapNew(maxvcpus)))
        return NULL;

    if (nvcpus > curvcpus) {
        *enable = true;

        for (i = 0; i < maxvcpus && curvcpus < nvcpus; i++) {
            vcpu = virDomainDefGetVcpu(def, i);
            vcpupriv =  QEMU_DOMAIN_VCPU_PRIVATE(vcpu);

            if (vcpu->online)
                continue;

            if (vcpupriv->vcpus == 0)
                continue;

            curvcpus += vcpupriv->vcpus;

            if (curvcpus > nvcpus) {
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                               _("target vm vcpu granularity does not allow the "
                                 "desired vcpu count"));
                goto error;
            }

            ignore_value(virBitmapSetBit(ret, i));
        }
    } else {
        *enable = false;

        for (i = maxvcpus - 1; i >= 0 && curvcpus > nvcpus; i--) {
            vcpu = virDomainDefGetVcpu(def, i);
            vcpupriv =  QEMU_DOMAIN_VCPU_PRIVATE(vcpu);

            if (!vcpu->online)
                continue;

            if (vcpupriv->vcpus == 0)
                continue;

            if (!vcpupriv->alias)
                continue;

            curvcpus -= vcpupriv->vcpus;

            if (curvcpus < nvcpus) {
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                               _("target vm vcpu granularity does not allow the "
                                 "desired vcpu count"));
                goto error;
            }

            ignore_value(virBitmapSetBit(ret, i));
        }
    }

    if (curvcpus != nvcpus) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("failed to find appropriate hotpluggable vcpus to "
                         "reach the desired target vcpu count"));
        goto error;
    }

    return ret;

 error:
    virBitmapFree(ret);
    return NULL;
}


static int
qemuDomainSetVcpusLive(virQEMUDriverPtr driver,
                       virQEMUDriverConfigPtr cfg,
                       virDomainObjPtr vm,
                       virBitmapPtr vcpumap,
                       bool enable)
{
    qemuDomainObjPrivatePtr priv = vm->privateData;
    qemuCgroupEmulatorAllNodesDataPtr emulatorCgroup = NULL;
    ssize_t nextvcpu = -1;
    int ret = -1;

    if (qemuCgroupEmulatorAllNodesAllow(priv->cgroup, &emulatorCgroup) < 0)
        goto cleanup;

    if (enable) {
        while ((nextvcpu = virBitmapNextSetBit(vcpumap, nextvcpu)) != -1) {
            if (qemuDomainHotplugAddVcpu(driver, cfg, vm, nextvcpu) < 0)
                goto cleanup;
        }
    } else {
        for (nextvcpu = virDomainDefGetVcpusMax(vm->def) - 1; nextvcpu >= 0; nextvcpu--) {
            if (!virBitmapIsBitSet(vcpumap, nextvcpu))
                continue;

            if (qemuDomainHotplugDelVcpu(driver, cfg, vm, nextvcpu) < 0)
                goto cleanup;
        }
    }

    ret = 0;

 cleanup:
    qemuCgroupEmulatorAllNodesRestore(emulatorCgroup);

    return ret;
}


/**
 * qemuDomainSetVcpusConfig:
 * @def: config/offline definition of a domain
 * @nvcpus: target vcpu count
 *
 * Properly handle cold(un)plug of vcpus:
 * - plug in inactive vcpus/uplug active rather than rewriting state
 * - fix hotpluggable state
 */
static void
qemuDomainSetVcpusConfig(virDomainDefPtr def,
                         unsigned int nvcpus,
                         bool hotpluggable)
{
    virDomainVcpuDefPtr vcpu;
    size_t curvcpus = virDomainDefGetVcpus(def);
    size_t maxvcpus = virDomainDefGetVcpusMax(def);
    size_t i;

    /* ordering information may become invalid, thus clear it */
    virDomainDefVcpuOrderClear(def);

    if (curvcpus == nvcpus)
        return;

    if (curvcpus < nvcpus) {
        for (i = 0; i < maxvcpus; i++) {
            vcpu = virDomainDefGetVcpu(def, i);

            if (!vcpu)
                continue;

            if (vcpu->online) {
                /* non-hotpluggable vcpus need to be clustered at the beggining,
                 * thus we need to force vcpus to be hotpluggable when we find
                 * vcpus that are hotpluggable and online prior to the ones
                 * we are going to add */
                if (vcpu->hotpluggable == VIR_TRISTATE_BOOL_YES)
                    hotpluggable = true;

                continue;
            }

            vcpu->online = true;
            if (hotpluggable) {
                vcpu->hotpluggable = VIR_TRISTATE_BOOL_YES;
                def->individualvcpus = true;
            } else {
                vcpu->hotpluggable = VIR_TRISTATE_BOOL_NO;
            }

            if (++curvcpus == nvcpus)
                break;
        }
    } else {
        for (i = maxvcpus; i != 0; i--) {
            vcpu = virDomainDefGetVcpu(def, i - 1);

            if (!vcpu || !vcpu->online)
                continue;

            vcpu->online = false;
            vcpu->hotpluggable = VIR_TRISTATE_BOOL_YES;

            if (--curvcpus == nvcpus)
                break;
        }
    }
}


int
qemuDomainSetVcpusInternal(virQEMUDriverPtr driver,
                           virDomainObjPtr vm,
                           virDomainDefPtr def,
                           virDomainDefPtr persistentDef,
                           unsigned int nvcpus,
                           bool hotpluggable)
{
    virQEMUDriverConfigPtr cfg = virQEMUDriverGetConfig(driver);
    virBitmapPtr vcpumap = NULL;
    bool enable;
    int ret = -1;

    if (def && nvcpus > virDomainDefGetVcpusMax(def)) {
        virReportError(VIR_ERR_INVALID_ARG,
                       _("requested vcpus is greater than max allowable"
                         " vcpus for the live domain: %u > %u"),
                       nvcpus, virDomainDefGetVcpusMax(def));
        goto cleanup;
    }

    if (persistentDef && nvcpus > virDomainDefGetVcpusMax(persistentDef)) {
        virReportError(VIR_ERR_INVALID_ARG,
                       _("requested vcpus is greater than max allowable"
                         " vcpus for the persistent domain: %u > %u"),
                       nvcpus, virDomainDefGetVcpusMax(persistentDef));
        goto cleanup;
    }

    if (def) {
        if (!(vcpumap = qemuDomainSelectHotplugVcpuEntities(vm->def, nvcpus,
                                                            &enable)))
            goto cleanup;

        if (qemuDomainSetVcpusLive(driver, cfg, vm, vcpumap, enable) < 0)
            goto cleanup;
    }

    if (persistentDef) {
        qemuDomainSetVcpusConfig(persistentDef, nvcpus, hotpluggable);

        if (virDomainSaveConfig(cfg->configDir, driver->caps, persistentDef) < 0)
            goto cleanup;
    }

    ret = 0;

 cleanup:
    virBitmapFree(vcpumap);
    virObjectUnref(cfg);
    return ret;
}


static void
qemuDomainSetVcpuConfig(virDomainDefPtr def,
                        virBitmapPtr map,
                        bool state)
{
    virDomainVcpuDefPtr vcpu;
    ssize_t next = -1;

    def->individualvcpus = true;

    /* ordering information may become invalid, thus clear it */
    virDomainDefVcpuOrderClear(def);

    while ((next = virBitmapNextSetBit(map, next)) >= 0) {
        if (!(vcpu = virDomainDefGetVcpu(def, next)))
            continue;

        vcpu->online = state;
        vcpu->hotpluggable = VIR_TRISTATE_BOOL_YES;
    }
}


/**
 * qemuDomainFilterHotplugVcpuEntities:
 *
 * Returns a bitmap of hotpluggable vcpu entities that correspond to the logical
 * vcpus requested in @vcpus.
 */
static virBitmapPtr
qemuDomainFilterHotplugVcpuEntities(virDomainDefPtr def,
                                    virBitmapPtr vcpus,
                                    bool state)
{
    qemuDomainVcpuPrivatePtr vcpupriv;
    virDomainVcpuDefPtr vcpu;
    virBitmapPtr map = NULL;
    virBitmapPtr ret = NULL;
    ssize_t next = -1;
    size_t i;

    if (!(map = virBitmapNewCopy(vcpus)))
        return NULL;

    /* make sure that all selected vcpus are in the correct state */
    while ((next = virBitmapNextSetBit(map, next)) >= 0) {
        if (!(vcpu = virDomainDefGetVcpu(def, next)))
            continue;

        if (vcpu->online == state) {
            virReportError(VIR_ERR_INVALID_ARG,
                           _("vcpu '%zd' is already in requested state"), next);
            goto cleanup;
        }

        if (vcpu->online && !vcpu->hotpluggable) {
            virReportError(VIR_ERR_INVALID_ARG,
                           _("vcpu '%zd' can't be hotunplugged"), next);
            goto cleanup;
        }
    }

    /* Make sure that all vCPUs belonging to a single hotpluggable entity were
     * selected and then de-select any sub-threads of it. */
    next = -1;
    while ((next = virBitmapNextSetBit(map, next)) >= 0) {
        if (!(vcpu = virDomainDefGetVcpu(def, next)))
            continue;

        vcpupriv = QEMU_DOMAIN_VCPU_PRIVATE(vcpu);

        if (vcpupriv->vcpus == 0) {
            virReportError(VIR_ERR_INVALID_ARG,
                           _("vcpu '%zd' belongs to a larger hotpluggable entity, "
                             "but siblings were not selected"), next);
            goto cleanup;
        }

        for (i = next + 1; i < next + vcpupriv->vcpus; i++) {
            if (!virBitmapIsBitSet(map, i)) {
                virReportError(VIR_ERR_INVALID_ARG,
                               _("vcpu '%zu' was not selected but it belongs to "
                                 "hotpluggable entity '%zd-%zd' which was "
                                 "partially selected"),
                               i, next, next + vcpupriv->vcpus - 1);
                goto cleanup;
            }

            /* clear the subthreads */
            ignore_value(virBitmapClearBit(map, i));
        }
    }

    VIR_STEAL_PTR(ret, map);

 cleanup:
    virBitmapFree(map);
    return ret;
}


static int
qemuDomainVcpuValidateConfig(virDomainDefPtr def,
                             virBitmapPtr map)
{
    virDomainVcpuDefPtr vcpu;
    size_t maxvcpus = virDomainDefGetVcpusMax(def);
    ssize_t next;
    ssize_t firstvcpu = -1;

    /* vcpu 0 can't be modified */
    if (virBitmapIsBitSet(map, 0)) {
        virReportError(VIR_ERR_INVALID_ARG, "%s",
                       _("vCPU '0' can't be modified"));
        return -1;
    }

    /* non-hotpluggable vcpus need to stay clustered starting from vcpu 0 */
    for (next = virBitmapNextSetBit(map, -1) + 1; next < maxvcpus; next++) {
        if (!(vcpu = virDomainDefGetVcpu(def, next)))
            continue;

        /* skip vcpus being modified */
        if (virBitmapIsBitSet(map, next)) {
            if (firstvcpu < 0)
                firstvcpu = next;

            continue;
        }

        if (vcpu->online && vcpu->hotpluggable == VIR_TRISTATE_BOOL_NO) {
            virReportError(VIR_ERR_INVALID_ARG,
                           _("vcpu '%zd' can't be modified as it is followed "
                             "by non-hotpluggable online vcpus"), firstvcpu);
            return -1;
        }
    }

    return 0;
}


int
qemuDomainSetVcpuInternal(virQEMUDriverPtr driver,
                          virDomainObjPtr vm,
                          virDomainDefPtr def,
                          virDomainDefPtr persistentDef,
                          virBitmapPtr map,
                          bool state)
{
    virQEMUDriverConfigPtr cfg = virQEMUDriverGetConfig(driver);
    virBitmapPtr livevcpus = NULL;
    int ret = -1;

    if (def) {
        if (!qemuDomainSupportsNewVcpuHotplug(vm)) {
            virReportError(VIR_ERR_OPERATION_UNSUPPORTED, "%s",
                           _("this qemu version does not support specific "
                             "vCPU hotplug"));
            goto cleanup;
        }

        if (!(livevcpus = qemuDomainFilterHotplugVcpuEntities(def, map, state)))
            goto cleanup;

        /* Make sure that only one hotpluggable entity is selected.
         * qemuDomainSetVcpusLive allows setting more at once but error
         * resolution in case of a partial failure is hard, so don't let users
         * do so */
        if (virBitmapCountBits(livevcpus) != 1) {
            virReportError(VIR_ERR_OPERATION_UNSUPPORTED, "%s",
                           _("only one hotpluggable entity can be selected"));
            goto cleanup;
        }
    }

    if (persistentDef) {
        if (qemuDomainVcpuValidateConfig(persistentDef, map) < 0)
            goto cleanup;
    }

    if (livevcpus &&
        qemuDomainSetVcpusLive(driver, cfg, vm, livevcpus, state) < 0)
        goto cleanup;

    if (persistentDef) {
        qemuDomainSetVcpuConfig(persistentDef, map, state);

        if (virDomainSaveConfig(cfg->configDir, driver->caps, persistentDef) < 0)
            goto cleanup;
    }

    ret = 0;

 cleanup:
    virBitmapFree(livevcpus);
    virObjectUnref(cfg);
    return ret;
}


int
qemuDomainDetachInputDevice(virDomainObjPtr vm,
                            virDomainInputDefPtr def,
                            bool async)
{
    qemuDomainObjPrivatePtr priv = vm->privateData;
    virQEMUDriverPtr driver = priv->driver;
    virDomainInputDefPtr input;
    int ret = -1;
    int idx;

    if ((idx = virDomainInputDefFind(vm->def, def)) < 0) {
        virReportError(VIR_ERR_OPERATION_FAILED, "%s",
                       _("matching input device not found"));
        return -1;
    }
    input = vm->def->inputs[idx];

    switch ((virDomainInputBus) input->bus) {
    case VIR_DOMAIN_INPUT_BUS_PS2:
    case VIR_DOMAIN_INPUT_BUS_XEN:
    case VIR_DOMAIN_INPUT_BUS_PARALLELS:
        virReportError(VIR_ERR_OPERATION_UNSUPPORTED,
                       _("input device on bus '%s' cannot be detached"),
                       virDomainInputBusTypeToString(input->bus));
        return -1;

    case VIR_DOMAIN_INPUT_BUS_LAST:
    case VIR_DOMAIN_INPUT_BUS_USB:
    case VIR_DOMAIN_INPUT_BUS_VIRTIO:
        break;
    }

    if (!async)
        qemuDomainMarkDeviceForRemoval(vm, &input->info);

    qemuDomainObjEnterMonitor(driver, vm);
    if (qemuMonitorDelDevice(priv->mon, input->info.alias)) {
        ignore_value(qemuDomainObjExitMonitor(driver, vm));
        goto cleanup;
    }
    if (qemuDomainObjExitMonitor(driver, vm) < 0)
        goto cleanup;

    if (async) {
        ret = 0;
    } else {
        if ((ret = qemuDomainWaitForDeviceRemoval(vm)) == 1)
            ret = qemuDomainRemoveInputDevice(vm, input);
    }

 cleanup:
    if (!async)
        qemuDomainResetDeviceRemoval(vm);
    return ret;
}


int
qemuDomainDetachVsockDevice(virDomainObjPtr vm,
                            virDomainVsockDefPtr dev,
                            bool async)
{
    qemuDomainObjPrivatePtr priv = vm->privateData;
    virQEMUDriverPtr driver = priv->driver;
    virDomainVsockDefPtr vsock = vm->def->vsock;
    int ret = -1;


    if (!vsock ||
        !virDomainVsockDefEquals(dev, vsock)) {
        virReportError(VIR_ERR_OPERATION_FAILED, "%s",
                       _("matching vsock device not found"));
        return -1;
    }

    if (!async)
        qemuDomainMarkDeviceForRemoval(vm, &vsock->info);

    qemuDomainObjEnterMonitor(driver, vm);
    if (qemuMonitorDelDevice(priv->mon, vsock->info.alias)) {
        ignore_value(qemuDomainObjExitMonitor(driver, vm));
        goto cleanup;
    }
    if (qemuDomainObjExitMonitor(driver, vm) < 0)
        goto cleanup;

    if (async) {
        ret = 0;
    } else {
        if ((ret = qemuDomainWaitForDeviceRemoval(vm)) == 1)
            ret = qemuDomainRemoveVsockDevice(vm, vsock);
    }

 cleanup:
    if (!async)
        qemuDomainResetDeviceRemoval(vm);
    return ret;
}
