#include <linux/init.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/printk.h>

static void __iomem *kobox_bar0;
static u32 kobox_observed;

static const struct pci_device_id kobox_pci_ids[] = {
    { PCI_DEVICE(PCI_ANY_ID, PCI_ANY_ID) },
    { 0, }
};
MODULE_DEVICE_TABLE(pci, kobox_pci_ids);

static int kobox_pci_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
    int err;

    (void)id;
    err = pci_enable_device(pdev);
    if (err != 0) {
        pr_info("kobox_pci_minimal: pci_enable_device failed: %d\n", err);
        return err;
    }

    pci_set_master(pdev);
    kobox_bar0 = pci_iomap(pdev, 0, 0);
    if (kobox_bar0 == NULL) {
        pci_disable_device(pdev);
        pr_info("kobox_pci_minimal: pci_iomap failed\n");
        return -ENOMEM;
    }

    iowrite32(0x12345678, kobox_bar0);
    kobox_observed = ioread32(kobox_bar0);
    if (kobox_observed != 0x12345678) {
        pci_iounmap(pdev, kobox_bar0);
        kobox_bar0 = NULL;
        pci_disable_device(pdev);
        pr_info("kobox_pci_minimal: mmio mismatch: %x\n", kobox_observed);
        return -EIO;
    }

    pr_info("kobox_pci_minimal: probe observed=%x\n", kobox_observed);
    return 0;
}

static void kobox_pci_remove(struct pci_dev *pdev)
{
    if (kobox_bar0 != NULL) {
        pci_iounmap(pdev, kobox_bar0);
        kobox_bar0 = NULL;
    }
    pci_disable_device(pdev);
    pr_info("kobox_pci_minimal: remove\n");
}

static struct pci_driver kobox_pci_driver = {
    .name = "kobox_pci_minimal",
    .id_table = kobox_pci_ids,
    .probe = kobox_pci_probe,
    .remove = kobox_pci_remove,
};

static int __init kobox_pci_minimal_init(void)
{
    return pci_register_driver(&kobox_pci_driver);
}

static void __exit kobox_pci_minimal_exit(void)
{
    pci_unregister_driver(&kobox_pci_driver);
}

module_init(kobox_pci_minimal_init);
module_exit(kobox_pci_minimal_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("kobox minimal PCI fixture");
MODULE_AUTHOR("kobox");
