#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <thread>
#include <chrono>
#include "frame.h"
using namespace babycam;

static std::vector<uint8_t> videoFrame(bool key) {
    std::vector<uint8_t> body;
    int op = key ? 0x0000 : 0x0100;
    body.push_back((op >> 8) & 0xFF); body.push_back(op & 0xFF);
    int hdr = key ? 18 : 10;
    for (int i = 2; i < hdr; ++i) body.push_back(0x00);
    const uint8_t sc[] = {0,0,0,1, (uint8_t)(key?0x67:0x41), 0x88, 0x84};
    body.insert(body.end(), sc, sc + sizeof(sc));
    return buildFrame(8, body);
}
static std::vector<uint8_t> audioFrame() {
    std::vector<uint8_t> body(20, 0); body[0]=0x02; body[1]=0x01;
    for (int i = 0; i < 320; ++i) body.push_back(0xFF);
    return buildFrame(8, body);
}

int main(int argc, char** argv) {
    const char* host = argc > 1 ? argv[1] : "127.0.0.1";
    int port = argc > 2 ? atoi(argv[2]) : 11224;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{}; a.sin_family=AF_INET; a.sin_port=htons(port); inet_pton(AF_INET,host,&a.sin_addr);
    if (connect(fd,(sockaddr*)&a,sizeof a)!=0){printf("PPSAPP_CONNECT_FAIL\n");return 1;}
    auto login = buildFrame(1, {0x00,0x03});
    send(fd, login.data(), login.size(), 0);
    printf("PPSAPP_UP\n"); fflush(stdout);
    for (int i = 0; i < 50; ++i) {
        auto v = videoFrame(i % 10 == 0);
        send(fd, v.data(), v.size(), 0);
        auto au = audioFrame(); send(fd, au.data(), au.size(), 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    sleep(1); close(fd); return 0;
}
