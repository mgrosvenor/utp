#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>

#include <exanic/exanic.h>
#include <exanic/fifo_rx.h>
#include <exanic/fifo_tx.h>
#include <exanic/time.h>



#include "src/etcpSockApi.h"
#include "src/debug.h"
#include "src/CircularQueue.h"
#include "src/packets.h"


static etcpState_t* etcpState = NULL;

typedef struct  {
    exanic_t* dev;
    exanic_rx_t* rxBuff;
    exanic_tx_t* txBuff;
} exaNicState_t;
exaNicState_t nicState;


typedef struct {
    i64 foo;
} tcState_t;
tcState_t tcState;

int etcptpTestClient()
{
    //Open the connection
    etcpSocket_t* sock = etcpSocketNew(etcpState);
    etcpConnect(sock,16,2048,0x000001,0x00000F, 0x0000002, 0x00000E, true, -1, -1);

    //Write to the connection
    i64 len = 128;
    i8 dat[len];
    for(int i = 0; i < len; i++){
        dat[i] = 0xAA + i;
    }
    etcpSend(sock,dat,&len);

    //Close the connection
    etcpClose(sock);

    return 0;
}

int etcptpTestServer()
{
    //Open a socket and bind it
    etcpSocket_t* sock = etcpSocketNew(etcpState);
    etcpBind(sock,16,2048,0x000002,0x00000E, -1, -1);

    //Tell the socket to list
    etcpListen(sock,8);

    etcpSocket_t* accSock = NULL;
    etcpError_t accErr = etcpETRYAGAIN;
    for( accErr = etcpETRYAGAIN; accErr == etcpETRYAGAIN; accErr = etcpAccept(sock,&accSock)){
        sleep(1);
    }
    if(accErr != etcpENOERR){
        ERR("Something borke on accept!\n");
        return -1;
    }

    i8 data[128] = {0};
    i64 len = 128;
    etcpError_t recvErr = etcpETRYAGAIN;
    for(recvErr = etcpETRYAGAIN; recvErr == etcpETRYAGAIN; recvErr = etcpRecv(accSock,&data,&len)){
        sleep(1);
    }

    if(recvErr != etcpENOERR){
        ERR("Something borke on accept!\n");
        return -1;
    }


    DBG("Success!\n");
    for(int i = 0; i < 128; i++){
        printf("%i 0x%02x\n", i, (uint8_t)data[i]);
    }

    //Close the connection
    etcpClose(sock);

    return 0;

}


//Returns: >0 this is the number of acknowledgement packets that can be generated. <=0 no ack packets will be generated
int64_t etcpRxTc(void* const rxTcState, const cq_t* const datRxQ, const cq_t* const ackTxQ )
{
    DBG("RX TC Called!\n");
    (void)rxTcState;
    (void)datRxQ;
    (void)ackTxQ;
    return 1;
}

void etcpTxTc(void* const txTcState, const cq_t* const datTxQ, cq_t* ackTxQ, const cq_t* ackRxQ, bool* const ackFirst, i64* const maxAck_o, i64* const maxDat_o )
{
    (void)txTcState;
    (void)datTxQ;
    (void)ackTxQ;
    (void)ackRxQ;
    (void)ackFirst;
    (void)maxAck_o;
    (void)maxDat_o;
    int i = 0;
    for(; i < datTxQ->slotCount; i++){
        cqSlot_t* slot = NULL;
        cqError_t cqe = cqGetSlotIdx(datTxQ,&slot,i);
        if(cqe != cqENOERR){
            break;
        }
        pBuff_t* pbuff = slot->buff;
        pbuff->txState = ETCP_TX_NOW;

    }

    *maxDat_o = i;
}



//The ETCP internal state expects to be provided with hardware send and receive operations, these typedefs spell them out
//A generic wrapper around the "hardware" tx layer
//Returns: >0, number of bytes transmitted =0, no send capacity, try again, <0 hw specific error code
static int64_t exanicTx(void* const hwState, const void* const data, const int64_t len, uint64_t* const hwTxTimeNs )
{
    exaNicState_t* const exaNicState = hwState;

    hexdump(data,len);
    ssize_t result = exanic_transmit_frame(exaNicState->txBuff,(const char*)data, len);
    const uint32_t txTimeCyc = exanic_get_tx_timestamp(exaNicState->txBuff);
    *hwTxTimeNs = exanic_timestamp_to_counter(exaNicState->dev, txTimeCyc);
    return result;
}


//Returns: >0, number of bytes received, =0, nothing available right now, <0 hw specific error code
static int64_t exanicRx(void* const hwState, void* const data, const int64_t len, uint64_t* const hwRxTimeNs )
{
    (void)hwState;
    (void)len;


//    exaNicState_t* const exaNicState = hwState;
//    uint32_t rxTimeCyc = -1;

//    ssize_t result = exanic_receive_frame(exaNicState->rxBuff, (char*)data, len, &rxTimeCyc);
//    *hwRxTimeNs = exanic_timestamp_to_counter(exaNicState->dev, rxTimeCyc);
    *hwRxTimeNs = -1;
    char frame[218] = "\x02\x00\x00\x00\x00\x00\x01\x00\x00\x00\x00\x00\x88\x88\x03\x01\x50\x43\x54\x45\x04\x00\x0f\x00\x00\x00\x00\x00\x00\x00\x0e\x00\x00\x00\x00\x00\x00\x00\x49\x87\x63\xed\x99\x2b\x43\x14\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x80\x00\x00\x00\x00\x00\x00\x00\xaa\xab\xac\xad\xae\xaf\xb0\xb1\xb2\xb3\xb4\xb5\xb6\xb7\xb8\xb9\xba\xbb\xbc\xbd\xbe\xbf\xc0\xc1\xc2\xc3\xc4\xc5\xc6\xc7\xc8\xc9\xca\xcb\xcc\xcd\xce\xcf\xd0\xd1\xd2\xd3\xd4\xd5\xd6\xd7\xd8\xd9\xda\xdb\xdc\xdd\xde\xdf\xe0\xe1\xe2\xe3\xe4\xe5\xe6\xe7\xe8\xe9\xea\xeb\xec\xed\xee\xef\xf0\xf1\xf2\xf3\xf4\xf5\xf6\xf7\xf8\xf9\xfa\xfb\xfc\xfd\xfe\xff\x00\x01\x02\x03\x04\x05\x06\x07\x08\x09\x0a\x0b\x0c\x0d\x0e\x0f\x10\x11\x12\x13\x14\x15\x16\x17\x18\x19\x1a\x1b\x1c\x1d\x1e\x1f\x20\x21\x22\x23\x24\x25\x26\x27\x28\x29\x3f\xd1\x7c\xa9";
    memcpy(data,frame,218);
    ssize_t result = 218;

//    if(result > 0){
//        //hexdump(data,result);
//        printf("Dumping packet state\n");
//        printf("\n\nchar frame[%li] = {",result);
//        for(int i = 0; i < result; i++){
//            printf("\\x%02x",((uint8_t*)data)[i]);
//        }
//        printf("};\n");
//        printf("Done dumping state\n");
//    }
    return result;
}



static inline void exanicInit(exaNicState_t* const nicState, const char* exanicDev, const int exanicPort)
{
    nicState->dev = exanic_acquire_handle(exanicDev);
    if(!nicState->dev ){
        ERR("Could not get handle for device %s (%s)\n", exanicDev, exanic_get_last_error());
        return;
    }


    const int rxBuffNo = 0;
    nicState->rxBuff = exanic_acquire_rx_buffer(nicState->dev, exanicPort, rxBuffNo);
    if(!nicState->rxBuff ){
        ERR("Could not get handle for rx buffer %s:%i:%i (%s)\n", exanicDev, exanicPort, rxBuffNo, exanic_get_last_error());
        return;
    }


    const int txBuffNo = 0;
    const int reqMult = 4;
    nicState->txBuff = exanic_acquire_tx_buffer(nicState->dev, exanicPort, reqMult * 4096);
    if(!nicState->dev ){
        ERR("Could not get handle for tx buffer %s:%i:%i (%s)\n", exanicDev, exanicPort, txBuffNo, exanic_get_last_error());
        return;
    }

}


int main(int argc, char* argv[])
{
    (void)argc;
    (void)argv;

    if(argc < 4){
        printf("Usage test [client|server] exanic_device exanic_port");
        return -1;
    }

    const char* const exanicDev = argv[2];
    const int exanicPort = strtol(argv[3],NULL,10);

    exanicInit(&nicState, exanicDev, exanicPort);
    etcpState = etcpStateNew(&nicState,exanicTx,exanicRx,etcpTxTc,&tcState,true,etcpRxTc,&tcState,true);

    if(argv[1][0] == 's'){
        return etcptpTestServer();
    }
    else if(argv[1][0] == 'c'){
        return etcptpTestClient();
    }


    return -1;

}
