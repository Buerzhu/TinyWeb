#ifndef WEB_FUNCTION
#define WEB_FUNCTION

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <syslog.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <pthread.h>
#include <netdb.h>
#include <atomic>
#include<unordered_map>
#include <sys/time.h>


const int thread_num=4;//线程数量
#define LISTENQ 1024//listen()函数的参数
#define MAX_EVENT_NUM 65535//epoll_event的事件数
#define EVENT_TABLE_SIZE 4096//epoll_wait()的参数
#define NEW_CONN '0'//发送给子线程的信号，表示有新连接到来
#define CLOSE_DEAD_CONN '1'//发送给子线程的信号，表示关闭空闲的长连接
#define CLOSE_THREAD '2'//发送给子线程的信号，表示结束子线程
#define ALARM_TIME 30//定时时间，也是长连接的超时时间
#define USER_PER_THREAD 500//每个子线程的最大用户数

#define FILENAME_LEN  200//文件名称的最大长度
#define READ_BUFFER_SIZE  256//给每个用户分配的读缓冲区大小
#define WRITE_BUFFER_SIZE  256//给每个用户分配的写缓冲区大小


pthread_mutex_t mutex_conn;


static std::atomic<int> user_count(0);//C++11的原子变量，统计所有线程的当前活跃用户数
static std::atomic<int> max_user_online(0);


void unix_error(char *msg) /* Unix-style error */
{
    fprintf(stderr,"%s: %s\n", msg, strerror(errno));
    exit(0);
}

void app_error(char *msg) /* Application error */
{
    fprintf(stderr, "%s\n", msg);
    exit(0);
}

void show_error(char *msg)
{
    fprintf(stderr, "%s: %s\n", msg, strerror(errno));
}

void show_addr(sockaddr_in address)
{
    int save_errno=errno;
    char host[NI_MAXHOST];
    char service[NI_MAXSERV];
    socklen_t addrlength = sizeof(address );

    int ret=getnameinfo((struct sockaddr *)(&address), sizeof(struct sockaddr),host, sizeof(host), service, sizeof(service),NI_NUMERICHOST|NI_NUMERICSERV);
    if(ret!=0)
    {
        show_error("address changed failed");
    }
    else
    {
        printf("(%s: %s)\n",host,service);
    }
    errno=save_errno;

}

int Open(const char *pathname, int flags, mode_t mode)
{
    int rc;

    if ((rc = open(pathname, flags, mode))  < 0)
    unix_error("Open error");
    return rc;
}

ssize_t Read(int fd, void *buf, size_t count)
{
    ssize_t rc;

    if ((rc = read(fd, buf, count)) < 0)
    unix_error("Read error");
    return rc;
}

ssize_t Write(int fd, const void *buf, size_t count)
{
    ssize_t rc;

    if ((rc = write(fd, buf, count)) < 0)
    unix_error("Write error");
    return rc;
}


void Close(int fd)
{
    int rc;

    if ((rc = close(fd)) < 0)
    unix_error("Close error");
}


static int setnonblocking( int fd )
{
    int old_option = fcntl( fd, F_GETFL );
    int new_option = old_option | O_NONBLOCK;
    fcntl( fd, F_SETFL, new_option );
    return old_option;
}


static void removefd( int epollfd, int fd )//删除事件
{
    epoll_ctl( epollfd, EPOLL_CTL_DEL, fd, 0 );
    Close( fd );
}

static void addfd( int epollfd, int fd )//添加事件
{
    epoll_event event;
    event.data.fd = fd;
    event.events = EPOLLIN | EPOLLET;
    epoll_ctl( epollfd, EPOLL_CTL_ADD, fd, &event );
    setnonblocking( fd );
}

void addfd( int epollfd, int fd, bool one_shot )//添加事件
{
    epoll_event event;
    event.data.fd = fd;
    event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
    if( one_shot )
    {
        event.events |= EPOLLONESHOT;
    }
    epoll_ctl( epollfd, EPOLL_CTL_ADD, fd, &event );
    setnonblocking( fd );
}

void modfd( int epollfd, int fd, int ev )//修改事件
{
    epoll_event event;
    event.data.fd = fd;
    event.events = ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
    epoll_ctl( epollfd, EPOLL_CTL_MOD, fd, &event );
}

void show_sys_time()
{

    time_t timel;
    time(&timel);
    printf("time: %s",asctime(gmtime(&timel)));
}

static void addsig( int sig, void( handler )(int), bool restart = true )
{
    struct sigaction sa;
    memset( &sa, '\0', sizeof( sa ) );
    sa.sa_handler = handler;
    if( restart )
    {
        sa.sa_flags |= SA_RESTART;
    }
    sigfillset( &sa.sa_mask );
    assert( sigaction( sig, &sa, NULL ) != -1 );
}

int open_clientfd(char *ip, char *_port) {

    int port=atoi(_port);

    struct sockaddr_in address;
    bzero( &address, sizeof( address ) );
    address.sin_family = AF_INET;
    inet_pton( AF_INET, ip, &address.sin_addr );
    address.sin_port = htons( port );

    int fd=socket(AF_INET,SOCK_STREAM,0);

    int ret=connect(fd,(struct sockaddr*)&address,sizeof(address));
    if(ret<0)
    {
        app_error("server connected failed");
    }
    else
    {
        return fd;
    }
}

int open_listenfd(char *ip, char *_port,int backlog)//仅ipv4
{
    int port=atoi(_port);

    struct sockaddr_in address;
    bzero( &address, sizeof( address ) );
    address.sin_family = AF_INET;
    inet_pton( AF_INET, ip, &address.sin_addr );
    address.sin_port = htons( port );

    int fd=socket(AF_INET,SOCK_STREAM,0);

    int ret=bind(fd,(struct sockaddr*)&address,sizeof(address));
    assert(ret!=-1);

    ret=listen(fd,backlog);
    if(ret<0)
    {
        unix_error("listenfd opened failed.");
    }
    else
    {
        return fd;
    }

}

int open_listenfd(char *port)//ipv4/ipv6通用
{
    struct addrinfo hints, *listp, *p;
    int listenfd, rc, optval=1;

    /* Get a list of potential server addresses */
    memset(&hints, 0, sizeof(struct addrinfo));
    hints.ai_socktype = SOCK_STREAM;             /* Accept connections */
    hints.ai_flags = AI_PASSIVE | AI_ADDRCONFIG; /* ... on any IP address */
    hints.ai_flags |= AI_NUMERICSERV;            /* ... using port number */
    if ((rc = getaddrinfo(NULL, port, &hints, &listp)) != 0) {
        fprintf(stderr, "getaddrinfo failed (port %s): %s\n", port, gai_strerror(rc));
        return -2;
    }

    /* Walk the list for one that we can bind to */
    for (p = listp; p; p = p->ai_next) {
        /* Create a socket descriptor */
        if ((listenfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol)) < 0)
            continue;  /* Socket failed, try the next */

        /* Eliminates "Address already in use" error from bind */
        setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR,    //line:netp:csapp:setsockopt
                   (const void *)&optval , sizeof(int));

        /* Bind the descriptor to the address */
        if (bind(listenfd, p->ai_addr, p->ai_addrlen) == 0)
            break; /* Success */
        if (close(listenfd) < 0) { /* Bind failed, try the next */
            fprintf(stderr, "open_listenfd close failed: %s\n", strerror(errno));
            return -1;
        }
    }


    /* Clean up */
    freeaddrinfo(listp);
    if (!p) /* No address worked */
        return -1;

    /* Make it a listening socket ready to accept connection requests */
    if (listen(listenfd, LISTENQ) < 0) {
        close(listenfd);
    return -1;
    }
    return listenfd;
}


#endif // WEB_FUNCTION

