#include "http.h"
#include "web_assets.h"
#include <algorithm>
#include <cctype>

namespace babycam {

static std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c){ return std::tolower(c); });
    return s;
}

std::string HttpRequest::header(const std::string& name) const {
    std::string want = lower(name);
    size_t pos = 0;
    while (pos < rawHeaders.size()) {
        size_t nl = rawHeaders.find("\r\n", pos);
        std::string line = rawHeaders.substr(pos, (nl == std::string::npos ? rawHeaders.size() : nl) - pos);
        size_t c = line.find(':');
        if (c != std::string::npos && lower(line.substr(0, c)) == want) {
            std::string v = line.substr(c + 1);
            v.erase(0, v.find_first_not_of(" \t"));
            return v;
        }
        if (nl == std::string::npos) break;
        pos = nl + 2;
    }
    return {};
}

bool parseHttpRequest(const std::string& raw, HttpRequest& out) {
    auto he = raw.find("\r\n\r\n");
    if (he == std::string::npos) return false;
    std::string head = raw.substr(0, he);
    auto le = head.find("\r\n");
    std::string reqline = head.substr(0, le);
    auto s1 = reqline.find(' '), s2 = reqline.find(' ', s1 + 1);
    if (s1 == std::string::npos || s2 == std::string::npos) return false;
    out.method = reqline.substr(0, s1);
    std::string target = reqline.substr(s1 + 1, s2 - s1 - 1);
    auto q = target.find('?');
    out.path = (q == std::string::npos) ? target : target.substr(0, q);
    out.rawHeaders = (le == std::string::npos) ? "" : head.substr(le + 2);
    out.body = raw.substr(he + 4);
    return true;
}

bool findAsset(const std::string& path, HttpAsset& out) {
    for (size_t i = 0; i < webassets::kAssetCount; ++i) {
        if (path == webassets::kAssets[i].route) {
            out.ctype = webassets::kAssets[i].ctype;
            out.data = webassets::kAssets[i].data;
            out.len = webassets::kAssets[i].len;
            return true;
        }
    }
    return false;
}

std::string buildAssetResponse(const std::string& path) {
    HttpAsset a;
    if (!findAsset(path, a)) {
        std::string b = "not found";
        return "HTTP/1.1 404 Not Found\r\nContent-Length: " + std::to_string(b.size()) +
               "\r\nConnection: close\r\n\r\n" + b;
    }
    std::string head = "HTTP/1.1 200 OK\r\nContent-Type: " + std::string(a.ctype) +
                       "\r\nContent-Length: " + std::to_string(a.len) +
                       "\r\nConnection: close\r\n\r\n";
    std::string resp = head;
    resp.append((const char*)a.data, a.len);
    return resp;
}

} // namespace babycam
