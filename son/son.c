//文件名: son/son.c
//
//描述: 这个文件实现SON进程
//SON进程首先连接到所有邻居, 然后启动listen_to_neighbor线程, 每个该线程持续接收来自一个邻居的进入报文, 并将该报文转发给SIP进程. 
//然后SON进程等待来自SIP进程的连接. 在与SIP进程建立连接之后, SON进程持续接收来自SIP进程的sendpkt_arg_t结构, 并将接收到的报文发送到重叠网络中.  

#include <stdlib.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <strings.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <signal.h>
#include <sys/utsname.h>
#include <assert.h>
#include <stdbool.h>
#include <errno.h>

#include "../common/constants.h"
#include "../common/pkt.h"
#include "son.h"
#include "../topology/topology.h"
#include "neighbortable.h"

//你应该在这个时间段内启动所有重叠网络节点上的SON进程
#define SON_START_DELAY 60

/**************************************************************/
//声明全局变量
/**************************************************************/

//将邻居表声明为一个全局变量 
nbr_entry_t* nt; 
//将与SIP进程之间的TCP连接声明为一个全局变量
int sip_conn;
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
/**************************************************************/
//实现重叠网络函数
/**************************************************************/

// 这个线程打开TCP端口CONNECTION_PORT, 等待节点ID比自己大的所有邻居的进入连接,
// 在所有进入连接都建立后, 这个线程终止.
void* waitNbrs(void* arg) {
    //你需要编写这里的代码.
    int my_node_id = topology_getMyNodeID();
    printf("本机 nodeId = %d\n，下面开始等待nodeID比自己大的所有邻居进入连接",my_node_id);

    int nbr_num = topology_getNbrNum();
    int big_node_count = 0;
    for (int i = 0; i < nbr_num; ++i) {
        if(nt[i].nodeID>my_node_id){
            big_node_count++;
        }
    }
    printf("比本机 nodeId大的邻居节点有%d个\n",big_node_count);

    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    int connfd;
    struct sockaddr_in server_addr;
    bzero(&server_addr,sizeof (server_addr));
    server_addr.sin_family = AF_INET;		                    // 设置地址族为IPv4
    server_addr.sin_port = htons(CONNECTION_PORT);
    in_addr_t myIP;
    char ip[20];
    sprintf(ip, "114.212.190.%d",my_node_id);
    inet_aton(ip, (struct in_addr *)&myIP);
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    int ret = bind(listen_fd, (const struct sockaddr *)&server_addr, sizeof(server_addr));
    if (ret < 0)
        perror("bind");
    else
        printf("bind success.\n");
    //3.listen()

    listen(listen_fd, 1024);
    while (big_node_count>0) {
        struct sockaddr_in cliaddr;
        socklen_t clilen = sizeof(cliaddr);
        //4.accept()
        connfd = accept(listen_fd, (struct sockaddr *) &cliaddr, &clilen);
        int cli_node_id = topology_getNodeIDfromip(&cliaddr.sin_addr);
        printf("已成功与nodeID = %d 的邻居节点建立tcp连接\n",cli_node_id);
        big_node_count--;
        for (int i = 0; i < nbr_num; ++i) {
            if(nt[i].nodeID == cli_node_id){
                nt[i].conn = connfd;
                break;
            }
        }
    }
}

// 这个函数连接到节点ID比自己小的所有邻居.
// 在所有外出连接都建立后, 返回1, 否则返回-1.
int connectNbrs() {
    //你需要编写这里的代码.
    int my_node_id = topology_getMyNodeID();
    printf("本机 nodeId = %d，下面开始向nodeID比自己小的节点发送连接请求\n",my_node_id);
    int nbr_num = topology_getNbrNum();
    for (int i = 0; i < nbr_num; ++i) {
        if(nt[i].nodeID<my_node_id){
            printf("nodeId = %d的邻居节点小于本机节点，尝试发起连接\n",nt[i].nodeID);
            int tcp_client = socket(AF_INET, SOCK_STREAM, 0);
            struct sockaddr_in server_addr = {0};
            server_addr.sin_family = AF_INET;                           // 设置地址族为IPv4
            server_addr.sin_port = htons(CONNECTION_PORT);						// 设置地址的端口号信息
            server_addr.sin_addr.s_addr = nt[i].nodeIP;
            int ret = connect(tcp_client, (const struct sockaddr *)&server_addr, sizeof(server_addr));
            if (ret < 0)
                printf("与nodeID = %d的邻居节点无法建立连接\n",nt[i].nodeID);
            else
                printf("成功与nodeID = %d的邻居节点建立连接.\n",nt[i].nodeID);
            nt[i].conn = tcp_client;
        }
    }
    return 1;

}

//每个listen_to_neighbor线程持续接收来自一个邻居的报文. 它将接收到的报文转发给SIP进程.
//所有的listen_to_neighbor线程都是在到邻居的TCP连接全部建立之后启动的.
void* listen_to_neighbor(void* arg) {
    int nbr = *((int*) arg);
    //与该邻居的tcp连接描述符
    int conn = nt[nbr].conn;
    printf("准备建立连接接受 nodeID = %d 的邻居传来的报文",nt[nbr].nodeID);
    while (1) {
        //TODO
        sip_pkt_t *pkt = (sip_pkt_t *) malloc(sizeof(sip_pkt_t));
        int retnum = recvpkt(pkt, conn);
        if (retnum == -1) {
            printf("与 nodeID = %d 邻居的连接关闭", nt[nbr].nodeID);
            free(pkt);
            break;
        } else {

            printf("收到来自 nodeID = %d 邻居的报文,现在将其转发给SIP", nt[nbr].nodeID);
            forwardpktToSIP(pkt, sip_conn);
        }
        free(pkt);
    }
    return 0;
}

//这个函数打开TCP端口SON_PORT, 等待来自本地SIP进程的进入连接. 
//在本地SIP进程连接之后, 这个函数持续接收来自SIP进程的sendpkt_arg_t结构, 并将报文发送到重叠网络中的下一跳. 
//如果下一跳的节点ID为BROADCAST_NODEID, 报文应发送到所有邻居节点.
void waitSIP() {
    //你需要编写这里的代码.
    int my_node_id = topology_getMyNodeID();
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    int connfd;
    struct sockaddr_in server_addr;
    bzero(&server_addr,sizeof (server_addr));
    server_addr.sin_family = AF_INET;		                    // 设置地址族为IPv4
    server_addr.sin_port = htons(SON_PORT);
    in_addr_t myIP;
    char ip[20];
    sprintf(ip, "114.212.190.%d",my_node_id);
    inet_aton(ip, (struct in_addr *)&myIP);
    server_addr.sin_addr.s_addr = myIP;
    int ret = bind(listen_fd, (const struct sockaddr *)&server_addr, sizeof(server_addr));
    if (ret < 0)
        perror("bind");
    else
        printf("bind success.\n");

    //3.listen()
    struct sockaddr_in cliaddr;
    listen(listen_fd, 1024);
    socklen_t clilen = sizeof(cliaddr);
    //4.accept()
    connfd = accept(listen_fd, (struct sockaddr *) &cliaddr, &clilen);
    sip_conn = connfd;
    printf("son recv tcp connection request from sip\n");
    while (1){
        sendpkt_arg_t* sendpktArg = (sendpkt_arg_t*) malloc(sizeof(sendpkt_arg_t));
        int retnum = getpktToSend(sendpktArg,sip_conn);
        if(retnum==-1){
            printf("son recv from sip error\n");
            free(sendpktArg);
            break;
        }else{
            printf("son recv something from sip\n");
            int next_node = sendpktArg->nextNodeID;
            printf("next NodeID = %d\n",next_node);
            int nbrNum = topology_getNbrNum();
            int flag = 0;
            if(next_node == BROADCAST_NODEID){
                printf("it is a broadcast pkt, send to every nbr\n");
            }
            for (int i = 0; i < nbrNum; ++i) {
                if(nt[i].nodeID == next_node || next_node == BROADCAST_NODEID){

                    printf("find next rip from nbr and send it\n");
                    sendpkt(&sendpktArg->pkt,nt[i].conn);
                    flag = 1;
                }
            }
            if(flag == 0){
                printf("discard pkt，no next rip can be found from nbrtable\n");
            }
        }
        free(sendpktArg);
    }

}

//这个函数停止重叠网络, 当接收到信号SIGINT时, 该函数被调用.
//它关闭所有的连接, 释放所有动态分配的内存.
void son_stop() {
    //你需要编写这里的代码.
    int nbr_num = topology_getNbrNum();
    int i;

    for(i = 0; i < nbr_num; i++){
        if (nt[i].conn >= 0)
            close(nt[i].conn);
    }
    close(sip_conn);

    free(nt);
    printf("son exit\n");
    exit(0);
}

int main() {
	//启动重叠网络初始化工作
	printf("Overlay network: Node %d initializing...\n",topology_getMyNodeID());	

	//创建一个邻居表
	nt = nt_create();
	//将sip_conn初始化为-1, 即还未与SIP进程连接.
	sip_conn = -1;
	
	//注册一个信号句柄, 用于终止进程
	signal(SIGINT, son_stop);

	//打印所有邻居
	int nbrNum = topology_getNbrNum();
	int i;
	for(i=0;i<nbrNum;i++) {
		printf("Overlay network: neighbor %d:%d\n",i+1,nt[i].nodeID);
	}

	//启动waitNbrs线程, 等待节点ID比自己大的所有邻居的进入连接
	pthread_t waitNbrs_thread;
	pthread_create(&waitNbrs_thread,NULL,waitNbrs,(void*)0);

	//等待其他节点启动
	sleep(SON_START_DELAY);
	
	//连接到节点ID比自己小的所有邻居
	connectNbrs();

	//等待waitNbrs线程返回
	pthread_join(waitNbrs_thread,NULL);	

	//此时, 所有与邻居之间的连接都建立好了
	
	//创建线程监听所有邻居
	for(i=0;i<nbrNum;i++) {
		int* idx = (int*)malloc(sizeof(int));
		*idx = i;
		pthread_t nbr_listen_thread;
		pthread_create(&nbr_listen_thread,NULL,listen_to_neighbor,(void*)idx);
	}
	printf("Overlay network: node initialized...\n");
	printf("Overlay network: waiting for connection from SIP process...\n");

	//等待来自SIP进程的连接
	waitSIP();
}
