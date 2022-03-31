// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
#include "FileUtils.h"

#include "Luau/Common.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#else
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#endif

#include <string.h>
#include <iostream>
#include <fstream>
#include <filesystem>
#include <sstream>

#ifdef _WIN32
static std::wstring fromUtf8(const std::string& path)
{
    size_t result = MultiByteToWideChar(CP_UTF8, 0, path.data(), int(path.size()), nullptr, 0);
    LUAU_ASSERT(result);

    std::wstring buf(result, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, path.data(), int(path.size()), &buf[0], int(buf.size()));

    return buf;
}

static std::string toUtf8(const std::wstring& path)
{
    size_t result = WideCharToMultiByte(CP_UTF8, 0, path.data(), int(path.size()), nullptr, 0, nullptr, nullptr);
    LUAU_ASSERT(result);

    std::string buf(result, '\0');
    WideCharToMultiByte(CP_UTF8, 0, path.data(), int(path.size()), &buf[0], int(buf.size()), nullptr, nullptr);

    return buf;
}
#endif

std::optional<std::string> readFile(const std::string& name)
{
#ifdef _WIN32
    FILE* file = _wfopen(fromUtf8(name).c_str(), L"rb");
#else
    FILE* file = fopen(name.c_str(), "rb");
#endif

    if (!file)
        return std::nullopt;

    fseek(file, 0, SEEK_END);
    long length = ftell(file);
    if (length < 0)
    {
        fclose(file);
        return std::nullopt;
    }
    fseek(file, 0, SEEK_SET);

    std::string result(length, 0);

    size_t read = fread(result.data(), 1, length, file);
    fclose(file);

    if (read != size_t(length))
        return std::nullopt;

    // Skip first line if it's a shebang
    if (length > 2 && result[0] == '#' && result[1] == '!')
        result.erase(0, result.find('\n'));

    return result;
}

std::optional<std::string> readStdin()
{
    std::string result;
    char buffer[4096] = {};

    while (fgets(buffer, sizeof(buffer), stdin) != nullptr)
        result.append(buffer);

    // If eof was not reached for stdin, then a read error occurred
    if (!feof(stdin))
        return std::nullopt;

    return result;
}

std::optional<std::string> readFile(const std::filesystem::path& filePath)
{
    std::ifstream fileContents;
    fileContents.open(filePath);

    std::string output;
    std::stringstream buffer;

    if (fileContents)
    {
        buffer << fileContents.rdbuf();
        output = buffer.str();
        return output;
    }
    else
    {
        return std::nullopt;
    }
}

template<typename Ch>
static void joinPaths(std::basic_string<Ch>& str, const Ch* lhs, const Ch* rhs)
{
    str = lhs;
    if (!str.empty() && str.back() != '/' && str.back() != '\\' && *rhs != '/' && *rhs != '\\')
        str += '/';
    str += rhs;
}

#ifdef _WIN32
static bool traverseDirectoryRec(const std::wstring& path, bool recursive, const std::function<void(const std::string& name)>& callback)
{
    std::wstring query = path + std::wstring(L"/*");

    WIN32_FIND_DATAW data;
    HANDLE h = FindFirstFileW(query.c_str(), &data);

    if (h == INVALID_HANDLE_VALUE)
        return false;

    std::wstring buf;

    do
    {
        if (wcscmp(data.cFileName, L".") != 0 && wcscmp(data.cFileName, L"..") != 0)
        {
            joinPaths(buf, path.c_str(), data.cFileName);

            if (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
            {
                // Skip reparse points to avoid handling cycles
            }
            else if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY && recursive)
            {
                traverseDirectoryRec(buf, recursive, callback);
            }
            else
            {
                callback(toUtf8(buf));
            }
        }
    } while (FindNextFileW(h, &data));

    FindClose(h);

    return true;
}

bool traverseDirectory(const std::string& path, bool recursive, const std::function<void(const std::string& name)>& callback)
{
    return traverseDirectoryRec(fromUtf8(path), recursive, callback);
}
#else
static bool traverseDirectoryRec(const std::string& path, bool recursive, const std::function<void(const std::string& name)>& callback)
{
    int fd = open(path.c_str(), O_DIRECTORY);
    DIR* dir = fdopendir(fd);

    if (!dir)
        return false;

    std::string buf;

    while (dirent* entry = readdir(dir))
    {
        const dirent& data = *entry;

        if (strcmp(data.d_name, ".") != 0 && strcmp(data.d_name, "..") != 0)
        {
            joinPaths(buf, path.c_str(), data.d_name);

            int type = data.d_type;
            int mode = -1;

            // we need to stat DT_UNKNOWN to be able to tell the type
            if (type == DT_UNKNOWN)
            {
                struct stat st = {};
#ifdef _ATFILE_SOURCE
                fstatat(fd, data.d_name, &st, 0);
#else
                lstat(buf.c_str(), &st);
#endif

                mode = st.st_mode;
            }

            if ((type == DT_DIR || mode == S_IFDIR) && recursive)
            {
                traverseDirectoryRec(buf, recursive, callback);
            }
            else if (type == DT_REG || mode == S_IFREG)
            {
                callback(buf);
            }
            else if (type == DT_LNK || mode == S_IFLNK)
            {
                // Skip symbolic links to avoid handling cycles
            }
        }
    }

    closedir(dir);

    return true;
}

bool traverseDirectory(const std::string& path, bool recursive, const std::function<void(const std::string& name)>& callback)
{
    return traverseDirectoryRec(path, recursive, callback);
}
#endif

bool isDirectory(const std::string& path)
{
#ifdef _WIN32
    DWORD fileAttributes = GetFileAttributesW(fromUtf8(path).c_str());
    if (fileAttributes == INVALID_FILE_ATTRIBUTES)
        return false;
    return (fileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
#else
    struct stat st = {};
    lstat(path.c_str(), &st);
    return (st.st_mode & S_IFMT) == S_IFDIR;
#endif
}

std::string joinPaths(const std::string& lhs, const std::string& rhs)
{
    std::string result = lhs;
    if (!result.empty() && result.back() != '/' && result.back() != '\\')
        result += '/';
    result += rhs;
    return result;
}

std::optional<std::string> getParentPath(const std::string& path)
{
    if (path == "" || path == "." || path == "/")
        return std::nullopt;

#ifdef _WIN32
    if (path.size() == 2 && path.back() == ':')
        return std::nullopt;
#endif

    std::string::size_type slash = path.find_last_of("\\/", path.size() - 1);

    if (slash == 0)
        return "/";

    if (slash != std::string::npos)
        return path.substr(0, slash);

    return "";
}

std::string getFileName(const std::string& path)
{
    std::string::size_type slash = path.find_last_of("\\/", path.size() - 1);
    if (slash != std::string::npos)
    {
        std::string nameWithExtension = path.substr(slash + 1);

        // Remove specific extensions
        for (auto& ext : {".server.lua", ".server.luau", ".client.lua", ".client.luau", ".lua", ".luau"})
        {
            std::string::size_type extension = nameWithExtension.find(ext);
            if (extension != std::string::npos)
                return nameWithExtension.erase(extension, strlen(ext));
        }

        return nameWithExtension;
    }

    return "";
}

std::string getExtension(const std::string& path)
{
    std::string::size_type dot = path.find_last_of(".\\/");

    if (dot == std::string::npos || path[dot] != '.')
        return "";

    return path.substr(dot);
}

std::vector<std::string> getSourceFiles(int argc, char** argv)
{
    std::vector<std::string> files;

    for (int i = 1; i < argc; ++i)
    {
        // Treat '-' as a special file whose source is read from stdin
        // All other arguments that start with '-' are skipped
        if (argv[i][0] == '-' && argv[i][1] != '\0')
            continue;

        if (isDirectory(argv[i]))
        {
            traverseDirectory(argv[i], true,
                [&](const std::string& name)
                {
                    std::string ext = getExtension(name);

                    if (ext == ".lua" || ext == ".luau")
                        files.push_back(name);
                });
        }
        else
        {
            files.push_back(argv[i]);
        }
    }

    return files;
}