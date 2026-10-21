// Minimal filesystem layer: the engine only needs one-level directory listing,
// existence/size checks and whole-file binary reads. POSIX dirent/stat live
// here so the rest of the library compiles unchanged on Windows.
#pragma once
#include <stddef.h>
#include <stdint.h>

#include <string>
#include <vector>

namespace fs {

// One directory level, names only; like readdir it includes "." and "..".
// path may use either separator and may be empty (current directory).
bool list_dir(const std::string &path, std::vector<std::string> *names);

bool exists(const std::string &path);

// True for a regular file only; size receives the byte count.
bool is_file(const std::string &path, size_t *size);

// Whole file in binary mode. Error text matches the old fopen-based loaders.
bool read_file(const std::string &path, std::vector<uint8_t> *data, std::string *err);

} // namespace fs
