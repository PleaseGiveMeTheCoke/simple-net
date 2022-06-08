//文件名: sip/sip.c
//
//描述: 这个文件实现SIP进程  

#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <string.h>
#include <strings.h>
#include <arpa/inet.h>
#include <signal.h>
#include <netdb.h>
#include <assert.h>
#include <sys/utsname.h>
#include <pthread.h>
#include <unistd.h>

#include "../common/constants.h"
#include "../common/pkt.h"
#include "../common/seg.h"
#include "../topology/topology.h"
#include "sip.h"
#include "nbrcosttable.h"
#include "dvtable.h"
#include "routingtable.h"

//SIP层等待这段时间让SIP路由协议建立路由路径. 
#define  SIP_WAITTIME 60

/**************************************************************/
//声明全局变量
/**************************************************************/
int son_conn; 			//到重叠网络的连接
int stcp_conn;			//到STCP的连接
nbr_cost_entry_t* nct;			//邻居代价表
dv_t* dv;				//距离矢量表
pthread_mutex_t* dv_mutex;		//距离矢量表互斥量
routingtable_t* routingtable;		//路由表
pthread_mutex_t* routingtable_mutex;	//路由表互斥量

/**************************************************************/
//实现SIP的函数
/**************************************************************/

//SIP进程使用这个函数连接到本地SON进程的端口SON_PORT.
//成功时返回连接描述符, 否则返回-1.
int connectToSON() {
    int my_node_id = topology_getMyNodeID();
    int tcp_client = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in server_addr = {0};
    server_addr.sin_family = AF_INET;                           // 设置地址族为IPv4
    server_addr.sin_port = htons(SON_PORT);						// 设置地址的端口号信息
    in_addr_t myIP;
    char ip[20];
    sprintf(ip, "114.212.190.%d",my_node_id);
    inet_aton(ip, (struct in_addr *)&myIP);
    server_addr.sin_addr.s_addr = myIP;
    int ret = connect(tcp_client, (const struct sockaddr *)&server_addr, sizeof(server_addr));
    if (ret < 0) {
        printf("sip can not connect with son\n");
        return -1;
    }
    else {
        printf("sip connect with son successfully.\n");
        son_conn = tcp_client;
        return tcp_client;
    }
}

//这个线程每隔ROUTEUPDATE_INTERVAL时间发送路由更新报文.路由更新报文包含这个节点
//的距离矢量.广播是通过设置SIP报文头中的dest_nodeID为BROADCAST_NODEID,并通过son_sendpkt()发送报文来完成的.
void* routeupdate_daemon(void* arg) {
	//你需要编写这里的代码.
    while (1) {
        sip_pkt_t pkt;
        pkt.header.dest_nodeID = BROADCAST_NODEID;
        pkt.header.src_nodeID = topology_getMyNodeID();
        pkt.header.type = ROUTE_UPDATE;
        pkt_routeupdate_t pru;
        pru.entryNum = topology_getNodeNum();
        pkt.header.length = pru.entryNum * sizeof(routeupdate_entry_t) + sizeof(unsigned int);
        pthread_mutex_lock(dv_mutex);
        dv_t entry= dv[topology_getNbrNum()];
        for(int i = 0;i<pru.entryNum;i++){
            pru.entry[i].nodeID = entry.dvEntry[i].nodeID;
            pru.entry[i].cost = entry.dvEntry[i].cost;
        }
        pthread_mutex_unlock(dv_mutex);

        memcpy(&pkt.data,&pru,pkt.header.length);
        int retnum = son_sendpkt(BROADCAST_NODEID, &pkt, son_conn);
        if(retnum==-1){
            printf("cannot send routing update pkt to son\n");
            break;
        }
        printf("sip have send a routing update pkt to son\n");
        sleep(ROUTEUPDATE_INTERVAL);
    }
}

//这个线程处理来自SON进程的进入报文. 它通过调用son_recvpkt()接收来自SON进程的报文.
//如果报文是SIP报文,并且目的节点就是本节点,就转发报文给STCP进程. 如果目的节点不是本节点,
//就根据路由表转发报文给下一跳.如果报文是路由更新报文,就更新距离矢量表和路由表.
void* pkthandler(void* arg) {
	//你需要编写这里的代码.
    sip_pkt_t pkt;
    while (1){
        int retnum = son_recvpkt(&pkt, son_conn);
        if(retnum==-1){
            printf("sip cannot get pkt from son\n");
            printf("connection between son and sip is close or error\n");
            break;
        }else{
            printf("Routing: sip received a pkt from son. src node id: %d  dest port: %d\n",pkt.header.src_nodeID,pkt.header.dest_nodeID);
            if(pkt.header.type == ROUTE_UPDATE){
                printf("pkt type: ROUTE_UPDATE\n");
                pkt_routeupdate_t* routeupdate = (pkt_routeupdate_t*)pkt.data;
                pthread_mutex_lock(dv_mutex);
                for(int i = 0;i<topology_getNbrNum()+1;i++){
                    if(dv[i].nodeID == pkt.header.src_nodeID){
                        //步骤一：更新源节点的向量表
                        for(int j = 0; j< topology_getNodeNum();j++){
                            dv[i].dvEntry[j].cost = routeupdate->entry[j].cost;
                        }
                    }
                    if(dv[i].nodeID == topology_getMyNodeID()){
                        //步骤二：更新本节点的向量表
                        int cost = dvtable_getcost(dv, topology_getMyNodeID(),pkt.header.src_nodeID);
                        for(int j = 0; j< topology_getNodeNum();j++){
                            int toNodeId = routeupdate->entry[j].nodeID;
                            int newCost = routeupdate->entry[j].cost+cost;
                            int oldCost = dv[i].dvEntry[j].cost;
                            if(newCost < oldCost){
                                dv[i].dvEntry[j].cost = newCost;
                                int slotNum = makehash(toNodeId);
                                routingtable_entry_t* head = routingtable->hash[slotNum];
                                while (head!=NULL){
                                    if(head->destNodeID == toNodeId){
                                        printf("update rounting table,to arrive %d ,the old next nodeId is：%d , update next nodeId to ：%d\n",toNodeId,head->nextNodeID,pkt.header.src_nodeID);
                                        head->nextNodeID = pkt.header.src_nodeID;
                                        break;
                                    }
                                    head = head->next;
                                }
                            }
                        }
                    }
                }
                pthread_mutex_unlock(dv_mutex);
            }else{
                printf("pkt type: SIP\n");
                if(pkt.header.dest_nodeID == topology_getMyNodeID()){
                    printf("dest node id is my node id,forward it to stcp\n");
                    sendseg_arg_t sat;
                    sat.nodeID = pkt.header.src_nodeID;
                    sat.seg = *((seg_t *)pkt.data);
                    forwardsegToSTCP(stcp_conn,&sat);
                }else{
                    printf("dest node id is not my node id, send it to next node by routing table\n");
                    int dest_nodeID = pkt.header.dest_nodeID;
                    int next_nodeID = -1;
                    int slotNum = makehash(dest_nodeID);
                    routingtable_entry_t* head = routingtable->hash[slotNum];
                    while (head!=NULL){
                        if(head->destNodeID == dest_nodeID){
                            printf("find next node in my routing table，next node id is: %d\n",head->nextNodeID);
                            next_nodeID = head->nextNodeID;
                            son_sendpkt(head->nextNodeID, &pkt, son_conn);
                            break;
                        }
                        head = head->next;
                    }
                    if(next_nodeID == -1){
                        printf("can not find dest node id :%d in routing table, discard pkt\n",dest_nodeID);
                    }
                }
            }
        }
    }

    close(son_conn);
    son_conn = -1;
    pthread_exit(NULL);
}

//这个函数终止SIP进程, 当SIP进程收到信号SIGINT时会调用这个函数. 
//它关闭所有连接, 释放所有动态分配的内存.
void sip_stop() {
	//你需要编写这里的代码.
    close(son_conn);
    printf("Sip process is going to exit\n");
    exit(0);
}

//这个函数打开端口SIP_PORT并等待来自本地STCP进程的TCP连接.
//在连接建立后, 这个函数从STCP进程处持续接收包含段及其目的节点ID的sendseg_arg_t. 
//接收的段被封装进数据报(一个段在一个数据报中), 然后使用son_sendpkt发送该报文到下一跳. 下一跳节点ID提取自路由表.
//当本地STCP进程断开连接时, 这个函数等待下一个STCP进程的连接.
void waitSTCP() {
	//你需要编写这里的代码.
    //你需要编写这里的代码.
    int my_node_id = topology_getMyNodeID();
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    int connfd;
    struct sockaddr_in server_addr;
    bzero(&server_addr,sizeof (server_addr));
    server_addr.sin_family = AF_INET;		                    // 设置地址族为IPv4
    server_addr.sin_port = htons(SIP_PORT);
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
    stcp_conn = connfd;
    printf("已接收到来自STCP进程的tcp连接请求\n");
    while (1){
        sendseg_arg_t * sendsegArg = (sendseg_arg_t*) malloc(sizeof(sendseg_arg_t));
        int retnum = getsegToSend(connfd,sendsegArg);
        if(retnum==-1){
            printf("从STCP层接受段异常\n");
            free(sendsegArg);
            break;
        }else{
            printf("收到从STCP层传来的段\n");

            sip_pkt_t pkt;
            int dest_node = sendsegArg->nodeID;
            int next_node = routingtable_getnextnode(routingtable,dest_node);
            pkt.header.dest_nodeID = dest_node;
            pkt.header.src_nodeID = topology_getMyNodeID();
            pkt.header.length = strlen((char*)&(sendsegArg->seg));
            pkt.header.type = SIP;
            memcpy(pkt.data,&(sendsegArg->seg),sizeof(seg_t));

            son_sendpkt(next_node,&pkt,son_conn);

        }
        free(sendsegArg);
    }
}

int main(int argc, char *argv[]) {
	printf("SIP layer is starting, pls wait...\n");

	//初始化全局变量
	nct = nbrcosttable_create();
	dv = dvtable_create();
	dv_mutex = (pthread_mutex_t*)malloc(sizeof(pthread_mutex_t));
	pthread_mutex_init(dv_mutex,NULL);
	routingtable = routingtable_create();
	routingtable_mutex = (pthread_mutex_t*)malloc(sizeof(pthread_mutex_t));
	pthread_mutex_init(routingtable_mutex,NULL);
	son_conn = -1;
	stcp_conn = -1;

	nbrcosttable_print(nct);
	dvtable_print(dv);
	routingtable_print(routingtable);

	//注册用于终止进程的信号句柄
	signal(SIGINT, sip_stop);

	//连接到本地SON进程 
	son_conn = connectToSON();
	if(son_conn<0) {
		printf("can't connect to SON process\n");
		exit(1);		
	}
	
	//启动线程处理来自SON进程的进入报文 
	pthread_t pkt_handler_thread; 
	pthread_create(&pkt_handler_thread,NULL,pkthandler,(void*)0);

	//启动路由更新线程 
	pthread_t routeupdate_thread;
	pthread_create(&routeupdate_thread,NULL,routeupdate_daemon,(void*)0);	

	printf("SIP layer is started...\n");
	printf("waiting for routes to be established\n");
	sleep(SIP_WAITTIME);
	routingtable_print(routingtable);

	//等待来自STCP进程的连接
	printf("waiting for connection from STCP process\n");
	waitSTCP(); 

}


