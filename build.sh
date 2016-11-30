#!/bin/sh

rm -f /home/ryanandri07/android/android_kernel_motoe/clarity-N-condor/boot/zImage-dtb
rm -f /home/ryanandri07/android/android_kernel_motoe/clarity-N-condor/system/lib/modules/*.ko
rm -f /home/ryanandri07/android/android_kernel_motoe/arch/arm/boot/zImage

mv /home/ryanandri07/android/android_kernel_motoe/arch/arm/boot/zImage-dtb /home/ryanandri07/android/android_kernel_motoe/clarity-N-condor/boot

# get modules into one place
find -name "*.ko" -exec cp {} /home/ryanandri07/android/android_kernel_motoe/clarity-N-condor/system/lib/modules \;
sleep 2

# Remove Unneeded
/home/ryanandri07/android/linaro-cortex-a7-4.9/bin/arm-cortex_a7-linux-gnueabihf-strip --strip-unneeded /home/ryanandri07/android/android_kernel_motoe/clarity-N-condor/system/lib/modules/*.ko

cd /home/ryanandri07/android/android_kernel_motoe/clarity-N-condor

DATE=`date +%d-%m-%Y`;
zip -r /home/ryanandri07/android/release/Clarity-N-$DATE.zip *

