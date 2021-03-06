/*
 * Copyright (c) 2016, All rights reserved.
 * See LICENSE.txt for full details. 
 * 
 *  Created:   1 Apr 2016
 *  File name: etcpConn.c
 *  Description:
 *  <INSERT DESCRIPTION HERE> 
 */


#include <stddef.h>
#include <stdlib.h>

#include "utils.h"
#include "debug.h"
#include "etcpConn.h"


void etcpConnDelete(etcpConn_t* const conn)
{
    if_unlikely(!conn){ return; }

    if_likely(conn->txQ != NULL){ cqDelete(conn->txQ); }
    if_likely(conn->rxQ != NULL){ cqDelete(conn->rxQ); }
    if_likely(conn->staleQ != NULL){ llDelete(conn->staleQ); }

    free(conn);

}


etcpConn_t* etcpConnNew(etcpState_t* const state, const i64 windowSizeLog2, const i32 buffSize, const uint32_t srcAddr, const uint32_t srcPort, const uint64_t dstAddr, const uint32_t dstPort, const i64 vlan, const i64 priority)
{
    etcpConn_t* conn = calloc(1, sizeof(etcpConn_t));
    if_unlikely(!conn){ return NULL; }

    conn->rxQ = cqNew(buffSize,windowSizeLog2);
    if_unlikely(conn->rxQ == NULL){
        etcpConnDelete(conn);
        return NULL;
    }

    conn->txQ = cqNew(buffSize,windowSizeLog2);
    if_unlikely(conn->txQ == NULL){
        etcpConnDelete(conn);
        return NULL;
    }

    conn->staleQ = llNew(buffSize);
    if_unlikely(conn->staleQ == NULL){
        etcpConnDelete(conn);
        return NULL;
    }


    conn->flowId.srcAddr = srcAddr;
    conn->flowId.srcPort = srcPort;
    conn->flowId.dstAddr = dstAddr;
    conn->flowId.dstPort = dstPort;

    conn->state = state;

    conn->vlan     = vlan;
    conn->priority = priority;

    return conn;
}
