// vscreen.c — 抓屏内核模块 v0.4
//
// 双后端:
//   A. CONFIG_FB=y  (传统 fbdev 内核): fb notifier + 显存拷贝
//   B. GKI (CONFIG_FB=n, CONFIG_DRM=y): DRM plane 直读
//      - 符号地址由 root 用户态从 /proc/kallsyms 解析后经 VS_SET_SYMS 喂入
//        (kallsyms_lookup_name 在 GKI 上常被 kprobe 加固拦截, 此为唯一稳路)
//      - ko 与 GKI 内核同源编译, drm/dma_buf 结构体布局精确一致
//
// 接口:
//   /dev/vscreen
//   ioctl VS_SET_SYMS (root, GKI 后端初始化)
//   ioctl VS_GET_INFO / VS_CAPTURE / VS_WAIT + mmap

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/wait.h>
#include <linux/sched.h>
#include <linux/debugfs.h>

#ifdef CONFIG_FB
#include <linux/fb.h>
#else
#include <drm/drm_device.h>
#include <drm/drm_mode_object.h>
#include <drm/drm_plane.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem.h>
#include <drm/drm_prime.h>
#include <drm/drm_file.h>
#include <linux/dma-buf.h>
#include <drm/drm_fourcc.h>
#endif

#define VS_NAME "vscreen"

#define VS_IOC_MAGIC 'S'
struct vs_info {
    __u32 width;
    __u32 height;
    __u32 stride;
    __u32 fourcc;       /* DRM fourcc (DRM 后端) 或 bpp (fb 后端) */
    __u32 frame_size;
    __u32 seq;
};
struct vs_syms {
    __u64 drm_minor_acquire;
    __u64 drm_minor_release;
    __u64 dma_buf_vmap;
    __u64 dma_buf_vunmap;
};
#define VS_GET_INFO  _IOR(VS_IOC_MAGIC, 1, struct vs_info)
#define VS_CAPTURE   _IO (VS_IOC_MAGIC, 2)
#define VS_WAIT      _IOW(VS_IOC_MAGIC, 3, __u32)
#define VS_SET_SYMS  _IOW(VS_IOC_MAGIC, 4, struct vs_syms)

static DECLARE_WAIT_QUEUE_HEAD(vs_wq);
static atomic_t frame_seq = ATOMIC_INIT(0);

/* ---- 输出帧缓冲 (用户态 mmap 目标) ---- */
static void    *vs_buf;
static size_t   vs_buf_size;

static int vs_ensure_buf(size_t need)
{
    if (vs_buf && vs_buf_size >= need) return 0;
    if (vs_buf) vfree(vs_buf);
    vs_buf_size = PAGE_ALIGN(need);
    vs_buf = vzalloc(vs_buf_size);
    if (!vs_buf) { vs_buf_size = 0; return -ENOMEM; }
    return 0;
}

static long vs_ioctl(struct file *f, unsigned int cmd, unsigned long arg);
static int  vs_mmap(struct file *f, struct vm_area_struct *vma);

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

/* debugfs 兜底通道: /sys/kernel/debug/vscreen (root 可访问, 不依赖 devtmpfs) */
static struct dentry *vs_dbg;

#ifdef CONFIG_FB
/* ================= 后端 A: fbdev ================= */

static struct fb_info *vs_fb;
static int vs_fb_index = 0;
module_param(vs_fb_index, int, 0444);

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

static int vs_fb_capture(struct vs_info *i)
{
    if (!vs_fb) return -ENODEV;
    i->width  = vs_fb->var.xres;
    i->height = vs_fb->var.yres;
    i->stride = vs_fb->fix.line_length;
    i->fourcc = vs_fb->var.bits_per_pixel;
    i->frame_size = vs_fb->fix.line_length * vs_fb->var.yres;
    if (vs_ensure_buf(i->frame_size)) return -ENOMEM;
    memcpy(vs_buf, vs_fb->screen_buffer, i->frame_size);
    atomic_inc(&frame_seq);
    wake_up_interruptible(&vs_wq);
    return 0;
}

#else
/* ================= 后端 B: GKI DRM ================= */

typedef struct drm_minor *(*vs_minor_acq_t)(unsigned int);
static vs_minor_acq_t  p_drm_minor_acquire;
static void          (*p_drm_minor_release)(struct drm_minor *);
static void *(*p_dma_buf_vmap)(struct dma_buf *dmabuf);
static void (*p_dma_buf_vunmap)(struct dma_buf *dmabuf, void *vaddr);
static int   vs_syms_ready;

static DEFINE_MUTEX(vs_lock);
static struct drm_device *vs_dev;

#define VSCREEN_CARD_MINOR 64          /* card0: DRM_MAJOR 226, minor 64 */

static int vs_set_syms(void __user *arg)
{
    struct vs_syms sy;
    if (copy_from_user(&sy, arg, sizeof(sy)))
        return -EFAULT;
    p_drm_minor_acquire = (vs_minor_acq_t)(unsigned long)sy.drm_minor_acquire;
    p_drm_minor_release = (void (*)(struct drm_minor *))(unsigned long)sy.drm_minor_release;
    p_dma_buf_vmap      = (void *(*)(struct dma_buf *))(unsigned long)sy.dma_buf_vmap;
    p_dma_buf_vunmap    = (void (*)(struct dma_buf *, void *))(unsigned long)sy.dma_buf_vunmap;
    if (!p_drm_minor_acquire || !p_drm_minor_release)
        return -EINVAL;
    vs_syms_ready = 1;
    pr_info("vscreen: syms fed acq=%px rel=%px vmap=%px\n",
            p_drm_minor_acquire, p_drm_minor_release, p_dma_buf_vmap);
    return 0;
}

static int vs_drm_capture(struct vs_info *i)
{
    struct drm_minor *minor;
    struct drm_device *dev;
    struct drm_plane *plane;
    int ret = -EAGAIN;

    if (!vs_syms_ready) return -EAGAIN;   /* 等用户态喂符号 */

    mutex_lock(&vs_lock);
    if (!vs_dev) {
        minor = p_drm_minor_acquire(VSCREEN_CARD_MINOR);
        if (IS_ERR(minor)) {
            mutex_unlock(&vs_lock);
            return PTR_ERR(minor);
        }
        vs_dev = minor->dev;
        p_drm_minor_release(minor);
        pr_info("vscreen: drm_device attached\n");
    }
    dev = vs_dev;
    mutex_unlock(&vs_lock);

    mutex_lock(&dev->mode_config.mutex);
    drm_for_each_plane(plane, dev) {
        struct drm_framebuffer *fb;
        struct drm_gem_object *obj;
        void *vaddr = NULL;

        fb = plane->state ? plane->state->fb : plane->fb;
        if (!fb || !fb->obj[0]) continue;
        obj = fb->obj[0];

        if (obj->import_attach && p_dma_buf_vmap)
            vaddr = p_dma_buf_vmap(obj->import_attach->dmabuf);
        else if (obj->funcs && obj->funcs->vmap)
            vaddr = obj->funcs->vmap(obj);
        if (!vaddr) continue;            /* secure 层取不到, 跳过 */

        i->width  = fb->width;
        i->height = fb->height;
        i->stride = fb->pitches[0];
        i->fourcc = fb->format->format;
        i->frame_size = fb->pitches[0] * fb->height;

        if (vs_ensure_buf(i->frame_size) == 0) {
            memcpy(vs_buf, vaddr, i->frame_size);
            atomic_inc(&frame_seq);
            wake_up_interruptible(&vs_wq);
            ret = 0;
        }
        if (obj->import_attach && p_dma_buf_vunmap)
            p_dma_buf_vunmap(obj->import_attach->dmabuf, vaddr);
        break;                            /* 第一个可读的活动 fb */
    }
    mutex_unlock(&dev->mode_config.mutex);
    return ret;
}

#endif /* CONFIG_FB */

/* ================= 统一设备接口 ================= */

static long vs_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
    struct vs_info i;
    __u32 last;

    switch (cmd) {
#ifndef CONFIG_FB
    case VS_SET_SYMS:
        return vs_set_syms((void __user *)arg);
#endif
    case VS_GET_INFO:
#ifdef CONFIG_FB
        if (vs_fb_capture(&i)) return -ENODEV;
#else
        if (vs_drm_capture(&i)) return -EAGAIN;
#endif
        if (copy_to_user((void __user *)arg, &i, sizeof(i)))
            return -EFAULT;
        return 0;

    case VS_CAPTURE:
#ifdef CONFIG_FB
        return vs_fb_capture(&i);
#else
        {
            struct vs_info tmp;
            return vs_drm_capture(&tmp);
        }
#endif
    case VS_WAIT:
        if (copy_from_user(&last, (void __user *)arg, sizeof(last)))
            return -EFAULT;
        wait_event_interruptible_timeout(vs_wq,
            atomic_read(&frame_seq) != last, msecs_to_jiffies(33));
        return atomic_read(&frame_seq);
    }
    return -ENOTTY;
}

static int vs_mmap(struct file *f, struct vm_area_struct *vma)
{
    if (!vs_buf) return -ENODEV;
    return remap_vmalloc_range(vma, vs_buf, 0);
}

static int __init vs_init(void)
{
    int ret;

    /* debugfs 兜底: 即使 misc 注册在目标固件上不可见也可用 */
    vs_dbg = debugfs_create_file(VS_NAME, 0666, NULL, NULL, &vs_fops);

    ret = misc_register(&vs_misc);
    if (ret) {
        if (IS_ERR_OR_NULL(vs_dbg))
            return ret;          /* 两个都没成 */
        pr_info("vscreen: misc failed (%d), debugfs only\n", ret);
        return 0;
    }
#ifdef CONFIG_FB
    ret = fb_register_client(&vs_nb);
    if (ret) { misc_deregister(&vs_misc); return ret; }
#else
    /* DRM: 符号地址由 root 经 VS_SET_SYMS 喂入 */
#endif
    pr_info("vscreen: v0.4 ready\n");
    return 0;
}

static void __exit vs_exit(void)
{
#ifdef CONFIG_FB
    fb_unregister_client(&vs_nb);
#endif
    if (!IS_ERR_OR_NULL(vs_dbg)) debugfs_remove(vs_dbg);
    misc_deregister(&vs_misc);
    if (vs_buf) vfree(vs_buf);
}

module_init(vs_init);
module_exit(vs_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("screen capture bridge v0.4 (fbdev / GKI DRM, usermode-fed syms)");
