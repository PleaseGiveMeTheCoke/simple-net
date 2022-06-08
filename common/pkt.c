// 文件名: common/pkt.c
#include "pkt.h"
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <errno.h>

// son_sendpkt()由SIP进程调用, 其作用是要求SON进程将报文发送到重叠网络中. SON进程和SIP进程通过一个本地TCP连接互连.
// 在son_sendpkt()中, 报文及其下一跳的节点ID被封装进数据结构sendpkt_arg_t, 并通过TCP连接发送给SON进程.
// 参数son_conn是SIP进程和SON进程之间的TCP连接套接字描述符.
// 当通过SIP进程和SON进程之间的TCP连接发送数据结构sendpkt_arg_t时, 使用'!&'和'!#'作为分隔符, 按照'!& sendpkt_arg_t结构 !#'的顺序发送.
// 如果发送成功, 返回1, 否则返回-1.
int son_sendpkt(int nextNodeID, sip_pkt_t* pkt, int son_conn)
{
    sendpkt_arg_t* pkg = (sendpkt_arg_t*)malloc(sizeof (sendpkt_arg_t));
    pkg->pkt = *pkt;
    pkg->nextNodeID = nextNodeID;
    printf("sip try to send sat to son to tell son that nextNodeID = %d",nextNodeID);
    if(send(son_conn,"!&",2,0) > 0)
        if(send(son_conn,pkg,sizeof(*pkg),0) > 0)
            if(send(son_conn,"!#",2,0) > 0)
                return 1;
    return -1;
}


static int readn( int fd, char *bp, size_t len)
{
    int cnt;
    int rc;

    cnt = len;
    while ( cnt > 0 )
    {
        rc = recv( fd, bp, cnt, 0 );
        if ( rc < 0 )               /* read error? */
        {
            if ( errno == EINTR )   /* interrupted? */
                continue;           /* restart the read */
            return -1;              /* return error */
        }
        if ( rc == 0 )              /* EOF? */
            return len - cnt;       /* return short count */
        bp += rc;
        cnt -= rc;
    }
    return len;
}
// son_recvpkt()函数由SIP进程调用, 其作用是接收来自SON进程的报文.
// 参数son_conn是SIP进程和SON进程之间TCP连接的套接字描述符. 报文通过SIP进程和SON进程之间的TCP连接发送, 使用分隔符!&和!#.
// 为了接收报文, 这个函数使用一个简单的有限状态机FSM
// PKTSTART1 -- 起点
// PKTSTART2 -- 接收到'!', 期待'&'
// PKTRECV -- 接收到'&', 开始接收数据
// PKTSTOP1 -- 接收到'!', 期待'#'以结束数据的接收
// 如果成功接收报文, 返回1, 否则返回-1.
int son_recvpkt(sip_pkt_t* pkt, int son_conn)
{
    int state = 0;
    char ch;
    printf("sip try to recv pkt from son\n");
    while(readn(son_conn,&ch,1) > 0){
        printf("we have read something\n");
        switch(state){
            case 0 :
            {
                if(ch == '!')
                    state = 1;
                break;
            }
            case 1 :
            {
                if(ch == '&'){
                    state = 2;

                    if(readn(son_conn,(void *)pkt,sizeof(sip_pkt_t)) < 0){
                        printf("error occurs when readn in %s\n",__func__);
                        return -1;
                    }
                }else {
                    state = 0;
                }
                break;
            }
            case 2 :
            {
                if(ch == '!')
                    state = 3;
                else
                    return -1;
                break;
            }
            case 3 :
            {
                if(ch == '#'){
                    return 0;
                }else {
                    return -1;
                }
            }
        }
    }
    return -1;
}

// 这个函数由SON进程调用, 其作用是接收数据结构sendpkt_arg_t.
// 报文和下一跳的节点ID被封装进sendpkt_arg_t结构.
// 参数sip_conn是在SIP进程和SON进程之间的TCP连接的套接字描述符.
// sendpkt_arg_t结构通过SIP进程和SON进程之间的TCP连接发送, 使用分隔符!&和!#.
// 为了接收报文, 这个函数使用一个简单的有限状态机FSM
// PKTSTART1 -- 起点
// PKTSTART2 -- 接收到'!', 期待'&'
// PKTRECV -- 接收到'&', 开始接收数据
// PKTSTOP1 -- 接收到'!', 期待'#'以结束数据的接收
// 如果成功接收sendpkt_arg_t结构, 返回1, 否则返回-1.
int getpktToSend(sendpkt_arg_t* pkt, int sip_conn)
{
    int state = 0;
    char ch;
    printf("son try to recv sat from sip\n");
    while(readn(sip_conn,&ch,1) > 0){
        printf("we have read something\n");

        switch(state){
            case 0 :
            {
                if(ch == '!')
                    state = 1;
                break;
            }
            case 1 :
            {
                if(ch == '&'){
                    state = 2;

                    if(readn(sip_conn,(void *)pkt,sizeof(sendpkt_arg_t)) < 0){
                        printf("error occurs when readn in %s\n",__func__);
                        return -1;
                    }
                }else {
                    state = 0;
                }
                break;
            }
            case 2 :
            {
                if(ch == '!')
                    state = 3;
                else
                    return -1;
                break;
            }
            case 3 :
            {
                if(ch == '#'){
                    return 0;
                }else {
                    return -1;
                }
            }
        }

    }
    printf("son know that next nodeID = %d",pkt->nextNodeID);
    return 1;
}

// forwardpktToSIP()函数是在SON进程接收到来自重叠网络中其邻居的报文后被调用的.
// SON进程调用这个函数将报文转发给SIP进程.
// 参数sip_conn是SIP进程和SON进程之间的TCP连接的套接字描述符.
// 报文通过SIP进程和SON进程之间的TCP连接发送, 使用分隔符!&和!#, 按照'!& 报文 !#'的顺序发送.
// 如果报文发送成功, 返回1, 否则返回-1.
int forwardpktToSIP(sip_pkt_t* pkt, int sip_conn)
{

    printf("son send pkt to sip\n");
    printf("pkt type: %d\n",pkt->header.type);
    if(send(sip_conn,"!&",2,0) > 0)
        if(send(sip_conn,pkt,sizeof(sip_pkt_t),0) > 0)
            if(send(sip_conn,"!#",2,0) > 0)
                return 1;
    return -1;
}

// sendpkt()函数由SON进程调用, 其作用是将接收自SIP进程的报文发送给下一跳.
// 参数conn是到下一跳节点的TCP连接的套接字描述符.
// 报文通过SON进程和其邻居节点之间的TCP连接发送, 使用分隔符!&和!#, 按照'!& 报文 !#'的顺序发送.
// 如果报文发送成功, 返回1, 否则返回-1.
int sendpkt(sip_pkt_t* pkt, int conn)
{
    printf("son send pkt to next rip\n");
    printf("pkt type: %d\n",pkt->header.type);
    if(send(conn,"!&",2,0) > 0)
        if(send(conn,pkt,sizeof(sip_pkt_t),0) > 0)
            if(send(conn,"!#",2,0) > 0)
                return 1;
    return -1;
}

// recvpkt()函数由SON进程调用, 其作用是接收来自重叠网络中其邻居的报文.
// 参数conn是到其邻居的TCP连接的套接字描述符.
// 报文通过SON进程和其邻居之间的TCP连接发送, 使用分隔符!&和!#.
// 为了接收报文, 这个函数使用一个简单的有限状态机FSM
// PKTSTART1 -- 起点
// PKTSTART2 -- 接收到'!', 期待'&'
// PKTRECV -- 接收到'&', 开始接收数据
// PKTSTOP1 -- 接收到'!', 期待'#'以结束数据的接收
// 如果成功接收报文, 返回1, 否则返回-1.
int recvpkt(sip_pkt_t* pkt, int conn)
{
    int state = 0;
    char ch;
    printf("son try to recv pkt from nbr\n");
    while(readn(conn,&ch,1) > 0){
        printf("we have read something\n");

        switch(state){
            case 0 :
            {
                if(ch == '!')
                    state = 1;
                break;
            }
            case 1 :
            {
                if(ch == '&'){
                    state = 2;

                    if(readn(conn,(void *)pkt,sizeof(sip_pkt_t)) < 0){
                        printf("error occurs when readn in %s\n",__func__);
                        return -1;
                    }
                }else {
                    state = 0;
                }
                break;
            }
            case 2 :
            {
                if(ch == '!')
                    state = 3;
                else
                    return -1;
                break;
            }
            case 3 :
            {
                if(ch == '#'){
                    return 0;
                }else {
                    return -1;
                }
            }
        }

    }
    return 1;
}
