#!/bin/bash

gdb-multiarch ./kernel/lebirun_debug.kernel \
  -q \
  -ex "set architecture i386:x86-64" \
  -ex "target remote localhost:1234" \
  -ex "continue"

