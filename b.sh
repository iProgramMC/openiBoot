#! /bin/bash

set -e

echo "building using scons"
scons iPodTouch1G

echo "turning into raw image"
xpwntool ipt_1g_openiboot.img3 ../../../../iphone-qemu/ipod-data/ipt_1g_openiboot.dec
