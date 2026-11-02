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
    const std::filesystem::path directory = root / filename;
    const std::filesystem::path file = directory / L"payload.txt";
#else
    const std::filesystem::path directory = root / (std::string(230, 'x') + "-\xc3\xa4");
    const std::filesystem::path file = directory / "payload.txt";
#endif
    ck::io::remove_file(file);
    if (!ck::io::remove_tree(root))
        return 11;
    if (!ck::io::create_directories(root))
        return 12;
    if (!ck::io::create_directories(directory))
        return 13;

    ck::io::remove_file(file);
    if (!ck::io::remove_tree(root))
        return 14;
    if (!ck::io::create_directories(root))
        return 15;
    if (!ck::io::create_directories(directory))
        return 16;
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
    return ck::io::remove_tree(root) ? 0 : 6;
}
