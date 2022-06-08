//�ļ���: client/stcp_client.c
//
//����: ����ļ�����STCP�ͻ��˽ӿ�ʵ�� 

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

//����tcbtableΪȫ�ֱ���
client_tcb_t* tcb_table[MAX_TRANSPORT_CONNECTIONS];
//������SIP���̵�TCP����Ϊȫ�ֱ���
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
//STCP APIʵ��
//
/*********************************************************************/

// ���������ʼ��TCB��, ��������Ŀ���ΪNULL.  
// �������TCP�׽���������conn��ʼ��һ��STCP���ȫ�ֱ���, �ñ�����Ϊsip_sendseg��sip_recvseg���������.
// ���, �����������seghandler�߳�����������STCP��. �ͻ���ֻ��һ��seghandler.
void stcp_client_init(int conn) 
{
    for (int i = 0; i < MAX_TRANSPORT_CONNECTIONS; ++i) {
        tcb_table[i] = NULL;
    }
    sip_conn = conn;
    pthread_create(&seghandler_t,NULL,seghandler,NULL);
    return;
}

// ����������ҿͻ���TCB�����ҵ���һ��NULL��Ŀ, Ȼ��ʹ��malloc()Ϊ����Ŀ����һ���µ�TCB��Ŀ.
// ��TCB�е������ֶζ�����ʼ��. ����, TCB state������ΪCLOSED���ͻ��˶˿ڱ�����Ϊ�������ò���client_port. 
// TCB������Ŀ��������Ӧ��Ϊ�ͻ��˵����׽���ID�������������, �����ڱ�ʶ�ͻ��˵�����. 
// ���TCB����û����Ŀ����, �����������-1.
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

            //���ͻ�����������ݳ�ʼ��
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
// ��������������ӷ�����. �����׽���ID, �������ڵ�ID�ͷ������Ķ˿ں���Ϊ�������. �׽���ID�����ҵ�TCB��Ŀ.  
// �����������TCB�ķ������ڵ�ID�ͷ������˿ں�,  Ȼ��ʹ��sip_sendseg()����һ��SYN�θ�������.  
// �ڷ�����SYN��֮��, һ����ʱ��������. �����SYNSEG_TIMEOUTʱ��֮��û���յ�SYNACK, SYN �ν����ش�. 
// ����յ���, �ͷ���1. ����, ����ش�SYN�Ĵ�������SYN_MAX_RETRY, �ͽ�stateת����CLOSED, ������-1.
int stcp_client_connect(int sockfd, int nodeID, unsigned int server_port) 
{
    client_tcb_t* tcb = tcb_table[sockfd];
    if(tcb==NULL){
        printf("%d��tcb������\n",sockfd);
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
            //stcp_sock��init�г�ʼ�����
            sip_sendseg(sip_conn, &syn_seg);
            printf("�ͻ���ip: %d �˿�: %d �������ip: %d �˿�: %d������SYN\n",tcb->client_nodeID,tcb->client_portNum,nodeID,server_port);
            sleep(1);
            //usleep(SYN_TIMEOUT / 1000);
            while (tcb->state != CONNECTED && try <= SYN_MAX_RETRY) {
                try++;
                printf("������ip: %d �˿�: %d����Ӧ�����е�%d���ش�\n",nodeID, syn_seg.seg.header.dest_port, try);
                sip_sendseg(sip_conn, &syn_seg);
                sleep(1);
            }
            if (tcb->state == CONNECTED) {
                printf("�ͻ��ˣ�%d�Ŷ˿��� ����ˣ�%d�Ŷ˿ڳɹ���������\n",tcb->client_portNum,tcb->server_portNum);
                return 1;
            } else {
                tcb->state = CLOSED;
                printf("�ͻ��ˣ�%d�Ŷ˿������ˣ�%d�Ŷ˿��ش��������࣬����ʧ��\n",tcb->client_portNum,server_port);
                return -1;
            }
        }
        case SYNSENT:{
            printf("״̬�쳣,��ǰ״̬Ϊsyn_sent,����Ϊclosed\n");
            break;
        }
        case CONNECTED:{
            printf("״̬�쳣,��ǰ״̬Ϊconnected,����Ϊclosed\n");
            return -1;
            break;
        }
        case FINWAIT:{
            printf("״̬�쳣,��ǰ״̬Ϊfinwait,����Ϊclosed\n");
            return -1;
            break;
        }
    }
    return -1;
}

// �������ݸ�STCP������. �������ʹ���׽���ID�ҵ�TCB���е���Ŀ.
// Ȼ����ʹ���ṩ�����ݴ���segBuf, �������ӵ����ͻ�����������.
// ������ͻ������ڲ�������֮ǰΪ��, һ����Ϊsendbuf_timer���߳̾ͻ�����.
// ÿ��SENDBUF_ROLLING_INTERVALʱ���ѯ���ͻ������Լ���Ƿ��г�ʱ�¼�����. 
// ��������ڳɹ�ʱ����1�����򷵻�-1. 
// stcp_client_send��һ����������������.
// ��Ϊ�û����ݱ���ƬΪ�̶���С��STCP��, ����һ��stcp_client_send���ÿ��ܻ�������segBuf
// ����ӵ����ͻ�����������. ������óɹ�, ���ݾͱ�����TCB���ͻ�����������, ���ݻ������ڵ����,
// ���ݿ��ܱ����䵽������, ���ڶ����еȴ�����.
void add_to_buffer(client_tcb_t* tcb, unsigned int length, void* data){
    //�������ݶοռ�
    segBuf_t *pSegBuf = (segBuf_t *) malloc(sizeof(segBuf_t));
    //���
    memset(pSegBuf, 0, sizeof(segBuf_t));
    //��������
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
        printf("Ҫ�������ݵ�%d��tcbΪ��\n",sockfd);
        return -1;
    }else if(tcb->state!=CONNECTED){
        printf("Ҫ�������ݵ�%d��tcb״̬�쳣��Ϊ%d\n",sockfd,tcb->state);
        return -1;
    }
    //�����ݼ��뻺����
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
    //���ͻ���������ֱ���ѷ���δȷ����Ŀ�ﵽGBN_WINDOW
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
        printf("%s:, seq_num Ϊ%d�ı��Ķ��ѷ���\n", __func__, tcb->sendBufunSent->seg.header.seq_num);
        tcb->sendBufunSent = tcb->sendBufunSent->next;
        tcb->unAck_segNum++;
    }
    pthread_mutex_unlock(tcb->bufMutex);
}

// ����������ڶϿ���������������. �����׽���ID��Ϊ�������. �׽���ID�����ҵ�TCB���е���Ŀ.  
// �����������FIN�θ�������. �ڷ���FIN֮��, state��ת����FINWAIT, ������һ����ʱ��.
// ��������ճ�ʱ֮ǰstateת����CLOSED, �����FINACK�ѱ��ɹ�����. ����, ����ھ���FIN_MAX_RETRY�γ���֮��,
// state��ȻΪFINWAIT, state��ת����CLOSED, ������-1.
int stcp_client_disconnect(int sockfd) {
    printf("���ԶϿ�%d��tcb���������\n",sockfd);
    client_tcb_t* tcb = tcb_table[sockfd];
    if(tcb==NULL){
        printf("�Ͽ�����ʧ�ܣ�%d��tcb״̬�쳣\n",sockfd);
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
            printf("���ӹر�ʧ�ܣ�%d��tcb״̬�쳣��Ϊclosed\n",sockfd);
            return -1;
            break;
        }
        case SYNSENT:
        {
            printf("���ӹر�ʧ�ܣ�%d��tcb״̬�쳣��ΪSYNSENT\n",sockfd);
            return -1;
            break;
        }
        case CONNECTED:
        {
            tcb->state = FINWAIT;
            int try_times = 0;
            sip_sendseg(sip_conn,&fin_seg);
            printf("�ͻ���%d�Ŷ˿ڳ����������%d�Ŷ˿ڷ���fin\n",tcb->client_portNum,tcb->server_portNum);
            printf("��ǰ�ͻ���%d�Ŷ˿�״̬��CONNECTED��ΪFINWAIT\n",tcb->client_portNum);

            sleep(1);
            while(tcb->state != CLOSED && try_times < FIN_MAX_RETRY){
                try_times++;
                sip_sendseg(sip_conn, &fin_seg);
                printf("��ʱ��δ�յ�FINACK����ʼ��%d���ش�FIN����\n",try_times);
                sleep(1);
            }

            if (tcb->state == CLOSED){
                printf("�ͻ��ˣ�%d ���������%d�˿ڵ����ӹرճɹ�\n",tcb->client_portNum,tcb->server_portNum);
                return 1;
            } else {
                printf("���ӹر�ʧ��\n");
                tcb->state = CLOSED;
                return -1;
            }
        }
        case FINWAIT:
        {
            printf("���ӹر�ʧ�ܣ�tcb״̬�쳣��Ϊfinwait\n");
            return -1;
            break;
        }
    }


    return 0;
}

// �����������free()�ͷ�TCB��Ŀ. ��������Ŀ���ΪNULL, �ɹ�ʱ(��λ����ȷ��״̬)����1,
// ʧ��ʱ(��λ�ڴ����״̬)����-1.
int stcp_client_close(int sockfd) {
    printf("�ͷ�%d��tcb����Դ\n",sockfd);
    client_tcb_t* tcb =  tcb_table[sockfd];
    if(tcb==NULL){
        printf("û���ҵ�%d��tcb\n",sockfd);
        return -1;
    }else{
        free(tcb->bufMutex);
        free(tcb);
        tcb_table[sockfd] = NULL;
        printf("�ͷ�%d��tcb�ɹ�\n",sockfd);
        return 1;
    }
}

// ������stcp_client_init()�������߳�. �������������Է������Ľ����. 
// seghandler�����Ϊһ������sip_recvseg()������ѭ��. ���sip_recvseg()ʧ��, ��˵����SIP���̵������ѹر�,
// �߳̽���ֹ. ����STCP�ε���ʱ����������״̬, ���Բ�ȡ��ͬ�Ķ���. ��鿴�ͻ���FSM���˽����ϸ��.
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
            printf("�ͻ����յ����Ķ�\n");
            client_tcb_t *item = NULL;
            for (int i = 0; i < MAX_TRANSPORT_CONNECTIONS; i++) {
                if (tcb_table[i] != NULL && tcb_table[i]->client_portNum == recv_seg->seg.header.dest_port) {
                    item = tcb_table[i];
                    break;
                }
            }
            if (item == NULL) {
                printf("û���ҵ�Ŀ�Ķ˿�%d����Ŀ�Ķ˿�%d�ѱ�ռ��\n", recv_seg->seg.header.dest_port, recv_seg->seg.header.dest_port);
                continue;
            }
            switch (item->state) {
                case CLOSED: {
                    printf("Ŀ�Ķ˿�%d������״̬Ϊ�ѹر�,�޷������Ķ�\n",recv_seg->seg.header.dest_port);
                    break;
                }
                case SYNSENT: {
                    if (recv_seg->seg.header.type == SYNACK) {
                        printf("�ͻ���%d�Ŷ˿��յ����Է�����%d�Ŷ˿ڵ�SYNACK,״̬����Ϊ������\n",recv_seg->seg.header.dest_port,recv_seg->seg.header.src_port);
                        item->state = CONNECTED;
                        item->server_portNum = recv_seg->seg.header.src_port;
                    } else {
                        printf("�ͻ���%d�Ŷ˿ڵ�ǰ״̬Ϊsynsent���޷�������synack��ı���\n",recv_seg->seg.header.dest_port);
                    }
                    break;
                }
                case CONNECTED: {
                    printf("�ͻ���%d�˿ڵĵ�ǰ״̬ΪCONNECTED���յ����Է�����%d�Ŷ˿ڱ��ĶΣ���������Ϊ�� %d\n", recv_seg->seg.header.dest_port,
                           recv_seg->seg.header.src_port,recv_seg->seg.header.type);
                    if (recv_seg->seg.header.type == DATAACK) {
                        segBuf_t *pSegBuf = item->sendBufHead;
                        //ɾ����Щ���к�С�ڷ����������Ĵ����ܵ���һ���кŵĶ�
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
                            printf("%s:, seq_num Ϊ%d�ı��Ķ��ѷ���\n", __func__, pSegBuf->seg.header.seq_num);
                            item->sendBufunSent = item->sendBufunSent->next;
                            item->unAck_segNum++;
                        }
                        pthread_mutex_unlock(item->bufMutex);
                    }

                    else if(recv_seg->seg.header.type==SYNACK){
                        printf("�ͻ���%d�˿����������%d�˿ڽ������ӣ�������Ӧ��SYNACK���Ķ�",
                               item->client_portNum,item->server_portNum);
                    }else{
                        //����������Ӧ
                    }
                    break;
                }
                case FINWAIT: {
                    if (recv_seg->seg.header.type == FINACK) {
                        printf("�ͻ���%d�Ŷ˿��յ�FINACK,����closed״̬\n",item->client_portNum);
                        item->state = CLOSED;
                    } else {
                        printf("�ͻ���%d�Ŷ˿��յ���Ч���ĶΣ���ǰ״̬��finwait\n",item->client_portNum);
                    }
                    break;
                }
            }
        }
        free(recv_seg);
    }
}


//����̳߳�����ѯ���ͻ������Դ�����ʱ�¼�. ������ͻ������ǿ�, ��Ӧһֱ����.
//���(��ǰʱ�� - ��һ���ѷ��͵�δ��ȷ�϶εķ���ʱ��) > DATA_TIMEOUT, �ͷ���һ�γ�ʱ�¼�.
//����ʱ�¼�����ʱ, ���·��������ѷ��͵�δ��ȷ�϶�. �����ͻ�����Ϊ��ʱ, ����߳̽���ֹ.
void* sendBuf_timer(void* clienttcb)
{
    client_tcb_t* tcb = (client_tcb_t*)clienttcb;
    while (1){
        if(tcb->sendBufHead==NULL){
            printf("%d�Ŷ˿ڵķ��ͻ�����Ϊ�գ�ֹͣ��ʱ�߳�\n",tcb->client_portNum);
            break;
        }else{
            pthread_mutex_lock(tcb->bufMutex);
            segBuf_t* head = tcb->sendBufHead;
            if(get_current_time()-head->sentTime>1){
                printf("��ʱ���ش������ѷ��͵�δȷ�ϵĶ�\n");
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

