# F673AV9a OpenWrt 移植项目

目标：中兴 ZXHN F673AV9a（ZX279128S, 2× Cortex-A9, 512MB RAM, 256MB SPI NAND）
方案：保留原厂内核，OpenWrt 25.12.x 用户态写入 kernel1/rootfs1 槽位。

## 阶段进度

- [x] P0 u-boot 侦察（printenv / mtdparts / nand info / help / bdinfo）
- [x] P1 全分区备份（8 分区 100% 覆盖：uboot/others/parameter/wlan/usercfg/middleware/kernel1/kernel2，存于本地 U 盘）
- [x] P2 CI 编译配置（本仓库 GitHub Actions）
- [ ] P3 OpenWrt 25.12.6 用户态构建（进行中）
- [ ] P4 rootfs 重打包 + tftpboot 写入 kernel1/rootfs1 槽位
- [ ] P5 others 分区 BootImageNum 切槽启动
- [ ] P6 网络功能验证（LAN/DHCP/firewall）

## 硬件情报（串口实测）

| 项目 | 值 |
|---|---|
| SoC | ZTE ZX279128S（cpuid=27912810，双核 A9） |
| DRAM | 512 MiB @ 0x40000000 |
| Flash | SPI NAND 256 MiB（128KiB block / 2KiB page） |
| U-Boot | 2021.01，FIT 启动，可打断 |
| 工具链 | arm-sanechips-linux-gnueabi-gcc 5.3.1（soft-float ABI） |
| 分区 | kernel1@0x1a00000(备用) / kernel2@0x3a00000(当前有效) |
| 网络 | u-boot eth0 + GPON serdes 已初始化，tftpboot 可用（无 tftpput） |

## CI 编译

`.github/workflows/build.yml`：推送到 main（build.yml 或 config/ 变更）自动触发；
默认 target `mvebu/cortexa9`（armv7-a Cortex-A9 ABI 最接近）；
OpenWrt 标签 `v25.12.6`；成功后发 Release 提供 rootfs.tar.gz 直链下载。

## 安全守则

1. kernel2/rootfs2 是当前活系统，永不触碰
2. 启动时拔光纤（防 TR-069）
3. 引导试验先走内存路径，不落 flash
4. mtd dump 单次上限 8~16MB，32MB 分区按 8MB 分块
5. others 分区内容 = 启动槽选择器：BootImageNum=0x00000001,DownloadState=3
