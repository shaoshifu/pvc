/*
 * particle.h - 轻量级粒子系统
 * 用于爆炸、击中、烟雾等特效
 */
#ifndef PARTICLE_H
#define PARTICLE_H

#include <windows.h>

/* 粒子类型 */
typedef enum {
    PT_DOT,      /* 圆点（默认） */
    PT_SPARK,    /* 火花（拉长的点+拖尾） */
    PT_SMOKE,    /* 烟雾（半透明圆） */
    PT_STAR,     /* 星光（十字/钻石形状） */
    PT_ICE,      /* 冰晶（蓝色六边形） */
    PT_LEAF,     /* 叶片（植物碎片） */
} ParticleType;

/* 单个粒子 */
typedef struct {
    float x, y;           /* 位置 */
    float vx, vy;         /* 速度 */
    float ax, ay;         /* 加速度（重力等） */
    float life;           /* 剩余生命（秒） */
    float maxLife;        /* 初始生命 */
    COLORREF color;       /* 颜色 */
    float size;           /* 大小（像素） */
    float rotation;       /* 旋转角度（度） */
    float rotationSpeed;  /* 旋转速度 */
    ParticleType type;    /* 类型 */
} Particle;

#define MAX_PARTICLES 512

/* 初始化粒子系统 */
void particleInit(void);

/* 发射粒子组 */
void emitParticles(float x, float y, int count, ParticleType type, COLORREF color,
                   float speedMin, float speedMax, float lifeMin, float lifeMax,
                   float sizeMin, float sizeMax);

/* 预设发射器 */
void emitExplosion(float x, float y, COLORREF color);      /* 爆炸（径向） */
void emitHitSpark(float x, float y, COLORREF color);       /* 击中火花 */
void emitSmoke(float x, float y);                          /* 烟雾上升 */
void emitSunGlow(float x, float y);                        /* 阳光产出 */
void emitIceBurst(float x, float y);                       /* 冰冻爆发 */
void emitFlameTail(float x, float y, float vx, float vy);  /* 火焰拖尾 */

/* 更新所有粒子 */
void particleUpdate(float dt);

/* 绘制所有粒子 */
void particleDraw(HDC dc);

/* 清空所有粒子 */
void particleClear(void);

/* 统计信息 */
int particleGetCount(void);

#endif /* PARTICLE_H */
