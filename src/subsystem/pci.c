#include "subsystem/pci.h"

#include <string.h>

static unsigned int pci_irq_vector_count;
static unsigned int pci_irq_vectors[KB_PCI_SUBSYSTEM_IRQ_VECTOR_MAX];

void kb_pci_subsystem_irq_vectors_clear(void)
{
    pci_irq_vector_count = 0;
    memset(pci_irq_vectors, 0, sizeof(pci_irq_vectors));
}

int kb_pci_subsystem_irq_vectors_set(unsigned int count, const unsigned int *vectors)
{
    if (count > KB_PCI_SUBSYSTEM_IRQ_VECTOR_MAX || (count != 0 && vectors == NULL)) {
        return -22;
    }

    kb_pci_subsystem_irq_vectors_clear();
    if (count == 0) {
        return 0;
    }

    memcpy(pci_irq_vectors, vectors, (size_t)count * sizeof(pci_irq_vectors[0]));
    pci_irq_vector_count = count;
    return 0;
}

unsigned int kb_pci_subsystem_irq_vector_count(void)
{
    return pci_irq_vector_count;
}

int kb_pci_subsystem_irq_vector(unsigned int nr, unsigned int *out_vector)
{
    if (out_vector == NULL || nr >= pci_irq_vector_count) {
        return -22;
    }
    *out_vector = pci_irq_vectors[nr];
    return 0;
}
