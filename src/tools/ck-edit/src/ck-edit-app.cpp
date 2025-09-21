#include "ck/edit/markdown_editor.hpp"

#include <iostream>
#include <string_view>

namespace
{

void printHelp()
{
    std::cout << ck::edit::appName() << " - " << ck::edit::appShortDescription() << "\n\n";
    std::cout << "Usage: " << ck::edit::appName() << " [FILE...]\n";
    std::cout << "Launch the editor and open each FILE provided on the command line." << std::endl;
}

bool isHelpFlag(std::string_view arg)
{
    return arg == "--help" || arg == "-h";
}

} // namespace

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; ++i)
    {
        if (isHelpFlag(std::string_view{argv[i]}))
        {
            printHelp();
            return 0;
        }
    }

    ck::edit::MarkdownEditorApp app(argc, argv);
    app.run();
    app.shutDown();
    return 0;
}
