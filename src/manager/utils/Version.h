#ifndef WINJECT_MANAGER_UTILS_VERSION_H_
#define WINJECT_MANAGER_UTILS_VERSION_H_

#include <stdint.h>
#include <string>

namespace winject
{

struct WinjectVersion
{
    uint8_t x = 0;
    uint8_t y = 0;
    uint16_t z = 0;
};

struct MplaneResult;

bool parse_winject_version(const std::string& text, WinjectVersion* out);
bool parse_version_from_mplane(const MplaneResult& r, WinjectVersion* out);
bool protocol_compatible(const WinjectVersion& a, const WinjectVersion& b);
WinjectVersion own_version();

}  // namespace winject

#endif  // WINJECT_MANAGER_UTILS_VERSION_H_
