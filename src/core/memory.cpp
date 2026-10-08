#include "monitor/core/memory.hpp"
#include "monitor/native/target.hpp"
#include <cstring>

namespace wechatbot::monitor {

/**
 * @brief 校验指定内存地址及长度是否可安全读取
 * @details 通过 Windows API VirtualQuery 检查内存页状态，要求内存页必须满足：
 *          1. 状态为 MEM_COMMIT (已提交物理内存)
 *          2. 页面保护属性不能包含 PAGE_GUARD (保护页) 或 PAGE_NOACCESS (禁止访问)
 *          3. 请求的读取区间不能跨越到下一个非法的内存区域
 * @param address 待检查的内存起始指针
 * @param length 待读取的字节长度
 * @return true 允许读取；false 指针非法或不可读
 */
bool IsReadableRange(const void* address, size_t length) {
    if (!address || length == 0)
        return false;
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(address, &info, sizeof(info)) != sizeof(info) ||
        info.State != MEM_COMMIT || (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
        return false;
    const auto start = reinterpret_cast<uintptr_t>(address);
    const auto regionStart = reinterpret_cast<uintptr_t>(info.BaseAddress);
    const auto regionEnd = regionStart + info.RegionSize;
    return start >= regionStart && start <= regionEnd && length <= regionEnd - start;
}

/**
 * @brief 安全读取并还原微信原生 UTF-8 字符串
 * @details 微信内部字符串对象采用 SSO (Small String Optimization, 小字符串优化) 内存布局：
 *          - +0x00 ~ +0x0F (16 字节): 若容量 < 16，直接在此处存放内联字符；若容量 >= 16，前 8 字节为堆指针
 *          - +0x10 (8 字节 uint64_t): 字符串有效字节长度 length
 *          - +0x18 (8 字节 uint64_t): 字符串分配容量 capacity
 *          
 *          安全防护要点：
 *          1. SEH 防护 (__try ... __except)：捕获一切访问违规，保证微信宿主绝不闪退。
 *          2. 边界回溯 (UTF-8 Boundary Rollback)：当截取受限缓冲区时，若截断落在多字节字符（如汉字3字节）中间，
 *             自动向前回溯并丢弃残缺字节，确保产出的字符串始终是合法 UTF-8，防止 JSON 解析报错。
 *          3. MultiByteToWideChar 校验：严格校验整个拷贝区域是否满足 UTF-8 规范。
 */
TextRead CopyNativeString(const void* object, char* destination, size_t destinationCapacity) {
    TextRead result{};
    result.status = ReadStatus::InvalidObject;
    if (!destination || destinationCapacity == 0)
        return result;
    destination[0] = 0;
    if (!IsReadableRange(object, 0x20))
        return result;
    __try {
        const auto* bytes = reinterpret_cast<const uint8_t*>(object);
        const uint64_t length = *reinterpret_cast<const uint64_t*>(bytes + 0x10);
        const uint64_t capacity = *reinterpret_cast<const uint64_t*>(bytes + 0x18);
        result.originalBytes = length;
        result.lengthKnown = true;
        // 合法性校验：容量不能小于长度、容量上限 64MB、内联容量与长度一致性
        if (capacity < length || capacity > 0x4000000 || (capacity < 0x10 && length > 15)) {
            result.status = ReadStatus::InvalidLayout;
            return result;
        }
        if (!length) {
            result.status = ReadStatus::Empty;
            return result;
        }
        // SSO 解引用：小字符串取内联首地址，长字符串解引用前 8 字节指针
        const char* text = reinterpret_cast<const char*>(bytes);
        if (capacity >= 0x10)
            text = *reinterpret_cast<char* const*>(bytes);
        size_t copied = length < destinationCapacity ? static_cast<size_t>(length) : destinationCapacity - 1;
        if (!IsReadableRange(text, copied)) {
            result.status = ReadStatus::Unreadable;
            return result;
        }
        memcpy(destination, text, copied);

        // UTF-8 汉字截断回溯处理：若快照截断发生在多字节字符中间，回退 lead 指针
        if (copied < length && copied) {
            size_t lead = copied - 1;
            while (lead > 0 && (static_cast<unsigned char>(destination[lead]) & 0xC0) == 0x80)
                --lead;
            const auto first = static_cast<unsigned char>(destination[lead]);
            const size_t width = first < 0x80 ? 1 : first >= 0xC2 && first <= 0xDF ? 2 :
                                 first >= 0xE0 && first <= 0xEF ? 3 :
                                 first >= 0xF0 && first <= 0xF4 ? 4 : 0;
            if (width && copied - lead < width)
                copied = lead; // 丢弃不完整的残缺字符字节
        }
        destination[copied] = 0;
        // 校验编码合法性
        if (copied && !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, destination,
                                          static_cast<int>(copied), nullptr, 0)) {
            destination[0] = 0;
            result.status = ReadStatus::InvalidUtf8;
            return result;
        }
        result.capturedBytes = static_cast<uint32_t>(copied);
        result.status = copied < length ? ReadStatus::Truncated : ReadStatus::Ok;
        return result;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        destination[0] = 0;
        result.capturedBytes = 0;
        result.status = ReadStatus::Exception;
        return result;
    }
}

TextRead CopyNativeBinary(const void* object, uint8_t* destination, size_t destinationCapacity) {
    TextRead result{};
    result.status = ReadStatus::InvalidObject;
    if (!destination || !destinationCapacity || !IsReadableRange(object, 0x20)) return result;
    __try {
        const auto* native = static_cast<const uint8_t*>(object);
        const auto length = *reinterpret_cast<const uint64_t*>(native + 0x10);
        const auto capacity = *reinterpret_cast<const uint64_t*>(native + 0x18);
        result.lengthKnown = true;
        result.originalBytes = length;
        if (length > capacity || capacity > 0x4000000 || (capacity < 16 && length > 15)) {
            result.status = ReadStatus::InvalidLayout;
            return result;
        }
        if (!length) { result.status = ReadStatus::Empty; return result; }
        const auto* payload = capacity < 16 ? native : *reinterpret_cast<const uint8_t* const*>(native);
        const size_t count = length < destinationCapacity ? static_cast<size_t>(length) : destinationCapacity;
        if (!IsReadableRange(payload, count)) { result.status = ReadStatus::Unreadable; return result; }
        memcpy(destination, payload, count);
        result.capturedBytes = static_cast<uint32_t>(count);
        result.status = count == length ? ReadStatus::Ok : ReadStatus::Truncated;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        result.status = ReadStatus::Exception;
        result.capturedBytes = 0;
    }
    return result;
}

TextRead CopyNativeWideString(const void* object, char* destination, size_t destinationCapacity) {
    TextRead result{};
    result.status = ReadStatus::InvalidObject;
    if (!destination || !destinationCapacity) return result;
    destination[0] = 0;
    if (!IsReadableRange(object, 0x20)) return result;
    __try {
        const auto* native = static_cast<const uint8_t*>(object);
        const auto length = *reinterpret_cast<const uint64_t*>(native + 0x10);
        const auto capacity = *reinterpret_cast<const uint64_t*>(native + 0x18);
        // This reader is for paths. Bound work as well as the output snapshot.
        if (length > capacity || capacity > 0x4000000 || length > 32768 ||
            (capacity < 8 && length > 7)) { result.status = ReadStatus::InvalidLayout; return result; }
        if (!length) { result.status = ReadStatus::Empty; result.lengthKnown = true; return result; }
        const auto* payload = capacity < 8 ? reinterpret_cast<const uint16_t*>(native)
                                         : *reinterpret_cast<const uint16_t* const*>(native);
        if (!IsReadableRange(payload, static_cast<size_t>(length) * 2)) {
            result.status = ReadStatus::Unreadable; return result;
        }
        bool truncated = false;
        uint64_t total = 0;
        size_t copied = 0;
        for (uint64_t i = 0; i < length; ++i) {
            uint32_t cp = payload[i];
            if (cp >= 0xD800 && cp <= 0xDBFF) {
                if (++i >= length || payload[i] < 0xDC00 || payload[i] > 0xDFFF) {
                    destination[0] = 0; result.status = ReadStatus::InvalidLayout; return result;
                }
                cp = 0x10000 + ((cp - 0xD800) << 10) + (payload[i] - 0xDC00);
            } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                destination[0] = 0; result.status = ReadStatus::InvalidLayout; return result;
            }
            char utf8[4]{};
            size_t width = 0;
            if (cp < 0x80) { utf8[0] = static_cast<char>(cp); width = 1; }
            else if (cp < 0x800) {
                utf8[0] = static_cast<char>(0xC0 | (cp >> 6));
                utf8[1] = static_cast<char>(0x80 | (cp & 63)); width = 2;
            } else if (cp < 0x10000) {
                utf8[0] = static_cast<char>(0xE0 | (cp >> 12));
                utf8[1] = static_cast<char>(0x80 | ((cp >> 6) & 63));
                utf8[2] = static_cast<char>(0x80 | (cp & 63)); width = 3;
            } else {
                utf8[0] = static_cast<char>(0xF0 | (cp >> 18));
                utf8[1] = static_cast<char>(0x80 | ((cp >> 12) & 63));
                utf8[2] = static_cast<char>(0x80 | ((cp >> 6) & 63));
                utf8[3] = static_cast<char>(0x80 | (cp & 63)); width = 4;
            }
            total += width;
            if (!truncated && width < destinationCapacity - copied) {
                memcpy(destination + copied, utf8, width); copied += width;
            } else truncated = true;
        }
        destination[copied] = 0;
        result.lengthKnown = true;
        result.originalBytes = total;
        result.capturedBytes = static_cast<uint32_t>(copied);
        result.status = truncated ? ReadStatus::Truncated : ReadStatus::Ok;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        destination[0] = 0;
        result.capturedBytes = 0;
        result.status = ReadStatus::Exception;
    }
    return result;
}

TextRead CopyMessageString(const uint8_t* item, size_t objectSize, size_t bitsOffset,
                          size_t offset, uint32_t bit, bool wrapped,
                          char* destination, size_t capacity) {
    TextRead result{};
    result.status = ReadStatus::InvalidObject;
    if (!item || !g_weixin || !IsReadableRange(item, objectSize))
        return result;
    __try {
        const uint32_t hasBits = *reinterpret_cast<const uint32_t*>(item + bitsOffset);
        if ((hasBits & bit) == 0) {
            result.status = ReadStatus::Missing;
            return result;
        }
        const auto wrapper = *reinterpret_cast<const uint8_t* const*>(item + offset);
        if (!wrapped)
            return CopyNativeString(wrapper, destination, capacity);
        if (!IsReadableRange(wrapper, 0x18))
            return result;
        const uintptr_t wrapperVtable = *reinterpret_cast<const uintptr_t*>(wrapper);
        if (wrapperVtable != reinterpret_cast<uintptr_t>(g_weixin) + kBuiltinStringVtableRva) {
            result.status = ReadStatus::InvalidWrapper;
            return result;
        }
        if ((*reinterpret_cast<const uint32_t*>(wrapper + 0x14) & 1U) == 0) {
            result.status = ReadStatus::InnerMissing;
            return result;
        }
        const auto nativeString = *reinterpret_cast<const void* const*>(wrapper + 8);
        return CopyNativeString(nativeString, destination, capacity);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        result.status = ReadStatus::Exception;
        return result;
    }
}

TextRead CopyField(const uint8_t* item, MessageLayout layout, StringField field,
                   char* destination, size_t capacity) {
    return CopyMessageString(item, layout.size, layout.hasBits, field.offset,
                             field.bit, field.wrapped, destination, capacity);
}
void CopyScalars(const uint8_t* item, uint32_t hasBits, const ScalarField* fields,
                 size_t count, uint64_t* output) {
    // Called inside the capture function's SEH boundary while the object is live.
    for (size_t i = 0; i < count; ++i) {
        const auto& field = fields[i];
        if (hasBits & field.bit)
            output[i] = field.wide ? *reinterpret_cast<const uint64_t*>(item + field.offset)
                                   : *reinterpret_cast<const uint32_t*>(item + field.offset);
    }
}

} // namespace wechatbot::monitor
