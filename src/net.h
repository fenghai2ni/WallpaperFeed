#pragma once
#include "common.h"
#include <string>

namespace net {

struct Response {
    int status = 0;
    std::string body;          // 仅 GetText 时填充
    std::string contentType;
};

// GET 文本内容（跟随重定向）
bool GetText(const std::wstring& url, Response& res, std::wstring& err);

// GET / POST 表单下载到文件。outPath 为 .part 临时文件，成功后替换。
bool FetchToFile(const std::wstring& url, const std::wstring& referer,
                 const std::string* formBody, const std::wstring& outPath,
                 Response& res, std::wstring& err);

} // namespace net
