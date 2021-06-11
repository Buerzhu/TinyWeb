#ifndef WEB_THREAD_H
#define WEB_THREAD_H

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
#include <signal.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include<time.h>
#include<queue>
#include<map>
#include<stack>
#include<pthread.h>
#include"http_conn.h"
#include"web_function.h"

using namespace std;

const char* server_busy_info="HTTP/1.1 503 Service Unavailable\r\n\r\n";//当子线程的用户数达到上限值时向新连接发送该信息

struct timer_cmp
{
    bool operator () (util_timer* a,util_timer* b)
    {
        return a->expire>b->expire;
    }
};

template<typename T>
class webthread
{
public:
    webthread() : user_num(0),stop(false)
    {
        epollfd = epoll_create(EVENT_TABLE_SIZE);
        assert( epollfd != -1 );

        int ret = socketpair( PF_UNIX, SOCK_STREAM, 0, pipefd);
        assert( ret != -1 );

        setnonblocking( pipefd[1] );
        addfd( epollfd, pipefd[0] );
        users=new T[USER_PER_THREAD];//提前分配用户空间
    }
    ~webthread()
    {
        Close(pipefd[1]);
        Close(pipefd[0]);
        Close(epollfd);
    }

public:
    pthread_t tid;
    int pipefd[2];//通过该管道实现子线程与主线程的双向通信
    static int listenfd;//主线程中的监听描述符
    static int m_epollfd;//主线程的epollfd，在工作线程中访问该epollfd来重置listenfd的事件

    void work();
private:
    int user_num;//当前在线客户端数量
    int epollfd;//工作线程的epollfd
    T* users;//客户端连接的工作空间
    bool stop;//是否退出线程主循环

};
template<typename T>
int webthread<T>::listenfd=-1;

template<typename T>
int webthread<T>::m_epollfd=-1;

template<typename T>
void webthread<T>::work()
{
    epoll_event events[MAX_EVENT_NUM];
    stack<int> index_free;//初始化空闲队列
    for(int i=USER_PER_THREAD-1;i>=0;i--)
    {
        index_free.push(i);
    }
    unordered_map<int,int> mp;//用户索引与连接符的匹配
    priority_queue<util_timer*,vector<util_timer*>,timer_cmp> timer_queue;//定时器容器
    bool timeshot=false;//定时事件


    while(!stop)
    {
        int number=epoll_wait(epollfd,events,MAX_EVENT_NUM,-1);
        if(number<0 && errno!=EINTR)
        {
            unix_error("epoll failed.");
            break;
        }
        for(int i=0;i<number;i++)
        {
            int sockfd=events[i].data.fd;
            if(sockfd==pipefd[0] && ( events[i].events & EPOLLIN ))//如果接收到主线程的消息
            {
                char infos[1024];
                int ret=recv(sockfd,infos,sizeof(infos),0);
                if( ( ( ret < 0 ) && ( errno != EAGAIN ) ) || ret == 0 )
                {
                    continue;
                }
                else
                {   for(int j=0;j<ret;j++)
                    {
                        switch (infos[j])
                        {
                            case NEW_CONN://处理新到来的连接
                            {
                                struct sockaddr_in client_address;
                                socklen_t client_addrlength = sizeof( client_address );
                                int connfd = accept( listenfd, ( struct sockaddr* )&client_address, &client_addrlength );
                                int conn_count=0;
                                while(connfd>0)
                                {
                                    conn_count++;
                                    if(!index_free.empty())//如果还没有达到本线程的最大用户数，则给新用户分配用户空间
                                    {
                                        int user_index=index_free.top();
                                        index_free.pop();
                                        mp[connfd]=user_index;
                                        util_timer* _timer=new util_timer(time(NULL)+ALARM_TIME);//给每个用户分配一个定时器
                                        _timer->client=&users[user_index];//将定时器与用户地址关联起来，方便后面通过定时器关闭非活动连接
                                        timer_queue.push(_timer);//将定时器放入队列容器中
                                        users[user_index].init( epollfd, connfd, client_address,_timer,user_index,&index_free);//初始化用户空间
                                    }
                                    else//如果用户已满，向新连接发送服务器繁忙消息
                                    {
                                        ret=send(connfd,server_busy_info,strlen(server_busy_info),0);
                                        if(ret<=0)
                                        {
                                            printf("busy_info send failed.\n");
                                        }
                                        Close(connfd);
                                    }
                                    memset((char*)&client_address,0,client_addrlength);
                                    connfd = accept( listenfd, ( struct sockaddr* )&client_address, &client_addrlength );
                                }
                                modfd(m_epollfd, listenfd, EPOLLIN);//重置listenfd的读事件
                                if(conn_count==0)
                                {
                                    printf("user_num_now: %d\n",user_num);
                                    show_error("accept connection failed");
                                    continue;
                                }
                                break;
                            }
                            case CLOSE_DEAD_CONN:
                            {
                                timeshot=true;//定时事件优先级最低
                                break;
                            }
                            case CLOSE_THREAD:
                            {
                                printf("thread:%ld get close info\n",tid);
                                stop=true;//退出线程主循环
                                break;
                            }
                            default:
                                break;
                        }
                    }
                }
            }
            else if( events[i].events & ( EPOLLRDHUP | EPOLLHUP | EPOLLERR ) )//如果有连接描述符出现错误
            {
                int user_index=mp[sockfd];
                users[user_index].close_conn();
            }
            else if( events[i].events & EPOLLIN )//如果有连接描述符可读
            {
                int user_index=mp[sockfd];
                if( users[user_index].read() )//读取连接描述符的消息到用户的读缓冲区
                {
                    users[user_index].process();//解析用户读缓冲区的http请求，解析后往用户的写缓冲区写http应答，并将该连接描述符的epoll注册事件改为写事件
                }
                else
                {
                    users[user_index].close_conn();//如果失败则关闭对应描述符，并释放被原用户占用的空间
                }
            }
            else if( events[i].events & EPOLLOUT )//如果有连接描述符可写
            {
                int user_index=mp[sockfd];
                if( !users[user_index].write() )//如果写失败或者是短连接则立即关闭连接
                {
                    users[user_index].close_conn();
                }
                else//如果是长连接则延长定时器的定时时间，并将该连接描述符的epoll注册事件改为读事件
                {
                    util_timer* timer_temp=users[user_index].timer;
                    timer_temp->expire=time(NULL)+ALARM_TIME;
                }
            }
            else
            {}
            if(timeshot)//定时时间到，回收非活动连接
            {
                while(!timer_queue.empty())
                {
                    util_timer* timer_temp=timer_queue.top();
                    timer_queue.pop();
                    timer_queue.push(timer_temp);
                    timer_temp=timer_queue.top();//由于指针指向的元素值发生改变，需要重排元素
                    time_t t=time(NULL);
                    if(timer_temp->free)//如果定时器关联的连接已关闭，则释放定时器占用的空间
                    {
                        timer_queue.pop();
                        delete timer_temp;
                    }
                    else if(timer_temp->expire<=t)//关闭定时器关联的连接并释放空间
                    {
                        timer_temp->timed_event();
                        timer_queue.pop();
                        delete timer_temp;
                    }
                    else//剩下的连接还没到超时时间
                    {
                        timeshot=false;
                        break;
                    }
                }
                timeshot=false;
            }
        }
    }

    //退出线程前先回收堆空间以避免内存泄漏
    while (!timer_queue.empty())
    {
        util_timer* timer_temp=timer_queue.top();
        timer_queue.pop();
        if(!timer_temp->free)
        {
            timer_temp->timed_event();
        }
        delete timer_temp;
    }
    delete [] users;
    printf("closed thread: %ld\n",tid);

}

#endif // WEB_THREAD_H

