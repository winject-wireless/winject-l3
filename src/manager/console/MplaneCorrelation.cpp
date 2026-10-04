#include "console/MplaneCorrelation.h"

#include "console/MplaneErrno.h"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>

namespace winject
{

bool parse_mplane_cmd(const char* line, uint8_t* id, const char** body,
                      bool* malformed)
{
    if (line == nullptr || body == nullptr || malformed == nullptr)
    {
        return false;
    }
    *malformed = false;
    if (strncmp(line, "cmd:", 4) != 0)
    {
        *body = line;
        return false;
    }
    const char* rest = line + 4;
    char* end = nullptr;
    const unsigned long parsed = strtoul(rest, &end, 10);
    if (end == rest || parsed > 255u)
    {
        *malformed = true;
        return true;
    }
    if (*end != ' ')
    {
        *malformed = true;
        return true;
    }
    const char* command = end + 1;
    if (*command == '\0')
    {
        *malformed = true;
        return true;
    }
    *id = static_cast<uint8_t>(parsed);
    *body = command;
    return true;
}

static std::string strip_trailing_crlf(std::string s)
{
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r'))
    {
        s.pop_back();
    }
    return s;
}

std::string format_correlated_reply(uint8_t id, const std::string& text)
{
    std::string body = strip_trailing_crlf(text);
    const bool had_nl = !text.empty() && text.back() == '\n';
    char id_buf[16];
    snprintf(id_buf, sizeof(id_buf), "%u", static_cast<unsigned>(id));

    if (body.rfind("OK", 0) == 0 &&
        (body.size() == 2 || body[2] == ' ' || body[2] == '\n'))
    {
        std::string out = "OK:";
        out += id_buf;
        if (body.size() > 2)
        {
            out += body.substr(2);
        }
        else if (had_nl)
        {
            out += '\n';
        }
        return out;
    }
    if (body.rfind("NOK ", 0) == 0)
    {
        std::string out = "NOK:";
        out += id_buf;
        out += body.substr(3);
        if (had_nl && (out.empty() || out.back() != '\n'))
        {
            out += '\n';
        }
        return out;
    }

    std::string out = "OK:";
    out += id_buf;
    out += ' ';
    const size_t nl = body.find('\n');
    if (nl == std::string::npos)
    {
        out += body;
    }
    else
    {
        out += body.substr(0, nl);
        out += body.substr(nl);
    }
    if (had_nl && (out.empty() || out.back() != '\n'))
    {
        out += '\n';
    }
    return out;
}

std::string format_mplane_cmd(uint8_t req_id, const std::string& line)
{
    std::string body = line;
    while (!body.empty() && (body.back() == '\n' || body.back() == '\r'))
    {
        body.pop_back();
    }
    char prefix[16];
    snprintf(prefix, sizeof(prefix), "cmd:%u ", static_cast<unsigned>(req_id));
    std::string wire = prefix;
    wire += body;
    wire.push_back('\n');
    return wire;
}

bool parse_correlated_reply(const std::string& line, uint8_t* req_id, bool* ok,
                            std::string* payload)
{
    if (req_id == nullptr || ok == nullptr)
    {
        return false;
    }
    const char* rest = nullptr;
    if (line.rfind("OK:", 0) == 0)
    {
        *ok = true;
        rest = line.c_str() + 3;
    }
    else if (line.rfind("NOK:", 0) == 0)
    {
        *ok = false;
        rest = line.c_str() + 4;
    }
    else
    {
        return false;
    }
    unsigned id = 0;
    if (sscanf(rest, "%u", &id) != 1 || id > 255u)
    {
        return false;
    }
    *req_id = static_cast<uint8_t>(id);
    const char* sp = strchr(rest, ' ');
    if (payload != nullptr)
    {
        if (sp != nullptr && sp[1] != '\0')
        {
            *payload = sp + 1;
        }
        else
        {
            payload->clear();
        }
    }
    return true;
}

bool mplane_nok_token(const std::string& err, std::string* token)
{
    const auto nok = err.find(" -> nok ");
    const auto nok_upper = err.find(" -> NOK ");
    const size_t pos =
        nok != std::string::npos
            ? nok
            : (nok_upper != std::string::npos ? nok_upper : std::string::npos);
    if (pos == std::string::npos)
    {
        return false;
    }
    *token = err.substr(pos + 8);
    return true;
}

static bool looks_like_errno_token(const std::string& s)
{
    if (s.size() < 2 || s[0] != 'E')
    {
        return false;
    }
    for (size_t i = 1; i < s.size(); ++i)
    {
        if (!std::isupper(static_cast<unsigned char>(s[i])))
        {
            return false;
        }
    }
    return true;
}

std::string mplane_err_string(const std::string& err)
{
    std::string token;
    if (mplane_nok_token(err, &token))
    {
        return token;
    }
    if (err == "cancelled")
    {
        return k_ecanceled;
    }
    if (err == "timeout")
    {
        return k_etimedout;
    }
    if (looks_like_errno_token(err))
    {
        return err;
    }
    return err.empty() ? k_eio : k_eio;
}

}  // namespace winject
