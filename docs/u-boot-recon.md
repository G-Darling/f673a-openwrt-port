# P0 u-boot 侦察结果（2026-11-24 实测）

## 硬件情报（实测确认）

| 项目 | 值 |
|---|---|
| SoC | ZTE ZX279128S（cpuid=27912810，双核 A9，1 核在线） |
| DRAM | 512 MiB @ 0x40000000（size 0x20000000） |
| Flash | SPI NAND 256 MiB（skyhigh 01-25-01，128KiB block / 2KiB page / OOB 128B） |
| U-Boot | 2021.01（2023-11-27 构建），FIT 启动，独立 fdt |
| 工具链 | arm-sanechips-linux-gnueabi-gcc 5.3.1（Buildroot 2017.05，**soft-float ABI**） |
| 固件版本 | versioninfo = V2.2.0P2N19（2023-08-22） |
| ethaddr | 00:29:47:00:00:32 |
| 网口 | eth0（u-boot 层可用，含 GPON serdes init + mdio） |

## MTD 分区表（实测 mtd list）

```
u-boot        0x00000000 - 0x00200000   2M
others        0x00200000 - 0x00600000   4M
parameter     0x00600000 - 0x00a00000   4M
wlan          0x00a00000 - 0x00e00000   4M
usercfg       0x00e00000 - 0x01600000   8M
middleware    0x01600000 - 0x01a00000   4M
kernel1/rootfs1  0x01a00000 - 0x03a00000  32M  ← 同偏移复用（kernel+rootfs 合一）
kernel2/rootfs2  0x03a00000 - 0x05a00000  32M  ← 当前有效
osgi1         0x05a00000 - 0x08c00000  50M
osgi2         0x08c00000 - 0x0be00000  50M
plugin_data   0x0be00000 - 0x0ff00000  65M
```

## 关键环境变量（baseline 存档见 u-boot-env-baseline.md）

```
bootargs=console=ttyAMA0,115200n8 earlyprintk=pl011,0x94404000 vid=53
         pcie_pme=nomsi root=/dev/mtdblock13 ro rootfstype=jffs2 mem=512M
bootcmd=zxboot
bootdelay=2
ipaddr=192.168.1.1   serverip=192.168.1.100   ← TFTP 已预设地址
kernel_addr_r=0x00080000  fdt_addr_r=0x22000000  fdtfile=qserver.dtb
```

## 可用命令（决定备份路线）

- ✅ `tftpboot` / `ping` / `bootp` —— 网络下行可用
- ❌ `tftpput` / `dhcp` —— **无上行通道**
- ✅ `mtd`（list/read/erase/write）、`nand`、`sf`、`fatload/fatwrite`（U 盘通道！）
- ✅ `loady`（ymodem 串口接收）、`bootm`、`fdt`、`setenv/saveenv`

## P1 备份方案（定稿）

u-boot 无法向 PC 推数据（无 tftpput/nc），**走 Linux shell 路线**：

1. PC 网口设 `192.168.1.100`，网线直连光猫 LAN
2. u-boot `ping 192.168.1.100` 验证链路
3. `setenv bootargs '...原值... init=/bin/sh'` → `run zxboot` 或 `bootm`（**不写 flash，安全**）
4. Linux shell 内：`cat /proc/mtd` / `/proc/version` / `ls /lib/modules`
5. 备份走网络（`tftp` 客户端或 `dd|nc`），全分区 dump 到 PC
6. 若 Linux 网卡驱动未自动加载 → 手动 `insmod` 后再试；兜底用串口 dump 关键小分区（u-boot 2M / parameter 4M / wlan 4M）

### 备用通道：FAT U 盘

`fatwrite` 可用 → 若 U 盘插入可被识别（`usb start`，help 无 usb 命令，存疑），
可将分区 dump 到内存后 `fatwrite` 写入 U 盘。优先级低于网络路线。

## 红线

- 备份完成前**不写 flash 任何分区**
- 全程拔光纤（防 TR-069）
- kernel2/rootfs2 是当前活系统，永不触碰；kernel1/rootfs1 同偏移复用，实验要格外小心
