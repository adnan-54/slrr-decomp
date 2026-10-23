#!/bin/bash
# vmboot with memory/time caps (an uncapped runaway froze the container); args pass through
cd /root/slrr-re
ulimit -v "${VMBOOT_MEM:-1000000}"
exec timeout "${VMBOOT_TIMEOUT:-60}" src/build/vmboot "$@"
