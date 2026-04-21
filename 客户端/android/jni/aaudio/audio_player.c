// audio_player.c - 环形缓冲区 + AAudio 低延迟播放
#include <aaudio/AAudio.h>
#include <android/log.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdatomic.h>
#include <pthread.h>

#define LOG_TAG "AudioPlayer"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

#define SAMPLE_RATE        48000
#define CHANNELS           2
#define BYTES_PER_SAMPLE   2
#define BYTES_PER_FRAME    (CHANNELS * BYTES_PER_SAMPLE)

// 环形缓冲区大小：200ms 数据（可根据需要调整，越大越不容易断流，但延迟略增）
#define RING_BUFFER_MS     200
#define RING_BUFFER_FRAMES (SAMPLE_RATE * RING_BUFFER_MS / 1000)
#define RING_BUFFER_SIZE   (RING_BUFFER_FRAMES * BYTES_PER_FRAME)

// 每次从 readyz 读取的块大小（例如 4096 字节）
#define CHUNK_SIZE         4096

// 你的原始读取函数
int read_(int fd, char* buf, size_t size, int max_size);
int readyz(int fd, char* buf, int size_max);

// 播放器结构体
typedef struct {
    AAudioStream* stream;
    int fd;
    atomic_int isPlaying;
    int* running;

    // 环形缓冲区
    char* ringBuffer;
    int writePos;      // 生产者写入位置
    int readPos;       // 消费者读取位置
    int dataSize;      // 缓冲区中有效数据字节数
    pthread_mutex_t mutex;
} PCMPlayer;

static PCMPlayer g_player = {0};

// 生产者：向环形缓冲区追加数据（如果空间不足，丢弃最旧的数据）
static int appendData(PCMPlayer* player, const char* data, int len) {
    pthread_mutex_lock(&player->mutex);
    int freeSpace = RING_BUFFER_SIZE - player->dataSize;
    if (freeSpace < len) {
        // 缓冲区满，丢弃最旧的 (len - freeSpace) 字节
        int discard = len - freeSpace;
        player->readPos = (player->readPos + discard) % RING_BUFFER_SIZE;
        player->dataSize -= discard;
        freeSpace = RING_BUFFER_SIZE - player->dataSize;
    }
    // 写入数据（分两段拷贝，处理环形回绕）
    int firstPart = RING_BUFFER_SIZE - player->writePos;
    if (firstPart >= len) {
        memcpy(player->ringBuffer + player->writePos, data, len);
    } else {
        memcpy(player->ringBuffer + player->writePos, data, firstPart);
        memcpy(player->ringBuffer, data + firstPart, len - firstPart);
    }
    player->writePos = (player->writePos + len) % RING_BUFFER_SIZE;
    player->dataSize += len;
    pthread_mutex_unlock(&player->mutex);
    return len;
}

// 消费者：从环形缓冲区读取数据，若不足则填静音
static int readData(PCMPlayer* player, char* dest, int bytesNeeded) {
    pthread_mutex_lock(&player->mutex);
    int available = player->dataSize;
    int bytesRead = (available < bytesNeeded) ? available : bytesNeeded;
    if (bytesRead > 0) {
        int firstPart = RING_BUFFER_SIZE - player->readPos;
        if (firstPart >= bytesRead) {
            memcpy(dest, player->ringBuffer + player->readPos, bytesRead);
        } else {
            memcpy(dest, player->ringBuffer + player->readPos, firstPart);
            memcpy(dest + firstPart, player->ringBuffer, bytesRead - firstPart);
        }
        player->readPos = (player->readPos + bytesRead) % RING_BUFFER_SIZE;
        player->dataSize -= bytesRead;
    }
    if (bytesRead < bytesNeeded) {
        memset(dest + bytesRead, 0, bytesNeeded - bytesRead);
    }
    pthread_mutex_unlock(&player->mutex);
    return bytesRead;
}

// AAudio 数据回调（高优先级实时线程）
static aaudio_data_callback_result_t dataCallback(
    AAudioStream* stream,
    void* userData,
    void* audioData,
    int32_t numFrames) {
    PCMPlayer* player = (PCMPlayer*)userData;
    if (!player || !atomic_load(&player->isPlaying)) {
        return AAUDIO_CALLBACK_RESULT_STOP;
    }
    int32_t bytesNeeded = numFrames * BYTES_PER_FRAME;
    readData(player, (char*)audioData, bytesNeeded);
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

// 错误回调
static void errorCallback(AAudioStream* stream, void* userData, aaudio_result_t error) {
    LOGE("AAudio error: %d", error);
    PCMPlayer* player = (PCMPlayer*)userData;
    if (player) atomic_store(&player->isPlaying, 0);
}

// 配置并打开 AAudio 流
static int setupStream(PCMPlayer* player) {
    AAudioStreamBuilder* builder = NULL;
    aaudio_result_t result = AAudio_createStreamBuilder(&builder);
    if (result != AAUDIO_OK) {
        LOGE("createStreamBuilder failed: %d", result);
        return -1;
    }
    AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setSampleRate(builder, SAMPLE_RATE);
    AAudioStreamBuilder_setChannelCount(builder, CHANNELS);
    AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_EXCLUSIVE);
    AAudioStreamBuilder_setDataCallback(builder, dataCallback, player);
    AAudioStreamBuilder_setErrorCallback(builder, errorCallback, player);
    result = AAudioStreamBuilder_openStream(builder, &player->stream);
    AAudioStreamBuilder_delete(builder);
    if (result != AAUDIO_OK) {
        LOGE("openStream failed: %d", result);
        return -1;
    }
    // 优化缓冲区大小
    int32_t framesPerBurst = AAudioStream_getFramesPerBurst(player->stream);
    int32_t optimalSize = framesPerBurst * 2;
    AAudioStream_setBufferSizeInFrames(player->stream, optimalSize);
    LOGI("Stream opened: burst=%d, buffer=%d, exclusive=%d",
         framesPerBurst, optimalSize,
         AAudioStream_getSharingMode(player->stream) == AAUDIO_SHARING_MODE_EXCLUSIVE);
    return 0;
}

// 主入口函数
void audio_play(int fd, int* running) {
    LOGI("Audio play start (ring buffer + AAudio)");

    // 跳过 PCM 头部（根据你的实际格式调整偏移）
    char header[69];
    read_(fd, header, 69, 69);

    // 初始化播放器
    memset(&g_player, 0, sizeof(g_player));
    g_player.fd = fd;
    g_player.running = running;
    atomic_init(&g_player.isPlaying, 0);
    pthread_mutex_init(&g_player.mutex, NULL);
    g_player.ringBuffer = (char*)malloc(RING_BUFFER_SIZE);
    if (!g_player.ringBuffer) {
        LOGE("malloc ring buffer failed");
        return;
    }
    g_player.writePos = 0;
    g_player.readPos = 0;
    g_player.dataSize = 0;

    // 打开 AAudio 流
    if (setupStream(&g_player) != 0) {
        free(g_player.ringBuffer);
        return;
    }

    // 启动播放
    atomic_store(&g_player.isPlaying, 1);
    if (AAudioStream_requestStart(g_player.stream) != AAUDIO_OK) {
        LOGE("requestStart failed");
        atomic_store(&g_player.isPlaying, 0);
        AAudioStream_close(g_player.stream);
        free(g_player.ringBuffer);
        return;
    }

    LOGI("AAudio is playing (ring buffer) ...");

    // 主循环：不断从 readyz 读取数据并填入环形缓冲区
    while (*running && atomic_load(&g_player.isPlaying)) {
        char chunk[CHUNK_SIZE];
        int ret = readyz(fd, chunk, sizeof(chunk));
        if (ret > 0) {
            appendData(&g_player, chunk, ret);
        } else if (ret == 0) {
            // 数据源暂时无数据，稍微休眠避免忙等
            usleep(10000);
        } else {
            // 错误或文件结束，退出循环
            LOGE("readyz returned %d, stop reading", ret);
            break;
        }
        // 控制主循环速率，避免 CPU 过高（可根据实际需要调整）
        usleep(5000);
    }

    // 清理
    LOGI("Stopping AAudio...");
    atomic_store(&g_player.isPlaying, 0);
    AAudioStream_requestStop(g_player.stream);
    AAudioStream_close(g_player.stream);
    free(g_player.ringBuffer);
    pthread_mutex_destroy(&g_player.mutex);
    LOGI("Audio play finished");
}
