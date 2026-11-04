#ifndef WINJECT_CORE_MPLANE_MPLANE_RESULT_H_
#define WINJECT_CORE_MPLANE_MPLANE_RESULT_H_

#include <string>
#include <vector>

namespace winject::core
{

// One radio m-plane reply: OK with its body, or NOK with the radio's error
// text (or a local one: "timeout", "EPROTO", ...).
struct MplaneResult
{
    bool ok = false;
    std::vector<std::string> body_lines;
    std::string payload;
    std::string error;

    // The reply body as the radio sent it: body lines joined by '\n', or the
    // OK payload when there are none.
    std::string text() const
    {
        std::string body;
        for (const auto& line : body_lines)
        {
            if (!body.empty())
            {
                body += '\n';
            }
            body += line;
        }
        if (body.empty() && !payload.empty())
        {
            body = payload;
        }
        return body;
    }

    bool is_error(const char* code) const
    {
        return !ok && (error == code || error.find(code) != std::string::npos);
    }
    bool is_enosys() const
    {
        return is_error("ENOSYS");
    }
    bool is_eproto() const
    {
        return is_error("EPROTO");
    }
};

}  // namespace winject::core

#endif  // WINJECT_CORE_MPLANE_MPLANE_RESULT_H_
