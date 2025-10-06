#include <gtest/gtest.h>

#include "ck/hotkeys.hpp"
#include "ck/commands/ck_edit.hpp"

namespace
{

void ensureRegistered()
{
    ck::hotkeys::registerDefaultSchemes();
}

} // namespace

TEST(Hotkeys, RegistersDefaultScheme)
{
    ensureRegistered();
#if defined(__APPLE__)
    EXPECT_STREQ("mac", ck::hotkeys::activeScheme().data());
#else
    EXPECT_STREQ("linux", ck::hotkeys::activeScheme().data());
#endif
}

TEST(Hotkeys, LookupReturnsCkEditBinding)
{
    ensureRegistered();
    const auto *binding = ck::hotkeys::lookup(ck::commands::edit::cmToggleWrap);
    ASSERT_NE(nullptr, binding);
    EXPECT_NE(0, binding->key.code);
    EXPECT_FALSE(binding->display.empty());
}

TEST(Hotkeys, ApplyCommandLineSchemeOverrides)
{
    ensureRegistered();
    ck::hotkeys::setActiveScheme("linux");
    int argc = 2;
    char arg0[] = "ck-test";
    char arg1[] = "--hotkeys=mac";
    char *argv[] = {arg0, arg1, nullptr};
    ck::hotkeys::applyCommandLineScheme(argc, argv);
    EXPECT_EQ(1, argc);
    EXPECT_STREQ("ck-test", argv[0]);
    EXPECT_STREQ("mac", ck::hotkeys::activeScheme().data());
}

TEST(Hotkeys, CommandLabelsProvideDisplayNames)
{
    ensureRegistered();
    EXPECT_EQ("Toggle Wrap", ck::hotkeys::commandLabel(ck::commands::edit::cmToggleWrap));
    EXPECT_EQ("Quit", ck::hotkeys::commandLabel(cmQuit));
}
