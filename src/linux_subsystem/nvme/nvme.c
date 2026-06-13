#include "linux_subsystem/nvme/nvme.h"
#include "linux_subsystem/dma/dma.h"

#include <string.h>

typedef struct kb_nvme_dbbuf_state {
    uint64_t dbs_dma;
    uint64_t eis_dma;
    void *dbs_cpu;
    void *eis_cpu;
    size_t dbs_size;
    size_t eis_size;
} kb_nvme_dbbuf_state_t;

static kb_nvme_dbbuf_state_t dbbuf_state;

void kb_nvme_subsystem_track_dbbuf(uint64_t dbs_dma, uint64_t eis_dma)
{
    dbbuf_state.dbs_dma = dbs_dma;
    dbbuf_state.eis_dma = eis_dma;
    dbbuf_state.dbs_cpu = kb_subsystem_dma_cpu_addr(dbs_dma, &dbbuf_state.dbs_size);
    dbbuf_state.eis_cpu = kb_subsystem_dma_cpu_addr(eis_dma, &dbbuf_state.eis_size);
}

int kb_nvme_subsystem_dbbuf_ready(void)
{
    return dbbuf_state.dbs_dma != 0 && dbbuf_state.eis_dma != 0;
}

uint64_t kb_nvme_subsystem_dbbuf_dbs_dma(void)
{
    return dbbuf_state.dbs_dma;
}

uint64_t kb_nvme_subsystem_dbbuf_eis_dma(void)
{
    return dbbuf_state.eis_dma;
}

void *kb_nvme_subsystem_dbbuf_dbs_cpu(size_t *out_size)
{
    if (out_size != NULL) {
        *out_size = dbbuf_state.dbs_size;
    }
    return dbbuf_state.dbs_cpu;
}

void *kb_nvme_subsystem_dbbuf_eis_cpu(size_t *out_size)
{
    if (out_size != NULL) {
        *out_size = dbbuf_state.eis_size;
    }
    return dbbuf_state.eis_cpu;
}

void kb_nvme_subsystem_reset_dbbuf_shadow(void)
{
    if (dbbuf_state.dbs_cpu != NULL && dbbuf_state.dbs_size != 0) {
        memset(dbbuf_state.dbs_cpu, 0, dbbuf_state.dbs_size);
    }
    if (dbbuf_state.eis_cpu != NULL && dbbuf_state.eis_size != 0) {
        memset(dbbuf_state.eis_cpu, 0, dbbuf_state.eis_size);
    }
}
