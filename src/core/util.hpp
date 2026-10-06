#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace sp {

double now_seconds();
std::string utf8(std::wstring_view w);
std::wstring wide(std::string_view s);
std::wstring app_data_dir();     // %APPDATA%\SpiderPet, created on first use
std::wstring documents_dir();    // Documents\SpiderPet, created on first use
uint32_t fnv1a(std::wstring_view s);
std::wstring lower(std::wstring_view s);
std::wstring trim(std::wstring_view s);
void debug_log(const std::string& line);  // %APPDATA%\SpiderPet\debug.log when SPIDERPET_LOG=1
void task_log(const std::string& line);   // %APPDATA%\SpiderPet\tasks.log, always: what each task step did (kept small)

}  // namespace sp
