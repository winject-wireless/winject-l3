#ifndef WINJECT_MANAGER_LOG_H_
#define WINJECT_MANAGER_LOG_H_

namespace winject
{

// Line format: YYYY-MM-DD HH:MM:SS.sss | LEVEL | msg
void log_printf(const char* level, const char* fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;

}  // namespace winject

#define LOG_INF(fmt, ...) winject::log_printf("INF", fmt, ##__VA_ARGS__)
#define LOG_ERR(fmt, ...) winject::log_printf("ERR", fmt, ##__VA_ARGS__)
#define LOG_WRN(fmt, ...) winject::log_printf("WRN", fmt, ##__VA_ARGS__)

#endif  // WINJECT_MANAGER_LOG_H_
