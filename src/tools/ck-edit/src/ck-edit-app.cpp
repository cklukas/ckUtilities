#include "ck/edit/markdown_editor.hpp"

int main(int argc, char **argv)
{
    ck::edit::MarkdownEditorApp app(argc, argv);
    app.run();
    app.shutDown();
    return 0;
}
