// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <cctype>
#include <cstring>
#include <future>
#include <map>
#include <mutex>
#include <string>

#include <httplib.h>

#include "common/logging/log.h"
#include "common/types.h"
#include "core/libraries/network/ssl.h"

namespace Core::Loader {
class SymbolsResolver;
}

namespace Libraries::Http {

struct OrbisHttpUriElement {
    bool opaque;
    char* scheme;
    char* username;
    char* password;
    char* hostname;
    char* path;
    char* query;
    char* fragment;
    u16 port;
    u8 reserved[10];
};

enum OrbisHttpRequestMethod : s32 {
    ORBIS_INTERNAL_HTTP_REQUEST_METHOD_GET = 0,
    ORBIS_INTERNAL_HTTP_REQUEST_METHOD_POST = 1,
    ORBIS_INTERNAL_HTTP_REQUEST_METHOD_HEAD = 2,
    ORBIS_INTERNAL_HTTP_REQUEST_METHOD_OPTIONS = 3,
    ORBIS_INTERNAL_HTTP_REQUEST_METHOD_PUT = 4,
    ORBIS_INTERNAL_HTTP_REQUEST_METHOD_DELETE = 5,
    ORBIS_INTERNAL_HTTP_REQUEST_METHOD_TRACE = 6,
    ORBIS_INTERNAL_HTTP_REQUEST_METHOD_CONNECT = 7,
    ORBIS_INTERNAL_HTTP_REQUEST_METHOD_INVALID = 8,
};

class RequestTemplate {
public:
    int id{};
    std::map<std::string, std::string> headers;
    std::string user_agent{};
    bool is_async = false;

    RequestTemplate() = default;
    explicit RequestTemplate(int tmpl_id, std::string user_agent_value = {})
        : id(tmpl_id), user_agent(std::move(user_agent_value)) {}

    void AddHeader(const char* name, const char* value) {
        headers[std::string(name)] = std::string(value);
    }
};

class RequestObj {
public:
    int id{};
    RequestTemplate* req_template = nullptr;

    RequestObj() = default;
    explicit RequestObj(s32 req_id, RequestTemplate* req_template_value, s32 method_value,
                        std::string url_value, u64 content_length_value)
        : id(req_id), req_template(req_template_value),
          method(static_cast<OrbisHttpRequestMethod>(method_value)),
          content_length(content_length_value) {
        SetUrl(url_value);
    }

    RequestObj(const RequestObj&) = delete;
    RequestObj& operator=(const RequestObj&) = delete;

    RequestObj(RequestObj&& other) noexcept
        : id(other.id), req_template(other.req_template), request_future(std::move(other.request_future)),
          result_body(other.result_body), current_result_read_chunk_index(other.current_result_read_chunk_index),
          result_body_size(other.result_body_size), method(other.method), host(std::move(other.host)),
          path(std::move(other.path)), content_length(other.content_length), status_code(other.status_code),
          is_sent(other.is_sent), req_headers(std::move(other.req_headers)), url(std::move(other.url)),
          post_data(other.post_data), post_data_size(other.post_data_size) {
        other.result_body = nullptr;
        other.post_data = nullptr;
        other.result_body_size = 0;
        other.post_data_size = 0;
        other.current_result_read_chunk_index = 0;
        other.status_code = static_cast<u32>(-1);
        other.is_sent = false;
    }

    RequestObj& operator=(RequestObj&& other) noexcept {
        if (this == &other) {
            return *this;
        }

        delete[] result_body;
        delete[] post_data;

        id = other.id;
        req_template = other.req_template;
        request_future = std::move(other.request_future);
        result_body = other.result_body;
        current_result_read_chunk_index = other.current_result_read_chunk_index;
        result_body_size = other.result_body_size;
        method = other.method;
        host = std::move(other.host);
        path = std::move(other.path);
        content_length = other.content_length;
        status_code = other.status_code;
        is_sent = other.is_sent;
        req_headers = std::move(other.req_headers);
        url = std::move(other.url);
        post_data = other.post_data;
        post_data_size = other.post_data_size;

        other.result_body = nullptr;
        other.post_data = nullptr;
        other.result_body_size = 0;
        other.post_data_size = 0;
        other.current_result_read_chunk_index = 0;
        other.status_code = static_cast<u32>(-1);
        other.is_sent = false;
        return *this;
    }

    ~RequestObj() {
        delete[] result_body;
        delete[] post_data;
    }

    void SendRequest() {
        request_future = std::async(std::launch::async, [this] { _SendRequest(); });
        if (!req_template->is_async) {
            WaitForRequest();
        }
    }

    void WaitForRequest() {
        if (request_future.valid()) {
            request_future.get();
        }
    }

    u32 ReadData(char* dest, u32 size) {
        if (result_body == nullptr || dest == nullptr || size == 0 || result_body_size == 0) {
            return 0;
        }

        const u64 start_index = static_cast<u64>(current_result_read_chunk_index) * size;
        if (start_index >= result_body_size) {
            return 0;
        }

        const u64 remaining_bytes = result_body_size - start_index;
        const u64 bytes_to_copy = std::min<u64>(remaining_bytes, size);
        std::memcpy(dest, result_body + start_index, bytes_to_copy);
        current_result_read_chunk_index++;
        return static_cast<u32>(bytes_to_copy);
    }

    void SetUrl(const std::string& new_url) {
        if (new_url.empty()) {
            return;
        }

        url = new_url;
        const u64 scheme_end = url.find("://");
        const u64 path_start = url.find('/', scheme_end == std::string::npos ? 0 : scheme_end + 3);
        if (path_start == std::string::npos) {
            host = url;
            path = "/";
        } else {
            host = url.substr(0, path_start);
            path = url.substr(path_start);
        }
    }

    void SetPostData(const void* data, u64 size) {
        delete[] post_data;
        post_data = nullptr;
        post_data_size = 0;

        if (data == nullptr || size == 0) {
            return;
        }

        post_data_size = size;
        post_data = new u8[post_data_size];
        std::memcpy(post_data, data, post_data_size);
    }

    u32 GetStatusCode() const {
        return status_code;
    }

    u64 GetContentLength() const {
        return static_cast<u64>(result_body_size);
    }

    bool IsSent() const {
        return is_sent;
    }

    bool IsCompleted() const {
        return status_code != static_cast<u32>(-1);
    }

private:
    std::future<void> request_future{};
    char* result_body = nullptr;
    u32 current_result_read_chunk_index = 0;
    u32 result_body_size = 0;
    OrbisHttpRequestMethod method = ORBIS_INTERNAL_HTTP_REQUEST_METHOD_INVALID;
    std::string host{};
    std::string path{};
    u64 content_length = 0;
    u32 status_code = static_cast<u32>(-1);
    bool is_sent = false;
    std::map<std::string, std::string> req_headers;
    std::string url{};
    u8* post_data = nullptr;
    u64 post_data_size = 0;

    void _SendRequest() {
        httplib::Client cli(host);
        httplib::Headers headers;

        for (const auto& [key, value] : req_template->headers) {
            headers.emplace(key, value);
        }
        for (const auto& [key, value] : req_headers) {
            headers.emplace(key, value);
        }

        std::string content_type = "application/json";
        if (const auto it = headers.find("Content-Type"); it != headers.end()) {
            content_type = it->second;
        }

        is_sent = true;
        httplib::Result response;
        switch (method) {
        case ORBIS_INTERNAL_HTTP_REQUEST_METHOD_GET:
            response = cli.Get(path, headers);
            break;
        case ORBIS_INTERNAL_HTTP_REQUEST_METHOD_POST:
            response = cli.Post(path, headers, reinterpret_cast<const char*>(post_data),
                                post_data_size, content_type);
            break;
        case ORBIS_INTERNAL_HTTP_REQUEST_METHOD_HEAD:
            response = cli.Head(path, headers);
            break;
        case ORBIS_INTERNAL_HTTP_REQUEST_METHOD_OPTIONS:
            response = cli.Options(path, headers);
            break;
        case ORBIS_INTERNAL_HTTP_REQUEST_METHOD_PUT:
            response = cli.Put(path, headers, reinterpret_cast<const char*>(post_data),
                               post_data_size, content_type);
            break;
        case ORBIS_INTERNAL_HTTP_REQUEST_METHOD_DELETE:
            response = cli.Delete(path, headers);
            break;
        case ORBIS_INTERNAL_HTTP_REQUEST_METHOD_TRACE:
            LOG_ERROR(Lib_Http, "TRACE HTTP method not implemented");
            return;
        case ORBIS_INTERNAL_HTTP_REQUEST_METHOD_CONNECT:
            LOG_ERROR(Lib_Http, "CONNECT HTTP method not implemented");
            return;
        default:
            LOG_ERROR(Lib_Http, "Invalid HTTP method");
            return;
        }

        if (!response) {
            return;
        }

        status_code = response->status;
        if (response->status / 100 == 2) {
            result_body_size = static_cast<u32>(response->body.size());
            result_body = new char[result_body_size];
            std::memcpy(result_body, response->body.data(), result_body_size);
        }
    }
};

struct HttpRequestInternal {
    int state;          // +0x20
    int errorCode;      // +0x28
    int httpStatusCode; // +0x20C
    std::mutex m_mutex;
};
using OrbisHttpsCaList = Libraries::Ssl::OrbisSslCaList;

int PS4_SYSV_ABI sceHttpAbortRequest();
int PS4_SYSV_ABI sceHttpAbortRequestForce();
int PS4_SYSV_ABI sceHttpAbortWaitRequest();
int PS4_SYSV_ABI sceHttpAddCookie();
int PS4_SYSV_ABI sceHttpAddQuery();
int PS4_SYSV_ABI sceHttpAddRequestHeader(int id, const char* name, const char* value, s32 mode);
int PS4_SYSV_ABI sceHttpAddRequestHeaderRaw();
int PS4_SYSV_ABI sceHttpAuthCacheExport();
int PS4_SYSV_ABI sceHttpAuthCacheFlush();
int PS4_SYSV_ABI sceHttpAuthCacheImport();
int PS4_SYSV_ABI sceHttpCacheRedirectedConnectionEnabled();
int PS4_SYSV_ABI sceHttpCookieExport();
int PS4_SYSV_ABI sceHttpCookieFlush();
int PS4_SYSV_ABI sceHttpCookieImport();
int PS4_SYSV_ABI sceHttpCreateConnection();
int PS4_SYSV_ABI sceHttpCreateConnectionWithURL(int tmplId, const char* url, bool enableKeepalive);
int PS4_SYSV_ABI sceHttpCreateEpoll();
int PS4_SYSV_ABI sceHttpCreateRequest();
int PS4_SYSV_ABI sceHttpCreateRequest2();
int PS4_SYSV_ABI sceHttpCreateRequestWithURL(int connId, s32 method, const char* url,
                                             u64 contentLength);
int PS4_SYSV_ABI sceHttpCreateRequestWithURL2();
int PS4_SYSV_ABI sceHttpCreateTemplate(s32 conn_id, const char* user_agent, s32 http_v, s32 flags);
int PS4_SYSV_ABI sceHttpDbgEnableProfile();
int PS4_SYSV_ABI sceHttpDbgGetConnectionStat();
int PS4_SYSV_ABI sceHttpDbgGetRequestStat();
int PS4_SYSV_ABI sceHttpDbgSetPrintf();
int PS4_SYSV_ABI sceHttpDbgShowConnectionStat();
int PS4_SYSV_ABI sceHttpDbgShowMemoryPoolStat();
int PS4_SYSV_ABI sceHttpDbgShowRequestStat();
int PS4_SYSV_ABI sceHttpDbgShowStat();
int PS4_SYSV_ABI sceHttpDeleteConnection();
int PS4_SYSV_ABI sceHttpDeleteRequest(s32 req_id);
int PS4_SYSV_ABI sceHttpDeleteTemplate();
int PS4_SYSV_ABI sceHttpDestroyEpoll();
int PS4_SYSV_ABI sceHttpGetAcceptEncodingGZIPEnabled();
int PS4_SYSV_ABI sceHttpGetAllResponseHeaders(int reqId, char** header, u64* headerSize);
int PS4_SYSV_ABI sceHttpGetAuthEnabled();
int PS4_SYSV_ABI sceHttpGetAutoRedirect();
int PS4_SYSV_ABI sceHttpGetConnectionStat();
int PS4_SYSV_ABI sceHttpGetCookie();
int PS4_SYSV_ABI sceHttpGetCookieEnabled();
int PS4_SYSV_ABI sceHttpGetCookieStats();
int PS4_SYSV_ABI sceHttpGetEpoll();
int PS4_SYSV_ABI sceHttpGetEpollId();
int PS4_SYSV_ABI sceHttpGetLastErrno();
int PS4_SYSV_ABI sceHttpGetMemoryPoolStats();
int PS4_SYSV_ABI sceHttpGetNonblock();
int PS4_SYSV_ABI sceHttpGetRegisteredCtxIds();
int PS4_SYSV_ABI sceHttpGetResponseContentLength(u32 req_id, u64* out_content_length, u32* flag);
int PS4_SYSV_ABI sceHttpGetStatusCode(int reqId, int* statusCode);
int PS4_SYSV_ABI sceHttpInit(int libnetMemId, int libsslCtxId, u64 poolSize);
int PS4_SYSV_ABI sceHttpParseResponseHeader(const char* header, u64 headerLen, const char* fieldStr,
                                            const char** fieldValue, u64* valueLen);
int PS4_SYSV_ABI sceHttpParseStatusLine(const char* statusLine, u64 lineLen, int32_t* httpMajorVer,
                                        int32_t* httpMinorVer, int32_t* responseCode,
                                        const char** reasonPhrase, u64* phraseLen);
int PS4_SYSV_ABI sceHttpReadData(s32 reqId, void* data, u64 size);
int PS4_SYSV_ABI sceHttpRedirectCacheFlush();
int PS4_SYSV_ABI sceHttpRemoveRequestHeader();
int PS4_SYSV_ABI sceHttpRequestGetAllHeaders();
int PS4_SYSV_ABI sceHttpsDisableOption();
int PS4_SYSV_ABI sceHttpsDisableOptionPrivate();
int PS4_SYSV_ABI sceHttpsEnableOption(u32 options);
int PS4_SYSV_ABI sceHttpsEnableOptionPrivate();
int PS4_SYSV_ABI sceHttpSendRequest(int reqId, const void* postData, u64 size);
int PS4_SYSV_ABI sceHttpSetAcceptEncodingGZIPEnabled();
int PS4_SYSV_ABI sceHttpSetAuthEnabled();
int PS4_SYSV_ABI sceHttpSetAuthInfoCallback();
int PS4_SYSV_ABI sceHttpSetAutoRedirect();
int PS4_SYSV_ABI sceHttpSetChunkedTransferEnabled();
int PS4_SYSV_ABI sceHttpSetConnectTimeOut();
int PS4_SYSV_ABI sceHttpSetCookieEnabled();
int PS4_SYSV_ABI sceHttpSetCookieMaxNum();
int PS4_SYSV_ABI sceHttpSetCookieMaxNumPerDomain();
int PS4_SYSV_ABI sceHttpSetCookieMaxSize();
int PS4_SYSV_ABI sceHttpSetCookieRecvCallback();
int PS4_SYSV_ABI sceHttpSetCookieSendCallback();
int PS4_SYSV_ABI sceHttpSetCookieTotalMaxSize();
int PS4_SYSV_ABI sceHttpSetDefaultAcceptEncodingGZIPEnabled();
int PS4_SYSV_ABI sceHttpSetDelayBuildRequestEnabled();
int PS4_SYSV_ABI sceHttpSetEpoll();
int PS4_SYSV_ABI sceHttpSetEpollId();
int PS4_SYSV_ABI sceHttpSetHttp09Enabled();
int PS4_SYSV_ABI sceHttpSetInflateGZIPEnabled();
int PS4_SYSV_ABI sceHttpSetNonblock(s32 tmpl_id, bool enable);
int PS4_SYSV_ABI sceHttpSetPolicyOption();
int PS4_SYSV_ABI sceHttpSetPriorityOption();
int PS4_SYSV_ABI sceHttpSetProxy();
int PS4_SYSV_ABI sceHttpSetRecvBlockSize();
int PS4_SYSV_ABI sceHttpSetRecvTimeOut();
int PS4_SYSV_ABI sceHttpSetRedirectCallback();
int PS4_SYSV_ABI sceHttpSetRequestContentLength();
int PS4_SYSV_ABI sceHttpSetRequestStatusCallback();
int PS4_SYSV_ABI sceHttpSetResolveRetry();
int PS4_SYSV_ABI sceHttpSetResolveTimeOut();
int PS4_SYSV_ABI sceHttpSetResponseHeaderMaxSize();
int PS4_SYSV_ABI sceHttpSetSendTimeOut();
int PS4_SYSV_ABI sceHttpSetSocketCreationCallback();
int PS4_SYSV_ABI sceHttpsFreeCaList();
int PS4_SYSV_ABI sceHttpsGetCaList(int httpCtxId, OrbisHttpsCaList* list);
int PS4_SYSV_ABI sceHttpsGetSslError();
int PS4_SYSV_ABI sceHttpsLoadCert();
int PS4_SYSV_ABI sceHttpsSetMinSslVersion();
int PS4_SYSV_ABI sceHttpsSetSslCallback();
int PS4_SYSV_ABI sceHttpsSetSslVersion();
int PS4_SYSV_ABI sceHttpsUnloadCert();
int PS4_SYSV_ABI sceHttpTerm();
int PS4_SYSV_ABI sceHttpTryGetNonblock();
int PS4_SYSV_ABI sceHttpTrySetNonblock();
int PS4_SYSV_ABI sceHttpUnsetEpoll();
int PS4_SYSV_ABI sceHttpUriBuild(char* out, u64* require, u64 prepare,
                                 const OrbisHttpUriElement* srcElement, u32 option);
int PS4_SYSV_ABI sceHttpUriCopy();
int PS4_SYSV_ABI sceHttpUriEscape(char* out, u64* require, u64 prepare, const char* in);
int PS4_SYSV_ABI sceHttpUriMerge(char* mergedUrl, char* url, char* relativeUri, u64* require,
                                 u64 prepare, u32 option);
int PS4_SYSV_ABI sceHttpUriParse(OrbisHttpUriElement* out, const char* srcUri, void* pool,
                                 u64* require, u64 prepare);
int PS4_SYSV_ABI sceHttpUriSweepPath(char* dst, const char* src, u64 srcSize);
int PS4_SYSV_ABI sceHttpUriUnescape(char* out, u64* require, u64 prepare, const char* in);
int PS4_SYSV_ABI sceHttpWaitRequest();

void RegisterLib(Core::Loader::SymbolsResolver* sym);
} // namespace Libraries::Http
