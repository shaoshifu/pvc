#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>      /* _wputenv */

int main(void) {
    STARTUPINFOW si = {sizeof(si)};
    PROCESS_INFORMATION pi;
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    /* ⚠️ 这里是 CreateProcessW 拉起的子进程，环境会被继承 ——
       所以必须先把存档路径改到测试档，否则 pvz.exe 会直接写玩家真存档。
       CreateProcessW 的 lpEnvironment 传 NULL = 继承本进程环境，
       因此这一行对子进程也生效。 */
    _wputenv(L"PVZ_SAVE_FILE=_test_weapons_save.dat");

    if (!CreateProcessW(L"pvz.exe", NULL, NULL, NULL, FALSE, 
                        CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        printf("FAIL: CreateProcess\n");
        return 1;
    }
    
    Sleep(3000);  /* 等待初始化 */
    
    TerminateProcess(pi.hProcess, 0);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    
    printf("OK: pvz.exe 启动成功（隐藏窗口测试）\n");
    return 0;
}
