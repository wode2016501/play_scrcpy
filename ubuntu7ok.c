// ubuntu_receiver_final.c
// 编译: gcc -o ubuntu_receiver ubuntu_receiver_final.c -lSDL2 -lpthread -lavcodec -lavutil -lswscale -lz
// 运行: ./ubuntu_receiver 192.168.100.1

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <linux/input.h>
#include <SDL2/SDL.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <fcntl.h>
#include <libswscale/swscale.h>

// ==================== 配置 ====================
#define DEFAULT_SERVER_IP "192.168.100.1"
#define TOUCH_PORT 9000
#define VIDEO_PORT 9999
#define AUDIO_PORT 9998

static int running = 1;
static int touch_socket = -1;
static char server_ip[32] = DEFAULT_SERVER_IP;
static pthread_t video_thread, audio_thread ,event_thread;

static SDL_Window* window = NULL;
static SDL_Renderer* renderer = NULL;
static SDL_Texture* texture = NULL;
static SDL_AudioDeviceID audio_device = 0;

// 分辨率
static int SERVER_WIDTH = 0;
static int SERVER_HEIGHT = 0;
static int WINDOW_WIDTH = 0;
static int WINDOW_HEIGHT = 0;

// 鼠标状态
static int mouse_pressed = 0;
static int mouse_id = 0;

// XY转换模式
#define XY_SWAP_MODE 0

// 信号处理
void signal_handler(int sig) {
    printf("\n[!] 收到信号 %d，正在退出...\n", sig);
    running = 0;
}

// 坐标转换 (窗口坐标 -> 服务器坐标)
int map_x(int window_x, int window_y) {
    float window_aspect = (float)WINDOW_WIDTH / WINDOW_HEIGHT;
    float video_aspect = (float)SERVER_WIDTH / SERVER_HEIGHT;
    
    int video_display_width, video_display_height;
    int offset_x = 0, offset_y = 0;
    
    if (window_aspect > video_aspect) {
        video_display_height = WINDOW_HEIGHT;
        video_display_width = (int)(video_display_height * video_aspect);
        offset_x = (WINDOW_WIDTH - video_display_width) / 2;
    } else {
        video_display_width = WINDOW_WIDTH;
        video_display_height = (int)(video_display_width / video_aspect);
        offset_y = (WINDOW_HEIGHT - video_display_height) / 2;
    }
    
    int video_x = window_x - offset_x;
    int video_y = window_y - offset_y;
    
    if (video_x < 0 || video_x >= video_display_width || 
        video_y < 0 || video_y >= video_display_height) {
        return -1;
    }
    
    int server_x = video_x * SERVER_WIDTH / video_display_width;
    int server_y = video_y * SERVER_HEIGHT / video_display_height;
    
    if (XY_SWAP_MODE == 1) {
        int temp = server_x;
        server_x = server_y;
        server_y = temp;
    }
    
    return server_x;
}

int map_y(int window_x, int window_y) {
    float window_aspect = (float)WINDOW_WIDTH / WINDOW_HEIGHT;
    float video_aspect = (float)SERVER_WIDTH / SERVER_HEIGHT;
    
    int video_display_width, video_display_height;
    int offset_x = 0, offset_y = 0;
    
    if (window_aspect > video_aspect) {
        video_display_height = WINDOW_HEIGHT;
        video_display_width = (int)(video_display_height * video_aspect);
        offset_x = (WINDOW_WIDTH - video_display_width) / 2;
    } else {
        video_display_width = WINDOW_WIDTH;
        video_display_height = (int)(video_display_width / video_aspect);
        offset_y = (WINDOW_HEIGHT - video_display_height) / 2;
    }
    
    int video_x = window_x - offset_x;
    int video_y = window_y - offset_y;
    
    if (video_x < 0 || video_x >= video_display_width || 
        video_y < 0 || video_y >= video_display_height) {
        return -1;
    }
    
    int server_x = video_x * SERVER_WIDTH / video_display_width;
    int server_y = video_y * SERVER_HEIGHT / video_display_height;
    
    if (XY_SWAP_MODE == 1) {
        int temp = server_x;
        server_x = server_y;
        server_y = temp;
    }
    
    return server_y;
}

// 输入事件结构体
typedef struct {
    long long tv_sec;
    long long tv_usec;
    unsigned short type;
    unsigned short code;
    unsigned int value;
} input_event_test;

// ==================== 读取函数 ====================
int read_(int fd, char* buf, size_t size, int max_size) {
    if (size > max_size) return -1;
    int y = size;
    int ret = 0;
    while (y > 0) {
        ret = read(fd, buf, y);
        if (ret < 0) return -1;
        if (ret == 0) return -1;
        buf += ret;
        y -= ret;
    }
    return size;
}

int readyz(int fd, char* buf, int size_max) {
    long long pts;
    int ret = read_(fd, (char*)&pts, 8, size_max);
    if (ret != 8) return -1;
    
    int size;
    ret = read_(fd, (char*)&size, 4, size_max);
    if (ret != 4) return -1;
   
    size = ntohl(size);
    if (size > 0 && size <= size_max) {
        ret = read_(fd, buf, size, size_max);
        if (ret == size) return size;
    }
    return -1;
}

// ==================== 触摸发送函数 ====================
int send_input_event(int type, int code, int value) {
    if (touch_socket < 0) {
        touch_socket = socket(AF_INET, SOCK_STREAM, 0);
        if (touch_socket < 0) return -1;
        
        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(TOUCH_PORT);
        addr.sin_addr.s_addr = inet_addr(server_ip);
        
        if (connect(touch_socket, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            perror("connect touch");
            close(touch_socket);
            touch_socket = -1;
            return -1;
        }
        printf("[+] 触摸服务器已连接\n");
    }
    
    input_event_test ev;
    struct timeval tv;
    gettimeofday(&tv, NULL);
    
    ev.tv_sec = tv.tv_sec;
    ev.tv_usec = tv.tv_usec;
    ev.type = type;
    ev.code = code;
    ev.value = value;
    
    ssize_t ret = send(touch_socket, &ev, sizeof(ev), 0);
    if (ret != sizeof(ev)) {
        printf("[-] 发送触摸事件失败\n");
        close(touch_socket);
        touch_socket = -1;
        return -1;
    }
    return 0;
}

void send_touch_down(int x, int y) {
    int mapped_x = map_x(x, y);
    int mapped_y = map_y(x, y);
    
    if (mapped_x < 0 || mapped_y < 0) {
        printf("[触摸按下] 点击在视频区域外，忽略\n");
        return;
    }
    
    printf("[触摸按下] 窗口(%d,%d) -> 服务器(%d,%d)\n", x, y, mapped_x, mapped_y);
    
    send_input_event(EV_ABS, ABS_MT_SLOT, 0);
    send_input_event(EV_ABS, ABS_MT_TRACKING_ID, mouse_id);
    send_input_event(EV_ABS, ABS_MT_POSITION_X, mapped_x);
    send_input_event(EV_ABS, ABS_MT_POSITION_Y, mapped_y);
    send_input_event(EV_KEY, BTN_TOUCH, 1);
    send_input_event(EV_SYN, SYN_REPORT, 0);
}

void send_touch_move(int x, int y) {
    int mapped_x = map_x(x, y);
    int mapped_y = map_y(x, y);
    
    if (mapped_x < 0 || mapped_y < 0) return;
    printf("[触摸移动] 窗口(%d,%d) -> 服务器(%d,%d)\n", x, y, mapped_x, mapped_y);
    send_input_event(EV_ABS, ABS_MT_SLOT, 0);
    send_input_event(EV_ABS, ABS_MT_POSITION_X, mapped_x);
    send_input_event(EV_ABS, ABS_MT_POSITION_Y, mapped_y);
    send_input_event(EV_SYN, SYN_REPORT, 0);
}

void send_touch_up(void) {
    printf("[触摸抬起]\n");
    
    send_input_event(EV_ABS, ABS_MT_SLOT, 0);
    send_input_event(EV_ABS, ABS_MT_TRACKING_ID, -1);
    send_input_event(EV_KEY, BTN_TOUCH, 0);
    send_input_event(EV_SYN, SYN_REPORT, 0);
}

// 发送按键事件
void send_key_event(int linux_keycode, int pressed) {
    send_input_event(EV_KEY, linux_keycode, pressed ? 1 : 0);
    send_input_event(EV_SYN, SYN_REPORT, 0);
}


void* event_thread_func(void* arg) {
            touch_socket = socket(AF_INET, SOCK_STREAM, 0);
        if (touch_socket < 0) return -1;
        
        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(TOUCH_PORT);
        addr.sin_addr.s_addr = inet_addr(server_ip);
        
        if (connect(touch_socket, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            perror("connect touch");
            close(touch_socket);
            touch_socket = -1;
            return -1;
        }
        printf("[+] 触摸服务器已连接\n");
            // 处理SDL事件（鼠标和窗口）- 保持原有逻辑，只添加鼠标按键
        SDL_Event event;
        while (SDL_WaitEvent(&event)&& running) {
            switch (event.type) {
                case SDL_QUIT:
                    running = 0;
                    break;
                    
                case SDL_WINDOWEVENT:
                    if (event.window.event == SDL_WINDOWEVENT_RESIZED) {
                        WINDOW_WIDTH = event.window.data1;
                        WINDOW_HEIGHT = event.window.data2;
                    }
                    break;
                    
                case SDL_MOUSEBUTTONDOWN:
                    if (event.button.button == SDL_BUTTON_LEFT) {
                        if (!mouse_pressed) {
                            mouse_pressed = 1;
                            send_touch_down(event.button.x, event.button.y);
                        }
                    } else if (event.button.button == SDL_BUTTON_RIGHT) {
                        // 右键：返回键
                        printf("[右键] 返回\n");
                        send_key_event(KEY_BACK, 1);
                       // usleep(20000);
                        send_key_event(KEY_BACK, 0);
                    } else if (event.button.button == SDL_BUTTON_MIDDLE) {
                        // 中键：Home键
                        printf("[中键] Home\n");
                        send_key_event(KEY_HOME, 1);
                        //usleep(20000);
                        send_key_event(KEY_HOME, 0);
                    }
                    break;
                    
                case SDL_MOUSEBUTTONUP:
                    if (event.button.button == SDL_BUTTON_LEFT) {
                        if (mouse_pressed) {
                            mouse_pressed = 0;
                            send_touch_up();
                        }
                    }
                    break;
                    
                case SDL_MOUSEMOTION:
                    if (mouse_pressed) {
                        send_touch_move(event.motion.x, event.motion.y);
                    }
                    break;
                    
                case SDL_MOUSEWHEEL:
                    // 滚轮：上下滚动
                    printf("[滚轮] %s\n", event.wheel.y > 0 ? "向上" : "向下");
                    if (event.wheel.y > 0) {
                        send_key_event(KEY_UP, 1);
                        //usleep(10000);
                        send_key_event(KEY_UP, 0);
                    } else {
                        send_key_event(KEY_DOWN, 1);
                       // usleep(10000);
                        send_key_event(KEY_DOWN, 0);
                    }
                    break;
                    
                case SDL_KEYDOWN:
                    if (event.key.keysym.sym == SDLK_ESCAPE ||
                        event.key.keysym.sym == SDLK_q) {
                        running = 0;
                    }
                    break;
            }
        }
        printf("[+] 事件线程退出\n");
}


// ==================== 视频接收线程 ====================
void* video_thread_func(void* arg) {
    printf("[+] 视频线程启动\n");
    
    // 连接视频服务器
    int video_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (video_fd < 0) {
        perror("socket");
        return NULL;
    }
    
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(VIDEO_PORT);
    addr.sin_addr.s_addr = inet_addr(server_ip);
    
    if (connect(video_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("connect video");
        close(video_fd);
        return NULL;
    }
    printf("[+] 视频服务器已连接\n");
    
    // 读取视频头 (69字节)
    char header[69];
    if (read_(video_fd, header, 69, 69) != 69) {
        printf("[-] 读取视频头失败\n");
        close(video_fd);
        return NULL;
    }
    
    // 读取分辨率
    int width, height;
    read_(video_fd, (char*)&width, 4, 69);
    read_(video_fd, (char*)&height, 4, 69);
    SERVER_WIDTH = ntohl(width);
    SERVER_HEIGHT = ntohl(height);
    
    // 窗口大小 = 服务端分辨率
    WINDOW_WIDTH = SERVER_WIDTH;
    WINDOW_HEIGHT = SERVER_HEIGHT;
    
    printf("[+] 服务端分辨率: %dx%d\n", SERVER_WIDTH, SERVER_HEIGHT);
    printf("[+] 窗口初始大小: %dx%d\n", WINDOW_WIDTH, WINDOW_HEIGHT);
    printf("[+] XY转换模式: %d\n", XY_SWAP_MODE);
    
    // 初始化SDL
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) < 0) {
        printf("[-] SDL初始化失败: %s\n", SDL_GetError());
        close(video_fd);
        return NULL;
    }
    
    // 创建窗口（可缩放）
    window = SDL_CreateWindow("Ubuntu Receiver - Screen Mirror",
                              SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
                              WINDOW_WIDTH, WINDOW_HEIGHT,
                              SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
    if (!window) {
        printf("[-] 创建窗口失败: %s\n", SDL_GetError());
        close(video_fd);
        return NULL;
    }
    printf("[+] 窗口创建成功（支持缩放和拖动）\n");
    
    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    if (!renderer) {
        printf("[-] 创建渲染器失败: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        close(video_fd);
        return NULL;
    }
    
    // 初始化FFmpeg解码器
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!codec) {
        printf("[-] 找不到H.264解码器\n");
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        close(video_fd);
        return NULL;
    }
    printf("[+] 找到H.264解码器: %s\n", codec->name);
    
    AVCodecContext* codec_ctx = avcodec_alloc_context3(codec);
    if (!codec_ctx) {
        printf("[-] 分配解码器上下文失败\n");
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        close(video_fd);
        return NULL;
    }
    
    codec_ctx->width = SERVER_WIDTH;
    codec_ctx->height = SERVER_HEIGHT;
    codec_ctx->pix_fmt = AV_PIX_FMT_YUV420P;
    codec_ctx->thread_count = 4;
    
    // 打开解码器
    if (avcodec_open2(codec_ctx, codec, NULL) < 0) {
        printf("[-] 打开解码器失败\n");
        avcodec_free_context(&codec_ctx);
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        close(video_fd);
        return NULL;
    }
    printf("[+] 解码器打开成功\n");
    
    AVFrame* frame = av_frame_alloc();
    AVFrame* rgb_frame = av_frame_alloc();
    struct SwsContext* sws_ctx = sws_getContext(SERVER_WIDTH, SERVER_HEIGHT, AV_PIX_FMT_YUV420P,
                                                SERVER_WIDTH, SERVER_HEIGHT, AV_PIX_FMT_RGB32,
                                                SWS_BILINEAR, NULL, NULL, NULL);
    
    if (!sws_ctx) {
        printf("[-] 创建图像转换器失败\n");
        av_frame_free(&frame);
        av_frame_free(&rgb_frame);
        avcodec_free_context(&codec_ctx);
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        close(video_fd);
        return NULL;
    }
    
    texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
                                SDL_TEXTUREACCESS_STREAMING, SERVER_WIDTH, SERVER_HEIGHT);
    if (!texture) {
        printf("[-] 创建纹理失败: %s\n", SDL_GetError());
        sws_freeContext(sws_ctx);
        av_frame_free(&frame);
        av_frame_free(&rgb_frame);
        avcodec_free_context(&codec_ctx);
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        close(video_fd);
        return NULL;
    }
    
    int buffer_size = 1024 * 1024 * 6;  // 6MB缓冲区
    uint8_t* video_buffer = (uint8_t*)malloc(buffer_size);
    uint8_t* rgb_buffer = (uint8_t*)malloc(SERVER_WIDTH * SERVER_HEIGHT * 4);
    
    if (!video_buffer || !rgb_buffer) {
        printf("[-] 分配缓冲区失败\n");
        free(video_buffer);
        free(rgb_buffer);
        SDL_DestroyTexture(texture);
        sws_freeContext(sws_ctx);
        av_frame_free(&frame);
        av_frame_free(&rgb_frame);
        avcodec_free_context(&codec_ctx);
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        close(video_fd);
        return NULL;
    }
    
    av_image_fill_arrays(rgb_frame->data, rgb_frame->linesize, rgb_buffer,
                         AV_PIX_FMT_RGB32, SERVER_WIDTH, SERVER_HEIGHT, 1);
    
    printf("[+] 开始接收视频流...\n");
    printf("[!] 鼠标左键点击和拖动来控制\n");
    printf("[!] 鼠标右键=返回键 | 鼠标中键=Home键 | 滚轮=滚动\n");
    printf("[!] 按 ESC 或 Q 退出\n\n");
    
    AVPacket pkt;
    av_init_packet(&pkt);
    int frame_count = 0;
    int in_frame_count = 0;
    //int init=1;
        pthread_create(&event_thread, NULL, event_thread_func, NULL);
    while (running) {
        // 获取当前窗口大小
        int new_width, new_height;
        SDL_GetWindowSize(window, &new_width, &new_height);
        if (new_width != WINDOW_WIDTH || new_height != WINDOW_HEIGHT) {
            WINDOW_WIDTH = new_width;
            WINDOW_HEIGHT = new_height;
            printf("[窗口] 大小改变: %dx%d\n", WINDOW_WIDTH, WINDOW_HEIGHT);
        }
        
        // 接收视频数据
        int size = readyz(video_fd, (char*)video_buffer, buffer_size);
        if (size <= 0) {
            if (running) {
                printf("[-] 读取视频数据失败, size=%d\n", size);
                break;
            }
            continue;
        }

        in_frame_count++;
        
        // 解码
        pkt.data = video_buffer;
        pkt.size = size;
        pkt.pts = AV_NOPTS_VALUE;
        pkt.dts = AV_NOPTS_VALUE;
        int ret = avcodec_send_packet(codec_ctx, &pkt);
       // printf("[+] 发送包到解码器, 大小: %d bytes\n", size);
        if (ret < 0) {
            char errbuf[256];
            av_strerror(ret, errbuf, sizeof(errbuf));
             printf("[-] 发送包到解码器失败: %s\n", errbuf);
            continue;
        }
        
     while (1) {
            ret = avcodec_receive_frame(codec_ctx, frame);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
               // printf("+解码器需要更多的数据\n");
                break;
            } else if (ret < 0) {
                char errbuf[256];
                av_strerror(ret, errbuf, sizeof(errbuf));
                 printf("[-] 接收解码帧失败: %s\n", errbuf);
                break;
            }
            // 转换颜色空间
            sws_scale(sws_ctx, (const uint8_t* const*)frame->data, frame->linesize,
                     0, SERVER_HEIGHT, rgb_frame->data, rgb_frame->linesize);
            
            // 更新纹理
            SDL_UpdateTexture(texture, NULL, rgb_frame->data[0], rgb_frame->linesize[0]);
            
            // 清空渲染器
            SDL_RenderClear(renderer);
            
            // 计算显示区域（保持宽高比，居中显示）
            float window_aspect = (float)WINDOW_WIDTH / WINDOW_HEIGHT;
            float video_aspect = (float)SERVER_WIDTH / SERVER_HEIGHT;
            
            SDL_Rect dst_rect;
            if (window_aspect > video_aspect) {
                // 窗口更宽，上下黑边
                dst_rect.h = WINDOW_HEIGHT;
                dst_rect.w = (int)(dst_rect.h * video_aspect);
                dst_rect.x = (WINDOW_WIDTH - dst_rect.w) / 2;
                dst_rect.y = 0;
            } else {
                // 窗口更高，左右黑边
                dst_rect.w = WINDOW_WIDTH;
                dst_rect.h = (int)(dst_rect.w / video_aspect);
                dst_rect.x = 0;
                dst_rect.y = (WINDOW_HEIGHT - dst_rect.h) / 2;
            }
            
            SDL_RenderCopy(renderer, texture, NULL, &dst_rect);
            SDL_RenderPresent(renderer);
            frame_count++;
            /*
            if (frame_count % 30 == 0 && frame_count > 0) {
                printf("[视频] 已显示 %d 帧\n", frame_count);
            }*/
        }
        

    }
    
    // 清理资源
    printf("[+] 清理资源...\n");
    free(video_buffer);
    free(rgb_buffer);
    av_frame_free(&frame);
    av_frame_free(&rgb_frame);
    avcodec_free_context(&codec_ctx);
    sws_freeContext(sws_ctx);
    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    close(video_fd);
    
    printf("[+] 视频线程退出, 总帧数: %d\n", frame_count);
    return NULL;
}

// ==================== 音频接收线程 ====================
void* audio_thread_func(void* arg) {
    printf("[+] 音频线程启动\n");
    
    int audio_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (audio_fd < 0) {
        perror("socket");
        return NULL;
    }
    
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(AUDIO_PORT);
    addr.sin_addr.s_addr = inet_addr(server_ip);
    
    if (connect(audio_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        printf("[-] 音频服务器连接失败，继续运行\n");
        close(audio_fd);
        return NULL;
    }
    printf("[+] 音频服务器已连接\n");
    
    // 跳过音频头
    char header[69];
    read_(audio_fd, header, 69, 69);
    
    // 初始化SDL音频
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0) {
        printf("[-] SDL音频初始化失败: %s\n", SDL_GetError());
        close(audio_fd);
        return NULL;
    }
    
    SDL_AudioSpec want, have;
    SDL_zero(want);
    want.freq = 48000;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 4096;
    want.callback = NULL;
    
    audio_device = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (audio_device == 0) {
        printf("[-] 打开音频设备失败: %s\n", SDL_GetError());
        close(audio_fd);
        return NULL;
    }
    
    printf("[+] 音频设备打开: %dHz %d通道\n", have.freq, have.channels);
    
    int buffer_size = 176400;
    uint8_t* pcm_buffer = (uint8_t*)malloc(buffer_size);
    SDL_PauseAudioDevice(audio_device, 0);
    
    while (running) {
        int size = readyz(audio_fd, (char*)pcm_buffer, buffer_size);
        if (size <= 0) {
            if (running) printf("[-] 音频连接断开\n");
            break;
        }
        
        SDL_QueueAudio(audio_device, pcm_buffer, size);
        
        int queued = SDL_GetQueuedAudioSize(audio_device);
        if (queued > buffer_size * 2) {
            SDL_ClearQueuedAudio(audio_device);
        }
    }
    
    free(pcm_buffer);
    SDL_PauseAudioDevice(audio_device, 1);
    SDL_CloseAudioDevice(audio_device);
    close(audio_fd);
    
    printf("[+] 音频线程退出\n");
    return NULL;
}

// ==================== 主函数 ====================
int main(int argc, char* argv[]) {
    printf("\n");
    printf("========================================\n");
    printf("  Ubuntu 接收端 - 最终版\n");
    printf("========================================\n");
    
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    if (argc > 1) {
        strcpy(server_ip, argv[1]);
    } else {
        printf("使用默认IP: %s\n", DEFAULT_SERVER_IP);
        printf("用法: %s <server_ip>\n", argv[0]);
    }
    
    printf("[+] 目标服务器: %s\n", server_ip);
    printf("[+] 触摸端口: %d\n", TOUCH_PORT);
    printf("[+] 视频端口: %d\n", VIDEO_PORT);
    printf("[+] 音频端口: %d\n", AUDIO_PORT);
    printf("========================================\n");
    
    // 启动线程
    pthread_create(&video_thread, NULL, video_thread_func, NULL);
    pthread_create(&audio_thread, NULL, audio_thread_func, NULL);

    
    // 等待线程结束
    pthread_join(video_thread, NULL);
    pthread_join(audio_thread, NULL);
    pthread_join(event_thread, NULL);
    if (touch_socket >= 0) close(touch_socket);
    SDL_Quit();
    
    printf("[+] 程序退出\n");
    return 0;
}

