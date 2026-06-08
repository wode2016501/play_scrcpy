// windows_receiver_opengl_full.c
// 编译: gcc -o receiver.exe windows_receiver_opengl_full.c -lopengl32 -lglu32 -lgdi32 -lwinmm -lws2_32 -lavcodec -lavutil -lswscale -lm
// 运行: receiver.exe [服务器IP]

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <mmsystem.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <GL/gl.h>
#include <GL/glu.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>

#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "opengl32.lib")
#pragma comment(lib, "glu32.lib")

// ==================== 配置 ====================
#define DEFAULT_SERVER_IP "192.168.100.1"
#define TOUCH_PORT      9000
#define VIDEO_PORT      9999
#define AUDIO_PORT      9998
#define XY_SWAP_MODE    0

#define AUDIO_BUFFER_SAMPLES 4096
#define AUDIO_BUFFER_SIZE    (AUDIO_BUFFER_SAMPLES * 2 * 2) // 16384
#define VIDEO_BUFFER_SIZE    (1024 * 1024 * 6)

// ==================== 数据结构 ====================
typedef struct {
    int id;
    int x;
    int y;
    int active;   // 0=按下 1=移动 2=抬起
} TouchPoint;

typedef struct {
    int keyCode;
    int active;
} KeyPoint;

typedef struct {
    int w, a, s, d;
} WasdState;

// ==================== 全局变量 ====================
static volatile int running = 1;
static SOCKET touch_socket = INVALID_SOCKET;
static SOCKET video_fd = INVALID_SOCKET;
static SOCKET audio_fd = INVALID_SOCKET;
static char server_ip[32] = DEFAULT_SERVER_IP;
static HWND g_hWnd = NULL;
static HDC g_hDC = NULL;
static HGLRC g_hRC = NULL;

// 视频相关
static int SERVER_WIDTH = 0;
static int SERVER_HEIGHT = 0;
static GLuint g_texId = 0;
static void *g_pixelData = NULL;
static CRITICAL_SECTION g_csBitmap;

// 音频相关
static HWAVEOUT g_hWaveOut = NULL;
static WAVEHDR g_waveHdr[2] = {0};
static volatile int g_audioBufferReady[2] = {1, 1};
static HANDLE g_hAudioEvent = NULL;

// 鼠标/触摸
static int mouse_pressed = 0;
static const int mouse_id = 0;

// WASD 模拟触摸
static int start_wasd_x = 371;
static int start_wasd_y = 863;
static int wsad_count = 0;
static const int wasd_id = 1;
static WasdState wasd_state = {0,0,0,0};
static int wasd_x = 371;
static int wasd_y = 863;

static int frame_in = 0, frame_out = 0;

// ==================== 函数声明 ====================
LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
SOCKET tcp_connect(const char *ip, int port);
int read_(SOCKET fd, char *buf, size_t size, int max_size);
int readyz(SOCKET fd, char *buf, int size_max);
void send_touch_down(int id, int x, int y);
void send_touch_move(int id, int x, int y);
void send_touch_up(int id);
void send_key_event(int linux_keycode, int pressed);
int WinVKToLinuxKeycode(WPARAM vk);
int map_x(int winX, int winY);
int map_y(int winX, int winY);
DWORD WINAPI video_thread_func(LPVOID arg);
DWORD WINAPI audio_thread_func(LPVOID arg);
void InitOpenGL(int width, int height);
void ResizeOpenGL(int width, int height);
void RenderFrame(void);
void UpdateTexture(void *pixels, int width, int height);
void CleanupOpenGL(void);

// ==================== 网络函数 ====================
SOCKET tcp_connect(const char *ip, int port) {
    SOCKET sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == INVALID_SOCKET) {
        printf("[-] Socket 创建失败: %d\n", WSAGetLastError());
        return INVALID_SOCKET;
    }
    int recv_bufsize = 512 * 1024;
    setsockopt(sock, SOL_SOCKET, SO_RCVBUF, (char*)&recv_bufsize, sizeof(recv_bufsize));

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
    int mx = (id == mouse_id) ? x : x;
    int my = (id == mouse_id) ? y : y;
    if (mx < 0 || my < 0) return;
    TouchPoint tp = { id, mx, my, 0 };
    int sz = sizeof(tp);
    send(touch_socket, (char*)&sz, sizeof(int), 0);
    send(touch_socket, (char*)&tp, sz, 0);
}

void send_touch_move(int id, int x, int y) {
    int mx = (id == mouse_id) ? x : x;
    int my = (id == mouse_id) ? y : y;
    if (mx < 0 || my < 0) return;
    TouchPoint tp = { id, mx, my, 1 };
    int sz = sizeof(tp);
    send(touch_socket, (char*)&sz, sizeof(int), 0);
    send(touch_socket, (char*)&tp, sz, 0);
}

void send_touch_up(int id) {
    TouchPoint tp = { id, 0, 0, 2 };
    int sz = sizeof(tp);
    send(touch_socket, (char*)&sz, sizeof(int), 0);
    send(touch_socket, (char*)&tp, sz, 0);
}

void send_key_event(int linux_keycode, int pressed) {
    KeyPoint kp = { linux_keycode, pressed };
    int sz = sizeof(kp);
    send(touch_socket, (char*)&sz, sizeof(int), 0);
    send(touch_socket, (char*)&kp, sz, 0);
}

// ==================== 直接 Windows 虚拟键码 -> Linux 键码 ====================
int WinVKToLinuxKeycode(WPARAM vk) {
    switch (vk) {
        case VK_BACK:      return 14;   // KEY_BACKSPACE
        case VK_TAB:       return 15;   // KEY_TAB
        case VK_RETURN:    return 28;   // KEY_ENTER
        case VK_SHIFT:     return 42;   // KEY_LEFTSHIFT
        case VK_CONTROL:   return 29;   // KEY_LEFTCTRL
        case VK_MENU:      return 56;   // KEY_LEFTALT
        case VK_CAPITAL:   return 58;   // KEY_CAPSLOCK
        case VK_ESCAPE:    return 1;    // KEY_ESC
        case VK_SPACE:     return 57;   // KEY_SPACE
        case VK_LEFT:      return 105;  // KEY_LEFT
        case VK_UP:        return 103;  // KEY_UP
        case VK_RIGHT:     return 106;  // KEY_RIGHT
        case VK_DOWN:      return 108;  // KEY_DOWN
        case VK_HOME:      return 102;  // KEY_HOME
        case VK_END:       return 107;  // KEY_END
        case VK_PRIOR:     return 104;  // KEY_PAGEUP
        case VK_NEXT:      return 109;  // KEY_PAGEDOWN
        case VK_INSERT:    return 110;  // KEY_INSERT
        case VK_DELETE:    return 111;  // KEY_DELETE
        case VK_F1:        return 59;
        case VK_F2:        return 60;
        case VK_F3:        return 61;
        case VK_F4:        return 62;
        case VK_F5:        return 63;
        case VK_F6:        return 64;
        case VK_F7:        return 65;
        case VK_F8:        return 66;
        case VK_F9:        return 67;
        case VK_F10:       return 68;
        case VK_F11:       return 87;
        case VK_F12:       return 88;
        case '0': return 11; case '1': return 2;  case '2': return 3;
        case '3': return 4;  case '4': return 5;  case '5': return 6;
        case '6': return 7;  case '7': return 8;  case '8': return 9;
        case '9': return 10;
        case 'A': return 30; case 'B': return 48; case 'C': return 46;
        case 'D': return 32; case 'E': return 18; case 'F': return 33;
        case 'G': return 34; case 'H': return 35; case 'I': return 23;
        case 'J': return 36; case 'K': return 37; case 'L': return 38;
        case 'M': return 50; case 'N': return 49; case 'O': return 24;
        case 'P': return 25; case 'Q': return 16; case 'R': return 19;
        case 'S': return 31; case 'T': return 20; case 'U': return 22;
        case 'V': return 47; case 'W': return 17; case 'X': return 45;
        case 'Y': return 21; case 'Z': return 44;
        case VK_OEM_MINUS: return 12;   // KEY_MINUS
        case VK_OEM_PLUS:  return 13;   // KEY_EQUAL
        case VK_OEM_4:     return 26;   // KEY_LEFTBRACE
        case VK_OEM_6:     return 27;   // KEY_RIGHTBRACE
        case VK_OEM_5:     return 43;   // KEY_BACKSLASH
        case VK_OEM_1:     return 39;   // KEY_SEMICOLON
        case VK_OEM_7:     return 40;   // KEY_APOSTROPHE
        case VK_OEM_COMMA: return 51;   // KEY_COMMA
        case VK_OEM_PERIOD:return 52;   // KEY_DOT
        case VK_OEM_2:     return 53;   // KEY_SLASH
        default: return 0;
    }
}

// ==================== 坐标映射（窗口坐标 -> 服务器坐标）====================
// 由于视频被拉伸填满整个客户区，所以直接线性缩放即可
int map_x(int winX, int winY) {
    (void)winY; // 未使用
    RECT client;
    GetClientRect(g_hWnd, &client);
    int clientW = client.right - client.left;
    if (clientW <= 0) return -1;
    int sx = winX * SERVER_WIDTH / clientW;
    return sx;
}

int map_y(int winX, int winY) {
    (void)winX;
    RECT client;
    GetClientRect(g_hWnd, &client);
    int clientH = client.bottom - client.top;
    if (clientH <= 0) return -1;
    int sy = winY * SERVER_HEIGHT / clientH;
    return sy;
}

// ==================== OpenGL 初始化与渲染 ====================
void InitOpenGL(int width, int height) {
    g_hDC = GetDC(g_hWnd);
    PIXELFORMATDESCRIPTOR pfd = {
        sizeof(PIXELFORMATDESCRIPTOR), 1,
        PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER,
        PFD_TYPE_RGBA, 32, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        16, 0, 0, PFD_MAIN_PLANE, 0, 0, 0, 0
    };
    int pixelFormat = ChoosePixelFormat(g_hDC, &pfd);
    SetPixelFormat(g_hDC, pixelFormat, &pfd);
    g_hRC = wglCreateContext(g_hDC);
    wglMakeCurrent(g_hDC, g_hRC);

    glEnable(GL_TEXTURE_2D);
    glGenTextures(1, &g_texId);
    glBindTexture(GL_TEXTURE_2D, g_texId);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0,
                 GL_BGRA_EXT, GL_UNSIGNED_BYTE, NULL);
    glBindTexture(GL_TEXTURE_2D, 0);

    ResizeOpenGL(width, height);
}

void ResizeOpenGL(int width, int height) {
    if (!g_hRC) return;
    glViewport(0, 0, width, height);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, width, height, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
}

void UpdateTexture(void *pixels, int width, int height) {
    glBindTexture(GL_TEXTURE_2D, g_texId);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height,
                    GL_BGRA_EXT, GL_UNSIGNED_BYTE, pixels);
    glBindTexture(GL_TEXTURE_2D, 0);
}

void RenderFrame() {
    if (!g_hDC || !g_hRC) return;
    // 获取当前窗口客户区大小
    RECT clientRect;
    GetClientRect(g_hWnd, &clientRect);
    int winWidth = clientRect.right - clientRect.left;
    int winHeight = clientRect.bottom - clientRect.top;
    if (winWidth <= 0 || winHeight <= 0) return;

    glClear(GL_COLOR_BUFFER_BIT);
    glBindTexture(GL_TEXTURE_2D, g_texId);
    glEnable(GL_TEXTURE_2D);
    glBegin(GL_QUADS);
        glTexCoord2f(0.0f, 0.0f); glVertex2f(0.0f, 0.0f);
        glTexCoord2f(1.0f, 0.0f); glVertex2f((float)winWidth, 0.0f);
        glTexCoord2f(1.0f, 1.0f); glVertex2f((float)winWidth, (float)winHeight);
        glTexCoord2f(0.0f, 1.0f); glVertex2f(0.0f, (float)winHeight);
    glEnd();
    glDisable(GL_TEXTURE_2D);
    SwapBuffers(g_hDC);
}

void CleanupOpenGL() {
    if (g_texId) glDeleteTextures(1, &g_texId);
    if (g_hRC) {
        wglMakeCurrent(NULL, NULL);
        wglDeleteContext(g_hRC);
    }
    if (g_hDC) ReleaseDC(g_hWnd, g_hDC);
}

// ==================== 音频回调 ====================
void CALLBACK audio_callback(HWAVEOUT hwo, UINT uMsg, DWORD_PTR dwInstance, DWORD_PTR dwParam1, DWORD_PTR dwParam2) {
    if (uMsg == WOM_DONE) {
        WAVEHDR *hdr = (WAVEHDR*)dwParam1;
        waveOutUnprepareHeader(hwo, hdr, sizeof(WAVEHDR));
        int idx = (hdr->dwUser == 0) ? 0 : 1;
        g_audioBufferReady[idx] = 1;
        SetEvent(g_hAudioEvent);
    }
}

// ==================== 音频线程 ====================
DWORD WINAPI audio_thread_func(LPVOID arg) {
    printf("[+] 音频线程启动\n");
    char header[69];
    if (read_(audio_fd, header, 69, 69) != 69) {
        printf("[-] 音频头读取失败\n");
        running = 0;
        PostMessage(g_hWnd, WM_CLOSE, 0, 0);
        return 1;
    }
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
        PostMessage(g_hWnd, WM_CLOSE, 0, 0);
        return 1;
    }
    uint8_t *buf1 = (uint8_t*)malloc(AUDIO_BUFFER_SIZE);
    uint8_t *buf2 = (uint8_t*)malloc(AUDIO_BUFFER_SIZE);
    if (!buf1 || !buf2) {
        free(buf1); free(buf2);
        waveOutClose(g_hWaveOut);
        running = 0;
        PostMessage(g_hWnd, WM_CLOSE, 0, 0);
        return 1;
    }
    g_waveHdr[0].lpData = (LPSTR)buf1;
    g_waveHdr[0].dwBufferLength = AUDIO_BUFFER_SIZE;
    g_waveHdr[0].dwUser = 0;
    g_waveHdr[1].lpData = (LPSTR)buf2;
    g_waveHdr[1].dwBufferLength = AUDIO_BUFFER_SIZE;
    g_waveHdr[1].dwUser = 1;
    for (int i = 0; i < 2; i++) {
        int sz = readyz(audio_fd, (char*)g_waveHdr[i].lpData, (int)g_waveHdr[i].dwBufferLength);
        if (sz <= 0) {
            free(buf1); free(buf2);
            waveOutClose(g_hWaveOut);
            running = 0;
            PostMessage(g_hWnd, WM_CLOSE, 0, 0);
            return 1;
        }
        g_waveHdr[i].dwBufferLength = sz;
        waveOutPrepareHeader(g_hWaveOut, &g_waveHdr[i], sizeof(WAVEHDR));
        waveOutWrite(g_hWaveOut, &g_waveHdr[i], sizeof(WAVEHDR));
        g_audioBufferReady[i] = 0;
    }
    while (running) {
        WaitForSingleObject(g_hAudioEvent, INFINITE);
        int idx = -1;
        for (int i = 0; i < 2; i++) {
            if (g_audioBufferReady[i]) { idx = i; break; }
        }
        if (idx == -1) continue;
        int sz = readyz(audio_fd, (char*)g_waveHdr[idx].lpData, (int)g_waveHdr[idx].dwBufferLength);
        if (sz <= 0) break;
        g_waveHdr[idx].dwBufferLength = sz;
        waveOutPrepareHeader(g_hWaveOut, &g_waveHdr[idx], sizeof(WAVEHDR));
        waveOutWrite(g_hWaveOut, &g_waveHdr[idx], sizeof(WAVEHDR));
        g_audioBufferReady[idx] = 0;
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
DWORD WINAPI video_thread_func(LPVOID arg) {
    printf("[+] 视频线程启动\n");
    char header[69];
    if (read_(video_fd, header, 69, 69) != 69) {
        printf("[-] 视频头读取失败\n");
        running = 0;
        PostMessage(g_hWnd, WM_CLOSE, 0, 0);
        return 1;
    }
    int w, h;
    read_(video_fd, (char*)&w, 4, 69);
    read_(video_fd, (char*)&h, 4, 69);
    SERVER_WIDTH = ntohl(w);
    SERVER_HEIGHT = ntohl(h);
    printf("[+] 服务端分辨率: %dx%d\n", SERVER_WIDTH, SERVER_HEIGHT);
    SetWindowPos(g_hWnd, NULL, 0, 0, SERVER_WIDTH, SERVER_HEIGHT, SWP_NOMOVE | SWP_NOZORDER);
    UpdateWindow(g_hWnd);
    InitOpenGL(SERVER_WIDTH, SERVER_HEIGHT);

    int pixelSize = SERVER_WIDTH * SERVER_HEIGHT * 4;
    g_pixelData = malloc(pixelSize);
    if (!g_pixelData) {
        printf("[-] 分配像素缓冲区失败\n");
        running = 0;
        PostMessage(g_hWnd, WM_CLOSE, 0, 0);
        return 1;
    }
    memset(g_pixelData, 0, pixelSize);

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
    struct SwsContext *sws_ctx = sws_getContext(SERVER_WIDTH, SERVER_HEIGHT, AV_PIX_FMT_YUV420P,
                                                SERVER_WIDTH, SERVER_HEIGHT, AV_PIX_FMT_RGB32,
                                                SWS_BILINEAR, NULL, NULL, NULL);
    if (!sws_ctx) {
        printf("[-] 初始化颜色转换失败\n");
        running = 0;
        return 1;
    }
    uint8_t *video_buffer = (uint8_t*)malloc(VIDEO_BUFFER_SIZE);
    AVPacket *pkt = av_packet_alloc();
    while (running) {
        int sz = readyz(video_fd, (char*)video_buffer, VIDEO_BUFFER_SIZE);
        if (sz <= 0) break;
        pkt->data = video_buffer;
        pkt->size = sz;
        if (avcodec_send_packet(codec_ctx, pkt) < 0) continue;
        frame_in++;
        while (avcodec_receive_frame(codec_ctx, frame) == 0) {
            EnterCriticalSection(&g_csBitmap);
            uint8_t *dest[4] = { (uint8_t*)g_pixelData, NULL, NULL, NULL };
            int linesize[4] = { SERVER_WIDTH * 4, 0, 0, 0 };
            sws_scale(sws_ctx, (const uint8_t* const*)frame->data, frame->linesize,
                      0, SERVER_HEIGHT, dest, linesize);
            LeaveCriticalSection(&g_csBitmap);
            UpdateTexture(g_pixelData, SERVER_WIDTH, SERVER_HEIGHT);
            RenderFrame();
            frame_out++;
        }
    }
    free(video_buffer);
    av_packet_free(&pkt);
    av_frame_free(&frame);
    avcodec_free_context(&codec_ctx);
    sws_freeContext(sws_ctx);
    free(g_pixelData);
    CleanupOpenGL();
    closesocket(video_fd);
    video_fd = INVALID_SOCKET;
    running = 0;
    printf("[+] 视频线程退出, 总帧数: %d\n", frame_out);
    return 0;
}

// ==================== 窗口过程 ====================
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_SIZE: {
            int width = LOWORD(lParam);
            int height = HIWORD(lParam);
            if (g_hRC && width > 0 && height > 0) {
                ResizeOpenGL(width, height);
                // 立即重绘一帧以适应新大小
                RenderFrame();
            }
            break;
        }
        case WM_LBUTTONDOWN: {
            int x = GET_X_LPARAM(lParam);
            int y = GET_Y_LPARAM(lParam);
            if (!mouse_pressed) {
                mouse_pressed = 1;
                send_touch_down(mouse_id, map_x(x, y), map_y(x, y));
            }
            break;
        }
        case WM_MOUSEMOVE:
            if (mouse_pressed) {
                int x = GET_X_LPARAM(lParam);
                int y = GET_Y_LPARAM(lParam);
                send_touch_move(mouse_id, map_x(x, y), map_y(x, y));
            }
            break;
        case WM_LBUTTONUP:
            if (mouse_pressed) {
                mouse_pressed = 0;
                send_touch_up(mouse_id);
            }
            break;
        case WM_RBUTTONDOWN:
            send_key_event(158, 1); send_key_event(158, 0);
            break;
        case WM_MBUTTONDOWN:
            send_key_event(102, 1); send_key_event(102, 0);
            break;
        case WM_MOUSEWHEEL: {
            short delta = GET_WHEEL_DELTA_WPARAM(wParam);
            if (delta > 0) {
                send_key_event(103, 1); send_key_event(103, 0);
            } else {
                send_key_event(108, 1); send_key_event(108, 0);
            }
            break;
        }
        case WM_KEYDOWN: {
            int linuxKey = WinVKToLinuxKeycode(wParam);
            if (linuxKey) {
                int changed = 0;
                switch (wParam) {
                    case 'W': if (!wasd_state.w) { wasd_state.w=1; wsad_count++; wasd_y-=100; changed=1; } break;
                    case 'A': if (!wasd_state.a) { wasd_state.a=1; wsad_count++; wasd_x-=100; changed=1; } break;
                    case 'S': if (!wasd_state.s) { wasd_state.s=1; wsad_count++; wasd_y+=100; changed=1; } break;
                    case 'D': if (!wasd_state.d) { wasd_state.d=1; wsad_count++; wasd_x+=100; changed=1; } break;
                }
                if (changed) {
                    if (wsad_count == 1)
                        send_touch_down(wasd_id, start_wasd_x, start_wasd_y);
                    send_touch_move(wasd_id, wasd_x, wasd_y);
                }
                send_key_event(linuxKey, 1);
            }
            if (wParam == VK_ESCAPE || wParam == 'Q') running = 0;
            break;
        }
        case WM_KEYUP: {
            int linuxKey = WinVKToLinuxKeycode(wParam);
            if (linuxKey) {
                int changed = 0;
                switch (wParam) {
                    case 'W': if (wasd_state.w) { wasd_state.w=0; wsad_count--; wasd_y+=100; changed=1; } break;
                    case 'A': if (wasd_state.a) { wasd_state.a=0; wsad_count--; wasd_x+=100; changed=1; } break;
                    case 'S': if (wasd_state.s) { wasd_state.s=0; wsad_count--; wasd_y-=100; changed=1; } break;
                    case 'D': if (wasd_state.d) { wasd_state.d=0; wsad_count--; wasd_x-=100; changed=1; } break;
                }
                if (changed) {
                    send_touch_move(wasd_id, wasd_x, wasd_y);
                    if (wsad_count == 0) send_touch_up(wasd_id);
                }
                send_key_event(linuxKey, 0);
            }
            break;
        }
        case WM_CLOSE:
            running = 0;
            DestroyWindow(hwnd);
            break;
        case WM_DESTROY:
            PostQuitMessage(0);
            break;
        default:
            return DefWindowProc(hwnd, msg, wParam, lParam);
    }
    return 0;
}

// ==================== 主函数 ====================
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    if (strlen(lpCmdLine) > 0) {
        strncpy(server_ip, lpCmdLine, sizeof(server_ip)-1);
        server_ip[sizeof(server_ip)-1] = 0;
    }
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2,2), &wsaData) != 0) {
        MessageBox(NULL, "WSAStartup 失败", "错误", MB_OK);
        return 1;
    }
    touch_socket = tcp_connect(server_ip, TOUCH_PORT);
    video_fd = tcp_connect(server_ip, VIDEO_PORT);
    audio_fd = tcp_connect(server_ip, AUDIO_PORT);
    if (touch_socket == INVALID_SOCKET || video_fd == INVALID_SOCKET || audio_fd == INVALID_SOCKET) {
        MessageBox(NULL, "无法连接到服务器", "错误", MB_OK);
        goto cleanup;
    }
    WNDCLASSEX wc = {0};
    wc.cbSize = sizeof(WNDCLASSEX);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = "RemoteReceiverClass";
    if (!RegisterClassEx(&wc)) {
        MessageBox(NULL, "窗口注册失败", "错误", MB_OK);
        goto cleanup;
    }
    g_hWnd = CreateWindowEx(0, "RemoteReceiverClass", "OpenGL Receiver - Screen Mirror",
                            WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                            800, 600, NULL, NULL, hInstance, NULL);
    if (!g_hWnd) {
        MessageBox(NULL, "窗口创建失败", "错误", MB_OK);
        goto cleanup;
    }
    ShowWindow(g_hWnd, nCmdShow);
    UpdateWindow(g_hWnd);
    InitializeCriticalSection(&g_csBitmap);
    g_hAudioEvent = CreateEvent(NULL, FALSE, FALSE, NULL);

    HANDLE hVideo = CreateThread(NULL, 0, video_thread_func, NULL, 0, NULL);
    HANDLE hAudio = CreateThread(NULL, 0, audio_thread_func, NULL, 0, NULL);
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
    CloseHandle(g_hAudioEvent);

cleanup:
    if (touch_socket != INVALID_SOCKET) closesocket(touch_socket);
    if (video_fd != INVALID_SOCKET) closesocket(video_fd);
    if (audio_fd != INVALID_SOCKET) closesocket(audio_fd);
    WSACleanup();
    DeleteCriticalSection(&g_csBitmap);
    return 0;
}