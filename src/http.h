#pragma once
#include <cstddef>
#include <string>

namespace babycam {

struct HttpRequest { std::string method, path, body, rawHeaders;
    std::string header(const std::string& name) const; };

bool parseHttpRequest(const std::string& raw, HttpRequest& out);

struct HttpAsset { const char* ctype; const unsigned char* data; size_t len; };
bool findAsset(const std::string& path, HttpAsset& out);
std::string buildAssetResponse(const std::string& path);

} // namespace babycam
