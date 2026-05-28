#include <linux/init.h>
#include <linux/module.h>
#include <linux/printk.h>
#include <linux/slab.h>

static int __init kobox_probe_init(void)
{
    void *ptr = kmalloc(16, GFP_KERNEL);
    if (ptr == NULL) {
        pr_info("kobox_probe: kmalloc failed\n");
        return -ENOMEM;
    }

    kfree(ptr);
    pr_info("kobox_probe: loaded\n");
    return 0;
}

static void __exit kobox_probe_exit(void)
{
    pr_info("kobox_probe: unloaded\n");
}

module_init(kobox_probe_init);
module_exit(kobox_probe_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("kobox probe fixture");
MODULE_AUTHOR("kobox");
