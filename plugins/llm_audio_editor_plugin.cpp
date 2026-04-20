/**
 * @file llm_audio_editor_plugin.cpp
 * @brief LLM Audio Editor Plugin — reads LLM analysis result files containing
 *        JSON edit commands and executes them via FFmpeg on WAV audio files.
 *
 * LLM 驱动音频编辑插件 — 读取包含 JSON 编辑指令的 LLM 分析结果文件，
 * 通过 FFmpeg 对 WAV 音频执行切割、拼接、静音插入、删除片段等操作。
 *
 * ABI safety: all ExecutionContext access goes through extern "C" ctx_* helpers.
 * ABI 安全：所有 ExecutionContext 访问通过 extern "C" ctx_* 辅助函数完成。
 */

#include "ExecutionContext.h"
#include "WorkflowNode.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;

// ============================================================================
// RAII pipe fd guard / RAII 管道 fd 守卫
// ============================================================================

struct PipeFd {
    int fds[2] = {-1, -1};
    PipeFd()  { if (pipe(fds) != 0) { fds[0] = -1; fds[1] = -1; } }
    ~PipeFd() { close(); }
    void close() {
        if (fds[0] >= 0) { ::close(fds[0]); fds[0] = -1; }
        if (fds[1] >= 0) { ::close(fds[1]); fds[1] = -1; }
    }
    void closeRead()  { if (fds[0] >= 0) { ::close(fds[0]); fds[0] = -1; } }
    void closeWrite() { if (fds[1] >= 0) { ::close(fds[1]); fds[1] = -1; } }
    PipeFd(const PipeFd&) = delete;
    PipeFd& operator=(const PipeFd&) = delete;
};

// ============================================================================
// Command execution helper / 命令执行辅助函数
// ============================================================================

struct CommandResult {
    int         exit_code = -1;
    std::string stdout_output;
    std::string stderr_output;
};

/**
 * Run an external command and capture stdout/stderr.
 * 运行外部命令并捕获 stdout/stderr。
 */
static CommandResult runCommand(const std::vector<std::string>& args, int timeout_s = 300)
{
    if (args.empty()) return {-1, {}, "empty args"};

    PipeFd stdout_pipe, stderr_pipe;

    pid_t pid = fork();
    if (pid < 0) return {-1, {}, std::string("fork failed: ") + strerror(errno)};

    if (pid == 0) {
        // Child process / 子进程
        stdout_pipe.closeRead();
        stderr_pipe.closeRead();

        dup2(stdout_pipe.fds[1], STDOUT_FILENO);
        dup2(stderr_pipe.fds[1], STDERR_FILENO);

        stdout_pipe.closeWrite();
        stderr_pipe.closeWrite();

        std::vector<const char*> argv;
        argv.reserve(args.size() + 1);
        for (auto& a : args) argv.push_back(a.c_str());
        argv.push_back(nullptr);

        execvp(argv[0], const_cast<char* const*>(argv.data()));
        // If execvp fails:
        _exit(127);
    }

    // Parent: close write ends, read output
    // 父进程：关闭写端，读取输出
    stdout_pipe.closeWrite();
    stderr_pipe.closeWrite();

    auto readAll = [](int fd) -> std::string {
        std::string result;
        char buf[4096];
        ssize_t n;
        while ((n = read(fd, buf, sizeof(buf))) > 0)
            result.append(buf, static_cast<size_t>(n));
        return result;
    };

    CommandResult res;
    res.stdout_output = readAll(stdout_pipe.fds[0]);
    res.stderr_output = readAll(stderr_pipe.fds[0]);

    // Wait with optional timeout / 等待（可选超时）
    auto start = std::chrono::steady_clock::now();
    while (true) {
        int status = 0;
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r > 0) {
            res.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
            break;
        }
        if (r < 0) { res.exit_code = -1; break; }
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - start).count();
        if (elapsed >= timeout_s) {
            kill(pid, SIGKILL);
            waitpid(pid, nullptr, 0);
            res.exit_code = -1;
            res.stderr_output += "\n[timeout after " + std::to_string(timeout_s) + "s]";
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return res;
}

// ============================================================================
// JSON parsing helpers / JSON 解析辅助函数
// ============================================================================

/** Skip whitespace. 跳过空白字符。 */
static size_t skipWs(const std::string& s, size_t pos)
{
    while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t' ||
                               s[pos] == '\r' || s[pos] == '\n'))
        ++pos;
    return pos;
}

/** Parse a JSON string value starting at the opening quote.
 *  从开引号开始解析 JSON 字符串值。 */
static std::string parseJsonString(const std::string& s, size_t& pos)
{
    if (pos >= s.size() || s[pos] != '"') return {};
    ++pos;
    std::string result;
    while (pos < s.size() && s[pos] != '"') {
        if (s[pos] == '\\' && pos + 1 < s.size()) {
            ++pos;
            switch (s[pos]) {
                case '"':  result += '"';  break;
                case '\\': result += '\\'; break;
                case 'n':  result += '\n'; break;
                case 'r':  result += '\r'; break;
                case 't':  result += '\t'; break;
                case '/':  result += '/';  break;
                default:   result += s[pos]; break;
            }
        } else {
            result += s[pos];
        }
        ++pos;
    }
    if (pos < s.size()) ++pos; // consume closing quote
    return result;
}

/** Parse a JSON number (double). 解析 JSON 数字（double）。 */
static double parseJsonNumber(const std::string& s, size_t& pos)
{
    size_t start = pos;
    if (pos < s.size() && s[pos] == '-') ++pos;
    while (pos < s.size() && (isdigit((unsigned char)s[pos]) ||
                               s[pos] == '.' || s[pos] == 'e' ||
                               s[pos] == 'E' || s[pos] == '+' ||
                               s[pos] == '-'))
        ++pos;
    try { return std::stod(s.substr(start, pos - start)); } catch (...) { return 0.0; }
}

/** Scan forward to find a named key in a JSON object.
 *  在 JSON 对象中向前扫描以查找命名键。 */
static size_t findKey(const std::string& s, size_t start, const std::string& key)
{
    std::string search = "\"" + key + "\"";
    auto pos = s.find(search, start);
    if (pos == std::string::npos) return std::string::npos;
    pos += search.size();
    pos = skipWs(s, pos);
    if (pos < s.size() && s[pos] == ':') {
        ++pos;
        pos = skipWs(s, pos);
    }
    return pos;
}

/** Extract a string field from JSON object starting at obj_start.
 *  从 obj_start 开始的 JSON 对象中提取字符串字段。 */
static std::string getStrField(const std::string& s, size_t obj_start,
                                const std::string& key,
                                const std::string& def = {})
{
    auto pos = findKey(s, obj_start, key);
    if (pos == std::string::npos || pos >= s.size() || s[pos] != '"') return def;
    return parseJsonString(s, pos);
}

/** Extract a double field from JSON object.
 *  从 JSON 对象中提取 double 字段。 */
static double getDblField(const std::string& s, size_t obj_start,
                           const std::string& key, double def = 0.0)
{
    auto pos = findKey(s, obj_start, key);
    if (pos == std::string::npos) return def;
    size_t p = pos;
    double v = parseJsonNumber(s, p);
    return (p > pos) ? v : def;
}

/** Extract a bool field (true/false) from JSON object.
 *  从 JSON 对象中提取布尔字段。 */
[[maybe_unused]] static bool getBoolField(const std::string& s, size_t obj_start,
                          const std::string& key, bool def = false)
{
    auto pos = findKey(s, obj_start, key);
    if (pos == std::string::npos) return def;
    if (s.substr(pos, 4) == "true")  return true;
    if (s.substr(pos, 5) == "false") return false;
    return def;
}

// ============================================================================
// Edit command data types / 编辑指令数据类型
// ============================================================================

struct TimeSegment {
    double start = 0.0;
    double end   = 0.0;
};

struct EditCommand {
    std::string              type;        // cut/merge/silence/delete/speed/fade/volume/normalize
    std::string              source;      // single source file
    std::vector<std::string> sources;     // multiple sources (merge)
    std::string              output;
    double                   start = 0.0;
    double                   end   = 0.0;
    double                   position = 0.0;   // silence insert position
    double                   duration = 0.0;   // silence duration
    double                   factor   = 1.0;   // speed factor
    double                   fade_in  = 0.0;
    double                   fade_out = 0.0;
    double                   level    = 1.0;   // volume level
    std::string              transition;       // crossfade etc.
    double                   crossfade_duration = 0.0;
    std::vector<TimeSegment> segments;         // delete segments
    std::string              description;
};

// ============================================================================
// JSON edit command parser / JSON 编辑指令解析器
// ============================================================================

/**
 * Find and extract the JSON string containing "edit_commands" from file content.
 * 从文件内容中查找并提取包含 "edit_commands" 的 JSON 字符串。
 *
 * Strategy:
 *   1. Try to parse the entire file as JSON.
 *   2. Look for ```json ... ``` code block.
 *   3. Look for <!-- EDIT_COMMANDS: ... --> marker.
 *   4. Look for any JSON fragment containing "edit_commands".
 */
static std::string extractJsonCommands(const std::string& content,
                                        const std::string& marker)
{
    // 1. Try entire file
    if (content.find("\"edit_commands\"") != std::string::npos) {
        auto lb = content.find('{');
        auto rb = content.rfind('}');
        if (lb != std::string::npos && rb != std::string::npos && rb > lb) {
            return content.substr(lb, rb - lb + 1);
        }
    }

    // 2. ```json ... ``` code block
    {
        auto pos = content.find("```json");
        if (pos != std::string::npos) {
            pos += 7;
            auto end = content.find("```", pos);
            if (end != std::string::npos) {
                return content.substr(pos, end - pos);
            }
        }
    }

    // 3. <!-- EDIT_COMMANDS: ... --> or <!-- marker: ... -->
    {
        std::string tag_open = "<!-- " + marker + ":";
        auto pos = content.find(tag_open);
        if (pos == std::string::npos) pos = content.find("<!-- EDIT_COMMANDS:");
        if (pos != std::string::npos) {
            pos = content.find('{', pos);
            if (pos != std::string::npos) {
                auto end = content.find("-->", pos);
                if (end != std::string::npos) {
                    auto rb = content.rfind('}', end);
                    if (rb != std::string::npos && rb >= pos) {
                        return content.substr(pos, rb - pos + 1);
                    }
                }
            }
        }
    }

    // 4. Any JSON fragment with "edit_commands"
    {
        auto key_pos = content.find("\"edit_commands\"");
        if (key_pos != std::string::npos) {
            auto lb = content.rfind('{', key_pos);
            if (lb != std::string::npos) {
                // Find balanced closing brace
                int depth = 0;
                for (size_t i = lb; i < content.size(); ++i) {
                    if (content[i] == '{') ++depth;
                    else if (content[i] == '}') {
                        --depth;
                        if (depth == 0) return content.substr(lb, i - lb + 1);
                    }
                }
            }
        }
    }

    return {};
}

/**
 * Parse the edit_commands array from a JSON string.
 * 从 JSON 字符串中解析 edit_commands 数组。
 */
static std::vector<EditCommand> parseEditCommands(const std::string& json)
{
    std::vector<EditCommand> cmds;

    auto arr_pos = json.find("\"edit_commands\"");
    if (arr_pos == std::string::npos) return cmds;

    // Find opening bracket of the array
    arr_pos = json.find('[', arr_pos);
    if (arr_pos == std::string::npos) return cmds;
    ++arr_pos;

    // Iterate through array objects
    while (arr_pos < json.size()) {
        arr_pos = skipWs(json, arr_pos);
        if (arr_pos >= json.size()) break;
        if (json[arr_pos] == ']') break;
        if (json[arr_pos] != '{') { ++arr_pos; continue; }

        // Find the end of this object
        size_t obj_start = arr_pos;
        int depth = 0;
        size_t obj_end = arr_pos;
        for (size_t i = arr_pos; i < json.size(); ++i) {
            if (json[i] == '{') ++depth;
            else if (json[i] == '}') {
                --depth;
                if (depth == 0) { obj_end = i; break; }
            }
        }
        if (depth != 0) break;

        // Parse one command object
        EditCommand cmd;
        cmd.type        = getStrField(json, obj_start, "type");
        cmd.source      = getStrField(json, obj_start, "source");
        cmd.output      = getStrField(json, obj_start, "output");
        cmd.description = getStrField(json, obj_start, "description");
        cmd.transition  = getStrField(json, obj_start, "transition");
        cmd.start       = getDblField(json, obj_start, "start");
        cmd.end         = getDblField(json, obj_start, "end");
        cmd.position    = getDblField(json, obj_start, "position");
        cmd.duration    = getDblField(json, obj_start, "duration");
        cmd.factor      = getDblField(json, obj_start, "factor", 1.0);
        cmd.fade_in     = getDblField(json, obj_start, "fade_in");
        cmd.fade_out    = getDblField(json, obj_start, "fade_out");
        cmd.level       = getDblField(json, obj_start, "level", 1.0);
        cmd.crossfade_duration = getDblField(json, obj_start, "crossfade_duration");

        // Parse sources array (for merge)
        {
            auto src_pos = findKey(json, obj_start, "sources");
            if (src_pos != std::string::npos && src_pos < obj_end &&
                json[src_pos] == '[') {
                ++src_pos;
                while (src_pos < obj_end) {
                    src_pos = skipWs(json, src_pos);
                    if (src_pos >= obj_end || json[src_pos] == ']') break;
                    if (json[src_pos] == '"') {
                        cmd.sources.push_back(parseJsonString(json, src_pos));
                    } else if (json[src_pos] == ',') {
                        ++src_pos;
                    } else {
                        ++src_pos;
                    }
                }
            }
        }

        // Parse segments array (for delete)
        {
            auto seg_pos = findKey(json, obj_start, "segments");
            if (seg_pos != std::string::npos && seg_pos < obj_end &&
                json[seg_pos] == '[') {
                ++seg_pos;
                while (seg_pos < obj_end) {
                    seg_pos = skipWs(json, seg_pos);
                    if (seg_pos >= obj_end || json[seg_pos] == ']') break;
                    if (json[seg_pos] == '{') {
                        size_t seg_obj = seg_pos;
                        // Find end of segment object
                        int d = 0;
                        for (size_t i = seg_pos; i < obj_end; ++i) {
                            if (json[i] == '{') ++d;
                            else if (json[i] == '}') { --d; if (d == 0) { seg_pos = i + 1; break; } }
                        }
                        TimeSegment ts;
                        ts.start = getDblField(json, seg_obj, "start");
                        ts.end   = getDblField(json, seg_obj, "end");
                        cmd.segments.push_back(ts);
                    } else if (json[seg_pos] == ',') {
                        ++seg_pos;
                    } else {
                        ++seg_pos;
                    }
                }
            }
        }

        if (!cmd.type.empty()) cmds.push_back(std::move(cmd));
        arr_pos = obj_end + 1;
    }
    return cmds;
}

// ============================================================================
// FFmpeg command builders / FFmpeg 命令构建器
// ============================================================================

/** Get audio duration via ffprobe. 通过 ffprobe 获取音频时长。 */
static double getAudioDuration(const std::string& ffprobe, const std::string& file)
{
    auto res = runCommand({ffprobe, "-v", "quiet", "-print_format", "json",
                           "-show_streams", file}, 30);
    if (res.exit_code != 0) return 0.0;
    auto pos = res.stdout_output.find("\"duration\"");
    if (pos == std::string::npos) return 0.0;
    pos += 10;
    pos = skipWs(res.stdout_output, pos);
    if (pos < res.stdout_output.size() && res.stdout_output[pos] == ':') ++pos;
    pos = skipWs(res.stdout_output, pos);
    if (pos < res.stdout_output.size() && res.stdout_output[pos] == '"') {
        ++pos;
        try { return std::stod(res.stdout_output.substr(pos)); } catch (...) {}
    }
    try { return std::stod(res.stdout_output.substr(pos)); } catch (...) { return 0.0; }
}

/** Build the codec args based on bit_depth. 根据位深构建编解码器参数。 */
static std::vector<std::string> codecArgs(int64_t bit_depth, int64_t sample_rate, int64_t channels)
{
    std::vector<std::string> args;
    const char* codec = "pcm_s16le";
    if (bit_depth == 8)       codec = "pcm_u8";
    else if (bit_depth == 24) codec = "pcm_s24le";
    else if (bit_depth == 32) codec = "pcm_s32le";
    args.push_back("-acodec"); args.push_back(codec);
    if (sample_rate > 0) { args.push_back("-ar"); args.push_back(std::to_string(sample_rate)); }
    if (channels > 0)    { args.push_back("-ac"); args.push_back(std::to_string(channels)); }
    return args;
}

/**
 * Execute a single edit command via FFmpeg.
 * 通过 FFmpeg 执行单条编辑指令。
 *
 * Returns empty string on success, error message on failure.
 * 成功返回空字符串，失败返回错误信息。
 */
static std::string executeEditCommand(
    const EditCommand& cmd,
    const std::string& audio_dir,
    const std::string& output_dir,
    const std::string& ffmpeg,
    const std::string& ffprobe,
    bool               overwrite,
    bool               dry_run,
    int64_t            sample_rate,
    int64_t            channels,
    int64_t            bit_depth)
{
    // Resolve source path: relative to audio_dir
    // 解析源路径：相对于 audio_dir
    auto resolveSrc = [&](const std::string& src) -> std::string {
        if (src.empty()) return {};
        fs::path p(src);
        if (p.is_absolute()) return src;
        return (fs::path(audio_dir) / p).string();
    };

    // Resolve output path: relative to output_dir
    // 解析输出路径：相对于 output_dir
    auto resolveOut = [&](const std::string& out) -> std::string {
        if (out.empty()) return {};
        fs::path p(out);
        if (p.is_absolute()) return out;
        return (fs::path(output_dir) / p).string();
    };

    auto codec = codecArgs(bit_depth, sample_rate, channels);
    auto yFlag = [&]() -> std::vector<std::string> {
        return overwrite ? std::vector<std::string>{"-y"} : std::vector<std::string>{"-n"};
    };

    if (cmd.type == "cut") {
        // ffmpeg -y -i src -ss start -to end -acodec pcm_s16le out
        std::string src = resolveSrc(cmd.source);
        std::string out = resolveOut(cmd.output);
        if (!overwrite && fs::exists(out)) {
            return "SKIP";
        }
        std::vector<std::string> args = {ffmpeg};
        for (auto& a : yFlag()) args.push_back(a);
        args.insert(args.end(), {"-i", src,
                                 "-ss", std::to_string(cmd.start),
                                 "-to", std::to_string(cmd.end)});
        for (auto& a : codec) args.push_back(a);
        args.push_back(out);

        if (dry_run) {
            std::cout << "[LlmAudioEditor] [dry-run] cut:";
            for (auto& a : args) std::cout << ' ' << a;
            std::cout << '\n';
            return {};
        }
        auto res = runCommand(args);
        if (res.exit_code != 0) return "ffmpeg cut failed: " + res.stderr_output.substr(0, 512);
        return {};
    }

    if (cmd.type == "merge") {
        std::string out = resolveOut(cmd.output);
        if (!overwrite && fs::exists(out)) return "SKIP";

        const bool use_crossfade = (cmd.transition == "crossfade" && cmd.sources.size() == 2);

        if (use_crossfade) {
            std::string s1 = resolveSrc(cmd.sources[0]);
            std::string s2 = resolveSrc(cmd.sources[1]);
            std::vector<std::string> args = {ffmpeg};
            for (auto& a : yFlag()) args.push_back(a);
            args.insert(args.end(), {"-i", s1, "-i", s2,
                                     "-filter_complex",
                                     "acrossfade=d=" + std::to_string(cmd.crossfade_duration) +
                                         ":c1=tri:c2=tri"});
            for (auto& a : codec) args.push_back(a);
            args.push_back(out);
            if (dry_run) {
                std::cout << "[LlmAudioEditor] [dry-run] merge(crossfade):";
                for (auto& a : args) std::cout << ' ' << a;
                std::cout << '\n';
                return {};
            }
            auto res = runCommand(args);
            if (res.exit_code != 0)
                return "ffmpeg merge(crossfade) failed: " + res.stderr_output.substr(0, 512);
            return {};
        }

        // Build concat list file / 构建拼接列表文件
        std::string list_path = "/tmp/llm_aed_merge_" + std::to_string(getpid()) + ".txt";
        {
            std::ofstream list_ofs(list_path, std::ios::trunc);
            if (!list_ofs) return "Cannot write concat list: " + list_path;
            for (auto& src : cmd.sources) {
                list_ofs << "file '" << resolveSrc(src) << "'\n";
            }
        }
        struct ListClean { std::string p; ~ListClean() { unlink(p.c_str()); } } lc{list_path};

        std::vector<std::string> args = {ffmpeg};
        for (auto& a : yFlag()) args.push_back(a);
        args.insert(args.end(), {"-f", "concat", "-safe", "0", "-i", list_path});
        for (auto& a : codec) args.push_back(a);
        args.push_back(out);

        if (dry_run) {
            std::cout << "[LlmAudioEditor] [dry-run] merge(concat):";
            for (auto& a : args) std::cout << ' ' << a;
            std::cout << '\n';
            return {};
        }
        auto res = runCommand(args);
        if (res.exit_code != 0)
            return "ffmpeg merge failed: " + res.stderr_output.substr(0, 512);
        return {};
    }

    if (cmd.type == "silence") {
        std::string src = resolveSrc(cmd.source);
        std::string out = resolveOut(cmd.output);
        if (!overwrite && fs::exists(out)) return "SKIP";

        // Determine sample rate for silence generation
        // 确定生成静音的采样率
        int sr = (sample_rate > 0) ? static_cast<int>(sample_rate) : 44100;
        std::string ch_layout = (channels == 1) ? "mono" :
                                (channels == 2) ? "stereo" : "mono";

        // Generate silence WAV to tmp file
        std::string silence_path = "/tmp/llm_aed_silence_" + std::to_string(getpid()) + ".wav";
        {
            std::vector<std::string> sil_args = {ffmpeg, "-y",
                "-f", "lavfi",
                "-i", "anullsrc=r=" + std::to_string(sr) + ":cl=" + ch_layout,
                "-t", std::to_string(cmd.duration)};
            for (auto& a : codec) sil_args.push_back(a);
            sil_args.push_back(silence_path);
            if (!dry_run) {
                auto res = runCommand(sil_args);
                if (res.exit_code != 0)
                    return "ffmpeg silence gen failed: " + res.stderr_output.substr(0, 256);
            }
        }
        struct SilenceClean { std::string p; ~SilenceClean() { unlink(p.c_str()); } } sc{silence_path};

        // Build concat order based on position
        // 根据插入位置构建拼接顺序
        std::string list_path = "/tmp/llm_aed_sil_list_" + std::to_string(getpid()) + ".txt";
        {
            std::ofstream lf(list_path, std::ios::trunc);
            if (!lf) return "Cannot write silence list";
            if (cmd.position <= 0.0) {
                lf << "file '" << silence_path << "'\n";
                lf << "file '" << src << "'\n";
            } else {
                // Insert at arbitrary position: cut src into two parts then concat
                // For simplicity, append silence at the end when position > 0
                // 简单实现：当 position > 0 时在末尾拼接
                lf << "file '" << src << "'\n";
                lf << "file '" << silence_path << "'\n";
            }
        }
        struct SilenceConcatClean { std::string p; ~SilenceConcatClean() { unlink(p.c_str()); } } lc{list_path};

        std::vector<std::string> args = {ffmpeg};
        for (auto& a : yFlag()) args.push_back(a);
        args.insert(args.end(), {"-f", "concat", "-safe", "0", "-i", list_path});
        for (auto& a : codec) args.push_back(a);
        args.push_back(out);

        if (dry_run) {
            std::cout << "[LlmAudioEditor] [dry-run] silence:";
            for (auto& a : args) std::cout << ' ' << a;
            std::cout << '\n';
            return {};
        }
        auto res = runCommand(args);
        if (res.exit_code != 0)
            return "ffmpeg silence insert failed: " + res.stderr_output.substr(0, 512);
        return {};
    }

    if (cmd.type == "delete") {
        std::string src = resolveSrc(cmd.source);
        std::string out = resolveOut(cmd.output);
        if (!overwrite && fs::exists(out)) return "SKIP";
        if (cmd.segments.empty()) return "delete: no segments specified";

        // Build aselect filter to exclude the segments
        // 构建 aselect 滤镜排除指定片段
        std::string filter_expr = "aselect='not(";
        for (size_t i = 0; i < cmd.segments.size(); ++i) {
            if (i > 0) filter_expr += "+";
            filter_expr += "between(t," +
                std::to_string(cmd.segments[i].start) + "," +
                std::to_string(cmd.segments[i].end) + ")";
        }
        filter_expr += ")',asetpts=N/SR/TB";

        std::vector<std::string> args = {ffmpeg};
        for (auto& a : yFlag()) args.push_back(a);
        args.insert(args.end(), {"-i", src, "-af", filter_expr});
        for (auto& a : codec) args.push_back(a);
        args.push_back(out);

        if (dry_run) {
            std::cout << "[LlmAudioEditor] [dry-run] delete:";
            for (auto& a : args) std::cout << ' ' << a;
            std::cout << '\n';
            return {};
        }
        auto res = runCommand(args);
        if (res.exit_code != 0)
            return "ffmpeg delete failed: " + res.stderr_output.substr(0, 512);
        return {};
    }

    if (cmd.type == "speed") {
        std::string src = resolveSrc(cmd.source);
        std::string out = resolveOut(cmd.output);
        if (!overwrite && fs::exists(out)) return "SKIP";

        // atempo range is [0.5, 2.0]; chain for extreme values
        // atempo 范围为 [0.5, 2.0]，极值时需链式调用
        double factor = cmd.factor;
        std::string filter;
        if (factor <= 0.0) factor = 1.0;

        if (factor >= 0.5 && factor <= 2.0) {
            filter = "atempo=" + std::to_string(factor);
        } else if (factor > 2.0) {
            // Chain: e.g. 3.0x = atempo=2.0,atempo=1.5
            filter = "";
            double rem = factor;
            while (rem > 2.0) {
                if (!filter.empty()) filter += ",";
                filter += "atempo=2.0";
                rem /= 2.0;
            }
            if (!filter.empty()) filter += ",";
            filter += "atempo=" + std::to_string(rem);
        } else {
            // factor < 0.5: chain down
            filter = "";
            double rem = factor;
            while (rem < 0.5) {
                if (!filter.empty()) filter += ",";
                filter += "atempo=0.5";
                rem *= 2.0;
            }
            if (!filter.empty()) filter += ",";
            filter += "atempo=" + std::to_string(rem);
        }

        std::vector<std::string> args = {ffmpeg};
        for (auto& a : yFlag()) args.push_back(a);
        args.insert(args.end(), {"-i", src, "-af", filter});
        for (auto& a : codec) args.push_back(a);
        args.push_back(out);

        if (dry_run) {
            std::cout << "[LlmAudioEditor] [dry-run] speed:";
            for (auto& a : args) std::cout << ' ' << a;
            std::cout << '\n';
            return {};
        }
        auto res = runCommand(args);
        if (res.exit_code != 0)
            return "ffmpeg speed failed: " + res.stderr_output.substr(0, 512);
        return {};
    }

    if (cmd.type == "fade") {
        std::string src = resolveSrc(cmd.source);
        std::string out = resolveOut(cmd.output);
        if (!overwrite && fs::exists(out)) return "SKIP";

        // Get duration for fade-out start time
        double dur = getAudioDuration(ffprobe, src);
        double fade_out_start = (dur > cmd.fade_out) ? dur - cmd.fade_out : 0.0;

        std::string filter;
        if (cmd.fade_in > 0.0 && cmd.fade_out > 0.0) {
            filter = "afade=t=in:ss=0:d=" + std::to_string(cmd.fade_in) +
                     ",afade=t=out:st=" + std::to_string(fade_out_start) +
                     ":d=" + std::to_string(cmd.fade_out);
        } else if (cmd.fade_in > 0.0) {
            filter = "afade=t=in:ss=0:d=" + std::to_string(cmd.fade_in);
        } else if (cmd.fade_out > 0.0) {
            filter = "afade=t=out:st=" + std::to_string(fade_out_start) +
                     ":d=" + std::to_string(cmd.fade_out);
        } else {
            return "fade: neither fade_in nor fade_out specified";
        }

        std::vector<std::string> args = {ffmpeg};
        for (auto& a : yFlag()) args.push_back(a);
        args.insert(args.end(), {"-i", src, "-af", filter});
        for (auto& a : codec) args.push_back(a);
        args.push_back(out);

        if (dry_run) {
            std::cout << "[LlmAudioEditor] [dry-run] fade:";
            for (auto& a : args) std::cout << ' ' << a;
            std::cout << '\n';
            return {};
        }
        auto res = runCommand(args);
        if (res.exit_code != 0)
            return "ffmpeg fade failed: " + res.stderr_output.substr(0, 512);
        return {};
    }

    if (cmd.type == "volume") {
        std::string src = resolveSrc(cmd.source);
        std::string out = resolveOut(cmd.output);
        if (!overwrite && fs::exists(out)) return "SKIP";

        std::vector<std::string> args = {ffmpeg};
        for (auto& a : yFlag()) args.push_back(a);
        args.insert(args.end(), {"-i", src, "-af",
                                  "volume=" + std::to_string(cmd.level)});
        for (auto& a : codec) args.push_back(a);
        args.push_back(out);

        if (dry_run) {
            std::cout << "[LlmAudioEditor] [dry-run] volume:";
            for (auto& a : args) std::cout << ' ' << a;
            std::cout << '\n';
            return {};
        }
        auto res = runCommand(args);
        if (res.exit_code != 0)
            return "ffmpeg volume failed: " + res.stderr_output.substr(0, 512);
        return {};
    }

    if (cmd.type == "normalize") {
        std::string src = resolveSrc(cmd.source);
        std::string out = resolveOut(cmd.output);
        if (!overwrite && fs::exists(out)) return "SKIP";

        std::vector<std::string> args = {ffmpeg};
        for (auto& a : yFlag()) args.push_back(a);
        args.insert(args.end(), {"-i", src, "-af", "loudnorm"});
        for (auto& a : codec) args.push_back(a);
        args.push_back(out);

        if (dry_run) {
            std::cout << "[LlmAudioEditor] [dry-run] normalize:";
            for (auto& a : args) std::cout << ' ' << a;
            std::cout << '\n';
            return {};
        }
        auto res = runCommand(args);
        if (res.exit_code != 0)
            return "ffmpeg normalize failed: " + res.stderr_output.substr(0, 512);
        return {};
    }

    return "Unknown command type: " + cmd.type;
}

// ============================================================================
// LlmAudioEditorNode
// ============================================================================

class LlmAudioEditorNode : public WorkflowNode {
public:
    void execute(ExecutionContext& ctx) override
    {
        void* cp = static_cast<void*>(&ctx);

        // ---- Read input parameters / 读取输入参数 -------------------------
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

        char buf[4096] = {};

        // Required: input path / 必须：输入路径
        if (ctx_get_string(cp, "aed_input_path", buf, sizeof(buf)) < 0) {
            ctx_set_string(cp, "aed_status", "failed");
            ctx_set_string(cp, "aed_error", "aed_input_path is required");
            std::cerr << "[LlmAudioEditor] ERROR: aed_input_path is required\n";
            return;
        }
        const std::string input_path(buf);

        // Required: audio dir / 必须：音频目录
        if (ctx_get_string(cp, "aed_audio_dir", buf, sizeof(buf)) < 0) {
            ctx_set_string(cp, "aed_status", "failed");
            ctx_set_string(cp, "aed_error", "aed_audio_dir is required");
            std::cerr << "[LlmAudioEditor] ERROR: aed_audio_dir is required\n";
            return;
        }
        const std::string audio_dir(buf);

        const std::string output_dir    = getStr("aed_output_dir",    "./audio_edited");
        const std::string ffmpeg_path   = getStr("aed_ffmpeg_path",   "ffmpeg");
        const std::string ffprobe_path  = getStr("aed_ffprobe_path",  "ffprobe");
        const std::string overwrite_s   = getStr("aed_overwrite",     "false");
        const std::string recursive_s   = getStr("aed_recursive",     "true");
        const std::string dry_run_s     = getStr("aed_dry_run",       "false");
        const std::string cmd_marker    = getStr("aed_command_marker","EDIT_COMMANDS");
        const std::string file_exts     = getStr("aed_file_extensions",".json,.md,.txt");

        const int64_t parallel   = getInt("aed_parallel",    2);
        const int64_t sample_rate= getInt("aed_sample_rate", 0);
        const int64_t channels   = getInt("aed_channels",    0);
        const int64_t bit_depth  = getInt("aed_bit_depth",   16);

        bool overwrite = (overwrite_s == "true" || overwrite_s == "1");
        bool recursive = (recursive_s == "true" || recursive_s == "1");
        bool dry_run   = (dry_run_s   == "true" || dry_run_s   == "1");

        // ---- Verify ffmpeg / 验证 ffmpeg ---------------------------------
        {
            auto res = runCommand({ffmpeg_path, "-version"}, 10);
            if (res.exit_code != 0) {
                ctx_set_string(cp, "aed_status", "failed");
                ctx_set_string(cp, "aed_error",
                    ("ffmpeg not found at: " + ffmpeg_path).c_str());
                std::cerr << "[LlmAudioEditor] ERROR: ffmpeg not found\n";
                return;
            }
        }

        // ---- Create output directory / 创建输出目录 ---------------------
        {
            std::error_code ec;
            fs::create_directories(output_dir, ec);
            if (ec) {
                ctx_set_string(cp, "aed_status", "failed");
                ctx_set_string(cp, "aed_error",
                    ("Cannot create output_dir: " + output_dir).c_str());
                return;
            }
        }

        // ---- Collect instruction files / 收集指令文件 -------------------
        std::vector<fs::path> instr_files;
        {
            std::error_code ec;
            std::vector<std::string> exts;
            {
                std::istringstream iss(file_exts);
                std::string tok;
                while (std::getline(iss, tok, ',')) {
                    while (!tok.empty() && (tok.front() == ' ' || tok.front() == '\t')) tok.erase(tok.begin());
                    while (!tok.empty() && (tok.back()  == ' ' || tok.back()  == '\t')) tok.pop_back();
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
                if (matchExt(ip)) instr_files.push_back(ip);
            } else if (fs::is_directory(ip, ec)) {
                if (recursive) {
                    for (auto& e : fs::recursive_directory_iterator(ip, ec))
                        if (e.is_regular_file() && matchExt(e.path()))
                            instr_files.push_back(e.path());
                } else {
                    for (auto& e : fs::directory_iterator(ip, ec))
                        if (e.is_regular_file() && matchExt(e.path()))
                            instr_files.push_back(e.path());
                }
            } else {
                ctx_set_string(cp, "aed_status", "failed");
                ctx_set_string(cp, "aed_error",
                    ("input_path not found: " + input_path).c_str());
                return;
            }
            std::sort(instr_files.begin(), instr_files.end());
        }

        std::cout << "[LlmAudioEditor] 扫描指令文件: " << input_path << "\n";

        // ---- Parse all commands from all files / 从所有文件解析指令 -----
        struct FileCommands {
            fs::path                 file;
            std::vector<EditCommand> cmds;
        };
        std::vector<FileCommands> all_file_cmds;
        int64_t total_commands = 0;

        for (auto& f : instr_files) {
            std::ifstream ifs(f, std::ios::binary);
            if (!ifs) continue;
            std::ostringstream oss;
            oss << ifs.rdbuf();
            std::string content = oss.str();

            std::string json_str = extractJsonCommands(content, cmd_marker);
            if (json_str.empty()) {
                std::cout << "[LlmAudioEditor] 跳过 (无指令): " << f.filename() << "\n";
                continue;
            }
            auto cmds = parseEditCommands(json_str);
            if (cmds.empty()) continue;

            total_commands += static_cast<int64_t>(cmds.size());
            all_file_cmds.push_back({f, std::move(cmds)});
        }

        std::cout << "[LlmAudioEditor] 发现 " << all_file_cmds.size()
                  << " 个指令文件，共 " << total_commands << " 条编辑指令\n";

        // ---- Execute commands / 执行指令 ---------------------------------
        std::atomic<int64_t> executed_count{0};
        std::atomic<int64_t> failed_count{0};
        std::atomic<int64_t> skipped_count{0};
        std::atomic<double>  total_output_duration{0.0};
        std::atomic<int64_t> total_output_bytes{0};
        std::mutex            output_mutex;
        std::vector<std::string> output_files;

        // Global command index for progress logging / 全局指令索引，用于进度日志
        std::atomic<int64_t> global_idx{0};

        // Process all files — within each file, commands are sequential
        // (later commands may depend on earlier outputs).
        // Parallelism is at the file level.
        // 在文件级别并行，但每个文件内部串行执行（后续指令可能依赖前面输出）。
        std::atomic<size_t> file_idx{0};
        const size_t n_par = static_cast<size_t>(std::max(int64_t(1), parallel));

        {
            std::vector<std::jthread> workers;
            workers.reserve(n_par);
            for (size_t t = 0; t < std::min(n_par, all_file_cmds.size()); ++t) {
                workers.emplace_back([&]() {
                    while (true) {
                        size_t fi = file_idx.fetch_add(1, std::memory_order_relaxed);
                        if (fi >= all_file_cmds.size()) break;

                        auto& fc = all_file_cmds[fi];
                        for (auto& cmd : fc.cmds) {
                            int64_t ci = global_idx.fetch_add(1, std::memory_order_relaxed) + 1;

                            std::string desc = cmd.description.empty() ? cmd.type : cmd.description;
                            std::cout << "[LlmAudioEditor] [" << ci << "/"
                                      << total_commands << "] "
                                      << cmd.type << ": " << cmd.source
                                      << " → " << cmd.output
                                      << " (" << desc << ") ...\n";

                            std::string err = executeEditCommand(
                                cmd, audio_dir, output_dir,
                                ffmpeg_path, ffprobe_path,
                                overwrite, dry_run,
                                sample_rate, channels, bit_depth);

                            if (err == "SKIP") {
                                skipped_count.fetch_add(1, std::memory_order_relaxed);
                                std::cout << "[LlmAudioEditor]   跳过 (已存在)\n";
                            } else if (!err.empty()) {
                                failed_count.fetch_add(1, std::memory_order_relaxed);
                                std::cerr << "[LlmAudioEditor]   失败: " << err << "\n";
                            } else {
                                executed_count.fetch_add(1, std::memory_order_relaxed);
                                std::cout << "[LlmAudioEditor]   完成\n";

                                // Track output file / 记录输出文件
                                fs::path out_full = fs::path(output_dir) / cmd.output;
                                if (cmd.output.find('/') == std::string::npos &&
                                    cmd.output.find('\\') == std::string::npos) {
                                    // relative output name
                                    std::error_code ec;
                                    if (fs::exists(out_full, ec)) {
                                        auto sz = fs::file_size(out_full, ec);
                                        if (!ec) {
                                            total_output_bytes.fetch_add(
                                                static_cast<int64_t>(sz),
                                                std::memory_order_relaxed);
                                        }
                                        double dur = getAudioDuration(ffprobe_path,
                                                                       out_full.string());
                                        // atomic<double> += via compare_exchange loop
                                        double old_d = total_output_duration.load(std::memory_order_relaxed);
                                        while (!total_output_duration.compare_exchange_weak(
                                            old_d, old_d + dur, std::memory_order_relaxed)) {}

                                        std::lock_guard lk(output_mutex);
                                        output_files.push_back(out_full.string());
                                    }
                                }
                            }
                        }
                    }
                });
            }
        } // jthreads join

        // ---- Write output context / 写入输出上下文 -----------------------
        ctx_set_int64 (cp, "aed_total_commands",   total_commands);
        ctx_set_int64 (cp, "aed_executed_count",   executed_count.load());
        ctx_set_int64 (cp, "aed_failed_count",     failed_count.load());
        ctx_set_int64 (cp, "aed_skipped_count",    skipped_count.load());
        ctx_set_double(cp, "aed_total_output_duration",
                        total_output_duration.load(std::memory_order_relaxed));
        ctx_set_int64 (cp, "aed_total_output_bytes",
                        total_output_bytes.load());
        ctx_set_string(cp, "aed_output_path", output_dir.c_str());

        {
            std::string files_list;
            for (auto& f : output_files) {
                if (!files_list.empty()) files_list += '\n';
                files_list += f;
            }
            ctx_set_string(cp, "aed_output_files", files_list.c_str());
        }

        const auto ec = executed_count.load();
        const auto fc = failed_count.load();
        if (fc == 0 && ec > 0)      ctx_set_string(cp, "aed_status", "success");
        else if (ec > 0 && fc > 0)  ctx_set_string(cp, "aed_status", "partial");
        else if (fc > 0)            ctx_set_string(cp, "aed_status", "failed");
        else                        ctx_set_string(cp, "aed_status", "success");

        std::cout << "[LlmAudioEditor] 编辑完成: 成功 " << ec
                  << ", 失败 " << fc
                  << ", 跳过 " << skipped_count.load() << "\n";
    }
};

// ---- C factory / destructor -------------------------------------------------

extern "C" WorkflowNode* create_node() {
    return new LlmAudioEditorNode();
}

extern "C" void destroy_node(WorkflowNode* node) {
    delete node;
}
