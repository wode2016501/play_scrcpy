// jni/video_codec.c
#include "video_codec.h"
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#include <android/log.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <arpa/inet.h>

#define LOG_TAG "VideoCodec"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

int qinputCount = 0,inputCount = 0, outputCount = 0;

// 读取函数声明
int read_(int fd, char* buf, size_t size, int max_size);
int readyz(int fd, char* buf, int size_max);
int rreadyz(int fd, char* buf, int size_max);


// 判断 H.264 NALU 类型
int get_nalu_type(const uint8_t* data, size_t size) {
	if (size < 5) return -1;
	// 查找起始码 0x00 0x00 0x01 或 0x00 0x00 0x00 0x01
	int offset = 0;
	if (data[0] == 0 && data[1] == 0) {
		if (data[2] == 1) offset = 3;
		else if (data[2] == 0 && data[3] == 1) offset = 4;
		else return -1;
	}
	// NALU 类型 = 第 offset 字节的低 5 位是否为I帧
	return data[offset] & 0x1F==5;
}


void video_decode(int fd, ANativeWindow* window, int* running) {
	//char buff[77];
	/*int ret = read_(fd, buff, 69, 69);
	if (ret != 69) {
		LOGE("读取视频头失败");
		return;
	}*/
    
	int width, height;
	int ret = read_(fd, (char*)&width, 4, 69);
	if (ret != 4) return;
	ret = read_(fd, (char*)&height, 4, 69);
	if (ret != 4) return;
    

	//width = ntohl(width);
	//height = ntohl(height);
	LOGI("视频分辨率: %dx%d", width, height);

    
    
    
	ANativeWindow_setBuffersGeometry(window, width, height, WINDOW_FORMAT_RGBX_8888);

	AMediaCodec* codec = AMediaCodec_createDecoderByType("video/avc");
	AMediaFormat* format = AMediaFormat_new();
	AMediaFormat_setString(format, "mime", "video/avc");
	AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_WIDTH, width);
	AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_HEIGHT, height);
	// AMediaFormat_setInt32(format, PARAMETER_KEY_VIDEO_FPS, 60);
	AMediaCodec_configure(codec, format, window, NULL, 0);
	AMediaCodec_start(codec);

	LOGI("视频解码开始");

	int buffersize = 1024 * 1024 * 6;
	char* buffer = malloc(buffersize);
	if(buffer==0){
        return ; 
    }
        
	ssize_t bufidx ;
	size_t bufsize;
	uint8_t* buf ; 
	int size; 
	int deng=-1; 
	while (*running) {
		bufidx = AMediaCodec_dequeueInputBuffer(codec, deng);
		if (bufidx >= 0) {

			buf = AMediaCodec_getInputBuffer(codec, bufidx, &bufsize);
			size = rreadyz(fd, (char*)buf, bufsize);
 
			if (size < 1) 
				break; 
			AMediaCodec_queueInputBuffer(codec, bufidx, 0, size, 0, 0);
			inputCount++;
			deng=16666; 

		}else{
            //无丢帧方案要注销这里
            
			size =  rreadyz(fd,buffer,buffersize);
            
			if (size < 1) 
				break; 
			qinputCount++;
            
		}
		AMediaCodecBufferInfo info;
		while(1){
			ssize_t outidx = AMediaCodec_dequeueOutputBuffer(codec, &info, 0);
			if (outidx < 0) 
				break;

			AMediaCodec_releaseOutputBuffer(codec, outidx, 1); 
			outputCount++;
		}

		if (inputCount % 30 == 0) {
			LOGI("视频: 输入=%d 输出=%d,丢弃=%d", inputCount, outputCount,qinputCount);
		}
	}
	 free(buffer);
	AMediaCodec_stop(codec);
	AMediaCodec_delete(codec);
	AMediaFormat_delete(format);
}


