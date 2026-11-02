#include "ck/file_io.hpp"

#include <filesystem>
#include <string>

#ifndef CKTOOLS_FILE_IO_TEST_ROOT
#error "CKTOOLS_FILE_IO_TEST_ROOT must be defined"
#endif

int main()
{
    const std::filesystem::path root = CKTOOLS_FILE_IO_TEST_ROOT;
#if defined(_WIN32)
    std::wstring filename(230, L'x');
    filename[17] = L'\u00e4';
    const std::filesystem::path file = root / filename;
#else
    const std::filesystem::path file = root / (std::string(230, 'x') + "-\xc3\xa4.txt");
#endif
    std::error_code error;
    ck::io::remove_file(file);
    std::filesystem::remove_all(root, error);
    if (error)
        return 1;
    std::filesystem::create_directories(root, error);
    if (error)
        return 1;

    ck::io::remove_file(file);
    std::filesystem::remove_all(root, error);
    if (error)
        return 1;
    std::filesystem::create_directories(root, error);
    if (error)
        return 1;
    const std::string first = "first line\n\x00\xc3\xbc";
    const std::string second = "\nsecond line\n";
    if (!ck::io::write_file(file, first) || !ck::io::write_file(file, second, ck::io::WriteMode::Append))
        return 2;

    std::string contents;
    if (!ck::io::read_file(file, contents) || contents != first + second)
        return 3;
    std::string prefix;
    if (!ck::io::read_file(file, prefix, 5) || prefix != first.substr(0, 5))
        return 4;

    if (!ck::io::remove_file(file))
        return 5;
    std::filesystem::remove_all(root, error);
    return error ? 6 : 0;
}
