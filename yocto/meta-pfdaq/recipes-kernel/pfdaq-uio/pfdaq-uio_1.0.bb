SUMMARY = "pfdaq UIO driver with in-kernel acknowledge of the fabric latency unit"
LICENSE = "GPL-2.0-only"
LIC_FILES_CHKSUM = "file://pfdaq_uio.c;beginline=1;endline=1;md5=50d2ba0afecd20f74c12a4bdbcfcfe61"

inherit module

SRC_URI = "file://Makefile file://pfdaq_uio.c"
S = "${WORKDIR}"

RPROVIDES:${PN} += "kernel-module-pfdaq-uio"
