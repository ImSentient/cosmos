HOMEPAGE = "https://kalico.gg/"
LICENSE = "GPL-3.0-only"
LIC_FILES_CHKSUM = "file://COPYING;md5=1ebbd3e34237af26da5dc08a4e440464"

FILESEXTRAPATHS:prepend := "${THISDIR}/files:"

SRC_URI = "git:///home/paul/carbon/kalico-hx711s-new3;protocol=file;branch=paul/hx711s-new3 \
    file://0001-remove-save-config-subfile-check.patch \
    file://0002-reduce-calibration-difference-tolerance.patch \
    file://0001-Reduce-log-rotate-threshold.patch \
    file://0001-Don-t-write-tmp-files-to-ramdisk.patch \
    file://0001-implement-pulldown-resistor-in-thermistor-calc.patch \
    file://0001-probe-home-z-includes-z-offset.patch \
    file://0001-force-specific-input-shaper.patch \
    file://0004-Add-MESH_SOAK-beta-tooling.patch \
"
SRCREV = "b5966b6eef907755d6f5234aa004c5f9d96edfcd"

PR = "r22"

S = "${WORKDIR}/git"

DEPENDS = " \
    python3-native \
"
