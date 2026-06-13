// jni/video_codec.c
//#include "video_codec.h"
//$ gcc /sdcard/h264.c  -lmediandk  -llog
//nc ip:9999 >/sdcard/k
//./a.out

#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#include <android/log.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <arpa/inet.h>
#include <pthread.h>

#include <android/native_window.h>
#define LOG_TAG "VideoCodec"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

int qinputCount = 0,inputCount = 0, outputCount = 0;
// 读取函数声明
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

int fd=-1;
int runing=1;
void *video_decode(void *k) {
	char buff[77];
	int ret = read_(fd, buff, 69, 69);
	if (ret != 69) {
		printf("读取视频头失败");
		return 0;
	}

	int width, height;
	ret = read_(fd, (char*)&width, 4, 69);
	if (ret != 4) return 0;
	ret = read_(fd, (char*)&height, 4, 69);
	if (ret != 4) return 0;

	width = ntohl(width);
	height = ntohl(height);
	printf("视频分辨率: %dx%d", width, height);
	//ANativeWindow_setBuffersGeometry(0, width, height, WINDOW_FORMAT_RGBX_8888);

	AMediaCodec* codec = AMediaCodec_createDecoderByType("video/avc");
	AMediaFormat* format = AMediaFormat_new();
	AMediaFormat_setString(format, "mime", "video/avc");
	AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_WIDTH, width);
	AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_HEIGHT, height);
	// AMediaFormat_setInt32(format, PARAMETER_KEY_VIDEO_FPS, 60);
	AMediaCodec_configure(codec, format, 0, NULL, 0);
	AMediaCodec_start(codec);

	printf("视频解码开始");

	int buffersize = 1024 * 1024 * 6;
	char* buffer = malloc(buffersize);
	
	ssize_t bufidx ;
	size_t bufsize;
	uint8_t* buf ; 
	int size; 
	int deng=-1; 
	while (runing) {
		bufidx = AMediaCodec_dequeueInputBuffer(codec, deng);
		if (bufidx >= 0) {
			buf = AMediaCodec_getInputBuffer(codec, bufidx, &bufsize);
			size = readyz(fd, (char*)buf, bufsize);
			if (size < 1) 
				break; 
			AMediaCodec_queueInputBuffer(codec, bufidx, 0, size, 0, 0);
			inputCount++;
			}
		AMediaCodecBufferInfo info;
		while(1){
			ssize_t outidx = AMediaCodec_dequeueOutputBuffer(codec, &info, 0);
			if (outidx < 0) 
				break;

			AMediaCodec_releaseOutputBuffer(codec, outidx, 0); 
			outputCount++;
		}

	}
	runing=0; 
	// free(buffer);
	AMediaCodec_stop(codec);
	AMediaCodec_delete(codec);
	AMediaFormat_delete(format);
}

int main(int argc,char **argv){
     if(argc!=2){
     printf("用法: \n\tnc ip:9999 >file\n\t过一分钟以上run:\n\t%s file",argv[0]);
     return 0;
     }
    fd=open(argv[1],0);
    if(fd<1){
    printf("打开文件失败\n"); 
    return -1; 
    }
    long videoThread=0; 
    pthread_create(&videoThread, NULL,video_decode, NULL);
    int ret=0; 
    while(runing){
	    sleep(1);
	    printf("视频: 输入=%d 输出=%d,帧率=%d\n", inputCount, outputCount,outputCount-ret);
	    ret=outputCount; 
    }
    
}

