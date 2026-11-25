# u-boot 环境变量基线（2026-11-24 14:10 实测 printenv）

> ⚠️ 只读存档。任何 setenv 之前，此文件就是回滚基准。
> 恢复方法：u-boot 下逐条 `setenv <key> <value>`，或 `env default -a` 后重设关键项。

```
baudrate=115200
bootargs=console=ttyAMA0,115200n8 earlyprintk=pl011,0x94404000 vid=53 pcie_pme=nomsi root=/dev/mtdblock13 ro rootfstype=jffs2 mem=512M; 
bootcmd=zxboot
bootdelay=2
ethact=eth0
ethaddr=00:29:47:00:00:32
fdt_addr_r=0x22000000
fdt_high=0xffffffffffffffff
fdtcontroladdr=5f710cf0
fdtfile=qserver.dtb
initrd_high=0xffffffffffffffff
ipaddr=192.168.1.1
kernel_addr_r=0x00080000
kernel_name=Image
mtdids=nand0=spi-nand0
mtdparts=mtdparts=spi-nand0:256m@0x0(whole flash),2m@0x0(u-boot),4m@0x200000(others),4m@0x600000(parameter tags),4m@0xa00000(wlan),8m@0xe00000(usercfg),4m@0x1600000(middleware),32m@0x1a00000(kernel1),32m@0x3a00000(kernel2),50m@0x5a00000(osgi1),50m@0x8c00000(osgi2),65m@0xbe00000(plugin_data),32m@0x1a00000(rootfs1),32m@0x3a00000(rootfs2),
serverip=192.168.1.100
stderr=serial
stdin=serial
stdout=serial
update=sf probe; sf update 0x48000000 0x0 0x80000
versioninfo=U-Boot V2.2.0P2N19 20230822023656 0x3a00000 0x01 0x83 0x80 0x3c0000

Environment size: 962/126972 bytes
```

注意事项：
- `bootargs` 末尾有一个分号，恢复时保留原样
- `kernel_addr_r=0x00080000` 偏低（近向量表），zxboot 内部可能重新指定；如自 bootm 需用 0x42000000 一类安全地址
- `versioninfo` 第三字段 0x3a00000 = 当前活动 kernel/rootfs 偏移（kernel2/rootfs2）
