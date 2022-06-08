
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <string.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include "nbrcosttable.h"
#include "../common/constants.h"
#include "../topology/topology.h"

//这个函数动态创建邻居代价表并使用邻居节点ID和直接链路代价初始化该表.
//邻居的节点ID和直接链路代价提取自文件topology.dat.
nbr_cost_entry_t* nbrcosttable_create()
{
    int myNodeId = topology_getMyNodeID();
    nbr_cost_entry_t* table = (nbr_cost_entry_t *) malloc(topology_getNbrNum() * sizeof(nbr_cost_entry_t));
    int* nbrArray = topology_getNbrArray();
    for(int i = 0;i < topology_getNbrNum();i++) {
        table[i].nodeID = nbrArray[i];
        table[i].cost = topology_getCost(myNodeId, nbrArray[i]);
    }
    return table;
}

//这个函数删除邻居代价表.
//它释放所有用于邻居代价表的动态分配内存.
void nbrcosttable_destroy(nbr_cost_entry_t* nct)
{
    free(nct);
}

//这个函数用于获取邻居的直接链路代价.
//如果邻居节点在表中发现,就返回直接链路代价.否则返回INFINITE_COST.
unsigned int nbrcosttable_getcost(nbr_cost_entry_t* nct, int nodeID)
{
    for(int i = 0;i < topology_getNbrNum();i++){
        if(nct[i].nodeID == nodeID){
            return nct[i].cost;
        }
    }
    return INFINITE_COST;
}

//这个函数打印邻居代价表的内容.
void nbrcosttable_print(nbr_cost_entry_t* nct)
{
    for(int i = 0;i < topology_getNbrNum();i++){
        printf("邻居%d : nodeId = %d, cost = %d\n",i+1,nct[i].nodeID,nct[i].cost);
    }
}
