// Copyright 2020-2021 The jdh99 Authors. All rights reserved.
// 虚拟端口
// Authors: jdh99 <jdh821@163.com>
// 本模块功能:
// 1.串口,射频,网口等接口统一收发管理
// 2.使用fifo机制可直接从中断接收数据并进行缓存处理

#include "vsocket.h"
#include "tzfifo.h"
#include "tzmalloc.h"
#include "tztime.h"
#include "async.h"
#include "tzlist.h"
#include "lagan.h"

#include <string.h>

#define TAG "vsocket"

#pragma pack(1)

typedef struct {
    bool used;
    int maxLen;
    intptr_t txFifo;
    intptr_t rxFifo;

    // API
    TZIsAllowSendFunc isAllowSend;
    TZNetDataFunc send;
} tSocket;

typedef struct {
    int metric;
    uint64_t rxTime;
    uint32_t ip;
    uint16_t port;
} tRxTag;

typedef struct {
    uint32_t ip;
    uint16_t port;
} tTxTag;

// 接收观察者
typedef struct {
    VSocketRxFunc callback;
} tItem;

#pragma pack()

static int gMid = -1;
static tSocket* gSockets = NULL;
static int gMaxSocketNum = 0;
// 用于发送和接收缓存
static TZBufferDynamic* gBuffer = NULL;
// 存储接收观察者
static intptr_t gList = 0;

static int task(void);
static void checkTxFifo(void);
static void checkRxFifo(void);
static bool isObserverExist(VSocketRxFunc callback);
static TZListNode* createNode(void);

// VSocketLoad 模块载入
// mid是tzmalloc内存管理中的内存id,maxSocketNum是最大端口数
bool VSocketLoad(int mid, int maxSocketNum) {
    if (mid < 0 || maxSocketNum < 0) {
        return false;
    }
    gMid = mid;
    gList = TZListCreateList(gMid);
    if (gList == 0) {
        LE(TAG, "load failed!create list failed");
        return false;
    }
    gSockets = TZMalloc(gMid, maxSocketNum * (int)sizeof(tSocket));
    if (gSockets == NULL) {
        TZListDrop(gList); 
        gList = 0;
        LE(TAG, "load failed!create sockets failed");
        return false;
    }

    if (AsyncStart(task, ASYNC_NO_WAIT) == false) {
        LE(TAG, "load failed!create task failed");
        return false;
    }

    LI(TAG, "load success");
    gMaxSocketNum = maxSocketNum;
    return true;
}

static int task(void) {
    static struct pt pt = {0};

    PT_BEGIN(&pt);

    checkTxFifo();
    checkRxFifo();

    PT_END(&pt);
}

static void checkTxFifo(void) {
    static tTxTag tag;
    static int num = 0;

    for (int i = 0; i < gMaxSocketNum; i++) {
        if (gSockets[i].used == false || gSockets[i].txFifo == 0) {
            continue;
        }
        if (gSockets[i].isAllowSend() == false) {
            continue;
        }

        num = TZFifoReadMix(gSockets[i].txFifo, (uint8_t*)&tag, sizeof(tTxTag), 
            gBuffer->buf, gBuffer->len);
        if (num <= 0) {
            continue;
        }
        gSockets[i].send(gBuffer->buf, num, tag.ip, tag.port);
    }
}

static void checkRxFifo(void) {
    static tRxTag tag;
    static int num = 0;
    static TZListNode* node = NULL;
    static tItem* item = NULL;
    static VSocketRxParam rxParam;

    for (int i = 0; i < gMaxSocketNum; i++) {
        if (gSockets[i].used == false || gSockets[i].rxFifo == 0) {
            continue;
        }

        num = TZFifoReadMix(gSockets[i].rxFifo, (uint8_t*)&tag, sizeof(tRxTag), 
            gBuffer->buf, gBuffer->len);
        if (num <= 0) {
            continue;
        }
        if (gList == 0) {
            continue;
        }

        rxParam.Pipe = i;
        rxParam.Bytes = gBuffer->buf;
        rxParam.Size = num;
        rxParam.Metric = tag.metric;
        rxParam.RxTime = tag.rxTime;
        rxParam.IP = tag.ip;
        rxParam.Port = tag.port;

        // 通知观察者
        node = TZListGetHeader(gList);
        for (;;) {
            if (node == NULL) {
                break;
            }
            item = (tItem*)node->Data;
            item->callback(&rxParam);
            node = node->Next;
        }
    }
}

// VSocketCreate 创建socket,并建立fifo
// 如果不需要发送fifo,则TxFifoItemSum可设置为0
// 接收fifo是必须的,RxFifoItemSum不能设置为0
bool VSocketCreate(VSocketInfo* socketInfo) {
    if (socketInfo == NULL || socketInfo->Pipe >= gMaxSocketNum || 
        gSockets[socketInfo->Pipe].used || socketInfo->MaxLen <= 0) {
        LE(TAG, "socket create failed!param is wrong");
        return false;
    }

    if (socketInfo->TxFifoItemSum > 0) {
        // 多4个字节是因为fifo存储混合结构体需增加4字节长度
        gSockets[socketInfo->Pipe].txFifo = TZFifoCreate(gMid, 
            socketInfo->TxFifoItemSum, socketInfo->MaxLen + sizeof(tTxTag) + 4);
        if (gSockets[socketInfo->Pipe].txFifo == 0) {
            LE(TAG, "socket:%d create failed!create tx fifo failed", socketInfo->Pipe);
            return false;
        }
    }
    if (socketInfo->RxFifoItemSum > 0) {
        // 多4个字节是因为fifo存储混合结构体需增加4字节长度
        gSockets[socketInfo->Pipe].rxFifo = TZFifoCreate(gMid, 
            socketInfo->RxFifoItemSum, socketInfo->MaxLen + sizeof(tRxTag) + 4);
        if (gSockets[socketInfo->Pipe].rxFifo == 0) {
            if (gSockets[socketInfo->Pipe].txFifo) {
                TZFifoDelete(gSockets[socketInfo->Pipe].txFifo);
                gSockets[socketInfo->Pipe].txFifo = 0;
            }
            LE(TAG, "socket:%d create failed!create rx fifo failed", socketInfo->Pipe);
            return false;
        }
    }
    gSockets[socketInfo->Pipe].maxLen = socketInfo->MaxLen;
    gSockets[socketInfo->Pipe].isAllowSend = socketInfo->IsAllowSend;
    gSockets[socketInfo->Pipe].send = socketInfo->Send;
    gSockets[socketInfo->Pipe].used = true;

    // 开辟用于发送和接收的缓存
    if (gBuffer == NULL) {
        gBuffer = TZMalloc(gMid, (int)sizeof(TZBufferDynamic) + socketInfo->MaxLen);
        if (gBuffer == NULL) {
            LE(TAG, "socket:%d create failed!malloc buffer failed", socketInfo->Pipe);
            return false;
        }
        gBuffer->len = socketInfo->MaxLen;
    } else {
        if (socketInfo->MaxLen > gBuffer->len) {
            TZBufferDynamic* buffer = TZMalloc(gMid, 
                (int)sizeof(TZBufferDynamic) + socketInfo->MaxLen);
            if (buffer == NULL) {
                LE(TAG, "socket:%d create failed!malloc buffer failed", socketInfo->Pipe);
                return false;
            }
            TZFree(gBuffer);
            gBuffer = buffer;
            gBuffer->len = socketInfo->MaxLen;
        }
    }
    LI(TAG, "socket:%d create success", socketInfo->Pipe);
    return true;
}

// VSocketTx 发送数据
// 非网络端口不需要管ip和port两个字段,填0即可
bool VSocketTx(VSocketTxParam* txParam) {
    if (txParam->Pipe >= gMaxSocketNum || 
        gSockets[txParam->Pipe].used == false || txParam->Bytes == NULL || 
        txParam->Size <= 0 || txParam->Size > gSockets[txParam->Pipe].maxLen) {
        LE(TAG, "%d send failed!param is wrong", txParam->Pipe);
        return false;
    }
    if (gSockets[txParam->Pipe].txFifo == 0) {
        if (gSockets[txParam->Pipe].isAllowSend()) {
            gSockets[txParam->Pipe].send(txParam->Bytes, txParam->Size, 
                txParam->IP, txParam->Port);
            return true;
        }
        LW(TAG, "%d send failed!pipe is busy", txParam->Pipe);
        return false;
    }
    if (TZFifoWriteable(gSockets[txParam->Pipe].txFifo) == false) {
        LW(TAG, "%d send failed!fifo is full", txParam->Pipe);
        return false;
    }

    tTxTag tag;
    tag.ip = txParam->IP;
    tag.port = txParam->Port;
    if (TZFifoWriteMix(gSockets[txParam->Pipe].txFifo, (uint8_t*)&tag, 
        sizeof(tTxTag), txParam->Bytes, txParam->Size) == false) {
        LW(TAG, "%d send failed!write fifo failed", txParam->Pipe);
        return false;
    }
    return true;
}

// VSocketRx 接收数据
// 应用模块接收到数据后需调用本函数
// rxTime字段不需要管,填0即可
// 非网络端口不需要管ip和port两个字段,填0即可
bool VSocketRx(VSocketRxParam* rxParam) {
    tRxTag tag;
    tag.rxTime = TZTimeGet();
    tag.metric = rxParam->Metric;
    tag.ip = rxParam->IP;
    tag.port = rxParam->Port;

    if (rxParam->Pipe >= gMaxSocketNum || 
        gSockets[rxParam->Pipe].used == false || rxParam->Bytes == NULL || 
        rxParam->Size <= 0 || rxParam->Size > gSockets[rxParam->Pipe].maxLen) {
        LE(TAG, "%d receive failed!param is wrong", rxParam->Pipe);
        return false;
    }
    if (gSockets[rxParam->Pipe].rxFifo == 0) {
        LW(TAG, "%d receive failed!no fifo", rxParam->Pipe);
        return false;
    }
    if (TZFifoWriteable(gSockets[rxParam->Pipe].rxFifo) == false) {
        LW(TAG, "%d receive failed!fifo is full", rxParam->Pipe);
        return false;
    }
    if (TZFifoWriteMix(gSockets[rxParam->Pipe].rxFifo, (uint8_t*)&tag, 
        sizeof(tRxTag), rxParam->Bytes, rxParam->Size) == false) {
        LW(TAG, "%d receive failed!write fifo failed", rxParam->Pipe);
        return false;
    }
    return true;
}

// VSocketRegisterObserver 注册接收观察者
// callback是回调函数,端口接收到数据会回调此函数
bool VSocketRegisterObserver(VSocketRxFunc callback) {
    if (gMid < 0 || callback == NULL) {
        LE(TAG, "register observer failed!param is wrong");
        return false;
    }

    if (isObserverExist(callback)) {
        LI(TAG, "register observer success.callback is exist");
        return true;
    }

    TZListNode* node = createNode();
    if (node == NULL) {
        LE(TAG, "register observer failed!create node failed");
        return false;
    }
    tItem* item = (tItem*)node->Data;
    item->callback = callback;
    TZListAppend(gList, node);
    return true;
}

static bool isObserverExist(VSocketRxFunc callback) {
    TZListNode* node = TZListGetHeader(gList);
    tItem* item = NULL;
    for (;;) {
        if (node == NULL) {
            break;
        }
        item = (tItem*)node->Data;
        if (item->callback == callback) {
            return true;
        }
        node = node->Next;
    }
    return false;
}

static TZListNode* createNode(void) {
    TZListNode* node = TZListCreateNode(gList);
    if (node == NULL) {
        return NULL;
    }
    node->Data = TZMalloc(gMid, sizeof(tItem));
    if (node->Data == NULL) {
        TZFree(node);
        return NULL;
    }
    return node;
}

// VSocketIsAllowSend 是否允许发送
bool VSocketIsAllowSend(int pipe) {
    if (pipe >= gMaxSocketNum || gSockets[pipe].used == false) {
        return false;
    }
    if (gSockets[pipe].txFifo == 0) {
        return gSockets[pipe].isAllowSend();
    } else {
        return TZFifoWriteable(gSockets[pipe].txFifo);
    }
}

// VSocketRxFifoWriteable 检查接收fifo是否可以写入
bool VSocketRxFifoWriteable(int pipe) {
    if (pipe >= gMaxSocketNum || gSockets[pipe].used == false || 
        gSockets[pipe].rxFifo == 0) {
        return false;
    }
    return TZFifoWriteable(gSockets[pipe].rxFifo);
}
