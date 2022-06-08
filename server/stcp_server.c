//�ļ���: server/stcp_server.c
//
//����: ����ļ�����STCP�������ӿ�ʵ��. 

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
//����tcbtableΪȫ�ֱ���
server_tcb_t* tcbTable[MAX_TRANSPORT_CONNECTIONS];
//������SIP���̵�����Ϊȫ�ֱ���
int sip_conn;

/*********************************************************************/
//
//STCP APIʵ��
//
/*********************************************************************/

// ���������ʼ��TCB��, ��������Ŀ���ΪNULL. �������TCP�׽���������conn��ʼ��һ��STCP���ȫ�ֱ���, 
// �ñ�����Ϊsip_sendseg��sip_recvseg���������. ���, �����������seghandler�߳�����������STCP��.
// ������ֻ��һ��seghandler.
void stcp_server_init(int conn) {
    //�ÿ�tcb��
    memset(tcbTable,0,sizeof(server_tcb_t *)*MAX_TRANSPORT_CONNECTIONS);
    pthread_t tid;
    //��ʼ��ȫ�ֱ������ø�ȫ�ֱ�����ģ��˫�������ͨ��
    sip_conn = conn;
    //���������̣߳����Ͻ��ܶԶ���Ϣ
    pthread_create(&tid,NULL,seghandler,NULL);
    return;
}

// ����������ҷ�����TCB�����ҵ���һ��NULL��Ŀ, Ȼ��ʹ��malloc()Ϊ����Ŀ����һ���µ�TCB��Ŀ.
// ��TCB�е������ֶζ�����ʼ��, ����, TCB state������ΪCLOSED, �������˿ڱ�����Ϊ�������ò���server_port. 
// TCB������Ŀ������Ӧ��Ϊ�����������׽���ID�������������, �����ڱ�ʶ�������˵�����. 
// ���TCB����û����Ŀ����, �����������-1.
int stcp_server_sock(unsigned int server_port) {
    for (int i = 0; i < MAX_TRANSPORT_CONNECTIONS; ++i) {
        if(tcbTable[i]==NULL){
            printf("server tcb table [%d]Ϊ��,���ڳ�ʼ����port = %d\n",i,server_port);
            tcbTable[i] = (server_tcb_t *) malloc(sizeof (server_tcb_t));
            server_tcb_t * tcb = tcbTable[i];
            tcb->state = CLOSED;
            tcb->server_portNum = server_port;
            tcb->server_nodeID = topology_getMyNodeID();
            tcb->client_portNum = -1;//��ʼ��ʱ�ݲ����
            tcb->client_nodeID = -1;//�ݲ����
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

// �������ʹ��sockfd���TCBָ��, �������ӵ�stateת��ΪLISTENING. ��Ȼ��������ʱ������æ�ȴ�ֱ��TCB״̬ת��ΪCONNECTED 
// (���յ�SYNʱ, seghandler�����״̬��ת��). �ú�����һ������ѭ���еȴ�TCB��stateת��ΪCONNECTED,  
// ��������ת��ʱ, �ú�������1. �����ʹ�ò�ͬ�ķ�����ʵ�����������ȴ�.
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

// ��������STCP�ͻ��˵�����. �������ÿ��RECVBUF_POLLING_INTERVALʱ��
// �Ͳ�ѯ���ջ�����, ֱ���ȴ������ݵ���, ��Ȼ��洢���ݲ�����1. ����������ʧ��, �򷵻�-1.
int stcp_server_recv(int sockfd, void* buf, unsigned int length) {
    server_tcb_t* tcb = tcbTable[sockfd];
    if(tcb==NULL||tcb->state!=CONNECTED){
        printf("������%d��tcb�쳣\n",sockfd);
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

// �����������free()�ͷ�TCB��Ŀ. ��������Ŀ���ΪNULL, �ɹ�ʱ(��λ����ȷ��״̬)����1,
// ʧ��ʱ(��λ�ڴ����״̬)����-1.
int stcp_server_close(int sockfd) {
    printf("�����������ͷ�%d��tcb����Դ\n",sockfd);
    if(sockfd < 0 || tcbTable[sockfd] == NULL)
        return -1;
    server_tcb_t* tcb = tcbTable[sockfd];
    free(tcb->recvBuf);
    free(tcb->bufMutex);
    free(tcb);
    tcbTable[sockfd] = NULL;
    return 1;
}

// ������stcp_server_init()�������߳�. �������������Կͻ��˵Ľ�������. seghandler�����Ϊһ������sip_recvseg()������ѭ��, 
// ���sip_recvseg()ʧ��, ��˵����SIP���̵������ѹر�, �߳̽���ֹ. ����STCP�ε���ʱ����������״̬, ���Բ�ȡ��ͬ�Ķ���.
// ��鿴�����FSM���˽����ϸ��.
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
            printf("�������յ����Ķ�\n");
            printf("����:%d\n", pSeg->seg.header.type);
            printf("Դ�˿�:%d\n", pSeg->seg.header.src_port);
            printf("Դip��ַ:%d\n", srcNodeId);
            printf("Ŀ�Ķ˿�:%d\n", pSeg->seg.header.dest_port);
            printf("���ݳ���:%d\n", pSeg->seg.header.length);
            server_tcb_t *pTcb = find_tcb(pSeg->seg.header.dest_port);
            if (pTcb == NULL) {
                printf("�˿ںų��� %d�˿ڲ����ڣ������ñ���\n", pSeg->seg.header.dest_port);
                continue;
            }
            if (pSeg->seg.header.type == SYN) {
                printf("�յ����� %d �Ŷ˿ڵ�SYN���ĶΡ�Ŀǰ�˿ڵ�����״̬Ϊ %d\n", pTcb->server_portNum,pTcb->state);
                switch (pTcb->state) {
                    case CLOSED:
                        printf("%d�Ŷ˿��ѹرգ��޷���Ӧ����\n",pTcb->server_portNum);
                        break;
                    case LISTENING: {
                        printf("��%d�Ŷ˿ڵ�����״̬����Ϊ CONNECTED\n",pTcb->server_portNum);
                        pTcb->state = CONNECTED;
                        pTcb->client_portNum = pSeg->seg.header.src_port;
                        pTcb->client_nodeID = srcNodeId;
                        //modify the seg and send back the ack
                        //����ֱ�������յ��Ķ�
                        pSeg->seg.header.ack_num = pSeg->seg.header.seq_num + 1;
                        pSeg->seg.header.type = SYNACK;
                        pSeg->seg.header.dest_port = pSeg->seg.header.src_port;
                        pSeg->seg.header.src_port = pTcb->server_portNum;
                        pSeg->nodeID = srcNodeId;
                        printf("����SYNACK  Դ�˿ڣ�%d  Ŀ�Ķ˿ڣ�%d \n",pSeg->seg.header.src_port,pSeg->seg.header.dest_port);
                        sip_sendseg(conn, pSeg);
                    }
                        break;
                    case CONNECTED: {
                        if(pTcb->client_portNum!=pSeg->seg.header.src_port){
                            printf("%d �Ŷ˿ں�����%d �Ŷ˿ڽ������ӣ��޷�����%d �Ŷ˿ڽ�������",
                                   pTcb->server_portNum,pTcb->client_portNum,pSeg->seg.header.src_port);
                        }else {
                            printf("%d �Ŷ˿��յ�����%d �Ŷ˿ڵ�����SYN �ش�SYNACK",pTcb->server_portNum,pTcb->client_portNum);
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
                printf("��ǰ�������˿ڣ�%d��״̬Ϊ %d\n",pTcb->server_portNum, pTcb->state);
                printf("�յ����� �ͻ���%d�˿ڵ�FIN����",pTcb->client_portNum);
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
                        printf("������%d�Ŷ˿ڽ���CLOSEWAIT״̬��������һ��ʱ����ͷ���Դ\n", pTcb->server_portNum);
                        pthread_create(&tid, NULL, timeout, (void *) pTcb);
                        break;
                    }
                    case CLOSEWAIT: {
                        printf("������%d�Ŷ˿��յ�����FIN��������ӦFINACK���ͻ���\n", pTcb->server_portNum);
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
                        printf("��ǰ������%d�Ŷ˿�ΪCONNECTED���յ����Կͻ���%d�Ŷ˿ڷ��������ݱ��Ķ�\n"
                                ,pTcb->server_portNum,pTcb->client_portNum);
                        printf("���Ķ���ϢΪ��\n");

                        printf("type: %d\n",pSeg->seg.header.type);
                        printf("length: %d\n",pSeg->seg.header.length);
                        printf("seq_num: %d\n",pSeg->seg.header.seq_num);
                        printf("ack_num: %d\n",pSeg->seg.header.ack_num);
                        if (pSeg->seg.header.seq_num == pTcb->expect_seqNum) {
                            printf("seq_num == expect_num�������Ķη�����ջ�����\n");
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
                        printf("������״̬��Ϣ�쳣");
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
            printf("�ҵ�tcb�飬���Ϊ%d\n", i);
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
            printf("�������˿ڣ�%d CLOSEWAIT״̬����������CLOSED״̬\n",p->server_portNum);

            if(p !=NULL)
                p -> state = CLOSED;
            return NULL;
        }
        sleep(1);
    }
}

