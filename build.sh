#!/bin/sh

rm -f /home/ryanandri07/android/android_kernel_motoe/clarity-N-condor/boot/zImage-dtb
rm -f /home/ryanandri07/android/android_kernel_motoe/clarity-N-condor/system/lib/modules/*.ko
rm -f /home/ryanandri07/android/android_kernel_motoe/clarity-N-condor/system/lib/modules/pronto/pronto_wlan.ko
rm -f /home/ryanandri07/android/android_kernel_motoe/arch/arm/boot/zImage

mv /home/ryanandri07/android/android_kernel_motoe/arch/arm/boot/zImage-dtb /home/ryanandri07/android/android_kernel_motoe/clarity-N-condor/boot

# get modules into one place
find -name "*.ko" -exec cp {} /home/ryanandri07/android/android_kernel_motoe/clarity-N-condor/system/lib/modules \;
sleep 2

# Remove Unneeded
/home/ryanandri07/android/linaro-cortex-a7-4.9/bin/arm-cortex_a7-linux-gnueabihf-strip --strip-unneeded /home/ryanandri07/android/android_kernel_motoe/clarity-N-condor/system/lib/modules/*.ko

# move to proper place
mv /home/ryanandri07/android/android_kernel_motoe/clarity-N-condor/system/lib/modules/wlan.ko /home/ryanandri07/android/android_kernel_motoe/clarity-N-condor/system/lib/modules/pronto/pronto_wlan.ko

DATE = `date +%d-%m-%Y`;
cd /home/ryanandri07/android/android_kernel_motoe/clarity-N-condor
zip -r /home/ryanandri07/android/release/Clarity-N-$DATE.zip *

