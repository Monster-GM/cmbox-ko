# cmbox-ko — GKI android12-5.10 内核模块

目标设备: Redmi 2312DRA50C (kernel 5.10.236-android12-9-00003-gfb24cf99ad97-ab14313284)

- `ko/vts.c` → vts.ko: 虚拟触摸屏注入 (DOWN/MOVE/UP ioctl)
- `ko/vscreen.c` → vscreen.ko: 抓屏桥 (fbdev 通道; GKI 无 fb0 的设备为占位, DRM 通道 v2)

GitHub Actions: 推送即自动构建 (AOSP common android12-5.10 @ fb24cf99ad97 + LOCALVERSION 对齐)
Artifacts: vts.ko / vscreen.ko → 放到设备 `/data/local/tmp/ko/5.10.236-android12-9-00003-gfb24cf99ad97-ab14313284/`
