#include "console/MplaneCorrelation.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace winject
{

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

}  // namespace winject
