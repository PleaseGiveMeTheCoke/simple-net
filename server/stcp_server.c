//文件名: server/stcp_server.c
//
//描述: 这个文件包含STCP服务器接口实现. 

#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <stdio.h>
#include <sys/select.h>
#include <strings.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>
#include <assert.h>
#include <sys/time.h>
#include "stcp_server.h"
#include "../topology/topology.h"
#include "../common/constants.h"
extern server_tcb_t *find_tcb(int);
extern void *timeout(void *);
//声明tcbtable为全局变量
server_tcb_t* tcbTable[MAX_TRANSPORT_CONNECTIONS];
//声明到SIP进程的连接为全局变量
int sip_conn;

/*********************************************************************/
//
//STCP API实现
//
/*********************************************************************/

// 这个函数初始化TCB表, 将所有条目标记为NULL. 它还针对TCP套接字描述符conn初始化一个STCP层的全局变量, 
// 该变量作为sip_sendseg和sip_recvseg的输入参数. 最后, 这个函数启动seghandler线程来处理进入的STCP段.
// 服务器只有一个seghandler.
void stcp_server_init(int conn) {
    //置空tcb表
    memset(tcbTable,0,sizeof(server_tcb_t *)*MAX_TRANSPORT_CONNECTIONS);
    pthread_t tid;
    //初始化全局变量，用该全局变量来模拟双方网络层通信
    sip_conn = conn;
    //创建监听线程，不断接受对端消息
    pthread_create(&tid,NULL,seghandler,NULL);
    return;
}

// 这个函数查找服务器TCB表以找到第一个NULL条目, 然后使用malloc()为该条目创建一个新的TCB条目.
// 该TCB中的所有字段都被初始化, 例如, TCB state被设置为CLOSED, 服务器端口被设置为函数调用参数server_port. 
// TCB表中条目的索引应作为服务器的新套接字ID被这个函数返回, 它用于标识服务器端的连接. 
// 如果TCB表中没有条目可用, 这个函数返回-1.
int stcp_server_sock(unsigned int server_port) {
    for (int i = 0; i < MAX_TRANSPORT_CONNECTIONS; ++i) {
        if(tcbTable[i]==NULL){
            printf("server tcb table [%d]为空,现在初始化，port = %d\n",i,server_port);
            tcbTable[i] = (server_tcb_t *) malloc(sizeof (server_tcb_t));
            server_tcb_t * tcb = tcbTable[i];
            tcb->state = CLOSED;
            tcb->server_portNum = server_port;
            tcb->server_nodeID = topology_getMyNodeID();
            tcb->client_portNum = -1;//初始化时暂不清楚
            tcb->client_nodeID = -1;//暂不清楚
            tcb->expect_seqNum = 0;
            tcb->bufMutex =  (pthread_mutex_t *)malloc(sizeof(pthread_mutex_t));
            pthread_mutex_init(tcb->bufMutex,NULL);
            tcb->recvBuf = (char*)malloc(RECEIVE_BUF_SIZE);
            tcb->usedBufLen = 0;
            return i;
        }
    }
    return -1;
}

// 这个函数使用sockfd获得TCB指针, 并将连接的state转换为LISTENING. 它然后启动定时器进入忙等待直到TCB状态转换为CONNECTED 
// (当收到SYN时, seghandler会进行状态的转换). 该函数在一个无穷循环中等待TCB的state转换为CONNECTED,  
// 当发生了转换时, 该函数返回1. 你可以使用不同的方法来实现这种阻塞等待.
int stcp_server_accept(int sockfd) {
    server_tcb_t * tcb = tcbTable[sockfd];
    if(tcb==NULL){
        printf("accept error!\nerror func: %s\n",__func__ );
        return -1;
    }
    if(tcb->state!=CLOSED){
        printf("accept error!\nerror func: %s\n",__func__ );
        return -1;
    }
    tcb->state = LISTENING;
    while (tcb->state!=CONNECTED){
        sleep(1);
    }
    return 1 ;
}

// 接收来自STCP客户端的数据. 这个函数每隔RECVBUF_POLLING_INTERVAL时间
// 就查询接收缓冲区, 直到等待的数据到达, 它然后存储数据并返回1. 如果这个函数失败, 则返回-1.
int stcp_server_recv(int sockfd, void* buf, unsigned int length) {
    server_tcb_t* tcb = tcbTable[sockfd];
    if(tcb==NULL||tcb->state!=CONNECTED){
        printf("服务器%d号tcb异常\n",sockfd);
        return -1;
    }
    while (1){
        pthread_mutex_lock(tcb->bufMutex);
        if(tcb->usedBufLen<length){
            if(tcb->state==CLOSED){
                memcpy(buf,tcb->recvBuf, tcb->usedBufLen);
                tcb->usedBufLen = 0;
                pthread_mutex_unlock(tcb->bufMutex);
                break;
            }
            pthread_mutex_unlock(tcb->bufMutex);
            sleep(RECVBUF_POLLING_INTERVAL);
        }else{
            memcpy(buf,tcb->recvBuf,length);
            for(int i = 0;i<tcb->usedBufLen-length;i++){
                tcb->recvBuf[i] = tcb->recvBuf[i+length];
            }
            tcb->usedBufLen-=length;
            pthread_mutex_unlock(tcb->bufMutex);
            break;
        }
    }
    return 1;
}

// 这个函数调用free()释放TCB条目. 它将该条目标记为NULL, 成功时(即位于正确的状态)返回1,
// 失败时(即位于错误的状态)返回-1.
int stcp_server_close(int sockfd) {
    printf("服务器现在释放%d号tcb的资源\n",sockfd);
    if(sockfd < 0 || tcbTable[sockfd] == NULL)
        return -1;
    server_tcb_t* tcb = tcbTable[sockfd];
    free(tcb->recvBuf);
    free(tcb->bufMutex);
    free(tcb);
    tcbTable[sockfd] = NULL;
    return 1;
}

// 这是由stcp_server_init()启动的线程. 它处理所有来自客户端的进入数据. seghandler被设计为一个调用sip_recvseg()的无穷循环, 
// 如果sip_recvseg()失败, 则说明到SIP进程的连接已关闭, 线程将终止. 根据STCP段到达时连接所处的状态, 可以采取不同的动作.
// 请查看服务端FSM以了解更多细节.
void *seghandler(void* arg) {
    pthread_detach(pthread_self());
    int conn = sip_conn;
    while (1) {
        sendseg_arg_t *pSeg = (sendseg_arg_t *) malloc(sizeof(sendseg_arg_t));
        int ret = sip_recvseg(conn, pSeg);
        int srcNodeId = pSeg->nodeID;
        if(ret==-1){
            printf("now exit seghandler\n");
            free(pSeg);
            break;
        }else if(ret==1){
            printf("invalid read because of lost\n");
        }else{
            printf("服务器收到报文段\n");
            printf("类型:%d\n", pSeg->seg.header.type);
            printf("源端口:%d\n", pSeg->seg.header.src_port);
            printf("源ip地址:%d\n", srcNodeId);
            printf("目的端口:%d\n", pSeg->seg.header.dest_port);
            printf("数据长度:%d\n", pSeg->seg.header.length);
            server_tcb_t *pTcb = find_tcb(pSeg->seg.header.dest_port);
            if (pTcb == NULL) {
                printf("端口号出错： %d端口不存在，丢弃该报文\n", pSeg->seg.header.dest_port);
                continue;
            }
            if (pSeg->seg.header.type == SYN) {
                printf("收到发往 %d 号端口的SYN报文段。目前端口的连接状态为 %d\n", pTcb->server_portNum,pTcb->state);
                switch (pTcb->state) {
                    case CLOSED:
                        printf("%d号端口已关闭，无法响应连接\n",pTcb->server_portNum);
                        break;
                    case LISTENING: {
                        printf("将%d号端口的连接状态更改为 CONNECTED\n",pTcb->server_portNum);
                        pTcb->state = CONNECTED;
                        pTcb->client_portNum = pSeg->seg.header.src_port;
                        pTcb->client_nodeID = srcNodeId;
                        //modify the seg and send back the ack
                        //这里直接用了收到的段
                        pSeg->seg.header.ack_num = pSeg->seg.header.seq_num + 1;
                        pSeg->seg.header.type = SYNACK;
                        pSeg->seg.header.dest_port = pSeg->seg.header.src_port;
                        pSeg->seg.header.src_port = pTcb->server_portNum;
                        pSeg->nodeID = srcNodeId;
                        printf("发送SYNACK  源端口：%d  目的端口：%d \n",pSeg->seg.header.src_port,pSeg->seg.header.dest_port);
                        sip_sendseg(conn, pSeg);
                    }
                        break;
                    case CONNECTED: {
                        if(pTcb->client_portNum!=pSeg->seg.header.src_port){
                            printf("%d 号端口号已与%d 号端口建立连接，无法再与%d 号端口建立连接",
                                   pTcb->server_portNum,pTcb->client_portNum,pSeg->seg.header.src_port);
                        }else {
                            printf("%d 号端口收到来自%d 号端口的冗余SYN 重传SYNACK",pTcb->server_portNum,pTcb->client_portNum);
                            pTcb->client_portNum = pSeg->seg.header.src_port;
                            pSeg->seg.header.ack_num = pSeg->seg.header.seq_num + 1;
                            pSeg->seg.header.type = SYNACK;
                            pSeg->seg.header.dest_port = pSeg->seg.header.src_port;
                            pSeg->seg.header.src_port = pTcb->server_portNum;
                            pSeg->nodeID = srcNodeId;
                            sip_sendseg(conn, pSeg);
                        }
                    }
                        break;
                    case CLOSEWAIT:
                        break;
                    default:
                        break;
                }
            } else if (pSeg->seg.header.type == FIN) {
                printf("当前服务器端口：%d的状态为 %d\n",pTcb->server_portNum, pTcb->state);
                printf("收到来自 客户端%d端口的FIN请求",pTcb->client_portNum);
                switch (pTcb->state) {
                    case CLOSED:
                        break;
                    case LISTENING:
                        break;
                    case CONNECTED: {
                        pTcb->state = CLOSEWAIT;
                        pSeg->seg.header.ack_num = pSeg->seg.header.seq_num + 1;
                        pSeg->seg.header.type = FINACK;
                        pSeg->seg.header.dest_port = pSeg->seg.header.src_port;
                        pSeg->seg.header.src_port = pTcb->server_portNum;
                        pSeg->nodeID = srcNodeId;
                        sip_sendseg(conn, pSeg);
                        pthread_t tid;
                        printf("服务器%d号端口进入CLOSEWAIT状态，即将在一段时间后释放资源\n", pTcb->server_portNum);
                        pthread_create(&tid, NULL, timeout, (void *) pTcb);
                        break;
                    }
                    case CLOSEWAIT: {
                        printf("服务器%d号端口收到冗余FIN，继续响应FINACK给客户端\n", pTcb->server_portNum);
                        pSeg->seg.header.ack_num = pSeg->seg.header.seq_num + 1;
                        pSeg->seg.header.type = FINACK;
                        pSeg->seg.header.dest_port = pSeg->seg.header.src_port;
                        pSeg->seg.header.src_port = pTcb->server_portNum;
                        pSeg->nodeID = srcNodeId;
                        sip_sendseg(conn, pSeg);
                        break;
                    }
                    default:
                        break;
                }
            } else if (pSeg->seg.header.type == DATA) {
                switch (pTcb->state) {
                    case CLOSED: {
                        printf("error occur: receive a data pack in state closed\n");
                        break;
                    }
                    case LISTENING: {
                        printf("error occur: receive a data pack in state listening\n");
                        break;
                    }
                    case CONNECTED: {
                        printf("当前服务器%d号端口为CONNECTED，收到来自客户端%d号端口发来的数据报文段\n"
                                ,pTcb->server_portNum,pTcb->client_portNum);
                        printf("报文段信息为：\n");

                        printf("type: %d\n",pSeg->seg.header.type);
                        printf("length: %d\n",pSeg->seg.header.length);
                        printf("seq_num: %d\n",pSeg->seg.header.seq_num);
                        printf("ack_num: %d\n",pSeg->seg.header.ack_num);
                        if (pSeg->seg.header.seq_num == pTcb->expect_seqNum) {
                            printf("seq_num == expect_num，将报文段放入接收缓冲区\n");
                            //push data to buffer
                            pTcb->expect_seqNum += pSeg->seg.header.length;
                            assert(pTcb->usedBufLen + pSeg->seg.header.length < RECEIVE_BUF_SIZE);
                            pthread_mutex_lock(pTcb->bufMutex);
                            char *buf_tail = pTcb->recvBuf + pTcb->usedBufLen;
                            for (int i = 0; i < pSeg->seg.header.length; i++) {
                                buf_tail[i] = pSeg->seg.data[i];
                            }
                            pTcb->usedBufLen += pSeg->seg.header.length;
                            pthread_mutex_unlock(pTcb->bufMutex);
                        }
                        sendseg_arg_t * response = ( sendseg_arg_t *)malloc(sizeof (sendseg_arg_t));
                        response->seg.header.type = DATAACK;
                        response->seg.header.dest_port = pSeg->seg.header.src_port;
                        response->seg.header.src_port = pSeg->seg.header.dest_port;
                        response->seg.header.ack_num = pTcb->expect_seqNum;
                        response->seg.header.length = 0;
                        response->nodeID = srcNodeId;
                        sip_sendseg(conn,response);
                        break;
                    }
                    case CLOSEWAIT: {
                        printf("error occur: receive a data pack in state closewait\n");
                        break;
                    }
                    default: {
                        printf("服务器状态信息异常");
                        break;
                    }
                }
            } else if (pSeg->seg.header.type == DATAACK) {

            } else {
                printf("wrong segment type %d\n", pSeg->seg.header.type);
            }
        }
        free(pSeg);
    }
    return 0;
}
server_tcb_t * find_tcb(int port){
    int i = 0;
    while(i < MAX_TRANSPORT_CONNECTIONS){
        if(tcbTable[i] != NULL && tcbTable[i]->server_portNum == port){
            printf("找到tcb块，编号为%d\n", i);
            return tcbTable[i];
        }
        i++;
    }
    return NULL;
}
void *timeout(void *arg){
    pthread_detach(pthread_self());
    struct timeval tv,tv_curr;
    gettimeofday(&tv,NULL);
    while(1){
        gettimeofday(&tv_curr,NULL);
        int timeuse = tv_curr.tv_sec - tv.tv_sec;
        if(timeuse > 4){
            server_tcb_t *p = (server_tcb_t *)arg;
            printf("服务器端口：%d CLOSEWAIT状态结束，进入CLOSED状态\n",p->server_portNum);

            if(p !=NULL)
                p -> state = CLOSED;
            return NULL;
        }
        sleep(1);
    }
}

