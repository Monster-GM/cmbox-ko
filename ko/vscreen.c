// vscreen.c — 抓屏内核模块 v0.3
//
// 双后端:
//   A. CONFIG_FB=y  (传统 fbdev 内核): fb notifier + 显存 mmap (v0.2 路线)
//   B. GKI (CONFIG_FB=n, CONFIG_DRM=y): kallsyms 找到 drm_minor_acquire,
//      获取 card0 的 drm_device → 遍历 plane → 找活动 fb →
//      dmabuf vmap / gem funcs->vmap → 拷贝到共享缓冲
//      (ko 与 GKI 内核同源编译, 结构体布局精确一致, 无需手工偏移表)
//
// 接口不变:
//   /dev/vscreen
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
#include <linux/kprobes.h>
#include <linux/version.h>

#ifdef CONFIG_FB
#include <linux/fb.h>
#endif

#if IS_ENABLED(CONFIG_DRM) && !IS_ENABLED(CONFIG_FB)
#define VSCREEN_USE_DRM 1
#include <drm/drm_device.h>
#include <drm/drm_mode_object.h>
#include <drm/drm_plane.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem.h>
#include <drm/drm_prime.h>
#include <drm/drm_file.h>
#include <linux/dma-buf.h>
#include <linux/dma-buf-map.h>
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
#define VS_GET_INFO  _IOR(VS_IOC_MAGIC, 1, struct vs_info)
#define VS_CAPTURE   _IO (VS_IOC_MAGIC, 2)
#define VS_WAIT      _IOW(VS_IOC_MAGIC, 3, __u32)

static DECLARE_WAIT_QUEUE_HEAD(vs_wq);
static atomic_t frame_seq = ATOMIC_INIT(0);

/* ---- 共享输出帧缓冲 (用户态 mmap 目标) ---- */
static void    *vs_buf;
static size_t   vs_buf_size;
static u32      vs_width, vs_height, vs_stride, vs_fourcc_or_bpp;

static int vs_ensure_buf(size_t need)
{
    if (vs_buf && vs_buf_size >= need) return 0;
    if (vs_buf) vfree(vs_buf);
    vs_buf_size = PAGE_ALIGN(need);
    vs_buf = vzalloc(vs_buf_size);
    if (!vs_buf) { vs_buf_size = 0; return -ENOMEM; }
    return 0;
}

#ifdef CONFIG_FB
/* ============ 后端 A: fbdev ============ */
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

#else
/* ============ 后端 B: GKI DRM ============ */
#define VSCREEN_USE_DRM 1

typedef unsigned long (*kallsyms_lookup_name_t)(const char *name);
static kallsyms_lookup_name_t vs_kln;

struct drm_minor;                       /* 由内核树头文件提供真实定义 */

static int vs_resolve_symbols(void)
{
    struct kprobe kp = { .symbol_name = "kallsyms_lookup_name" };
    int ret = register_kprobe(&kp);
    if (ret < 0) {
        pr_err("vscreen: kprobe for kallsyms_lookup_name failed: %d\n", ret);
        return ret;
    }
    vs_kln = (kallsyms_lookup_name_t)kp.addr;
    unregister_kprobe(&kp);
    if (!vs_kln) return -ENOENT;

    /* 校验关键符号可寻址 */
    if (!vs_kln("drm_minor_acquire") || !vs_kln("drm_minor_release")) {
        pr_err("vscreen: drm_minor symbols not found\n");
        return -ENOENT;
    }
    return 0;
}

#endif /* CONFIG_FB */

/* ================================================================
 * 以下为设备接口层 (两种后端共用接口, 内部分发)
 * ================================================================ */
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

#ifdef CONFIG_FB
/* ---- fbdev 后端实现 ---- */

static int vs_backend_capture(struct vs_info *i)
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

static int __init vs_backend_init(void)
{
    int ret = fb_register_client(&vs_nb);
    if (ret) return ret;
    return 0;
}

static void vs_backend_exit(void)
{
    fb_unregister_client(&vs_nb);
}

#else
/* ---- GKI DRM 后端实现 ---- */

typedef struct drm_minor *(*vs_minor_acq_t)(unsigned int);
static vs_minor_acq_t  p_drm_minor_acquire;
static void          (*p_drm_minor_release)(struct drm_minor *);
static int  (*p_dma_buf_vmap)(struct dma_buf *dmabuf, struct dma_buf_map *map);
static void (*p_dma_buf_vunmap)(struct dma_buf *dmabuf, struct dma_buf_map *map);

static DEFINE_MUTEX(vs_lock);
static struct drm_device *vs_dev;

static int vs_drm_symbols(void)
{
    int ret = vs_resolve_symbols();
    if (ret) return ret;

    p_drm_minor_acquire = (vs_minor_acq_t)vs_kln("drm_minor_acquire");
    if (!p_drm_minor_acquire)
        p_drm_minor_acquire = (vs_minor_acq_t)vs_kln("__drm_minor_acquire");
    p_drm_minor_release = (void (*)(struct drm_minor *))vs_kln("drm_minor_release");
    p_dma_buf_vmap    = (int (*)(struct dma_buf *, struct dma_buf_map *))vs_kln("dma_buf_vmap");
    p_dma_buf_vunmap  = (void (*)(struct dma_buf *, struct dma_buf_map *))vs_kln("dma_buf_vunmap");

    if (!p_drm_minor_acquire || !p_drm_minor_release) return -ENOENT;
    pr_info("vscreen: drm_minor_acquire=%px dma_buf_vmap=%px\n",
            p_drm_minor_acquire, p_dma_buf_vmap);
    return 0;
}

/* card0 的 minor 号固定为 64 (DRM_MAJOR=226, minor 64..) */
#define VSCREEN_CARD_MINOR 64

static int vs_drm_capture(struct vs_info *i)
{
    struct drm_minor *minor;
    struct drm_device *dev;
    struct drm_plane *plane;
    struct dma_buf_map vs_map = { 0 };
    int ret = -EAGAIN;

    if (!vs_dev) {
        if (!p_drm_minor_acquire) return -ENOENT;
        minor = p_drm_minor_acquire(VSCREEN_CARD_MINOR);
        if (IS_ERR(minor)) return PTR_ERR(minor);
        vs_dev = minor->dev;
        p_drm_minor_release(minor);
        pr_info("vscreen: drm_device attached %s\n", dev->unique);
    }
    dev = vs_dev;

    mutex_lock(&dev->mode_config.mutex);
    drm_for_each_plane(plane, dev) {
        struct drm_framebuffer *fb;
        struct drm_gem_object *obj;
        void *vaddr = NULL;

        fb = plane->state ? plane->state->fb : plane->fb;
        if (!fb || !fb->obj[0]) continue;

        obj = fb->obj[0];
        if (obj->import_attach && p_dma_buf_vmap) {
            memset(&vs_map, 0, sizeof(vs_map));
            if (p_dma_buf_vmap(obj->import_attach->dmabuf, &vs_map) == 0)
                vaddr = vs_map.vaddr;
        }
        else if (obj->funcs && obj->funcs->vmap)
            vaddr = obj->funcs->vmap(obj);

        if (!vaddr) continue;   /* secure 层取不到, 跳过 */

        /* 命中第一个可读的活动 fb */
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
            p_dma_buf_vunmap(obj->import_attach->dmabuf, &vs_map);
        break;                  /* 只要 primary 的第一帧 */
    }
    mutex_unlock(&dev->mode_config.mutex);
    return ret;
}
#endif /* CONFIG_FB */

/* ---- 统一 ioctl / mmap ---- */
static long vs_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
    struct vs_info i;
    __u32 last;

    switch (cmd) {
    case VS_GET_INFO:
#ifdef CONFIG_FB
        if (vs_backend_capture(&i)) return -ENODEV;
#else
        if (vs_drm_capture(&i)) return -EAGAIN;
#endif
        if (copy_to_user((void __user *)arg, &i, sizeof(i)))
            return -EFAULT;
        return 0;

    case VS_CAPTURE:
#ifdef CONFIG_FB
        return vs_backend_capture(&i);
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
    int ret = misc_register(&vs_misc);
    if (ret) return ret;

#ifndef CONFIG_FB
    ret = vs_drm_symbols();
    if (ret) {
        misc_deregister(&vs_misc);
        return ret;
    }
#else
    vs_backend_init();
#endif
    pr_info("vscreen: v0.3 ready\n");
    return 0;
}

static void __exit vs_exit(void)
{
#ifndef CONFIG_FB
    /* vs_dev 由 drm_minor_release 已释放, 无需额外清理 */
#endif
#ifdef CONFIG_FB
    vs_backend_exit();
#endif
    misc_deregister(&vs_misc);
    if (vs_buf) vfree(vs_buf);
}

module_init(vs_init);
module_exit(vs_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("screen capture bridge v0.3 (fbdev / GKI DRM dual backend)");
