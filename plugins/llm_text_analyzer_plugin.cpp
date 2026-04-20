/**
 * @file llm_text_analyzer_plugin.cpp
 * @brief LLM Text Analyzer Plugin — reads TXT files, sends content to LLM API
 *        (OpenAI compatible / DashScope / Ollama) and writes analysis results.
 *
 * LLM 文本分析插件 — 读取 TXT 文件，将内容发送给大模型 API
 * （支持 OpenAI 兼容 / 阿里云通义千问 / 本地 Ollama），
 * 执行用户自定义分析任务，将结果写入输出文件。
 *
 * ABI safety: all ExecutionContext access goes through extern "C" ctx_* helpers.
 * ABI 安全：所有 ExecutionContext 访问通过 extern "C" ctx_* 辅助函数完成。
 */

#include "ExecutionContext.h"
#include "WorkflowNode.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <netdb.h>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>
#include <vector>

// OpenSSL headers
#include <openssl/err.h>
#include <openssl/ssl.h>

namespace fs = std::filesystem;

// ============================================================================
// Preset task prompts / 预设任务提示词
// ============================================================================

constexpr std::string_view kSummaryPrompt =
    "请对以下文本进行分析，输出：\n"
    "1. 核心摘要（200字以内）\n"
    "2. 关键要点（列表形式）\n"
    "3. 内容分类\n\n{content}";

constexpr std::string_view kKeywordsPrompt =
    "从以下文本中提取关键词和短语，按重要性排序，输出 JSON 数组格式：\n\n{content}";

constexpr std::string_view kSentimentPrompt =
    "分析以下文本的情感倾向，输出：情感标签(正面/负面/中性)、置信度(0-1)、关键情感词：\n\n{content}";

constexpr std::string_view kClassificationPrompt =
    "将以下文本分类到合适的类别中，输出类别名称和理由：\n\n{content}";

constexpr std::string_view kQaExtractPrompt =
    "从以下文本中提取所有的问答对(Q&A)，输出结构化的问题和答案：\n\n{content}";

// ============================================================================
// RAII helpers / RAII 资源管理辅助类
// ============================================================================

/** RAII wrapper for SSL_CTX. SSL_CTX 的 RAII 包装器。 */
struct SslCtxGuard {
    SSL_CTX* ctx = nullptr;
    explicit SslCtxGuard(SSL_CTX* c) : ctx(c) {}
    ~SslCtxGuard() { if (ctx) SSL_CTX_free(ctx); }
    SslCtxGuard(const SslCtxGuard&) = delete;
    SslCtxGuard& operator=(const SslCtxGuard&) = delete;
};

/** RAII wrapper for SSL*. SSL 指针的 RAII 包装器。 */
struct SslGuard {
    SSL* ssl = nullptr;
    explicit SslGuard(SSL* s) : ssl(s) {}
    ~SslGuard() { if (ssl) { SSL_shutdown(ssl); SSL_free(ssl); } }
    SslGuard(const SslGuard&) = delete;
    SslGuard& operator=(const SslGuard&) = delete;
};

/** RAII wrapper for socket fd. 套接字 fd 的 RAII 包装器。 */
struct FdGuard {
    int fd = -1;
    explicit FdGuard(int f) : fd(f) {}
    ~FdGuard() { if (fd >= 0) close(fd); }
    FdGuard(const FdGuard&) = delete;
    FdGuard& operator=(const FdGuard&) = delete;
};

// ============================================================================
// HttpClient — HTTP/HTTPS POST with SSL (skips SSL for plain HTTP)
// HttpClient — 支持 SSL 的 HTTP/HTTPS POST 客户端（纯 HTTP 跳过 SSL）
// ============================================================================

struct HttpResponse {
    int         status_code = 0;
    std::string body;
};

class HttpClient {
public:
    /**
     * Perform an HTTP or HTTPS POST request.
     * 执行 HTTP 或 HTTPS POST 请求。
     *
     * @param url      Full URL including scheme.
     * @param headers  Extra headers (e.g. "Authorization: Bearer xxx\r\n")
     * @param body     Request body
     * @param timeout_s Timeout in seconds
     */
    [[nodiscard]] static HttpResponse post(
        const std::string& url,
        const std::string& headers,
        const std::string& body,
        int timeout_s = 120)
    {
        // Parse URL: scheme, host, port, path
        // 解析 URL：方案、主机、端口、路径
        bool use_ssl = false;
        std::string host, path;
        int port = 80;

        if (url.rfind("https://", 0) == 0) {
            use_ssl = true;
            port = 443;
            auto rest = url.substr(8);
            auto slash = rest.find('/');
            host = (slash == std::string::npos) ? rest : rest.substr(0, slash);
            path = (slash == std::string::npos) ? "/" : rest.substr(slash);
        } else if (url.rfind("http://", 0) == 0) {
            auto rest = url.substr(7);
            auto slash = rest.find('/');
            host = (slash == std::string::npos) ? rest : rest.substr(0, slash);
            path = (slash == std::string::npos) ? "/" : rest.substr(slash);
        } else {
            return {-1, "Unsupported scheme"};
        }

        // Handle optional port in host string
        // 处理主机字符串中的可选端口
        if (auto colon = host.find(':'); colon != std::string::npos) {
            port = std::stoi(host.substr(colon + 1));
            host = host.substr(0, colon);
        }

        // DNS resolution / DNS 解析
        struct addrinfo hints{};
        hints.ai_family   = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        struct addrinfo* res = nullptr;
        if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0) {
            return {-1, "DNS resolution failed for: " + host};
        }
        struct AddrInfoGuard { struct addrinfo* p; ~AddrInfoGuard() { freeaddrinfo(p); } } ag{res};

        // Connect socket / 连接套接字
        int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        if (fd < 0) return {-1, "socket() failed"};
        FdGuard fdg(fd);

        // Set timeout on socket / 设置套接字超时
        struct timeval tv;
        tv.tv_sec  = timeout_s;
        tv.tv_usec = 0;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        if (connect(fd, res->ai_addr, res->ai_addrlen) != 0) {
            return {-1, std::string("connect() failed: ") + strerror(errno)};
        }

        if (use_ssl) {
            return postSsl(fd, host, path, headers, body);
        } else {
            return postPlain(fd, path, host, port, headers, body);
        }
    }

private:
    // Build and send raw HTTP/1.1 request, parse response.
    // 构建并发送 HTTP/1.1 裸请求，解析响应。
    static HttpResponse postPlain(int fd, const std::string& path,
                                   const std::string& host, int port,
                                   const std::string& extra_headers,
                                   const std::string& body)
    {
        std::string req = buildRequest(path, host, port, extra_headers, body, false);
        if (write(fd, req.data(), req.size()) < 0) {
            return {-1, "write() failed"};
        }
        return readResponse(fd, [&](void* buf, size_t len) -> ssize_t {
            return read(fd, buf, len);
        });
    }

    static HttpResponse postSsl(int fd, const std::string& host,
                                 const std::string& path,
                                 const std::string& extra_headers,
                                 const std::string& body)
    {
        SSL_CTX* raw_ctx = SSL_CTX_new(TLS_client_method());
        if (!raw_ctx) return {-1, "SSL_CTX_new failed"};
        SslCtxGuard ctx_guard(raw_ctx);

        SSL* raw_ssl = SSL_new(raw_ctx);
        if (!raw_ssl) return {-1, "SSL_new failed"};
        SslGuard ssl_guard(raw_ssl);

        SSL_set_fd(raw_ssl, fd);
        SSL_set_tlsext_host_name(raw_ssl, host.c_str());
        if (SSL_connect(raw_ssl) != 1) {
            return {-1, "SSL_connect failed"};
        }

        std::string req = buildRequest(path, host, 443, extra_headers, body, true);
        if (SSL_write(raw_ssl, req.data(), static_cast<int>(req.size())) <= 0) {
            return {-1, "SSL_write failed"};
        }
        return readResponse(fd, [&](void* buf, size_t len) -> ssize_t {
            return SSL_read(raw_ssl, buf, static_cast<int>(len));
        });
    }

    static std::string buildRequest(const std::string& path,
                                     const std::string& host,
                                     int port,
                                     const std::string& extra_headers,
                                     const std::string& body,
                                     bool is_ssl)
    {
        std::ostringstream oss;
        oss << "POST " << path << " HTTP/1.1\r\n"
            << "Host: " << host;
        if ((!is_ssl && port != 80) || (is_ssl && port != 443)) {
            oss << ":" << port;
        }
        oss << "\r\n"
            << "Content-Type: application/json\r\n"
            << "Content-Length: " << body.size() << "\r\n"
            << "Connection: close\r\n"
            << extra_headers
            << "\r\n"
            << body;
        return oss.str();
    }

    template<typename ReadFn>
    static HttpResponse readResponse(int /*fd*/, ReadFn read_fn)
    {
        // Read all response data / 读取全部响应数据
        std::string raw;
        char buf[4096];
        ssize_t n;
        while ((n = read_fn(buf, sizeof(buf))) > 0) {
            raw.append(buf, static_cast<size_t>(n));
        }

        if (raw.empty()) return {-1, "Empty response"};

        // Parse status line / 解析状态行
        auto line_end = raw.find("\r\n");
        if (line_end == std::string::npos) return {-1, "Malformed response"};

        int status = 0;
        auto space1 = raw.find(' ');
        if (space1 != std::string::npos && space1 < line_end) {
            auto space2 = raw.find(' ', space1 + 1);
            status = std::stoi(raw.substr(space1 + 1, space2 - space1 - 1));
        }

        // Find header/body separator / 查找头部/正文分隔符
        auto sep = raw.find("\r\n\r\n");
        if (sep == std::string::npos) return {status, raw};

        std::string response_body = raw.substr(sep + 4);

        // Handle chunked transfer encoding / 处理分块传输编码
        auto header_section = raw.substr(0, sep);
        if (header_section.find("Transfer-Encoding: chunked") != std::string::npos ||
            header_section.find("transfer-encoding: chunked") != std::string::npos) {
            response_body = unchunk(response_body);
        }

        return {status, response_body};
    }

    // Decode chunked transfer encoding / 解码分块传输编码
    static std::string unchunk(const std::string& chunked)
    {
        std::string result;
        size_t pos = 0;
        while (pos < chunked.size()) {
            auto crlf = chunked.find("\r\n", pos);
            if (crlf == std::string::npos) break;
            std::string size_str = chunked.substr(pos, crlf - pos);
            // Remove chunk extensions if any
            auto semi = size_str.find(';');
            if (semi != std::string::npos) size_str = size_str.substr(0, semi);
            size_t chunk_size = 0;
            try { chunk_size = std::stoul(size_str, nullptr, 16); } catch (...) { break; }
            if (chunk_size == 0) break;
            pos = crlf + 2;
            if (pos + chunk_size > chunked.size()) break;
            result.append(chunked.substr(pos, chunk_size));
            pos += chunk_size + 2; // skip trailing CRLF
        }
        return result;
    }
};

// ============================================================================
// Mini JSON extraction helpers / 简易 JSON 提取辅助函数
// ============================================================================

/** Extract the value of a top-level string field from JSON.
 *  从 JSON 中提取顶层字符串字段的值。 */
static std::string jsonExtractString(const std::string& json, const std::string& key)
{
    std::string search = "\"" + key + "\"";
    auto pos = json.find(search);
    if (pos == std::string::npos) return {};
    pos += search.size();
    // skip whitespace and colon
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == ':' || json[pos] == '\t')) ++pos;
    if (pos >= json.size() || json[pos] != '"') return {};
    ++pos;
    std::string result;
    while (pos < json.size() && json[pos] != '"') {
        if (json[pos] == '\\' && pos + 1 < json.size()) {
            ++pos;
            switch (json[pos]) {
                case 'n': result += '\n'; break;
                case 't': result += '\t'; break;
                case 'r': result += '\r'; break;
                default: result += json[pos]; break;
            }
        } else {
            result += json[pos];
        }
        ++pos;
    }
    return result;
}

/** Extract integer value from JSON. JSON 中提取整数值。 */
static int64_t jsonExtractInt(const std::string& json, const std::string& key, int64_t def = 0)
{
    std::string search = "\"" + key + "\"";
    auto pos = json.find(search);
    if (pos == std::string::npos) return def;
    pos += search.size();
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == ':' || json[pos] == '\t')) ++pos;
    if (pos >= json.size()) return def;
    try { return std::stoll(json.substr(pos)); } catch (...) { return def; }
}

/**
 * Extract the LLM response content from the API response body.
 * 从 API 响应体中提取 LLM 回复内容。
 */
static std::string extractLlmContent(const std::string& json_body, const std::string& provider)
{
    if (provider == "dashscope") {
        // DashScope native: output.text or output.choices[0].message.content
        // 通义千问原生格式
        auto pos = json_body.find("\"output\"");
        if (pos != std::string::npos) {
            auto sub = json_body.substr(pos);
            auto text = jsonExtractString(sub, "text");
            if (!text.empty()) return text;
        }
    }

    if (provider == "ollama") {
        // Ollama: message.content
        auto pos = json_body.find("\"message\"");
        if (pos != std::string::npos) {
            auto sub = json_body.substr(pos);
            auto content = jsonExtractString(sub, "content");
            if (!content.empty()) return content;
        }
    }

    // OpenAI compatible: choices[0].message.content
    // OpenAI 兼容格式
    auto choices_pos = json_body.find("\"choices\"");
    if (choices_pos != std::string::npos) {
        auto msg_pos = json_body.find("\"message\"", choices_pos);
        if (msg_pos != std::string::npos) {
            auto sub = json_body.substr(msg_pos);
            return jsonExtractString(sub, "content");
        }
    }
    return {};
}

/** Extract total_tokens from usage. 从 usage 中提取 total_tokens。 */
static int64_t extractTokensUsed(const std::string& json_body)
{
    auto pos = json_body.find("\"usage\"");
    if (pos == std::string::npos) return 0;
    auto sub = json_body.substr(pos);
    int64_t t = jsonExtractInt(sub, "total_tokens", 0);
    if (t > 0) return t;
    // DashScope may use input_tokens + output_tokens
    int64_t in_t  = jsonExtractInt(sub, "input_tokens", 0);
    int64_t out_t = jsonExtractInt(sub, "output_tokens", 0);
    return in_t + out_t;
}

// ============================================================================
// Text segmentation / 长文本分段
// ============================================================================

/**
 * Split text into chunks of at most max_chars, respecting paragraph or
 * sentence boundaries.
 * 按段落或句子边界将文本切分为最大 max_chars 个字符的块。
 */
static std::vector<std::string> splitText(const std::string& text, size_t max_chars)
{
    if (text.size() <= max_chars) return {text};

    std::vector<std::string> chunks;
    size_t start = 0;
    while (start < text.size()) {
        if (text.size() - start <= max_chars) {
            chunks.push_back(text.substr(start));
            break;
        }
        // Try paragraph boundary first
        // 先尝试段落边界
        size_t end = start + max_chars;
        auto para = text.rfind("\n\n", end);
        if (para != std::string::npos && para > start) {
            end = para + 2;
        } else {
            // Try sentence boundary: . or 。
            // 再尝试句子边界
            auto sent = text.rfind('.', end);
            auto sent_cn = text.rfind('\xe3', end); // 。 starts with 0xe3 in UTF-8
            size_t best = std::string::npos;
            if (sent != std::string::npos && sent > start) best = sent + 1;
            if (sent_cn != std::string::npos && sent_cn > start &&
                sent_cn > (best == std::string::npos ? start : best)) {
                // Check it's actually 。 (e3 80 82)
                if (sent_cn + 2 < text.size() &&
                    (unsigned char)text[sent_cn] == 0xe3 &&
                    (unsigned char)text[sent_cn+1] == 0x80 &&
                    (unsigned char)text[sent_cn+2] == 0x82) {
                    best = sent_cn + 3;
                }
            }
            if (best != std::string::npos) end = best;
        }
        chunks.push_back(text.substr(start, end - start));
        start = end;
    }
    return chunks;
}

// ============================================================================
// String helpers / 字符串辅助函数
// ============================================================================

static std::string replaceAll(std::string s, const std::string& from, const std::string& to)
{
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
    return s;
}

static std::string trim(const std::string& s)
{
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return {};
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

// ============================================================================
// JSON string escaping / JSON 字符串转义
// ============================================================================

static std::string jsonEscape(const std::string& s)
{
    std::string out;
    out.reserve(s.size() + 32);
    for (unsigned char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20) {
                    char esc[8];
                    snprintf(esc, sizeof(esc), "\\u%04x", c);
                    out += esc;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    return out;
}

// ============================================================================
// LlmTextAnalyzerNode
// ============================================================================

class LlmTextAnalyzerNode : public WorkflowNode {
public:
    void execute(ExecutionContext& ctx) override
    {
        void* cp = static_cast<void*>(&ctx);

        // ---- Read input parameters / 读取输入参数 -------------------------
        char buf[4096] = {};

        // Required: input path / 必须：输入路径
        if (ctx_get_string(cp, "llm_input_path", buf, sizeof(buf)) < 0) {
            ctx_set_string(cp, "llm_status", "failed");
            ctx_set_string(cp, "llm_error", "llm_input_path is required");
            std::cerr << "[LlmTextAnalyzer] ERROR: llm_input_path is required\n";
            return;
        }
        std::string input_path(buf);

        // Required: API key / 必须：API Key
        char api_key[1024] = {};
        if (ctx_get_string(cp, "llm_api_key", api_key, sizeof(api_key)) < 0) {
            ctx_set_string(cp, "llm_status", "failed");
            ctx_set_string(cp, "llm_error", "llm_api_key is required");
            std::cerr << "[LlmTextAnalyzer] ERROR: llm_api_key is required\n";
            return;
        }

        auto getStr = [&](const char* key, const char* def) -> std::string {
            char b[4096] = {};
            if (ctx_get_string(cp, key, b, sizeof(b)) >= 0) return b;
            return def;
        };
        auto getInt = [&](const char* key, int64_t def) -> int64_t {
            int64_t v = def;
            ctx_get_int64(cp, key, &v);
            return v;
        };
        auto getDbl = [&](const char* key, double def) -> double {
            double v = def;
            ctx_get_double(cp, key, &v);
            return v;
        };

        const std::string output_dir   = getStr("llm_output_dir",   "./llm_output");
        const std::string endpoint     = getStr("llm_api_endpoint",  "https://api.openai.com/v1/chat/completions");
        const std::string model        = getStr("llm_model",         "gpt-4o");
        const std::string provider     = getStr("llm_provider",      "openai");
        const std::string sys_prompt   = getStr("llm_system_prompt", "你是一个专业的文本分析助手。");
        const std::string task_type    = getStr("llm_task_type",     "summary");
        const std::string out_format   = getStr("llm_output_format", "markdown");
        const std::string recursive_s  = getStr("llm_recursive",     "true");
        const std::string overwrite_s  = getStr("llm_overwrite",     "false");
        const std::string merge_s      = getStr("llm_merge_output",  "false");
        const std::string merge_prompt = getStr("llm_merge_prompt",
            "以下是多篇文本的分析结果，请综合所有内容生成一份总结报告：\n\n{all_results}");
        const std::string file_exts    = getStr("llm_file_extensions", ".txt");

        const int64_t max_tokens      = getInt("llm_max_tokens",    4096);
        const int64_t max_input_chars = getInt("llm_max_input_chars", 32000);
        const int64_t parallel        = getInt("llm_parallel",       2);
        const int64_t timeout_s       = getInt("llm_timeout",        120);
        const double  temperature     = getDbl("llm_temperature",    0.3);

        bool recursive = (recursive_s == "true" || recursive_s == "1");
        bool overwrite = (overwrite_s == "true" || overwrite_s == "1");
        bool merge     = (merge_s     == "true" || merge_s     == "1");

        // Determine user prompt template from task type
        // 根据任务类型确定用户提示词模板
        std::string user_tmpl;
        if (task_type == "summary")        user_tmpl = std::string(kSummaryPrompt);
        else if (task_type == "keywords")  user_tmpl = std::string(kKeywordsPrompt);
        else if (task_type == "sentiment") user_tmpl = std::string(kSentimentPrompt);
        else if (task_type == "classification") user_tmpl = std::string(kClassificationPrompt);
        else if (task_type == "qa_extract")    user_tmpl = std::string(kQaExtractPrompt);
        else { // "custom" or any other — use user_prompt_template param
            user_tmpl = getStr("llm_user_prompt_template",
                "请分析以下文本内容，提供摘要、关键词和情感倾向：\n\n{content}");
        }

        // ---- Collect input files / 收集输入文件 ---------------------------
        std::vector<fs::path> input_files;
        {
            std::error_code ec;
            // Parse extensions
            std::vector<std::string> exts;
            {
                std::istringstream iss(file_exts);
                std::string tok;
                while (std::getline(iss, tok, ',')) {
                    tok = trim(tok);
                    if (!tok.empty()) exts.push_back(tok);
                }
            }

            auto matchExt = [&](const fs::path& p) {
                if (exts.empty()) return true;
                std::string ext = p.extension().string();
                for (auto& e : exts) if (e == ext) return true;
                return false;
            };

            fs::path ip(input_path);
            if (fs::is_regular_file(ip, ec)) {
                if (matchExt(ip)) input_files.push_back(ip);
            } else if (fs::is_directory(ip, ec)) {
                if (recursive) {
                    for (auto& entry : fs::recursive_directory_iterator(ip, ec)) {
                        if (entry.is_regular_file() && matchExt(entry.path()))
                            input_files.push_back(entry.path());
                    }
                } else {
                    for (auto& entry : fs::directory_iterator(ip, ec)) {
                        if (entry.is_regular_file() && matchExt(entry.path()))
                            input_files.push_back(entry.path());
                    }
                }
            } else {
                ctx_set_string(cp, "llm_status", "failed");
                ctx_set_string(cp, "llm_error", ("input_path not found: " + input_path).c_str());
                return;
            }
            std::sort(input_files.begin(), input_files.end());
        }

        const int64_t total_found = static_cast<int64_t>(input_files.size());
        ctx_set_int64(cp, "llm_total_found", total_found);
        std::cout << "[LlmTextAnalyzer] 发现 " << total_found << " 个输入文件\n";

        // ---- Create output directory / 创建输出目录 -----------------------
        {
            std::error_code ec;
            fs::create_directories(output_dir, ec);
            if (ec) {
                ctx_set_string(cp, "llm_status", "failed");
                ctx_set_string(cp, "llm_error",
                    ("Cannot create output_dir: " + output_dir + ": " + ec.message()).c_str());
                return;
            }
        }

        // ---- Counters and state / 计数器与状态 ---------------------------
        std::atomic<int64_t> analyzed_count{0};
        std::atomic<int64_t> failed_count{0};
        std::atomic<int64_t> skipped_count{0};
        std::atomic<int64_t> total_input_chars{0};
        std::atomic<int64_t> total_output_chars{0};
        std::atomic<int64_t> total_tokens{0};
        std::mutex output_mutex;
        std::vector<std::string> output_files;

        // ---- LLM call helper / LLM 调用辅助函数 -------------------------
        auto callLlm = [&](const std::string& content,
                            const std::string& filename) -> std::string
        {
            // Build user prompt / 构建用户提示词
            std::string user_prompt = replaceAll(user_tmpl, "{content}", content);
            user_prompt = replaceAll(user_prompt, "{filename}", filename);

            // Build request body based on provider / 根据提供商构建请求体
            std::string request_body;
            std::string actual_endpoint = endpoint;
            std::string extra_headers;

            if (provider == "dashscope") {
                // DashScope native format / 通义千问原生格式
                actual_endpoint = "https://dashscope.aliyuncs.com/api/v1/services/aigc/text-generation/generation";
                request_body = "{"
                    "\"model\":\"" + jsonEscape(model) + "\","
                    "\"input\":{"
                        "\"messages\":["
                            "{\"role\":\"system\",\"content\":\"" + jsonEscape(sys_prompt) + "\"},"
                            "{\"role\":\"user\",\"content\":\"" + jsonEscape(user_prompt) + "\"}"
                        "]"
                    "},"
                    "\"parameters\":{"
                        "\"max_tokens\":" + std::to_string(max_tokens) + ","
                        "\"temperature\":" + std::to_string(temperature) +
                    "}"
                "}";
                extra_headers = std::string("Authorization: Bearer ") + api_key + "\r\n";
            } else if (provider == "ollama") {
                // Ollama local format / Ollama 本地格式
                actual_endpoint = endpoint; // typically http://localhost:11434/api/chat
                request_body = "{"
                    "\"model\":\"" + jsonEscape(model) + "\","
                    "\"messages\":["
                        "{\"role\":\"system\",\"content\":\"" + jsonEscape(sys_prompt) + "\"},"
                        "{\"role\":\"user\",\"content\":\"" + jsonEscape(user_prompt) + "\"}"
                    "],"
                    "\"stream\":false"
                "}";
                extra_headers = {}; // Ollama doesn't need auth
            } else {
                // OpenAI compatible format / OpenAI 兼容格式
                request_body = "{"
                    "\"model\":\"" + jsonEscape(model) + "\","
                    "\"messages\":["
                        "{\"role\":\"system\",\"content\":\"" + jsonEscape(sys_prompt) + "\"},"
                        "{\"role\":\"user\",\"content\":\"" + jsonEscape(user_prompt) + "\"}"
                    "],"
                    "\"max_tokens\":" + std::to_string(max_tokens) + ","
                    "\"temperature\":" + std::to_string(temperature) +
                "}";
                extra_headers = std::string("Authorization: Bearer ") + api_key + "\r\n";
            }

            auto resp = HttpClient::post(actual_endpoint, extra_headers,
                                          request_body, static_cast<int>(timeout_s));
            if (resp.status_code != 200) {
                return {}; // caller will mark as failed
            }
            total_tokens.fetch_add(extractTokensUsed(resp.body),
                                    std::memory_order_relaxed);
            return extractLlmContent(resp.body, provider);
        };

        // ---- Process a single file / 处理单个文件 -----------------------
        auto processFile = [&](const fs::path& file_path)
        {
            const std::string filename = file_path.filename().string();
            const std::string stem     = file_path.stem().string();

            // Determine output file path / 确定输出文件路径
            std::string out_ext = (out_format == "json") ? ".json" :
                                  (out_format == "txt")  ? ".txt"  : ".md";
            fs::path out_file = fs::path(output_dir) / (stem + "_analysis" + out_ext);

            // Skip if exists and overwrite is false / 若文件已存在且不覆盖则跳过
            if (!overwrite && fs::exists(out_file)) {
                skipped_count.fetch_add(1, std::memory_order_relaxed);
                std::cout << "[LlmTextAnalyzer] 跳过 (已存在): " << filename << "\n";
                return;
            }

            // Read input file / 读取输入文件
            std::string content;
            {
                std::ifstream ifs(file_path, std::ios::binary);
                if (!ifs) {
                    failed_count.fetch_add(1, std::memory_order_relaxed);
                    std::cerr << "[LlmTextAnalyzer] 无法打开: " << file_path << "\n";
                    return;
                }
                std::ostringstream oss;
                oss << ifs.rdbuf();
                content = oss.str();
            }
            total_input_chars.fetch_add(static_cast<int64_t>(content.size()),
                                         std::memory_order_relaxed);

            std::cout << "[LlmTextAnalyzer] 分析: " << filename
                      << " (" << content.size() << " 字符)\n";

            // Handle long text segmentation / 处理长文本分段
            std::string analysis_result;
            auto segments = splitText(content, static_cast<size_t>(max_input_chars));

            if (segments.size() == 1) {
                analysis_result = callLlm(content, filename);
            } else {
                std::cout << "[LlmTextAnalyzer] 长文本分 " << segments.size()
                          << " 段处理: " << filename << "\n";
                std::vector<std::string> seg_results;
                for (size_t i = 0; i < segments.size(); ++i) {
                    std::cout << "[LlmTextAnalyzer]   段落 " << (i+1) << "/"
                              << segments.size() << " ...\n";
                    auto r = callLlm(segments[i], filename);
                    if (r.empty()) {
                        failed_count.fetch_add(1, std::memory_order_relaxed);
                        return;
                    }
                    seg_results.push_back(r);
                }
                // Merge segment results with a second LLM call
                // 用二次 LLM 调用合并分段结果
                std::string all_segs;
                for (size_t i = 0; i < seg_results.size(); ++i) {
                    all_segs += "=== 第" + std::to_string(i+1) + "段分析结果 ===\n";
                    all_segs += seg_results[i] + "\n\n";
                }
                std::string merge_tmpl =
                    "以上是一篇长文档按段落分析的结果，请综合汇总为一份完整分析报告：\n\n{content}";
                std::string old_tmpl = user_tmpl;
                // Temporarily use merge template
                std::string merged = callLlm(all_segs, filename);
                if (merged.empty()) merged = all_segs; // fallback
                analysis_result = merged;
            }

            if (analysis_result.empty()) {
                failed_count.fetch_add(1, std::memory_order_relaxed);
                std::cerr << "[LlmTextAnalyzer] API 调用失败: " << filename << "\n";
                return;
            }

            total_output_chars.fetch_add(static_cast<int64_t>(analysis_result.size()),
                                          std::memory_order_relaxed);

            // Format output / 格式化输出
            std::string output_content;
            auto now = std::chrono::system_clock::now();
            auto t   = std::chrono::system_clock::to_time_t(now);
            char time_buf[64] = {};
            struct tm tm_info{};
            localtime_r(&t, &tm_info);
            strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", &tm_info);

            if (out_format == "markdown") {
                output_content =
                    "# 文本分析报告\n\n"
                    "**源文件**: " + filename + "\n"
                    "**分析时间**: " + std::string(time_buf) + "\n"
                    "**模型**: " + model + "\n\n"
                    "---\n\n" +
                    analysis_result + "\n";
            } else if (out_format == "json") {
                output_content =
                    "{\n"
                    "  \"source_file\": \"" + jsonEscape(filename) + "\",\n"
                    "  \"analysis_time\": \"" + std::string(time_buf) + "\",\n"
                    "  \"model\": \"" + jsonEscape(model) + "\",\n"
                    "  \"result\": \"" + jsonEscape(analysis_result) + "\"\n"
                    "}\n";
            } else { // txt
                output_content =
                    "源文件: " + filename + "\n"
                    "分析时间: " + std::string(time_buf) + "\n"
                    "模型: " + model + "\n\n" +
                    analysis_result + "\n";
            }

            // Atomic write: write to .tmp then rename
            // 原子写入：先写 .tmp 再 rename
            fs::path tmp_file = out_file.string() + ".tmp";
            {
                std::ofstream ofs(tmp_file, std::ios::binary | std::ios::trunc);
                if (!ofs) {
                    failed_count.fetch_add(1, std::memory_order_relaxed);
                    std::cerr << "[LlmTextAnalyzer] 无法写入: " << tmp_file << "\n";
                    return;
                }
                ofs << output_content;
            }
            std::error_code ec;
            fs::rename(tmp_file, out_file, ec);
            if (ec) {
                fs::remove(tmp_file, ec);
                failed_count.fetch_add(1, std::memory_order_relaxed);
                return;
            }

            analyzed_count.fetch_add(1, std::memory_order_relaxed);
            std::cout << "[LlmTextAnalyzer] ✓ " << filename
                      << " → " << out_file.filename().string() << "\n";

            {
                std::lock_guard lock(output_mutex);
                output_files.push_back(out_file.string());
            }
        };

        // ---- Parallel execution / 并行执行 --------------------------------
        const size_t n_par = static_cast<size_t>(std::max(int64_t(1), parallel));
        const size_t n_files = input_files.size();
        std::atomic<size_t> idx{0};

        {
            std::vector<std::jthread> workers;
            workers.reserve(n_par);
            for (size_t t = 0; t < std::min(n_par, n_files); ++t) {
                workers.emplace_back([&]() {
                    while (true) {
                        size_t i = idx.fetch_add(1, std::memory_order_relaxed);
                        if (i >= n_files) break;
                        processFile(input_files[i]);
                    }
                });
            }
        } // jthreads join here

        // ---- Merge output / 合并输出 --------------------------------------
        std::string merged_output_file;
        if (merge && !output_files.empty()) {
            std::cout << "[LlmTextAnalyzer] 合并 " << output_files.size() << " 个分析结果...\n";
            std::string all_results;
            for (auto& of : output_files) {
                std::ifstream ifs(of);
                if (!ifs) continue;
                std::ostringstream oss;
                oss << ifs.rdbuf();
                all_results += "=== " + fs::path(of).filename().string() + " ===\n";
                all_results += oss.str() + "\n\n";
            }
            std::string merge_user = replaceAll(merge_prompt, "{all_results}", all_results);
            // Direct LLM call for merge (using kSummaryPrompt style)
            auto old_user_tmpl = user_tmpl;
            // Override user_tmpl temporarily for the merge call
            std::string merge_result;
            {
                // Build merge request directly
                std::string request_body;
                std::string extra_headers;
                std::string actual_endpoint = endpoint;

                if (provider == "dashscope") {
                    actual_endpoint = "https://dashscope.aliyuncs.com/api/v1/services/aigc/text-generation/generation";
                    request_body = "{"
                        "\"model\":\"" + jsonEscape(model) + "\","
                        "\"input\":{\"messages\":["
                            "{\"role\":\"system\",\"content\":\"" + jsonEscape(sys_prompt) + "\"},"
                            "{\"role\":\"user\",\"content\":\"" + jsonEscape(merge_user) + "\"}"
                        "]},"
                        "\"parameters\":{\"max_tokens\":" + std::to_string(max_tokens) + ","
                            "\"temperature\":" + std::to_string(temperature) + "}"
                    "}";
                    extra_headers = std::string("Authorization: Bearer ") + api_key + "\r\n";
                } else if (provider == "ollama") {
                    request_body = "{"
                        "\"model\":\"" + jsonEscape(model) + "\","
                        "\"messages\":["
                            "{\"role\":\"system\",\"content\":\"" + jsonEscape(sys_prompt) + "\"},"
                            "{\"role\":\"user\",\"content\":\"" + jsonEscape(merge_user) + "\"}"
                        "],\"stream\":false}";
                } else {
                    request_body = "{"
                        "\"model\":\"" + jsonEscape(model) + "\","
                        "\"messages\":["
                            "{\"role\":\"system\",\"content\":\"" + jsonEscape(sys_prompt) + "\"},"
                            "{\"role\":\"user\",\"content\":\"" + jsonEscape(merge_user) + "\"}"
                        "],"
                        "\"max_tokens\":" + std::to_string(max_tokens) + ","
                        "\"temperature\":" + std::to_string(temperature) + "}";
                    extra_headers = std::string("Authorization: Bearer ") + api_key + "\r\n";
                }
                auto resp = HttpClient::post(actual_endpoint, extra_headers,
                                              request_body, static_cast<int>(timeout_s));
                if (resp.status_code == 200) {
                    merge_result = extractLlmContent(resp.body, provider);
                    total_tokens.fetch_add(extractTokensUsed(resp.body),
                                           std::memory_order_relaxed);
                }
            }

            if (merge_result.empty()) merge_result = all_results;

            fs::path mf = fs::path(output_dir) / "_merged_report.md";
            fs::path mf_tmp = mf.string() + ".tmp";
            {
                std::ofstream ofs(mf_tmp, std::ios::binary | std::ios::trunc);
                if (ofs) {
                    ofs << "# 综合分析报告\n\n";
                    ofs << merge_result << "\n";
                }
            }
            std::error_code ec;
            fs::rename(mf_tmp, mf, ec);
            if (!ec) {
                merged_output_file = mf.string();
                ctx_set_string(cp, "llm_merged_output_file", merged_output_file.c_str());
                std::cout << "[LlmTextAnalyzer] 合并报告已生成: " << mf << "\n";
            }
        }

        // ---- Write output context / 写入输出上下文 -----------------------
        ctx_set_int64(cp, "llm_total_found",      total_found);
        ctx_set_int64(cp, "llm_analyzed_count",   analyzed_count.load());
        ctx_set_int64(cp, "llm_failed_count",     failed_count.load());
        ctx_set_int64(cp, "llm_skipped_count",    skipped_count.load());
        ctx_set_int64(cp, "llm_total_input_chars", total_input_chars.load());
        ctx_set_int64(cp, "llm_total_output_chars", total_output_chars.load());
        ctx_set_int64(cp, "llm_total_tokens_used", total_tokens.load());
        ctx_set_string(cp, "llm_output_path", output_dir.c_str());

        {
            std::string files_list;
            for (auto& f : output_files) {
                if (!files_list.empty()) files_list += '\n';
                files_list += f;
            }
            ctx_set_string(cp, "llm_output_files", files_list.c_str());
        }

        // Determine status / 确定状态
        const auto ac = analyzed_count.load();
        const auto fc = failed_count.load();
        if (fc == 0 && ac > 0)       ctx_set_string(cp, "llm_status", "success");
        else if (ac > 0 && fc > 0)   ctx_set_string(cp, "llm_status", "partial");
        else                          ctx_set_string(cp, "llm_status", "failed");

        std::cout << "[LlmTextAnalyzer] 完成: 成功 " << ac
                  << ", 失败 " << fc
                  << ", 跳过 " << skipped_count.load() << "\n";
    }
};

// ---- C factory / destructor -------------------------------------------------

extern "C" WorkflowNode* create_node() {
    return new LlmTextAnalyzerNode();
}

extern "C" void destroy_node(WorkflowNode* node) {
    delete node;
}
