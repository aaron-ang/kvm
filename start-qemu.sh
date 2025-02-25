#!/bin/bash

# QEMU command to start the virtual machine
qemu-system-x86_64 \
  -kernel bzImage \
  -initrd initrd.img-6.8.0-1021-aws \
  -m 1G \
  -append "console=ttyS0" \
  -nographic