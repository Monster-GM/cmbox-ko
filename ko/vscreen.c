// vscreen.c — 抓屏内核模块 (v1: fbdev 通道; DRM plane 通道骨架见文末)
// 用法: insmod vscreen.ko → 出现 /dev/vscreen
//   ioctl GET_INFO        → 分辨率/格式/帧大小
//   mmap                  → 映射"当前帧"缓冲到用户态 (零拷贝读)
//   ioctl CAPTURE         → 触发一次抓取 (用户态推流循环里调用)
//   ioctl WAIT_UPDATE     → 阻塞等下一帧事件 (fb notifier)
//
// v1 抓取通道: fbdev (fb_info->screen_buffer)
//   - 适用: 有 /dev/graphics/fb0 的机型 (多数 debug 用机/老机型)
//   - DRM-only 机型 (Pixel/新三星): 见文末 DRM 通道 TODO, 或先用
//     root 用户态直读方案过渡 (permissive 下 cat fb0 等价)
//
// 编译: 同 vts.c

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/fb.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/wait.h>
#include <linux/sched.h>

#define VS_NAME "vscreen"

static struct fb_info *vs_fb;
static int vs_fb_index = 0;         /* 用第几个 fb, 默认 0 */
module_param(fb_index, int, 0444);

static DECLARE_WAIT_QUEUE_HEAD(vs_wq);
static atomic_t frame_seq = ATOMIC_INIT(0);

/* ---- fb 事件: 抓 fb_info + 通知更新 ---- */
static int vs_fb_notifier(struct notifier_block *nb, unsigned long event, void *data)
{
    struct fb_event *e = data;
    struct fb_info *info = e->info;

    if (event == FB_EVENT_FB_REGISTERED && !vs_fb) {
        struct fb_info *target = registered_fb[vs_fb_index];
        if (target) {
            vs_fb = target;
            atomic_inc(&frame_seq);
            wake_up_interruptible(&vs_wq);
            pr_info("vscreen: attached fb%d %dx%d\n", vs_fb_index,
                    target->var.xres, target->var.yres);
        }
    }
    return NOTIFY_OK;
}

static struct notifier_block vs_nb = { .notifier_call = vs_fb_notifier };

/* ---- ioctl ---- */
#define VS_IOC_MAGIC 'S'
struct vs_info {
    __u32 width;
    __u32 height;
    __u32 stride;       /* 一行字节数 */
    __u32 bpp;          /* bits per pixel */
    __u32 frame_size;
    __u32 seq;          /* 当前帧序号 */
};
#define VS_GET_INFO  _IOR(VS_IOC_MAGIC, 1, struct vs_info)
#define VS_CAPTURE   _IO (VS_IOC_MAGIC, 2)
#define VS_WAIT      _IOW(VS_IOC_MAGIC, 3, __u32)   /* 参数=上次见过的seq */

static long vs_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
    struct vs_info i;

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
        /* fbdev 是"持续刷新"模型, 当前帧就在 screen_buffer 里,
           mmap 读到的永远是最新内容, 无需主动拷贝。
           这里仅推进序号让用户态知道有新内容。 */
        atomic_inc(&frame_seq);
        wake_up_interruptible(&vs_wq);
        return 0;

    case VS_WAIT: {
        __u32 last;
        if (copy_from_user(&last, (void __user *)arg, sizeof(last)))
            return -EFAULT;
        /* 简单节拍: fbdev 常驻刷新, 这里按 16ms 节拍唤醒推流循环 */
        wait_event_interruptible_timeout(vs_wq,
            atomic_read(&frame_seq) != last, msecs_to_jiffies(16));
        return atomic_read(&frame_seq);
    }
    }
    return -ENOTTY;
}

/* ---- mmap: 直接把 fb 的显存暴露给用户态 (只读语义由用户态自律) ---- */
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
    int ret;

    ret = fb_register_client(&vs_nb);
    if (ret) return ret;

    /* fb 可能早已注册好, 直接抓 */
    if (num_registered_fb > vs_fb_index)
        vs_fb = registered_fb[vs_fb_index];

    ret = misc_register(&vs_misc);
    if (ret) { fb_unregister_client(&vs_nb); return ret; }

    if (vs_fb)
        pr_info("vscreen: attached fb%d %dx%d bpp%d\n", vs_fb_index,
                vs_fb->var.xres, vs_fb->var.yres, vs_fb->var.bits_per_pixel);
    else
        pr_info("vscreen: waiting for fb registration...\n");
    return 0;
}

static void __exit vs_exit(void)
{
    misc_deregister(&vs_misc);
    fb_unregister_client(&vs_nb);
}

module_init(vs_init);
module_exit(vs_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("framebuffer capture bridge");

/* =====================================================================
 *  DRM plane 通道 (Pixel/新三星, fbdev 不存在时) — v2 TODO 骨架
 * =====================================================================
 *  1. kallsyms_lookup_name(kprobe 注册法恢复该符号) 拿到:
 *      drm_for_each_dev / drm_mode_config internals 所需符号
 *  2. 遍历 drm_device->mode_config.plane_list,
 *     找 plane->state->crtc != NULL 且 plane->type == DRM_PLANE_TYPE_PRIMARY
 *  3. fb = plane->state->fb;
 *     gem = fb->obj[0];                       // GEM 对象
 *     dma_buf 路径: gem->dma_buf → dma_buf_vmap(att) → 内核虚拟地址
 *  4. memcpy 到环形队列, 格式多为 NV12/BGRA, 用户态转 NV12 后喂硬编
 *  5. secure 层(受保护内容) vmap 出来是密文/黑块 — 放弃该层, 属预期
 *  6. 结构体偏移随内核版本漂移: 用 OCI 编译环境 + BTF/offset 配置表,
 *     学 DirtyFrag "per-KMI 预编译" 的做法
 * ===================================================================== */
