// jni/video_codec.h
#ifndef VIDEO_CODEC_H
#define VIDEO_CODEC_H

#include <android/native_window.h>

// 视频解码函数
void video_decode(int fd, ANativeWindow* window, int* running);

#endif