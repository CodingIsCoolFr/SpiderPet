#pragma once

#include <string>

namespace sp {

// HTTPS GET to a public source (Crossref, PubMed, Wikipedia...). Follows
// redirects. Returns the status code, or 0 when nothing answered.
long web_get(const std::string& url, std::string& body, unsigned timeout_ms = 10000);

// Plain HTTP to Ollama on this machine (127.0.0.1:11434).
long local_http(const char* method, const std::string& path, const std::string* body, std::string& out,
                unsigned timeout_ms);

// Percent-encodes UTF-8 for a URL; characters in `safe` stay as they are.
std::string url_encode(const std::string& s, const char* safe = "");

}  // namespace sp
