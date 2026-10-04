#include "utils/Version.h"

#include "WinjectBuildVersion.h"
#include "console/ConsoleClient.h"

#include <cctype>

namespace winject
{

bool parse_winject_version(const std::string& text, WinjectVersion* out)
{
    if (out == nullptr || text.size() < 6 || text[0] != 'v')
    {
        return false;
    }
    size_t i = 1;
    auto parse_u8 = [&](uint8_t* v) -> bool
    {
        if (i >= text.size() ||
            !std::isdigit(static_cast<unsigned char>(text[i])))
        {
            return false;
        }
        unsigned long acc = 0;
        while (i < text.size() &&
               std::isdigit(static_cast<unsigned char>(text[i])))
        {
            acc = acc * 10 + static_cast<unsigned long>(text[i] - '0');
            if (acc > 255)
            {
                return false;
            }
            ++i;
        }
        *v = static_cast<uint8_t>(acc);
        return true;
    };
    auto parse_u16 = [&](uint16_t* v) -> bool
    {
        if (i >= text.size() ||
            !std::isdigit(static_cast<unsigned char>(text[i])))
        {
            return false;
        }
        unsigned long acc = 0;
        while (i < text.size() &&
               std::isdigit(static_cast<unsigned char>(text[i])))
        {
            acc = acc * 10 + static_cast<unsigned long>(text[i] - '0');
            if (acc > 65535)
            {
                return false;
            }
            ++i;
        }
        *v = static_cast<uint16_t>(acc);
        return true;
    };
    uint8_t x = 0;
    uint8_t y = 0;
    uint16_t z = 0;
    if (!parse_u8(&x) || i >= text.size() || text[i] != '.')
    {
        return false;
    }
    ++i;
    if (!parse_u8(&y) || i >= text.size() || text[i] != '.')
    {
        return false;
    }
    ++i;
    if (!parse_u16(&z) || i != text.size())
    {
        return false;
    }
    out->x = x;
    out->y = y;
    out->z = z;
    return true;
}

bool parse_version_from_mplane(const MplaneResult& r, WinjectVersion* out)
{
    std::string text;
    for (const auto& line : r.body_lines)
    {
        if (!text.empty())
        {
            text += ' ';
        }
        text += line;
    }
    if (text.empty())
    {
        text = r.payload;
    }
    const std::string key = "ver=";
    const size_t pos = text.find(key);
    if (pos == std::string::npos)
    {
        return false;
    }
    size_t start = pos + key.size();
    while (start < text.size() && text[start] == ' ')
    {
        ++start;
    }
    size_t end = start;
    while (end < text.size() && text[end] != ' ' && text[end] != '\t')
    {
        ++end;
    }
    return parse_winject_version(text.substr(start, end - start), out);
}

bool protocol_compatible(const WinjectVersion& a, const WinjectVersion& b)
{
    return a.x == b.x && a.y == b.y;
}

WinjectVersion own_version()
{
    WinjectVersion v;
    v.x = static_cast<uint8_t>(WINJECT_VERSION_MAJOR);
    v.y = static_cast<uint8_t>(WINJECT_VERSION_MINOR);
    v.z = static_cast<uint16_t>(WINJECT_VERSION_PATCH);
    return v;
}

}  // namespace winject
