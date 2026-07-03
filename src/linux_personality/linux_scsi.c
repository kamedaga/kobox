#include "kobox/shim.h"
#include "linux_subsystem/scsi/scsi.h"
#include "linux_subsystem/usb/storage.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

void *kb_scsi_host_alloc(void *host_template, int private_size)
{
    if (private_size < 0) {
        return NULL;
    }
    return kb_scsi_subsystem_host_alloc(host_template, (size_t)private_size);
}

int kb_scsi_add_host_with_dma(void *host, void *dev, void *dma_dev)
{
    int result = kb_scsi_subsystem_host_add(host, dev, dma_dev);
    if (result == 0) {
        kb_usb_storage_subsystem_note_scsi_host(host, kb_scsi_subsystem_host_private(host), dev, dma_dev);
    }
    return result;
}

void kb_scsi_scan_host(void *host)
{
    kb_scsi_subsystem_host_scan(host);
    kb_usb_storage_subsystem_note_scsi_scan(host);
}

void kb_scsi_remove_host(void *host)
{
    kb_usb_storage_subsystem_remove_scsi_host(host);
    kb_scsi_subsystem_host_remove(host);
}

void kb_scsi_host_put(void *host)
{
    kb_scsi_subsystem_host_put(host);
}

int kb_scsi_is_host_device(void *dev)
{
    (void)dev;
    return 0;
}

void kb_scsi_done(void *cmd)
{
    (void)cmd;
}

void kb_scsi_report_bus_reset(void *host, int channel)
{
    (void)host;
    (void)channel;
}

void kb_scsi_report_device_reset(void *host, int channel, int target)
{
    (void)host;
    (void)channel;
    (void)target;
}

int kb_scsi_normalize_sense(const void *sense_buffer, int sb_len, void *sshdr)
{
    (void)sense_buffer;
    (void)sb_len;
    (void)sshdr;
    return 0;
}

void *kb_scsi_sense_desc_find(const void *sense_buffer, int sb_len, int desc_type)
{
    (void)sense_buffer;
    (void)sb_len;
    (void)desc_type;
    return NULL;
}

void kb_scsi_eh_prep_cmnd(void *scmd, void *ses, unsigned char *cmnd, int cmnd_size, unsigned sense_bytes)
{
    (void)scmd;
    (void)ses;
    (void)cmnd;
    (void)cmnd_size;
    (void)sense_bytes;
}

void kb_scsi_eh_restore_cmnd(void *scmd, void *ses)
{
    (void)scmd;
    (void)ses;
}

void kb_sg_miter_start(void *miter, void *sgl, unsigned int nents, unsigned int flags)
{
    (void)miter;
    (void)sgl;
    (void)nents;
    (void)flags;
}

int kb_sg_miter_next(void *miter)
{
    (void)miter;
    return 0;
}

int kb_sg_miter_skip(void *miter, unsigned int offset)
{
    (void)miter;
    (void)offset;
    return 0;
}

void kb_sg_miter_stop(void *miter)
{
    (void)miter;
}

int kb_sg_nents(void *sg)
{
    (void)sg;
    return 0;
}

void *kb_sg_next(void *sg)
{
    if (sg == NULL) {
        return NULL;
    }
    uintptr_t page_link = 0;
    memcpy(&page_link, sg, sizeof(page_link));
    if ((page_link & 0x2u) != 0) {
        return NULL;
    }
    if ((page_link & 0x1u) != 0) {
        uintptr_t chain = page_link & ~(uintptr_t)0x3u;
        return (void *)chain;
    }
    return (unsigned char *)sg + 32;
}
