// jni/audio_player.c
#include "audio_player.h"
#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>
#include <android/log.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>

#define LOG_TAG "AudioPlayer"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// 读取函数声明
int read_(int fd, char* buf, size_t size, int max_size);
int readyz(int fd, char* buf, int size_max);

// PCM 播放器结构
typedef struct {
	SLObjectItf engineObject;
	SLEngineItf engineEngine;
	SLObjectItf outputMixObject;
	SLObjectItf playerObject;
	SLPlayItf playerPlay;
	SLAndroidSimpleBufferQueueItf bufferQueue;
	int isPlaying;
	int fd;
} PCMPlayer;
int astatus=0; 
static PCMPlayer g_player;
static short* pcmBuffer = NULL;
static int pcmBufferSize = 176400;  // 48000 * 2 * 2 / 8 * 2 秒

// 回调函数
void bufferQueueCallback(SLAndroidSimpleBufferQueueItf bq, void* context) {
	PCMPlayer* player = (PCMPlayer*)context;
	if (!player->isPlaying) return;

	int fd = player->fd;
	int ret = readyz(fd, (char*)pcmBuffer, pcmBufferSize);
	if (ret <1) {
		player->isPlaying=0; 
		return ;
	}
	(*bq)->Enqueue(bq, pcmBuffer, ret);


}

void audio_play(int fd, int* running) {
	LOGI("音频播放启动");

	// 跳过 PCM 头
	char buf[69];
	read_(fd, buf, 69, 69);

	// 分配缓冲区
	pcmBuffer = malloc(pcmBufferSize);
	if (!pcmBuffer) {
		LOGE("分配缓冲区失败");
		return;
	}

	// 初始化 OpenSL ES
	slCreateEngine(&g_player.engineObject, 0, NULL, 0, NULL, NULL);
	(*g_player.engineObject)->Realize(g_player.engineObject, SL_BOOLEAN_FALSE);
	(*g_player.engineObject)->GetInterface(g_player.engineObject, SL_IID_ENGINE, &g_player.engineEngine);

	(*g_player.engineEngine)->CreateOutputMix(g_player.engineEngine, &g_player.outputMixObject, 0, NULL, NULL);
	(*g_player.outputMixObject)->Realize(g_player.outputMixObject, SL_BOOLEAN_FALSE);

	// 配置 PCM 格式
	SLDataLocator_AndroidSimpleBufferQueue loc_bufq = {
		SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE, 2
	};

	SLDataFormat_PCM format_pcm = {
		SL_DATAFORMAT_PCM, 2, 48000 * 1000,
		SL_PCMSAMPLEFORMAT_FIXED_16, SL_PCMSAMPLEFORMAT_FIXED_16,
		SL_SPEAKER_FRONT_LEFT | SL_SPEAKER_FRONT_RIGHT,
		SL_BYTEORDER_LITTLEENDIAN
	};

	SLDataSource audioSrc = {&loc_bufq, &format_pcm};

	SLDataLocator_OutputMix loc_outmix = {
		SL_DATALOCATOR_OUTPUTMIX, g_player.outputMixObject
	};
	SLDataSink audioSnk = {&loc_outmix, NULL};

	const SLInterfaceID ids[] = {SL_IID_ANDROIDSIMPLEBUFFERQUEUE};
	const SLboolean req[] = {SL_BOOLEAN_TRUE};

	(*g_player.engineEngine)->CreateAudioPlayer(g_player.engineEngine, &g_player.playerObject,
			&audioSrc, &audioSnk, 1, ids, req);

	(*g_player.playerObject)->Realize(g_player.playerObject, SL_BOOLEAN_FALSE);
	(*g_player.playerObject)->GetInterface(g_player.playerObject, SL_IID_PLAY, &g_player.playerPlay);
	(*g_player.playerObject)->GetInterface(g_player.playerObject, SL_IID_ANDROIDSIMPLEBUFFERQUEUE, &g_player.bufferQueue);
	g_player.fd=fd;
	(*g_player.bufferQueue)->RegisterCallback(g_player.bufferQueue, bufferQueueCallback, &g_player);

	// 预加载数据
	int ret = readyz(fd, (char*)pcmBuffer, pcmBufferSize);
	if (ret > 0) {
		(*g_player.bufferQueue)->Enqueue(g_player.bufferQueue, pcmBuffer, ret);
	}

	g_player.isPlaying = 1;
	(*g_player.playerPlay)->SetPlayState(g_player.playerPlay, SL_PLAYSTATE_PLAYING);

	LOGI("音频播放中...");

	// 等待停止信号
    /*
	while (*running&&g_player.isPlaying) {
		usleep(100000);
	}*/
    astatus=1; 
}
void freeaudio(){
    if(astatus==0)
        return ; 
	// 清理
	(*g_player.playerPlay)->SetPlayState(g_player.playerPlay, SL_PLAYSTATE_STOPPED);
	(*g_player.playerObject)->Destroy(g_player.playerObject);
	(*g_player.outputMixObject)->Destroy(g_player.outputMixObject);
	(*g_player.engineObject)->Destroy(g_player.engineObject);

	free(pcmBuffer);
	LOGI("音频播放停止");
}
