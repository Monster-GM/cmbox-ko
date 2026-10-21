#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
static int __init hello_init(void)
{
    /* 不可被"注册类" hook 掩盖的副作用: 置 TAINT_USER 位 (bit6=64) */
    add_taint(TAINT_USER, LOCKDEP_NOW_UNRELIABLE);
    return 0;
}
static void __exit hello_exit(void) { }
module_init(hello_init);
module_exit(hello_exit);
MODULE_LICENSE("GPL");
