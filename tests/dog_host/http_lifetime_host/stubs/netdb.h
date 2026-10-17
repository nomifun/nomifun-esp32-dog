#pragma once
struct hostent { char* h_addr; int h_length; };
inline int h_errno=0;
inline hostent* gethostbyname(const char*) { static unsigned long addr=0; static hostent h{(char*)&addr,sizeof(addr)}; return &h; }
