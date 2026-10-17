#pragma once
#include <direct.h>
#include <sys/stat.h>
inline int mkdir(const char* path, int) { return _mkdir(path); }
#ifndef S_ISDIR
#define S_ISDIR(mode) (((mode) & _S_IFMT) == _S_IFDIR)
#endif
#ifndef S_ISREG
#define S_ISREG(mode) (((mode) & _S_IFMT) == _S_IFREG)
#endif
