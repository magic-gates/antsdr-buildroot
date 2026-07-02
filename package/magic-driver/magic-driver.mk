MAGIC_DRIVER_VERSION = 0.1
MAGIC_DRIVER_SITE = $(BR2_EXTERNAL_MAGIC_SDR_PATH)/package/magic-driver/src
MAGIC_DRIVER_SITE_METHOD = local

$(eval $(kernel-module))
$(eval $(generic-package))
