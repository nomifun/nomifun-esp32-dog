#pragma once
#include <cstddef>
#include <cstring>
#define AF_INET 2
#define SOCK_STREAM 1
#define SHUT_RDWR 2
struct in_addr { unsigned long s_addr; };
struct sockaddr { unsigned short sa_family; char sa_data[14]; };
struct sockaddr_in { short sin_family; unsigned short sin_port; in_addr sin_addr; };
int host_socket(int,int,int); int host_connect(int,const sockaddr*,int);
int host_recv(int,void*,size_t,int); int host_send(int,const void*,size_t,int);
int host_shutdown(int,int); int host_close(int);
#define socket host_socket
#define connect host_connect
#define recv host_recv
#define send host_send
#define shutdown host_shutdown
#define close host_close
#define bzero(p,n) memset(p,0,n)
