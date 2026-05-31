#pragma once

#include <stddef.h>
#include <stdint.h>

void kb_nvme_subsystem_track_dbbuf(uint64_t dbs_dma, uint64_t eis_dma);
int kb_nvme_subsystem_dbbuf_ready(void);
uint64_t kb_nvme_subsystem_dbbuf_dbs_dma(void);
uint64_t kb_nvme_subsystem_dbbuf_eis_dma(void);
void *kb_nvme_subsystem_dbbuf_dbs_cpu(size_t *out_size);
void *kb_nvme_subsystem_dbbuf_eis_cpu(size_t *out_size);
void kb_nvme_subsystem_reset_dbbuf_shadow(void);
