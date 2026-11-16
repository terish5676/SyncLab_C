#!/bin/sh
echo "==================================================="
echo " Starting SyncLab Backend (C: pthreads + semaphores)"
echo "==================================================="
cd "$(dirname "$0")/backend" && make && ./build/synclab_server --host 127.0.0.1 --port 8000
