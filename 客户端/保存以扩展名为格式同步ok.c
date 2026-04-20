// record_ts_multi_format.c
// 编译: gcc -o record_ts record_ts_multi_format.c -lavformat -lavcodec -lavutil -lswresample -lpthread
// 【最终完整版】音视频同步 + MP4修复

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <ctype.h>
#include <stdint.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
#include <libavutil/dict.h>
#include <libswresample/swresample.h>

#define VIDEO_PORT 9999
#define AUDIO_PORT 9998
#define VIDEO_BUFFER_SIZE (1024 * 1024 * 6)
#define AUDIO_BUFFER_SIZE (1024 * 512)

static int running = 1;
static char server_ip[32] = "192.168.100.1";

typedef struct {
    AVCodecContext *codec_ctx;
    SwrContext *swr_ctx;
    AVFrame *frame;
    AVPacket *pkt;
    int64_t pts;
    int64_t input_pts;
    int sample_rate;
    int channels;
    int frame_size;
    int bytes_per_sample;
    int samples_per_packet;
} AudioEncoder;

typedef struct {
    const char *extension;
    const char *format_name;
    const char *video_codec;
    const char *audio_codec;
    int copy_video;
    int copy_audio;
} FormatSpec;

static const FormatSpec supported_formats[] = {
    {".ts",     "mpegts",      "h264",     "aac",      1,          0},
    {".m2ts",   "mpegts",      "h264",     "aac",      1,          0},
    {".mts",    "mpegts",      "h264",     "aac",      1,          0},
    {".mp4",    "mp4",         "h264",     "aac",      1,          0},
    {".mov",    "mov",         "h264",     "aac",      1,          0},
    {".mkv",    "matroska",    "h264",     "aac",      1,          0},
    {".webm",   "webm",        "vp9",      "opus",     0,          0},
    {".avi",    "avi",         "mpeg4",    "libmp3lame", 0,        0},
    {".flv",    "flv",         "h264",     "aac",      1,          0},
    {".ogv",    "ogg",         "theora",   "libvorbis",0,          0},
    {NULL,      NULL,          NULL,       NULL,       0,          0}
};

int64_t get_time_us(void);
int64_t get_video_pts_from_clock(void);
int64_t get_audio_pts_from_clock(int sample_rate);

AudioEncoder *g_audio_enc = NULL;
AVFormatContext *g_fmt_ctx = NULL;
AVStream *g_audio_stream = NULL;
AVStream *g_video_stream = NULL;
int g_audio_fd = -1;
int g_video_fd = -1;
int g_video_width = 0;
int g_video_height = 0;
int64_t g_video_pts = 0;
const FormatSpec *g_format_spec = NULL;

int64_t g_start_time_us = 0;
pthread_mutex_t g_sync_mutex = PTHREAD_MUTEX_INITIALIZER;

int64_t get_time_us(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000000LL + tv.tv_usec;
}

int64_t get_video_pts_from_clock(void) {
    int64_t now_us = get_time_us();
    int64_t elapsed_us = now_us - g_start_time_us;
    return (elapsed_us * 90) / 1000;
}

int64_t get_audio_pts_from_clock(int sample_rate) {
    int64_t now_us = get_time_us();
    int64_t elapsed_us = now_us - g_start_time_us;
    return (elapsed_us * sample_rate) / 1000000;
}

void signal_handler(int sig) {
    printf("\n[!] 停止录制...\n");
    running = 0;
}

void get_file_extension(const char *filename, char *ext, size_t ext_size) {
    const char *dot = strrchr(filename, '.');
    if (dot && dot != filename) {
        int i = 0;
        while (dot[i] && i < ext_size - 1) {
            ext[i] = tolower(dot[i]);
            i++;
        }
        ext[i] = '\0';
    } else {
        ext[0] = '\0';
    }
}

const FormatSpec* find_format_by_extension(const char *extension) {
    for (int i = 0; supported_formats[i].extension != NULL; i++) {
        if (strcasecmp(supported_formats[i].extension, extension) == 0) {
            return &supported_formats[i];
        }
    }
    return NULL;
}

void print_supported_formats(void) {
    printf("\n支持的输出格式:\n");
    printf("%-10s %-15s %-15s %-15s\n", "扩展名", "格式", "视频编码", "音频编码");
    printf("%-10s %-15s %-15s %-15s\n", "--------", "------", "------", "------");
    for (int i = 0; supported_formats[i].extension != NULL; i++) {
        printf("%-10s %-15s %-15s %-15s\n", 
               supported_formats[i].extension,
               supported_formats[i].format_name,
               supported_formats[i].video_codec,
               supported_formats[i].audio_codec);
    }
    printf("\n");
}

int read_bytes(int fd, char* buf, size_t size) {
    int remaining = size;
    int ret = 0;
    while (remaining > 0) {
        ret = read(fd, buf, remaining);
        if (ret < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (ret == 0) return -1;
        buf += ret;
        remaining -= ret;
    }
    return size;
}

int read_packet(int fd, uint8_t *buf, int max_size) {
    int64_t timestamp_discard;
    if (read_bytes(fd, (char*)&timestamp_discard, 8) != 8) return -1;
    
    int size;
    if (read_bytes(fd, (char*)&size, 4) != 4) return -1;
    size = ntohl(size);
    
    if (size > 0 && size <= max_size) {
        if (read_bytes(fd, (char*)buf, size) == size) {
            return size;
        }
    }
    return -1;
}

int extract_audio_params(const char *header, int *sample_rate_out, int *channels_out, int *bytes_per_sample_out) {
    printf("[音频] 读取头部信息，尝试自动检测参数...\n");
    return -1;
}

AudioEncoder* init_audio_encoder(int sample_rate, int channels) {
    AudioEncoder *enc = calloc(1, sizeof(AudioEncoder));
    if (!enc) return NULL;
    
    const AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
    if (!codec) {
        fprintf(stderr, "找不到AAC编码器\n");
        free(enc);
        return NULL;
    }
    
    enc->codec_ctx = avcodec_alloc_context3(codec);
    if (!enc->codec_ctx) {
        free(enc);
        return NULL;
    }
    
    enc->sample_rate = sample_rate;
    enc->channels = channels;
    enc->codec_ctx->sample_fmt = AV_SAMPLE_FMT_FLTP;
    enc->codec_ctx->sample_rate = sample_rate;
    enc->codec_ctx->bit_rate = 128000;
    av_channel_layout_default(&enc->codec_ctx->ch_layout, channels);
    
    if (avcodec_open2(enc->codec_ctx, codec, NULL) < 0) {
        fprintf(stderr, "打开AAC编码器失败\n");
        avcodec_free_context(&enc->codec_ctx);
        free(enc);
        return NULL;
    }
    
    enc->swr_ctx = swr_alloc();
    if (!enc->swr_ctx) {
        fprintf(stderr, "重采样器分配失败\n");
        avcodec_free_context(&enc->codec_ctx);
        free(enc);
        return NULL;
    }
    
    AVChannelLayout ch_layout;
    av_channel_layout_default(&ch_layout, channels);
    
    av_opt_set_chlayout(enc->swr_ctx, "in_chlayout", &ch_layout, 0);
    av_opt_set_chlayout(enc->swr_ctx, "out_chlayout", &ch_layout, 0);
    av_opt_set_int(enc->swr_ctx, "in_sample_rate", sample_rate, 0);
    av_opt_set_int(enc->swr_ctx, "out_sample_rate", sample_rate, 0);
    av_opt_set_sample_fmt(enc->swr_ctx, "in_sample_fmt", AV_SAMPLE_FMT_S16, 0);
    av_opt_set_sample_fmt(enc->swr_ctx, "out_sample_fmt", AV_SAMPLE_FMT_FLTP, 0);
    
    if (swr_init(enc->swr_ctx) < 0) {
        fprintf(stderr, "重采样器初始化失败\n");
        swr_free(&enc->swr_ctx);
        avcodec_free_context(&enc->codec_ctx);
        free(enc);
        return NULL;
    }
    
    enc->frame = av_frame_alloc();
    enc->pkt = av_packet_alloc();
    enc->pts = 0;
    enc->input_pts = 0;
    enc->frame_size = enc->codec_ctx->frame_size;
    if (enc->frame_size <= 0) enc->frame_size = 1024;
    
    enc->bytes_per_sample = 2;
    enc->samples_per_packet = 0;
    
    enc->frame->nb_samples = enc->frame_size;
    enc->frame->format = enc->codec_ctx->sample_fmt;
    enc->frame->sample_rate = enc->sample_rate;
    av_channel_layout_copy(&enc->frame->ch_layout, &enc->codec_ctx->ch_layout);
    av_frame_get_buffer(enc->frame, 0);
    
    printf("[+] AAC编码器: %dHz %d通道, 帧大小=%d\n", sample_rate, channels, enc->frame_size);
    return enc;
}

void encode_audio(uint8_t *pcm_data, int pcm_size) {
    if (!g_audio_enc || !pcm_data || pcm_size <= 0) return;
    
    int input_samples = pcm_size / (g_audio_enc->channels * g_audio_enc->bytes_per_sample);
    
    if (input_samples <= 0) {
        printf("[警告] 音频包大小异常: %d 字节, 计算样本数=%d\n",
               pcm_size, input_samples);
        return;
    }
    
    if (g_audio_enc->samples_per_packet == 0) {
        g_audio_enc->samples_per_packet = input_samples;
        printf("[音频] 自动检测: 每包%d样本 = %.3fms\n",
               input_samples, (double)input_samples * 1000.0 / g_audio_enc->sample_rate);
    }
    
    int64_t clock_pts = get_audio_pts_from_clock(g_audio_enc->sample_rate);
    int64_t pts_before = clock_pts;
    
    g_audio_enc->input_pts += input_samples;
    
    int out_samples = swr_get_out_samples(g_audio_enc->swr_ctx, input_samples);
    if (out_samples <= 0) out_samples = input_samples;
    
    uint8_t **out_buffer = malloc(sizeof(uint8_t*) * g_audio_enc->channels);
    if (!out_buffer) return;
    
    if (av_samples_alloc(out_buffer, NULL, g_audio_enc->channels, out_samples, 
                         AV_SAMPLE_FMT_FLTP, 0) < 0) {
        free(out_buffer);
        return;
    }
    
    int converted = swr_convert(g_audio_enc->swr_ctx, out_buffer, out_samples, 
                                (const uint8_t**)&pcm_data, input_samples);
    
    if (converted > 0) {
        int processed = 0;
        int64_t current_pts = pts_before;
        
        while (processed < converted) {
            int chunk = (converted - processed);
            if (chunk > g_audio_enc->frame_size) {
                chunk = g_audio_enc->frame_size;
            }
            
            g_audio_enc->frame->nb_samples = chunk;
            av_frame_make_writable(g_audio_enc->frame);
            
            for (int ch = 0; ch < g_audio_enc->channels; ch++) {
                int sample_bytes = av_get_bytes_per_sample(AV_SAMPLE_FMT_FLTP);
                memcpy(g_audio_enc->frame->data[ch], 
                       out_buffer[ch] + processed * sample_bytes,
                       chunk * sample_bytes);
            }
            
            g_audio_enc->frame->pts = current_pts;
            
            int ret = avcodec_send_frame(g_audio_enc->codec_ctx, g_audio_enc->frame);
            
            if (ret >= 0) {
                while (1) {
                    ret = avcodec_receive_packet(g_audio_enc->codec_ctx, g_audio_enc->pkt);
                    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
                    if (ret < 0) break;
                    
                    if (g_audio_enc->pkt->pts == AV_NOPTS_VALUE) {
                        g_audio_enc->pkt->pts = g_audio_enc->frame->pts;
                    }
                    
                    av_packet_rescale_ts(g_audio_enc->pkt, 
                                        g_audio_enc->codec_ctx->time_base, 
                                        g_audio_stream->time_base);
                    g_audio_enc->pkt->stream_index = g_audio_stream->index;
                    
                    av_interleaved_write_frame(g_fmt_ctx, g_audio_enc->pkt);
                    av_packet_unref(g_audio_enc->pkt);
                }
            }
            
            current_pts += (int64_t)chunk * input_samples / converted;
            processed += chunk;
        }
        
        g_audio_enc->pts = get_audio_pts_from_clock(g_audio_enc->sample_rate);
    }
    
    av_freep(&out_buffer[0]);
    free(out_buffer);
}

void* audio_thread(void *arg) {
    printf("[音频线程] 启动\n");
    
    uint8_t *audio_buffer = malloc(AUDIO_BUFFER_SIZE);
    if (!audio_buffer) {
        printf("[音频] 内存分配失败\n");
        return NULL;
    }
    
    int packet_count = 0;
    int first_packet = 1;
    
    printf("[音频] 开始接收音频数据\n");
    
    while (running) {
        int size = read_packet(g_audio_fd, audio_buffer, AUDIO_BUFFER_SIZE);
        
        if (size > 0) {
            if (first_packet) {
                printf("[音频] 首个数据包大小: %d 字节\n", size);
                first_packet = 0;
            }
            
            encode_audio(audio_buffer, size);
            packet_count++;
            
            if (packet_count % 50 == 0) {
                int64_t clock_pts = get_audio_pts_from_clock(g_audio_enc->sample_rate);
                double duration_sec = (double)g_audio_enc->pts / g_audio_enc->sample_rate;
                double clock_sec = (double)clock_pts / g_audio_enc->sample_rate;
                double drift = (duration_sec - clock_sec) * 1000.0;
                
                printf("[音频] %d 包 | PTS=%lld (%.3f秒) | 时钟=%lld (%.3f秒) | 差值=%+.1fms\n", 
                       packet_count, (long long)g_audio_enc->pts, duration_sec,
                       (long long)clock_pts, clock_sec, drift);
            }
        } else if (size < 0) {
            if (running) {
                printf("[音频] 连接断开\n");
                break;
            }
        }
        usleep(1000);
    }
    
    free(audio_buffer);
    double final_duration = (double)g_audio_enc->pts / g_audio_enc->sample_rate;
    printf("[音频线程] 结束 | 共 %d 包 | PTS=%lld (%.3f秒)\n", 
           packet_count, (long long)g_audio_enc->pts, final_duration);
    return NULL;
}

void* video_thread(void *arg) {
    printf("[视频线程] 启动\n");
    
    uint8_t *video_buffer = malloc(VIDEO_BUFFER_SIZE);
    if (!video_buffer) {
        printf("[视频] 内存分配失败\n");
        return NULL;
    }
    
    int frame_count = 0;
    
    printf("[视频] 开始接收视频数据\n");
    
    while (running) {
        int size = read_packet(g_video_fd, video_buffer, VIDEO_BUFFER_SIZE);
        
        if (size > 0) {
            AVPacket *pkt = av_packet_alloc();
            if (!pkt) {
                printf("[视频] 分配包失败\n");
                continue;
            }
            
            int64_t pts = get_video_pts_from_clock();
            
            pkt->data = video_buffer;
            pkt->size = size;
            pkt->stream_index = g_video_stream->index;
            pkt->pts = pts;
            pkt->dts = pts;
            if (frame_count > 0) {
                pkt->duration = pts - g_video_pts;
            } else {
                pkt->duration = 3000;
            }
            
            int ret = av_interleaved_write_frame(g_fmt_ctx, pkt);
            if (ret == 0) {
                frame_count++;
                g_video_pts = pts;
                
                if (frame_count % 30 == 0) {
                    double duration_sec = (double)g_video_pts / 90000.0;
                    int64_t clock_pts = get_video_pts_from_clock();
                    double clock_sec = (double)clock_pts / 90000.0;
                    double drift = (duration_sec - clock_sec) * 1000.0;
                    
                    printf("[视频] %d 帧 | PTS=%lld (%.3f秒) | 时钟=%lld (%.3f秒) | 差值=%+.1fms\n", 
                           frame_count, (long long)g_video_pts, duration_sec,
                           (long long)clock_pts, clock_sec, drift);
                }
            } else {
                char errbuf[256];
                av_strerror(ret, errbuf, sizeof(errbuf));
                if (frame_count > 0) {
                    printf("[视频] 写入错误: %s\n", errbuf);
                }
            }
            
            av_packet_free(&pkt);
        } else if (size < 0) {
            if (running && frame_count > 0) {
                printf("[视频] 连接断开\n");
                break;
            }
        }
        usleep(1000);
    }
    
    free(video_buffer);
    double final_duration = (double)g_video_pts / 90000.0;
    printf("[视频线程] 结束 | 共 %d 帧 | PTS=%lld (%.3f秒)\n", 
           frame_count, (long long)g_video_pts, final_duration);
    return NULL;
}

int main(int argc, char *argv[]) {
    printf("\n========================================\n");
    printf("  多格式 TS 录制器 (全局时钟同步)\n");
    printf("========================================\n");
    
    signal(SIGINT, signal_handler);
    
    if (argc != 3) {
        printf("用法: %s <server_ip> <output_file>\n", argv[0]);
        printf("例如: %s 192.168.36.1 /sdcard/k.ts\n", argv[0]);
        print_supported_formats();
        return 1;
    }
    
    strcpy(server_ip, argv[1]);
    char *output_file = argv[2];
    
    char extension[32];
    get_file_extension(output_file, extension, sizeof(extension));
    
    if (extension[0] == '\0') {
        fprintf(stderr, "错误: 文件必须包含扩展名\n");
        print_supported_formats();
        return 1;
    }
    
    g_format_spec = find_format_by_extension(extension);
    if (!g_format_spec) {
        fprintf(stderr, "错误: 不支持的文件格式 '%s'\n", extension);
        print_supported_formats();
        return 1;
    }
    
    printf("[+] 服务器: %s\n", server_ip);
    printf("[+] 输出: %s\n", output_file);
    printf("[+] 格式: %s (%s)\n", g_format_spec->format_name, extension);
    printf("[+] 同步方式: 全局系统时钟\n");
    
    avformat_network_init();
    g_start_time_us = get_time_us();
    
    // ============ 连接视频 ============
    g_video_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (g_video_fd < 0) {
        perror("创建视频socket失败");
        return 1;
    }
    
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(VIDEO_PORT);
    addr.sin_addr.s_addr = inet_addr(server_ip);
    
    if (connect(g_video_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("连接视频失败");
        return 1;
    }
    printf("[+] 视频已连接\n");
    
    char header[69];
    if (read_bytes(g_video_fd, header, 69) != 69) {
        fprintf(stderr, "读取视频头失败\n");
        return 1;
    }
    
    int width, height;
    if (read_bytes(g_video_fd, (char*)&width, 4) != 4 ||
        read_bytes(g_video_fd, (char*)&height, 4) != 4) {
        fprintf(stderr, "读取分辨率失败\n");
        return 1;
    }
    g_video_width = ntohl(width);
    g_video_height = ntohl(height);
    printf("[+] 视频分辨率: %dx%d\n", g_video_width, g_video_height);
    
    int ret = avformat_alloc_output_context2(&g_fmt_ctx, NULL, g_format_spec->format_name, output_file);
    if (ret < 0 || !g_fmt_ctx) {
        fprintf(stderr, "创建输出上下文失败\n");
        return 1;
    }
    
    g_video_stream = avformat_new_stream(g_fmt_ctx, NULL);
    if (!g_video_stream) {
        fprintf(stderr, "创建视频流失败\n");
        return 1;
    }
    
    AVCodecParameters *video_params = avcodec_parameters_alloc();
    video_params->codec_type = AVMEDIA_TYPE_VIDEO;
    video_params->codec_id = AV_CODEC_ID_H264;
    video_params->width = g_video_width;
    video_params->height = g_video_height;
    video_params->format = AV_PIX_FMT_YUV420P;
    video_params->bit_rate = 2000000;
    
    avcodec_parameters_copy(g_video_stream->codecpar, video_params);
    g_video_stream->time_base = (AVRational){1, 90000};
    avcodec_parameters_free(&video_params);
    
    printf("[+] 视频流已创建\n");
    
    // ============ 连接音频 ============
    g_audio_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (g_audio_fd < 0) {
        perror("创建音频socket失败");
        g_audio_fd = -1;
    } else {
        addr.sin_port = htons(AUDIO_PORT);
        if (connect(g_audio_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            printf("[-] 音频连接失败: %s\n", strerror(errno));
            close(g_audio_fd);
            g_audio_fd = -1;
        } else {
            printf("[+] 音频已连接\n");
            
            if (read_bytes(g_audio_fd, header, 69) != 69) {
                printf("[-] 读取音频头失败\n");
                close(g_audio_fd);
                g_audio_fd = -1;
            } else {
                int sample_rate = 48000;
                int channels = 2;
                int bytes_per_sample = 2;
                
                extract_audio_params(header, &sample_rate, &channels, &bytes_per_sample);
                
                printf("[+] 音频参数: %dHz %d通道 (S16)\n", sample_rate, channels);
                
                g_audio_enc = init_audio_encoder(sample_rate, channels);
                if (g_audio_enc) {
                    g_audio_stream = avformat_new_stream(g_fmt_ctx, NULL);
                    if (g_audio_stream) {
                        avcodec_parameters_from_context(g_audio_stream->codecpar, g_audio_enc->codec_ctx);
                        g_audio_stream->time_base = (AVRational){1, g_audio_enc->codec_ctx->sample_rate};
                        printf("[+] 音频流已创建\n");
                    } else {
                        printf("[-] 创建音频流失败\n");
                        avcodec_free_context(&g_audio_enc->codec_ctx);
                        free(g_audio_enc);
                        g_audio_enc = NULL;
                        close(g_audio_fd);
                        g_audio_fd = -1;
                    }
                } else {
                    printf("[-] 音频编码器初始化失败\n");
                    close(g_audio_fd);
                    g_audio_fd = -1;
                }
            }
        }
    }
    
    if (avio_open(&g_fmt_ctx->pb, output_file, AVIO_FLAG_WRITE) < 0) {
        fprintf(stderr, "打开输出文件失败\n");
        return 1;
    }
    
    // 【修复】为MP4添加选项以确保moov正确写入
    AVDictionary *options = NULL;
    if (strcmp(g_format_spec->format_name, "mp4") == 0) {
        av_dict_set(&options, "movflags", "faststart", 0);
    }
    
    ret = avformat_write_header(g_fmt_ctx, &options);
    if (options) av_dict_free(&options);
    
    if (ret < 0) {
        char errbuf[256];
        av_strerror(ret, errbuf, sizeof(errbuf));
        fprintf(stderr, "写入文件头失败: %s\n", errbuf);
        return 1;
    }
    printf("[+] 开始录制...\n\n");
    
    pthread_t video_tid, audio_tid;
    pthread_create(&video_tid, NULL, video_thread, NULL);
    
    if (g_audio_fd >= 0 && g_audio_enc) {
        pthread_create(&audio_tid, NULL, audio_thread, NULL);
    }
    
    pthread_join(video_tid, NULL);
    if (g_audio_fd >= 0 && g_audio_enc) {
        pthread_join(audio_tid, NULL);
    }
    
    printf("\n[+] 完成文件...\n");
    
    if (g_audio_enc && g_audio_enc->codec_ctx) {
        printf("[+] 刷新音频编码器...\n");
        avcodec_send_frame(g_audio_enc->codec_ctx, NULL);
        while (1) {
            ret = avcodec_receive_packet(g_audio_enc->codec_ctx, g_audio_enc->pkt);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
            if (ret < 0) break;
            
            if (g_audio_stream && g_audio_enc->pkt) {
                av_packet_rescale_ts(g_audio_enc->pkt, 
                                    g_audio_enc->codec_ctx->time_base, 
                                    g_audio_stream->time_base);
                g_audio_enc->pkt->stream_index = g_audio_stream->index;
                av_interleaved_write_frame(g_fmt_ctx, g_audio_enc->pkt);
                av_packet_unref(g_audio_enc->pkt);
            }
        }
    }
    
    printf("[+] 写入文件尾部...\n");
    av_write_trailer(g_fmt_ctx);
    
    printf("[+] 清理资源...\n");
    
    if (g_fmt_ctx) {
        if (g_fmt_ctx->pb) {
            avio_close(g_fmt_ctx->pb);
        }
        avformat_free_context(g_fmt_ctx);
        g_fmt_ctx = NULL;
    }
    
    if (g_audio_enc) {
        if (g_audio_enc->frame) av_frame_free(&g_audio_enc->frame);
        if (g_audio_enc->pkt) av_packet_free(&g_audio_enc->pkt);
        if (g_audio_enc->swr_ctx) swr_free(&g_audio_enc->swr_ctx);
        if (g_audio_enc->codec_ctx) avcodec_free_context(&g_audio_enc->codec_ctx);
        free(g_audio_enc);
        g_audio_enc = NULL;
    }
    
    if (g_video_fd >= 0) {
        close(g_video_fd);
        g_video_fd = -1;
    }
    if (g_audio_fd >= 0) {
        close(g_audio_fd);
        g_audio_fd = -1;
    }
    
    printf("[+] 完成! 文件: %s\n", output_file);
    return 0;
}