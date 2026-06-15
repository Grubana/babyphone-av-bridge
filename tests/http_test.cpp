#include "doctest.h"
#include "http.h"
using namespace babycam;

TEST_CASE("parse request line, path, header, body") {
    std::string raw = "GET /app.js?x=1 HTTP/1.1\r\nHost: h\r\nUpgrade: websocket\r\n\r\n";
    HttpRequest r;
    REQUIRE(parseHttpRequest(raw, r));
    CHECK(r.method == "GET");
    CHECK(r.path == "/app.js");
    CHECK(r.header("upgrade") == "websocket");
}

TEST_CASE("index served at /, app.js found, unknown -> 404") {
    HttpAsset a;
    CHECK(findAsset("/", a));
    CHECK(findAsset("/app.js", a));
    CHECK_FALSE(findAsset("/nope", a));
    auto resp = buildAssetResponse("/");
    CHECK(resp.find("200 OK") != std::string::npos);
    CHECK(resp.find("text/html") != std::string::npos);
    auto r404 = buildAssetResponse("/nope");
    CHECK(r404.find("404") != std::string::npos);
}
