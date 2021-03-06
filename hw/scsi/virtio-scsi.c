/*
 * Virtio SCSI HBA
 *
 * Copyright IBM, Corp. 2010
 * Copyright Red Hat, Inc. 2011
 *
 * Authors:
 *   Stefan Hajnoczi    <stefanha@linux.vnet.ibm.com>
 *   Paolo Bonzini      <pbonzini@redhat.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 */

#include "hw/virtio/virtio-scsi.h"
#include "qemu/error-report.h"
#include <hw/scsi/scsi.h>
#include <block/scsi.h>
#include <hw/virtio/virtio-bus.h>

#define VIRTIO_SCSI_VQ_SIZE     128
#define VIRTIO_SCSI_CDB_SIZE    32
#define VIRTIO_SCSI_SENSE_SIZE  96
#define VIRTIO_SCSI_MAX_CHANNEL 0
#define VIRTIO_SCSI_MAX_TARGET  255
#define VIRTIO_SCSI_MAX_LUN     16383

/* Response codes */
#define VIRTIO_SCSI_S_OK                       0
#define VIRTIO_SCSI_S_OVERRUN                  1
#define VIRTIO_SCSI_S_ABORTED                  2
#define VIRTIO_SCSI_S_BAD_TARGET               3
#define VIRTIO_SCSI_S_RESET                    4
#define VIRTIO_SCSI_S_BUSY                     5
#define VIRTIO_SCSI_S_TRANSPORT_FAILURE        6
#define VIRTIO_SCSI_S_TARGET_FAILURE           7
#define VIRTIO_SCSI_S_NEXUS_FAILURE            8
#define VIRTIO_SCSI_S_FAILURE                  9
#define VIRTIO_SCSI_S_FUNCTION_SUCCEEDED       10
#define VIRTIO_SCSI_S_FUNCTION_REJECTED        11
#define VIRTIO_SCSI_S_INCORRECT_LUN            12

/* Controlq type codes.  */
#define VIRTIO_SCSI_T_TMF                      0
#define VIRTIO_SCSI_T_AN_QUERY                 1
#define VIRTIO_SCSI_T_AN_SUBSCRIBE             2

/* Valid TMF subtypes.  */
#define VIRTIO_SCSI_T_TMF_ABORT_TASK           0
#define VIRTIO_SCSI_T_TMF_ABORT_TASK_SET       1
#define VIRTIO_SCSI_T_TMF_CLEAR_ACA            2
#define VIRTIO_SCSI_T_TMF_CLEAR_TASK_SET       3
#define VIRTIO_SCSI_T_TMF_I_T_NEXUS_RESET      4
#define VIRTIO_SCSI_T_TMF_LOGICAL_UNIT_RESET   5
#define VIRTIO_SCSI_T_TMF_QUERY_TASK           6
#define VIRTIO_SCSI_T_TMF_QUERY_TASK_SET       7

/* Events.  */
#define VIRTIO_SCSI_T_EVENTS_MISSED            0x80000000
#define VIRTIO_SCSI_T_NO_EVENT                 0
#define VIRTIO_SCSI_T_TRANSPORT_RESET          1
#define VIRTIO_SCSI_T_ASYNC_NOTIFY             2
#define VIRTIO_SCSI_T_PARAM_CHANGE             3

/* Reasons for transport reset event */
#define VIRTIO_SCSI_EVT_RESET_HARD             0
#define VIRTIO_SCSI_EVT_RESET_RESCAN           1
#define VIRTIO_SCSI_EVT_RESET_REMOVED          2

/* SCSI command request, followed by data-out */
typedef struct {
    uint8_t lun[8];              /* Logical Unit Number */
    uint64_t tag;                /* Command identifier */
    uint8_t task_attr;           /* Task attribute */
    uint8_t prio;
    uint8_t crn;
    uint8_t cdb[];
} QEMU_PACKED VirtIOSCSICmdReq;

/* Response, followed by sense data and data-in */
typedef struct {
    uint32_t sense_len;          /* Sense data length */
    uint32_t resid;              /* Residual bytes in data buffer */
    uint16_t status_qualifier;   /* Status qualifier */
    uint8_t status;              /* Command completion status */
    uint8_t response;            /* Response values */
    uint8_t sense[];
} QEMU_PACKED VirtIOSCSICmdResp;

/* Task Management Request */
typedef struct {
    uint32_t type;
    uint32_t subtype;
    uint8_t lun[8];
    uint64_t tag;
} QEMU_PACKED VirtIOSCSICtrlTMFReq;

typedef struct {
    uint8_t response;
} QEMU_PACKED VirtIOSCSICtrlTMFResp;

/* Asynchronous notification query/subscription */
typedef struct {
    uint32_t type;
    uint8_t lun[8];
    uint32_t event_requested;
} QEMU_PACKED VirtIOSCSICtrlANReq;

typedef struct {
    uint32_t event_actual;
    uint8_t response;
} QEMU_PACKED VirtIOSCSICtrlANResp;

typedef struct {
    uint32_t event;
    uint8_t lun[8];
    uint32_t reason;
} QEMU_PACKED VirtIOSCSIEvent;

typedef struct {
    uint32_t num_queues;
    uint32_t seg_max;
    uint32_t max_sectors;
    uint32_t cmd_per_lun;
    uint32_t event_info_size;
    uint32_t sense_size;
    uint32_t cdb_size;
    uint16_t max_channel;
    uint16_t max_target;
    uint32_t max_lun;
} QEMU_PACKED VirtIOSCSIConfig;

typedef struct VirtIOSCSIReq {
    VirtIOSCSI *dev;
    VirtQueue *vq;
    VirtQueueElement elem;
    QEMUSGList qsgl;
    SCSIRequest *sreq;
    union {
        char                  *buf;
        VirtIOSCSICmdReq      *cmd;
        VirtIOSCSICtrlTMFReq  *tmf;
        VirtIOSCSICtrlANReq   *an;
    } req;
    union {
        char                  *buf;
        VirtIOSCSICmdResp     *cmd;
        VirtIOSCSICtrlTMFResp *tmf;
        VirtIOSCSICtrlANResp  *an;
        VirtIOSCSIEvent       *event;
    } resp;
} VirtIOSCSIReq;

static inline int virtio_scsi_get_lun(uint8_t *lun)
{
    return ((lun[2] << 8) | lun[3]) & 0x3FFF;
}

static inline SCSIDevice *virtio_scsi_device_find(VirtIOSCSI *s, uint8_t *lun)
{
    if (lun[0] != 1) {
        return NULL;
    }
    if (lun[2] != 0 && !(lun[2] >= 0x40 && lun[2] < 0x80)) {
        return NULL;
    }
    return scsi_device_find(&s->bus, 0, lun[1], virtio_scsi_get_lun(lun));
}

static void virtio_scsi_complete_req(VirtIOSCSIReq *req)
{
    VirtIOSCSI *s = req->dev;
    VirtQueue *vq = req->vq;
    VirtIODevice *vdev = VIRTIO_DEVICE(s);
    virtqueue_push(vq, &req->elem, req->qsgl.size + req->elem.in_sg[0].iov_len);
    qemu_sglist_destroy(&req->qsgl);
    if (req->sreq) {
        req->sreq->hba_private = NULL;
        scsi_req_unref(req->sreq);
    }
    g_free(req);
    virtio_notify(vdev, vq);
}

static void virtio_scsi_bad_req(void)
{
    error_report("wrong size for virtio-scsi headers");
    exit(1);
}

static void qemu_sgl_init_external(QEMUSGList *qsgl, struct iovec *sg,
                                   hwaddr *addr, int num)
{
    qemu_sglist_init(qsgl, num, &dma_context_memory);
    while (num--) {
        qemu_sglist_add(qsgl, *(addr++), (sg++)->iov_len);
    }
}

static void virtio_scsi_parse_req(VirtIOSCSI *s, VirtQueue *vq,
                                  VirtIOSCSIReq *req)
{
    assert(req->elem.in_num);
    req->vq = vq;
    req->dev = s;
    req->sreq = NULL;
    if (req->elem.out_num) {
        req->req.buf = req->elem.out_sg[0].iov_base;
    }
    req->resp.buf = req->elem.in_sg[0].iov_base;

    if (req->elem.out_num > 1) {
        qemu_sgl_init_external(&req->qsgl, &req->elem.out_sg[1],
                               &req->elem.out_addr[1],
                               req->elem.out_num - 1);
    } else {
        qemu_sgl_init_external(&req->qsgl, &req->elem.in_sg[1],
                               &req->elem.in_addr[1],
                               req->elem.in_num - 1);
    }
}

static VirtIOSCSIReq *virtio_scsi_pop_req(VirtIOSCSI *s, VirtQueue *vq)
{
    VirtIOSCSIReq *req;
    req = g_malloc(sizeof(*req));
    if (!virtqueue_pop(vq, &req->elem)) {
        g_free(req);
        return NULL;
    }

    virtio_scsi_parse_req(s, vq, req);
    return req;
}

static void virtio_scsi_save_request(QEMUFile *f, SCSIRequest *sreq)
{
    VirtIOSCSIReq *req = sreq->hba_private;
    uint32_t n = virtio_queue_get_id(req->vq) - 2;

    assert(n < req->dev->conf.num_queues);
    qemu_put_be32s(f, &n);
    qemu_put_buffer(f, (unsigned char *)&req->elem, sizeof(req->elem));
}

static void *virtio_scsi_load_request(QEMUFile *f, SCSIRequest *sreq)
{
    SCSIBus *bus = sreq->bus;
    VirtIOSCSI *s = container_of(bus, VirtIOSCSI, bus);
    VirtIOSCSIReq *req;
    uint32_t n;

    req = g_malloc(sizeof(*req));
    qemu_get_be32s(f, &n);
    assert(n < s->conf.num_queues);
    qemu_get_buffer(f, (unsigned char *)&req->elem, sizeof(req->elem));
    virtio_scsi_parse_req(s, s->cmd_vqs[n], req);

    scsi_req_ref(sreq);
    req->sreq = sreq;
    if (req->sreq->cmd.mode != SCSI_XFER_NONE) {
        int req_mode =
            (req->elem.in_num > 1 ? SCSI_XFER_FROM_DEV : SCSI_XFER_TO_DEV);

        assert(req->sreq->cmd.mode == req_mode);
    }
    return req;
}

static void virtio_scsi_do_tmf(VirtIOSCSI *s, VirtIOSCSIReq *req)
{
    SCSIDevice *d = virtio_scsi_device_find(s, req->req.tmf->lun);
    SCSIRequest *r, *next;
    BusChild *kid;
    int target;

    /* Here VIRTIO_SCSI_S_OK means "FUNCTION COMPLETE".  */
    req->resp.tmf->response = VIRTIO_SCSI_S_OK;

    switch (req->req.tmf->subtype) {
    case VIRTIO_SCSI_T_TMF_ABORT_TASK:
    case VIRTIO_SCSI_T_TMF_QUERY_TASK:
        if (!d) {
            goto fail;
        }
        if (d->lun != virtio_scsi_get_lun(req->req.tmf->lun)) {
            goto incorrect_lun;
        }
        QTAILQ_FOREACH_SAFE(r, &d->requests, next, next) {
            VirtIOSCSIReq *cmd_req = r->hba_private;
            if (cmd_req && cmd_req->req.cmd->tag == req->req.tmf->tag) {
                break;
            }
        }
        if (r) {
            /*
             * Assert that the request has not been completed yet, we
             * check for it in the loop above.
             */
            assert(r->hba_private);
            if (req->req.tmf->subtype == VIRTIO_SCSI_T_TMF_QUERY_TASK) {
                /* "If the specified command is present in the task set, then
                 * return a service response set to FUNCTION SUCCEEDED".
                 */
                req->resp.tmf->response = VIRTIO_SCSI_S_FUNCTION_SUCCEEDED;
            } else {
                scsi_req_cancel(r);
            }
        }
        break;

    case VIRTIO_SCSI_T_TMF_LOGICAL_UNIT_RESET:
        if (!d) {
            goto fail;
        }
        if (d->lun != virtio_scsi_get_lun(req->req.tmf->lun)) {
            goto incorrect_lun;
        }
        s->resetting++;
        qdev_reset_all(&d->qdev);
        s->resetting--;
        break;

    case VIRTIO_SCSI_T_TMF_ABORT_TASK_SET:
    case VIRTIO_SCSI_T_TMF_CLEAR_TASK_SET:
    case VIRTIO_SCSI_T_TMF_QUERY_TASK_SET:
        if (!d) {
            goto fail;
        }
        if (d->lun != virtio_scsi_get_lun(req->req.tmf->lun)) {
            goto incorrect_lun;
        }
        QTAILQ_FOREACH_SAFE(r, &d->requests, next, next) {
            if (r->hba_private) {
                if (req->req.tmf->subtype == VIRTIO_SCSI_T_TMF_QUERY_TASK_SET) {
                    /* "If there is any command present in the task set, then
                     * return a service response set to FUNCTION SUCCEEDED".
                     */
                    req->resp.tmf->response = VIRTIO_SCSI_S_FUNCTION_SUCCEEDED;
                    break;
                } else {
                    scsi_req_cancel(r);
                }
            }
        }
        break;

    case VIRTIO_SCSI_T_TMF_I_T_NEXUS_RESET:
        target = req->req.tmf->lun[1];
        s->resetting++;
        QTAILQ_FOREACH(kid, &s->bus.qbus.children, sibling) {
             d = DO_UPCAST(SCSIDevice, qdev, kid->child);
             if (d->channel == 0 && d->id == target) {
                qdev_reset_all(&d->qdev);
             }
        }
        s->resetting--;
        break;

    case VIRTIO_SCSI_T_TMF_CLEAR_ACA:
    default:
        req->resp.tmf->response = VIRTIO_SCSI_S_FUNCTION_REJECTED;
        break;
    }

    return;

incorrect_lun:
    req->resp.tmf->response = VIRTIO_SCSI_S_INCORRECT_LUN;
    return;

fail:
    req->resp.tmf->response = VIRTIO_SCSI_S_BAD_TARGET;
}

static void virtio_scsi_handle_ctrl(VirtIODevice *vdev, VirtQueue *vq)
{
    VirtIOSCSI *s = (VirtIOSCSI *)vdev;
    VirtIOSCSIReq *req;

    while ((req = virtio_scsi_pop_req(s, vq))) {
        int out_size, in_size;
        if (req->elem.out_num < 1 || req->elem.in_num < 1) {
            virtio_scsi_bad_req();
            continue;
        }

        out_size = req->elem.out_sg[0].iov_len;
        in_size = req->elem.in_sg[0].iov_len;
        if (req->req.tmf->type == VIRTIO_SCSI_T_TMF) {
            if (out_size < sizeof(VirtIOSCSICtrlTMFReq) ||
                in_size < sizeof(VirtIOSCSICtrlTMFResp)) {
                virtio_scsi_bad_req();
            }
            virtio_scsi_do_tmf(s, req);

        } else if (req->req.tmf->type == VIRTIO_SCSI_T_AN_QUERY ||
                   req->req.tmf->type == VIRTIO_SCSI_T_AN_SUBSCRIBE) {
            if (out_size < sizeof(VirtIOSCSICtrlANReq) ||
                in_size < sizeof(VirtIOSCSICtrlANResp)) {
                virtio_scsi_bad_req();
            }
            req->resp.an->event_actual = 0;
            req->resp.an->response = VIRTIO_SCSI_S_OK;
        }
        virtio_scsi_complete_req(req);
    }
}

static void virtio_scsi_command_complete(SCSIRequest *r, uint32_t status,
                                         size_t resid)
{
    VirtIOSCSIReq *req = r->hba_private;
    uint32_t sense_len;

    req->resp.cmd->response = VIRTIO_SCSI_S_OK;
    req->resp.cmd->status = status;
    if (req->resp.cmd->status == GOOD) {
        req->resp.cmd->resid = tswap32(resid);
    } else {
        req->resp.cmd->resid = 0;
        sense_len = scsi_req_get_sense(r, req->resp.cmd->sense,
                                       VIRTIO_SCSI_SENSE_SIZE);
        req->resp.cmd->sense_len = tswap32(sense_len);
    }
    virtio_scsi_complete_req(req);
}

static QEMUSGList *virtio_scsi_get_sg_list(SCSIRequest *r)
{
    VirtIOSCSIReq *req = r->hba_private;

    return &req->qsgl;
}

static void virtio_scsi_request_cancelled(SCSIRequest *r)
{
    VirtIOSCSIReq *req = r->hba_private;

    if (!req) {
        return;
    }
    if (req->dev->resetting) {
        req->resp.cmd->response = VIRTIO_SCSI_S_RESET;
    } else {
        req->resp.cmd->response = VIRTIO_SCSI_S_ABORTED;
    }
    virtio_scsi_complete_req(req);
}

static void virtio_scsi_fail_cmd_req(VirtIOSCSIReq *req)
{
    req->resp.cmd->response = VIRTIO_SCSI_S_FAILURE;
    virtio_scsi_complete_req(req);
}

static void virtio_scsi_handle_cmd(VirtIODevice *vdev, VirtQueue *vq)
{
    VirtIOSCSI *s = (VirtIOSCSI *)vdev;
    VirtIOSCSIReq *req;
    int n;

    while ((req = virtio_scsi_pop_req(s, vq))) {
        SCSIDevice *d;
        int out_size, in_size;
        if (req->elem.out_num < 1 || req->elem.in_num < 1) {
            virtio_scsi_bad_req();
        }

        out_size = req->elem.out_sg[0].iov_len;
        in_size = req->elem.in_sg[0].iov_len;
        if (out_size < sizeof(VirtIOSCSICmdReq) + s->cdb_size ||
            in_size < sizeof(VirtIOSCSICmdResp) + s->sense_size) {
            virtio_scsi_bad_req();
        }

        if (req->elem.out_num > 1 && req->elem.in_num > 1) {
            virtio_scsi_fail_cmd_req(req);
            continue;
        }

        d = virtio_scsi_device_find(s, req->req.cmd->lun);
        if (!d) {
            req->resp.cmd->response = VIRTIO_SCSI_S_BAD_TARGET;
            virtio_scsi_complete_req(req);
            continue;
        }
        req->sreq = scsi_req_new(d, req->req.cmd->tag,
                                 virtio_scsi_get_lun(req->req.cmd->lun),
                                 req->req.cmd->cdb, req);

        if (req->sreq->cmd.mode != SCSI_XFER_NONE) {
            int req_mode =
                (req->elem.in_num > 1 ? SCSI_XFER_FROM_DEV : SCSI_XFER_TO_DEV);

            if (req->sreq->cmd.mode != req_mode ||
                req->sreq->cmd.xfer > req->qsgl.size) {
                req->resp.cmd->response = VIRTIO_SCSI_S_OVERRUN;
                virtio_scsi_complete_req(req);
                continue;
            }
        }

        n = scsi_req_enqueue(req->sreq);
        if (n) {
            scsi_req_continue(req->sreq);
        }
    }
}

static void virtio_scsi_get_config(VirtIODevice *vdev,
                                   uint8_t *config)
{
    VirtIOSCSIConfig *scsiconf = (VirtIOSCSIConfig *)config;
    VirtIOSCSI *s = (VirtIOSCSI *)vdev;

    stl_raw(&scsiconf->num_queues, s->conf.num_queues);
    stl_raw(&scsiconf->seg_max, 128 - 2);
    stl_raw(&scsiconf->max_sectors, s->conf.max_sectors);
    stl_raw(&scsiconf->cmd_per_lun, s->conf.cmd_per_lun);
    stl_raw(&scsiconf->event_info_size, sizeof(VirtIOSCSIEvent));
    stl_raw(&scsiconf->sense_size, s->sense_size);
    stl_raw(&scsiconf->cdb_size, s->cdb_size);
    stw_raw(&scsiconf->max_channel, VIRTIO_SCSI_MAX_CHANNEL);
    stw_raw(&scsiconf->max_target, VIRTIO_SCSI_MAX_TARGET);
    stl_raw(&scsiconf->max_lun, VIRTIO_SCSI_MAX_LUN);
}

static void virtio_scsi_set_config(VirtIODevice *vdev,
                                   const uint8_t *config)
{
    VirtIOSCSIConfig *scsiconf = (VirtIOSCSIConfig *)config;
    VirtIOSCSI *s = (VirtIOSCSI *)vdev;

    if ((uint32_t) ldl_raw(&scsiconf->sense_size) >= 65536 ||
        (uint32_t) ldl_raw(&scsiconf->cdb_size) >= 256) {
        error_report("bad data written to virtio-scsi configuration space");
        exit(1);
    }

    s->sense_size = ldl_raw(&scsiconf->sense_size);
    s->cdb_size = ldl_raw(&scsiconf->cdb_size);
}

static uint32_t virtio_scsi_get_features(VirtIODevice *vdev,
                                         uint32_t requested_features)
{
    return requested_features;
}

static void virtio_scsi_reset(VirtIODevice *vdev)
{
    VirtIOSCSI *s = (VirtIOSCSI *)vdev;

    s->resetting++;
    qbus_reset_all(&s->bus.qbus);
    s->resetting--;

    s->sense_size = VIRTIO_SCSI_SENSE_SIZE;
    s->cdb_size = VIRTIO_SCSI_CDB_SIZE;
    s->events_dropped = false;
}

/* The device does not have anything to save beyond the virtio data.
 * Request data is saved with callbacks from SCSI devices.
 */
static void virtio_scsi_save(QEMUFile *f, void *opaque)
{
    VirtIODevice *vdev = VIRTIO_DEVICE(opaque);
    virtio_save(vdev, f);
}

static int virtio_scsi_load(QEMUFile *f, void *opaque, int version_id)
{
    VirtIODevice *vdev = VIRTIO_DEVICE(opaque);
    int ret;

    ret = virtio_load(vdev, f);
    if (ret) {
        return ret;
    }
    return 0;
}

static void virtio_scsi_push_event(VirtIOSCSI *s, SCSIDevice *dev,
                                   uint32_t event, uint32_t reason)
{
    VirtIOSCSIReq *req = virtio_scsi_pop_req(s, s->event_vq);
    VirtIOSCSIEvent *evt;
    VirtIODevice *vdev = VIRTIO_DEVICE(s);
    int in_size;

    if (!(vdev->status & VIRTIO_CONFIG_S_DRIVER_OK)) {
        return;
    }

    if (!req) {
        s->events_dropped = true;
        return;
    }

    if (req->elem.out_num || req->elem.in_num != 1) {
        virtio_scsi_bad_req();
    }

    if (s->events_dropped) {
        event |= VIRTIO_SCSI_T_EVENTS_MISSED;
        s->events_dropped = false;
    }

    in_size = req->elem.in_sg[0].iov_len;
    if (in_size < sizeof(VirtIOSCSIEvent)) {
        virtio_scsi_bad_req();
    }

    evt = req->resp.event;
    memset(evt, 0, sizeof(VirtIOSCSIEvent));
    evt->event = event;
    evt->reason = reason;
    if (!dev) {
        assert(event == VIRTIO_SCSI_T_NO_EVENT);
    } else {
        evt->lun[0] = 1;
        evt->lun[1] = dev->id;

        /* Linux wants us to keep the same encoding we use for REPORT LUNS.  */
        if (dev->lun >= 256) {
            evt->lun[2] = (dev->lun >> 8) | 0x40;
        }
        evt->lun[3] = dev->lun & 0xFF;
    }
    virtio_scsi_complete_req(req);
}

static void virtio_scsi_handle_event(VirtIODevice *vdev, VirtQueue *vq)
{
    VirtIOSCSI *s = VIRTIO_SCSI(vdev);

    if (s->events_dropped) {
        virtio_scsi_push_event(s, NULL, VIRTIO_SCSI_T_NO_EVENT, 0);
    }
}

static void virtio_scsi_change(SCSIBus *bus, SCSIDevice *dev, SCSISense sense)
{
    VirtIOSCSI *s = container_of(bus, VirtIOSCSI, bus);
    VirtIODevice *vdev = VIRTIO_DEVICE(s);

    if (((vdev->guest_features >> VIRTIO_SCSI_F_CHANGE) & 1) &&
        dev->type != TYPE_ROM) {
        virtio_scsi_push_event(s, dev, VIRTIO_SCSI_T_PARAM_CHANGE,
                               sense.asc | (sense.ascq << 8));
    }
}

static void virtio_scsi_hotplug(SCSIBus *bus, SCSIDevice *dev)
{
    VirtIOSCSI *s = container_of(bus, VirtIOSCSI, bus);
    VirtIODevice *vdev = VIRTIO_DEVICE(s);

    if ((vdev->guest_features >> VIRTIO_SCSI_F_HOTPLUG) & 1) {
        virtio_scsi_push_event(s, dev, VIRTIO_SCSI_T_TRANSPORT_RESET,
                               VIRTIO_SCSI_EVT_RESET_RESCAN);
    }
}

static void virtio_scsi_hot_unplug(SCSIBus *bus, SCSIDevice *dev)
{
    VirtIOSCSI *s = container_of(bus, VirtIOSCSI, bus);
    VirtIODevice *vdev = VIRTIO_DEVICE(s);

    if ((vdev->guest_features >> VIRTIO_SCSI_F_HOTPLUG) & 1) {
        virtio_scsi_push_event(s, dev, VIRTIO_SCSI_T_TRANSPORT_RESET,
                               VIRTIO_SCSI_EVT_RESET_REMOVED);
    }
}

static struct SCSIBusInfo virtio_scsi_scsi_info = {
    .tcq = true,
    .max_channel = VIRTIO_SCSI_MAX_CHANNEL,
    .max_target = VIRTIO_SCSI_MAX_TARGET,
    .max_lun = VIRTIO_SCSI_MAX_LUN,

    .complete = virtio_scsi_command_complete,
    .cancel = virtio_scsi_request_cancelled,
    .change = virtio_scsi_change,
    .hotplug = virtio_scsi_hotplug,
    .hot_unplug = virtio_scsi_hot_unplug,
    .get_sg_list = virtio_scsi_get_sg_list,
    .save_request = virtio_scsi_save_request,
    .load_request = virtio_scsi_load_request,
};

static int virtio_scsi_device_init(VirtIODevice *vdev)
{
    DeviceState *qdev = DEVICE(vdev);
    VirtIOSCSI *s = VIRTIO_SCSI(vdev);
    static int virtio_scsi_id;
    int i;

    virtio_init(VIRTIO_DEVICE(s), "virtio-scsi", VIRTIO_ID_SCSI,
                sizeof(VirtIOSCSIConfig));

    s->cmd_vqs = g_malloc0(s->conf.num_queues * sizeof(VirtQueue *));

    /* TODO set up vdev function pointers */
    vdev->get_config = virtio_scsi_get_config;
    vdev->set_config = virtio_scsi_set_config;
    vdev->get_features = virtio_scsi_get_features;
    vdev->reset = virtio_scsi_reset;

    s->ctrl_vq = virtio_add_queue(vdev, VIRTIO_SCSI_VQ_SIZE,
                                  virtio_scsi_handle_ctrl);
    s->event_vq = virtio_add_queue(vdev, VIRTIO_SCSI_VQ_SIZE,
                                   virtio_scsi_handle_event);
    for (i = 0; i < s->conf.num_queues; i++) {
        s->cmd_vqs[i] = virtio_add_queue(vdev, VIRTIO_SCSI_VQ_SIZE,
                                         virtio_scsi_handle_cmd);
    }

    scsi_bus_new(&s->bus, qdev, &virtio_scsi_scsi_info);
    if (!qdev->hotplugged) {
        scsi_bus_legacy_handle_cmdline(&s->bus);
    }

    register_savevm(qdev, "virtio-scsi", virtio_scsi_id++, 1,
                    virtio_scsi_save, virtio_scsi_load, s);

    return 0;
}

static int virtio_scsi_device_exit(DeviceState *qdev)
{
    VirtIOSCSI *s = VIRTIO_SCSI(qdev);
    VirtIODevice *vdev = VIRTIO_DEVICE(qdev);

    unregister_savevm(qdev, "virtio-scsi", s);
    g_free(s->cmd_vqs);
    virtio_common_cleanup(vdev);
    return 0;
}

static Property virtio_scsi_properties[] = {
    DEFINE_VIRTIO_SCSI_PROPERTIES(VirtIOSCSI, conf),
    DEFINE_PROP_END_OF_LIST(),
};

static void virtio_scsi_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    VirtioDeviceClass *vdc = VIRTIO_DEVICE_CLASS(klass);
    dc->exit = virtio_scsi_device_exit;
    dc->props = virtio_scsi_properties;
    vdc->init = virtio_scsi_device_init;
    vdc->get_config = virtio_scsi_get_config;
    vdc->set_config = virtio_scsi_set_config;
    vdc->get_features = virtio_scsi_get_features;
    vdc->reset = virtio_scsi_reset;
}

static const TypeInfo virtio_scsi_info = {
    .name = TYPE_VIRTIO_SCSI,
    .parent = TYPE_VIRTIO_DEVICE,
    .instance_size = sizeof(VirtIOSCSI),
    .class_init = virtio_scsi_class_init,
};

static void virtio_register_types(void)
{
    type_register_static(&virtio_scsi_info);
}

type_init(virtio_register_types)
