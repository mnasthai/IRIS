#include "monitor/media/media_candidate_file.hpp"
#include <algorithm>

namespace wechatbot::monitor::media_receive_detail {

uint64_t CandidateFileLength(const BY_HANDLE_FILE_INFORMATION& info) {
    return (uint64_t{info.nFileSizeHigh} << 32) | info.nFileSizeLow;
}

namespace {
/**
 * @brief 判断两个 Win32 文件信息结构是否指代同一个未被修改的物理文件
 * @details 严格比对磁盘卷序列号、NTFS 文件索引 ID (Inode 等价物)、文件尺寸及最后修改时间戳
 */
bool SameFile(const BY_HANDLE_FILE_INFORMATION& a, const BY_HANDLE_FILE_INFORMATION& b) {
    return a.dwVolumeSerialNumber == b.dwVolumeSerialNumber && a.nFileIndexHigh == b.nFileIndexHigh &&
        a.nFileIndexLow == b.nFileIndexLow && a.nFileSizeHigh == b.nFileSizeHigh && a.nFileSizeLow == b.nFileSizeLow &&
        a.ftLastWriteTime.dwHighDateTime == b.ftLastWriteTime.dwHighDateTime &&
        a.ftLastWriteTime.dwLowDateTime == b.ftLastWriteTime.dwLowDateTime;
}
} // namespace

/**
 * @brief 在 MinHook 回调发生的第一瞬间立即打开候选文件
 * @details 
 *   由于微信在接收图片时会先将临时文件写入缓存目录，若不立即获取句柄，
 *   在后续异步线程调度期间该文件可能被微信自身清理或重命名。
 *   因此在回调内部立即以只读模式打开，记录打开时的初始元数据以备后用。
 */
void OpenImageCandidate(const std::wstring& path, ImageCandidate& out) {
    out.path = path;
    out.handle.reset(CreateFileW(out.path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (!out.handle) { out.openError = GetLastError(); out.status = CandidateStatus::OpenFailed; return; }
    
    // 确保目标是普通磁盘文件，杜绝管道、设备或目录
    if (GetFileType(out.handle.get()) != FILE_TYPE_DISK ||
        !GetFileInformationByHandle(out.handle.get(), &out.initial) ||
        (out.initial.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) {
        out.status = CandidateStatus::NotRegularFile;
    } else if (!CandidateFileLength(out.initial) || CandidateFileLength(out.initial) > kMaxImageBytes) {
        out.status = CandidateStatus::SizeRejected;
    } else {
        out.status = CandidateStatus::Opened;
        return;
    }
    out.handle.reset();
}

/**
 * @brief 在工作线程中核验并读取已锁定的候选图片文件
 * @details 
 *   安全防护机制：
 *   1. 【读前核对】：重新获取当前句柄的文件信息并与 initial 进行 SameFile 比对；
 *   2. 【文件指针归零】：通过 SetFilePointerEx 确保从 0 偏移开始顺序读取；
 *   3. 【分块读取】：以最大 1MB 为步长分块读入内存；
 *   4. 【读后核对】：尝试多读 1 字节确保已到达真正的 EOF，并再次比对前后 SameFile，
 *      确保在读取全过程中文件内容没有受到外部任何写入干扰。
 */
CandidateReadResult ReadCandidate(ImageCandidate& candidate) {
    if (!candidate.handle) return {candidate.status};
    
    // 读前核对
    BY_HANDLE_FILE_INFORMATION before{}, after{};
    if (!GetFileInformationByHandle(candidate.handle.get(), &before) || !SameFile(candidate.initial, before))
        return {CandidateStatus::FileChanged};
    
    const auto length = CandidateFileLength(before);
    if (!length || length > kMaxImageBytes) return {CandidateStatus::SizeRejected};
    
    // 重设指针至文件开头
    LARGE_INTEGER start{};
    if (!SetFilePointerEx(candidate.handle.get(), start, nullptr, FILE_BEGIN)) return {CandidateStatus::SeekFailed};
    
    // 分块读取数据
    CandidateReadResult result{CandidateStatus::StableFileRead};
    result.bytes.resize(static_cast<size_t>(length));
    size_t done = 0;
    while (done < result.bytes.size()) {
        DWORD read = 0;
        const DWORD chunk = static_cast<DWORD>((std::min)(result.bytes.size() - done, size_t{1024 * 1024}));
        if (!ReadFile(candidate.handle.get(), result.bytes.data() + done, chunk, &read, nullptr) || !read)
            return {CandidateStatus::ReadFailed};
        done += read;
    }
    
    // 读后核对：验证已到达 EOF 且读期间文件未被篡改
    char extra = 0; DWORD extraLength = 0;
    if (!ReadFile(candidate.handle.get(), &extra, 1, &extraLength, nullptr) || extraLength ||
        !GetFileInformationByHandle(candidate.handle.get(), &after) || !SameFile(before, after))
        return {CandidateStatus::FileChanged};
    
    return result;
}

} // namespace wechatbot::monitor::media_receive_detail
