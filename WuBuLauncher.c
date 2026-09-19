/*
 * WuBuCapture Launcher — watches and restarts the worker process
 * Never dies. Restarts worker if it crashes or if source is recompiled.
 * 
 * Build: gcc -O2 -std=c11 -o WuBuLauncher.exe WuBuLauncher.c -luser32
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

#define WORKER_EXE "WuBuCapture.exe"
#define SOURCE_FILE "WuBuCapture.c"
#define RESTART_DELAY_MS 500

static DWORD get_file_mtime(const char *path) {
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    FILETIME ft;
    GetFileTime(h, NULL, NULL, &ft);
    CloseHandle(h);
    return ft.dwLowDateTime;
}

int main() {
    printf("[LAUNCHER] WuBuCapture Launcher started\n");
    printf("[LAUNCHER] Worker: %s\n", WORKER_EXE);
    printf("[LAUNCHER] Watching: %s for changes\n", SOURCE_FILE);
    
    DWORD last_mtime = get_file_mtime(SOURCE_FILE);
    
    while (1) {
        /* Start worker */
        STARTUPINFOA si = {sizeof(si)};
        PROCESS_INFORMATION pi = {0};
        
        printf("[LAUNCHER] Starting worker...\n");
        
        char cmd[512];
        snprintf(cmd, sizeof(cmd), "%s", WORKER_EXE);
        
        if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE,
            ABOVE_NORMAL_PRIORITY_CLASS, NULL, NULL, &si, &pi)) {
            fprintf(stderr, "[LAUNCHER] Failed to start worker: %lu\n", GetLastError());
            Sleep(2000);
            continue;
        }
        
        HANDLE hProcess = pi.hProcess;
        DWORD pid = pi.dwProcessId;
        printf("[LAUNCHER] Worker started (PID %lu)\n", pid);
        
        /* Monitor loop */
        BOOL running = TRUE;
        while (running) {
            /* Check if process is still alive */
            DWORD exitCode;
            if (GetExitCodeProcess(hProcess, &exitCode) && exitCode != STILL_ACTIVE) {
                printf("[LAUNCHER] Worker exited with code %lu. Restarting...\n", exitCode);
                running = FALSE;
                break;
            }
            
            /* Check if source file changed */
            DWORD mtime = get_file_mtime(SOURCE_FILE);
            if (mtime != last_mtime) {
                printf("[LAUNCHER] Source changed! Recompiling and restarting...\n");
                last_mtime = mtime;
                
                /* Gracefully terminate worker */
                TerminateProcess(hProcess, 0);
                WaitForSingleObject(hProcess, 2000);
                running = FALSE;
                
                /* Rebuild */
                printf("[LAUNCHER] Building...\n");
                int ret = system("gcc -O2 -std=c11 -o WuBuCapture.exe WuBuCapture.c "
                    "-lmfplat -lmf -lmfreadwrite -lmfuuid "
                    "-ld3d11 -ldxgi -ld3dcompiler "
                    "-lole32 -loleaut32 -luuid -luser32 -lgdi32 -lwinmm -ldxguid 2>build_errors.log");
                
                if (ret != 0) {
                    fprintf(stderr, "[LAUNCHER] BUILD FAILED! Check build_errors.log\n");
                    /* Wait for source to change again */
                    while (get_file_mtime(SOURCE_FILE) == last_mtime) {
                        Sleep(500);
                    }
                    last_mtime = get_file_mtime(SOURCE_FILE);
                } else {
                    printf("[LAUNCHER] Build OK. Restarting worker...\n");
                }
                break;
            }
            
            Sleep(500);  /* Poll interval */
        }
        
        CloseHandle(hProcess);
        if (pi.hThread) CloseHandle(pi.hThread);
        
        printf("[LAUNCHER] Restarting in %dms...\n", RESTART_DELAY_MS);
        Sleep(RESTART_DELAY_MS);
    }
    
    return 0;
}
