//tcpserver3.4.c
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
char *shjar=0; 
int socket_local_client(char *a, int b, int c);
int read_yz(int fd, char *p, int size_max);
int read_(int fd, void *p, int size);
int htou = 0; // 77;
int server_socket, client_socket;
// int status = 0;


void* serjar_thread(void* arg) {
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
int main(int argc, char **argv)
{
    int port = 8080;
      char scrcpyport[200]; 
    if(argc==5){
    	    port = atoi(argv[1]);
    	    sprintf(scrcpyport,"scrcpy_%08d",atoi(argv[2])); 
    	    htou = atoi(argv[4]);
    	    shjar=argv[3]; 
    }
    else
     {
        printf("端口错误\n");
        printf("用法: %s [端口号]  [scrcpy端口] [启动scrcpy_server脚本]【跳过开头字节 默认%d】\n", argv[0], htou);
        exit(1);
    }
    printf("端口 %d htou: %d\n", port, htou);
    signal(SIGPIPE, handle_sigpipe);
    int fd = -1;
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
    int client_lengthmax = 200;
    int client_arr[200]; // 存储客户端数组
    for (int i = 0; i < client_lengthmax; i++)
        client_arr[i] = -1;
    int client_length = 0; // 记录客户端数量
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
    char tou[TOU_SIZE];
    int tousize = 0;
    char *p = 0;
    int count = 0;
    while (1)
    {
        if (client_length == 0)
        {
            printf("0客户端\n");
            fflush(stdout);
            if (fd != -1)
            {
                close(fd);
                fd = -1;
            }
            client_socket = accept(server_socket, (struct sockaddr *)&client_addr, &addr_size);
            if (client_socket < 0)
            {
                printf("accept error\n");
                close(server_socket);
                return 0;
            }
              pthread_create(&serjar, NULL, serjar_thread, NULL);
            client_arr[0] = client_socket;
            client_length++;
                    printf("打开文件: %s\n", scrcpyport);
            fflush(stdout);
            sleep(1); 
            fd = socket_local_client(scrcpyport, 0, 1);
            if (fd < 0)
            {
                printf("打开文件失败: %s\n", argv[2]);
                close(client_socket);
                close(server_socket);
                return 0;
            }
            printf("打开文件: %s成功\n", argv[2]);
            fflush(stdout);

            char *p = tou;
            tousize = 0;
            if (htou < 1)
            {
                sleep(1);
                ret = read(fd, p, BUFFER_SIZE);
                continue;
            }
            ret = read_(fd, p, htou);
            if (ret != htou)
            {
                printf("读取文件失败1: %s\n", argv[2]);
                close(server_socket);
                return 0;
            }
            printf("读取文件: %s成功\n", argv[2]);
            fflush(stdout);

            p += htou;
            tousize += htou;
            printf("htou %d\n", htou);
            char *type = p - 4;
            for (int i = 0; i < 4; i++)
                printf("%#x ", type[i]);
                printf("\n");
            if (strncmp(type, h264, 4) == 0)
            {
                printf("检测到h264文件\n");
                // 读取长x宽信息
                ret = read_(fd, p, 8);
                if (ret != 8)
                {
                    printf("读取文件失败2: %s\n", argv[2]);
                    close(server_socket);
                    return 0;
                }
                p += 8;
                tousize += 8;
                ret = read_yz(fd, p, TOU_SIZE - tousize);
                if (ret == -1)
                {
                    printf("读取文件失败2: %s\n", argv[2]);
                    close(server_socket);
                    return 0;
                }
                for (int i = 0; i < 12; i++)
                    printf("%#x ", p[i]);
                printf("\n");
                tousize += ret;
            }

            printf("tousize %d\n", tousize);
            write(client_socket, tou, tousize);
            continue;
        }
        maxfd = server_socket;
        // FD_ZERO(&wd_set);
        FD_ZERO(&rd_set);
        FD_SET(server_socket, &rd_set);
        FD_SET(fd, &rd_set);
        if (fd > maxfd)
            maxfd = fd;
        count = 0;
        for (int i = 0; i < client_lengthmax; ++i)
        {
            if (client_arr[i] == -1)
            {
                continue;
            }
            FD_SET(client_arr[i], &rd_set);
            if (client_arr[i] > maxfd)
                maxfd = client_arr[i];
            count++;
            if (count == client_length)
                break;
        }

        ret = select(maxfd + 1, &rd_set, 0, NULL, 0);
        if (ret < 0)
        {
            printf("select 错误代码: %d\n", ret);
            perror("select()");
            for (int i = 0; i < client_lengthmax; i++)
            {
                if (client_arr[i] == -1)
                    continue;
                close(client_arr[i]);
            }
            close(server_socket);
            exit(1);
        }
        if (FD_ISSET(server_socket, &rd_set))
        {
            client_socket = accept(server_socket, (struct sockaddr *)&client_addr, &addr_size);
            if (client_length >= client_lengthmax)
            {
                char *msgs = "客户端已经满\n";
                printf("%s", msgs);
                close(client_socket);
            }
            else
            {
                for (int i = 0; i < client_lengthmax; i++)
                {
                    if (client_arr[i] == -1)
                    {
                        client_arr[i] = client_socket;
                        break;
                    }
                }
                client_length++;
                printf("%d 连接成功count %d\n", client_socket, client_length);
                write(client_socket, tou, tousize);
            }
        }
        if (FD_ISSET(fd, &rd_set))
        {
            // printf("读取fd数据:\n");
            str_length = read_yz(fd, buffer, BUFFER_SIZE);
            if (str_length == -1)
            {
                printf("读取文件失败: %s\n", argv[2]);
                for (int i = 0; i < client_lengthmax; i++)
                {
                    if (client_arr[i] == -1)
                        continue;
                    close(client_arr[i]);
                    client_length--;
                }
                close(fd);
                fd = -1;
                continue;
            }
            FD_ZERO(&wd_set);
            FD_ZERO(&rd_set);
            FD_SET(fd, &rd_set);
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
                fprintf(stderr, "select 错误代码: %d\n", ret);
                perror("select()");
                for (int i = 0; i < client_lengthmax; i++)
                {
                    if (client_arr[i] == -1)
                        continue;
                    close(client_arr[i]);
                }
                close(server_socket);
                exit(1);
            }
            count = 0;
            for (int i = 0; i < client_lengthmax; i++)
            {
                if (client_arr[i] == -1)
                {
                    continue;
                }
                // printf("client_arr[%d] = %d\n", i, client_arr[i]);
                if (FD_ISSET(client_arr[i], &wd_set))
                {
                    // ret=str_length;
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
        count = 0;
        for (int i = 0; i < client_lengthmax; i++)
        {
            if (client_arr[i] == -1)
            {
                continue;
            }
            // printf("client_arr[%d] = %d\n", i, client_arr[i]);
            if (FD_ISSET(client_arr[i], &rd_set))
            {
                // ret=str_length;
                ret = read(client_arr[i], buffer, BUFFER_SIZE); // 发送数据
                if (ret < 1)                                    // 读取数据完毕关闭套接字
                {
                    // status = 0;
                    printf("连接已经关闭: %d-1  %d  \n", client_length, client_arr[i]);
                    close(client_arr[i]);
                    client_arr[i] = -1;
                    client_length--;
                    continue;
                }
                ret = write(fd, buffer, ret); // 发送数据
                if (ret < 1)
                {
                    printf("写入文件失败: %s,%d\n", argv[2], ret);
                    for (int i = 0; i < client_lengthmax; i++)
                    {
                        if (client_arr[i] == -1)
                            continue;
                        close(client_arr[i]);
                        client_length--;
                    }
                    close(fd);
                    fd = -1;
                    continue;
                }
            }
            count++;
            if (count >= client_length)
                break;
        }
    }
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
    /*
        len = (int *)p;
        if (*len != 16777216) //h264
        {
            return -1;
        }
    */
    /*
        for(int i=0;i<5;i++){
            printf("%#x ",p[i]);
        }
        printf("\n");
    */
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
