#pragma once
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace wechatbot::monitor::media_receive_detail {

/// 允许发布的最大图片大小限制 (20MB)
inline constexpr size_t kMaxPublishedImageBytes = 20 * 1024 * 1024;

/**
 * @brief 图片元数据检验结果
 */
struct ImageInspection {
    const char* extension = nullptr; ///< 规范化扩展名 (如 ".png", ".jpg", ".wxgf")
    uint32_t width = 0;              ///< 图片宽度 (像素)
    uint32_t height = 0;             ///< 图片高度 (像素)
};

/**
 * @brief 检测并返回语音数据的标准扩展名
 * @details 检查 SILK_V3 帧头或 AMR 帧头，有效则返回 ".silk" 或 ".amr"；无效返回 nullptr
 */
const char* VoiceExtension(std::string_view bytes);

/**
 * @brief 快速检测图片数据的可能格式扩展名
 * @details 基于文件头魔数做初筛识别（".png", ".jpg", ".wxgf"），仅用于诊断，不保证图片无损完整
 */
const char* ImageExtension(std::string_view bytes);

/**
 * @brief 深度解码检验图片完整性并提取宽高
 * @details 
 *   通过 Windows Imaging Component (COM WIC) 真实解码图片所有像素为 32bppRGBA。
 *   - 拦截截断 (Truncated)、损坏、或加密的 .dat 占位文件；
 *   - 仅当整张图片能无错逐行解码完毕时才判定合法并返回宽高与后缀。
 * @return 检验成功返回 ImageInspection；解码失败或残缺返回 std::nullopt
 */
std::optional<ImageInspection> InspectImage(std::string_view bytes);

/**
 * @brief 检验微信私有 WXGF 动画表情/贴纸文件头
 * @details 微信自定义的动画表情格式以 "wxgf" 魔数开头，本函数解析其头长度、版本与宽高尺寸
 * @return 检验成功返回 ImageInspection (后缀为 ".wxgf")；头损坏返回 std::nullopt
 */
std::optional<ImageInspection> InspectWxgfHeader(std::string_view bytes);

} // namespace wechatbot::monitor::media_receive_detail
