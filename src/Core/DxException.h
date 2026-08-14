#pragma once

#include <Windows.h>

#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace Shadow::Core
{
inline std::string DescribeHResult(const HRESULT result)
{
    // FORMAT_MESSAGE_ALLOCATE_BUFFER 会把缓冲区的所有权交给调用方，
    // 因此成功取得错误信息后必须使用 LocalFree 释放它。
    char* messageBuffer = nullptr;
    const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                        FORMAT_MESSAGE_IGNORE_INSERTS;
    const DWORD length = FormatMessageA(
        flags,
        nullptr,
        static_cast<DWORD>(result),
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<char*>(&messageBuffer),
        0,
        nullptr);

    std::string message;
    if (length != 0 && messageBuffer != nullptr)
    {
        message.assign(messageBuffer, length);
        LocalFree(messageBuffer);
    }

    return message;
}

inline void ThrowIfFailed(const HRESULT result, const std::string_view operation)
{
    if (SUCCEEDED(result))
    {
        return;
    }

    // 同时保留 HRESULT 数值和 Windows 提供的可读文本：前者便于搜索文档，
    // 后者便于在本地直接判断失败原因。
    std::ostringstream description;
    description << operation << " failed with HRESULT 0x" << std::hex << std::uppercase
                << static_cast<unsigned long>(result);

    const std::string systemMessage = DescribeHResult(result);
    if (!systemMessage.empty())
    {
        description << ": " << systemMessage;
    }

    throw std::runtime_error(description.str());
}
} // namespace Shadow::Core
