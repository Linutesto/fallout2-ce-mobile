#ifndef SFALL_FILESYSTEM_H
#define SFALL_FILESYSTEM_H

#include <stddef.h>
#include "db.h"

namespace fallout {

int sfallFileSystemCopy(const char* path, const char* source);
int sfallFileSystemCreate(const char* path, int size);
int sfallFileSystemSize(int id);
int sfallFileSystemPos(int id);
void sfallFileSystemSeek(int id, int pos);
void sfallFileSystemResize(int id, int size);
void sfallFileSystemWrite(int id, unsigned int value, int width);
void sfallFileSystemWriteString(int id, const char* text, bool terminator);
unsigned int sfallFileSystemRead(int id, int width);
File* sfallFileSystemOpen(const char* path);
bool sfallFileSystemSave(File* stream);
bool sfallFileSystemLoad(File* stream);
int sfallFileSystemFind(const char* path);
void sfallFileSystemDelete(int id);
void sfallFileSystemReset();
bool sfallFileSystemResolveAlias(const char* path, char* resolvedPath, size_t resolvedPathSize);

} // namespace fallout

#endif // SFALL_FILESYSTEM_H
