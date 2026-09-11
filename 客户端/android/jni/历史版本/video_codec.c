// jni/video_codec.c
#include "video_codec.h"
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#include <android/log.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <arpa/inet.h>
#include <sys/select.h>
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

	AMediaCodec* codec=AMediaCodec_createDecoderByType("video/avc");
   // const char *hw_name = "OMX.MTK.VIDEO.DECODER.AVC";
   //const char *hw_name = "OMX.google.h264.decoder";
//AMediaCodec *codec = AMediaCodec_createCodecByName(hw_name);
    //char *codecName = NULL;
//media_status_t status = AMediaCodec_getName(codec, &codecName);
	AMediaFormat* format = AMediaFormat_new();
	AMediaFormat_setString(format, "mime", "video/avc");
	AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_WIDTH, width);
	AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_HEIGHT, height);
	// AMediaFormat_setInt32(format, PARAMETER_KEY_VIDEO_FPS, 60);
	AMediaCodec_configure(codec, format, window, NULL, 0);
	AMediaCodec_start(codec);

	//LOGI("视频解码: %s 开始",codecName);

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
     int maxfd = fd+1;
      fd_set rd_set;
      AMediaCodecBufferInfo info;
      struct timeval tv;

// 注意：select 的第三个参数是写集合，你写 0 或 NULL 都行。
// 但你原来的 &rd_set 在 select 返回后会被内核修改，
// 如果是在循环里，下次调用前需要重新赋值 rd_set。
int size1=0; 
	while (*running) {
        
          
            
		bufidx = AMediaCodec_dequeueInputBuffer(codec, deng);
		if (bufidx >= 0) {

			buf = AMediaCodec_getInputBuffer(codec, bufidx, &bufsize);
            FD_ZERO(&rd_set);
          FD_SET(fd, &rd_set);
          tv.tv_sec = 0;          // 秒数（0秒）
tv.tv_usec = 16666;     // 微秒数（16666微秒 = 16.666ms）

          ret = select(maxfd, &rd_set, 0, NULL, &tv);
        if (ret < 0)
        {
            perror("错误 select()");
            break;
        }
         
            if (FD_ISSET(fd, &rd_set))
        {
			size = rreadyz(fd, (char*)buf, bufsize);
            memcpy(buffer,buf,size);
            size1=size;
          }else
          {
             memcpy(buf,buffer,size1);
             size=size1; 
          }
 
			if (size < 1) 
				break; 
			AMediaCodec_queueInputBuffer(codec, bufidx, 0, size, 0, 0);
			inputCount++;
            if(deng==-1)
			    deng=16666; 
		}else{
            //无丢帧方案要注销这里
            
			size1 =  rreadyz(fd,buffer,buffersize);
            
			if (size1 < 1) 
				break; 
			qinputCount++;
		}
       
		
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


