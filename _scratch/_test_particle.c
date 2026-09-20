/*
 * _test_particle.c - 粒子系统独立测试
 * 编译：gcc -O2 -o _test_particle.exe _test_particle.c particle.c -lgdi32 -luser32 -lm -Wall -Wextra
 * 运行：./_test_particle.exe
 */
#include "particle.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static HWND hwnd;
static HDC hdc, memDC;
static HBITMAP memBmp, oldBmp;
static int winW = 800, winH = 600;
static int running = 1;

/* 窗口过程 */
LRESULT CALLBACK WndProc(HWND hw, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CLOSE:
            running = 0;
            return 0;
        case WM_KEYDOWN:
            if (wp == VK_ESCAPE) running = 0;
            return 0;
        case WM_LBUTTONDOWN: {
            /* 左键点击：发射爆炸 */
            int mx = LOWORD(lp);
            int my = HIWORD(lp);
            emitExplosion((float)mx, (float)my, RGB(255, 100, 50));
            return 0;
        }
        case WM_RBUTTONDOWN: {
            /* 右键点击：发射冰冻 */
            int mx = LOWORD(lp);
            int my = HIWORD(lp);
            emitIceBurst((float)mx, (float)my);
            return 0;
        }
        case WM_MBUTTONDOWN: {
            /* 中键点击：发射阳光 */
            int mx = LOWORD(lp);
            int my = HIWORD(lp);
            emitSunGlow((float)mx, (float)my);
            return 0;
        }
    }
    return DefWindowProcW(hw, msg, wp, lp);
}

int main(void) {
    /* 初始化随机种子 */
    srand((unsigned)time(NULL));
    
    /* 注册窗口类 */
    WNDCLASSW wc = {0};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.lpszClassName = L"ParticleTest";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    RegisterClassW(&wc);
    
    /* 创建窗口 */
    RECT rc = {0, 0, winW, winH};
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    hwnd = CreateWindowW(L"ParticleTest", L"粒子系统测试 - 左键爆炸 右键冰冻 中键阳光 ESC退出",
                         WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                         rc.right - rc.left, rc.bottom - rc.top,
                         NULL, NULL, wc.hInstance, NULL);
    ShowWindow(hwnd, SW_SHOW);
    
    /* 创建离屏DC */
    hdc = GetDC(hwnd);
    memDC = CreateCompatibleDC(hdc);
    memBmp = CreateCompatibleBitmap(hdc, winW, winH);
    oldBmp = (HBITMAP)SelectObject(memDC, memBmp);
    
    /* 初始化粒子系统 */
    particleInit();
    
    printf("粒子系统测试启动\n");
    printf("- 左键点击：火焰爆炸\n");
    printf("- 右键点击：冰冻爆发\n");
    printf("- 中键点击：阳光粒子\n");
    printf("- ESC键：退出\n\n");
    
    /* 主循环 */
    LARGE_INTEGER freq, last, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&last);
    
    float autoSpawnTimer = 0;
    int frameCount = 0;
    float fpsTimer = 0;
    
    while (running) {
        /* 处理消息 */
        MSG msg;
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        
        /* 计算delta time */
        QueryPerformanceCounter(&now);
        float dt = (float)(now.QuadPart - last.QuadPart) / (float)freq.QuadPart;
        last = now;
        
        if (dt > 0.1f) dt = 0.1f;  /* 限制最大dt */
        
        /* 自动生成粒子（演示） */
        autoSpawnTimer += dt;
        if (autoSpawnTimer > 0.5f) {
            autoSpawnTimer = 0;
            float x = 100.0f + (float)(rand() % 600);
            float y = 100.0f + (float)(rand() % 400);
            
            int type = rand() % 5;
            switch (type) {
                case 0: emitHitSpark(x, y, RGB(255, 255, 100)); break;
                case 1: emitSmoke(x, y); break;
                case 2: emitFlameTail(x, y, 50.0f, -30.0f); break;
                case 3: emitIceBurst(x, y); break;
                case 4: emitSunGlow(x, y); break;
            }
        }
        
        /* 更新粒子 */
        particleUpdate(dt);
        
        /* 清屏 */
        RECT rc = {0, 0, winW, winH};
        FillRect(memDC, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
        
        /* 绘制粒子 */
        particleDraw(memDC);
        
        /* 绘制信息文本 */
        SetBkMode(memDC, TRANSPARENT);
        SetTextColor(memDC, RGB(0, 255, 0));
        char buf[256];
        sprintf(buf, "粒子数: %d / %d", particleGetCount(), MAX_PARTICLES);
        TextOutA(memDC, 10, 10, buf, (int)strlen(buf));
        
        /* 显示FPS */
        fpsTimer += dt;
        frameCount++;
        if (fpsTimer >= 1.0f) {
            sprintf(buf, "FPS: %d", frameCount);
            fpsTimer = 0;
            frameCount = 0;
        }
        TextOutA(memDC, 10, 30, buf, (int)strlen(buf));
        
        /* 拷贝到屏幕 */
        BitBlt(hdc, 0, 0, winW, winH, memDC, 0, 0, SRCCOPY);
        
        Sleep(16);  /* ~60fps */
    }
    
    /* 清理 */
    SelectObject(memDC, oldBmp);
    DeleteObject(memBmp);
    DeleteDC(memDC);
    ReleaseDC(hwnd, hdc);
    DestroyWindow(hwnd);
    
    printf("\n测试结束\n");
    return 0;
}
