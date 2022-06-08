//文件名: son/neighbortable.c
//
//描述: 这个文件实现用于邻居表的API
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include "neighbortable.h"
#include "../topology/topology.h"

//这个函数首先动态创建一个邻居表. 然后解析文件topology/topology.dat, 填充所有条目中的nodeID和nodeIP字段, 将conn字段初始化为-1.
//返回创建的邻居表.
nbr_entry_t* nt_create()
{
    printf("开始创建邻居表\n");
    int nbr_num = topology_getNbrNum();
    printf("邻居个数为：%d\n",nbr_num);
    nbr_entry_t* table = (nbr_entry_t*) malloc(sizeof (nbr_entry_t)*nbr_num);
    int* nbr_array =  topology_getNbrArray();
    for (int i = 0; i < nbr_num; ++i) {
        table[i].conn = -1;
        table[i].nodeID = nbr_array[i];
        char ip[20];
        sprintf(ip, "114.212.190.%d", table[i].nodeID);
        printf("邻居%d的nodeID为：%d  nodeIP为：%s\n",i+1,table[i].nodeID,ip);

        inet_aton(ip, (struct in_addr *)&table[i].nodeIP);
    }
    free(nbr_array);
    return table;
}

//这个函数删除一个邻居表. 它关闭所有连接, 释放所有动态分配的内存.
void nt_destroy(nbr_entry_t* nt)
{
    int nbr_num = topology_getNbrNum();
    int i;

    for (i = 0; i < nbr_num; i++){
        close(nt[i].conn);
    }

    free(nt);

    return;
}

//这个函数为邻居表中指定的邻居节点条目分配一个TCP连接. 如果分配成功, 返回1, 否则返回-1.
int nt_addconn(nbr_entry_t* nt, int nodeID, int conn)
{
    int nbr_num = topology_getNbrNum();
    for (int i = 0; i < nbr_num; i++){
        if (nt[i].nodeID == nodeID){
            nt[i].conn = conn;
            return 1;
        }
    }
    return -1;
}
