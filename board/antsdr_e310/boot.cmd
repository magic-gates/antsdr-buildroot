setenv bootargs 'console=ttyPS0,115200 root=/dev/ram0 rdinit=/init cma=32M earlyprintk'
load mmc 0:1 0x10000000 image.itb
bootm 0x10000000
