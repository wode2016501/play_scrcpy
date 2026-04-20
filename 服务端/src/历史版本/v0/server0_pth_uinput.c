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

#define PORT 9000
int  SCREEN_WIDTH= 1440;
int  SCREEN_HEIGHT=3168;

// 全局变量
//static int uinput_fd = -1;
static int server_socket = -1;
static int running = 1;

// ==================== 启用所有按键 ====================
void enable_all_keys(int fd) {
	// 字母键 A-Z
	for (int key = KEY_A; key <= KEY_Z; key++) {
		ioctl(fd, UI_SET_KEYBIT, key);
	}

	// 数字键 0-9
	for (int key = KEY_0; key <= KEY_9; key++) {
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

	printf("✓ 已启用所有按键\n");
}

// ==================== 创建虚拟设备 ====================
int create_virtual_device() {
	int fd;
	struct uinput_user_dev uidev;

	fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
	if (fd < 0) {
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
	uidev.absmax[ABS_MT_SLOT] = 9;
	uidev.absmin[ABS_MT_TRACKING_ID] = 0;
	uidev.absmax[ABS_MT_TRACKING_ID] = 65535;

	write(fd, &uidev, sizeof(uidev));

	if (ioctl(fd, UI_DEV_CREATE) < 0) {
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

// ==================== 注入事件到虚拟设备 ====================
int inject_event( int uinput_fd,struct input_event *ev) {
	if (uinput_fd < 0) {
		return -1;
	}

	if (write(uinput_fd, ev, sizeof(struct input_event)) != sizeof(struct input_event)) {
		perror("写入设备失败");
		return -1;
	}
	return 0;
}

// ==================== 接收并注入事件 ====================
int id=0; 
void * receive_thread(void* arg) {
	int uinput_fd = create_virtual_device();
	if (uinput_fd < 0) {
		return ( void *)  1 ;
	}
	int client_fd = *(int*)arg;
	struct input_event ev;
	ssize_t bytes_read;
	int event_count = 0;

	printf("开始接收触摸事件并注入到虚拟设备...\n\n");

	while (running) {
		bytes_read = recv(client_fd, &ev, sizeof(ev), 0);

		if (bytes_read == sizeof(ev)) {
			// 注入到虚拟设备
			if (inject_event(uinput_fd,&ev) == 0) {
				event_count++;
				// 显示事件
				if (ev.type == EV_ABS) {
					if (ev.code == ABS_MT_POSITION_X) {
						printf("[%d] X坐标: %d\n", event_count, ev.value);
					} else if (ev.code == ABS_MT_POSITION_Y) {
						printf("[%d] Y坐标: %d\n", event_count, ev.value);
					} else if (ev.code == ABS_MT_SLOT) {
						printf("[%d] 触摸槽: %d\n", event_count, ev.value);
					} else if (ev.code == ABS_MT_TRACKING_ID) {
						printf("[%d] 跟踪ID: %d\n", event_count, ev.value);
					}
				} else if (ev.type == EV_KEY && ev.code == BTN_TOUCH) {
					printf("[%d] 触摸: %s\n", event_count, ev.value ? "按下" : "抬起");
				} else if (ev.type == EV_KEY && ev.code != BTN_TOUCH) {
					printf("[%d] 按键: code=%d, %s\n", event_count, ev.code, ev.value ? "按下" : "抬起");
				} else if (ev.type == EV_SYN) {
					// 同步事件，不打印
				}
			}
		} else if (bytes_read == 0) {
			printf("连接已断开\n");
			break;
		} else if (bytes_read < 0) {
			perror("接收数据失败\n");
			break;
		}
	}
	if (uinput_fd >= 0) {
		ioctl(uinput_fd, UI_DEV_DESTROY);
		close(uinput_fd);
	}
	close(client_fd);
	return NULL;
}

// ==================== 信号处理 ====================
void signal_handler(int sig) {
	printf("\n收到信号 %d，正在退出...\n", sig);
	running = 0;
	if (server_socket >= 0) {
		close(server_socket);
	}
}

// ==================== 主函数 ====================
int main(int argc,char **argv) {
	if(argc==3){
		SCREEN_WIDTH =atoi(argv[1]);
		SCREEN_HEIGHT= atoi(argv[2]);
		}
	int client_fd;
	struct sockaddr_in server_addr, client_addr;
	socklen_t client_len = sizeof(client_addr);
	pthread_t recv_thread;

	printf("========================================\n");
	printf("统一接收端（设备创建 + 网络接收）\n");
		printf("分辨率%dx%d\n",SCREEN_WIDTH,SCREEN_HEIGHT);
	
	printf("========================================\n\n");

	// 设置信号处理
	signal(SIGINT, signal_handler);
	signal(SIGTERM, signal_handler);
	/* 
	// 1. 创建虚拟设备
	uinput_fd = create_virtual_device();
	if (uinput_fd < 0) {
	return 1;
	}
	*/
	// 显示设备节点
	system("ls -l /dev/input/event* 2>/dev/null | tail -1");
	printf("\n");

	// 2. 创建 socket 服务器
	server_socket = socket(AF_INET, SOCK_STREAM, 0);
	if (server_socket < 0) {
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

	if (bind(server_socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
		perror("绑定失败");
		close(server_socket);
		return 1;
	}

	// 监听
	if (listen(server_socket, 5) < 0) {
		perror("监听失败");
		close(server_socket);
		return 1;
	}

	printf("等待发送端连接，端口: %d...\n", PORT);

	// 接受连接
	while(1){
		client_fd = accept(server_socket, (struct sockaddr*)&client_addr, &client_len);
		if (client_fd < 0) {
			perror("接受连接失败");
			close(server_socket);
			return 1;
		}

		printf("✓ 已连接到发送端: %s\n", inet_ntoa(client_addr.sin_addr));

		// 3. 创建接收线程
		pthread_create(&recv_thread, NULL, receive_thread, &client_fd);
	}
	// 4. 等待退出信号
	/*
	   while (running) {
	   sleep(1);
	   }

	// 5. 清理
	//pthread_join(recv_thread, NULL);

	if (uinput_fd >= 0) {
	ioctl(uinput_fd, UI_DEV_DESTROY);
	close(uinput_fd);
	}
	*/ 
	if (server_socket >= 0) {
		close(server_socket);
	}

	printf("设备已销毁，程序退出\n");
	return 0;
}
