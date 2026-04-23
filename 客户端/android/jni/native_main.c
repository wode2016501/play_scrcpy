// native_main.c - 添加 XY 转换开关
#include <android_native_app_glue.h>
#include <android/log.h>
#include <android/input.h>
#include <linux/input.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>
#include <sys/types.h>
#include <fcntl.h>
#include <android/native_window_jni.h>
#include <jni.h>
#include <stdlib.h>
#include <netinet/tcp.h>   // 添加这一行




// 音视频解码头文件
#include "video_codec.h"
#include "audio_player.h"

#define LOG_TAG "NativeApp"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)

static int touchSocket = -1;
static  int audioFd=-1; 
static  int videoFd=-1;
/*
struct idtime{
	struct timespec prev_ts;
	struct timespec now;
};
long long interval_us = 0;
struct idtime *idt=0;
*/
// ==================== 自定义固定大小结构体 ====================

// ==================== 分辨率配置 ====================
// 发送端（电视，竖屏）
int  SENDER_WIDTH= 1920;
int  SENDER_HEIGHT= 1080;

// 接收端（横屏）
int  RECEIVER_WIDTH =  2376; 
int  RECEIVER_HEIGHT =1080; 

// ==================== XY 转换开关 ====================
// 0: 不转换（直接缩放）
// 1: 交换 XY（竖屏转横屏）
// 2: 只交换不缩放
int  XY_SWAP_MODE =  0; // 修改这里：0=不转换, 1=竖屏转横屏, 2=只交换

// ==================== 配置 ====================

#define TOUCH_RECEIVER_PORT 9000
#define IP "192.168.100.1"
char ipip[20]; 
#define VIDEO_SERVER_PORT 9999
#define AUDIO_SERVER_PORT 9998

// ==================== 触摸点管理 ====================
typedef struct {
	int id;
	int x;
	int y;
	int active;
} TouchPoint;
// ==================== 按键点管理 ====================
typedef struct
{
	int keyCode;
	int active;
} KeyPoint;


// ==================== 音视频 ====================
static ANativeWindow* nativeWindow = NULL;
static int running = 1;
static pthread_t videoThread, audioThread;

// ==================== 函数声明 ====================
void* video_decode_thread(void* arg);
void* audio_decode_thread(void* arg);
int tcp_connect(const char* ip, int port);
int read_(int fd, char* buf, size_t size, int max_size);
int readyz(int fd, char* buf, int size_max);
void freeaudio(); 



// 坐标转换函数
int map_x(int x, int y) {
	switch (XY_SWAP_MODE) {
		case 0:  // 不转换，直接缩放
			return x * RECEIVER_WIDTH / SENDER_WIDTH;
		case 1:  // 竖屏转横屏：Y -> X
			return RECEIVER_WIDTH-y * RECEIVER_WIDTH / SENDER_HEIGHT;
		case 2:  // 只交换，不缩放
			return y;
		default:
			return x * RECEIVER_WIDTH / SENDER_WIDTH;
	}
}

int map_y(int x, int y) {
	switch (XY_SWAP_MODE) {
		case 0:  // 不转换，直接缩放
			return y * RECEIVER_HEIGHT / SENDER_HEIGHT;
		case 1:  // 竖屏转横屏：X -> Y
			return x * RECEIVER_HEIGHT / SENDER_WIDTH;
		case 2:  // 只交换，不缩放
			return x;
		default:
			return y * RECEIVER_HEIGHT / SENDER_HEIGHT;
	}
}

// ==================== 全屏设置 ====================
void set_fullscreen(struct android_app* app) {
	JNIEnv* env = NULL;

	// 获取 JNI 环境
	if ((*app->activity->vm)->GetEnv(app->activity->vm, (void**)&env, JNI_VERSION_1_6) != JNI_OK) {
		if ((*app->activity->vm)->AttachCurrentThread(app->activity->vm, &env, NULL) != JNI_OK) {
			LOGE("无法获取 JNI 环境");
			return;
		}
	}

	// 获取 Activity（不删除，这是全局引用）
	jobject activity = app->activity->clazz;
	jclass cls = (*env)->GetObjectClass(env, activity);

	// 获取 getWindow 方法
	jmethodID getWindow = (*env)->GetMethodID(env, cls, "getWindow", "()Landroid/view/Window;");
	if (getWindow == NULL) {
		LOGE("无法获取 getWindow 方法");
		(*env)->DeleteLocalRef(env, cls);
		return;
	}

	jobject window = (*env)->CallObjectMethod(env, activity, getWindow);
	if (window == NULL) {
		LOGE("window 为 NULL");
		(*env)->DeleteLocalRef(env, cls);
		return;
	}

	// 获取 Window 类
	jclass windowClass = (*env)->FindClass(env, "android/view/Window");
	if (windowClass == NULL) {
		LOGE("无法找到 Window 类");
		(*env)->DeleteLocalRef(env, window);
		(*env)->DeleteLocalRef(env, cls);
		return;
	}

	// 设置 FLAG_FULLSCREEN 和 FLAG_KEEP_SCREEN_ON
	jint FLAG_FULLSCREEN = 0x00000400;
	jint FLAG_KEEP_SCREEN_ON = 0x00000040;

	jmethodID addFlags = (*env)->GetMethodID(env, windowClass, "addFlags", "(I)V");
	if (addFlags != NULL) {
		(*env)->CallVoidMethod(env, window, addFlags, FLAG_FULLSCREEN | FLAG_KEEP_SCREEN_ON);
		LOGI("✓ 设置 FLAG_FULLSCREEN 和 FLAG_KEEP_SCREEN_ON");
	}

	// 隐藏导航栏和状态栏
	jmethodID getDecorView = (*env)->GetMethodID(env, windowClass, "getDecorView", "()Landroid/view/View;");
	if (getDecorView != NULL) {
		jobject decorView = (*env)->CallObjectMethod(env, window, getDecorView);
		if (decorView != NULL) {
			jclass viewClass = (*env)->FindClass(env, "android/view/View");
			if (viewClass != NULL) {
				jint SYSTEM_UI_FLAG_HIDE_NAVIGATION = 0x00000002;
				jint SYSTEM_UI_FLAG_FULLSCREEN = 0x00000004;
				jint SYSTEM_UI_FLAG_IMMERSIVE_STICKY = 0x00001000;

				jmethodID setSystemUiVisibility = (*env)->GetMethodID(env, viewClass, "setSystemUiVisibility", "(I)V");
				if (setSystemUiVisibility != NULL) {
					(*env)->CallVoidMethod(env, decorView, setSystemUiVisibility, 
							SYSTEM_UI_FLAG_HIDE_NAVIGATION | 
							SYSTEM_UI_FLAG_FULLSCREEN | 
							SYSTEM_UI_FLAG_IMMERSIVE_STICKY);
					LOGI("✓ 隐藏导航栏和状态栏");
				}
				(*env)->DeleteLocalRef(env, viewClass);
			}
			(*env)->DeleteLocalRef(env, decorView);
		}
	}

	LOGI("✓ 全屏模式已启用");

	// 清理本地引用（只删除我们创建的）
	(*env)->DeleteLocalRef(env, windowClass);
	(*env)->DeleteLocalRef(env, window);
	(*env)->DeleteLocalRef(env, cls);

	// 注意：不删除 activity，因为它是全局引用
}



// ==================== 网络连接 ====================
int tcp_connect(const char* ip, int port) {
	int sock = socket(AF_INET, SOCK_STREAM, 0);
	if (sock < 0) {
		LOGE("创建 socket 失败: %s", strerror(errno));
		return -1;
	}

	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	addr.sin_addr.s_addr = inet_addr(ip);

	if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
		LOGE("连接 %s:%d 失败: %s", ip, port, strerror(errno));
		close(sock);
		return -1;
	}

	LOGI("✓ 已连接到 %s:%d", ip, port);
	return sock;
}


// 发送触摸事件（带坐标转换）
void send_touch_event(int id, int x, int y, int action) {
	// 转换坐标
	int mapped_x = map_x(x, y);
	int mapped_y = map_y(x, y);

	LOGD("坐标转换: (%d,%d) -> (%d,%d) [模式=%d] %dx%d ", x, y, mapped_x, mapped_y, XY_SWAP_MODE,RECEIVER_WIDTH,RECEIVER_HEIGHT);


	TouchPoint touch_point;
	touch_point.id = id;
	touch_point.x = mapped_x;
	touch_point.y = mapped_y;
	touch_point.active = action; // 0=按下, 1=移动, 2=抬起  

	int ret=sizeof(TouchPoint);
	ret=write(touchSocket, &ret, sizeof(int));
	if(ret != sizeof(int))    {
		LOGE("发送触摸事件大小失败");
		close(touchSocket);
		touchSocket = -1;
		running=0;
		return;
	}
	ret=write(touchSocket, &touch_point, sizeof(TouchPoint));
	if(ret != sizeof(TouchPoint))    {
		LOGE("发送触摸事件失败");
		close(touchSocket);
		touchSocket = -1;
		running=0;
		return;
	}
}

// 发送按键事件
void send_key_event(int keyCode, int action) {
	KeyPoint key;
	key.keyCode = keyCode;
	key.active = action;
	int ret=sizeof(KeyPoint);
	ret=write(touchSocket, &ret, sizeof(int));
	if(ret != sizeof(int))    {
		LOGE("发送按键事件大小失败");
		close(touchSocket);
		touchSocket = -1;
		running=0;
		return;
	}
	ret=write(touchSocket, &key, sizeof(KeyPoint));
	if(ret != sizeof(KeyPoint))    {
		running=0;
		LOGE("发送按键事件失败");
		close(touchSocket);
		touchSocket = -1;
		return;
	}
	LOGI("发送按键: code=%d, action=%s", keyCode, action ? "DOWN" : "UP");
}



// ==================== 触摸事件处理 ====================
int handle_touch_event(AInputEvent* event) {
	if (AInputEvent_getType(event) != AINPUT_EVENT_TYPE_MOTION) {
		return 0;
	}

	int action = AMotionEvent_getAction(event);
	int actionMasked = action & AMOTION_EVENT_ACTION_MASK;
	int pointerIndex = (action & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) 
		>> AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT;

	switch (actionMasked) {
		case AMOTION_EVENT_ACTION_DOWN:
		case AMOTION_EVENT_ACTION_POINTER_DOWN: {
								int id = AMotionEvent_getPointerId(event, pointerIndex);
								int x = (int) AMotionEvent_getX(event, pointerIndex);
								int y = (int) AMotionEvent_getY(event, pointerIndex);

								LOGI("DOWN: id=%d, 原始坐标=(%d,%d)", id, x, y);
								send_touch_event(id, x, y, 0);
								break;
							}

		case AMOTION_EVENT_ACTION_MOVE: {
							int count = AMotionEvent_getPointerCount(event);
							if(count>10)count=10; 
							for (int i = 0; i < count; i++) {
                                /*
								clock_gettime(CLOCK_MONOTONIC, &idt[i].now);   // 注意用 . 而不是 ->
								interval_us = (idt[i].now.tv_sec - idt[i].prev_ts.tv_sec) * 1000000LL +
									(idt[i].now.tv_nsec - idt[i].prev_ts.tv_nsec) / 1000;

								if (interval_us < 3000) {
									// 间隔太小，跳过此事件（不写入）
									continue;   // 而不是 return
								}
								idt[i].prev_ts = idt[i].now;
                                */
								int id = AMotionEvent_getPointerId(event, i);
								int x = (int) AMotionEvent_getX(event, i);
								int y = (int) AMotionEvent_getY(event, i);
								send_touch_event(id, x, y, 1);
							}
							break;
						}

		case AMOTION_EVENT_ACTION_UP:
		case AMOTION_EVENT_ACTION_POINTER_UP: {
							      int id = AMotionEvent_getPointerId(event, pointerIndex);
							      LOGI("UP: id=%d", id);
							      send_touch_event(id, 0, 0, 2);
							      break;
						      }

		case AMOTION_EVENT_ACTION_CANCEL: {
							  LOGI("清除CANCEL");

							  break;
						  }
	}
	return 1;
}

// ==================== 按键处理 ====================
int android_to_linux_keycode(int android_keycode) {
	switch (android_keycode) {
		case 3:   return 102;  // HOME
		case 4:   return 158;  // BACK
		case 19:  return 103;  // DPAD_UP
		case 20:  return 108;  // DPAD_DOWN
		case 21:  return 105;  // DPAD_LEFT
		case 22:  return 106;  // DPAD_RIGHT
		case 23:  return 28;   // DPAD_CENTER
		case 66:  return 28;   // ENTER
		case 24:  return 115;  // VOLUME_UP
		case 25:  return 114;  // VOLUME_DOWN
		case 26:  return 116;  // POWER
		case 82:  return 139;  // MENU
		default: return android_keycode;
	}
}

int handle_key_event(AInputEvent* event) {
	if (AInputEvent_getType(event) != AINPUT_EVENT_TYPE_KEY) {
		return 0;
	}

	int android_keycode = AKeyEvent_getKeyCode(event);
	int action = AKeyEvent_getAction(event);
	int linux_keycode = android_to_linux_keycode(android_keycode);

	LOGI("按键: android=%d -> linux=%d, action=%d", android_keycode, linux_keycode, action);

	if (action == AKEY_EVENT_ACTION_DOWN) {
		send_key_event(linux_keycode, 1);
	} else if (action == AKEY_EVENT_ACTION_UP) {
		send_key_event(linux_keycode, 0);
	}

	return 1;
}

// ==================== 输入事件回调 ====================
static int32_t on_input_event(struct android_app* app, AInputEvent* event) {
	if(running==0)
		return 0; 
	if (AInputEvent_getType(event) == AINPUT_EVENT_TYPE_MOTION) {
		return handle_touch_event(event);
	} else if (AInputEvent_getType(event) == AINPUT_EVENT_TYPE_KEY) {
		return handle_key_event(event);
	}
	return 0;
}

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

// ==================== 视频解码线程 ====================
void* video_decode_thread(void* arg) {
	LOGI("视频解码线程启动");

	while (running && !nativeWindow) {
		usleep(100000);
	}

	if (!nativeWindow) {
		LOGE("窗口未创建");
		close(videoFd);
		running=0;
		return NULL;
	}
	video_decode(videoFd, nativeWindow, &running);

	close(videoFd);
	LOGI("视频解码线程退出");
	running=0;
	return NULL;
}




void closeall(){
	running = 0;
	pthread_join(videoThread, NULL);
	freeaudio();
	close(audioFd);
	close(videoFd);
	close(touchSocket); 
	if (touchSocket >= 0) {
		close(touchSocket);
	}
}

// ==================== 应用生命周期 ====================

static void on_app_cmd(struct android_app* app, int32_t cmd) {
	switch (cmd) {
		case APP_CMD_PAUSE:
			LOGI("应用进入后台");
			running=0;
			if(touchSocket>0)
				closeall();
			LOGI("后台退出");
			exit(0); 
			break;
		case APP_CMD_RESUME:
			LOGI("应用恢复前台");
			break;
		case APP_CMD_WINDOW_RESIZED:
			LOGI("窗口大小改变");
			break;
		case APP_CMD_WINDOW_REDRAW_NEEDED:
			LOGI("窗口需要重绘");
			running=1;   // ★ 恢复解码
			break;
		case APP_CMD_TERM_WINDOW:
			LOGI("窗口销毁 - 立即暂停解码");
			running = 0;  // ★ 立即暂停
			break;
		default:
			break;
	}
}


// ==================== NativeActivity 入口 ====================
void android_main(struct android_app* app) {
	sprintf(ipip,"%s",IP); 
	int fd=open("/sdcard/scrcpy.txt",0); 
	if(fd>0){
		memset(ipip,0,20);
		int ret=read(fd,ipip,20);
		if(ret<5||strlen(ipip)<6)
			sprintf(ipip,"%s",IP);
		LOGD("读取ip: %s",ipip); 
		close(fd);
	}
    /*
	struct idtime tidt[10];
	idt=tidt;
	memset(idt,0,sizeof(tidt));
    */
	audioFd = tcp_connect(ipip, AUDIO_SERVER_PORT);
	if (audioFd < 0) {
		LOGE("连接音频服务器失败");
		return; 
	}


	videoFd = tcp_connect(ipip, VIDEO_SERVER_PORT);
	if (videoFd < 0) {
		LOGE("连接视频服务器失败");
		close(audioFd);
		return;
	}
	touchSocket = tcp_connect(ipip, TOUCH_RECEIVER_PORT);
	if (touchSocket < 0) {
		LOGE("连接按键服务器失败");
		close(audioFd);
		close(videoFd);
		return; 
	}
	int ret=audio_play(audioFd, &running);
	if(ret==-1){
		running=0;
		return ;
	}
	int flag = 1;
	ret=setsockopt(touchSocket, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
	if(ret<0)
		return ; 
	LOGI("NativeActivity 启动\nip=%s",ipip);
	app->onInputEvent = on_input_event;
	app->onAppCmd = on_app_cmd;  // ★ 添加生命周期回调
	set_fullscreen(app);
	pthread_create(&videoThread, NULL, video_decode_thread, NULL);
	int ident;
	int events;
	struct android_poll_source* source;

	LOGE("进入while");
	while (app->destroyRequested == 0&&running) {

		LOGE("等待ALooper_pollOnce");
		while ((ident = ALooper_pollOnce(0, NULL, &events, (void**)&source)) >= 0) {
			if (source) {
				source->process(app, source);
			}
			LOGE("ALooper_pollOnce内循环");
		}
		LOGE("退出ALooper_pollOnce");
		if (app->window && !nativeWindow) {
			nativeWindow = app->window;
			LOGI("窗口已创建，分辨率: %dx%d", 
					ANativeWindow_getWidth(nativeWindow),
					ANativeWindow_getHeight(nativeWindow));
			SENDER_WIDTH=ANativeWindow_getWidth(nativeWindow);
			SENDER_HEIGHT=ANativeWindow_getHeight(nativeWindow);
			LOGI("========================================");
			LOGI("发送端分辨率: %dx%d", SENDER_WIDTH, SENDER_HEIGHT);
			LOGI("接收端分辨率: %dx%d", RECEIVER_WIDTH, RECEIVER_HEIGHT);
			LOGI("XY转换模式: %d", XY_SWAP_MODE);
			LOGI("IP: %s",ipip);
			switch (XY_SWAP_MODE) {
				case 0:
					LOGI("  模式0: 不转换，直接缩放");
					break;
				case 1:
					LOGI("  模式1: 竖屏转横屏 (Y->X, X->Y)");
					break;
				case 2:
					LOGI("  模式2: 只交换XY，不缩放");
					break;
			}
			LOGI("========================================");
			break; 
		}
		usleep(10000);
	}




	while (running) {
		ident = ALooper_pollOnce(-1, NULL, &events, (void**)&source);
		if(ident<0) 
			break; 
		if (source) {
			source->process(app, source);
		}
	}

	closeall();
	LOGI("NativeActivity 退出");
	return ; 
}
