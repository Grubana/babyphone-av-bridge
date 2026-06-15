#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
int main(int argc, char** argv) {
    int port = argc > 1 ? atoi(argv[1]) : 11224;
    int fd = socket(AF_INET, SOCK_STREAM, 0); int one=1; setsockopt(fd,SOL_SOCKET,SO_REUSEADDR,&one,sizeof one);
    sockaddr_in a{}; a.sin_family=AF_INET; a.sin_addr.s_addr=INADDR_ANY; a.sin_port=htons(port);
    if (bind(fd,(sockaddr*)&a,sizeof a)!=0){printf("MON_BIND_FAIL\n");return 1;}
    listen(fd,1); printf("MON_UP\n"); fflush(stdout);
    int c = accept(fd, nullptr, nullptr); if (c<0) return 1;
    char buf[8192]; while (recv(c, buf, sizeof buf, 0) > 0) {}
    close(c); close(fd); return 0;
}
