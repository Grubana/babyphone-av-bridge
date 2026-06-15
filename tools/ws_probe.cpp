#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
int main(int argc, char** argv) {
    int port = argc > 1 ? atoi(argv[1]) : 8080;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{}; a.sin_family=AF_INET; a.sin_port=htons(port); inet_pton(AF_INET,"127.0.0.1",&a.sin_addr);
    if (connect(fd,(sockaddr*)&a,sizeof a)!=0){printf("WS_CONNECT_FAIL\n");return 1;}
    const char* req = "GET /ws HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                      "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n";
    send(fd, req, strlen(req), 0);
    std::string buf; char b[4096];
    while (buf.find("\r\n\r\n") == std::string::npos) { ssize_t n=recv(fd,b,sizeof b,0); if(n<=0){printf("WS_NO_HS\n");return 1;} buf.append(b,n); }
    if (buf.find("101") == std::string::npos) { printf("WS_BAD_HS\n"); return 1; }
    std::string acc = buf.substr(buf.find("\r\n\r\n")+4);
    bool sawVideo=false, sawAudio=false; int frames=0;
    timeval tv{8,0}; setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&tv,sizeof tv);
    while (frames < 40 && !(sawVideo && sawAudio)) {
        while (acc.size() >= 2) {
            size_t off = 2; size_t len = (unsigned char)acc[1] & 0x7F;
            if (len == 126) { if (acc.size() < 4) break; len = ((unsigned char)acc[2]<<8)|(unsigned char)acc[3]; off = 4; }
            else if (len == 127) { if (acc.size() < 10) break; off = 10; len=0; for(int i=2;i<10;i++) len=(len<<8)|(unsigned char)acc[i]; }
            if (acc.size() < off + len) break;
            unsigned char tag = (unsigned char)acc[off];
            if (tag == 0) sawVideo = true; else if (tag == 1) sawAudio = true;
            frames++; acc.erase(0, off + len);
        }
        if (sawVideo && sawAudio) break;
        ssize_t n = recv(fd, b, sizeof b, 0); if (n <= 0) break; acc.append(b, n);
    }
    close(fd);
    printf("WS frames=%d video=%d audio=%d\n", frames, sawVideo, sawAudio);
    if (sawVideo && sawAudio) { printf("WS_OK\n"); return 0; }
    printf("WS_FAIL\n"); return 1;
}
