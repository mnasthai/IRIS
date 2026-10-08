/**
 * @file receive_snapshot.cpp
 * @brief 微信批量与单项接收消息内存解构实现
 * @details 
 *   实现对 std::vector<AddMsg> 容器与单个 AddMsg 结构体的安全解析。
 *   包含严格的内存探针检查、虚表指纹比对、hasBits 位标志解析、SSO 字符串与标量安全拷贝，
 *   以及全面的 Windows SEH 异常隔离。
 */

#include "monitor/receive/receive_snapshot.hpp"
#include "monitor/core/memory.hpp"
#include "monitor/native/target.hpp"
#include "monitor/diagnostics/logging.hpp"
#include "monitor/media/media_capture.hpp"

namespace wechatbot::monitor {

/**
 * @brief 解析单个 AddMsg 消息项
 * @param batch 当前批次的上下文 Event 对象
 * @param item 指向当前消息项结构体内存首地址的指针 AddMsg 实例
 * @param index 当前消息在批次数组中的索引下标
 * 
 * @details 
 *   利用 CopyField SSO 解引用与 UTF-8 边界回溯防截断 提取
 *   调用 CopyScalars 提取 CreateTime、NewMsgId、Status 等字段
 *   调用 QueueEvent 将快照加入队列 若为图片/语音消息则触发 CaptureMediaReceive 追踪媒体下载
 */
void ObserveItem(const Event& batch, const uint8_t* item, uint32_t index) {

    Event event = batch;  // 父级 Batch 消息批次的基本信息 比如当时的系统时间、进程 PID、Call ID 等
    event.SetKind(EventKind::Item); // 自增主键 ID
    event.header.sequence = NextSequence();  // 记录当前消息在批次里的下标索引 index 内存物理地址 item 这批一共有几条 count

    auto& snapshot = event.Get<MessageSnapshot>();
    auto& message = snapshot.message;
    message.index = index;
    message.item = reinterpret_cast<uint64_t>(item);
    message.count = batch.Get<CallSnapshot>().message.count;

    __try {
        // 检查内存 防止野指针
        if (!IsReadableRange(item, kReceiveLayout.size)) {
            QueueEvent(event);
            return;
        }

        // 步骤 2: 虚表校验
        message.vtableMatch =
            *reinterpret_cast<const uintptr_t*>(item) ==
            reinterpret_cast<uintptr_t>(g_weixin) + kAddMsgVtableRva;
        if (!message.vtableMatch) {
            QueueEvent(event);
            return;
        }

        // 步骤 3: 提取 hasBits 字段存在位图
        // 微信服务器与客户端通信的核心协议是 Google Protocol Buffers（Protobuf）
        // Protobuf 协议
        // 在 Protobuf 编译生成的 C++ 结构体里 为了极度节省内存 并不会给每个字段都设默认值
        // 而是在结构体末尾（偏移量 +0x6C 处）放了一个 32 位的整型掩码（Bitmap） 叫做 _has_bits_
        // 二进制的 0x08 是 0000 1000（即从右往左第 4 位）
        // 如果这一位是 1 说明服务器发过来的消息里 确实包含了 消息类型 MsgType 字段
        message.hasBits = *reinterpret_cast<const uint32_t*>(item + kReceiveLayout.hasBits);
        if (message.hasBits & 0x08)
            message.msgType = *reinterpret_cast<const uint32_t*>(item + 0x14);  // 确认消息类型

        // 步骤 4: 提取字符串字段
        // 发信人
        snapshot.from.read = CopyField(item, kReceiveLayout, kReceiveFrom, snapshot.from.data(), snapshot.from.capacity());
        // 收信人
        snapshot.to.read = CopyField(item, kReceiveLayout, kReceiveTo, snapshot.to.data(), snapshot.to.capacity());
        // 正文
        snapshot.content.read = CopyField(item, kReceiveLayout, kReceiveContent, snapshot.content.data(), snapshot.content.capacity());
        // 消息源 XML 上下文
        snapshot.msgSource.read = CopyField(item, kReceiveLayout, kReceiveSource, snapshot.msgSource.data(), snapshot.msgSource.capacity());
        // 其它辅助字段
        snapshot.field11.read = CopyField(item, kReceiveLayout, kReceiveAux, snapshot.field11.data(), snapshot.field11.capacity());

        // 步骤 5: 提取标量字段 (CreateTime, NewMsgId, Status 等)
        CopyScalars(item, message.hasBits, kReceiveScalars, std::size(kReceiveScalars), snapshot.rawFields.data());
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // Windows 硬件异常拦截 一旦发生缺页中断等硬件异常 标记虚表匹配失败 确保微信不闪退
        message.vtableMatch = 0;
    }

    // 步骤 6: 提交事件快照至无锁环形队列
    QueueEvent(event);

    // 步骤 7: 若为图片或语音消息 移交多媒体管线追踪下载完成事件
    CaptureMediaReceive(event, item);
}

    
/**
 * @brief 观察并解析接收消息批次向量 (std::vector<AddMsg>)
 * @param context 微信内部上下文指针
 * @param vector 包含 AddMsg 结构体的 std::vector 容器首地址
 * @param flag3 批处理控制标志位 1
 * @param flag4 批处理控制标志位 2
 * @return Event 组装完成的批处理 Event 快照
 * 
 * @details
 * std::vector 结构其实极其简单 只有两个 64 位指针 所以解析起来很头疼
 * begin-->指向数组第一个元素的内存地址
 * end-->指向数组最后一个元素之后的内存地址
 */

Event ObserveBatch(void* context, void* vector, unsigned char flag3, unsigned char flag4) {
    Event batch = MakeCallEvent(EventKind::Batch);
    auto& message = batch.Get<CallSnapshot>().message;
    message.context = reinterpret_cast<uint64_t>(context);
    message.vector = reinterpret_cast<uint64_t>(vector);
    message.flag3 = flag3;
    message.flag4 = flag4;

    __try {
        // 步骤 1: 先内存检查
        if (!IsReadableRange(vector, sizeof(uintptr_t) * 2)) {
            QueueEvent(batch);
            return batch;
        }

        const auto* bounds = reinterpret_cast<const uintptr_t*>(vector);
        message.begin = bounds[0];
        message.end = bounds[1];
        if (!message.begin || message.end < message.begin) {
            QueueEvent(batch);
            return batch;
        }

        // 步骤 2: 检查每条消息
        // 每条 AddMsg 占用 0x78 字节 120
        const uint64_t byteLength = message.end - message.begin;
        if ((byteLength % kBatchStride) != 0 || byteLength > kBatchStride * kMaxBatchItems ||
            (byteLength && !IsReadableRange(reinterpret_cast<const void*>(message.begin),
                                            static_cast<size_t>(byteLength)))) {
            QueueEvent(batch);
            return batch;  // 如果步长对不上说明 内存错乱 直接放弃 读了也白度
        }

        // 步骤 3: 计算消息条数并遍历
        message.validVector = 1;
        message.count = static_cast<uint32_t>(byteLength / kBatchStride);
        QueueEvent(batch);

        // 步骤 4: 逐项解析每个 AddMsg 结构体
        const uint32_t limit = message.count > kMaxLoggedItemsPerBatch
            ? static_cast<uint32_t>(kMaxLoggedItemsPerBatch) : message.count;
        for (uint32_t index = 0; index < limit; ++index) {
            ObserveItem(batch,
                        reinterpret_cast<const uint8_t*>(message.begin + index * kBatchStride),
                        index);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // SEH 异常兜底 防止意外异常向上传播打断原流程
        QueueEvent(batch);
    }
    return batch;
}

} // namespace wechatbot::monitor
