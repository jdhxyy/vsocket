// Copyright 2020-2021 The jdh99 Authors. All rights reserved.
// ����˿�
// Authors: jdh99 <jdh821@163.com>
// ��ģ�鹦��:
// 1.����,��Ƶ,���ڵȽӿ�ͳһ�շ�����
// 2.ʹ��fifo���ƿ�ֱ�Ӵ��жϽ������ݲ����л��洦��

#ifndef VSOCKET_H
#define VSOCKET_H

#include "tztype.h"

#pragma pack(1)

// VSocketInfo �˿���Ϣ
typedef struct {
    int Pipe;
    int MaxLen;
    int TxFifoItemSum;
    int RxFifoItemSum;

    // API
    TZIsAllowSendFunc IsAllowSend;
    // ������˿ڿɲ���ip��port�����ֶ�
    TZNetDataFunc Send;
} VSocketInfo;

// VSocketRxParam �˿ڽ������ݲ���
typedef struct {
    int Pipe;
    uint8_t* Bytes;
    int Size;
    int Metric;
    // ����ʱ��.��λ:us
    uint64_t RxTime;

    // Դ�ڵ�������Ϣ.��������˿ڿɲ����������ֶ�
    uint32_t IP;
    uint16_t Port;
} VSocketRxParam;

// VSocketTxParam �˿ڷ������ݲ���
typedef struct {
    int Pipe;
    uint8_t* Bytes;
    int Size;

    // Ŀ�Ľڵ�������Ϣ.��������˿ڿɲ����������ֶ�
    uint32_t IP;
    uint16_t Port;
} VSocketTxParam;

#pragma pack()

// VSocketRxFunc ��ĳ�˿ڽ��պ�������
typedef void (*VSocketRxFunc)(VSocketRxParam* rxParam);

// VSocketLoad ģ������
// mid��tzmalloc�ڴ�����е��ڴ�id,maxSocketNum�����˿���
bool VSocketLoad(int mid, int maxSocketNum);

// VSocketCreate ����socket,������fifo
// �������Ҫ����fifo,��TxFifoItemSum������Ϊ0
// ����fifo�Ǳ����,RxFifoItemSum��������Ϊ0
bool VSocketCreate(VSocketInfo* socketInfo);

// VSocketTx ��������
// ������˿ڲ���Ҫ��ip��port�����ֶ�,��0����
bool VSocketTx(VSocketTxParam* txParam);

// VSocketRx ��������
// Ӧ��ģ����յ����ݺ�����ñ�����
// rxTime�ֶβ���Ҫ��,��0����
// ������˿ڲ���Ҫ��ip��port�����ֶ�,��0����
bool VSocketRx(VSocketRxParam* rxParam);

// VSocketRegisterObserver ע����չ۲���
// callback�ǻص�����,�˿ڽ��յ����ݻ�ص��˺���
bool VSocketRegisterObserver(VSocketRxFunc callback);

// VSocketIsAllowSend �Ƿ�������
bool VSocketIsAllowSend(int pipe);

// VSocketRxFifoWriteable ������fifo�Ƿ����д��
bool VSocketRxFifoWriteable(int pipe);

#endif
