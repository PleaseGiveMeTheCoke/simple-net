//�ļ���: server/app_stress_server.c

//����: ����ѹ�����԰汾�ķ������������. �������������ӵ�����SIP����. Ȼ��������stcp_server_init()��ʼ��STCP������.
//��ͨ������stcp_server_sock()��stcp_server_accept()�����׽��ֲ��ȴ����Կͻ��˵�����. ��Ȼ������ļ�����. 
//����֮��, ������һ��������, �����ļ����ݲ��������浽receivedtext.txt�ļ���.
//���, ������ͨ������stcp_server_close()�ر��׽���, ���Ͽ��뱾��SIP���̵�����.

//����: ��

//���: STCP������״̬

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

//����һ������, ʹ�ÿͻ��˶˿ں�87�ͷ������˿ں�88. 
#define CLIENTPORT1 87
#define SERVERPORT1 88
//�ڽ��յ��ļ����ݱ������, �������ȴ�15��, Ȼ��ر�����.
#define WAITTIME 15

//����������ӵ�����SIP���̵Ķ˿�SIP_PORT. ���TCP����ʧ��, ����-1. ���ӳɹ�, ����TCP�׽���������, STCP��ʹ�ø����������Ͷ�.
int connectToSIP() {

    //����Ҫ��д����Ĵ���.
    // 1��ʹ��socket()������ȡһ��socket�ļ�������
    int tcp_client = socket(AF_INET, SOCK_STREAM, 0);
    // 2��׼������˵ĵ�ַ�Ͷ˿ڣ�'192.168.0.107'��ʾĿ��ip��ַ��12341��ʾĿ�Ķ˿ں�
    struct sockaddr_in server_addr = {0};
    server_addr.sin_family = AF_INET;                           // ���õ�ַ��ΪIPv4
    server_addr.sin_port = htons(SIP_PORT);// ���õ�ַ�Ķ˿ں���Ϣ
    int my_node_id = topology_getMyNodeID();
    in_addr_t myIP;
    char ip[20];
    sprintf(ip, "114.212.190.%d",my_node_id);
    inet_aton(ip, (struct in_addr *)&myIP);
    server_addr.sin_addr.s_addr = myIP;	//������IP��ַ
    // 3�����ӵ�������
    int ret = connect(tcp_client, (const struct sockaddr *)&server_addr, sizeof(server_addr));
    if (ret < 0)
        perror("connect");
    else
        printf("connect success socket = %d.\n", tcp_client);
    return tcp_client;

}

//��������Ͽ�������SIP���̵�TCP����. 
void disconnectToSIP(int sip_conn) {

	//����Ҫ��д����Ĵ���.
    close(sip_conn);
}

int main() {
	//���ڶ����ʵ����������
	srand(time(NULL));

	//���ӵ�SIP���̲����TCP�׽���������
	int sip_conn = connectToSIP();
	if(sip_conn<0) {
		printf("can not connect to the local SIP process\n");
	}

	//��ʼ��STCP������
	stcp_server_init(sip_conn);

	//�ڶ˿�SERVERPORT1�ϴ���STCP�������׽��� 
	int sockfd= stcp_server_sock(SERVERPORT1);
	if(sockfd<0) {
		printf("can't create stcp server\n");
		exit(1);
	}
	//��������������STCP�ͻ��˵����� 
	stcp_server_accept(sockfd);

	//���Ƚ����ļ�����, Ȼ������ļ�����
	int fileLen;
	stcp_server_recv(sockfd,&fileLen,sizeof(int));
	char* buf = (char*) malloc(fileLen);
	stcp_server_recv(sockfd,buf,fileLen);

	//�����յ����ļ����ݱ��浽�ļ�receivedtext.txt��
	FILE* f;
	f = fopen("receivedtext.txt","a");
	fwrite(buf,fileLen,1,f);
	fclose(f);
	free(buf);

	//�ȴ�һ���
	sleep(WAITTIME);

	//�ر�STCP������ 
	if(stcp_server_close(sockfd)<0) {
		printf("can't destroy stcp server\n");
		exit(1);
	}				

	//�Ͽ���SIP����֮�������
	disconnectToSIP(sip_conn);
}
