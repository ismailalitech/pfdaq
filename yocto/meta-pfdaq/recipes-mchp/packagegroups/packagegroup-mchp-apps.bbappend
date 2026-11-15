# AMP additions for the Discovery Kit (mirrors the vendor mpfs-icicle-kit-amp list, FreeRTOS only).
RDEPENDS:packagegroup-mchp-apps:append:mpfs-disco-kit-amp = "\
    kernel-module-rpmsg-char \
    kernel-module-rpmsg-client-sample \
    kernel-module-rpmsg-ctrl \
    kernel-module-rpmsg-tty \
    polarfire-soc-freertos-amp-demo \
    polarfire-soc-linux-examples-amp \
    pfdaq-tools \
    kernel-module-pfdaq-uio \
"
