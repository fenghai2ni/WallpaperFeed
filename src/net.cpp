#include "net.h"

#include <winhttp.h>
#include <vector>
#include <cstdio>

#pragma comment(lib, "winhttp.lib")

namespace {

const wchar_t* kUserAgent =
    L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
    L"(KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36";

struct UrlParts {
    std::wstring host;
    std::wstring path;
    int port = 443;
    bool https = true;
};

bool Crack(const std::wstring& url, UrlParts& out, std::wstring& err) {
    wchar_t host[256] = {};
    wchar_t path[4096] = {};
    wchar_t extra[2048] = {};
    wchar_t scheme[32] = {};

    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    uc.lpszScheme = scheme;
    uc.dwSchemeLength = 31;
    uc.lpszHostName = host;
    uc.dwHostNameLength = 255;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 4095;
    uc.lpszExtraInfo = extra;
    uc.dwExtraInfoLength = 2047;

    if (!WinHttpCrackUrl(url.c_str(), (DWORD)url.size(), 0, &uc)) {
        err = L"URL 解析失败";
        return false;
    }

    out.host = host;
    out.path = path;
    if (extra[0]) out.path += extra;
    out.port = (int)uc.nPort;
    out.https = (uc.nScheme == INTERNET_SCHEME_HTTPS);
    return true;
}

// 每个线程复用一条 WinHTTP 会话 + 连接：同一线程连续请求不再重复 TCP/TLS 握手，
// 换壁纸时（列表 + 1~3 张下载）能省掉好几次握手，列表/缩略图线程收益更大。
struct SessionCache {
    HINTERNET session = nullptr;
    HINTERNET conn = nullptr;
    std::wstring host;
    int port = 0;

    ~SessionCache() {
        if (conn) WinHttpCloseHandle(conn);
        if (session) WinHttpCloseHandle(session);
    }
};

thread_local SessionCache tls;

bool EnsureSession(std::wstring& err) {
    if (tls.session) return true;
    tls.session = WinHttpOpen(kUserAgent, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                              WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!tls.session) {
        tls.session = WinHttpOpen(kUserAgent, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    }
    if (!tls.session) {
        err = L"初始化网络会话失败";
        return false;
    }
    // 连接 6s / 发送 8s / 接收 20s：站点响应慢时尽快失败换下一张，而不是一次挂 1 分钟
    WinHttpSetTimeouts(tls.session, 6000, 8000, 20000, 20000);
    return true;
}

bool EnsureConn(const UrlParts& up, std::wstring& err) {
    if (tls.conn && tls.host == up.host && tls.port == up.port) return true;
    if (tls.conn) {
        WinHttpCloseHandle(tls.conn);
        tls.conn = nullptr;
    }
    tls.conn = WinHttpConnect(tls.session, up.host.c_str(), (INTERNET_PORT)up.port, 0);
    if (!tls.conn) {
        err = L"连接服务器失败";
        return false;
    }
    tls.host = up.host;
    tls.port = up.port;
    return true;
}

bool DoRequest(const std::wstring& url, const std::wstring& referer,
               const std::string* formBody, const std::wstring& outPath,
               net::Response& res, std::wstring& err) {

    UrlParts up;
    if (!Crack(url, up, err)) return false;

    if (!EnsureSession(err)) return false;
    if (!EnsureConn(up, err)) return false;

    HINTERNET req = WinHttpOpenRequest(tls.conn, formBody ? L"POST" : L"GET", up.path.c_str(),
                                       nullptr, WINHTTP_NO_REFERER,
                                       WINHTTP_DEFAULT_ACCEPT_TYPES,
                                       up.https ? WINHTTP_FLAG_SECURE : 0);
    if (!req) {
        err = L"创建请求失败";
        // 连接可能已失效，下次重建
        if (tls.conn) { WinHttpCloseHandle(tls.conn); tls.conn = nullptr; }
        return false;
    }

    DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
    WinHttpSetOption(req, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy));

    WinHttpAddRequestHeaders(req,
        L"Accept: text/html,application/xhtml+xml,application/xml;q=0.9,"
        L"image/avif,image/webp,image/apng,*/*;q=0.8\r\n"
        L"Accept-Language: zh-CN,zh;q=0.9,en;q=0.8\r\n",
        (DWORD)-1L, WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);

    if (!referer.empty()) {
        std::wstring refHeader = L"Referer: " + referer;
        WinHttpAddRequestHeaders(req, refHeader.c_str(), (DWORD)-1L, WINHTTP_ADDREQ_FLAG_ADD);
    }
    if (formBody) {
        WinHttpAddRequestHeaders(req, L"Content-Type: application/x-www-form-urlencoded",
                                 (DWORD)-1L, WINHTTP_ADDREQ_FLAG_ADD);
    }

    BOOL sent = WinHttpSendRequest(
        req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
        formBody ? (LPVOID)formBody->data() : WINHTTP_NO_REQUEST_DATA,
        formBody ? (DWORD)formBody->size() : 0,
        formBody ? (DWORD)formBody->size() : 0, 0);

    if (!sent || !WinHttpReceiveResponse(req, nullptr)) {
        err = L"请求失败（网络不可达或超时）";
        WinHttpCloseHandle(req);
        // 这条连接可能已经坏了，丢掉让下次重建
        if (tls.conn) { WinHttpCloseHandle(tls.conn); tls.conn = nullptr; }
        return false;
    }

    DWORD statusCode = 0;
    DWORD len = sizeof(statusCode);
    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &len, WINHTTP_NO_HEADER_INDEX);
    res.status = (int)statusCode;

    wchar_t ctype[512] = {};
    len = sizeof(ctype) - sizeof(wchar_t);
    if (WinHttpQueryHeaders(req, WINHTTP_QUERY_CONTENT_TYPE, WINHTTP_HEADER_NAME_BY_INDEX,
                            ctype, &len, WINHTTP_NO_HEADER_INDEX)) {
        res.contentType = WideToUtf8(ctype);
    }

    HANDLE file = INVALID_HANDLE_VALUE;
    if (!outPath.empty() && res.status == 200) {
        file = CreateFileW(outPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            err = L"无法写入临时文件";
            WinHttpCloseHandle(req);
            return false;
        }
    }

    uint64_t total = 0;
    bool ok = true;
    while (true) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(req, &avail)) { ok = false; break; }
        if (avail == 0) break;

        std::vector<char> buf(avail);
        DWORD read = 0;
        if (!WinHttpReadData(req, buf.data(), avail, &read)) { ok = false; break; }
        if (read == 0) break;
        total += read;

        if (file != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            if (!WriteFile(file, buf.data(), read, &written, nullptr) || written != read) {
                ok = false;
                break;
            }
        } else {
            res.body.append(buf.data(), read);
        }
    }

    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);

    WinHttpCloseHandle(req);

    if (!ok) {
        err = L"数据读取中断";
        if (file != INVALID_HANDLE_VALUE) DeleteFileW(outPath.c_str());
        return false;
    }
    if (res.status != 200) {
        wchar_t msg[96];
        swprintf_s(msg, L"服务器返回 HTTP %d", res.status);
        err = msg;
        if (file != INVALID_HANDLE_VALUE) DeleteFileW(outPath.c_str());
        return false;
    }
    return true;
}

} // namespace

namespace net {

bool GetText(const std::wstring& url, Response& res, std::wstring& err) {
    res = Response();
    return DoRequest(url, L"", nullptr, L"", res, err);
}

bool FetchToFile(const std::wstring& url, const std::wstring& referer,
                 const std::string* formBody, const std::wstring& outPath,
                 Response& res, std::wstring& err) {
    res = Response();
    return DoRequest(url, referer, formBody, outPath, res, err);
}

} // namespace net
