SUMMARY = "pfdaq board-side tools: stream capture/verification and latency measurement"
DESCRIPTION = "Linux consumer for the pfdaq fabric stream source on the PolarFire SoC Discovery Kit."
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://pfdaq_cap.c;beginline=1;endline=1;md5=6ec41034e04432ee375d0e14fba596f4"

SRC_URI = "file://pfdaq_cap.c file://pfdaq_lat.c"
S = "${WORKDIR}"

do_compile() {
    ${CC} ${CFLAGS} ${LDFLAGS} -Wall -Wextra -O2 -o pfdaq_cap ${WORKDIR}/pfdaq_cap.c
    ${CC} ${CFLAGS} ${LDFLAGS} -Wall -Wextra -O2 -o pfdaq_lat ${WORKDIR}/pfdaq_lat.c
}

do_install() {
    install -d ${D}${bindir}
    install -m 0755 pfdaq_cap ${D}${bindir}/
    install -m 0755 pfdaq_lat ${D}${bindir}/
}
