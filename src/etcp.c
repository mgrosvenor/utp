/*
 * Copyright (c) 2016, All rights reserved.
 * See LICENSE.txt for full details. 
 * 
 *  Created:   30 Mar 2016
 *  File name: tecp.c
 *  Description:
 *  <INSERT DESCRIPTION HERE> 
 */

#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <pthread.h>
#include <assert.h>

#include <time.h>

#include <sys/socket.h>
#include <linux/if_ether.h>
#include <arpa/inet.h>

#include "CircularQueue.h"
#include "types.h"
#include "spooky_hash.h"
#include "debug.h"

#include "packets.h"
#include "utils.h"
#include "etcp.h"

#include "etcpState.h"
#include "etcpSockApi.h"


static inline etcpError_t etcpOnRxDat(etcpState_t* const state, pBuff_t* const pbuff, const etcpFlowId_t* const flowId)
{
    //DBG("Working on new data message with type = 0x%016x\n", head->type);

    const i64 msgSpace = pbuff->msgSize - pbuff->encapHdrSize - pbuff->etcpHdrSize;
    const i64 minSizeDatHdr = sizeof(etcpMsgDatHdr_t);
    if_unlikely(pbuff->msgSize - pbuff->encapHdrSize - pbuff->etcpHdrSize  < minSizeDatHdr){
        WARN("Not enough bytes to parse data header, required %li but got %li\n", minSizeDatHdr, msgSpace);
        return etcpEBADPKT; //Bad packet, not enough data in it
    }
    etcpMsgDatHdr_t* const datHdr = (etcpMsgDatHdr_t* const)(pbuff->etcpDatHdr + 1);
    //DBG("Working on new data message with seq = 0x%016x\n", datHdr->seqNum);

    //Got a valid data header, more sanity checking
    const uint64_t datLen = msgSpace - minSizeDatHdr;
    if_unlikely(datLen != datHdr->datLen){
        WARN("Data length has unexpected value. Expected %li, but got %li\n",datLen, datHdr->datLen);
        return etcpEBADPKT;
    }
    //DBG("Working on new data message with len = %li\n", datHdr->datLen);

    //Find the source map for this packet
    const htKey_t dstKey = {.keyHi = flowId->dstAddr, .keyLo = flowId->dstPort };
    etcpLAMap_t* srcsMap = NULL;
    htError_t htErr = htGet(state->dstMap,&dstKey,(void**)&srcsMap);
    if_unlikely(htErr == htENOTFOUND){
        WARN("Packet unexpected. No one listening to Add=%li, Port=%li\n", flowId->dstAddr, flowId->dstPort);
        return etcpEREJCONN;
    }
    else if_unlikely(htErr != htENOEROR){
        ERR("Unexpected hash table error: %s\n", htError2Str(htErr));
        return etcpEHTERR;
    }

    //Someone is listening to this destination, but is this the connection already established? Try to get the connection
    etcpConn_t* recvConn = NULL;
    etcpConn_t* sendConn = NULL;
    const htKey_t srcKey = { .keyHi = flowId->srcAddr, .keyLo = flowId->srcPort };
    htErr = htGet(srcsMap->table,&srcKey,(void**)&recvConn);
    if(htErr == htENOTFOUND){
        etcpError_t err = addNewConn(state, srcsMap, flowId, datHdr->noRet, &recvConn, &sendConn);
        if_unlikely(err != etcpENOERR){
            ERR("Error trying to add new connection\n");
            return err;
        }
    }

    //By this point, the connection structure should be properly populated one way or antoher
    const i64 seqPkt        = datHdr->seqNum;
    const i64 seqMin        = recvConn->rxQ->rdMin; //The very minimum sequence number that we will consider
    const i64 seqMax        = recvConn->rxQ->wrMax; //One greater than the biggest seq we can handle
    DBG("Got dat packet seqPkt = %li, seqMin= %li, seqMax = %li\n",
        seqPkt,
        seqMin,
        seqMax
    );

    //When we receive a packet, it should be between seqMin and seqMax
    //-- if seq >= seqMax, it is beyond the end of the rx window, ignore it, the packet will be sent again
    //-- if seq < seqMin, it has already been ack'd, the ack must have got lost, send another ack
    if_unlikely(seqPkt >= seqMax){
        FAT("Ignoring packet, seqPkt %li >= %li seqMax, packet will not fit in window\n", seqPkt, seqMax);
        return etcpERANGE;
    }

    if_unlikely(seqPkt < seqMin){
        WARN("Stale packet, seqPkt %li < %li seqMin, packet has already been ack'd\n", seqPkt, seqMin);

        if_eqlikely(datHdr->noAck){
            //This packet does not want an ack, and it's stale, so just ignore it
            return etcpECQERR;;
        }

        //Packet does want an ack. We can't ignore these because the send side might be waiting for a lost ack, which will
        //hold up new TX's
        //Note: This is a slow-path, a ordered push to a LL O(n) + malloc() is more costly a push to the CQ O(c).

        const i64 toCopy = pbuff->buffSize + sizeof(pBuff_t); //Include the size of pbuff header so that we can copy the whole thing
        i64 toCopyTmp = toCopy;
        datHdr->staleDat = 1; //Mark the packet as stale so we don't try to deliver again to user
        llError_t err = llPushSeqOrd(recvConn->staleQ,pbuff,&toCopyTmp,seqPkt);
        if_unlikely(err == llETRUNC){
            WARN("Payload (%liB) is too big for slot (%liB), truncating\n", toCopy, toCopyTmp );
        }
        else if_unlikely(err != llENOERR){
            WARN("Error inserting into linked-list: %s", llError2Str(err));
            return etcpECQERR;
        }

        return etcpENOERR;

    }

    const i64 toCopy = pbuff->buffSize + sizeof(pBuff_t); //Include the size of pbuff header so that we can copy the whole thing
    i64 toCopyTmp = toCopy;
    cqError_t err = cqPush(recvConn->rxQ,pbuff,&toCopyTmp,seqPkt);

    if_unlikely(err == cqENOSLOT){
        WARN("Unexpected state, packet not enough space in queue\n");
        return etcpETRYAGAIN; //We've run out of slots to put packets into, drop the packet. But can this actually happen?
    }
    else if_unlikely(err == cqETRUNC){
        WARN("Payload (%liB) is too big for slot (%liB), truncating\n", toCopy, toCopyTmp );
    }
    else if_unlikely(err != cqENOERR){
        WARN("Error inserting into Circular Queue: %s", cqError2Str(err));
        return etcpECQERR;
    }

    cqCommitSlot(recvConn->rxQ,seqPkt,toCopyTmp);

    return etcpENOERR;
}

static inline  etcpError_t etcpProcessAck(cq_t* const cq, const uint64_t seq, const etcpTime_t* const ackTime, const etcpTime_t* const datFirstTime, const etcpTime_t* const datLastTime)
{
    //DBG("Processing ack for seq=%li\n", seq);
    if(seq < (uint64_t)cq->rdMin){
        WARN("Stale ack, this ack has already been accepted\n");
        return etcpENOERR;
    }

    cqSlot_t* slot = NULL;
    const cqError_t err = cqGetRd(cq,&slot,seq);
    if_unlikely(err != cqENOERR){
        WARN("Error getting value from Circular Queue: %s", cqError2Str(err));
        return etcpECQERR;
    }

    const pBuff_t* pbuff = slot->buff;
    const etcpMsgHead_t* const head = pbuff->etcpHdr;
    const etcpMsgDatHdr_t* const datHdr = pbuff->etcpDatHdr;
    if_unlikely(seq != datHdr->seqNum){
        WARN("Got an ACK for a packet that's gone.\n");
        return etcpENOERR;
    }
    //Successful ack! -- Do timing stats here
    DBG("Successful ack for seq %li\n", seq);

    //TODO XXX can do stats here.
    const i64 totalRttTime     = ackTime->swRxTimeNs - head->ts.swTxTimeNs; //Total round trip for the sack vs dat
//    const i64 remoteProcessing = ackTime->swTxTimeNs - datFirstTime->swRxTimeNs; //Time between the first data packet RX and the ack TX
//    //Not supported without NIC help, assume this is constant on both sides
//    //const i64 remoteHwTime     = ackTime->hwTxTimeNs - ackTime->hwRxTimeNs; //Time in hardware on the remote side
//    const i64 localHwTxTime    = head->ts.hwTxTimeNs - head->ts.swTxTimeNs; //Time in TX hardware on the local side
//    const i64 localHwRxTime    = ackTime->swRxTimeNs - ackTime->hwRxTimeNs; //Time in RX hardware on the local side
//    const i64 localHwTime      = localHwTxTime + localHwRxTime;
//    const i64 remoteHwTime     = localHwTime;
//    const i64 networkTime      = totalRttTime - remoteHwTime - remoteProcessing - localHwTime;
    (void)datLastTime;//Not needed right now
    (void)datFirstTime;
//    DBG("TIMING STATS:\n");
//    DBG("-------------------------------------\n");
    DBG("Total RTT:           %lins (%lius, %lims, %lis)\n", totalRttTime, totalRttTime / 1000, totalRttTime / 1000/1000, totalRttTime / 1000/1000/1000);
//    DBG("Remote Processing:   %lins (%lius, %lims, %lis)\n", remoteProcessing, remoteProcessing/ 1000, remoteProcessing / 1000/1000, remoteProcessing / 1000/1000/1000);
//    DBG("Local HW TX:         %lins (%lius, %lims, %lis)\n", localHwTxTime, localHwTxTime / 1000, localHwTxTime / 1000/1000, localHwTxTime / 1000/1000/1000 );
//    DBG("Local HW RX:         %lins (%lius, %lims, %lis)\n", localHwRxTime, localHwRxTime / 1000, localHwRxTime / 1000/1000, localHwRxTime / 1000/1000/1000 );
//    DBG("Local HW:            %lins (%lius, %lims, %lis)\n", localHwTime, localHwTime/1000, localHwTime / 1000/1000, localHwTime / 1000/1000/1000);
//    DBG("Remote HW (guess)    %lins (%lius, %lims, %lis)\n", remoteHwTime, remoteHwTime/1000, remoteHwTime/ 1000/1000, remoteHwTime / 1000/1000/1000);
//    DBG("Network time:        %lins (%lius, %lims, %lis)\n", networkTime, networkTime/1000, networkTime/1000/1000, networkTime / 1000/1000/1000);
//    DBG("-------------------------------------\n");

    //Packet is now ack'd, we can release this slot and use it for another TX
    const cqError_t cqErr = cqReleaseSlot(cq,seq);
    if_unlikely(cqErr != cqENOERR){
        ERR("Unexpected cq error: %s\n", cqError2Str(cqErr));
        return etcpECQERR;
    }
    return etcpENOERR;
}



static inline  etcpError_t etcpOnRxAck(etcpState_t* const state, pBuff_t* const pbuff, const etcpFlowId_t* const flowId)
{
    //DBG("Working on new ack message\n");

    const i64 minSizeSackHdr = sizeof(etcpMsgSackHdr_t);
    const i64 msgSpace = pbuff->msgSize - pbuff->encapHdrSize - pbuff->etcpDatHdrSize;
    if_unlikely(msgSpace < minSizeSackHdr){
        WARN("Not enough bytes to parse sack header, required %li but got %li\n", minSizeSackHdr, msgSpace);
        return etcpEBADPKT; //Bad packet, not enough data in it
    }

    //Got a valid sack header, more sanity checking
    etcpMsgSackHdr_t* const sackHdr = (etcpMsgSackHdr_t* const)(pbuff->etcpHdr + 1);
    const i64 sackLen       = msgSpace - minSizeSackHdr;
    pbuff->etcpSackHdr      = sackHdr;
    pbuff->etcpSackHdrSize  = sackLen;
    if_unlikely(sackLen != sackHdr->sackCount * sizeof(etcpSackField_t)){
        WARN("Sack length has unexpected value. Expected %li, but got %li\n",sackLen, sackHdr->sackCount * sizeof(etcpSackField_t));
        return etcpEBADPKT;
    }
    etcpSackField_t* const sackFields = (etcpSackField_t* const)(sackHdr + 1);


    //Find the source map for this packet
    const htKey_t dstKey = {.keyHi = flowId->srcAddr, .keyLo = flowId->srcPort }; //Since this is an ack, we swap src / dst
    etcpLAMap_t* srcsMap = NULL;
    htError_t htErr = htGet(state->dstMap,&dstKey,(void**)&srcsMap);
    if_unlikely(htErr == htENOTFOUND){
        ERR("Ack unexpected. No one listening to destination addr=%li, port=%li\n", flowId->srcAddr, flowId->srcPort);
        return etcpEREJCONN;
    }
    else if_unlikely(htErr != htENOEROR){
        ERR("Hash table error: %s\n", htError2Str(htErr));
        return etcpEHTERR;
    }

    //Someone is listening to this destination, but is there also someone listening to the source?
    etcpConn_t* conn = NULL;
    const htKey_t srcKey = { .keyHi = flowId->dstAddr, .keyLo = flowId->dstPort }; //Flip these for an ack packet
    htErr = htGet(srcsMap->table,&srcKey,(void**)&conn);
    if_unlikely(htErr == htENOTFOUND){
        ERR("Ack unexpected. No one listening to source addr=%li, port=%li\n", flowId->dstAddr, flowId->dstPort);
        return etcpEREJCONN;
    }
    else if_unlikely(htErr != htENOEROR){
        ERR("Hash table error: %s\n", htError2Str(htErr));
        return etcpEHTERR;
    }

    //By now we have located the connection structure for this ack packet
    //Try to put the sack into the AckRxQ so that the Transmission Control function can use it as an input
    i64 slotIdx = -1;
    i64 len    = pbuff->buffSize + sizeof(pBuff_t);
    i64 lenTmp = sackLen;
    cqPushNext(conn->rxQ,sackHdr, &lenTmp,&slotIdx); //No error checking it's ok if this fails.
    if(len < sackLen){
        WARN("Truncated SACK packet into ackRxQ\n");
    }
    cqCommitSlot(conn->rxQ,slotIdx,len);


    //Process the acks and apply to TX packets waiting.
    const uint64_t sackBaseSeq = sackHdr->sackBaseSeq;
    for(i64 sackIdx = 0; sackIdx < sackHdr->sackCount; sackIdx++){
        const uint64_t ackOffset = sackFields[sackIdx].offset;
        const uint64_t ackCount  = sackFields[sackIdx].count;
        DBG("Working on %li ACKs starting at %li \n", ackCount, sackBaseSeq + ackOffset);
        for(uint16_t ackIdx = 0; ackIdx < ackCount; ackIdx++){
            const uint64_t ackSeq = sackBaseSeq + ackOffset + ackIdx;
            etcpProcessAck(conn->txQ,ackSeq,&pbuff->etcpHdr->ts, &sackHdr->timeFirst, &sackHdr->timeLast);
        }
    }

    return etcpENOERR;
}




static inline  etcpError_t etcpOnRxPacket(etcpState_t* const state, pBuff_t* const pbuff, i64 srcAddr, i64 dstAddr, i64 hwRxTimeNs)
{
    //First sanity check the packet
    const i64 minSizeHdr = sizeof(etcpMsgHead_t);
    if_unlikely(pbuff->msgSize - pbuff->encapHdrSize < minSizeHdr){
        WARN("Not enough bytes to parse ETCP header\n");
        return etcpEBADPKT; //Bad packet, not enough data in it
    }
    etcpMsgHead_t* const head = pbuff->etcpHdr;

    //Get a timestamp as soon as we know we have a place to put it.
    struct timespec ts = {0};
    clock_gettime(CLOCK_REALTIME,&ts);
    head->ts.hwRxTimeNs = hwRxTimeNs;
    head->ts.swRxTimeNs  = ts.tv_sec * 1000 * 1000 * 1000 + ts.tv_nsec;
    head->hwRxTs = 1;
    head->swRxTs = 1;

    //Do this in a common place since everyone needs it
    const etcpFlowId_t flowId = {
        .srcAddr = srcAddr,
        .dstAddr = dstAddr,
        .srcPort = head->srcPort,
        .dstPort = head->dstPort,
    };

    //Now we can process the message
    switch(head->fulltype){
//        case ETCP_V1_FULLHEAD(ETCP_FIN): //XXX TODO, currently only the send side can disconnect...
        case ETCP_V1_FULLHEAD(ETCP_DAT):
            return etcpOnRxDat(state, pbuff, &flowId);

        case ETCP_V1_FULLHEAD(ETCP_ACK):
            return etcpOnRxAck(state, pbuff, &flowId);

        default:
            WARN("Bad header, unrecognised type msg_magic=%li (should be %li), version=%i (should be=%i), type=%li\n",
                    head->magic, ETCP_MAGIC, head->ver, ETCP_V1, head->type);
            return etcpEBADPKT; //Bad packet, not enough data in it
    }
    return etcpENOERR;

}

typedef struct __attribute__((packed)){
    uint16_t pcp: 3;
    uint16_t dei: 1;
    uint16_t vid: 12;
} eth8021qTCI_t;

#define ETH_P_ECTP 0x8888


//The expected transport for ETCP is Ethernet, but really it doesn't care. PCIE/IPV4/6 could work as well.
//This function codes the assumes an Ethernet frame, supplied with a frame check sequence to the ETCP processor.
//It expects that an out-of-band hardware timestamp is also passed in.
static inline  etcpError_t etcpOnRxEthernetFrame(etcpState_t* const state, pBuff_t* const pbuff, i64 hwRxTimeNs)
{
    const i64 minSizeEHdr = ETH_HLEN + ETH_FCS_LEN;
       if_unlikely(pbuff->msgSize < minSizeEHdr){
           WARN("Not enough bytes to parse Ethernet header, expected at least %li but got %li\n", minSizeEHdr, pbuff->msgSize);
           return etcpEBADPKT; //Bad packet, not enough data in it
       }
       struct ethhdr* const eHead = (struct ethhdr* const) pbuff->buffer ;
       pbuff->encapHdr = eHead;

       uint64_t dstAddr = 0;
       memcpy(&dstAddr, eHead->h_dest, ETH_ALEN);
       uint64_t srcAddr = 0;
       memcpy(&srcAddr, eHead->h_source, ETH_ALEN);
       uint16_t proto = ntohs(eHead->h_proto);

       pbuff->etcpHdr = (void*)(eHead + 1);
       pbuff->etcpHdrSize = sizeof(etcpMsgHead_t);
       i64 etcpPacketLen      = pbuff->msgSize - ETH_HLEN - ETH_FCS_LEN;

       if_likely(proto == ETH_P_ECTP ){
           pbuff->encapHdrSize = ETH_HLEN + ETH_FCS_LEN;
           return etcpOnRxPacket(state,pbuff,srcAddr,dstAddr, hwRxTimeNs);
       }

       //This is a VLAN tagged packet we can handle these too
       if_likely(proto == ETH_P_8021Q){
           pbuff->etcpHdr      = (void*)((uint8_t*)pbuff->etcpHdr + sizeof(eth8021qTCI_t));
           etcpPacketLen       = pbuff->msgSize - sizeof(eth8021qTCI_t);
           pbuff->encapHdrSize = ETH_HLEN + ETH_FCS_LEN + sizeof(eth8021qTCI_t);
           return etcpOnRxPacket(state, pbuff,srcAddr,dstAddr, hwRxTimeNs);
       }

       WARN("Unknown EtherType 0x%04x\n", proto);

       return etcpEBADPKT;

}


static inline etcpError_t etcpMkEthPkt(void* const buff, i64* const len_io, const uint64_t srcAddr, const uint64_t dstAddr, const i16 vlan, const uint8_t priority )
{
//    DBG("Making ethernet packet with source address=0x%016lx, dstAddr=0x%016lx, vlan=%i, priority=%i\n",
//            srcAddr,
//            dstAddr,
//            vlan,
//            priority);

    const i64 len = *len_io;
    if_unlikely(len < ETH_ZLEN){
        ERR("Not enough bytes in Ethernet frame. Required %li but got %i\n", ETH_ZLEN, len );
        return etcpEFATAL;
    }

    struct ethhdr* const ethHdr = buff;

    memcpy(&ethHdr->h_dest, &dstAddr, ETH_ALEN);
    memcpy(&ethHdr->h_source, &srcAddr, ETH_ALEN);
    ethHdr->h_proto = htons(ETH_P_ECTP);

    if_eqlikely(vlan < 0){
        //No VLAN tag header, we're done!
        *len_io = ETH_HLEN;
        return etcpENOERR;
    }

    eth8021qTCI_t* vlanHdr = (eth8021qTCI_t*)(ethHdr + 1);
    vlanHdr->dei= 0;
    vlanHdr->vid = vlan;
    vlanHdr->pcp = priority;

    *len_io = ETH_HLEN + sizeof(eth8021qTCI_t);
    return etcpENOERR;


}

//Put a sack packet into a buffer for transmit
static inline etcpError_t pushSackEthPacket(etcpConn_t* const conn, const i8* const sackHdrAndData, const i64 sackCount)
{
    cqSlot_t* slot = NULL;
    i64 seqNum;
    cqError_t cqErr = cqGetNextWr(conn->txQ, &slot,&seqNum);
    if_unlikely(cqErr == cqENOSLOT){
        WARN("Ran out of ACK queue slots\n");
        return etcpETRYAGAIN;
    }
    else if_unlikely(cqErr != cqENOERR){
        ERR("Error on circular queue: %s", cqError2Str(cqErr));
        return etcpECQERR;
    }

    //We got a slot, now check it's big enough then format a packet into it
    pBuff_t * pBuff = slot->buff;
    pBuff->buffer = (i8*)slot->buff + sizeof(pBuff_t);
    pBuff->buffSize = slot->len - sizeof(pBuff_t);

    i8* buff = pBuff->buffer;
    i64 buffLen = pBuff->buffSize;
    const i64 sackHdrAndDatSize = sizeof(etcpMsgSackHdr_t) + sizeof(etcpSackField_t) * sackCount;
    const i64 ethOverhead       = conn->vlan < 0 ? ETH_HLEN : ETH_HLEN + 2; //Include vlan space if needed!
    const i64 ethEtcpSackPktSize = ethOverhead + sizeof(etcpMsgHead_t) + sackHdrAndDatSize;
    if_unlikely(buffLen < ethEtcpSackPktSize){
        ERR("Slot length is too small for sack packet need %li but only got %li!",ethEtcpSackPktSize, buffLen );
    }

    //NB: Reverse the srcAddr and dstAddr so that the packet goes back to where it came
    i64 ethLen = buffLen;
    etcpError_t etcpErr = etcpMkEthPkt(buff,&ethLen, conn->flowId.dstAddr, conn->flowId.srcAddr, conn->vlan, conn->priority);
    if_unlikely(etcpErr != etcpENOERR){
        WARN("Could not format Ethernet packet\n");
        return etcpErr;
    }
    pBuff->encapHdr = buff;
    pBuff->encapHdrSize = ethLen;
    buff += ethLen;
    pBuff->msgSize = pBuff->encapHdrSize;

    etcpMsgHead_t* const head = (etcpMsgHead_t* const)buff;
    pBuff->etcpHdr      = head;
    pBuff->etcpHdrSize  = sizeof(etcpMsgHead_t);
    pBuff->msgSize     += pBuff->etcpHdrSize;
    head->fulltype      = ETCP_V1_FULLHEAD(ETCP_ACK);
    //Reverse the source and destination ports so that the packet goest back to where it came from
    head->srcPort       = conn->flowId.dstPort;
    head->dstPort       = conn->flowId.srcPort;
    buff                += sizeof(etcpMsgHead_t);

    pBuff->etcpSackHdr      = (etcpMsgSackHdr_t*)buff;
    pBuff->etcpSackHdrSize  = sizeof(etcpMsgSackHdr_t);
    pBuff->msgSize         += pBuff->etcpSackHdrSize;

    pBuff->etcpPayload  = buff + sizeof(etcpMsgSackHdr_t);
    pBuff->etcpPayloadSize = sizeof(etcpSackField_t) * sackCount;
    pBuff->msgSize         += pBuff->etcpPayloadSize;

    memcpy(buff,sackHdrAndData,sackHdrAndDatSize);

    cqErr = cqCommitSlot(conn->txQ,seqNum,ethEtcpSackPktSize);
    if_unlikely(cqErr != cqENOERR){
        ERR("Unexpected error on CQ: %s\n", cqError2Str(cqErr));
        return etcpECQERR;
    }
    assert(pBuff->msgSize == ethEtcpSackPktSize);

    return etcpENOERR;
}



//Traverse the receive queues and generate ack's
etcpError_t generateStaleAcks(etcpConn_t* const conn, const i64 maxAckPackets, const i64 maxSlots)
{
    if_eqlikely(maxAckPackets <= 0){
        return etcpENOERR; //Don't bother trying if you don't want me to!
    }

    if_unlikely(conn->staleQ->slotCount == 0){
        return etcpENOERR; //There's nothing to do here
    }

    //DBG("Generating at most %li stale sack packets from %li slots\n", maxAckPackets, maxSlots);

    i64 fieldIdx          = 0;
    bool fieldInProgress  = false;
    i64  expectSeqNum       = 0;
    i64 unsentAcks        = 0;
    const i64 tmpBuffSize = sizeof(etcpMsgSackHdr_t) + sizeof(etcpSackField_t) * ETCP_MAX_SACKS;
    assert(sizeof(tmpBuffSize) <=  256);
    i8 tmpBuff[tmpBuffSize];
    memset(tmpBuff,0,tmpBuffSize);

    etcpMsgSackHdr_t* const sackHdr  = (etcpMsgSackHdr_t* const)(tmpBuff + 0 );
    etcpSackField_t* const sackFields = ( etcpSackField_t* const)(tmpBuff + sizeof(etcpMsgSackHdr_t));

    //Iterate through the rx packets to build up sack ranges
    i64 completeAckPackets = 0;
    for(i64 i = 0; i < maxSlots; i++){

        //We collected enough sack fields to make a whole packet and send it
        if_unlikely(fieldIdx >= ETCP_MAX_SACKS){
            //sackHdr->rxWindowSegs = 0;
            etcpError_t err = pushSackEthPacket(conn,tmpBuff,ETCP_MAX_SACKS);
            if_unlikely(err == etcpETRYAGAIN){
                WARN("Ran out of slots for sending acks, come back again\n");
                return err;
            }
            else if_unlikely(err != etcpENOERR){
                ERR("Unexpected error making ack packet\n");
                return err;
            }

            //Reset the sackStructure to make a new one
            memset(tmpBuff,0,tmpBuffSize);
            fieldIdx        = 0;
            fieldInProgress = false;
            unsentAcks      = 0;

            if(completeAckPackets >= maxAckPackets){
                return etcpENOERR;
            }
        }


        llSlot_t* slot = NULL;
        const llError_t err = llGetFirst(conn->staleQ,&slot);
        if_unlikely(err != llENOERR){
            break;
        }
        
        //The next sequence we get should be either equalt to the expected sequence
        if_unlikely(i == 0){
            expectSeqNum = slot->seqNum;
        }
        const i64 seqNum = slot->seqNum;

        //DBG("Now looking at seq %i, expected=%li\n", seqNum, expectSeqNum);

        if_unlikely(seqNum < expectSeqNum){
            if(seqNum == expectSeqNum -1){
                WARN("Duplicate entries for sequence number %li in stale queue, igoring this one\n", seqNum);
                llReleaseHead(conn->staleQ); //We're done with this packet, throw away
                continue;

            }

            FAT("This should not happen, sequence numbers have gone backwards from %li to %li. This means there's been an ordering violation in the stale queue\n", seqNum, expectSeqNum);
            return etcpEFATAL;
        }

        //There is a break in the sequence number series. Start/finish a sack field
        if_eqlikely(seqNum > expectSeqNum){
            //The slot is empty --
            if_likely(fieldInProgress){
                fieldIdx++;
                fieldInProgress = false;
                expectSeqNum = seqNum; //Jump the expected value up to the new value
            }
        }

        //At this point we have a valid packet
        const pBuff_t* const pbuff          = slot->buff;
        const etcpMsgHead_t* const head     = pbuff->etcpHdr;
        etcpMsgDatHdr_t* const datHdr       = pbuff->etcpDatHdr;

        //Start a new field
        if_unlikely(!fieldInProgress){
            if_unlikely(fieldIdx == 0){
                sackHdr->timeFirst = head->ts;
                sackHdr->sackBaseSeq = seqNum;
                DBG("Staring new stale sack packet with sack base = %li\n", sackHdr->sackBaseSeq);
            }

            sackFields[fieldIdx].offset = datHdr->seqNum - sackHdr->sackBaseSeq;
            sackFields[fieldIdx].count = 0;
            fieldInProgress = true;
            DBG("Starting new sack field indx=%li, offset = %li\n", fieldIdx, sackFields[fieldIdx].offset);
        }
        unsentAcks++;
        sackFields[fieldIdx].count++;
        sackHdr->timeLast = head->ts;
        datHdr->ackSent = 1;
        expectSeqNum++;
        DBG("Made stale sack for seq=%li in field %li, offset=%i, count=%i, off + count=%i\n", i,fieldIdx, sackFields[fieldIdx].offset, sackFields[fieldIdx].count, sackFields[fieldIdx].offset + sackFields[fieldIdx].count );
    }

    //Push the last sack out
    if(unsentAcks > 0){
        sackHdr->sackCount = fieldIdx+1;
        etcpError_t err = pushSackEthPacket(conn,tmpBuff,fieldIdx+1);
        if_unlikely(err == etcpETRYAGAIN){
            WARN("Ran out of slots for sending acks, come back again\n");
            return err;
        }
        else if_unlikely(err != etcpENOERR){
            ERR("Unexpected error making ack packet\n");
            return err;
        }
    }
    return etcpENOERR;

}



//Traverse the receive queues and generate ack's
etcpError_t generateAcks(etcpConn_t* const conn, const i64 maxAckPackets, const i64 maxSlots)
{
    if_eqlikely(maxAckPackets <= 0){
        return etcpENOERR; //Don't bother trying if you don't want me to!
    }


    if_unlikely(conn->rxQ->readable == 0){
        return etcpENOERR; //There's nothing to do here
    }

    i64 fieldIdx          = 0;
    bool fieldInProgress  = false;
    i64 unsentAcks        = 0;
    const i64 tmpBuffSize = sizeof(etcpMsgSackHdr_t) + sizeof(etcpSackField_t) * ETCP_MAX_SACKS;
    assert(sizeof(tmpBuffSize) <=  256);
    i8 tmpBuff[tmpBuffSize];
    memset(tmpBuff,0,tmpBuffSize);

    etcpMsgSackHdr_t* const sackHdr  = (etcpMsgSackHdr_t* const)(tmpBuff + 0 );
    etcpSackField_t* const sackFields = ( etcpSackField_t* const)(tmpBuff + sizeof(etcpMsgSackHdr_t));

    //Iterate through the rx packets to build up sack ranges
    i64 completeAckPackets = 0;
    for(i64 i = conn->rxQ->rdMin; i < conn->rxQ->rdMax && i < conn->rxQ->rdMin + maxSlots; i++){

        //We collected enough sack fields to make a whole packet and send it
        if_unlikely(fieldIdx >= ETCP_MAX_SACKS){
            //sackHdr->rxWindowSegs = 0;
            DBG("Sending sack packet\n");
            etcpError_t err = pushSackEthPacket(conn,tmpBuff,ETCP_MAX_SACKS);
            if_unlikely(err == etcpETRYAGAIN){
                WARN("Ran out of slots for sending acks, come back again\n");
                return err;
            }
            else if_unlikely(err != etcpENOERR){
                ERR("Unexpected error making ack packet\n");
                return err;
            }

            //Account for the now sent messages by updating the seqAck value, only do so up to the edge of the first field, this
            //is the limit that will be receivable until new packets come.
            conn->seqAck = conn->seqAck + sackFields[0].offset + sackFields[0].count;

            //Reset the sackStructure to make a new one
            memset(tmpBuff,0,tmpBuffSize);
            fieldIdx        = 0;
            fieldInProgress = false;
            unsentAcks      = 0;

            if(completeAckPackets >= maxAckPackets){
                return etcpENOERR;
            }
        }


        //DBG("Now looking at seq %i\n", i);
        cqSlot_t* slot = NULL;
        const cqError_t err = cqGetRd(conn->rxQ,&slot,i);

        //This slot is empty, so stop making the field and start a new one
        if_eqlikely(err == cqEWRONGSLOT){
            //The slot is empty --
            if_likely(fieldInProgress){
                fieldIdx++;
                fieldInProgress = false;
            }
            continue;
        }
        else if_unlikely(err != cqENOERR){
            ERR("Error getting slot: %s\n", cqError2Str(err));
            return etcpECQERR;
        }

        //At this point we have a valid packet
        const pBuff_t* const pbuff          = slot->buff;
        const etcpMsgHead_t* const head     = pbuff->etcpHdr;
        etcpMsgDatHdr_t* const datHdr       = pbuff->etcpDatHdr;

        if_eqlikely(datHdr->ackSent){
            //This packet has been processed and ack'd, we're just waiting for it to be cleared from the window by a user RX
            continue;
        }

        if_eqlikely(datHdr->noAck){
            //This packet does not want an ack,
            if_likely(fieldInProgress){
                fieldIdx++;
                fieldInProgress = false;
            }
            datHdr->ackSent = 1; //Pretend to have sent it so it will get delivered to users
            continue;
        }

        //Start a new field
        if_unlikely(!fieldInProgress){
            if_unlikely(fieldIdx == 0){ //If we're starting a new sack packet, we need some extra fields
                sackHdr->timeFirst    = head->ts;
                sackHdr->sackBaseSeq  = conn->seqAck;
                DBG("Staring new sack packet with sack base = %li\n", sackHdr->sackBaseSeq);
            }


            sackFields[fieldIdx].offset = datHdr->seqNum - conn->seqAck;
            sackFields[fieldIdx].count = 0;
            fieldInProgress = true;
            DBG("Starting new sack field indx=%li, offset = %li\n", fieldIdx, sackFields[fieldIdx].offset);

        }
        unsentAcks++;
        sackFields[fieldIdx].count++;
        sackHdr->timeLast = head->ts;
        datHdr->ackSent = 1; //Mark the packet as ack-sent so it will get delivered to users
        DBG("Made sack for seq=%li in field %li, offset=%i, count=%i, off + count=%i\n", i,fieldIdx, sackFields[fieldIdx].offset, sackFields[fieldIdx].count, sackFields[fieldIdx].offset + sackFields[fieldIdx].count );
    }

    //Push the last sack out
    if(unsentAcks > 0){
        sackHdr->sackCount = fieldIdx+1;
        DBG("Sending sack packet\n");
        etcpError_t err = pushSackEthPacket(conn,tmpBuff,fieldIdx+1);
        if_unlikely(err == etcpETRYAGAIN){
            WARN("Ran out of slots for sending acks, come back again\n");
            return err;
        }
        else if_unlikely(err != etcpENOERR){
            ERR("Unexpected error making ack packet\n");
            return err;
        }

        //Account for the now sent messages by updating the seqAck value, only do so up to the edge of the first field, this
        //is the limit that will be receivable until new packets come.
        conn->seqAck = conn->seqAck + sackFields[0].offset + sackFields[0].count;
    }
    return etcpENOERR;

}


etcpError_t doEtcpNetTx(cq_t* const cq, const etcpState_t* const state, const i64 maxSlots )
{
    cqSlot_t* slot = NULL;


    for(i64 i = cq->rdMin; i < cq->rdMax && i < cq->rdMin + maxSlots; i++){
        const cqError_t err = cqGetRd(cq,&slot,i);
        if_eqlikely(err == cqEWRONGSLOT){
            //The slot is empty
            continue;
        }
        else if_unlikely(err != cqENOERR){
            ERR("Error getting slot: %s\n", cqError2Str(err));
            return etcpECQERR;
        }

        //DBG("Trying to send seq/slot %li\n", i);

        //We've now got a valid slot with a packet in it, grab it and see if the TC has decided it should be sent?
        pBuff_t* const pBuff = slot->buff;

        if_eqlikely(pBuff->txState == ETCP_TX_DRP ){
            //We're told to drop the packet. Release it and continue
            WARN("Dropping seq/slot %li\n", i);
            cqReleaseSlot(cq,i);
            continue;
        }
        else if_unlikely(pBuff->txState != ETCP_TX_NOW ){
            WARN("Ingnoring seq/slot %li waiting for ack\n", i);
            continue; //Not ready to send this packet now.
        }

        //before the packet is sent, make it ready to send again in the future just in case something goes wrong
        pBuff->txState = ETCP_TX_RDY;

        DBG("Sending seq/slot %li\n", i);

        switch(pBuff->etcpHdr->type){
            case ETCP_DAT:{
                if_likely(pBuff->etcpDatHdr->txAttempts == 0){
                    //Only put the timestamp in on the first time we send a data packet so we know how long it spends RTT incl
                    //in the txq.
                    //Would be nice to have an extra timestamp slot so that the local queueing time could be accounted for as well.
                    struct timespec ts = {0};
                    clock_gettime(CLOCK_REALTIME,&ts);
                    const i64 timeNowNs = ts.tv_sec * 1000 * 1000 * 1000 + ts.tv_nsec;
                    pBuff->etcpHdr->ts.swTxTimeNs = timeNowNs;
                    pBuff->etcpHdr->swTxTs        = 1;
                }
                break;
            }
            case ETCP_ACK:{
                struct timespec ts = {0};
                clock_gettime(CLOCK_REALTIME,&ts);
                const i64 timeNowNs = ts.tv_sec * 1000 * 1000 * 1000 + ts.tv_nsec;
                pBuff->etcpHdr->ts.swTxTimeNs = timeNowNs;
                pBuff->etcpHdr->swTxTs        = 1;
                break;
            }
            default:{
                ERR("Unkown packet type! %i\n",pBuff->etcpHdr->type );
                return etcpEFATAL;
            }
        }


        uint64_t hwTxTimeNs = 0;
        const pBuff_t* const pbuff = slot->buff;
        if_unlikely(state->ethHwTx(state->ethHwState, pBuff->buffer, pbuff->msgSize, &hwTxTimeNs) < 0){
            return etcpETRYAGAIN;
        }

        DBG("Sent packet %li\n", i);

        switch(pBuff->etcpHdr->type){
            case ETCP_DAT:{
                //The exanics do not yet support inline HW tx timestamping, but we can kind of fake it here
                //XXX HACK - not sure what a generic way to do this is?
                if_likely(pBuff->etcpDatHdr->txAttempts == 0){
                    pBuff->etcpHdr->ts.hwTxTimeNs = hwTxTimeNs;
                    pBuff->etcpHdr->hwTxTs        = 1;
                }
                pBuff->etcpDatHdr->txAttempts++; //Keep this around for next time.
                if_eqlikely(pBuff->etcpDatHdr->noAck){
                    //We're done with the packet, not expecting an ack, so drop it now
                    cqReleaseSlot(cq,i);
                }
                //Otherwise, we need to wait for the packet to be ack'd
                break;
            }
            case ETCP_ACK:{
                cqReleaseSlot(cq,i);
                DBG("Released packet %li\n", i);
                break;
            }
            default:{
                ERR("Unkown packet type! %i\n",pBuff->etcpHdr->type );
                return etcpEFATAL;
            }

        }

    }

    return etcpENOERR;
}






//Returns the number of packets received
i64 doEtcpNetRx(etcpState_t* state)
{

    i64 result = 0;
    i8 frameBuff[MAX_FRAME] = {0}; //This is crap, should have a frame pool, from which pointers can be taken and then
                                   //inserted into the CQ structure rather than copying. This would make the whole
                                   //stack 0/1-copy. The reason it's not done this way right now is beacuse

    pBuff_t* pbuff = (pBuff_t*)frameBuff;
    pbuff->buffer = pbuff + 1;
    pbuff->buffSize = MAX_FRAME - sizeof(pbuff);
    assert(pbuff->buffSize > 0);

    uint64_t hwRxTimeNs = 0;
    i64 rxLen = state->ethHwRx(state->ethHwState,pbuff->buffer,pbuff->buffSize, &hwRxTimeNs);
    pbuff->msgSize = rxLen;
    for(; rxLen > 0; rxLen = state->ethHwRx(state->ethHwState,pbuff->buffer,pbuff->buffSize,&hwRxTimeNs)){
        pbuff->msgSize = rxLen;
        etcpError_t err = etcpOnRxEthernetFrame(state, pbuff, hwRxTimeNs);
        result++;
        if_unlikely(err == etcpETRYAGAIN){
            WARN("Ring is full\n");
            break;
        }
    }

    if(rxLen < 0){
        WARN("Rx error %li\n", rxLen);
    }

    return result;

}


//This is a user facing function
etcpError_t doEtcpUserRx(etcpConn_t* const conn, void* __restrict data, i64* const len_io)
{

    //DBG("Doing user rx\n");
    while(1){
        i64 seqNum = -1;
        cqSlot_t* slot;
        cqError_t cqErr = cqGetNextRd(conn->rxQ,&slot,&seqNum);
        if_unlikely(cqErr == cqENOSLOT){
            //Nothing here. Give up
            return etcpETRYAGAIN;
        }
        else if_unlikely(cqErr != cqENOERR){
            ERR("Error on circular buffer: %s\n", cqError2Str(cqErr));
            return etcpECQERR;
        }
        //DBG("Got packet with sequence number %li\n",seqNum);


        const pBuff_t* pbuff = slot->buff;
        const etcpMsgDatHdr_t* const datHdr =  pbuff->etcpDatHdr;

        //We have a valid packet, but it might be stale, or not yet ack'd
        if(!datHdr->ackSent){
            //DBG("Packet has not been ack'd\n");
            return etcpETRYAGAIN; //Ack has not yet been made for this, cannot give over to the user until it has
        }

        if(datHdr->staleDat){
            DBG("Releasing stale packet\n");
            cqReleaseSlot(conn->rxQ,seqNum);
            continue; //The packet is stale, so release it, but get another one
        }

        const i8* dat = (i8*)(datHdr + 1);

        //Looks ok, give the data over to the user
        memcpy(data,dat,MIN(datHdr->datLen,*len_io));

        cqErr = cqReleaseSlot(conn->rxQ,seqNum);
        if(cqErr != cqENOERR){
            WARN("Unexpected error releasing slot %li: %s\n", seqNum, cqError2Str(cqErr));
            return etcpECQERR;
        }

        DBG("Packet with seq=%li and len=%li given to user\n", seqNum, *len_io);
        //We've copied a valid packet and released it, the user can have it now
        DBG("New rd_min=%li, wr_max=%li\n", conn->rxQ->rdMin, conn->rxQ->wrMax);
        return etcpENOERR;
    }

    //Unreachable!
    return etcpEFATAL;
}

//Assumes ethernet packets, does in-place construction of a packet and puts it into the circular queue ready to send
//This is a user facing function
etcpError_t doEtcpUserTx(etcpConn_t* const conn, const void* const toSendData, i64* const toSendLen_io)

{
    //DBG("Doing user tx, with %li bytes to send\n", *toSendLen_io);
    const i64 toSendLen = *toSendLen_io;
    i64 bytesSent = 0;

    cq_t* const txcq = conn->txQ;
    while(bytesSent< toSendLen){
        //DBG("Bytes sent so far = %lli\n", bytesSent);
        cqSlot_t* slot = NULL;
        i64 seqNum = 0;
        cqError_t cqErr = cqGetNextWr(txcq,&slot,&seqNum);

        //We haven't sent as much as we'd hoped, set the len_io and tell user to try again
        if_unlikely(cqErr == cqENOSLOT){
            //DBG("Ran out of CQ slots\n");
            *toSendLen_io = bytesSent;
            return etcpETRYAGAIN;
        }
        //Some other strange error. Shit.
        else if_unlikely(cqErr != cqENOERR){
            ERR("Error on circular queue: %s", cqError2Str(cqErr));
            return etcpECQERR;
        }
        //DBG("Got a new CQ slot\n");

        //We got a slot, now format a pBuff into it
        pBuff_t* pBuff  = slot->buff;
        pBuff->buffer   = pBuff + 1;
        pBuff->buffSize = slot->len - sizeof(pBuff);
        pBuff->msgSize  = 0;

        i8* buff    = pBuff->buffer;
        i64 buffLen = pBuff->buffSize;
        i64 ethLen  = buffLen;
        //XXX HACK - This should be externalised and happen after the ETCP packet formatting to allow multiple carrier transports
        etcpError_t etcpErr = etcpMkEthPkt(buff,&ethLen,conn->flowId.srcAddr, conn->flowId.dstAddr,conn->vlan, conn->priority);
        if_unlikely(etcpErr != etcpENOERR){
            WARN("Could not format Ethernet packet\n");
            return etcpErr;
        }
        pBuff->encapHdr     = buff;
        pBuff->encapHdrSize = ethLen;
        buff               += ethLen;
        pBuff->msgSize     += ethLen;
        buffLen            -= ethLen;

        const i64 hdrsLen = sizeof(etcpMsgHead_t) + sizeof(etcpMsgDatHdr_t);
        if_unlikely(buffLen < hdrsLen + 1){ //Should be able to send at least 1 byte!
            ERR("Slot lengths are too small!");
            return etcpEFATAL;
        }
        const i64 datSpace = buffLen - hdrsLen;
        const i64 datLen   = MIN(datSpace,toSendLen);
        pBuff->msgSize    += hdrsLen;

        struct timespec ts = {0};
        clock_gettime(CLOCK_REALTIME,&ts);
        etcpMsgHead_t* const head = (etcpMsgHead_t* const)buff;
        pBuff->etcpHdr = head;
        pBuff->etcpHdrSize = sizeof(etcpMsgHead_t);

        head->fulltype      = ETCP_V1_FULLHEAD(ETCP_DAT);
        head->srcPort       = conn->flowId.srcPort;
        head->dstPort       = conn->flowId.dstPort;
        head->ts.swTxTimeNs = ts.tv_sec * 1000 * 1000 * 1000 + ts.tv_nsec;

        etcpMsgDatHdr_t* const datHdr = (etcpMsgDatHdr_t* const)(head + 1);
        pBuff->etcpDatHdr     = datHdr;
        pBuff->etcpDatHdrSize = sizeof(etcpMsgDatHdr_t);


        datHdr->datLen     = datLen;
        pBuff->msgSize    += datLen;
        datHdr->seqNum     = conn->seqSnd;
        datHdr->txAttempts = 0;

        void* const msgDat = (void* const)(datHdr + 1);
        pBuff->etcpPayload = msgDat;
        pBuff->etcpPayloadSize = sizeof(datLen);

        //DBG("Copying %li payload to data\n", datLen);
        memcpy(msgDat,toSendData,datLen);

        pBuff->txState = ETCP_TX_RDY; //Packet is ready to be sent, subject to Transmission Control.

        //At this point, the packet is now ready to send!
        const i64 totalLen = ethLen + hdrsLen + datLen;
        cqErr = cqCommitSlot(conn->txQ,seqNum, totalLen);
        if_unlikely(cqErr != cqENOERR){
            ERR("Error on circular queue: %s", cqError2Str(cqErr));
            return etcpECQERR;
        }

        bytesSent += datLen;
        conn->seqSnd++;

    }

    *toSendLen_io = bytesSent;
    return etcpENOERR;

}



