/* 离屏渲染压力基准：用于量化后期自适应特效的收益。 */
#define WinMain pvz_winmain_unused
#include "pvz.c"
#undef WinMain

static HBITMAP wb, fb, ob;
static HDC outDC;

static void setupDc(void)
{
    HDC screen = GetDC(NULL);
    wchar_t *slash;
    gWorldDC = CreateCompatibleDC(screen);
    gFrameDC = CreateCompatibleDC(screen);
    outDC = CreateCompatibleDC(screen);
    wb = CreateCompatibleBitmap(screen, VIEW_W * SS, VIEW_H * SS);
    fb = CreateCompatibleBitmap(screen, VIEW_W, VIEW_H);
    ob = CreateCompatibleBitmap(screen, VIEW_W, VIEW_H);
    SelectObject(gWorldDC, wb);
    SelectObject(gFrameDC, fb);
    SelectObject(outDC, ob);
    ReleaseDC(NULL, screen);
    SetGraphicsMode(gWorldDC, GM_ADVANCED);
    gScaleXF.eM11 = (FLOAT)SS; gScaleXF.eM12 = 0;
    gScaleXF.eM21 = 0; gScaleXF.eM22 = (FLOAT)SS;
    gScaleXF.eDx = 0; gScaleXF.eDy = 0;
    gF15 = mkFont(15, 0, L"Arial"); gF18 = mkFont(18, 1, L"Arial");
    gF22 = mkFont(22, 1, L"Arial"); gF30 = mkFont(30, 1, L"Arial");
    gF54 = mkFont(54, 1, L"Arial"); gF72 = mkFont(72, 1, L"Arial");
    gIdentXF.eM11 = 1; gIdentXF.eM12 = 0; gIdentXF.eM21 = 0;
    gIdentXF.eM22 = 1; gIdentXF.eDx = 0; gIdentXF.eDy = 0;
    gCurXF = gScaleXF;
    GetModuleFileNameW(NULL, gAssetDir, MAX_PATH);
    slash = wcsrchr(gAssetDir, L'\\');
    if (slash) slash[1] = 0;
    wcscat(gAssetDir, L"assets\\");
    spriteLoadName(&gSprPlant[PT_PEASHOOTER], L"peashooter");
    spriteLoadName(&gSprZombie[ZT_NORMAL], L"zombie_normal");
    spriteLoadName(&gSprLawn, L"lawn");
    spriteLoadName(&gSprBackground, L"background");
}

static void fillStress(void)
{
    int i, r, c;
    resetGame();
    gState = ST_PLAY;
    gWave = 20;
    for (r = 0; r < ROWS; r++) for (c = 0; c < COLS; c++) {
        Plant *p = &grid[r][c];
        p->alive = 1; p->type = PT_PEASHOOTER; p->row = r; p->col = c;
        p->hp = p->maxhp = 300.0f; p->phase = (float)(r * COLS + c) * 0.1f;
    }
    for (i = 0; i < 120; i++) {
        Zombie *z = &zombies[i];
        z->active = 1; z->type = ZT_NORMAL; z->row = i % ROWS;
        z->x = 180.0f + (float)(i % 24) * 32.0f; z->y = cellBaseY(z->row);
        z->hp = z->maxhp = z->basehp = 1000.0f; z->anim = (float)i * 0.1f;
    }
    for (i = 0; i < MAX_PEAS; i++) {
        Pea *p = &peas[i];
        p->active = 1; p->row = i % ROWS; p->x = 120.0f + (float)(i % 40) * 20.0f;
        p->y = rowCY(p->row); p->vx = 300.0f; p->vy = (float)((i % 5) - 2) * 20.0f;
        p->dmg = 30.0f; p->wpn = WPN_NORMAL; p->src = PT_PEASHOOTER;
    }
    for (i = 0; i < MAX_PARTS; i++) {
        Particle *p = &parts[i];
        p->active = 1; p->x = (float)(i % 50) * 20.0f; p->y = 100.0f + (float)(i % 25) * 20.0f;
        p->life = p->maxlife = 1.0f; p->size = 4.0f; p->col = RGB(255, 200, 90);
    }
}

static double bench(int frames)
{
    LARGE_INTEGER f, a, b;
    int i;
    QueryPerformanceFrequency(&f);
    render(outDC); render(outDC);
    QueryPerformanceCounter(&a);
    for (i = 0; i < frames; i++) render(outDC);
    QueryPerformanceCounter(&b);
    return (double)(b.QuadPart - a.QuadPart) * 1000.0 / (double)f.QuadPart / frames;
}

int main(void)
{
    double full, adaptive;
    int i;
    setupDc();
    fillStress();
    gFxParticleLimit = MAX_PARTS; gFxTrailSteps = 4;
    full = bench(12);
    gFxParticleLimit = 180; gFxTrailSteps = 0;
    for (i = 180; i < MAX_PARTS; i++) parts[i].active = 0;
    adaptive = bench(12);
    printf("full=%.2fms adaptive=%.2fms improvement=%.1f%%\n",
           full, adaptive, full > 0.0 ? (full - adaptive) * 100.0 / full : 0.0);
    return 0;
}
