#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>

static int hm_open(struct inode *i, struct file *f) { return 0; }
static const struct file_operations hm_fops = { .owner = THIS_MODULE, .open = hm_open };
static struct miscdevice hm = {
    .minor = MISC_DYNAMIC_MINOR, .name = "hellomisc", .fops = &hm_fops, .mode = 0666,
};
static int __init hello_init(void)
{
    int r = misc_register(&hm);
    pr_emerg("HELLO_INIT r=%d\n", r);
    return 0;   /* 即使注册失败也保持加载, 便于观测 */
}
static void __exit hello_exit(void) { }
module_init(hello_init);
module_exit(hello_exit);
MODULE_LICENSE("GPL");
