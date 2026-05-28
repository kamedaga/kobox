#include <linux/dma-mapping.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/printk.h>
#include <linux/slab.h>

static int kobox_knob = 7;
module_param(kobox_knob, int, 0444);

static char kobox_data[] = "kobox";
static int kobox_irq_count;

static irqreturn_t kobox_driverish_irq(int irq, void *dev_id)
{
    (void)irq;
    if (dev_id != NULL) {
        (*(int *)dev_id)++;
    }
    return IRQ_HANDLED;
}

static int __init kobox_driverish_init(void)
{
    dma_addr_t dma_handle = 0;
    void *dma = NULL;

    int err = request_irq(0, kobox_driverish_irq, IRQF_SHARED, "kobox_driverish", &kobox_irq_count);
    if (err != 0) {
        pr_info("kobox_driverish: request_irq failed: %d\n", err);
        return err;
    }

    dma = dma_alloc_coherent(NULL, 64, &dma_handle, GFP_KERNEL);
    if (dma == NULL) {
        free_irq(0, &kobox_irq_count);
        pr_info("kobox_driverish: dma_alloc_coherent failed\n");
        return -ENOMEM;
    }

    ((char *)dma)[0] = (char)(kobox_data[0] + kobox_knob);
    dma_free_coherent(NULL, 64, dma, dma_handle);
    free_irq(0, &kobox_irq_count);

    if (kobox_irq_count == 0) {
        pr_info("kobox_driverish: irq was not observed\n");
        return -EIO;
    }

    pr_info("kobox_driverish: loaded knob=%d irq_count=%d\n", kobox_knob, kobox_irq_count);
    return 0;
}

static void __exit kobox_driverish_exit(void)
{
    pr_info("kobox_driverish: unloaded\n");
}

module_init(kobox_driverish_init);
module_exit(kobox_driverish_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("kobox driver-like fixture");
MODULE_AUTHOR("kobox");
