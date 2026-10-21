#include "platform/fs.h"

#include <stdio.h>

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
