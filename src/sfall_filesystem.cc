#include "sfall_filesystem.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include <string>
#include <vector>

#include "art.h"
#include "db.h"
#include "platform_compat.h"
#include "sfall_virtual_buffer.h"

namespace fallout {

typedef struct SfallFileSystemAlias {
    bool active;
    std::string path;
    std::string source;
    std::string normalizedPath;
    SfallVirtualBuffer buffer{0};
    bool saved = false;
} SfallFileSystemAlias;

static std::vector<SfallFileSystemAlias> aliases;

static std::string normalizePath(const char* path)
{
    std::string normalized = path != nullptr ? path : "";
    for (char& ch : normalized) {
        if (ch == '/') {
            ch = '\\';
        } else if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<char>(ch - 'A' + 'a');
        }
    }
    return normalized;
}

static bool isSoundEffectsPath(const std::string& normalizedPath)
{
    return normalizedPath.rfind("sound\\sfx\\", 0) == 0;
}

static int findAliasIndex(const char* path)
{
    if (path == nullptr || path[0] == '\0') {
        return -1;
    }

    std::string normalized = normalizePath(path);
    for (size_t index = 0; index < aliases.size(); index++) {
        if (aliases[index].active && aliases[index].normalizedPath == normalized) {
            return static_cast<int>(index);
        }
    }

    return -1;
}

int sfallFileSystemCopy(const char* path, const char* source)
{
    if (path == nullptr || path[0] == '\0' || source == nullptr || source[0] == '\0') {
        return -1;
    }

    int existingIndex = findAliasIndex(path);
    if (existingIndex != -1) {
        return existingIndex;
    }

    int fileSize = 0;
    if (dbGetFileSize(source, &fileSize) != 0) {
        return -1;
    }
    if (fileSize < 0 || fileSize > 10 * 1024 * 1024) return -1;
    std::vector<unsigned char> bytes(fileSize);
    if (fileSize > 0 && dbGetFileContents(source, bytes.data()) != 0) return -1;

    SfallFileSystemAlias alias;
    alias.active = true;
    alias.path = path;
    alias.source = normalizePath(source);
    alias.normalizedPath = normalizePath(path);
    alias.buffer = SfallVirtualBuffer(std::move(bytes));

    for (size_t index = 0; index < aliases.size(); index++) {
        if (!aliases[index].active) {
            aliases[index] = alias;
            artCacheFlush();
            return static_cast<int>(index);
        }
    }

    aliases.push_back(alias);
    artCacheFlush();
    return static_cast<int>(aliases.size() - 1);
}

int sfallFileSystemFind(const char* path)
{
    return findAliasIndex(path);
}

void sfallFileSystemDelete(int id)
{
    if (id < 0 || id >= static_cast<int>(aliases.size()) || !aliases[id].active) {
        return;
    }

    aliases[id].active = false;
    aliases[id].path.clear();
    aliases[id].source.clear();
    aliases[id].normalizedPath.clear();
    aliases[id].buffer = SfallVirtualBuffer(0);
    artCacheFlush();
}

void sfallFileSystemReset()
{
    if (aliases.empty()) {
        return;
    }

    aliases.clear();
    artCacheFlush();
}

bool sfallFileSystemResolveAlias(const char* path, char* resolvedPath, size_t resolvedPathSize)
{
    if (path == nullptr || path[0] == '\0' || resolvedPath == nullptr || resolvedPathSize == 0) {
        return false;
    }

    std::string normalized = normalizePath(path);
    if (isSoundEffectsPath(normalized)) {
        return false;
    }

    int aliasIndex = findAliasIndex(path);
    if (aliasIndex == -1 || aliases[aliasIndex].source == path) {
        return false;
    }

    int written = snprintf(resolvedPath, resolvedPathSize, "%s", aliases[aliasIndex].source.c_str());
    return written >= 0 && static_cast<size_t>(written) < resolvedPathSize;
}

static SfallFileSystemAlias* entry(int id)
{
    return id >= 0 && id < static_cast<int>(aliases.size()) && aliases[id].active ? &aliases[id] : nullptr;
}

int sfallFileSystemCreate(const char* path, int size)
{
    if (path == nullptr || !*path || size < 0 || size > 10 * 1024 * 1024) return -1;
    int old = findAliasIndex(path);
    if (old != -1) return old;
    SfallFileSystemAlias value;
    value.active = true;
    value.path = path;
    value.normalizedPath = normalizePath(path);
    value.buffer = SfallVirtualBuffer(size);
    for (size_t i = 0; i < aliases.size(); ++i) {
        if (!aliases[i].active) { aliases[i] = std::move(value); return static_cast<int>(i); }
    }
    aliases.push_back(std::move(value));
    return static_cast<int>(aliases.size() - 1);
}

int sfallFileSystemSize(int id) { auto* e = entry(id); return e ? e->buffer.size() : 0; }
int sfallFileSystemPos(int id) { auto* e = entry(id); return e ? e->buffer.pos() : 0; }
void sfallFileSystemSeek(int id, int pos) { auto* e = entry(id); if (e) e->buffer.seek(pos); }
void sfallFileSystemResize(int id, int size)
{
    auto* e = entry(id);
    if (!e) return;
    if (size == -1) { e->saved = true; return; }
    if (e->buffer.resize(size)) artCacheFlush();
}
void sfallFileSystemWrite(int id, unsigned int value, int width)
{
    auto* e = entry(id);
    if (e && e->buffer.write(value, width)) artCacheFlush();
}
unsigned int sfallFileSystemRead(int id, int width)
{
    auto* e = entry(id); return e ? e->buffer.read(width) : 0;
}
void sfallFileSystemWriteString(int id, const char* text, bool terminator)
{
    auto* e = entry(id);
    if (!e || !text) return;
    size_t size = strlen(text) + (terminator ? 1 : 0);
    if (size > static_cast<size_t>(e->buffer.size() - e->buffer.pos())) return;
    for (size_t i = 0; i < size; ++i) e->buffer.write(static_cast<unsigned char>(text[i]), 1);
    artCacheFlush();
}

// Give the engine a separate read cursor over the edited virtual file. tmpfile
// owns its anonymous storage; no script-controlled host path is ever written.
File* sfallFileSystemOpen(const char* path)
{
    int id = findAliasIndex(path);
    if (id < 0 || isSoundEffectsPath(normalizePath(path))) return nullptr;
    const auto& bytes = aliases[id].buffer.bytes();
    FILE* fp = tmpfile();
    if (!fp) return nullptr;
    if (!bytes.empty() && fwrite(bytes.data(), 1, bytes.size(), fp) != bytes.size()) { fclose(fp); return nullptr; }
    rewind(fp);
    File* stream = static_cast<File*>(malloc(sizeof(File)));
    if (!stream) { fclose(fp); return nullptr; }
    stream->type = XFILE_TYPE_FILE;
    stream->file = fp;
    return stream;
}

// Optional CE trailer: old saves end before this marker. Keep slot indexes,
// since scripts may retain virtual file handles in saved global variables.
bool sfallFileSystemSave(File* stream)
{
    if (fileWriteInt32(stream, 0x56465331) == -1 || fileWriteInt32(stream, aliases.size()) == -1) return false;
    for (const auto& e : aliases) {
        bool keep = e.active && e.saved;
        if (fileWriteInt32(stream, keep ? e.path.size() : 0) == -1) return false;
        if (!keep) continue;
        if (fileWrite(e.path.data(), 1, e.path.size(), stream) != e.path.size()
            || fileWriteInt32(stream, e.buffer.size()) == -1
            || fileWriteInt32(stream, e.buffer.pos()) == -1) return false;
        const auto& bytes = e.buffer.bytes();
        if (fileWrite(bytes.data(), 1, bytes.size(), stream) != bytes.size()) return false;
    }
    return true;
}

bool sfallFileSystemLoad(File* stream)
{
    int32_t magic, count;
    if (fileReadInt32(stream, &magic) == -1) return true; // older save
    if (magic != 0x56465331 || fileReadInt32(stream, &count) == -1 || count < 0 || count > 4096) return false;
    std::vector<SfallFileSystemAlias> loaded(count);
    size_t total = 0;
    for (auto& e : loaded) {
        int32_t len, size, pos;
        if (fileReadInt32(stream, &len) == -1 || len < 0 || len > 1024) return false;
        e.active = false;
        if (!len) continue;
        e.path.resize(len);
        if (fileRead(&e.path[0], 1, len, stream) != static_cast<size_t>(len)
            || fileReadInt32(stream, &size) == -1 || size < 0 || size > 10 * 1024 * 1024
            || fileReadInt32(stream, &pos) == -1 || pos < 0 || pos > size) return false;
        total += size;
        if (total > 64 * 1024 * 1024) return false;
        std::vector<unsigned char> bytes(size);
        if (fileRead(bytes.data(), 1, size, stream) != static_cast<size_t>(size)) return false;
        e.buffer = SfallVirtualBuffer(std::move(bytes));
        e.buffer.seek(pos);
        e.active = e.saved = true;
        e.normalizedPath = normalizePath(e.path.c_str());
    }
    aliases = std::move(loaded);
    artCacheFlush();
    return true;
}

} // namespace fallout
