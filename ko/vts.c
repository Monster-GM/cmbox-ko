// vts.c — 虚拟触摸屏注入内核模块
// 用法: insmod vts.ko  →  出现 /dev/vts (misc 设备)
// 用户态 ioctl 下发触摸动作, ko 转成 input 事件 → Android InputReader 当真实触摸屏处理
// 编译: 需要 target 内核头/defconfig, Android.bp 或 make -C /lib/modules/$(uname -r)/build M=$PWD

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/input.h>
#include <linux/input/mt.h>
#include <linux/slab.h>

#define VTS_NAME "vts"

/* 与目标面板一致 — 部署时按 getevent 输出改 */
static int panel_w = 1080;
static int panel_h = 2400;
module_param(panel_w, int, 0444);
module_param(panel_h, int, 0444);

static struct input_dev *vts_dev;

/* ---- 事件原语 ---- */
static void ts_sync(void)  { input_sync(vts_dev); }

static void ts_down(int slot, int x, int y)
{
    input_mt_slot(vts_dev, slot);
    input_mt_report_slot_state(vts_dev, MT_TOOL_FINGER, true);
    input_report_abs(vts_dev, ABS_MT_POSITION_X, x);
    input_report_abs(vts_dev, ABS_MT_POSITION_Y, y);
    input_report_abs(vts_dev, ABS_MT_TOUCH_MAJOR, 10);
    input_report_abs(vts_dev, ABS_MT_PRESSURE, 50);
    input_report_key(vts_dev, BTN_TOUCH, 1);
    ts_sync();
}

static void ts_move(int slot, int x, int y)
{
    input_mt_slot(vts_dev, slot);
    input_report_abs(vts_dev, ABS_MT_POSITION_X, x);
    input_report_abs(vts_dev, ABS_MT_POSITION_Y, y);
    ts_sync();
}

static void ts_up(int slot)
{
    input_mt_slot(vts_dev, slot);
    input_mt_report_slot_state(vts_dev, MT_TOOL_FINGER, false);
    input_report_key(vts_dev, BTN_TOUCH, 0);
    ts_sync();
}

/* ---- 用户态协议 ---- */
#define VTS_IOC_MAGIC   'V'
struct vts_evt {
    __s32 action;   /* 0=DOWN 1=MOVE 2=UP 3=CANCEL */
    __s32 slot;
    __s32 x;
    __s32 y;
};
#define VTS_INJECT      _IOWR(VTS_IOC_MAGIC, 1, struct vts_evt)
#define VTS_GET_PANEL   _IOR(VTS_IOC_MAGIC,  2, struct vts_dim)
struct vts_dim { __s32 w; __s32 h; };

static long vts_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
    struct vts_evt e;
    struct vts_dim d;

    switch (cmd) {
    case VTS_INJECT:
        if (copy_from_user(&e, (void __user *)arg, sizeof(e)))
            return -EFAULT;
        /* 坐标钳制, 防御性 */
        e.x = clamp(e.x, 0, panel_w - 1);
        e.y = clamp(e.y, 0, panel_h - 1);
        switch (e.action) {
        case 0: ts_down(e.slot, e.x, e.y); break;
        case 1: ts_move(e.slot, e.x, e.y); break;
        case 2: ts_up(e.slot);             break;
        case 3: input_mt_slot(vts_dev, e.slot);
                input_mt_report_slot_state(vts_dev, MT_TOOL_FINGER, false);
                input_report_key(vts_dev, BTN_TOUCH, 0);
                ts_sync();                 break;
        default: return -EINVAL;
        }
        return 0;
    case VTS_GET_PANEL:
        d.w = panel_w; d.h = panel_h;
        if (copy_to_user((void __user *)arg, &d, sizeof(d)))
            return -EFAULT;
        return 0;
    }
    return -ENOTTY;
}

static const struct file_operations vts_fops = {
    .owner          = THIS_MODULE,
    .unlocked_ioctl = vts_ioctl,
    .compat_ioctl   = vts_ioctl,
};

static struct miscdevice vts_misc = {
    .minor = MISC_DYNAMIC_MINOR,
    .name  = VTS_NAME,
    .fops  = &vts_fops,
    .mode  = 0666,          /* SELinux permissive + uid1000 场景 */
};

static int __init vts_init(void)
{
    int ret;

    vts_dev = input_allocate_device();
    if (!vts_dev) return -ENOMEM;

    vts_dev->name  = "vmt-touchscreen";
    vts_dev->id.bustype = BUS_VIRTUAL;   /* 若固件按 source 白名单校验, 改 BUS_I2C 伪装 */

    set_bit(EV_ABS, vts_dev->evbit);
    set_bit(EV_KEY, vts_dev->keybit);
    set_bit(BTN_TOUCH, vts_dev->keybit);
    input_set_abs_params(vts_dev, ABS_MT_POSITION_X, 0, panel_w - 1, 0, 0);
    input_set_abs_params(vts_dev, ABS_MT_POSITION_Y, 0, panel_h - 1, 0, 0);
    input_set_abs_params(vts_dev, ABS_MT_TRACKING_ID, 0, 9, 0, 0);
    input_set_abs_params(vts_dev, ABS_MT_TOUCH_MAJOR, 0, 255, 0, 0);
    input_set_abs_params(vts_dev, ABS_MT_PRESSURE, 0, 255, 0, 0);

    ret = input_mt_init_slots(vts_dev, 10, INPUT_MT_DIRECT);
    if (ret) { input_free_device(vts_dev); return ret; }

    ret = input_register_device(vts_dev);
    if (ret) { input_free_device(vts_dev); return ret; }

    ret = misc_register(&vts_misc);
    if (ret) { input_unregister_device(vts_dev); return ret; }

    pr_info("vts: ready /dev/%s panel=%dx%d\n", VTS_NAME, panel_w, panel_h);
    return 0;
}

static void __exit vts_exit(void)
{
    misc_deregister(&vts_misc);
    /* 归还所有 slot, 防止手指"卡在屏幕上" */
    input_mt_slot(vts_dev, 0);
    input_mt_report_slot_state(vts_dev, MT_TOOL_FINGER, false);
    input_report_key(vts_dev, BTN_TOUCH, 0);
    input_sync(vts_dev);
    input_unregister_device(vts_dev);
}

module_init(vts_init);
module_exit(vts_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("virtual touchscreen injector");
