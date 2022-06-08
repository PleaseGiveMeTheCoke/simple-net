//文件名: server/app_stress_server.c

//描述: 这是压力测试版本的服务器程序代码. 服务器首先连接到本地SIP进程. 然后它调用stcp_server_init()初始化STCP服务器.
//它通过调用stcp_server_sock()和stcp_server_accept()创建套接字并等待来自客户端的连接. 它然后接收文件长度. 
//在这之后, 它创建一个缓冲区, 接收文件数据并将它保存到receivedtext.txt文件中.
//最后, 服务器通过调用stcp_server_close()关闭套接字, 并断开与本地SIP进程的连接.

//输入: 无

//输出: STCP服务器状态

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <time.h>
#include "../topology/topology.h"

#include "../common/constants.h"
#include "stcp_server.h"

//创建一个连接, 使用客户端端口号87和服务器端口号88. 
#define CLIENTPORT1 87
#define SERVERPORT1 88
//在接收的文件数据被保存后, 服务器等待15秒, 然后关闭连接.
#define WAITTIME 15

//这个函数连接到本地SIP进程的端口SIP_PORT. 如果TCP连接失败, 返回-1. 连接成功, 返回TCP套接字描述符, STCP将使用该描述符发送段.
int connectToSIP() {

    //你需要编写这里的代码.
    // 1、使用socket()函数获取一个socket文件描述符
    int tcp_client = socket(AF_INET, SOCK_STREAM, 0);
    // 2、准备服务端的地址和端口，'192.168.0.107'表示目的ip地址，12341表示目的端口号
    struct sockaddr_in server_addr = {0};
    server_addr.sin_family = AF_INET;                           // 设置地址族为IPv4
    server_addr.sin_port = htons(SIP_PORT);// 设置地址的端口号信息
    int my_node_id = topology_getMyNodeID();
    in_addr_t myIP;
    char ip[20];
    sprintf(ip, "114.212.190.%d",my_node_id);
    inet_aton(ip, (struct in_addr *)&myIP);
    server_addr.sin_addr.s_addr = myIP;	//　设置IP地址
    // 3、链接到服务器
    int ret = connect(tcp_client, (const struct sockaddr *)&server_addr, sizeof(server_addr));
    if (ret < 0)
        perror("connect");
    else
        printf("connect success socket = %d.\n", tcp_client);
    return tcp_client;

}

//这个函数断开到本地SIP进程的TCP连接. 
void disconnectToSIP(int sip_conn) {

	//你需要编写这里的代码.
    close(sip_conn);
}

int main() {
	//用于丢包率的随机数种子
	srand(time(NULL));

	//连接到SIP进程并获得TCP套接字描述符
	int sip_conn = connectToSIP();
	if(sip_conn<0) {
		printf("can not connect to the local SIP process\n");
	}

	//初始化STCP服务器
	stcp_server_init(sip_conn);

	//在端口SERVERPORT1上创建STCP服务器套接字 
	int sockfd= stcp_server_sock(SERVERPORT1);
	if(sockfd<0) {
		printf("can't create stcp server\n");
		exit(1);
	}
	//监听并接受来自STCP客户端的连接 
	stcp_server_accept(sockfd);

	//首先接收文件长度, 然后接收文件数据
	int fileLen;
	stcp_server_recv(sockfd,&fileLen,sizeof(int));
	char* buf = (char*) malloc(fileLen);
	stcp_server_recv(sockfd,buf,fileLen);

	//将接收到的文件数据保存到文件receivedtext.txt中
	FILE* f;
	f = fopen("receivedtext.txt","a");
	fwrite(buf,fileLen,1,f);
	fclose(f);
	free(buf);

	//等待一会儿
	sleep(WAITTIME);

	//关闭STCP服务器 
	if(stcp_server_close(sockfd)<0) {
		printf("can't destroy stcp server\n");
		exit(1);
	}				

	//断开与SIP进程之间的连接
	disconnectToSIP(sip_conn);
}
