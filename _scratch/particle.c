/*
 * particle.c - 粒子系统实现
 */
#include "particle.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static Particle particles[MAX_PARTICLES];
static int particleN = 0;

/* 随机浮点数 [min, max] */
static float randf(float min, float max) {
    return min + (max - min) * ((float)rand() / (float)RAND_MAX);
}

void particleInit(void) {
    memset(particles, 0, sizeof(particles));
    particleN = 0;
}

void emitParticles(float x, float y, int count, ParticleType type, COLORREF color,
                   float speedMin, float speedMax, float lifeMin, float lifeMax,
                   float sizeMin, float sizeMax) {
    for (int i = 0; i < count; i++) {
        if (particleN >= MAX_PARTICLES) break;
        
        Particle *p = &particles[particleN++];
        p->x = x;
        p->y = y;
        
        /* 随机方向和速度 */
        float angle = randf(0, 2.0f * (float)M_PI);
        float speed = randf(speedMin, speedMax);
        p->vx = cosf(angle) * speed;
        p->vy = sinf(angle) * speed;
        
        /* 重力加速度（烟雾向上，其他向下） */
        p->ax = 0;
        p->ay = (type == PT_SMOKE) ? -120.0f : 280.0f;
        
        p->life = p->maxLife = randf(lifeMin, lifeMax);
        p->color = color;
        p->size = randf(sizeMin, sizeMax);
        p->rotation = randf(0, 360.0f);
        p->rotationSpeed = randf(-180.0f, 180.0f);
        p->type = type;
    }
}

/* === 预设发射器 === */

void emitExplosion(float x, float y, COLORREF color) {
    /* 爆炸：大量快速径向粒子 */
    emitParticles(x, y, 24, PT_DOT, color, 120.0f, 220.0f, 0.3f, 0.6f, 3.0f, 8.0f);
    emitParticles(x, y, 12, PT_SPARK, color, 180.0f, 280.0f, 0.2f, 0.4f, 2.0f, 5.0f);
}

void emitHitSpark(float x, float y, COLORREF color) {
    /* 击中火花：少量快速粒子 */
    emitParticles(x, y, 8, PT_SPARK, color, 80.0f, 150.0f, 0.15f, 0.3f, 2.0f, 4.0f);
}

void emitSmoke(float x, float y) {
    /* 烟雾：缓慢上升的半透明圆 */
    COLORREF gray = RGB(120, 120, 120);
    emitParticles(x, y, 6, PT_SMOKE, gray, 10.0f, 30.0f, 0.8f, 1.5f, 8.0f, 16.0f);
}

void emitSunGlow(float x, float y) {
    /* 阳光产出：金色粒子上浮 */
    COLORREF gold = RGB(255, 220, 70);
    emitParticles(x, y, 12, PT_STAR, gold, 20.0f, 50.0f, 0.5f, 1.0f, 4.0f, 8.0f);
}

void emitIceBurst(float x, float y) {
    /* 冰冻爆发：蓝色冰晶 */
    COLORREF ice = RGB(150, 220, 255);
    emitParticles(x, y, 16, PT_ICE, ice, 60.0f, 120.0f, 0.4f, 0.7f, 3.0f, 6.0f);
}

void emitFlameTail(float x, float y, float vx, float vy) {
    /* 火焰拖尾：橙红色小粒子，继承部分速度 */
    if (particleN >= MAX_PARTICLES - 3) return;
    
    for (int i = 0; i < 3; i++) {
        Particle *p = &particles[particleN++];
        p->x = x + randf(-3, 3);
        p->y = y + randf(-3, 3);
        p->vx = vx * 0.3f + randf(-20, 20);
        p->vy = vy * 0.3f + randf(-20, 20);
        p->ax = 0;
        p->ay = -50.0f;  /* 轻微上浮 */
        p->life = p->maxLife = randf(0.15f, 0.3f);
        p->color = (rand() % 2) ? RGB(255, 180, 60) : RGB(255, 80, 40);
        p->size = randf(2.0f, 4.0f);
        p->rotation = 0;
        p->rotationSpeed = 0;
        p->type = PT_DOT;
    }
}

void particleUpdate(float dt) {
    int writeIdx = 0;
    
    for (int i = 0; i < particleN; i++) {
        Particle *p = &particles[i];
        
        /* 更新生命 */
        p->life -= dt;
        if (p->life <= 0) continue;  /* 死亡，跳过 */
        
        /* 更新物理 */
        p->vx += p->ax * dt;
        p->vy += p->ay * dt;
        p->x += p->vx * dt;
        p->y += p->vy * dt;
        p->rotation += p->rotationSpeed * dt;
        
        /* 烟雾类型：随时间增大+淡化 */
        if (p->type == PT_SMOKE) {
            p->size += 12.0f * dt;
        }
        
        /* 保留存活的粒子（压缩数组） */
        if (writeIdx != i) {
            particles[writeIdx] = *p;
        }
        writeIdx++;
    }
    
    particleN = writeIdx;
}

void particleDraw(HDC dc) {
    for (int i = 0; i < particleN; i++) {
        Particle *p = &particles[i];
        
        /* 计算透明度（生命比例） */
        float alpha = p->life / p->maxLife;
        if (alpha > 1.0f) alpha = 1.0f;
        if (alpha < 0.0f) alpha = 0.0f;
        
        /* 烟雾类型快速淡出 */
        if (p->type == PT_SMOKE) {
            alpha = alpha * alpha;  /* 平方衰减 */
        }
        
        /* 提取RGB分量 */
        int r = GetRValue(p->color);
        int g = GetGValue(p->color);
        int b = GetBValue(p->color);
        
        /* 根据透明度调整颜色 */
        BYTE alphaByte = (BYTE)(alpha * 255);
        
        /* 创建半透明画刷/画笔 */
        BLENDFUNCTION bf = {AC_SRC_OVER, 0, alphaByte, 0};
        
        int x = (int)p->x;
        int y = (int)p->y;
        int sz = (int)p->size;
        
        switch (p->type) {
            case PT_DOT: {
                /* 圆点：用Ellipse绘制 */
                HBRUSH brush = CreateSolidBrush(p->color);
                HBRUSH oldBrush = (HBRUSH)SelectObject(dc, brush);
                HPEN pen = CreatePen(PS_NULL, 0, 0);
                HPEN oldPen = (HPEN)SelectObject(dc, pen);
                
                /* 简化：直接绘制，不做透明混合（性能考虑） */
                /* 真正的透明需要AlphaBlend + 离屏DC */
                Ellipse(dc, x - sz/2, y - sz/2, x + sz/2, y + sz/2);
                
                SelectObject(dc, oldBrush);
                SelectObject(dc, oldPen);
                DeleteObject(brush);
                DeleteObject(pen);
                break;
            }
            
            case PT_SPARK: {
                /* 火花：拉长的线段 */
                HPEN pen = CreatePen(PS_SOLID, (sz > 2) ? 2 : 1, p->color);
                HPEN oldPen = (HPEN)SelectObject(dc, pen);
                
                float len = sz * 2.0f;
                float angle = atan2f(p->vy, p->vx);
                int x1 = x - (int)(cosf(angle) * len * 0.5f);
                int y1 = y - (int)(sinf(angle) * len * 0.5f);
                int x2 = x + (int)(cosf(angle) * len * 0.5f);
                int y2 = y + (int)(sinf(angle) * len * 0.5f);
                
                MoveToEx(dc, x1, y1, NULL);
                LineTo(dc, x2, y2);
                
                SelectObject(dc, oldPen);
                DeleteObject(pen);
                break;
            }
            
            case PT_SMOKE: {
                /* 烟雾：半透明大圆 */
                /* 简化版：用灰色Ellipse */
                int gray = 120 + (int)(alpha * 60);
                HBRUSH brush = CreateSolidBrush(RGB(gray, gray, gray));
                HBRUSH oldBrush = (HBRUSH)SelectObject(dc, brush);
                HPEN pen = CreatePen(PS_NULL, 0, 0);
                HPEN oldPen = (HPEN)SelectObject(dc, pen);
                
                Ellipse(dc, x - sz, y - sz, x + sz, y + sz);
                
                SelectObject(dc, oldBrush);
                SelectObject(dc, oldPen);
                DeleteObject(brush);
                DeleteObject(pen);
                break;
            }
            
            case PT_STAR: {
                /* 星光：十字形 */
                HPEN pen = CreatePen(PS_SOLID, 2, p->color);
                HPEN oldPen = (HPEN)SelectObject(dc, pen);
                
                MoveToEx(dc, x - sz, y, NULL);
                LineTo(dc, x + sz, y);
                MoveToEx(dc, x, y - sz, NULL);
                LineTo(dc, x, y + sz);
                
                SelectObject(dc, oldPen);
                DeleteObject(pen);
                break;
            }
            
            case PT_ICE: {
                /* 冰晶：六边形（简化为钻石） */
                HPEN pen = CreatePen(PS_SOLID, 1, p->color);
                HPEN oldPen = (HPEN)SelectObject(dc, pen);
                
                POINT pts[5] = {
                    {x, y - sz},
                    {x + sz, y},
                    {x, y + sz},
                    {x - sz, y},
                    {x, y - sz}
                };
                Polyline(dc, pts, 5);
                
                SelectObject(dc, oldPen);
                DeleteObject(pen);
                break;
            }
            
            case PT_LEAF: {
                /* 叶片：简化为小椭圆 */
                HBRUSH brush = CreateSolidBrush(RGB(100, 180, 80));
                HBRUSH oldBrush = (HBRUSH)SelectObject(dc, brush);
                HPEN pen = CreatePen(PS_NULL, 0, 0);
                HPEN oldPen = (HPEN)SelectObject(dc, pen);
                
                Ellipse(dc, x - sz, y - sz/2, x + sz, y + sz/2);
                
                SelectObject(dc, oldBrush);
                SelectObject(dc, oldPen);
                DeleteObject(brush);
                DeleteObject(pen);
                break;
            }
        }
    }
}

void particleClear(void) {
    particleN = 0;
}

int particleGetCount(void) {
    return particleN;
}
