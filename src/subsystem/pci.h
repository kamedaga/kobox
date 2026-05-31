#pragma once

#include <stdint.h>

enum {
    KB_PCI_SUBSYSTEM_IRQ_VECTOR_MAX = 256,
};

void kb_pci_subsystem_irq_vectors_clear(void);
int kb_pci_subsystem_irq_vectors_set(unsigned int count, const unsigned int *vectors);
unsigned int kb_pci_subsystem_irq_vector_count(void);
int kb_pci_subsystem_irq_vector(unsigned int nr, unsigned int *out_vector);
