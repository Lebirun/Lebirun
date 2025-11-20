#!/bin/bash

gdb-multiarch ./kernel/lebirun.kernel \
  -q \
  -ex "set architecture i386" \
  -ex "target remote localhost:1234" \
  -ex "break kernel_main" \
  -ex "continue"

