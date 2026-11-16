@echo off
echo ===================================================
echo Starting SyncLab Backend (C: pthreads + semaphores)
echo ===================================================
echo The C backend uses POSIX threads/semaphores, so on Windows it runs inside WSL.
wsl --cd "%~dp0backend" sh -c "make && ./build/synclab_server --host 127.0.0.1 --port 8000 --dist ../frontend/dist"
pause
