
#include "seg.h"
#include <stdlib.h>
#include <sys/socket.h>
#include "seg.h"
#include "stdio.h"
#include <errno.h>
//STCP进程使用这个函数发送sendseg_arg_t结构(包含段及其目的节点ID)给SIP进程.
//参数sip_conn是在STCP进程和SIP进程之间连接的TCP描述符.
//如果sendseg_arg_t发送成功,就返回1,否则返回-1.
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


int sip_sendseg(int sip_conn, sendseg_arg_t* segPtr)
{
    segPtr->seg.header.checksum = checksum(&(segPtr->seg));
    printf("发送报文段  from:%d to:%d type:%d\n",segPtr->seg.header.src_port,
           segPtr->seg.header.dest_port,segPtr->seg.header.type);
    if(send(sip_conn,"!&",2,0) > 0)
        if(send(sip_conn,segPtr,sizeof(*segPtr),0) > 0)
            if(send(sip_conn,"!#",2,0) > 0)
                return 1;
    return -1;
}

//STCP进程使用这个函数来接收来自SIP进程的包含段及其源节点ID的sendseg_arg_t结构.
//参数sip_conn是STCP进程和SIP进程之间连接的TCP描述符.
//当接收到段时, 使用seglost()来判断该段是否应被丢弃并检查校验和.
//如果成功接收到sendseg_arg_t就返回1, 否则返回-1.
int sip_recvseg(int sip_conn, sendseg_arg_t* segPtr)
{
    int state = 0;
    char ch;
    printf("尝试从虚拟网络层中读取段\n");
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

                    if(readn(sip_conn,(void *)segPtr,sizeof(sendseg_arg_t)) < 0){
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
                    if (seglost(&(segPtr->seg)) == 1){
                        printf("we lost a pack\n");
                        return 1;
                    }else if (checkchecksum(&(segPtr->seg)) == -1){
                        printf("checksum error, abandom the pack\n");
                        return 1;
                    }else{
                        return 0;
                    }
                }else {
                    return -1;
                }
            }
        }
    }
    return -1;
}

//SIP进程使用这个函数接收来自STCP进程的包含段及其目的节点ID的sendseg_arg_t结构.
//参数stcp_conn是在STCP进程和SIP进程之间连接的TCP描述符.
//如果成功接收到sendseg_arg_t就返回1, 否则返回-1.
int getsegToSend(int stcp_conn, sendseg_arg_t* segPtr)
{
    int state = 0;
    char ch;
    printf("尝试从虚拟网络层中读取段\n");
    while(readn(stcp_conn,&ch,1) > 0){
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

                    if(readn(stcp_conn,(void *)segPtr,sizeof(sendseg_arg_t)) < 0){
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
                    if (seglost(&(segPtr->seg)) == 1){
                        printf("we lost a pack\n");
                        return 1;
                    }else if (checkchecksum(&(segPtr->seg)) == -1){
                        printf("checksum error, abandom the pack\n");
                        return 1;
                    }else{
                        return 0;
                    }
                }else {
                    return -1;
                }
            }
        }
    }
    return -1;
}

//SIP进程使用这个函数发送包含段及其源节点ID的sendseg_arg_t结构给STCP进程.
//参数stcp_conn是STCP进程和SIP进程之间连接的TCP描述符.
//如果sendseg_arg_t被成功发送就返回1, 否则返回-1.
int forwardsegToSTCP(int stcp_conn, sendseg_arg_t* segPtr)
{
    segPtr->seg.header.checksum = checksum(&(segPtr->seg));
    printf("发送报文段  from:%d to:%d type:%d\n",segPtr->seg.header.src_port,
           segPtr->seg.header.dest_port,segPtr->seg.header.type);
    if(send(stcp_conn,"!&",2,0) > 0)
        if(send(stcp_conn,segPtr,sizeof(*segPtr),0) > 0)
            if(send(stcp_conn,"!#",2,0) > 0)
                return 1;
    return -1;
}

// 一个段有PKT_LOST_RATE/2的可能性丢失, 或PKT_LOST_RATE/2的可能性有着错误的校验和.
// 如果数据包丢失了, 就返回1, 否则返回0. 
// 即使段没有丢失, 它也有PKT_LOST_RATE/2的可能性有着错误的校验和.
// 我们在段中反转一个随机比特来创建错误的校验和.
int seglost(seg_t* segPtr) {
    int random = rand()%100;
    if(random<PKT_LOSS_RATE*100) {
        //50%可能性丢失段
        if(rand()%2==0) {
            printf("seg lost!!!\n");
            return 1;
        }
            //50%可能性是错误的校验和
        else {
            //获取数据长度
            int len = sizeof(stcp_hdr_t)+segPtr->header.length;
            //获取要反转的随机位
            int errorbit = rand()%(len*8);
            //反转该比特
            char* temp = (char*)segPtr;
            temp = temp + errorbit/8;
            *temp = *temp^(1<<(errorbit%8));
            return 0;
        }
    }
    return 0;
}
unsigned short checksum(seg_t* segment)
{
    segment->header.checksum = 0x0;
    int count = sizeof(seg_t);
    char *addr = (char *)segment;
    unsigned long sum = 0;
    while(count > 1){
        sum += *(unsigned short*)addr;
        addr += 2;
        count -= 2;
    }
    //add remaining byte
    if(count > 0)
        sum += *(unsigned char*)addr;
    while(sum >> 16)
        //sum&0xffff:获取0-16位的低位值
        sum = (sum & 0xffff) + (sum >> 16);
    return ~sum;
}

//这个函数检查段中的校验和, 正确时返回1, 错误时返回-1
int checkchecksum(seg_t* segment)
{
    int count = sizeof(seg_t);
    char *addr = (char *)segment;
    unsigned long sum = 0;
    while(count > 1){
        sum += *(unsigned short*)addr;
        addr += 2;
        count -= 2;
    }
    //add remaining byte
    if(count > 0)
        sum += *(unsigned char*)addr;
    while(sum >> 16)
        sum = (sum & 0xffff) + (sum >> 16);
    if(sum != 0xffff)
        return -1;
    else
        return 1;
}
