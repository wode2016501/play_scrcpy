// windows_receiver.c
// 编译 (MinGW): gcc -o windows_receiver.exe windows_receiver.c -lws2_32 -lwinmm -lavcodec -lavutil -lswscale -lm
// 运行: windows_receiver.exe [服务器IP]

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <process.h>   // _beginthreadex
#include <signal.h>

#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>

// ==================== 配置 ====================
#define DEFAULT_SERVER_IP "192.168.100.1"
#define TOUCH_PORT      9000
#define VIDEO_PORT      9999
#define AUDIO_PORT      9998
#define XY_SWAP_MODE    0

// ==================== 触摸点管理 ====================
typedef struct {
    int id;
    int x;
    int y;
    int active;   // 0=按下 1=移动 2=抬起
} TouchPoint;

// ==================== 按键点管理 ====================
typedef struct {
    int keyCode;
    int active;
} KeyPoint;

// ==================== 全局变量 ====================
static volatile int running = 1;
static SOCKET touch_socket = INVALID_SOCKET;
static SOCKET video_fd = INVALID_SOCKET;
static SOCKET audio_fd = INVALID_SOCKET;
static char server_ip[32] = DEFAULT_SERVER_IP;

static HWND g_hWnd = NULL;
static HINSTANCE g_hInst = NULL;

// 视频相关
static int SERVER_WIDTH = 0;
static int SERVER_HEIGHT = 0;
static int WINDOW_WIDTH = 0;
static int WINDOW_HEIGHT = 0;

static HBITMAP g_hBitmap = NULL;          // 兼容位图
static BITMAPINFO g_bmpInfo = {0};
static void *g_pixelData = NULL;          // 指向位图像素数据的指针
static CRITICAL_SECTION g_csBitmap;       // 保护位图数据

// 音频相关
static HWAVEOUT g_hWaveOut = NULL;
static WAVEHDR g_waveHdr[2] = {0};        // 双缓冲
static int g_audioBufferCount = 2;
static volatile int g_audioIndex = 0;

// 鼠标状态
static int mouse_pressed = 0;
static const int mouse_id = 0;

// WASD 模拟触摸
static int start_wasd_x = 371;
static int start_wasd_y = 863;
static int wsad_count = 0;
static const int wasd_id = 1;
static struct wasd_key { int w,a,s,d; } wasd_state = {0,0,0,0};
static int wasd_x = 371;
static int wasd_y = 863;

static int frame_in = 0, frame_out = 0;

// ==================== 函数声明 ====================
LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
int tcp_connect(const char *ip, int port);
int read_(SOCKET fd, char *buf, size_t size, int max_size);
int readyz(SOCKET fd, char *buf, int size_max);
void send_touch_down(int id, int x, int y);
void send_touch_move(int id, int x, int y);
void send_touch_up(int id);
void send_key_event(int linux_keycode, int pressed);
int WinVKToSDLKey(WPARAM vk);
int SDLKeyToLinuxKeycode(int sdl_key);
void update_display_rect(int clientW, int clientH, RECT *dstRect);
void draw_video_frame(void);

// ==================== 网络连接 ====================
int tcp_connect(const char *ip, int port) {
    SOCKET sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == INVALID_SOCKET) {
        printf("[-] Socket 创建失败: %d\n", WSAGetLastError());
        return INVALID_SOCKET;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, ip, &addr.sin_addr);

    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        printf("[-] 连接 %s:%d 失败: %d\n", ip, port, WSAGetLastError());
        closesocket(sock);
        return INVALID_SOCKET;
    }
    printf("[+] 已连接到 %s:%d\n", ip, port);
    return sock;
}

// ==================== 数据读取 ====================
int read_(SOCKET fd, char *buf, size_t size, int max_size) {
    if (size > (size_t)max_size) return -1;
    size_t left = size;
    while (left > 0) {
        int ret = recv(fd, buf, (int)left, 0);
        if (ret <= 0) return -1;
        buf += ret;
        left -= ret;
    }
    return (int)size;
}

int readyz(SOCKET fd, char *buf, int size_max) {
    long long pts;
    if (read_(fd, (char*)&pts, 8, size_max) != 8) return -1;
    int size;
    if (read_(fd, (char*)&size, 4, size_max) != 4) return -1;
    size = ntohl(size);
    if (size > 0 && size <= size_max) {
        if (read_(fd, buf, size, size_max) == size) return size;
    }
    return -1;
}

// ==================== 触摸/按键发送 ====================
void send_touch_down(int id, int x, int y) {
    int mapped_x = x, mapped_y = y;
    if (id == mouse_id) {
        mapped_x = x;  // 已经传入了服务器坐标，见事件处理中的转换
        mapped_y = y;
    }
    if (mapped_x < 0 || mapped_y < 0) return;

    TouchPoint tp = { .id = id, .x = mapped_x, .y = mapped_y, .active = 0 };
    int tmp = sizeof(tp);
    send(touch_socket, (char*)&tmp, sizeof(int), 0);
    send(touch_socket, (char*)&tp, sizeof(tp), 0);
    printf("[触摸按下] 服务器(%d,%d)\n", mapped_x, mapped_y);
}

void send_touch_move(int id, int x, int y) {
    int mapped_x = x, mapped_y = y;
    if (id == mouse_id) {
        mapped_x = x;
        mapped_y = y;
    }
    if (mapped_x < 0 || mapped_y < 0) return;

    TouchPoint tp = { .id = id, .x = mapped_x, .y = mapped_y, .active = 1 };
    int tmp = sizeof(tp);
    send(touch_socket, (char*)&tmp, sizeof(int), 0);
    send(touch_socket, (char*)&tp, sizeof(tp), 0);
}

void send_touch_up(int id) {
    TouchPoint tp = { .id = id, .x = 0, .y = 0, .active = 2 };
    int tmp = sizeof(tp);
    send(touch_socket, (char*)&tmp, sizeof(int), 0);
    send(touch_socket, (char*)&tp, sizeof(tp), 0);
    printf("[触摸抬起]\n");
}

void send_key_event(int linux_keycode, int pressed) {
    KeyPoint kp = { .keyCode = linux_keycode, .active = pressed };
    int tmp = sizeof(kp);
    send(touch_socket, (char*)&tmp, sizeof(int), 0);
    send(touch_socket, (char*)&kp, sizeof(kp), 0);
}

// ==================== 虚拟键码转换 ====================
int WinVKToSDLKey(WPARAM vk) {
    switch (vk) {
        case VK_BACK:    return 8;   // SDLK_BACKSPACE
        case VK_TAB:     return 9;   // SDLK_TAB
        case VK_RETURN:  return 13;  // SDLK_RETURN
        case VK_SHIFT:   return 16;  // 需要区分左右，简化处理
        case VK_CONTROL: return 17;
        case VK_MENU:    return 18;  // ALT
        case VK_CAPITAL: return 1073741881; // SDLK_CAPSLOCK
        case VK_ESCAPE:  return 27;  // SDLK_ESCAPE
        case VK_SPACE:   return 32;  // SDLK_SPACE
        case VK_LEFT:    return 1073741904; // SDLK_LEFT
        case VK_UP:      return 1073741906;
        case VK_RIGHT:   return 1073741903;
        case VK_DOWN:    return 1073741905;
        case '0': return '0'; case '1': return '1'; case '2': return '2';
        case '3': return '3'; case '4': return '4'; case '5': return '5';
        case '6': return '6'; case '7': return '7'; case '8': return '8';
        case '9': return '9';
        case 'A': return 'a'; case 'B': return 'b'; case 'C': return 'c';
        case 'D': return 'd'; case 'E': return 'e'; case 'F': return 'f';
        case 'G': return 'g'; case 'H': return 'h'; case 'I': return 'i';
        case 'J': return 'j'; case 'K': return 'k'; case 'L': return 'l';
        case 'M': return 'm'; case 'N': return 'n'; case 'O': return 'o';
        case 'P': return 'p'; case 'Q': return 'q'; case 'R': return 'r';
        case 'S': return 's'; case 'T': return 't'; case 'U': return 'u';
        case 'V': return 'v'; case 'W': return 'w'; case 'X': return 'x';
        case 'Y': return 'y'; case 'Z': return 'z';
        case VK_F1: return 1073741882; case VK_F2: return 1073741883;
        case VK_F3: return 1073741884; case VK_F4: return 1073741885;
        case VK_F5: return 1073741886; case VK_F6: return 1073741887;
        case VK_F7: return 1073741888; case VK_F8: return 1073741889;
        case VK_F9: return 1073741890; case VK_F10: return 1073741891;
        case VK_F11: return 1073741892; case VK_F12: return 1073741893;
        case VK_HOME:   return 1073741898;
        case VK_END:    return 1073741901;
        case VK_PRIOR:  return 1073741899; // PAGEUP
        case VK_NEXT:   return 1073741902; // PAGEDOWN
        case VK_INSERT: return 1073741897;
        case VK_DELETE: return 127;
        case VK_OEM_MINUS: return '-';
        case VK_OEM_PLUS:  return '=';
        default: return 0;
    }
}

int SDLKeyToLinuxKeycode(int sdl_key) {
    // 原函数直接可用，此处省略定义（同原程序）
    // 为了篇幅，保留必要的映射
    switch (sdl_key) {
        case 'a': return 30; case 'b': return 48; case 'c': return 46;
        case 'd': return 32; case 'e': return 18; case 'f': return 33;
        case 'g': return 34; case 'h': return 35; case 'i': return 23;
        case 'j': return 36; case 'k': return 37; case 'l': return 38;
        case 'm': return 50; case 'n': return 49; case 'o': return 24;
        case 'p': return 25; case 'q': return 16; case 'r': return 19;
        case 's': return 31; case 't': return 20; case 'u': return 22;
        case 'v': return 47; case 'w': return 17; case 'x': return 45;
        case 'y': return 21; case 'z': return 44;
        case '0': return 11; case '1': return 2;  case '2': return 3;
        case '3': return 4;  case '4': return 5;  case '5': return 6;
        case '6': return 7;  case '7': return 8;  case '8': return 9;
        case '9': return 10;
        case 32: return 57;   // SPACE
        case 13: return 28;   // ENTER
        case 8:  return 14;   // BACKSPACE
        case 9:  return 15;   // TAB
        case 27: return 1;    // ESC
        case 1073741904: return 105; // LEFT
        case 1073741903: return 106; // RIGHT
        case 1073741906: return 103; // UP
        case 1073741905: return 108; // DOWN
        case 1073741882: return 59;  // F1
        case 1073741883: return 60;  // F2
        case 1073741884: return 61;  // F3
        case 1073741885: return 62;  // F4
        case 1073741886: return 63;  // F5
        case 1073741887: return 64;  // F6
        case 1073741888: return 65;  // F7
        case 1073741889: return 66;  // F8
        case 1073741890: return 67;  // F9
        case 1073741891: return 68;  // F10
        case 1073741892: return 87;  // F11
        case 1073741893: return 88;  // F12
        case 1073741898: return 102; // HOME
        case 1073741901: return 107; // END
        case 1073741899: return 104; // PAGEUP
        case 1073741902: return 109; // PAGEDOWN
        case 1073741897: return 110; // INSERT
        case 127:        return 111; // DELETE
        default: return 0;
    }
}

// ==================== 视频显示辅助 ====================
void update_display_rect(int clientW, int clientH, RECT *dstRect) {
    float video_aspect = (float)SERVER_WIDTH / SERVER_HEIGHT;
    float window_aspect = (float)clientW / clientH;

    if (window_aspect > video_aspect) {
        // 上下黑边
        dstRect->left = (clientW - (int)(clientH * video_aspect)) / 2;
        dstRect->right = dstRect->left + (int)(clientH * video_aspect);
        dstRect->top = 0;
        dstRect->bottom = clientH;
    } else {
        // 左右黑边
        dstRect->top = (clientH - (int)(clientW / video_aspect)) / 2;
        dstRect->bottom = dstRect->top + (int)(clientW / video_aspect);
        dstRect->left = 0;
        dstRect->right = clientW;
    }
}

void draw_video_frame(void) {
    if (!g_hWnd || !g_hBitmap) return;

    HDC hdc = GetDC(g_hWnd);
    if (!hdc) return;

    RECT clientRect;
    GetClientRect(g_hWnd, &clientRect);
    int clientW = clientRect.right - clientRect.left;
    int clientH = clientRect.bottom - clientRect.top;

    RECT dstRect;
    update_display_rect(clientW, clientH, &dstRect);

    HDC memDC = CreateCompatibleDC(hdc);
    SelectObject(memDC, g_hBitmap);

    // 拉伸绘制到位图数据区域（原始大小）
    SetStretchBltMode(hdc, HALFTONE);
    StretchBlt(hdc, dstRect.left, dstRect.top,
               dstRect.right - dstRect.left, dstRect.bottom - dstRect.top,
               memDC, 0, 0, SERVER_WIDTH, SERVER_HEIGHT, SRCCOPY);

    DeleteDC(memDC);
    ReleaseDC(g_hWnd, hdc);
}

// ==================== 音频回调（写入数据） ====================
void audio_callback(HWAVEOUT hwo, UINT uMsg, DWORD_PTR dwInstance, DWORD_PTR dwParam1, DWORD_PTR dwParam2) {
    if (uMsg == WOM_DONE) {
        WAVEHDR *hdr = (WAVEHDR*)dwParam1;
        waveOutUnprepareHeader(hwo, hdr, sizeof(WAVEHDR));
        // 标记缓冲区空闲
        int idx = (hdr->dwUser == 0) ? 0 : 1;
        // 可以由音频线程重新填充
    }
}

// ==================== 音频线程 ====================
unsigned __stdcall audio_thread_func(void *arg) {
    printf("[+] 音频线程启动\n");

    // 跳过音频头（69字节）
    char header[69];
    if (read_(audio_fd, header, 69, 69) != 69) {
        printf("[-] 音频头读取失败\n");
        running = 0;
        return 1;
    }

    // 初始化音频格式
    WAVEFORMATEX wfx = {0};
    wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = 2;
    wfx.nSamplesPerSec = 48000;
    wfx.wBitsPerSample = 16;
    wfx.nBlockAlign = wfx.nChannels * wfx.wBitsPerSample / 8;
    wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;

    if (waveOutOpen(&g_hWaveOut, WAVE_MAPPER, &wfx, (DWORD_PTR)audio_callback, 0, CALLBACK_FUNCTION) != MMSYSERR_NOERROR) {
        printf("[-] 打开音频设备失败\n");
        running = 0;
        return 1;
    }

    int buffer_size = 176400; // 约1秒数据
    uint8_t *pcm_buffer = (uint8_t*)malloc(buffer_size);
    if (!pcm_buffer) {
        running = 0;
        return 1;
    }

    // 准备两个缓冲区
    for (int i = 0; i < 2; i++) {
        memset(&g_waveHdr[i], 0, sizeof(WAVEHDR));
        g_waveHdr[i].lpData = (LPSTR)(pcm_buffer + i * buffer_size/2); // 实际分配双倍
        g_waveHdr[i].dwBufferLength = buffer_size/2;
        g_waveHdr[i].dwUser = i;
    }
    // 简单起见, 重新分配两个独立缓冲区
    free(pcm_buffer);
    pcm_buffer = (uint8_t*)malloc(176400);
    uint8_t *buf1 = (uint8_t*)malloc(88200);
    uint8_t *buf2 = (uint8_t*)malloc(88200);
    g_waveHdr[0].lpData = (LPSTR)buf1;
    g_waveHdr[0].dwBufferLength = 88200;
    g_waveHdr[1].lpData = (LPSTR)buf2;
    g_waveHdr[1].dwBufferLength = 88200;

    int currentBuf = 0;
    while (running) {
        int size = readyz(audio_fd, (char*)g_waveHdr[currentBuf].lpData,
                          (int)g_waveHdr[currentBuf].dwBufferLength);
        if (size <= 0) {
            printf("[-] 音频流断开\n");
            break;
        }
        // 调整实际数据长度
        g_waveHdr[currentBuf].dwBufferLength = size;
        waveOutPrepareHeader(g_hWaveOut, &g_waveHdr[currentBuf], sizeof(WAVEHDR));
        waveOutWrite(g_hWaveOut, &g_waveHdr[currentBuf], sizeof(WAVEHDR));
        currentBuf = (currentBuf + 1) % 2;
        // 等待缓冲区可用 (简化为简单延迟)
        Sleep(20);
    }

    waveOutReset(g_hWaveOut);
    waveOutClose(g_hWaveOut);
    free(buf1); free(buf2);
    closesocket(audio_fd);
    audio_fd = INVALID_SOCKET;
    printf("[+] 音频线程退出\n");
    return 0;
}

// ==================== 视频线程 ====================
unsigned __stdcall video_thread_func(void *arg) {
    printf("[+] 视频线程启动\n");

    // 读取视频头 (69字节)
    char header[69];
    if (read_(video_fd, header, 69, 69) != 69) {
        printf("[-] 视频头读取失败\n");
        running = 0;
        return 1;
    }

    int width, height;
    read_(video_fd, (char*)&width, 4, 69);
    read_(video_fd, (char*)&height, 4, 69);
    SERVER_WIDTH = ntohl(width);
    SERVER_HEIGHT = ntohl(height);
    WINDOW_WIDTH = SERVER_WIDTH;
    WINDOW_HEIGHT = SERVER_HEIGHT;

    printf("[+] 服务端分辨率: %dx%d\n", SERVER_WIDTH, SERVER_HEIGHT);
    printf("[+] XY转换模式: %d\n", XY_SWAP_MODE);

    // 创建兼容位图 (RGB32)
    memset(&g_bmpInfo, 0, sizeof(BITMAPINFO));
    g_bmpInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    g_bmpInfo.bmiHeader.biWidth = SERVER_WIDTH;
    g_bmpInfo.bmiHeader.biHeight = -SERVER_HEIGHT; // 自上而下
    g_bmpInfo.bmiHeader.biPlanes = 1;
    g_bmpInfo.bmiHeader.biBitCount = 32;
    g_bmpInfo.bmiHeader.biCompression = BI_RGB;

    HDC hdcScreen = GetDC(NULL);
    g_hBitmap = CreateDIBSection(hdcScreen, &g_bmpInfo, DIB_RGB_COLORS, &g_pixelData, NULL, 0);
    ReleaseDC(NULL, hdcScreen);
    if (!g_hBitmap) {
        printf("[-] 创建位图失败\n");
        running = 0;
        return 1;
    }

    // 初始化画布为黑色
    memset(g_pixelData, 0, SERVER_WIDTH * SERVER_HEIGHT * 4);

    // 设置窗口大小
    SetWindowPos(g_hWnd, NULL, 0, 0, WINDOW_WIDTH, WINDOW_HEIGHT,
                 SWP_NOMOVE | SWP_NOZORDER);
    UpdateWindow(g_hWnd);

    // 初始化解码器
    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!codec) {
        printf("[-] 找不到H.264解码器\n");
        running = 0;
        return 1;
    }

    AVCodecContext *codec_ctx = avcodec_alloc_context3(codec);
    codec_ctx->width = SERVER_WIDTH;
    codec_ctx->height = SERVER_HEIGHT;
    codec_ctx->pix_fmt = AV_PIX_FMT_YUV420P;
    codec_ctx->thread_count = 4;
    if (avcodec_open2(codec_ctx, codec, NULL) < 0) {
        printf("[-] 打开解码器失败\n");
        running = 0;
        return 1;
    }

    AVFrame *frame = av_frame_alloc();
    // 准备颜色转换上下文 (YUV420P -> RGB32)
    struct SwsContext *sws_ctx = sws_getContext(SERVER_WIDTH, SERVER_HEIGHT, AV_PIX_FMT_YUV420P,
                                                SERVER_WIDTH, SERVER_HEIGHT, AV_PIX_FMT_RGB32,
                                                SWS_BILINEAR, NULL, NULL, NULL);
    if (!sws_ctx) {
        printf("[-] 初始化颜色转换失败\n");
        running = 0;
        return 1;
    }

    int buffer_size = 1024 * 1024 * 6;
    uint8_t *video_buffer = (uint8_t*)malloc(buffer_size);
    AVPacket pkt;
    av_init_packet(&pkt);

    while (running) {
        int size = readyz(video_fd, (char*)video_buffer, buffer_size);
        if (size <= 0) break;

        pkt.data = video_buffer;
        pkt.size = size;
        if (avcodec_send_packet(codec_ctx, &pkt) < 0) continue;
        frame_in++;

        while (avcodec_receive_frame(codec_ctx, frame) == 0) {
            // YUV -> RGB32
            uint8_t *dest[4] = { (uint8_t*)g_pixelData, NULL, NULL, NULL };
            int dest_linesize[4] = { SERVER_WIDTH * 4, 0, 0, 0 };
            sws_scale(sws_ctx, frame->data, frame->linesize, 0, SERVER_HEIGHT, dest, dest_linesize);

            // 更新窗口显示 (直接绘制，不需要等待WM_PAINT)
            draw_video_frame();
            frame_out++;
        }
    }

    // 清理
    free(video_buffer);
    av_frame_free(&frame);
    avcodec_free_context(&codec_ctx);
    sws_freeContext(sws_ctx);
    DeleteObject(g_hBitmap);
    closesocket(video_fd);
    video_fd = INVALID_SOCKET;
    running = 0;
    printf("[+] 视频线程退出, 总帧数: %d\n", frame_out);
    return 0;
}

// ==================== 窗口过程 ====================
int map_x(int winX, int winY) {
    // 同原逻辑，根据当前窗口显示区域计算服务器坐标
    RECT client;
    GetClientRect(g_hWnd, &client);
    int clientW = client.right - client.left;
    int clientH = client.bottom - client.top;

    RECT dstRect;
    update_display_rect(clientW, clientH, &dstRect);
    int videoW = dstRect.right - dstRect.left;
    int videoH = dstRect.bottom - dstRect.top;

    int videoX = winX - dstRect.left;
    int videoY = winY - dstRect.top;
    if (videoX < 0 || videoX >= videoW || videoY < 0 || videoY >= videoH)
        return -1;
    int sx = videoX * SERVER_WIDTH / videoW;
    int sy = videoY * SERVER_HEIGHT / videoH;
    if (XY_SWAP_MODE) {
        int tmp = sx;
        sx = SERVER_HEIGHT - sy;
        sy = tmp;
    }
    return sx;
}

int map_y(int winX, int winY) {
    RECT client;
    GetClientRect(g_hWnd, &client);
    int clientW = client.right - client.left;
    int clientH = client.bottom - client.top;

    RECT dstRect;
    update_display_rect(clientW, clientH, &dstRect);
    int videoW = dstRect.right - dstRect.left;
    int videoH = dstRect.bottom - dstRect.top;

    int videoX = winX - dstRect.left;
    int videoY = winY - dstRect.top;
    if (videoX < 0 || videoX >= videoW || videoY < 0 || videoY >= videoH)
        return -1;
    int sx = videoX * SERVER_WIDTH / videoW;
    int sy = videoY * SERVER_HEIGHT / videoH;
    if (XY_SWAP_MODE) {
        int tmp = sx;
        sx = sy;
        sy = tmp;
    }
    return sy;
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    static int lastW = 0, lastH = 0;
    switch (msg) {
        case WM_CREATE:
            break;
        case WM_SIZE:
            {
                int newW = LOWORD(lParam);
                int newH = HIWORD(lParam);
                if (newW != lastW || newH != lastH) {
                    lastW = newW; lastH = newH;
                    WINDOW_WIDTH = newW;
                    WINDOW_HEIGHT = newH;
                    InvalidateRect(hwnd, NULL, TRUE);
                }
            }
            break;
        case WM_PAINT:
            {
                PAINTSTRUCT ps;
                HDC hdc = BeginPaint(hwnd, &ps);
                // 直接使用已有的位图绘制
                if (g_hBitmap) {
                    RECT client;
                    GetClientRect(hwnd, &client);
                    RECT dstRect;
                    update_display_rect(client.right - client.left, client.bottom - client.top, &dstRect);
                    HDC memDC = CreateCompatibleDC(hdc);
                    SelectObject(memDC, g_hBitmap);
                    StretchBlt(hdc, dstRect.left, dstRect.top,
                               dstRect.right - dstRect.left, dstRect.bottom - dstRect.top,
                               memDC, 0, 0, SERVER_WIDTH, SERVER_HEIGHT, SRCCOPY);
                    DeleteDC(memDC);
                }
                EndPaint(hwnd, &ps);
            }
            return 0;
        case WM_LBUTTONDOWN:
            {
                int x = GET_X_LPARAM(lParam);
                int y = GET_Y_LPARAM(lParam);
                if (!mouse_pressed) {
                    mouse_pressed = 1;
                    int sx = map_x(x, y);
                    int sy = map_y(x, y);
                    send_touch_down(mouse_id, sx, sy);
                }
            }
            break;
        case WM_MOUSEMOVE:
            if (mouse_pressed) {
                int x = GET_X_LPARAM(lParam);
                int y = GET_Y_LPARAM(lParam);
                int sx = map_x(x, y);
                int sy = map_y(x, y);
                send_touch_move(mouse_id, sx, sy);
            }
            break;
        case WM_LBUTTONUP:
            if (mouse_pressed) {
                mouse_pressed = 0;
                send_touch_up(mouse_id);
            }
            break;
        case WM_RBUTTONDOWN:
            // 右键 = 返回键
            send_key_event(158 /* KEY_BACK */, 1);
            send_key_event(158, 0);
            break;
        case WM_MBUTTONDOWN:
            // 中键 = Home
            send_key_event(102 /* KEY_HOME */, 1);
            send_key_event(102, 0);
            break;
        case WM_MOUSEWHEEL:
            {
                short delta = GET_WHEEL_DELTA_WPARAM(wParam);
                if (delta > 0) {
                    send_key_event(103 /* KEY_UP */, 1);
                    send_key_event(103, 0);
                } else {
                    send_key_event(108 /* KEY_DOWN */, 1);
                    send_key_event(108, 0);
                }
            }
            break;
        case WM_KEYDOWN:
            {
                int sdlKey = WinVKToSDLKey(wParam);
                if (sdlKey) {
                    int linuxKey = SDLKeyToLinuxKeycode(sdlKey);
                    // WASD 模拟触摸
                    int changed = 0;
                    switch (sdlKey) {
                        case 'w': if (!wasd_state.w) { wasd_state.w=1; wsad_count++; wasd_y-=100; changed=1; } break;
                        case 'a': if (!wasd_state.a) { wasd_state.a=1; wsad_count++; wasd_x-=100; changed=1; } break;
                        case 's': if (!wasd_state.s) { wasd_state.s=1; wsad_count++; wasd_y+=100; changed=1; } break;
                        case 'd': if (!wasd_state.d) { wasd_state.d=1; wsad_count++; wasd_x+=100; changed=1; } break;
                    }
                    if (changed) {
                        if (wsad_count == 1)
                            send_touch_down(wasd_id, start_wasd_x, start_wasd_y);
                        send_touch_move(wasd_id, wasd_x, wasd_y);
                    }
                    send_key_event(linuxKey, 1);
                }
                if (sdlKey == 27 || sdlKey == 'q') // ESC or Q
                    running = 0;
            }
            break;
        case WM_KEYUP:
            {
                int sdlKey = WinVKToSDLKey(wParam);
                if (sdlKey) {
                    int linuxKey = SDLKeyToLinuxKeycode(sdlKey);
                    int changed = 0;
                    switch (sdlKey) {
                        case 'w': if (wasd_state.w) { wasd_state.w=0; wsad_count--; wasd_y+=100; changed=1; } break;
                        case 'a': if (wasd_state.a) { wasd_state.a=0; wsad_count--; wasd_x+=100; changed=1; } break;
                        case 's': if (wasd_state.s) { wasd_state.s=0; wsad_count--; wasd_y-=100; changed=1; } break;
                        case 'd': if (wasd_state.d) { wasd_state.d=0; wsad_count--; wasd_x-=100; changed=1; } break;
                    }
                    if (changed) {
                        send_touch_move(wasd_id, wasd_x, wasd_y);
                        if (wsad_count == 0)
                            send_touch_up(wasd_id);
                    }
                    send_key_event(linuxKey, 0);
                }
            }
            break;
        case WM_DESTROY:
            running = 0;
            PostQuitMessage(0);
            break;
        default:
            return DefWindowProc(hwnd, msg, wParam, lParam);
    }
    return 0;
}

// ==================== 主函数 ====================
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    // 解析命令行参数
    char *ip = DEFAULT_SERVER_IP;
    if (strlen(lpCmdLine) > 0) {
        // 简单处理，忽略引号
        strncpy(server_ip, lpCmdLine, sizeof(server_ip)-1);
        server_ip[sizeof(server_ip)-1] = 0;
    }

    // 初始化Winsock
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2,2), &wsaData) != 0) {
        MessageBox(NULL, "WSAStartup failed", "Error", MB_OK);
        return 1;
    }

    // 连接服务器
    touch_socket = tcp_connect(server_ip, TOUCH_PORT);
    video_fd = tcp_connect(server_ip, VIDEO_PORT);
    audio_fd = tcp_connect(server_ip, AUDIO_PORT);
    if (touch_socket == INVALID_SOCKET || video_fd == INVALID_SOCKET || audio_fd == INVALID_SOCKET) {
        MessageBox(NULL, "无法连接到服务器", "错误", MB_OK);
        goto cleanup;
    }

    // 注册窗口类
    WNDCLASSEX wc = {0};
    wc.cbSize = sizeof(WNDCLASSEX);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BLACK+1);
    wc.lpszClassName = "RemoteReceiverClass";
    if (!RegisterClassEx(&wc)) {
        MessageBox(NULL, "窗口注册失败", "错误", MB_OK);
        goto cleanup;
    }

    g_hInst = hInstance;
    g_hWnd = CreateWindowEx(0, "RemoteReceiverClass", "Windows Receiver - Screen Mirror",
                            WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                            800, 600, NULL, NULL, hInstance, NULL);
    if (!g_hWnd) {
        MessageBox(NULL, "窗口创建失败", "错误", MB_OK);
        goto cleanup;
    }

    ShowWindow(g_hWnd, nCmdShow);
    UpdateWindow(g_hWnd);

    // 初始化临界区
    InitializeCriticalSection(&g_csBitmap);

    // 启动视频、音频线程
    HANDLE hVideo = (HANDLE)_beginthreadex(NULL, 0, video_thread_func, NULL, 0, NULL);
    HANDLE hAudio = (HANDLE)_beginthreadex(NULL, 0, audio_thread_func, NULL, 0, NULL);

    // 消息循环
    MSG msg;
    while (running && GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    running = 0;
    WaitForSingleObject(hVideo, INFINITE);
    WaitForSingleObject(hAudio, INFINITE);
    CloseHandle(hVideo);
    CloseHandle(hAudio);

cleanup:
    if (touch_socket != INVALID_SOCKET) closesocket(touch_socket);
    if (video_fd != INVALID_SOCKET) closesocket(video_fd);
    if (audio_fd != INVALID_SOCKET) closesocket(audio_fd);
    WSACleanup();
    DeleteCriticalSection(&g_csBitmap);
    return 0;
}