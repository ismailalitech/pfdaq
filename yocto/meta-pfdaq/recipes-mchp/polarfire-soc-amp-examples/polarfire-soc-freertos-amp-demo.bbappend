# The U54_4 firmware source lives in the repository under firmware/u54_4-supervisor, where it can
# be read and built outside Yocto as well. This recipe fetches it from there, lets do_patch apply
# the small patch to the vendor's own files, then runs the same apply.sh a manual build uses.
FILESEXTRAPATHS:prepend := "${THISDIR}/../../../../firmware/u54_4-supervisor:"

SRC_URI:append:mpfs-disco-kit-amp = " file://amp-examples-v2025.07.patch \
                                      file://apply.sh \
                                      file://pfdaq_supervisor.c \
                                      file://disco-kit/Makefile.freertos \
                                      file://disco-kit/Makefile.bm \
                                      file://disco-kit/MPFS_DISCOVERY_KIT_MSS_mss_cfg.xml"

COMPATIBLE_MACHINE = "(mpfs-icicle-kit-amp|mpfs-disco-kit-amp)"

EXTRA_OEMAKE:append:mpfs-disco-kit-amp = " BOARD=disco-kit"

do_pfdaq_overlay() {
    :
}
do_pfdaq_overlay:mpfs-disco-kit-amp() {
    bash ${WORKDIR}/apply.sh ${S} --no-patch
}
addtask pfdaq_overlay after do_patch before do_configure
