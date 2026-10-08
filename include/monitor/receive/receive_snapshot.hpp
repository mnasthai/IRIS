#pragma once
/**
 * @file receive_snapshot.hpp
 * @brief 微信接收消息快照与内存解构接口
 * @details 
 *   定义批量消息 (Batch) 与单项消息 (Item) 的内存观测函数。
 *   所有读取严格保持“有界观测”原则：只抽取受限长度的快照放入 Event，
 *   原生内存指针仅在回调执行期间瞬态借用 (Borrowed)，绝不向异步日志队列转交原生裸指针。
 */

#include "monitor/diagnostics/event.hpp"

namespace wechatbot::monitor {

// 严格保持有界观测：原生参数在回调期间仅被短期借用 (Borrowed)，绝不被日志队列长期持有。

/**
 * @brief 观测并提取单个 AddMsg 消息项
 * @param batch 当前批次的父级 Event 事件
 * @param item 指向当前消息项结构体内存首地址的指针
 * @param index 当前消息在批次数组中的下标索引 (0, 1, 2...)
 * @note 
 *   内部会自动校验虚表特征 (kAddMsgVtableRva)、hasBits 掩码，
 *   并安全提取 from, to, content, msgSource 以及标量数据 (MsgType, CreateTime, NewMsgId)。
 */
void ObserveItem(const Event& batch, const uint8_t* item, uint32_t index);

/**
 * @brief 观测并校验接收消息批处理向量 (std::vector<AddMsg>)
 * @param context 微信内部上下文指针 (RCX)
 * @param vector 指向 std::vector 首部的指针 (RDX)
 * @param flag3 批处理控制标志位 1 (R8)
 * @param flag4 批处理控制标志位 2 (R9)
 * @return Event 组装完成的 Batch 事件对象
 * @note 
 *   内部会校验 vector 边界可读性、步长整除 (kBatchStride = 0x78) 与上限限制，
 *   并循环遍历批次中每一项调用 ObserveItem。
 */
Event ObserveBatch(void* context, void* vector, unsigned char flag3, unsigned char flag4);

} // namespace wechatbot::monitor
