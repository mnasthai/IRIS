#include "monitor/media/media_inspection.hpp"
#include "monitor/media/media_receive_snapshot.hpp"
#include "monitor/media/media_file.hpp"
#include <wincodec.h>
#include <algorithm>
#include <cstring>
#include <type_traits>
#include <vector>
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

namespace wechatbot::monitor::media_receive_detail {

/**
 * @brief 检测并返回语音数据的标准扩展名 (.silk 或 .amr)
 */
const char* VoiceExtension(std::string_view bytes) {
    if (SilkDurationMs(bytes)) return ".silk";
    if (!bytes.starts_with("#!AMR\n") || bytes.size() > kMaxVoiceBytes) return nullptr;
    bytes.remove_prefix(6);
    // AMR-NB 编码中各帧类型的字节长度对照表
    constexpr uint8_t lengths[]{13, 14, 16, 18, 20, 21, 27, 32, 6, 0, 0, 0, 0, 0, 0, 1};
    unsigned frames = 0;
    while (!bytes.empty()) {
        const auto toc = static_cast<uint8_t>(bytes.front());
        const auto length = lengths[(toc >> 3) & 15];
        if ((toc & 0x83) || !length || length > bytes.size() || ++frames > 3000) return nullptr;
        bytes.remove_prefix(length);
    }
    return frames ? ".amr" : nullptr;
}

/**
 * @brief 基于文件头尾魔数快速识别常见图片格式（仅用于初步诊断）
 */
const char* ImageExtension(std::string_view bytes) {
    if (bytes.size() >= 45 && bytes.starts_with(std::string_view("\x89PNG\r\n\x1a\n", 8)) &&
        bytes.substr(12, 4) == "IHDR" && bytes.ends_with(std::string_view("\0\0\0\0IEND\xae\x42\x60\x82", 12))) return ".png";
    if (bytes.size() >= 4 && bytes.starts_with(std::string_view("\xff\xd8\xff", 3)) &&
        bytes.ends_with(std::string_view("\xff\xd9", 2))) return ".jpg";
    if (bytes.size() >= 16 && bytes.starts_with("wxgf")) return ".wxgf";
    return nullptr;
}

namespace {

/**
 * @brief Windows COM 组件接口指针的 RAII 封装
 */
template<class T> struct ComObject {
    T* value = nullptr;
    ComObject() = default;
    ComObject(const ComObject&) = delete;
    ComObject& operator=(const ComObject&) = delete;
    ~ComObject() { if (value) value->Release(); }
    T** Out() { return &value; }
    T* operator->() const { return value; }
};
static_assert(!std::is_copy_constructible_v<ComObject<IWICStream>>);

/**
 * @brief COM 套间 (Apartment) 生命周期管理 RAII 封装
 */
struct Apartment {
    explicit Apartment(bool initialized) noexcept : uninitialize(initialized) {}
    Apartment(const Apartment&) = delete;
    Apartment& operator=(const Apartment&) = delete;
    ~Apartment() { if (uninitialize) CoUninitialize(); }
    bool uninitialize;
};
static_assert(!std::is_copy_constructible_v<Apartment>);

/**
 * @brief 读取大端字节序 (Big Endian) 32 位整数
 */
uint32_t BigEndian(const char* value) {
    return (uint32_t{static_cast<uint8_t>(value[0])} << 24) |
        (uint32_t{static_cast<uint8_t>(value[1])} << 16) |
        (uint32_t{static_cast<uint8_t>(value[2])} << 8) | static_cast<uint8_t>(value[3]);
}

/**
 * @brief 严格全量解析并校验 PNG 文件结构与每一块的 CRC32 校验和
 * @details 确保文件未被截断且内部数据完整：
 *          - 第一块必须是 13 字节的 IHDR；
 *          - 必须包含 IDAT 图像数据块；
 *          - 最后一块必须是 0 字节的 IEND，且正好到达文件末尾。
 */
bool CompletePng(std::string_view bytes) {
    if (bytes.size() < 45) return false;
    size_t offset = 8; bool first = true, imageData = false;
    while (offset < bytes.size()) {
        if (bytes.size() - offset < 12) return false;
        const auto size = BigEndian(bytes.data() + offset);
        if (size > bytes.size() - offset - 12) return false;
        const auto type = bytes.substr(offset + 4, 4);
        if (first && (type != "IHDR" || size != 13)) return false;
        if (!first && type == "IHDR") return false;
        first = false;
        // 计算 PNG 块的 CRC32
        uint32_t crc = 0xffffffff;
        for (size_t i = offset + 4; i < offset + 8 + size; ++i) {
            crc ^= static_cast<uint8_t>(bytes[i]);
            for (unsigned bit = 0; bit < 8; ++bit)
                crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1U)));
        }
        if ((crc ^ 0xffffffffU) != BigEndian(bytes.data() + offset + 8 + size)) return false;
        offset += size + 12;
        if (type == "IDAT") imageData = true;
        if (type == "IEND") return imageData && size == 0 && offset == bytes.size();
    }
    return false;
}
} // namespace

/**
 * @brief 通过 COM WIC (Windows Imaging Component) 全像素解码校验图片真实完整性
 * @details 
 *   执行步骤：
 *   1. 初步魔数审查（拒绝非标准扩展名与超限大小）；
 *   2. 若为 PNG，先执行 CompletePng 进行严格分块 CRC 校验；
 *   3. 初始化 COM 多线程套间并实例化 CLSID_WICImagingFactory；
 *   4. 从内存创建 IWICStream 并挂载 IWICBitmapDecoder；
 *   5. 检查容器格式是否为标准的 PNG 或 JPEG，且仅包含 1 个图帧；
 *   6. 提取像素宽高，限制分辨率不超过 16384x16384 且总像素不超过 1 亿；
 *   7. 初始化格式转换器为 32bppRGBA 像素格式；
 *   8. 【全像素条带解码】：按每次 32 行步长分块调用 CopyPixels，
 *      遍历解码整张图的所有像素。只有当所有数据块均能正确解压解码时，才判定为完全有效！
 */
std::optional<ImageInspection> InspectImage(std::string_view bytes) {
    const char* extension = ImageExtension(bytes);
    if (!extension || strcmp(extension, ".wxgf") == 0 || bytes.size() > kMaxPublishedImageBytes ||
        (strcmp(extension, ".png") == 0 && !CompletePng(bytes))) return {};
    
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE) return {};
    Apartment apartment{SUCCEEDED(initialized)};
    
    ComObject<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(factory.Out())))) return {};
    
    ComObject<IWICStream> stream;
    ComObject<IWICBitmapDecoder> decoder;
    ComObject<IWICBitmapFrameDecode> frame;
    ComObject<IWICFormatConverter> converter;
    GUID container{};
    UINT count = 0, width = 0, height = 0;
    
    if (FAILED(factory->CreateStream(stream.Out())) ||
        FAILED(stream->InitializeFromMemory(reinterpret_cast<BYTE*>(const_cast<char*>(bytes.data())),
                                             static_cast<DWORD>(bytes.size()))) ||
        FAILED(factory->CreateDecoderFromStream(stream.value, nullptr, WICDecodeMetadataCacheOnLoad, decoder.Out())) ||
        FAILED(decoder->GetContainerFormat(&container)) ||
        (container != GUID_ContainerFormatPng && container != GUID_ContainerFormatJpeg) ||
        FAILED(decoder->GetFrameCount(&count)) || count != 1 ||
        FAILED(decoder->GetFrame(0, frame.Out())) || FAILED(frame->GetSize(&width, &height)) ||
        !width || !height || width > 16384 || height > 16384 || uint64_t{width} * height > 100000000 ||
        FAILED(factory->CreateFormatConverter(converter.Out())) ||
        FAILED(converter->Initialize(frame.value, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone,
                                      nullptr, 0, WICBitmapPaletteTypeCustom))) return {};
    
    if ((container == GUID_ContainerFormatPng) != (strcmp(extension, ".png") == 0)) return {};
    
    // 按 32 行为单位进行全像素条带解码，确保文件没有任何截断损坏
    const UINT stride = width * 4;
    std::vector<BYTE> pixels(static_cast<size_t>(stride) * (std::min)(height, UINT{32}));
    for (UINT row = 0; row < height;) {
        const UINT rows = (std::min)(height - row, UINT{32});
        WICRect rectangle{0, static_cast<INT>(row), static_cast<INT>(width), static_cast<INT>(rows)};
        if (FAILED(converter->CopyPixels(&rectangle, stride, stride * rows, pixels.data()))) return {};
        row += rows;
    }
    return ImageInspection{extension, width, height};
}

/**
 * @brief 解析微信私有 WXGF 动画表情/贴纸文件头
 */
std::optional<ImageInspection> InspectWxgfHeader(std::string_view bytes) {
    if (bytes.size() < 11 || bytes.size() > kMaxPublishedImageBytes || !bytes.starts_with("wxgf")) return {};
    const auto word = [&](size_t offset) {
        return (uint32_t{static_cast<uint8_t>(bytes[offset])} << 8) | static_cast<uint8_t>(bytes[offset + 1]);
    };
    const uint8_t headerLength = static_cast<uint8_t>(bytes[4]);
    const uint32_t version = word(5), width = word(7), height = word(9);
    if (headerLength < 11 || headerLength > bytes.size() || bytes.size() == headerLength ||
        (version != 1 && version != 2) || !width || !height ||
        width > 4096 || height > 4096 || uint64_t{width} * height > 16000000) return {};
    return ImageInspection{".wxgf", width, height};
}

} // namespace wechatbot::monitor::media_receive_detail
