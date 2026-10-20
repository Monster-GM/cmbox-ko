// vscreen.c — 抓屏内核模块 v0.2
//
// 编译行为:
//   CONFIG_FB=y  (有 fbdev 的内核): 完整功能 — fb notifier 抓 fb_info,
//                /dev/vscreen mmap 直通显存, ioctl 控制
//   CONFIG_FB=n  (GKI 5.10+ 无 fbdev): 编译为占位模块, ioctl 返回 -ENODEV
//                (真正的抓屏走 v2 DRM plane 通道, 见 03_内核投屏设计.md)
//
// 用法 (CONFIG_FB=y 时):
//   insmod vscreen.ko  → /dev/vscreen
//   ioctl VS_GET_INFO / VS_CAPTURE / VS_WAIT + mmap

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/wait.h>
#include <linux/sched.h>

#ifdef CONFIG_FB
#include <linux/fb.h>
#endif

#define VS_NAME "vscreen"

#define VS_IOC_MAGIC 'S'
struct vs_info {
    __u32 width;
    __u32 height;
    __u32 stride;
    __u32 bpp;
    __u32 frame_size;
    __u32 seq;
};
#define VS_GET_INFO  _IOR(VS_IOC_MAGIC, 1, struct vs_info)
#define VS_CAPTURE   _IO (VS_IOC_MAGIC, 2)
#define VS_WAIT      _IOW(VS_IOC_MAGIC, 3, __u32)

#ifdef CONFIG_FB

static struct fb_info *vs_fb;
static int vs_fb_index = 0;
module_param(vs_fb_index, int, 0444);

static DECLARE_WAIT_QUEUE_HEAD(vs_wq);
static atomic_t frame_seq = ATOMIC_INIT(0);

static int vs_fb_notifier(struct notifier_block *nb, unsigned long event, void *data)
{
    struct fb_event *e = data;

#ifdef FB_EVENT_FB_REGISTERED
    if (event == FB_EVENT_FB_REGISTERED && !vs_fb && e->info) {
        vs_fb = e->info;
        pr_info("vscreen: attached fb %dx%d bpp%u\n",
                vs_fb->var.xres, vs_fb->var.yres, vs_fb->var.bits_per_pixel);
    }
#endif
    if (vs_fb) {
        atomic_inc(&frame_seq);
        wake_up_interruptible(&vs_wq);
    }
    return NOTIFY_OK;
}

static struct notifier_block vs_nb = { .notifier_call = vs_fb_notifier };

static long vs_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
    struct vs_info i;
    __u32 last;

    if (!vs_fb) return -ENODEV;

    switch (cmd) {
    case VS_GET_INFO:
        i.width      = vs_fb->var.xres;
        i.height     = vs_fb->var.yres;
        i.stride     = vs_fb->fix.line_length;
        i.bpp        = vs_fb->var.bits_per_pixel;
        i.frame_size = vs_fb->fix.line_length * vs_fb->var.yres;
        i.seq        = atomic_read(&frame_seq);
        if (copy_to_user((void __user *)arg, &i, sizeof(i)))
            return -EFAULT;
        return 0;

    case VS_CAPTURE:
        atomic_inc(&frame_seq);
        wake_up_interruptible(&vs_wq);
        return 0;

    case VS_WAIT:
        if (copy_from_user(&last, (void __user *)arg, sizeof(last)))
            return -EFAULT;
        wait_event_interruptible_timeout(vs_wq,
            atomic_read(&frame_seq) != last, msecs_to_jiffies(16));
        return atomic_read(&frame_seq);
    }
    return -ENOTTY;
}

static int vs_mmap(struct file *f, struct vm_area_struct *vma)
{
    if (!vs_fb) return -ENODEV;
    /* vmalloc 显存 */
    if (vs_fb->screen_buffer && vs_fb->fix.smem_start == 0)
        return remap_vmalloc_range(vma, vs_fb->screen_buffer, 0);
    /* 物理连续显存 */
    return remap_pfn_range(vma, vma->vm_start,
                           vs_fb->fix.smem_start >> PAGE_SHIFT,
                           vma->vm_end - vma->vm_start,
                           vma->vm_page_prot);
}

static const struct file_operations vs_fops = {
    .owner          = THIS_MODULE,
    .unlocked_ioctl = vs_ioctl,
    .compat_ioctl   = vs_ioctl,
    .mmap           = vs_mmap,
};

static struct miscdevice vs_misc = {
    .minor = MISC_DYNAMIC_MINOR,
    .name  = VS_NAME,
    .fops  = &vs_fops,
    .mode  = 0666,
};

static int __init vs_init(void)
{
    int ret = fb_register_client(&vs_nb);
    if (ret) return ret;

    ret = misc_register(&vs_misc);
    if (ret) { fb_unregister_client(&vs_nb); return ret; }

    /* insmod 早于 fb 注册时由 notifier 挂接; insmod 晚于 fb 注册时
     * 会错过通知 → 用户态触发一次 fb 事件(切分辨率/亮灭屏)即挂接 */
    pr_info("vscreen: ready (waiting for fb attachment)\n");
    return 0;
}

static void __exit vs_exit(void)
{
    misc_deregister(&vs_misc);
    fb_unregister_client(&vs_nb);
}

#else /* !CONFIG_FB — GKI 占位模块 */

static long vs_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
    pr_info_once("vscreen: no capture backend on this kernel (DRM v2 planned)\n");
    return -ENODEV;
}

static const struct file_operations vs_fops = {
    .owner          = THIS_MODULE,
    .unlocked_ioctl = vs_ioctl,
};

static struct miscdevice vs_misc = {
    .minor = MISC_DYNAMIC_MINOR,
    .name  = VS_NAME,
    .fops  = &vs_fops,
    .mode  = 0666,
};

static int __init vs_init(void)
{
    pr_info("vscreen: stub (CONFIG_FB=n), capture backend pending DRM v2\n");
    return misc_register(&vs_misc);
}

static void __exit vs_exit(void)
{
    misc_deregister(&vs_misc);
}

#endif /* CONFIG_FB */

module_init(vs_init);
module_exit(vs_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("screen capture bridge (fbdev channel; DRM channel planned)");
