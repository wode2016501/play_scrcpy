// tcpserver3.4.c
/* 命令
shell: adb push /usr/local/share/scrcpy/scrcpy-server /data/local/tmp/scrcpy-server-manual.jar1
shell:  adb shell CLASSPATH=/data/local/tmp/scrcpy-server-manual.jar1 app_process / com.genymobile.scrcpy.Server 3.3.3 tunnel_forward=true audio=false control=false cleanup=false video_bit_rate=20000000
shell: cat >cutils.c
int socket_local_client;
 shell: '/media/wode/c1194b4d-35c7-4bc2-b0d8-9ea36982e97f/home/wode/ndk/wode-ndk-28/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android21-clang' cutils.c -o libcutils.so -shared
 shell: '/media/wode/c1194b4d-35c7-4bc2-b0d8-9ea36982e97f/home/wode/ndk/wode-ndk-28/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android21-clang' '/home/wode/src/0/tcpserver3.1.c' -lcutils -L.
 shell: adb push a.out /data/local/tmp
 shell: adb shell chmod 777 /data/local/tmp/a.out
 shell: adb shell /data/local/tmp/a.out 9999 scrcpy
*/
#include <pthread.h>
#include <stdio.h>
#include <arpa/inet.h>  // inet_addr() sockaddr_in
#include <string.h>     // bzero()
#include <sys/socket.h> // socket
#include <unistd.h>
#include <stdlib.h> // exit()
#include <sys/select.h>
#define BUFFER_SIZE 1024 * 1024 * 2
#define TOU_SIZE 500
#include <signal.h>
static pthread_t serjar;
char *shjar = 0;
int socket_local_client(char *a, int b, int c);
int read_yz(int fd, char *p, int size_max);
int read_(int fd, void *p, int size);
int htou = 0; // 77;
int server_socket, client_socket;
int client_lengthmax = 200;
int *client_arr;       // 存储客户端数组
int client_length = 0; // 记录客户端数量
char *tou;             // 存储开头数据
int tousize = 0;
char *scrcpyport;
int scrcpy_fd;
int ret = 0;

void *serjar_thread(void *arg)
{
    system(shjar);
    pthread_detach(pthread_self());
    return 0;
}
void handle_sigpipe(int signo)
{
    fprintf(stderr, "客户端异常退出\n");
    // status = 1;
    //  close(server_socket);
    //  return 0;
}
char *h264 = "h264";

int open_scrcpy_server()
{
    if (scrcpy_fd != -1)
        return 0;
    pthread_create(&serjar, NULL, serjar_thread, NULL);
    printf("打开文件: %s\n", scrcpyport);
    sleep(1);
    scrcpy_fd = socket_local_client(scrcpyport, 0, 1);
    if (scrcpy_fd < 0)
    {
        fprintf(stderr, "打开文件失败: %s\n", scrcpyport);
        return -1;
    }
    printf("打开文件: %s成功\n", scrcpyport);
    char *p = tou;
    tousize = 0;
    if (htou < 1)
    {
        fprintf(stderr, "htou %d 错误\n", htou);
        return -1;
    }
    ret = read_(scrcpy_fd, p, htou);
    if (ret != htou)
    {
        fprintf(stderr, "读取文件失败1: %s\n", scrcpyport);
        return -1;
    }
    printf("读取文件: %s成功\n", scrcpyport);

    p += htou;
    tousize += htou;
    printf("htou %d\n", htou);
    char *type = p - 4;
    for (int i = 0; i < 4; i++)
        printf("%#x ", type[i]);
    printf("\n");
    if (strncmp(type, h264, 4) != 0)
        return 0;

    printf("检测到h264文件\n");
    // 读取长x宽信息
    ret = read_(scrcpy_fd, p, 8);
    if (ret != 8)
    {
        fprintf(stderr, "读取长x宽失败: %s\n", scrcpyport);
        return -1;
    }
    p += 8;
    tousize += 8;
    ret = read_yz(scrcpy_fd, p, TOU_SIZE - tousize);
    if (ret == -1)
    {
        fprintf(stderr, "h264读取第一帧失败: %s\n", scrcpyport);
        return -1;
    }
    tousize += ret; // 更新缓冲区大小
    printf("读取第一帧成功\n");
    for (int i = 0; i < 12; i++)
        printf("%#x ", p[i]);
    printf("\n");

    return 0;
}

/**
 * 添加客户端连接函数
 * @param server_socket 服务器套接字
 * @return 成功返回客户端套接字，失败返回-1
 */
int add_client(int server_socket)
{
    struct sockaddr_in client_addr;  // 客户端地址结构体
    socklen_t addr_size;             // 地址结构体大小
    addr_size = sizeof(client_addr); // 初始化地址结构体大小
    // 接受客户端连接请求
    client_socket = accept(server_socket, (struct sockaddr *)&client_addr, &addr_size);
    // 检查accept是否成功
    if (client_socket < 0)
    {
        printf("accept error\n");
        return -1;
    }
    // 检查客户端数量是否已达上限
    if (client_length >= client_lengthmax)
    {
        printf("客户端已经满\n");
        close(client_socket);
        return -1;
    }

    int ok = -1; // 标记是否找到空位

    // 遍历客户端数组，寻找空位
    for (int i = 0; i < client_lengthmax; i++)
    {
        if (client_arr[i] == -1)
        {
            ok = i; // 标记找到空位
            break;
        }
    }
    // 如果没有找到空位
    if (ok == -1)
    {
        fprintf(stderr, "错误：客户端没空位\n");
        close(client_socket);
        return -1;
    }

    printf("%d 连接成功count %d\n", client_socket, client_length);
    if (scrcpy_fd == -1)
    {
        int ret = open_scrcpy_server();
        if (ret == -1)
        {
            fprintf(stderr, "打开scrcpy_server失败\n");
            close(client_socket);
            return -1;
        }
    }
    write(client_socket, tou, tousize); // 向客户端发送欢迎消息
    client_arr[ok] = client_socket;     // 将客户端套接字存储到客户端数组中
    client_length++;                    // 客户端数量加1
    return client_socket;
}

int main(int argc, char **argv)
{

    int port = 8080;
    char scrcpyports[200];
    char touu[TOU_SIZE]; // 存储开头数据
    tou = touu;
    int client_arrs[200];     // 存储客户端数组
    client_arr = client_arrs; // 存储客户端数组
    scrcpyport = scrcpyports;
    if (argc == 5)
    {
        port = atoi(argv[1]);
        sprintf(scrcpyport, "scrcpy_%08d", atoi(argv[2]));
        htou = atoi(argv[4]);
        shjar = argv[3];
    }
    else
    {
        printf("端口错误\n");
        printf("用法: %s [端口号]  [scrcpy端口] [启动scrcpy_server脚本]【跳过开头字节 默认%d】\n", argv[0], htou);
        exit(1);
    }
    printf("端口 %d htou: %d\n", port, htou);
    signal(SIGPIPE, handle_sigpipe);
    // int fd = -1;
    char listen_addr_str[] = "0.0.0.0";
    size_t listen_addr = inet_addr(listen_addr_str);
    struct sockaddr_in server_addr, client_addr;
    socklen_t addr_size;
    char *buffer = malloc(BUFFER_SIZE); // 缓冲区大小
    if (!buffer)
    {
        printf("内存分配失败\n");
        exit(1);
    }
    for (int i = 0; i < client_lengthmax; i++)
        client_arr[i] = -1;

    int str_length;
    int maxfd = 0;
    fd_set rd_set, wd_set;
    int ret = 0;
    server_socket = socket(PF_INET, SOCK_STREAM, 0); // 创建套接字

    memset(&server_addr, 0, sizeof(server_addr)); // 初始化
    server_addr.sin_family = INADDR_ANY;
    server_addr.sin_port = htons(port);
    server_addr.sin_addr.s_addr = listen_addr;

    if (bind(server_socket, (struct sockaddr *)&server_addr, sizeof(server_addr)) == -1)
    {
        printf("绑定失败\n");
        exit(1);
    }
    if (listen(server_socket, 5) == -1)
    {
        printf("监听失败\n");
        exit(1);
    }

    printf("创建tcp服务器成功\n");
    addr_size = sizeof(client_addr);
    struct timeval wodetime;
    wodetime.tv_sec = 10;
    wodetime.tv_usec = 0;

    char *p = 0;
    int count = 0;
    while (1)
    {
        if (client_length == 0)
        {
            printf("0客户端\n");
            fflush(stdout);
            if (scrcpy_fd != -1)
            {
                close(scrcpy_fd);
                scrcpy_fd = -1;
            }
            ret = add_client(server_socket);
            if (ret == -1)
            {
                fprintf(stderr, "add_client 错误代码: %d\n", ret);
                break;
            }
            continue;
        }
        maxfd = server_socket;
        FD_ZERO(&rd_set);
        FD_SET(server_socket, &rd_set);
        FD_SET(scrcpy_fd, &rd_set);
        if (scrcpy_fd > maxfd)
            maxfd = scrcpy_fd;
        ret = select(maxfd + 1, &rd_set, 0, NULL, 0);
        if (ret < 0)
        {
            perror("错误 select()");
            break;
        }

        if (FD_ISSET(server_socket, &rd_set))
        {
            int ret = add_client(server_socket);
            if (ret == -1)
            {
                fprintf(stderr, "add_client 错误代码: %d\n", ret);
                break;
            }
        }

        if (FD_ISSET(scrcpy_fd, &rd_set))
        {
            str_length = read_yz(scrcpy_fd, buffer, BUFFER_SIZE);
            if (str_length == -1)
            {
                fprintf(stderr, "读取zhen失败\n");
                break;
            }
            FD_ZERO(&wd_set);
            FD_ZERO(&rd_set);
            FD_SET(scrcpy_fd, &rd_set);
            FD_SET(server_socket, &rd_set);
            count = 0;
            for (int i = 0; i < client_lengthmax; ++i)
            {
                if (client_arr[i] == -1)
                {
                    continue;
                }
                FD_SET(client_arr[i], &wd_set);
                if (client_arr[i] > maxfd)
                    maxfd = client_arr[i];
                count++;
                if (count == client_length)
                    break;
            }
            ret = select(maxfd + 1, &rd_set, &wd_set, NULL, &wodetime);
            if (ret < 0)
            {
                perror("select()");
                break;
            }
            count = 0;
            for (int i = 0; i < client_lengthmax; i++)
            {
                if (client_arr[i] == -1)
                {
                    continue;
                }
                if (FD_ISSET(client_arr[i], &wd_set))
                {
                    ret = write(client_arr[i], buffer, str_length); // 发送数据
                    if (str_length != ret)                          // 读取数据完毕关闭套接字
                    {
                        // status = 0;
                        printf("连接已经关闭: %d-1  %d  \n", client_length, client_arr[i]);
                        close(client_arr[i]);
                        client_arr[i] = -1;
                        client_length--;
                    }
                }
                count++;
                if (count == client_length)
                    break;
            }
        }
    }
ext:
    for (int i = 0; i < client_lengthmax; i++)
    {
        if (client_arr[i] == -1)
            continue;
        close(client_arr[i]);
    }
    if (scrcpy_fd != -1)
        close(scrcpy_fd);
    close(server_socket);
    return 0;
}

int read_yz(int fd, char *p, int size_max) // 读取yizhen数据
{
    int size = 0;
    int ret = 0;
    ret = read_(fd, p, 8); // pts
    if (ret != 8)
    {
        return -1;
    }
    size += 8;
    p += 8;
    ret = read_(fd, p, 4); // size
    if (ret != 4)
    {
        return -1;
    }
    size += 4;
    int *len = (int *)p;
    p += 4;
    int len1 = ntohl(*len);
    if (len1 + size > size_max)
    {
        printf("len1 + size %d>%d size_max \n", len1 + size, size_max);
        return -1;
    }
    ret = read_(fd, p, len1);
    if (ret != len1)
    {
        printf("ret != len1 %d!=%d\n", ret, len1);
        return -1;
    }
    size += len1;
    return size;
}
int read_(int fd, void *p, int size)
{
    int y = size;
    int ret = 0;
    while (y > 0)
    {
        ret = read(fd, p, y);
        if (ret < 0)
            return ret;
        y -= ret;
        p += ret;
        /* code */
    }
    return size;
}
