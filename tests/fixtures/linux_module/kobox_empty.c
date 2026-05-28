#include <linux/init.h>
#include <linux/module.h>

static int __init kobox_empty_init(void)
{
    return 123;
}

static void __exit kobox_empty_exit(void)
{
}

module_init(kobox_empty_init);
module_exit(kobox_empty_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("kobox empty executable fixture");
MODULE_AUTHOR("kobox");
