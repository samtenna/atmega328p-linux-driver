#include <linux/printk.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/kernel.h>

static int __init driver_init(void) {
    pr_info("hello, world!\n");

    return 0;
}

static void __exit driver_exit(void) {
    pr_info("goodbye (cruel), world!\n");
}

module_init(driver_init);
module_exit(driver_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("ATMega328P driver");
