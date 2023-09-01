// Copyright 2020-2021 The jdh99 Authors. All rights reserved.
// 虚拟端口
// Authors: jdh99 <jdh821@163.com>
// 本模块功能:
// 1.串口,射频,网口等接口统一收发管理
// 2.使用fifo机制可直接从中断接收数据并进行缓存处理

#include "vsocket.h"
#include "kuggis.h"
#include "tzmalloc.h"
#include "tztime.h"
#include "async.h"
#include "tzlist.h"
#include "lagan.h"
#include "statistics.h"

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

// 统计项id
static int gIdRxOK = -1;
static int gIdRxFail = -1;
static int gIdTxOK = -1;
static int gIdTxFail = -1;

static bool gIsTxBusy = false;
static bool gIsRxBusy = false;

static int task(void);
static int checkTxFifo(void);
static int checkRxFifo(void);
static bool isObserverExist(VSocketRxFunc callback);
static TZListNode* createNode(void);

// VSocketLoad 模块载入
// mid是tzmalloc内存管理中的内存id,maxSocketNum是最大端口数
bool VSocketLoad(int mid, int maxSocketNum) {
    gIdRxOK = StatisticsRegister("vsocket_rx_ok");
    gIdRxFail = StatisticsRegister("vsocket_rx_fail");
    gIdTxOK = StatisticsRegister("vsocket_tx_ok");
    gIdTxFail = StatisticsRegister("vsocket_tx_fail");

    if (gIdRxOK < 0 || gIdRxFail < 0 || gIdTxOK < 0 || gIdTxFail < 0) {
        LE(TAG, "load fail!statistics register fail");
        return false;
    }

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

static int checkTxFifo(void) {
    static struct pt pt = {0};
    static tTxTag tag;
    static int num = 0;
    static int i = 0;
    static int freeSocketNum = 0;

    PT_BEGIN(&pt);

    freeSocketNum = 0;
    for (i = 0; i < gMaxSocketNum; i++) {
        if (gSockets[i].used == false || gSockets[i].txFifo == 0) {
            freeSocketNum++;
            continue;
        }
        if (gSockets[i].isAllowSend() == false) {
            continue;
        }

        num = KuggisRead(gSockets[i].txFifo, gBuffer->buf, gBuffer->len, (uint8_t*)&tag, sizeof(tTxTag));
        if (num <= 0) {
            freeSocketNum++;
            continue;
        }

        gIsTxBusy = true;
        gSockets[i].send(gBuffer->buf, num, tag.ip, tag.port);
        PT_YIELD(&pt);
    }

    if (freeSocketNum >= gMaxSocketNum) {
        gIsTxBusy = false;
    }

    PT_END(&pt);
}

static int checkRxFifo(void) {
    static struct pt pt = {0};
    static tRxTag tag;
    static int num = 0;
    static TZListNode* node = NULL;
    static tItem* item = NULL;
    static VSocketRxParam rxParam;
    static int i = 0;
    static int freeSocketNum = 0;

    PT_BEGIN(&pt);

    freeSocketNum = 0;
    for (i = 0; i < gMaxSocketNum; i++) {
        if (gSockets[i].used == false || gSockets[i].rxFifo == 0) {
            freeSocketNum++;
            continue;
        }

        num = KuggisRead(gSockets[i].rxFifo, gBuffer->buf, gBuffer->len, (uint8_t*)&tag, sizeof(tRxTag));
        if (num <= 0) {
            freeSocketNum++;
            continue;
        }
        if (gList == 0) {
            continue;
        }

        gIsRxBusy = true;

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
        // 如果要进一步YIELD,则需要gBuffer分为发送和接收两个buffer
        PT_YIELD(&pt);
    }

    if (freeSocketNum >= gMaxSocketNum) {
        gIsRxBusy = false;
    }

    PT_END(&pt);
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

    if (socketInfo->TxFifoSize > 0) {
        gSockets[socketInfo->Pipe].txFifo = KuggisCreate(gMid, socketInfo->TxFifoSize);
        if (gSockets[socketInfo->Pipe].txFifo == 0) {
            LE(TAG, "socket:%d create failed!create tx fifo failed", socketInfo->Pipe);
            return false;
        }
    }
    if (socketInfo->RxFifoSize > 0) {
        gSockets[socketInfo->Pipe].rxFifo = KuggisCreate(gMid, socketInfo->RxFifoSize);
        if (gSockets[socketInfo->Pipe].rxFifo == 0) {
            if (gSockets[socketInfo->Pipe].txFifo) {
                KuggisDelete(gSockets[socketInfo->Pipe].txFifo);
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
            TZBufferDynamic* buffer = TZMalloc(gMid, (int)sizeof(TZBufferDynamic) + socketInfo->MaxLen);
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
        StatisticsAdd(gIdTxFail);
        return false;
    }
    if (gSockets[txParam->Pipe].txFifo == 0) {
        if (gSockets[txParam->Pipe].isAllowSend()) {
            gSockets[txParam->Pipe].send(txParam->Bytes, txParam->Size, txParam->IP, txParam->Port);
            StatisticsAdd(gIdTxOK);
            return true;
        }
        LW(TAG, "%d send failed!pipe is busy", txParam->Pipe);
        StatisticsAdd(gIdTxFail);
        return false;
    }

    int count = KuggisWriteableCount(gSockets[txParam->Pipe].txFifo);
    if (count < txParam->Size) {
        LW(TAG, "%d send failed!fifo is full.%d %d", txParam->Pipe, count, txParam->Size);
        StatisticsAdd(gIdTxFail);
        return false;
    }

    tTxTag tag;
    tag.ip = txParam->IP;
    tag.port = txParam->Port;
    if (KuggisWrite(gSockets[txParam->Pipe].txFifo, txParam->Bytes, txParam->Size, (uint8_t*)&tag, sizeof(tTxTag)) == false) {
        LW(TAG, "%d send failed!write fifo failed", txParam->Pipe);
        StatisticsAdd(gIdTxFail);
        return false;
    }
    StatisticsAdd(gIdTxOK);
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
        LW(TAG, "rx fail!pipe:%d size:%d max len:%d", rxParam->Pipe, rxParam->Size, gSockets[rxParam->Pipe].maxLen);
        StatisticsAdd(gIdRxFail);
        return false;
    }
    if (gSockets[rxParam->Pipe].rxFifo == 0) {
        StatisticsAdd(gIdRxFail);
        return false;
    }
    int count = KuggisWriteableCount(gSockets[rxParam->Pipe].rxFifo);
    if (count < rxParam->Size) {
        LW(TAG, "rx fail!fifo is full.pipe:%d.%d %d", rxParam->Pipe, count, rxParam->Size);
        StatisticsAdd(gIdRxFail);
        return false;
    }
    if (KuggisWrite(gSockets[rxParam->Pipe].rxFifo, rxParam->Bytes, rxParam->Size, (uint8_t*)&tag, sizeof(tRxTag)) == false) {
        LW(TAG, "rx fail!write fail.pipe:%d", rxParam->Pipe);
        StatisticsAdd(gIdRxFail);
        return false;
    }
    StatisticsAdd(gIdRxOK);
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
        return KuggisWriteableCount(gSockets[pipe].txFifo) > 0;
    }
}

// VSocketRxFifoWriteable 检查接收fifo是否可以写入
bool VSocketRxFifoWriteable(int pipe) {
    if (pipe >= gMaxSocketNum || gSockets[pipe].used == false || 
        gSockets[pipe].rxFifo == 0) {
        return false;
    }
    return KuggisWriteableCount(gSockets[pipe].rxFifo) > 0;
}

// VSocketIsBusy 是否忙碌
// 对忙碌的定义是有发送和接收任务.本函数接口可用于低功耗,不忙时可进入休眠
bool VSocketIsBusy(void) {
    return gIsTxBusy == true || gIsRxBusy == true;
}
