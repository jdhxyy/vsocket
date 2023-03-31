// Copyright 2020-2021 The jdh99 Authors. All rights reserved.
// 虚拟端口
// Authors: jdh99 <jdh821@163.com>
// 本模块功能:
// 1.串口,射频,网口等接口统一收发管理
// 2.使用fifo机制可直接从中断接收数据并进行缓存处理

#ifndef VSOCKET_H
#define VSOCKET_H

#include "tztype.h"

#pragma pack(1)

// VSocketInfo 端口信息
typedef struct {
    int Pipe;
    int MaxLen;
    int TxFifoItemSum;
    int RxFifoItemSum;

    // API
    TZIsAllowSendFunc IsAllowSend;
    // 非网络端口可不管ip和port两个字段
    TZNetDataFunc Send;
} VSocketInfo;

// VSocketRxParam 端口接收数据参数
typedef struct {
    int Pipe;
    uint8_t* Bytes;
    int Size;
    int Metric;
    // 接收时间.单位:us
    uint64_t RxTime;

    // 源节点网络信息.不是网络端口可不管这两个字段
    uint32_t IP;
    uint16_t Port;
} VSocketRxParam;

// VSocketTxParam 端口发送数据参数
typedef struct {
    int Pipe;
    uint8_t* Bytes;
    int Size;

    // 目的节点网络信息.不是网络端口可不管这两个字段
    uint32_t IP;
    uint16_t Port;
} VSocketTxParam;

#pragma pack()

// VSocketRxFunc 从某端口接收函数类型
typedef void (*VSocketRxFunc)(VSocketRxParam* rxParam);

// VSocketLoad 模块载入
// mid是tzmalloc内存管理中的内存id,maxSocketNum是最大端口数
bool VSocketLoad(int mid, int maxSocketNum);

// VSocketCreate 创建socket,并建立fifo
// 如果不需要发送fifo,则TxFifoItemSum可设置为0
// 接收fifo是必须的,RxFifoItemSum不能设置为0
bool VSocketCreate(VSocketInfo* socketInfo);

// VSocketTx 发送数据
// 非网络端口不需要管ip和port两个字段,填0即可
bool VSocketTx(VSocketTxParam* txParam);

// VSocketRx 接收数据
// 应用模块接收到数据后需调用本函数
// rxTime字段不需要管,填0即可
// 非网络端口不需要管ip和port两个字段,填0即可
bool VSocketRx(VSocketRxParam* rxParam);

// VSocketRegisterObserver 注册接收观察者
// callback是回调函数,端口接收到数据会回调此函数
bool VSocketRegisterObserver(VSocketRxFunc callback);

// VSocketIsAllowSend 是否允许发送
bool VSocketIsAllowSend(int pipe);

// VSocketRxFifoWriteable 检查接收fifo是否可以写入
bool VSocketRxFifoWriteable(int pipe);

#endif
