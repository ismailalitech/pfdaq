FILESEXTRAPATHS:prepend:mpfs-disco-kit-amp := "${THISDIR}/files:"

SRC_URI:append:mpfs-disco-kit-amp = " file://mpfs-disco-kit-amp.dts"

# Add the project device tree to the kernel tree before configuration.
do_configure:prepend:mpfs-disco-kit-amp() {
    dtsdir="${S}/arch/riscv/boot/dts/microchip"
    install -m 0644 ${WORKDIR}/mpfs-disco-kit-amp.dts ${dtsdir}/
    grep -q 'mpfs-disco-kit-amp.dtb' ${dtsdir}/Makefile || \
        echo 'dtb-$(CONFIG_ARCH_MICROCHIP_POLARFIRE) += mpfs-disco-kit-amp.dtb' >> ${dtsdir}/Makefile
}
