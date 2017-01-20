#!/bin/sh

rm -f /home/ryanandri07/android/android_kernel_motoe/arch/arm/mach-msm/smd_rpc_sym.c
make clean && make mrproper
rm -f /home/ryanandri07/android/android_kernel_motoe/clarity-N-condor/boot/zImage-dtb
rm -f /home/ryanandri07/android/android_kernel_motoe/clarity-N-condor/system/lib/modules/*.ko
rm -f /home/ryanandri07/android/android_kernel_motoe/clarity-N-condor/system/lib/modules/pronto/pronto_wlan.ko

