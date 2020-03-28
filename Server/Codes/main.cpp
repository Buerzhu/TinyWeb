#include"web_thread.h"
#include"web_function.h"
#include"http_conn.h"

using namespace std;

webthread<http_conn>* thread_ptr;//指向自定义线程类的指针,这是一个全局变量，主线程和子线程通过线程类的双向管道来实现通信

int sig_pipefd[2];//系统中断信号处理函数与主函数之间的通信管道，本程序把信号处理放在主函数中进行



void* thread(void* arg)//子线程主函数
{
    int* p=(int*)arg;
    int index=*p;
    delete arg;

    thread_ptr[index].work();//对应的子线程开始工作
}

void create_son_thread()//创建子线程
{
    pthread_t tid;
    printf( "create son threads now\n" );
    for(int i=0;i<thread_num;i++)
    {
        int* index=new int(i);
        if(pthread_create(&tid,NULL,thread,index)!=0)//如果线程创建失败
        {
            delete [] thread_ptr;
            unix_error("thread created failed");
        }
        if(pthread_detach(tid))//如果线程分离失败
        {
            delete [] thread_ptr;
            unix_error("thread detach failed");
        }
        else
        {
            thread_ptr[i].tid=tid;
            printf("create thread: %ld\n",tid);

        }
    }
}

void close_son_thread()
{
    printf( "kill all the thread now\n" );
    char info=CLOSE_THREAD;
    for( int i = 0; i < thread_num; ++i )
    {
        send(thread_ptr[i].pipefd[1],&info,sizeof(info),0);
    }
}

static void sig_handler( int sig)//信号发送给主函数
{
    int save_errno = errno;
    int msg = sig;
    send( sig_pipefd[1], ( char* )&msg, 1, 0 );
    errno = save_errno;
}

void deal_new_conn(int &thread_index)//主线程分配新到的连接给子线程
{
    char info=NEW_CONN;
    thread_index=thread_index%thread_num;
    send(thread_ptr[thread_index].pipefd[1],&info,sizeof(info),0);//主线程通过对应的管道与子线程通信
    thread_index++;
}
void deal_sig_recv(bool& stop)//主线程处理系统信号
{
    char infos[1024];
    int ret=recv(sig_pipefd[0],infos,sizeof(infos),0);
    if( ret <= 0 )
    {
        return;
    }
    else
    {
        for( int i = 0; i < ret; ++i )
        {
            switch( infos[i] )
            {
                case SIGTERM:
                case SIGINT:
                {
                    close_son_thread();
                    stop=true;
                    break;
                }
                case SIGALRM:
                {
                    int info=CLOSE_DEAD_CONN;
                    for( int i = 0; i < thread_num; ++i )
                    {
                        send(thread_ptr[i].pipefd[1],&info,sizeof(info),0);
                    }
                    alarm(ALARM_TIME);//重新设置定时信号
                    break;
                }
                default:
                {
                    break;
                }
            }
        }
    }
}



int main(int argc, char *argv[])
{
    if(argc<=2)
    {
        fprintf(stderr,"usage: %s ip_address port\n",argv[0]);;
        exit(1);
    }
    char* ip=argv[1];
    char* port=argv[2];
//    char* ip="127.0.0.1";
//    char* port="12345";

    int listenfd=open_listenfd(ip,port,LISTENQ);

    thread_ptr=new webthread<http_conn>[thread_num];
    webthread<http_conn>::listenfd=listenfd;

    pthread_mutex_init(&mutex_conn,NULL);

    create_son_thread();//创建子线程，子线程开始运行

    int epollfd = epoll_create( 5 );
    assert( epollfd != -1 );

    addfd(epollfd,listenfd,true);
    webthread<http_conn>::m_epollfd=epollfd;



    int ret = socketpair( PF_UNIX, SOCK_STREAM, 0, sig_pipefd );//创建信号处理函数与主函数之间的通信管道
    assert( ret != -1 );

    setnonblocking( sig_pipefd[1] );
    addfd( epollfd, sig_pipefd[0] );


    addsig( SIGTERM, sig_handler );
    addsig( SIGINT, sig_handler );
    addsig( SIGALRM,sig_handler );
    addsig( SIGPIPE, SIG_IGN );

    epoll_event events[MAX_EVENT_NUM];


    int m_pid=(int)getpid();
    printf("PID: %d\n",m_pid);
    printf("server start\n");
    alarm(ALARM_TIME);//开始定时

    bool stop=false;
    int thread_index=0;

    while(!stop)
    {
        int number=epoll_wait(epollfd,events,MAX_EVENT_NUM,-1);
        if(number<0 && errno!=EINTR)
        {
            close_son_thread();
            unix_error("main thread epoll failed.");
            break;
        }

        for(int i=0;i<number;i++)
        {
            int sockfd=events[i].data.fd;
            if(sockfd==listenfd)//有新连接到来，循环分配给线程
            {
                deal_new_conn(thread_index);
            }
            else if( ( sockfd == sig_pipefd[0] ) && ( events[i].events & EPOLLIN ) )//接收到新信号，处理信号事件
            {
                deal_sig_recv(stop);
            }
            else
            {
                continue;
            }

        }
    }

    sleep(1);
    Close(epollfd);
    Close(listenfd);
    Close(sig_pipefd[1]);
    Close(sig_pipefd[0]);
    delete [] thread_ptr;
    printf("max_user_online: %d\n",(int)max_user_online);//最大客户端连接数
    printf("user_online_now: %d\n",(int)user_count);//C++11,检查当前所有连接是否关闭
    show_sys_time();
    printf("closed server.\n");

    exit(0);
}

