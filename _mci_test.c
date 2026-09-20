/* MCI 播放能力探测。
   WAV 用 waveaudio、MP3 用 mpegvideo；
   另外单独验证 `play ... repeat` 是否可用 —— 游戏里用的是 repeat 版本，
   只测 `play t` 会漏掉"循环标志不被支持"这种情况。 */
#include <windows.h>
#include <mmsystem.h>
#include <stdio.h>
#include <string.h>

static void tryType(const char *file, const char *type, int useRepeat)
{
    char cmd[512], mode[64] = "";
    MCIERROR e;
    sprintf(cmd, "open \"%s\" type %s alias t", file, type);
    e = mciSendStringA(cmd, NULL, 0, NULL);
    if (e) {
        char buf[256] = "";
        mciGetErrorStringA(e, buf, 256);
        printf("  [%s%s] open 失败 (%lu): %s\n",
               type, useRepeat ? "+repeat" : "", (unsigned long)e, buf);
        return;
    }
    e = mciSendStringA(useRepeat ? "play t repeat" : "play t", NULL, 0, NULL);
    printf("  [%s%s] open OK, play -> %lu\n",
           type, useRepeat ? "+repeat" : "", (unsigned long)e);
    Sleep(1200);
    mciSendStringA("status t mode", mode, 64, NULL);
    printf("  [%s%s] 1.2 秒后 mode = '%s'\n",
           type, useRepeat ? "+repeat" : "", mode);
    mciSendStringA("close t", NULL, 0, NULL);
}

int main(int argc, char **argv)
{
    const char *file = NULL;
    int i;
    for (i = 1; i < argc; i++)
        if (strcmp(argv[i], "--repeat") != 0) file = argv[i];
    if (!file) { printf("用法: _mci_test <音频文件> [--repeat]\n"); return 1; }
    printf("测试文件: %s\n", file);
    printf("不带 repeat:\n");
    tryType(file, "waveaudio", 0);
    printf("带 repeat:\n");
    tryType(file, "waveaudio", 1);
    return 0;
}
