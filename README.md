# vsocket

## 1. 介绍
虚拟端口

模块功能:
- 串口,射频,网口等接口统一收发管理
- 使用fifo机制可直接从中断接收数据并进行缓存处理

## 2. API
```c
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
```
