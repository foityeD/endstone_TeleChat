#include "tg_chat_bridge.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <utility>
#include <vector>

#include <endstone/event/event_priority.h>
#include <endstone/event/player/player_chat_event.h>
#include <endstone/logger.h>
#include <endstone/player.h>
#include <endstone/scheduler/scheduler.h>
#include <endstone/server.h>

#include <SimpleIni.h>

#include <windows.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")

namespace {

struct JsonValue {
    enum class Type {
        Null,
        Bool,
        Number,
        String,
        Object,
        Array,
    } type = Type::Null;

    bool boolean = false;
    double number = 0.0;
    long long integer = 0;
    bool is_integer = false;
    std::string str;
    std::vector<JsonValue> array;
    std::unordered_map<std::string, JsonValue> object;
};

class JsonParser {
public:
    explicit JsonParser(const std::string &input) : input_(input) {}

    bool parse(JsonValue &out, std::string &error)
    {
        skipWhitespace();
        if (!parseValue(out, error)) {
            return false;
        }
        skipWhitespace();
        if (pos_ != input_.size()) {
            error = "Trailing characters after JSON value";
            return false;
        }
        return true;
    }

private:
    void skipWhitespace()
    {
        while (pos_ < input_.size() && std::isspace(static_cast<unsigned char>(input_[pos_]))) {
            ++pos_;
        }
    }

    bool parseValue(JsonValue &out, std::string &error)
    {
        if (pos_ >= input_.size()) {
            error = "Unexpected end of input";
            return false;
        }

        char c = input_[pos_];
        if (c == 'n') {
            return parseLiteral(out, error, "null", JsonValue::Type::Null);
        }
        if (c == 't') {
            return parseLiteral(out, error, "true", JsonValue::Type::Bool, true);
        }
        if (c == 'f') {
            return parseLiteral(out, error, "false", JsonValue::Type::Bool, false);
        }
        if (c == '"') {
            out.type = JsonValue::Type::String;
            return parseString(out.str, error);
        }
        if (c == '{') {
            return parseObject(out, error);
        }
        if (c == '[') {
            return parseArray(out, error);
        }
        if (c == '-' || (c >= '0' && c <= '9')) {
            return parseNumber(out, error);
        }

        error = "Unexpected character in JSON";
        return false;
    }

    bool parseLiteral(JsonValue &out, std::string &error, const char *literal, JsonValue::Type type, bool boolean = false)
    {
        size_t len = std::strlen(literal);
        if (input_.substr(pos_, len) != literal) {
            error = "Invalid JSON literal";
            return false;
        }
        pos_ += len;
        out.type = type;
        out.boolean = boolean;
        return true;
    }

    static void appendUtf8(std::string &out, uint32_t codepoint)
    {
        if (codepoint <= 0x7F) {
            out.push_back(static_cast<char>(codepoint));
        }
        else if (codepoint <= 0x7FF) {
            out.push_back(static_cast<char>(0xC0 | ((codepoint >> 6) & 0x1F)));
            out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
        }
        else if (codepoint <= 0xFFFF) {
            out.push_back(static_cast<char>(0xE0 | ((codepoint >> 12) & 0x0F)));
            out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
        }
        else {
            out.push_back(static_cast<char>(0xF0 | ((codepoint >> 18) & 0x07)));
            out.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
        }
    }

    bool parseString(std::string &out, std::string &error)
    {
        if (input_[pos_] != '"') {
            error = "Expected string";
            return false;
        }
        ++pos_;
        out.clear();
        while (pos_ < input_.size()) {
            char c = input_[pos_++];
            if (c == '"') {
                return true;
            }
            if (c == '\\') {
                if (pos_ >= input_.size()) {
                    error = "Invalid escape sequence";
                    return false;
                }
                char esc = input_[pos_++];
                switch (esc) {
                case '"':
                case '\\':
                case '/':
                    out.push_back(esc);
                    break;
                case 'b':
                    out.push_back('\b');
                    break;
                case 'f':
                    out.push_back('\f');
                    break;
                case 'n':
                    out.push_back('\n');
                    break;
                case 'r':
                    out.push_back('\r');
                    break;
                case 't':
                    out.push_back('\t');
                    break;
                case 'u': {
                    if (pos_ + 4 > input_.size()) {
                        error = "Invalid unicode escape";
                        return false;
                    }
                    auto hexValue = [&](char h, bool &ok) -> uint32_t {
                        if (h >= '0' && h <= '9') return h - '0';
                        if (h >= 'a' && h <= 'f') return 10 + (h - 'a');
                        if (h >= 'A' && h <= 'F') return 10 + (h - 'A');
                        ok = false;
                        return 0;
                    };

                    bool ok = true;
                    uint32_t code = 0;
                    for (int i = 0; i < 4; ++i) {
                        code = (code << 4) | hexValue(input_[pos_ + i], ok);
                    }
                    if (!ok) {
                        error = "Invalid unicode escape";
                        return false;
                    }
                    pos_ += 4;

                    if (code >= 0xD800 && code <= 0xDBFF) {
                        if (pos_ + 6 <= input_.size() && input_[pos_] == '\\' && input_[pos_ + 1] == 'u') {
                            pos_ += 2;
                            bool ok2 = true;
                            uint32_t low = 0;
                            for (int i = 0; i < 4; ++i) {
                                low = (low << 4) | hexValue(input_[pos_ + i], ok2);
                            }
                            if (ok2 && low >= 0xDC00 && low <= 0xDFFF) {
                                pos_ += 4;
                                uint32_t full = 0x10000 + (((code - 0xD800) << 10) | (low - 0xDC00));
                                appendUtf8(out, full);
                                break;
                            }
                            error = "Invalid unicode surrogate";
                            return false;
                        }
                        error = "Invalid unicode surrogate";
                        return false;
                    }

                    appendUtf8(out, code);
                    break;
                }
                default:
                    error = "Invalid escape sequence";
                    return false;
                }
                continue;
            }
            out.push_back(c);
        }
        error = "Unterminated string";
        return false;
    }

    bool parseNumber(JsonValue &out, std::string &error)
    {
        const char *start = input_.c_str() + pos_;
        char *end = nullptr;
        double value = std::strtod(start, &end);
        if (end == start) {
            error = "Invalid number";
            return false;
        }
        size_t len = static_cast<size_t>(end - start);
        bool is_integer = true;
        for (size_t i = 0; i < len; ++i) {
            char c = start[i];
            if (c == '.' || c == 'e' || c == 'E') {
                is_integer = false;
                break;
            }
        }
        long long int_value = 0;
        if (is_integer) {
            char *end_int = nullptr;
            int_value = std::strtoll(start, &end_int, 10);
            if (end_int != end) {
                is_integer = false;
            }
        }

        pos_ = static_cast<size_t>(end - input_.c_str());
        out.type = JsonValue::Type::Number;
        out.number = value;
        out.is_integer = is_integer;
        out.integer = int_value;
        return true;
    }

    bool parseArray(JsonValue &out, std::string &error)
    {
        if (input_[pos_] != '[') {
            error = "Expected '['";
            return false;
        }
        ++pos_;
        skipWhitespace();
        out.type = JsonValue::Type::Array;
        out.array.clear();

        if (pos_ < input_.size() && input_[pos_] == ']') {
            ++pos_;
            return true;
        }

        while (pos_ < input_.size()) {
            JsonValue element;
            if (!parseValue(element, error)) {
                return false;
            }
            out.array.push_back(std::move(element));
            skipWhitespace();
            if (pos_ < input_.size() && input_[pos_] == ',') {
                ++pos_;
                skipWhitespace();
                continue;
            }
            if (pos_ < input_.size() && input_[pos_] == ']') {
                ++pos_;
                return true;
            }
            error = "Expected ',' or ']'";
            return false;
        }
        error = "Unterminated array";
        return false;
    }

    bool parseObject(JsonValue &out, std::string &error)
    {
        if (input_[pos_] != '{') {
            error = "Expected '{'";
            return false;
        }
        ++pos_;
        skipWhitespace();
        out.type = JsonValue::Type::Object;
        out.object.clear();

        if (pos_ < input_.size() && input_[pos_] == '}') {
            ++pos_;
            return true;
        }

        while (pos_ < input_.size()) {
            std::string key;
            if (!parseString(key, error)) {
                return false;
            }
            skipWhitespace();
            if (pos_ >= input_.size() || input_[pos_] != ':') {
                error = "Expected ':'";
                return false;
            }
            ++pos_;
            skipWhitespace();
            JsonValue value;
            if (!parseValue(value, error)) {
                return false;
            }
            out.object.emplace(std::move(key), std::move(value));
            skipWhitespace();
            if (pos_ < input_.size() && input_[pos_] == ',') {
                ++pos_;
                skipWhitespace();
                continue;
            }
            if (pos_ < input_.size() && input_[pos_] == '}') {
                ++pos_;
                return true;
            }
            error = "Expected ',' or '}'";
            return false;
        }
        error = "Unterminated object";
        return false;
    }

    const std::string &input_;
    size_t pos_ = 0;
};

const JsonValue *getField(const JsonValue &obj, const char *key)
{
    if (obj.type != JsonValue::Type::Object) {
        return nullptr;
    }
    auto it = obj.object.find(key);
    if (it == obj.object.end()) {
        return nullptr;
    }
    return &it->second;
}

struct TgUpdateParsed {
    long long update_id = 0;
    std::string text;
    std::string username;
    std::string chat_id;
    std::string chat_username;
    bool from_bot = false;
};

bool parseTelegramUpdates(const std::string &json, std::vector<TgUpdateParsed> &updates, std::string &error)
{
    JsonValue root;
    JsonParser parser(json);
    if (!parser.parse(root, error)) {
        return false;
    }
    const JsonValue *ok = getField(root, "ok");
    if (!ok || ok->type != JsonValue::Type::Bool || !ok->boolean) {
        error = "Telegram API response not ok";
        return false;
    }
    const JsonValue *result = getField(root, "result");
    if (!result || result->type != JsonValue::Type::Array) {
        updates.clear();
        return true;
    }

    updates.clear();
    for (const auto &item : result->array) {
        if (item.type != JsonValue::Type::Object) {
            continue;
        }
        TgUpdateParsed parsed;
        const JsonValue *update_id = getField(item, "update_id");
        if (update_id && update_id->type == JsonValue::Type::Number) {
            if (update_id->is_integer) {
                parsed.update_id = update_id->integer;
            }
            else {
                parsed.update_id = static_cast<long long>(update_id->number);
            }
        }
        const JsonValue *message = getField(item, "message");
        if (!message || message->type != JsonValue::Type::Object) {
            continue;
        }
        const JsonValue *text = getField(*message, "text");
        if (!text || text->type != JsonValue::Type::String) {
            continue;
        }
        parsed.text = text->str;

        const JsonValue *chat = getField(*message, "chat");
        if (chat && chat->type == JsonValue::Type::Object) {
            const JsonValue *chat_id = getField(*chat, "id");
            if (chat_id) {
                if (chat_id->type == JsonValue::Type::Number) {
                    if (chat_id->is_integer) {
                        parsed.chat_id = std::to_string(chat_id->integer);
                    }
                    else {
                        parsed.chat_id = std::to_string(static_cast<long long>(chat_id->number));
                    }
                }
                else if (chat_id->type == JsonValue::Type::String) {
                    parsed.chat_id = chat_id->str;
                }
            }
            const JsonValue *chat_username = getField(*chat, "username");
            if (chat_username && chat_username->type == JsonValue::Type::String) {
                parsed.chat_username = chat_username->str;
            }
        }

        const JsonValue *from = getField(*message, "from");
        if (from && from->type == JsonValue::Type::Object) {
            const JsonValue *is_bot = getField(*from, "is_bot");
            if (is_bot && is_bot->type == JsonValue::Type::Bool) {
                parsed.from_bot = is_bot->boolean;
            }
            const JsonValue *username = getField(*from, "username");
            if (username && username->type == JsonValue::Type::String) {
                parsed.username = username->str;
            }
            if (parsed.username.empty()) {
                const JsonValue *first_name = getField(*from, "first_name");
                if (first_name && first_name->type == JsonValue::Type::String) {
                    parsed.username = first_name->str;
                }
            }
        }

        updates.push_back(std::move(parsed));
    }

    return true;
}

std::wstring utf8ToWide(const std::string &text)
{
    if (text.empty()) {
        return std::wstring();
    }
    int len = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (len <= 0) {
        return std::wstring();
    }
    std::wstring wide(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), len);
    return wide;
}

std::string wideToUtf8(const std::wstring &text)
{
    if (text.empty()) {
        return std::string();
    }
    int len = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (len <= 0) {
        return std::string();
    }
    std::string out(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), len, nullptr, nullptr);
    return out;
}

std::string urlEncode(const std::string &value)
{
    static const char hex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(value.size());
    for (unsigned char c : value) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
            c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        }
        else {
            out.push_back('%');
            out.push_back(hex[(c >> 4) & 0xF]);
            out.push_back(hex[c & 0xF]);
        }
    }
    return out;
}

bool httpGet(const std::wstring &path, std::string &response_body, int timeout_ms, int &status_code, std::string &error)
{
    response_body.clear();
    status_code = 0;

    HINTERNET session = WinHttpOpen(L"EndstoneTgChatBridge/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        error = "WinHttpOpen failed";
        return false;
    }

    WinHttpSetTimeouts(session, timeout_ms, timeout_ms, timeout_ms, timeout_ms);

    HINTERNET connect = WinHttpConnect(session, L"api.telegram.org", INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!connect) {
        error = "WinHttpConnect failed";
        WinHttpCloseHandle(session);
        return false;
    }

    HINTERNET request = WinHttpOpenRequest(connect, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!request) {
        error = "WinHttpOpenRequest failed";
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return false;
    }

    BOOL ok = WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
    if (!ok) {
        error = "WinHttpSendRequest failed";
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return false;
    }

    ok = WinHttpReceiveResponse(request, nullptr);
    if (!ok) {
        error = "WinHttpReceiveResponse failed";
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return false;
    }

    DWORD status = 0;
    DWORD status_size = sizeof(status);
    if (WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                            &status, &status_size, WINHTTP_NO_HEADER_INDEX)) {
        status_code = static_cast<int>(status);
    }

    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request, &available)) {
            error = "WinHttpQueryDataAvailable failed";
            break;
        }
        if (available == 0) {
            break;
        }
        std::string buffer;
        buffer.resize(available);
        DWORD read = 0;
        if (!WinHttpReadData(request, buffer.data(), available, &read)) {
            error = "WinHttpReadData failed";
            break;
        }
        buffer.resize(read);
        response_body.append(buffer);
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);

    if (!error.empty()) {
        return false;
    }
    return true;
}

bool httpPost(const std::wstring &path, const std::string &body, std::string &response_body, int timeout_ms,
              int &status_code, std::string &error)
{
    response_body.clear();
    status_code = 0;

    HINTERNET session = WinHttpOpen(L"EndstoneTgChatBridge/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        error = "WinHttpOpen failed";
        return false;
    }

    WinHttpSetTimeouts(session, timeout_ms, timeout_ms, timeout_ms, timeout_ms);

    HINTERNET connect = WinHttpConnect(session, L"api.telegram.org", INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!connect) {
        error = "WinHttpConnect failed";
        WinHttpCloseHandle(session);
        return false;
    }

    HINTERNET request = WinHttpOpenRequest(connect, L"POST", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!request) {
        error = "WinHttpOpenRequest failed";
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return false;
    }

    const wchar_t *headers = L"Content-Type: application/x-www-form-urlencoded\r\n";
    BOOL ok = WinHttpSendRequest(request, headers, -1L, (LPVOID)body.data(),
                                 static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0);
    if (!ok) {
        error = "WinHttpSendRequest failed";
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return false;
    }

    ok = WinHttpReceiveResponse(request, nullptr);
    if (!ok) {
        error = "WinHttpReceiveResponse failed";
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return false;
    }

    DWORD status = 0;
    DWORD status_size = sizeof(status);
    if (WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                            &status, &status_size, WINHTTP_NO_HEADER_INDEX)) {
        status_code = static_cast<int>(status);
    }

    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request, &available)) {
            error = "WinHttpQueryDataAvailable failed";
            break;
        }
        if (available == 0) {
            break;
        }
        std::string buffer;
        buffer.resize(available);
        DWORD read = 0;
        if (!WinHttpReadData(request, buffer.data(), available, &read)) {
            error = "WinHttpReadData failed";
            break;
        }
        buffer.resize(read);
        response_body.append(buffer);
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);

    if (!error.empty()) {
        return false;
    }
    return true;
}

class TelegramClient {
public:
    TelegramClient(std::string token, int timeout_ms) : token_(std::move(token)), timeout_ms_(timeout_ms) {}

    bool sendMessage(const std::string &chat_id, const std::string &text, std::string &error)
    {
        std::string path = "/bot" + token_ + "/sendMessage";
        std::string body = "chat_id=" + urlEncode(chat_id) + "&text=" + urlEncode(text) +
                           "&disable_web_page_preview=true";
        return requestOkPost(path, body, error);
    }

    bool getUpdates(long long offset, int timeout_sec, std::vector<TgUpdateParsed> &updates, std::string &error)
    {
        std::ostringstream oss;
        oss << "/bot" << token_ << "/getUpdates?timeout=" << timeout_sec << "&allowed_updates=%5B%22message%22%5D";
        if (offset > 0) {
            oss << "&offset=" << offset;
        }
        std::string body;
        int status = 0;
        std::string http_error;
        if (!httpGet(utf8ToWide(oss.str()), body, timeout_ms_, status, http_error)) {
            error = http_error;
            return false;
        }
        if (status != 200) {
            error = "HTTP status " + std::to_string(status);
            return false;
        }
        return parseTelegramUpdates(body, updates, error);
    }

private:
    bool requestOk(const std::string &path, std::string &error)
    {
        std::string body;
        int status = 0;
        std::string http_error;
        if (!httpGet(utf8ToWide(path), body, timeout_ms_, status, http_error)) {
            error = http_error;
            return false;
        }
        if (status != 200) {
            error = "HTTP status " + std::to_string(status);
            return false;
        }
        JsonValue root;
        JsonParser parser(body);
        if (!parser.parse(root, error)) {
            return false;
        }
        const JsonValue *ok = getField(root, "ok");
        if (!ok || ok->type != JsonValue::Type::Bool || !ok->boolean) {
            error = "Telegram API response not ok";
            return false;
        }
        return true;
    }

    bool requestOkPost(const std::string &path, const std::string &post_body, std::string &error)
    {
        std::string body;
        int status = 0;
        std::string http_error;
        if (!httpPost(utf8ToWide(path), post_body, body, timeout_ms_, status, http_error)) {
            error = http_error;
            return false;
        }
        if (status != 200) {
            error = "HTTP status " + std::to_string(status);
            return false;
        }
        JsonValue root;
        JsonParser parser(body);
        if (!parser.parse(root, error)) {
            return false;
        }
        const JsonValue *ok = getField(root, "ok");
        if (!ok || ok->type != JsonValue::Type::Bool || !ok->boolean) {
            error = "Telegram API response not ok";
            return false;
        }
        return true;
    }

    std::string token_;
    int timeout_ms_ = 15000;
};

int parseInt(const char *value, int fallback)
{
    if (!value || !*value) {
        return fallback;
    }
    char *end = nullptr;
    long parsed = std::strtol(value, &end, 10);
    if (end == value) {
        return fallback;
    }
    return static_cast<int>(parsed);
}

bool parseBool(const char *value, bool fallback)
{
    if (!value) {
        return fallback;
    }
    std::string s(value);
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (s == "1" || s == "true" || s == "yes" || s == "on") {
        return true;
    }
    if (s == "0" || s == "false" || s == "no" || s == "off") {
        return false;
    }
    return fallback;
}

std::string trimCopy(std::string value)
{
    size_t start = 0;
    while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start]))) {
        ++start;
    }
    size_t end = value.size();
    while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1]))) {
        --end;
    }
    return value.substr(start, end - start);
}

std::string stripQuotes(std::string value)
{
    if (value.size() >= 2) {
        char first = value.front();
        char last = value.back();
        if ((first == '"' && last == '"') || (first == '\'' && last == '\'')) {
            return value.substr(1, value.size() - 2);
        }
    }
    return value;
}

std::string toLowerCopy(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

void replaceAll(std::string &text, const std::string &from, const std::string &to)
{
    if (from.empty()) {
        return;
    }
    size_t start = 0;
    while ((start = text.find(from, start)) != std::string::npos) {
        text.replace(start, from.length(), to);
        start += to.length();
    }
}

}  // namespace

void TgChatBridge::onLoad()
{
    loadOrCreateConfig();
    getLogger().info("Config loaded");
}

void TgChatBridge::onEnable()
{
    getLogger().info("Enabling Telegram chat bridge");
    registerEvent(&TgChatBridge::onPlayerChat, *this, endstone::EventPriority::Normal, true);

    getServer().getScheduler().runTaskTimer(*this, [this]() { flushIncoming(); }, 1, config_.incoming_flush_ticks);

    startWorker();
}

void TgChatBridge::onDisable()
{
    getLogger().info("Disabling Telegram chat bridge");
    stopWorker();
    getServer().getScheduler().cancelTasks(*this);
}

void TgChatBridge::onPlayerChat(endstone::PlayerChatEvent &event)
{
    if (!telegram_enabled_) {
        return;
    }

    const std::string player = event.getPlayer().getName();
    const std::string message = event.getMessage();
    if (config_.ignore_commands) {
        std::string trimmed = trimCopy(message);
        if (!trimmed.empty() && trimmed.front() == '/') {
            return;
        }
    }
    std::string formatted = applyTemplate(config_.game_to_tg_format, player, std::string(), message);

    bool dropped = false;
    formatted = enforceLimit(formatted, config_.max_game_to_tg, dropped);
    if (dropped) {
        getLogger().warning("Telegram message dropped due to length limit");
        return;
    }

    {
        std::lock_guard<std::mutex> lock(outgoing_mutex_);
        outgoing_queue_.push_back(std::move(formatted));
    }
    outgoing_cv_.notify_one();
}

void TgChatBridge::loadOrCreateConfig()
{
    config_ = Config{};

    std::filesystem::create_directories(getDataFolder());
    std::filesystem::path config_path = getDataFolder() / "config.ini";

    if (!std::filesystem::exists(config_path)) {
        std::ofstream out(config_path, std::ios::out | std::ios::trunc);
        if (out) {
            out <<
                "[telegram]\n"
                "; Telegram bot token\n"
                "bot_token = PUT_YOUR_BOT_TOKEN_HERE\n"
                "\n"
                "; Target chat id or channelusername\n"
                "chat_id = PUT_CHAT_ID_HERE\n"
                "\n"
                "; Long polling timeout in seconds\n"
                "poll_timeout_sec = 25\n"
                "\n"
                "; Sleep after failed poll (ms)\n"
                "poll_interval_ms = 1000\n"
                "\n"
                "; HTTP request timeout in ms\n"
                "request_timeout_ms = 15000\n"
                "\n"
                "[format]\n"
                "; Message format for Telegram output\n"
                "game_to_telegram = {Player_name}: {message}\n"
                "\n"
                "; Message format for in-game output\n"
                "telegram_to_game = [TG] {Username}: {message}\n"
                "\n"
                "[limits]\n"
                "; 0 or negative means no limit\n"
                "max_game_to_telegram = 256\n"
                "\n"
                "; 0 or negative means no limit\n"
                "max_telegram_to_game = 256\n"
                "\n"
                "; If false, over-limit messages are dropped\n"
                "truncate_over_limit = true\n"
                "\n"
                "[runtime]\n"
                "; How often to flush Telegram messages to game (ticks)\n"
                "incoming_flush_ticks = 20\n"
                "\n"
                "; Log Telegram API errors\n"
                "log_telegram_errors = true\n"
                "\n"
                "[filters]\n"
                "; Ignore slash commands (e.g. /tell)\n"
                "ignore_commands = true\n";
            if (!out.good()) {
                getLogger().warning("Failed to write config.ini");
            }
        }
        else {
            getLogger().warning("Failed to create config.ini");
        }
    }

    CSimpleIniA ini;
    ini.SetUnicode();
    if (ini.LoadFile(config_path.string().c_str()) < 0) {
        getLogger().warning("Failed to read config.ini, using defaults");
    }

    config_.bot_token = trimCopy(stripQuotes(ini.GetValue("telegram", "bot_token", "")));
    config_.chat_id = trimCopy(stripQuotes(ini.GetValue("telegram", "chat_id", "")));
    config_.poll_timeout_sec = parseInt(ini.GetValue("telegram", "poll_timeout_sec", "25"), 25);
    config_.poll_interval_ms = parseInt(ini.GetValue("telegram", "poll_interval_ms", "1000"), 1000);
    config_.request_timeout_ms = parseInt(ini.GetValue("telegram", "request_timeout_ms", "15000"), 15000);

    config_.game_to_tg_format = stripQuotes(ini.GetValue("format", "game_to_telegram", "{Player_name}: {message}"));
    config_.tg_to_game_format = stripQuotes(ini.GetValue("format", "telegram_to_game", "[TG] {Username}: {message}"));

    config_.max_game_to_tg = parseInt(ini.GetValue("limits", "max_game_to_telegram", "256"), 256);
    config_.max_tg_to_game = parseInt(ini.GetValue("limits", "max_telegram_to_game", "256"), 256);
    config_.truncate_over_limit = parseBool(ini.GetValue("limits", "truncate_over_limit", "true"), true);

    config_.incoming_flush_ticks = parseInt(ini.GetValue("runtime", "incoming_flush_ticks", "20"), 20);
    config_.log_telegram_errors = parseBool(ini.GetValue("runtime", "log_telegram_errors", "true"), true);
    config_.ignore_commands = parseBool(ini.GetValue("filters", "ignore_commands", "true"), true);

    if (config_.incoming_flush_ticks <= 0) {
        getLogger().warning("incoming_flush_ticks must be > 0, using 1");
        config_.incoming_flush_ticks = 1;
    }
    if (config_.poll_timeout_sec < 0) {
        config_.poll_timeout_sec = 0;
    }
    if (config_.poll_interval_ms <= 0) {
        config_.poll_interval_ms = 1000;
    }
    if (config_.request_timeout_ms <= 0) {
        config_.request_timeout_ms = 15000;
    }

    int min_timeout = config_.poll_timeout_sec * 1000 + 1000;
    if (config_.request_timeout_ms < min_timeout) {
        getLogger().warning("request_timeout_ms too small for long poll, using {}", min_timeout);
        config_.request_timeout_ms = min_timeout;
    }

    if (config_.game_to_tg_format.find("{message}") == std::string::npos) {
        getLogger().warning("game_to_telegram format missing {message}");
    }
    if (config_.tg_to_game_format.find("{message}") == std::string::npos) {
        getLogger().warning("telegram_to_game format missing {message}");
    }

    telegram_enabled_ = !config_.bot_token.empty() && !config_.chat_id.empty() &&
                        config_.bot_token != "PUT_YOUR_BOT_TOKEN_HERE" && config_.chat_id != "PUT_CHAT_ID_HERE";

    if (!telegram_enabled_) {
        getLogger().warning("Telegram is not configured. Set bot_token and chat_id in config.ini");
    }
}

void TgChatBridge::startWorker()
{
    if (!telegram_enabled_) {
        return;
    }
    running_.store(true);
    skip_existing_updates_ = true;
    worker_ = std::thread([this]() { workerLoop(); });
    sender_ = std::thread([this]() { senderLoop(); });
}

void TgChatBridge::stopWorker()
{
    running_.store(false);
    outgoing_cv_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
    if (sender_.joinable()) {
        sender_.join();
    }
}

void TgChatBridge::workerLoop()
{
    TelegramClient client(config_.bot_token, config_.request_timeout_ms);
    long long offset = 0;
    bool skip_existing = skip_existing_updates_;

    while (running_.load()) {
        std::vector<TgUpdateParsed> updates;
        std::string error;
        if (!client.getUpdates(offset, config_.poll_timeout_sec, updates, error)) {
            if (config_.log_telegram_errors) {
                getLogger().warning("Telegram getUpdates failed: {}", error);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(config_.poll_interval_ms));
            continue;
        }

        if (skip_existing) {
            for (const auto &update : updates) {
                offset = std::max(offset, update.update_id + 1);
            }
            skip_existing = false;
            continue;
        }

        for (const auto &update : updates) {
            offset = std::max(offset, update.update_id + 1);
            if (update.from_bot || update.text.empty()) {
                continue;
            }

            bool chat_matches = false;
            if (config_.chat_id.empty()) {
                chat_matches = true;
            }
            else if (!config_.chat_id.empty() && config_.chat_id[0] == '@') {
                if (!update.chat_username.empty()) {
                    std::string left = toLowerCopy("@" + update.chat_username);
                    std::string right = toLowerCopy(config_.chat_id);
                    if (left == right) {
                        chat_matches = true;
                    }
                }
                else {
                    chat_matches = false;
                }
            }
            else if (update.chat_id == config_.chat_id) {
                chat_matches = true;
            }

            if (!chat_matches) {
                continue;
            }

            TgIncomingMessage incoming;
            incoming.username = update.username.empty() ? "Unknown" : update.username;
            incoming.text = update.text;
            incoming.chat_id = update.chat_id;
            incoming.chat_username = update.chat_username;

            {
                std::lock_guard<std::mutex> lock(incoming_mutex_);
                incoming_queue_.push_back(std::move(incoming));
            }
        }
    }
}

void TgChatBridge::senderLoop()
{
    TelegramClient client(config_.bot_token, config_.request_timeout_ms);

    while (running_.load()) {
        std::unique_lock<std::mutex> lock(outgoing_mutex_);
        outgoing_cv_.wait(lock, [this]() { return !running_.load() || !outgoing_queue_.empty(); });
        if (!running_.load()) {
            return;
        }

        std::deque<std::string> batch;
        batch.swap(outgoing_queue_);
        lock.unlock();

        for (auto &msg : batch) {
            std::string error;
            if (!client.sendMessage(config_.chat_id, msg, error) && config_.log_telegram_errors) {
                getLogger().warning("Telegram sendMessage failed: {}", error);
            }
        }
    }
}

void TgChatBridge::flushIncoming()
{
    std::deque<TgIncomingMessage> local;
    {
        std::lock_guard<std::mutex> lock(incoming_mutex_);
        local.swap(incoming_queue_);
    }

    for (const auto &msg : local) {
        std::string formatted = applyTemplate(config_.tg_to_game_format, std::string(), msg.username, msg.text);
        bool dropped = false;
        formatted = enforceLimit(formatted, config_.max_tg_to_game, dropped);
        if (dropped) {
            getLogger().warning("Telegram message dropped due to length limit");
            continue;
        }
        getServer().broadcastMessage(formatted);
    }
}

std::string TgChatBridge::applyTemplate(const std::string &templ, const std::string &player, const std::string &username,
                                        const std::string &message) const
{
    std::string out = templ;
    replaceAll(out, "{Player_name}", player);
    replaceAll(out, "{Username}", username);
    replaceAll(out, "{message}", message);
    return out;
}

std::string TgChatBridge::enforceLimit(const std::string &text, int max_len, bool &dropped) const
{
    dropped = false;
    if (max_len <= 0) {
        return text;
    }
    if (static_cast<int>(text.size()) <= max_len) {
        return text;
    }
    if (config_.truncate_over_limit) {
        return text.substr(0, static_cast<size_t>(max_len));
    }
    dropped = true;
    return std::string();
}

ENDSTONE_PLUGIN("telechat", "1.0.0", TgChatBridge)
{
    description = "TeleChat - Telegram chat bridge for Endstone servers";
}
