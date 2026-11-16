#!/bin/sh
# Read-only: what the board offers for the MSS DMA (PDMA). Run on the board; it changes nothing.
echo "## kernel"; uname -a
echo "## uptime"; uptime
echo "## devicetree: PDMA node"
d=/proc/device-tree/soc/dma-controller@3000000
echo "compatible: $(tr '\0' ' ' < $d/compatible)"
echo "dma-channels: $(od -An -tu1 $d/dma-channels | tr -s ' ')  (big-endian bytes)"
echo "reg (bytes, big-endian): $(od -An -tx1 $d/reg | tr -s ' ')"
echo "interrupts (bytes, big-endian): $(od -An -tx1 $d/interrupts | tr -s ' ')"
echo "## devicetree: dma-proxy node"
echo "compatible: $(tr '\0' ' ' < /proc/device-tree/mpfs-dma-proxy/compatible)"
echo "dma-names: $(tr '\0' ' ' < /proc/device-tree/mpfs-dma-proxy/dma-names)"
echo "## drivers bound"
ls /sys/bus/platform/drivers/sf-pdma /sys/bus/platform/drivers/mpfs_dma_proxy
echo "## /sys/class/dma"
for c in /sys/class/dma/*; do echo "$(basename $c) in_use=$(cat $c/in_use) memcpy_count=$(cat $c/memcpy_count) bytes_transferred=$(cat $c/bytes_transferred)"; done
echo "## device nodes"
ls -l /dev/dma-proxy* /dev/udmabuf*
echo "## kernel config (DMA engine, PDMA, proxy, dmatest)"
zcat /proc/config.gz | grep -E 'CONFIG_(DMADEVICES|DMA_ENGINE|SF_PDMA|MPFS_DMA_PROXY|DMATEST|UIO_MICROCHIP_DMA|HZ|NO_HZ_IDLE|MPFS_DMA_NONCOHERENT|RISCV_DMA_NONCOHERENT|UDMABUF)[= ]|DMATEST'
echo "## modules loaded"; lsmod
echo "## /proc/iomem (PDMA)"; grep -i -E '3000000|dma' /proc/iomem
echo "## interrupts with dma in the name"; grep -i dma /proc/interrupts
echo "## u-dma-buf regions"
for r in udmabuf-ddr-c0 udmabuf-ddr-nc0 udmabuf-ddr-nc-wcb0; do echo "$r phys=$(cat /sys/class/u-dma-buf/$r/phys_addr) size=$(cat /sys/class/u-dma-buf/$r/size)"; done
echo "## cpus online"; cat /sys/devices/system/cpu/online
echo "## remoteproc"; cat /sys/class/remoteproc/remoteproc0/state
