//文件名: topology/topology.c
//
//描述: 这个文件实现一些用于解析拓扑文件的辅助函数 

#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <assert.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include "topology.h"
#include "../common/constants.h"
struct topology_t{
    char node1[32];
    int id1;
    char node2[32];
    int id2;
    unsigned int cost;
};

struct topology_t   topology_table[10];
int table_num = 0;
int isRead = -1;
int host_id = -1;
int topology_read(){
    FILE *fp = fopen("../topology/topology.dat", "r");
    assert(fp != NULL);
    table_num = 0;
    isRead = 1;
    struct topology_t *tp = &topology_table[table_num];
    while (fscanf(fp, "%s %s %u\n", tp->node1, tp->node2, &tp->cost) != EOF){
        tp->id1 = topology_getNodeIDfromname(tp->node1);
        tp->id2 = topology_getNodeIDfromname(tp->node2);
        assert(tp->id1 >= 0 && tp->id1 <= 255 && tp->id2 >= 0 && tp->id2 <= 255);
        tp++;
        table_num++;
    }
    fclose(fp);
}

//这个函数返回指定主机的节点ID.
//节点ID是节点IP地址最后8位表示的整数.
//例如, 一个节点的IP地址为202.119.32.12, 它的节点ID就是12.
//如果不能获取节点ID, 返回-1.
int topology_getNodeIDfromname(char* hostname)
{
    struct hostent *hptr;
    if ((hptr = gethostbyname(hostname)) == NULL){
        return -1;
    }

    return (int)(((unsigned char *)*hptr->h_addr_list)[3]);
}

//这个函数返回指定的IP地址的节点ID.
//如果不能获取节点ID, 返回-1.
int topology_getNodeIDfromip(struct in_addr* addr)
{
    return (addr->s_addr >> 24) & 0xff;
}

//这个函数返回本机的节点ID
//如果不能获取本机的节点ID, 返回-1.
int topology_getMyNodeID()
{
    if(host_id==-1){
        char host[20];
        if (gethostname(host, 20) == -1){
            int tmp = errno;
            printf("%s\n", strerror(tmp));
            return -1;
        }
        host_id = topology_getNodeIDfromname(host);
        return host_id;
    } else {
        return host_id;
    }
}

//这个函数解析保存在文件topology.dat中的拓扑信息.
//返回邻居数.
int topology_getNbrNum()
{
    if(isRead == -1){
        topology_read();
    }
    int my_node_id = topology_getMyNodeID();
    int nbrnum = 0;
    for (int i = 0; i < table_num; i++){
        if (topology_table[i].id1 == my_node_id|| topology_table[i].id2 == my_node_id){
            nbrnum++;
        }
    }
    return nbrnum;
}

//这个函数解析保存在文件topology.dat中的拓扑信息.
//返回重叠网络中的总节点数.
int topology_getNodeNum() {
    if (isRead == -1) {
        topology_read();
    }
    int map[256];
    int nodesum = 0;
    memset((void *) map, 0, sizeof(map));

    for (int i = 0; i < table_num; ++i) {
        struct topology_t t = topology_table[i];
        if (map[t.id1] == 0) {
            map[t.id1] = 1;
            nodesum++;
        }
        if (map[t.id2] == 0) {
            map[t.id1] = 1;
            nodesum++;
        }
    }
    return nodesum;

}

//这个函数解析保存在文件topology.dat中的拓扑信息.
//返回一个动态分配的数组, 它包含重叠网络中所有节点的ID. 
int* topology_getNodeArray()
{

    int nodenum = topology_getNodeNum();
    int* arr = (int*) malloc(sizeof (int)*nodenum);
    int map[256];
    int index = 0;
    memset((void *) map, 0, sizeof(map));
    for (int i = 0; i < table_num; ++i) {
        struct topology_t t = topology_table[i];
        if (map[t.id1] == 0) {
            map[t.id1] = 1;
            arr[index] = t.id1;
            index++;
        }
        if (map[t.id2] == 0) {
            map[t.id1] = 1;
            arr[index] = t.id2;
            index++;
        }
    }
    return arr;
}

//这个函数解析保存在文件topology.dat中的拓扑信息.
//返回一个动态分配的数组, 它包含所有邻居的节点ID.
int*  topology_getNbrArray()
{
    int my_node_id = topology_getMyNodeID();
    int nbrnum = topology_getNbrNum();
    int* arr = (int*) malloc(sizeof (int)*nbrnum);
    int index = 0;
    for (int i = 0; i < table_num; ++i) {
        if(topology_table[i].id1 == my_node_id){
            arr[index] = topology_table[i].id2;
            index++;
        }
        if(topology_table[i].id2 == my_node_id){
            arr[index] = topology_table[i].id1;
            index++;
        }
    }
    return arr;
}

//这个函数解析保存在文件topology.dat中的拓扑信息.
//返回指定两个节点之间的直接链路代价. 
//如果指定两个节点之间没有直接链路, 返回INFINITE_COST.
unsigned int topology_getCost(int fromNodeID, int toNodeID)
{
    if(isRead==-1){
        topology_read();
    }
    if(fromNodeID == toNodeID){
        return  0;
    }
    for (int i = 0; i < table_num; ++i) {
        if(fromNodeID == topology_table[i].id1){
            if(toNodeID == topology_table[i].id2){
                return topology_table[i].cost;
            }
        }else if(fromNodeID == topology_table[i].id2){
            if(toNodeID == topology_table[i].id1){
                return topology_table[i].cost;
            }
        }
    }
    return INFINITE_COST;
}
