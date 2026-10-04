#ifndef WINJECT_MANAGER_CONSOLE_MPLANE_ERRNO_H_
#define WINJECT_MANAGER_CONSOLE_MPLANE_ERRNO_H_

namespace winject
{

// Manager m-plane NOK codes (Linux errno names, same as the radio m-plane).
// docs/mplane.md § Reply conventions
constexpr const char k_einval[] = "EINVAL";
constexpr const char k_enoent[] = "ENOENT";
constexpr const char k_eexist[] = "EEXIST";
constexpr const char k_enodev[] = "ENODEV";
constexpr const char k_enosys[] = "ENOSYS";
constexpr const char k_enodata[] = "ENODATA";
constexpr const char k_eio[] = "EIO";
constexpr const char k_ecanceled[] = "ECANCELED";
constexpr const char k_etimedout[] = "ETIMEDOUT";
constexpr const char k_ebusy[] = "EBUSY";
constexpr const char k_eproto[] = "EPROTO";

}  // namespace winject

#endif  // WINJECT_MANAGER_CONSOLE_MPLANE_ERRNO_H_
