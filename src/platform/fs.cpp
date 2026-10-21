#include "platform/fs.h"

#include <stdio.h>

#include <utility>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#endif

namespace fs {

bool list_dir(const std::string &path, std::vector<std::string> *names)
{
#ifdef _WIN32
    std::string pattern = path.empty() ? "." : path;
    if (pattern.back() != '/' && pattern.back() != '\\') pattern += '/';
    pattern += '*';
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    do {
        names->push_back(fd.cFileName);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return true;
#else
    DIR *d = opendir(path.empty() ? "." : path.c_str());
    if (!d) return false;
    while (struct dirent *e = readdir(d)) names->push_back(e->d_name);
    closedir(d);
    return true;
#endif
}

#ifndef _WIN32
namespace {

// INVENTED: FindFirstFileA has no POSIX equivalent, so match the Win32 pattern
// search closely enough for the engine's callers. No 8.3 emulation.
char lower_ascii(char c)
{
    return c >= 'A' && c <= 'Z' ? (char)(c + ('a' - 'A')) : c;
}

bool wild_match(const char *s, const char *p)
{
    if (*p == '*') {
        while (*p == '*') ++p;
        if (!*p) return true;
        do {
            if (wild_match(s, p)) return true;
        } while (*s++);
        return false;
    }
    if (!*s) return false;
    if (*p != '?' && lower_ascii(*p) != lower_ascii(*s)) return false;
    return wild_match(s + 1, p + 1);
}

// Win32: a pattern ending in ".*" also matches names without a dot ("*.*" == "*").
bool pattern_match(const std::string &name, const std::string &pattern)
{
    if (wild_match(name.c_str(), pattern.c_str())) return true;
    if (pattern.size() >= 2 && pattern.compare(pattern.size() - 2, 2, ".*") == 0)
        return wild_match(name.c_str(), pattern.substr(0, pattern.size() - 2).c_str());
    return false;
}

} // namespace
#endif

bool find(const std::string &pattern, std::vector<FindEntry> *out)
{
#ifdef _WIN32
    // 0x545520/0x5455e0/0x5454d0
    std::string pat = pattern;
    for (char &c : pat)
        if (c == '/') c = '\\';
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    do {
        FindEntry e;
        e.name = fd.cFileName;
        e.is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        out->push_back(std::move(e));
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return true;
#else
    size_t sep = pattern.find_last_of("/\\");
    std::string dir = sep == std::string::npos ? "" : pattern.substr(0, sep);
    std::string name_pat = sep == std::string::npos ? pattern : pattern.substr(sep + 1);
    if (name_pat.empty()) return false;   // FindFirstFileA("dir\\") fails
    std::vector<std::string> names;
    if (!list_dir(dir, &names)) return false;
    bool matched = false;
    for (const std::string &name : names) {
        if (!pattern_match(name, name_pat)) continue;
        FindEntry e;
        e.name = name;
        std::string full = dir.empty() ? name : dir + "/" + name;
        e.is_dir = exists(full) && !is_file(full, nullptr);
        out->push_back(std::move(e));
        matched = true;
    }
    return matched;
#endif
}

bool exists(const std::string &path)
{
#ifdef _WIN32
    return GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES;
#else
    struct stat st;
    return stat(path.c_str(), &st) == 0;
#endif
}

bool is_file(const std::string &path, size_t *size)
{
#ifdef _WIN32
    WIN32_FILE_ATTRIBUTE_DATA info;
    if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &info)) return false;
    if (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) return false;
    if (size)
        *size = (size_t)(((uint64_t)info.nFileSizeHigh << 32) | info.nFileSizeLow);
    return true;
#else
    struct stat st;
    if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) return false;
    if (size) *size = (size_t)st.st_size;
    return true;
#endif
}

bool read_file(const std::string &path, std::vector<uint8_t> *data, std::string *err)
{
    data->clear();
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) {
        if (err) *err = "cannot open " + path;
        return false;
    }
    uint8_t buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0)
        data->insert(data->end(), buf, buf + n);
    bool read_ok = !ferror(f);
    fclose(f);
    if (!read_ok) {
        if (err) *err = "read error in " + path;
        data->clear();
        return false;
    }
    return true;
}

} // namespace fs
