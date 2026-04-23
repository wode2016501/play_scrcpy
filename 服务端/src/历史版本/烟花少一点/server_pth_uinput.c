// unified_receiver.c - 统一接收端（设备创建 + 网络接收）
#include <linux/input.h>
#include <linux/uinput.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <signal.h>
//#define printf(...) printf( LOG_TAG, __VA_ARGS__)
//#define fprintf(stderr,...) fprintf(stderr, LOG_TAG, __VA_ARGS__)
//#define printf(...) printf(  __VA_ARGS__)

#define PORT 9000
int SCREEN_WIDTH = 2376;
int SCREEN_HEIGHT = 1080;
// ==================== 触摸点管理 ====================
typedef struct
{
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

// ==================== 自定义固定大小结构体 ====================
typedef struct
{
	long long tv_sec;
	long long tv_usec;
	unsigned short type;
	unsigned short code;
	unsigned int value;
} input_event_test;

// 全局变量
static int uinput_fd = -1;
int event_count = 0;
static int server_socket = -1;
static int running = 1;
static TouchPoint touchPoints[100];
static int touchCount = 0;
static pthread_mutex_t touchMutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t uinputMutex = PTHREAD_MUTEX_INITIALIZER;


//延时
struct idtime {
	struct timespec prev_ts;
	struct timespec now;
};
/*
   struct idtime {
   struct timespec prev_ts;
   struct timespec now;
   };
   struct idtime tidt[10];
   struct idtime *idt = tidt;   // 指向数组首元素
   memset(idt, 0, sizeof(tidt));  // 所有成员初始为0

   for (int i = 0; i < count; i++) {
   clock_gettime(CLOCK_MONOTONIC, &idt[i].now);   // 注意用 . 而不是 ->
   long long interval_us = 0;
   if (idt[i].prev_ts.tv_sec != 0 || idt[i].prev_ts.tv_nsec != 0) {  // 避免初始0导致的误判
   interval_us = (idt[i].now.tv_sec - idt[i].prev_ts.tv_sec) * 1000000LL +
   (idt[i].now.tv_nsec - idt[i].prev_ts.tv_nsec) / 1000;
   }
   if (interval_us > 0 && interval_us < 3000) {
// 间隔太小，跳过此事件（不写入）
continue;   // 而不是 return
}
idt[i].prev_ts = idt[i].now;
// ... 写入 uinput ...
}
*/





// ==================== 注入事件到虚拟设备 ====================
int inject_event(int uinput_fd, struct input_event *ev)
{
	if (uinput_fd < 0)
	{
		return -1;
	}
	if (write(uinput_fd, ev, sizeof(struct input_event)) != sizeof(struct input_event))
	{
		perror("写入设备失败");
		return -1;
	}
	return 0;
}

// 使用自定义结构体发送
int send_input_event_test(int type, int code, int value)
{
	input_event_test test_ev;
	struct timeval tv;
	gettimeofday(&tv, NULL);
	test_ev.tv_sec = tv.tv_sec;
	test_ev.tv_usec = tv.tv_usec;
	test_ev.type = type;
	test_ev.code = code;
	test_ev.value = value;
	pthread_mutex_lock(&uinputMutex);
	if (inject_event(uinput_fd, &test_ev) == 0)
	{
		event_count++;
	}
	pthread_mutex_unlock(&uinputMutex);
	return 0;
}

// 发送触摸事件（带坐标转换）
void send_touch_event(int id, int x, int y, int action)
{
	if (action == 0)
	{ // 按下
		send_input_event_test(EV_ABS, ABS_MT_SLOT, id % 10);
		send_input_event_test(EV_ABS, ABS_MT_TRACKING_ID, id);
		send_input_event_test(EV_ABS, ABS_MT_POSITION_X, x);
		send_input_event_test(EV_ABS, ABS_MT_POSITION_Y, y);
		send_input_event_test(EV_KEY, BTN_TOUCH, 1);
	}
	else if (action == 1)
	{ // 移动
		send_input_event_test(EV_ABS, ABS_MT_SLOT, id % 10);
		send_input_event_test(EV_ABS, ABS_MT_POSITION_X, x);
		send_input_event_test(EV_ABS, ABS_MT_POSITION_Y, y);
	}
	else if (action == 2)
	{ // 抬起
		send_input_event_test(EV_ABS, ABS_MT_SLOT, id % 10);
		send_input_event_test(EV_ABS, ABS_MT_TRACKING_ID, -1);

		// 检查是否还有活动手指
		int hasActive = 0;
		pthread_mutex_lock(&touchMutex);
		for (int i = 0; i < touchCount; i++)
		{
			if (touchPoints[i].active && touchPoints[i].id != id)
			{
				hasActive = 1;
				break;
			}
		}
		pthread_mutex_unlock(&touchMutex);

		if (!hasActive)
		{
			send_input_event_test(EV_KEY, BTN_TOUCH, 0);
		}
	}

	// SYN_REPORT
	send_input_event_test(EV_SYN, SYN_REPORT, 0);
}

// 发送按键事件
void send_key_event(int keyCode, int action)
{
	send_input_event_test(EV_KEY, keyCode, action);
	send_input_event_test(EV_SYN, SYN_REPORT, 0);
	printf("发送按键: code=%d, action=%s", keyCode, action ? "DOWN" : "UP");
}

// ==================== 触摸点管理 ====================
void add_touch_point(int id, int x, int y)
{
	pthread_mutex_lock(&touchMutex);
	if (touchCount < 10)
	{
		touchPoints[touchCount].id = id;
		touchPoints[touchCount].x = x;
		touchPoints[touchCount].y = y;
		touchPoints[touchCount].active = 1;
		touchCount++;
		printf("+ 手指%d: (%d,%d) 总数=%d", id, x, y, touchCount);
	}
	pthread_mutex_unlock(&touchMutex);
}

void remove_touch_point(int id)
{
	pthread_mutex_lock(&touchMutex);
	for (int i = 0; i < touchCount; i++)
	{
		if (touchPoints[i].id == id)
		{
			for (int j = i; j < touchCount - 1; j++)
			{
				touchPoints[j] = touchPoints[j + 1];
			}
			touchCount--;
			printf("- 手指%d 剩余=%d", id, touchCount);
			break;
		}
	}
	pthread_mutex_unlock(&touchMutex);
}

void clear_all_touch_points()
{
	pthread_mutex_lock(&touchMutex);
	touchCount = 0;
	memset(touchPoints, 0, sizeof(touchPoints));
	pthread_mutex_unlock(&touchMutex);
}

// ==================== 启用所有按键 ====================
void enable_all_keys(int fd)
{
	for (int key = 0; key <= 248; key++)
	{
		ioctl(fd, UI_SET_KEYBIT, key);
	}
	/*
	// 字母键 A-Z
	for (int key = KEY_A; key <= KEY_Z; key++)
	{
	ioctl(fd, UI_SET_KEYBIT, key);
	}

	// 数字键 0-9
	for (int key = KEY_0; key <= KEY_9; key++)
	{
	ioctl(fd, UI_SET_KEYBIT, key);
	}

	// 功能键
	ioctl(fd, UI_SET_KEYBIT, KEY_ENTER);
	ioctl(fd, UI_SET_KEYBIT, KEY_SPACE);
	ioctl(fd, UI_SET_KEYBIT, KEY_BACKSPACE);
	ioctl(fd, UI_SET_KEYBIT, KEY_TAB);
	ioctl(fd, UI_SET_KEYBIT, KEY_ESC);
	ioctl(fd, UI_SET_KEYBIT, KEY_DELETE);

	// 方向键
	ioctl(fd, UI_SET_KEYBIT, KEY_UP);
	ioctl(fd, UI_SET_KEYBIT, KEY_DOWN);
	ioctl(fd, UI_SET_KEYBIT, KEY_LEFT);
	ioctl(fd, UI_SET_KEYBIT, KEY_RIGHT);

	// 系统键
	ioctl(fd, UI_SET_KEYBIT, KEY_HOME);
	ioctl(fd, UI_SET_KEYBIT, KEY_BACK);
	ioctl(fd, UI_SET_KEYBIT, KEY_MENU);
	ioctl(fd, UI_SET_KEYBIT, KEY_VOLUMEUP);
	ioctl(fd, UI_SET_KEYBIT, KEY_VOLUMEDOWN);
	ioctl(fd, UI_SET_KEYBIT, KEY_POWER);
	ioctl(fd, UI_SET_KEYBIT, KEY_CAMERA);

	// 修饰键
	ioctl(fd, UI_SET_KEYBIT, KEY_LEFTSHIFT);
	ioctl(fd, UI_SET_KEYBIT, KEY_RIGHTSHIFT);
	ioctl(fd, UI_SET_KEYBIT, KEY_LEFTCTRL);
	ioctl(fd, UI_SET_KEYBIT, KEY_RIGHTCTRL);
	ioctl(fd, UI_SET_KEYBIT, KEY_LEFTALT);
	ioctl(fd, UI_SET_KEYBIT, KEY_RIGHTALT);
	*/
	printf("✓ 已启用所有按键\n");
}

// ==================== 创建虚拟设备 ====================
int create_virtual_device()
{
	int fd;
	struct uinput_user_dev uidev;

	fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
	if (fd < 0)
	{
		perror("打开 /dev/uinput 失败");
		return -1;
	}

	printf("配置虚拟输入设备...\n");

	// 1. 配置触摸事件
	ioctl(fd, UI_SET_EVBIT, EV_KEY);
	ioctl(fd, UI_SET_KEYBIT, BTN_TOUCH);
	ioctl(fd, UI_SET_EVBIT, EV_ABS);

	ioctl(fd, UI_SET_ABSBIT, ABS_MT_SLOT);
	ioctl(fd, UI_SET_ABSBIT, ABS_MT_TRACKING_ID);
	ioctl(fd, UI_SET_ABSBIT, ABS_MT_POSITION_X);
	ioctl(fd, UI_SET_ABSBIT, ABS_MT_POSITION_Y);

	ioctl(fd, UI_SET_PROPBIT, INPUT_PROP_DIRECT);

	// 2. 配置按键事件
	enable_all_keys(fd);

	// 3. 配置设备参数
	memset(&uidev, 0, sizeof(uidev));
	strcpy(uidev.name, "Virtual Touch + Keyboard");
	uidev.id.bustype = BUS_USB;
	uidev.id.vendor = 0x0eef;
	uidev.id.product = 0x0002;
	uidev.id.version = 1;

	// 触摸坐标范围
	uidev.absmin[ABS_MT_POSITION_X] = 0;
	uidev.absmax[ABS_MT_POSITION_X] = SCREEN_WIDTH - 1;
	uidev.absmin[ABS_MT_POSITION_Y] = 0;
	uidev.absmax[ABS_MT_POSITION_Y] = SCREEN_HEIGHT - 1;
	uidev.absmin[ABS_MT_SLOT] = 0;
	uidev.absmax[ABS_MT_SLOT] = 9; //最大支持10个触点
	uidev.absmin[ABS_MT_TRACKING_ID] = 0;
	uidev.absmax[ABS_MT_TRACKING_ID] = 65535;

	write(fd, &uidev, sizeof(uidev));

	if (ioctl(fd, UI_DEV_CREATE) < 0)
	{
		perror("创建设备失败");
		close(fd);
		return -1;
	}

	printf("\n========================================\n");
	printf("✓ 虚拟设备创建成功！\n");
	printf("  设备名称: Virtual Touch + Keyboard\n");
	printf("  分辨率: %dx%d\n", SCREEN_WIDTH, SCREEN_HEIGHT);
	printf("  支持: 触摸屏 + 键盘按键\n");
	printf("========================================\n\n");

	return fd;
}
int read_(int fd, char *buf, size_t size, int max_size)
{
	if (size > max_size)
		return -1;
	int y = size;
	int ret = 0;
	while (y > 0)
	{
		ret = read(fd, buf, y);
		if (ret < 0)
			return -1;
		if (ret == 0)
			return -1;
		buf += ret;
		y -= ret;
	}
	return size;
}
// ==================== 接收并注入事件 ====================
void *receive_thread(void *arg)
{
	int client_fd = *(int *)arg;
	struct input_event ev;
	ssize_t bytes_read;
	TouchPoint tp;
	KeyPoint kp;
	printf("开始接收触摸事件并注入到虚拟设备...\n\n");
	int size = 0;
	int eventCount = 0;
	int eventmax = 0;
	int id[10];
	memset(id, 0, sizeof(id));
	struct idtime tidt[10];
	struct idtime *idt = tidt;   // 指向数组首元素
	memset(idt, 0, sizeof(tidt));  // 所有成员初始为0
	long long interval_us = 0;


	while (running)
	{
		bytes_read = read_(client_fd, &size, sizeof(int), sizeof(int));
		if (size == sizeof(TouchPoint))
		{
			bytes_read = read_(client_fd, &tp, sizeof(TouchPoint), sizeof(TouchPoint));
			if (bytes_read == sizeof(TouchPoint))
			{
				if(	tp.id<10){
					clock_gettime(CLOCK_MONOTONIC, &idt[tp.id].now);   // 注意用 . 而不是 ->
					interval_us = (idt[tp.id].now.tv_sec - idt[tp.id].prev_ts.tv_sec) * 1000000LL +
						(idt[tp.id].now.tv_nsec - idt[tp.id].prev_ts.tv_nsec) / 1000;
					idt[tp.id].prev_ts = idt[tp.id].now;
					if (interval_us < 4000) 
						usleep(4000); 
				}


				tp.id+=client_fd;
				printf("接收触摸事件: id=%d, x=%d, y=%d, action=%d,%dms\n", tp.id, tp.x, tp.y, tp.active,interval_us );
				send_touch_event(tp.id, tp.x, tp.y, tp.active);
				if (tp.active == 0)
				{
					id[eventCount] = tp.id;
					eventCount++;
					eventmax++;
					add_touch_point(tp.id, tp.x, tp.y);
				}
				if (tp.active == 2)
				{
					eventCount--;
					remove_touch_point(tp.id);
				}
				continue;
			}
			else
			{
				fprintf(stderr,"读取触摸事件失败\n");
				break;
			}
		}
		if (size == sizeof(KeyPoint))
		{
			bytes_read = read_(client_fd, &kp, sizeof(KeyPoint), sizeof(KeyPoint));
			if (bytes_read == sizeof(KeyPoint))
			{
				printf("接收按键事件: keyCode=%d, action=%d\n", kp.keyCode, kp.active);
				send_key_event(kp.keyCode, kp.active);
				continue;
			}
			else
			{
				fprintf(stderr,"读取按键事件失败\n");
				break;
			}
		}
		break;
	}
	if (eventCount > 0)
	{
		printf("还有%d个未抬起的触摸点，正在清理...\n", eventCount);
		for (int i = 0; i < touchCount; i++)
		{
			for (int j = 0; j < eventmax; j++)
			{
				if (touchPoints[i].id == id[j])
				{
					printf("清理触摸点 id=%d\n", touchPoints[i].id);
					send_touch_event(touchPoints[i].id, touchPoints[i].x, touchPoints[i].y, 2);
					eventCount--;
					if (eventCount <= 0)
					{
						break;
					}
				}
			}
		}
	}
	if (eventCount > 0)
	{
		fprintf(stderr,"警告：仍有%d个触摸点未正确抬起！\n", eventCount);
		running = 0;
	}
	close(client_fd);
	return NULL;
}

// ==================== 信号处理 ====================
void signal_handler(int sig)
{
	printf("\n收到信号 %d，正在退出...\n", sig);
	running = 0;
	if (server_socket >= 0)
	{
		close(server_socket);
	}
}

// ==================== 主函数 ====================
int main(int argc, char **argv)
{
	if (argc == 3)
	{
		SCREEN_WIDTH = atoi(argv[1]);
		SCREEN_HEIGHT = atoi(argv[2]);
	}
	int client_fd;
	struct sockaddr_in server_addr, client_addr;
	socklen_t client_len = sizeof(client_addr);
	pthread_t recv_thread;

	printf("========================================\n");
	printf("统一接收端（设备创建 + 网络接收）\n");
	printf("分辨率%dx%d\n", SCREEN_WIDTH, SCREEN_HEIGHT);

	printf("========================================\n\n");

	// 设置信号处理
	signal(SIGINT, signal_handler);
	signal(SIGTERM, signal_handler);



	// 2. 创建 socket 服务器
	server_socket = socket(AF_INET, SOCK_STREAM, 0);
	if (server_socket < 0)
	{
		perror("创建 socket 失败");
		return 1;
	}

	// 设置端口重用
	int opt = 1;
	setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

	// 绑定地址
	memset(&server_addr, 0, sizeof(server_addr));
	server_addr.sin_family = AF_INET;
	server_addr.sin_addr.s_addr = INADDR_ANY;
	server_addr.sin_port = htons(PORT);

	if (bind(server_socket, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0)
	{
		perror("绑定失败");
		close(server_socket);
		return 1;
	}

	// 监听
	if (listen(server_socket, 5) < 0)
	{
		perror("监听失败");
		close(server_socket);
		return 1;
	}

	printf("等待发送端连接，端口: %d...\n", PORT);


	// 1. 创建虚拟设备
	uinput_fd = create_virtual_device();
	if (uinput_fd < 0)
	{
		close(server_socket);
		return 1;
	}

	// 显示设备节点
	system("ls -l /dev/input/event* 2>/dev/null | tail -1");
	printf("\n");

	// 接受连接
	while (1)
	{
		client_fd = accept(server_socket, (struct sockaddr *)&client_addr, &client_len);
		if (client_fd < 0)
		{
			perror("接受连接失败");
			close(server_socket);
			return 1;
		}

		printf("✓ 已连接到发送端: %s\n", inet_ntoa(client_addr.sin_addr));

		// 3. 创建接收线程
		pthread_create(&recv_thread, NULL, receive_thread, &client_fd);
	}
	if (server_socket >= 0)
	{
		close(server_socket);
	}

	printf("设备已销毁，程序退出\n");
	return 0;
}
