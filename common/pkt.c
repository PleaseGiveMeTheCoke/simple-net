// �ļ���: common/pkt.c
#include "pkt.h"
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <errno.h>

// son_sendpkt()��SIP���̵���, ��������Ҫ��SON���̽����ķ��͵��ص�������. SON���̺�SIP����ͨ��һ������TCP���ӻ���.
// ��son_sendpkt()��, ���ļ�����һ���Ľڵ�ID����װ�����ݽṹsendpkt_arg_t, ��ͨ��TCP���ӷ��͸�SON����.
// ����son_conn��SIP���̺�SON����֮���TCP�����׽���������.
// ��ͨ��SIP���̺�SON����֮���TCP���ӷ������ݽṹsendpkt_arg_tʱ, ʹ��'!&'��'!#'��Ϊ�ָ���, ����'!& sendpkt_arg_t�ṹ !#'��˳����.
// ������ͳɹ�, ����1, ���򷵻�-1.
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
// son_recvpkt()������SIP���̵���, �������ǽ�������SON���̵ı���.
// ����son_conn��SIP���̺�SON����֮��TCP���ӵ��׽���������. ����ͨ��SIP���̺�SON����֮���TCP���ӷ���, ʹ�÷ָ���!&��!#.
// Ϊ�˽��ձ���, �������ʹ��һ���򵥵�����״̬��FSM
// PKTSTART1 -- ���
// PKTSTART2 -- ���յ�'!', �ڴ�'&'
// PKTRECV -- ���յ�'&', ��ʼ��������
// PKTSTOP1 -- ���յ�'!', �ڴ�'#'�Խ������ݵĽ���
// ����ɹ����ձ���, ����1, ���򷵻�-1.
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

// ���������SON���̵���, �������ǽ������ݽṹsendpkt_arg_t.
// ���ĺ���һ���Ľڵ�ID����װ��sendpkt_arg_t�ṹ.
// ����sip_conn����SIP���̺�SON����֮���TCP���ӵ��׽���������.
// sendpkt_arg_t�ṹͨ��SIP���̺�SON����֮���TCP���ӷ���, ʹ�÷ָ���!&��!#.
// Ϊ�˽��ձ���, �������ʹ��һ���򵥵�����״̬��FSM
// PKTSTART1 -- ���
// PKTSTART2 -- ���յ�'!', �ڴ�'&'
// PKTRECV -- ���յ�'&', ��ʼ��������
// PKTSTOP1 -- ���յ�'!', �ڴ�'#'�Խ������ݵĽ���
// ����ɹ�����sendpkt_arg_t�ṹ, ����1, ���򷵻�-1.
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

// forwardpktToSIP()��������SON���̽��յ������ص����������ھӵı��ĺ󱻵��õ�.
// SON���̵����������������ת����SIP����.
// ����sip_conn��SIP���̺�SON����֮���TCP���ӵ��׽���������.
// ����ͨ��SIP���̺�SON����֮���TCP���ӷ���, ʹ�÷ָ���!&��!#, ����'!& ���� !#'��˳����.
// ������ķ��ͳɹ�, ����1, ���򷵻�-1.
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

// sendpkt()������SON���̵���, �������ǽ�������SIP���̵ı��ķ��͸���һ��.
// ����conn�ǵ���һ���ڵ��TCP���ӵ��׽���������.
// ����ͨ��SON���̺����ھӽڵ�֮���TCP���ӷ���, ʹ�÷ָ���!&��!#, ����'!& ���� !#'��˳����.
// ������ķ��ͳɹ�, ����1, ���򷵻�-1.
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

// recvpkt()������SON���̵���, �������ǽ��������ص����������ھӵı���.
// ����conn�ǵ����ھӵ�TCP���ӵ��׽���������.
// ����ͨ��SON���̺����ھ�֮���TCP���ӷ���, ʹ�÷ָ���!&��!#.
// Ϊ�˽��ձ���, �������ʹ��һ���򵥵�����״̬��FSM
// PKTSTART1 -- ���
// PKTSTART2 -- ���յ�'!', �ڴ�'&'
// PKTRECV -- ���յ�'&', ��ʼ��������
// PKTSTOP1 -- ���յ�'!', �ڴ�'#'�Խ������ݵĽ���
// ����ɹ����ձ���, ����1, ���򷵻�-1.
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
