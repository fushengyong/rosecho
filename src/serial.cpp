#include "serial.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include <boost/bind.hpp>

#define DBG_LOG_TRACE(format, ...) \
    printf(format, ##__VA_ARGS__); \
    printf("\n")
#define DBG_LOG_DEBUG(format, ...) \
    printf(format, ##__VA_ARGS__); \
    printf("\n")
#define DBG_LOG_ERROR(format, ...) \
    printf(format, ##__VA_ARGS__); \
    printf("\n")
#define DBG_LOG_WARNING(format, ...) \
    printf(format, ##__VA_ARGS__);   \
    printf("\n")

static void *serial_recv(void *param)
{
    struct thread_param *p = (struct thread_param *)param;
    uint8_t recvbuff[1024];
    int recvlen = 0;
    Serial *pThis = (Serial *)p->param1;
    while (pThis->running)
    {
        memset(recvbuff, 0, sizeof(recvbuff));

        if ((recvlen = read(pThis->fd, recvbuff, sizeof(recvbuff))) == -1)
        {
            DBG_LOG_ERROR("uart_recv error:%d", errno);
            continue;
        }
        pThis->recv_cb(recvbuff, recvlen, p->param2);
    }
    return NULL;
}

bool Serial::config(int speed, int flow_ctrl, int databits, int stopbits,
                    int parity)
{
    int i;
    int status;
    int speed_arr[] = {B115200, B57600, B38400, B19200, B9600};
    int name_arr[] = {115200, 57600, 38400, 19200, 9600};

    struct termios options;

    //获取设备属性信息
    if (tcgetattr(fd, &options) != 0)
    {
        perror("SetupSerial");
        return false;
    }

    //设置串口输入波特率和输出波特率 /i o 入和出
    for (i = 0; i < sizeof(speed_arr) / sizeof(int); i++)
    {
        if (speed == name_arr[i])
        {
            cfsetispeed(&options, speed_arr[i]);
            cfsetospeed(&options, speed_arr[i]);
        }
    }

    //修改控制模式，保证程序不会占用串口
    options.c_cflag |= CLOCAL;
    //修改控制模式，使得能够从串口中读取输入数据
    options.c_cflag |= CREAD;

    //设置数据流控制
    switch (flow_ctrl)
    {
    case 0: //不使用流控制
        options.c_cflag &= ~CRTSCTS;
        break;
    case 1: //使用硬件流控制
        options.c_cflag |= CRTSCTS;
        break;
    case 2: //使用软件流控制
        options.c_cflag |= IXON | IXOFF | IXANY;
        break;
    default:
        fprintf(stderr, "Unsupported flow ctrl\n");
        return false;
    }
    //设置数据位
    options.c_cflag &= ~CSIZE; //屏蔽其他标志位
    switch (databits)
    {
    case 5:
        options.c_cflag |= CS5;
        break;
    case 6:
        options.c_cflag |= CS6;
        break;
    case 7:
        options.c_cflag |= CS7;
        break;
    case 8:
        options.c_cflag |= CS8;
        break;
    default:
        fprintf(stderr, "Unsupported data size\n");
        return false;
    }
    //设置校验位
    switch (parity)
    {
    case 'n':
    case 'N': //无奇偶校验位。
        options.c_cflag &= ~PARENB;
        options.c_iflag &= ~INPCK;
        break;
    case 'o':
    case 'O': //设置为奇校验
        options.c_cflag |= (PARODD | PARENB);
        options.c_iflag |= INPCK;
        break;
    case 'e':
    case 'E': //设置为偶校验
        options.c_cflag |= PARENB;
        options.c_cflag &= ~PARODD;
        options.c_iflag |= INPCK;
        break;
    case 's':
    case 'S': //设置为空格
        options.c_cflag &= ~PARENB;
        options.c_cflag &= ~CSTOPB;
        break;
    default:
        fprintf(stderr, "Unsupported parity\n");
        return false;
    }
    // 设置停止位
    switch (stopbits)
    {
    case 1:
        options.c_cflag &= ~CSTOPB;
        break;
    case 2:
        options.c_cflag |= CSTOPB;
        break;
    default:
        fprintf(stderr, "Unsupported stop bits\n");
        return false;
    }

    //修改输出模式，原始数据输出
    options.c_oflag &= ~OPOST;

    options.c_lflag |= ~(ICANON | ECHOE | ECHO | ISIG);
    //(ICANON | ECHO | ECHOE);

    //设置等待时间和最小接收字符
    options.c_cc[VTIME] = 1; /* 读取一个字符等待1*(1/10)s */
    options.c_cc[VMIN] = 1;  /* 读取字符的最少个数为1 */

    //如果发生数据溢出，接收数据，但是不再读取
    tcflush(fd, TCIFLUSH);

    //激活配置 (将修改后的termios数据设置到串口中）
    if (tcsetattr(fd, TCSANOW, &options) != 0)
    {
        perror("com set error!/n");
        return false;
    }
    return true;
}

bool Serial::open(const char *device, int rate, int flow_ctrl, int databits,
                  int stopbits, int parity, serial_recv_cb cb, void* param)
{
    int ret;
    pthread_attr_t attr;
    struct thread_param tparam = {this, param};
    recv_thread = 0;
    DBG_LOG_TRACE("open serial %s start\n", device);
    //句柄检查
    if (NULL == device || NULL == cb)
    {
        DBG_LOG_ERROR("NULL == device || NULL == recv_callback\n");
        return false;
    }

    recv_cb = cb;

    //打开设备
    fd = ::open(device, O_RDWR);
    if (fd < 0)
    {
        DBG_LOG_ERROR("open failed: %s\n", device);
        return false;
    }
    //设定属性
    if (config(rate, flow_ctrl, databits, stopbits, parity) == false)
    {
        goto error;
    }
    running = 1;

    //创建线程，接收数据
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED); //分离属性
    ret = pthread_create(&recv_thread, &attr, serial_recv, &tparam);
    if (0 != ret)
    {
        DBG_LOG_ERROR("uart recv thread create failed!\n");
        goto error;
    }

    DBG_LOG_TRACE("open serial %s end\n", device);
    return true;

error:
    running = 0;
    if (fd > 0)
    {
        ::close(fd);
    }
    if (0 != recv_thread)
    {
        pthread_join(recv_thread, NULL);
    }
    return false;
}

int Serial::send(uint8_t *data, int len)
{
    int ret = 0;
    int sended_len = 0;

    if (fd <= 0)
    {
        return -1;
    }

    //反复发送，直到全部发完
    while (sended_len != len)
    {
        int retlen = 0;
        retlen = write(fd, data + sended_len, len - sended_len);
        if (retlen < 0)
        {
            DBG_LOG_ERROR("serial send failed! ret=%d\n", ret);
            return -1;
        }
        sended_len += retlen;
    }

    return ret;
}

void Serial::close(void)
{
    running = 0;
    if (fd > 0)
    {
        ::close(fd);
    }
    if (0 != recv_thread)
    {
        pthread_join(recv_thread, NULL);
    }
}