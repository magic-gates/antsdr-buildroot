#!/bin/bash
set -e

BOARD_DIR="$(dirname $0)"

DTB="${BINARIES_DIR}/device.dtb"
KERNEL="${BINARIES_DIR}/zImage"
RAMDISK="${BINARIES_DIR}/rootfs.cpio.gz"

IMAGE_ITS="${BOARD_DIR}/image.its"

sed \
  -e "s|\${DTB}|$DTB|g" \
  -e "s|\${KERNEL}|$KERNEL|g" \
  -e "s|\${RAMDISK}|$RAMDISK|g" \
  "${IMAGE_ITS}" > "${BINARIES_DIR}/image.its"

mkimage -f "${BINARIES_DIR}/image.its" "${BINARIES_DIR}/image.itb"
