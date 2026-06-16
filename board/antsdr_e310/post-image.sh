#!/bin/bash
set -e

BOARD_DIR="$(dirname $0)"

DTB="${BINARIES_DIR}/device.dtb"
KERNEL="${BINARIES_DIR}/zImage"
RAMDISK="${BINARIES_DIR}/rootfs.cpio.gz"

DEVICE_ITS="${BOARD_DIR}/device.its"

sed \
  -e "s|\${DTB}|$DTB|g" \
  -e "s|\${KERNEL}|$KERNEL|g" \
  -e "s|\${RAMDISK}|$RAMDISK|g" \
  "${DEVICE_ITS}" > "${BINARIES_DIR}/device.its"

mkimage -f "${BINARIES_DIR}/device.its" "${BINARIES_DIR}/image.itb"
