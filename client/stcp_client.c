//文件名: client/stcp_client.c
//
//描述: 这个文件包含STCP客户端接口实现 

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <assert.h>
#include <strings.h>
#include <unistd.h>
#include <sys/time.h>
#include <time.h>
#include <pthread.h>
#include "../topology/topology.h"
#include "stcp_client.h"
#include "../common/seg.h"

//声明tcbtable为全局变量
client_tcb_t* tcb_table[MAX_TRANSPORT_CONNECTIONS];
//声明到SIP进程的TCP连接为全局变量
int sip_conn;
static pthread_t  seghandler_t;
static pthread_t  segBuf_timer_tid;
void* sendBuf_timer(void*);
static inline unsigned int get_current_time(){
    struct timeval now;
    gettimeofday(&now, NULL);
    return now.tv_sec;
}

/*********************************************************************/
//
//STCP API实现
//
/*********************************************************************/

// 这个函数初始化TCB表, 将所有条目标记为NULL.  
// 它还针对TCP套接字描述符conn初始化一个STCP层的全局变量, 该变量作为sip_sendseg和sip_recvseg的输入参数.
// 最后, 这个函数启动seghandler线程来处理进入的STCP段. 客户端只有一个seghandler.
void stcp_client_init(int conn) 
{
    for (int i = 0; i < MAX_TRANSPORT_CONNECTIONS; ++i) {
        tcb_table[i] = NULL;
    }
    sip_conn = conn;
    pthread_create(&seghandler_t,NULL,seghandler,NULL);
    return;
}

// 这个函数查找客户端TCB表以找到第一个NULL条目, 然后使用malloc()为该条目创建一个新的TCB条目.
// 该TCB中的所有字段都被初始化. 例如, TCB state被设置为CLOSED，客户端端口被设置为函数调用参数client_port. 
// TCB表中条目的索引号应作为客户端的新套接字ID被这个函数返回, 它用于标识客户端的连接. 
// 如果TCB表中没有条目可用, 这个函数返回-1.
int stcp_client_sock(unsigned int client_port) {
    for (int i = 0; i < MAX_TRANSPORT_CONNECTIONS; ++i) {
        if(tcb_table[i]==NULL){
            tcb_table[i] = (client_tcb_t*) malloc(sizeof (client_tcb_t));
            client_tcb_t * tcb = tcb_table[i];
            tcb->state = CLOSED;
            tcb->client_portNum = client_port;
            tcb->bufMutex =  (pthread_mutex_t *)malloc(sizeof(pthread_mutex_t));
            tcb->client_nodeID = topology_getMyNodeID();
            pthread_mutex_init(tcb->bufMutex,NULL);

            //发送缓冲区相关数据初始化
            tcb->sendBufTail = NULL;
            tcb->sendBufHead = NULL;
            tcb->sendBufunSent = NULL;
            tcb->unAck_segNum = 0;
            tcb->next_seqNum = 0;
            return i;
        }
    }
    return -1;
}
// 这个函数用于连接服务器. 它以套接字ID, 服务器节点ID和服务器的端口号作为输入参数. 套接字ID用于找到TCB条目.  
// 这个函数设置TCB的服务器节点ID和服务器端口号,  然后使用sip_sendseg()发送一个SYN段给服务器.  
// 在发送了SYN段之后, 一个定时器被启动. 如果在SYNSEG_TIMEOUT时间之内没有收到SYNACK, SYN 段将被重传. 
// 如果收到了, 就返回1. 否则, 如果重传SYN的次数大于SYN_MAX_RETRY, 就将state转换到CLOSED, 并返回-1.
int stcp_client_connect(int sockfd, int nodeID, unsigned int server_port) 
{
    client_tcb_t* tcb = tcb_table[sockfd];
    if(tcb==NULL){
        printf("%d号tcb不存在\n",sockfd);
        return -1;
    }
    tcb->server_portNum = server_port;
    tcb->server_nodeID = nodeID;
    sendseg_arg_t syn_seg;
    memset(&syn_seg,0,sizeof(syn_seg));
    syn_seg.seg.header.type = SYN;
    syn_seg.seg.header.src_port = tcb->client_portNum;
    syn_seg.seg.header.dest_port = tcb->server_portNum;
    syn_seg.seg.header.seq_num = sockfd;
    syn_seg.nodeID = nodeID;
    switch (tcb->state) {
        case CLOSED: {
            tcb->state = SYNSENT;
            int try = 0;
            //stcp_sock在init中初始化完毕
            sip_sendseg(sip_conn, &syn_seg);
            printf("客户端ip: %d 端口: %d 向服务器ip: %d 端口: %d发送了SYN\n",tcb->client_nodeID,tcb->client_portNum,nodeID,server_port);
            sleep(1);
            //usleep(SYN_TIMEOUT / 1000);
            while (tcb->state != CONNECTED && try <= SYN_MAX_RETRY) {
                try++;
                printf("服务器ip: %d 端口: %d无响应，进行第%d次重传\n",nodeID, syn_seg.seg.header.dest_port, try);
                sip_sendseg(sip_conn, &syn_seg);
                sleep(1);
            }
            if (tcb->state == CONNECTED) {
                printf("客户端：%d号端口与 服务端：%d号端口成功建立连接\n",tcb->client_portNum,tcb->server_portNum);
                return 1;
            } else {
                tcb->state = CLOSED;
                printf("客户端：%d号端口向服务端：%d号端口重传次数过多，连接失败\n",tcb->client_portNum,server_port);
                return -1;
            }
        }
        case SYNSENT:{
            printf("状态异常,当前状态为syn_sent,期望为closed\n");
            break;
        }
        case CONNECTED:{
            printf("状态异常,当前状态为connected,期望为closed\n");
            return -1;
            break;
        }
        case FINWAIT:{
            printf("状态异常,当前状态为finwait,期望为closed\n");
            return -1;
            break;
        }
    }
    return -1;
}

// 发送数据给STCP服务器. 这个函数使用套接字ID找到TCB表中的条目.
// 然后它使用提供的数据创建segBuf, 将它附加到发送缓冲区链表中.
// 如果发送缓冲区在插入数据之前为空, 一个名为sendbuf_timer的线程就会启动.
// 每隔SENDBUF_ROLLING_INTERVAL时间查询发送缓冲区以检查是否有超时事件发生. 
// 这个函数在成功时返回1，否则返回-1. 
// stcp_client_send是一个非阻塞函数调用.
// 因为用户数据被分片为固定大小的STCP段, 所以一次stcp_client_send调用可能会产生多个segBuf
// 被添加到发送缓冲区链表中. 如果调用成功, 数据就被放入TCB发送缓冲区链表中, 根据滑动窗口的情况,
// 数据可能被传输到网络中, 或在队列中等待传输.
void add_to_buffer(client_tcb_t* tcb, unsigned int length, void* data){
    //开辟数据段空间
    segBuf_t *pSegBuf = (segBuf_t *) malloc(sizeof(segBuf_t));
    //清空
    memset(pSegBuf, 0, sizeof(segBuf_t));
    //设置属性
    pSegBuf->seg.header.src_port = tcb->client_portNum;
    pSegBuf->seg.header.dest_port = tcb->server_portNum;
    pSegBuf->seg.header.length = length;
    pSegBuf->seg.header.seq_num = tcb->next_seqNum;
    pSegBuf->seg.header.type = DATA;
    memcpy(pSegBuf->seg.data, data, length);
    //fill next and time of this segBUf
    pSegBuf->next = NULL;
    //modify the TCB item,insert the segBuf to tail
    tcb->next_seqNum += length;
    pthread_mutex_lock(tcb->bufMutex);
    if (tcb->sendBufHead == NULL) {//init head,tail and start segBuf_timer()
        usleep(SENDBUF_POLLING_INTERVAL / 1000);//in case the timer have not exit
        tcb->sendBufHead = pSegBuf;
        tcb->sendBufTail = pSegBuf;
        tcb->sendBufunSent = pSegBuf;
        pthread_create(&segBuf_timer_tid, NULL, sendBuf_timer, (void *) tcb);
    } else {
        tcb->sendBufTail->next = pSegBuf;
        tcb->sendBufTail = pSegBuf;
        if (tcb->sendBufunSent == NULL)
            tcb->sendBufunSent = tcb->sendBufTail;
    }
    pthread_mutex_unlock(tcb->bufMutex);
}
int stcp_client_send(int sockfd, void* data, unsigned int length) {

    client_tcb_t* tcb = tcb_table[sockfd];

    if(tcb==NULL){
        printf("要发送数据的%d号tcb为空\n",sockfd);
        return -1;
    }else if(tcb->state!=CONNECTED){
        printf("要发送数据的%d号tcb状态异常，为%d\n",sockfd,tcb->state);
        return -1;
    }
    //将数据加入缓冲区
    if(length<=MAX_SEG_LEN) {
        add_to_buffer(tcb, length, data);
    }else{
        while (length>MAX_SEG_LEN){
            add_to_buffer(tcb, MAX_SEG_LEN, data);
            length-=MAX_SEG_LEN;
            data+=MAX_SEG_LEN;
        }
        if(length>0) {
            add_to_buffer(tcb, length, data);
        }

    }
    //发送缓冲区数据直到已发送未确认数目达到GBN_WINDOW
    pthread_mutex_lock(tcb->bufMutex);
    while (tcb->sendBufunSent != NULL && tcb->unAck_segNum < GBN_WINDOW) {
        if (tcb->sendBufunSent == NULL) {
            pthread_mutex_unlock(tcb->bufMutex);
            break;
        }
        tcb->sendBufunSent->sentTime = get_current_time();
        sendseg_arg_t sat;
        memset(&sat,0,sizeof(sat));

        sat.seg = tcb->sendBufunSent->seg;
        sat.nodeID = tcb->server_nodeID;
        sip_sendseg(sip_conn,&sat);
        printf("%s:, seq_num 为%d的报文段已发送\n", __func__, tcb->sendBufunSent->seg.header.seq_num);
        tcb->sendBufunSent = tcb->sendBufunSent->next;
        tcb->unAck_segNum++;
    }
    pthread_mutex_unlock(tcb->bufMutex);
}

// 这个函数用于断开到服务器的连接. 它以套接字ID作为输入参数. 套接字ID用于找到TCB表中的条目.  
// 这个函数发送FIN段给服务器. 在发送FIN之后, state将转换到FINWAIT, 并启动一个定时器.
// 如果在最终超时之前state转换到CLOSED, 则表明FINACK已被成功接收. 否则, 如果在经过FIN_MAX_RETRY次尝试之后,
// state仍然为FINWAIT, state将转换到CLOSED, 并返回-1.
int stcp_client_disconnect(int sockfd) {
    printf("尝试断开%d号tcb管理的连接\n",sockfd);
    client_tcb_t* tcb = tcb_table[sockfd];
    if(tcb==NULL){
        printf("断开连接失败，%d号tcb状态异常\n",sockfd);
        return -1;
    }
    sendseg_arg_t fin_seg;
    memset(&fin_seg,0,sizeof(fin_seg));
    fin_seg.seg.header.type = FIN;
    fin_seg.seg.header.src_port = tcb->client_portNum;
    fin_seg.seg.header.dest_port = tcb->server_portNum;
    fin_seg.seg.header.seq_num = sockfd;
    fin_seg.nodeID = tcb->server_nodeID;
    switch(tcb->state){
        case CLOSED:
        {
            printf("连接关闭失败：%d号tcb状态异常，为closed\n",sockfd);
            return -1;
            break;
        }
        case SYNSENT:
        {
            printf("连接关闭失败：%d号tcb状态异常，为SYNSENT\n",sockfd);
            return -1;
            break;
        }
        case CONNECTED:
        {
            tcb->state = FINWAIT;
            int try_times = 0;
            sip_sendseg(sip_conn,&fin_seg);
            printf("客户端%d号端口尝试向服务器%d号端口发送fin\n",tcb->client_portNum,tcb->server_portNum);
            printf("当前客户端%d号端口状态由CONNECTED变为FINWAIT\n",tcb->client_portNum);

            sleep(1);
            while(tcb->state != CLOSED && try_times < FIN_MAX_RETRY){
                try_times++;
                sip_sendseg(sip_conn, &fin_seg);
                printf("超时，未收到FINACK，开始第%d次重传FIN报文\n",try_times);
                sleep(1);
            }

            if (tcb->state == CLOSED){
                printf("客户端：%d 与服务器：%d端口的连接关闭成功\n",tcb->client_portNum,tcb->server_portNum);
                return 1;
            } else {
                printf("连接关闭失败\n");
                tcb->state = CLOSED;
                return -1;
            }
        }
        case FINWAIT:
        {
            printf("连接关闭失败：tcb状态异常，为finwait\n");
            return -1;
            break;
        }
    }


    return 0;
}

// 这个函数调用free()释放TCB条目. 它将该条目标记为NULL, 成功时(即位于正确的状态)返回1,
// 失败时(即位于错误的状态)返回-1.
int stcp_client_close(int sockfd) {
    printf("释放%d号tcb的资源\n",sockfd);
    client_tcb_t* tcb =  tcb_table[sockfd];
    if(tcb==NULL){
        printf("没有找到%d号tcb\n",sockfd);
        return -1;
    }else{
        free(tcb->bufMutex);
        free(tcb);
        tcb_table[sockfd] = NULL;
        printf("释放%d号tcb成功\n",sockfd);
        return 1;
    }
}

// 这是由stcp_client_init()启动的线程. 它处理所有来自服务器的进入段. 
// seghandler被设计为一个调用sip_recvseg()的无穷循环. 如果sip_recvseg()失败, 则说明到SIP进程的连接已关闭,
// 线程将终止. 根据STCP段到达时连接所处的状态, 可以采取不同的动作. 请查看客户端FSM以了解更多细节.
void* seghandler(void* arg) 
{
    while(1) {
        sendseg_arg_t * recv_seg = (sendseg_arg_t *) malloc(sizeof(sendseg_arg_t));
        int ret = sip_recvseg(sip_conn, recv_seg);
        int srcNodeId = recv_seg->nodeID;
        if (ret == 1) {
            printf("invalid read because of lost\n");

        }else if(ret==-1){
            printf("now exit seghandler\n");
            free(recv_seg);
            break;
        }else {
            printf("客户端收到报文段\n");
            client_tcb_t *item = NULL;
            for (int i = 0; i < MAX_TRANSPORT_CONNECTIONS; i++) {
                if (tcb_table[i] != NULL && tcb_table[i]->client_portNum == recv_seg->seg.header.dest_port) {
                    item = tcb_table[i];
                    break;
                }
            }
            if (item == NULL) {
                printf("没有找到目的端口%d，或目的端口%d已被占用\n", recv_seg->seg.header.dest_port, recv_seg->seg.header.dest_port);
                continue;
            }
            switch (item->state) {
                case CLOSED: {
                    printf("目的端口%d的连接状态为已关闭,无法处理报文段\n",recv_seg->seg.header.dest_port);
                    break;
                }
                case SYNSENT: {
                    if (recv_seg->seg.header.type == SYNACK) {
                        printf("客户端%d号端口收到来自服务器%d号端口的SYNACK,状态更改为已连接\n",recv_seg->seg.header.dest_port,recv_seg->seg.header.src_port);
                        item->state = CONNECTED;
                        item->server_portNum = recv_seg->seg.header.src_port;
                    } else {
                        printf("客户端%d号端口当前状态为synsent，无法解析除synack外的报文\n",recv_seg->seg.header.dest_port);
                    }
                    break;
                }
                case CONNECTED: {
                    printf("客户端%d端口的当前状态为CONNECTED。收到来自服务器%d号端口报文段，报文类型为： %d\n", recv_seg->seg.header.dest_port,
                           recv_seg->seg.header.src_port,recv_seg->seg.header.type);
                    if (recv_seg->seg.header.type == DATAACK) {
                        segBuf_t *pSegBuf = item->sendBufHead;
                        //删除那些序列号小于服务器传来的待接受的下一序列号的段
                        pthread_mutex_lock(item->bufMutex);
                        int seqNum = pSegBuf->seg.header.seq_num;
                        while (seqNum < recv_seg->seg.header.ack_num) {
                            if (item->sendBufHead == item->sendBufTail) {//only one segBuf
                                free(pSegBuf);
                                pSegBuf = item->sendBufHead = item->sendBufTail = NULL;
                                item->unAck_segNum = 0;
                                break;
                            } else {
                                item->sendBufHead = item->sendBufHead->next;
                                free(pSegBuf);
                                pSegBuf = item->sendBufHead;
                                item->unAck_segNum--;
                            }
                            seqNum = pSegBuf->seg.header.seq_num;
                        }
                        pthread_mutex_unlock(item->bufMutex);
                        //send the unsent segBuf
                        pthread_mutex_lock(item->bufMutex);
                        while (item->sendBufunSent != NULL && item->unAck_segNum < GBN_WINDOW) {
                            pSegBuf = item->sendBufunSent;
                            if (pSegBuf == NULL) {
                                pthread_mutex_unlock(item->bufMutex);
                                break;
                            }
                            struct timeval now;
                            gettimeofday(&now, NULL);
                            pSegBuf->sentTime = now.tv_sec;
                            sendseg_arg_t  sat;
                            sat.seg = pSegBuf->seg;
                            sat.nodeID = srcNodeId;
                            sip_sendseg(sip_conn, &sat);
                            printf("%s:, seq_num 为%d的报文段已发送\n", __func__, pSegBuf->seg.header.seq_num);
                            item->sendBufunSent = item->sendBufunSent->next;
                            item->unAck_segNum++;
                        }
                        pthread_mutex_unlock(item->bufMutex);
                    }

                    else if(recv_seg->seg.header.type==SYNACK){
                        printf("客户端%d端口已与服务器%d端口建立连接，不再响应该SYNACK报文段",
                               item->client_portNum,item->server_portNum);
                    }else{
                        //其他不做响应
                    }
                    break;
                }
                case FINWAIT: {
                    if (recv_seg->seg.header.type == FINACK) {
                        printf("客户端%d号端口收到FINACK,进入closed状态\n",item->client_portNum);
                        item->state = CLOSED;
                    } else {
                        printf("客户端%d号端口收到无效报文段，当前状态：finwait\n",item->client_portNum);
                    }
                    break;
                }
            }
        }
        free(recv_seg);
    }
}


//这个线程持续轮询发送缓冲区以触发超时事件. 如果发送缓冲区非空, 它应一直运行.
//如果(当前时间 - 第一个已发送但未被确认段的发送时间) > DATA_TIMEOUT, 就发生一次超时事件.
//当超时事件发生时, 重新发送所有已发送但未被确认段. 当发送缓冲区为空时, 这个线程将终止.
void* sendBuf_timer(void* clienttcb)
{
    client_tcb_t* tcb = (client_tcb_t*)clienttcb;
    while (1){
        if(tcb->sendBufHead==NULL){
            printf("%d号端口的发送缓冲区为空，停止计时线程\n",tcb->client_portNum);
            break;
        }else{
            pthread_mutex_lock(tcb->bufMutex);
            segBuf_t* head = tcb->sendBufHead;
            if(get_current_time()-head->sentTime>1){
                printf("超时，重传所有已发送但未确认的段\n");
                segBuf_t* begin = head;
                while (begin!=tcb->sendBufunSent){
                    begin->sentTime = get_current_time();
                    sendseg_arg_t  sat;
                    sat.seg = begin->seg;
                    sat.nodeID = tcb->server_nodeID;
                    sip_sendseg(sip_conn,&sat);
                    printf("in : %s, seq_num : %d has been resent\n",__func__,begin->seg.header.seq_num);
                    begin = begin->next;
                }
            }
        }
        pthread_mutex_unlock(tcb->bufMutex);
        usleep(SENDBUF_POLLING_INTERVAL/1000);
    }
}

