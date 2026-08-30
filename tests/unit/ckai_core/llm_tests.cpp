#include "ck/ai/llm.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>

namespace
{
class ScopedStubMode final
{
public:
    explicit ScopedStubMode(bool enabled)
    {
        if (const char *existing = std::getenv("CK_AI_FORCE_STUB"))
            previous_ = existing;
#ifdef _WIN32
        _putenv_s("CK_AI_FORCE_STUB", enabled ? "1" : "");
#else
        if (enabled)
            setenv("CK_AI_FORCE_STUB", "1", 1);
        else
            unsetenv("CK_AI_FORCE_STUB");
#endif
    }

    ~ScopedStubMode()
    {
#ifdef _WIN32
        _putenv_s("CK_AI_FORCE_STUB", previous_ ? previous_->c_str() : "");
#else
        if (previous_)
            setenv("CK_AI_FORCE_STUB", previous_->c_str(), 1);
        else
            unsetenv("CK_AI_FORCE_STUB");
#endif
    }

private:
    std::optional<std::string> previous_;
};
} // namespace

TEST(LlmTests, GeneratesDeterministicStub)
{
    ScopedStubMode stub_mode(true);
    ck::ai::RuntimeConfig runtime;
    runtime.model_path = "model.gguf";
    auto llm = ck::ai::Llm::open(runtime.model_path, runtime);
    ASSERT_NE(llm, nullptr);

    std::string collected;
    llm->set_system_prompt("system");
    ck::ai::GenerationConfig config;
    llm->generate("hello", config, [&](ck::ai::Chunk chunk) {
        collected.append(chunk.text);
        if (chunk.is_last)
            collected.push_back('\n');
    });

    EXPECT_NE(collected.find("[ck-ai]"), std::string::npos);
    EXPECT_NE(collected.find("hello"), std::string::npos);
}

TEST(LlmTests, ReportsUnavailableRealModel)
{
    ScopedStubMode stub_mode(false);
    ck::ai::RuntimeConfig runtime;
    const std::filesystem::path missing = std::filesystem::temp_directory_path() / "ck-utilities-missing-model.gguf";
    auto llm = ck::ai::Llm::open(missing.string(), runtime);

    ASSERT_NE(llm, nullptr);
    EXPECT_FALSE(llm->ready());
    EXPECT_FALSE(llm->load_error().empty());
    bool emitted = false;
    llm->generate_cancellable("hello", {}, [&emitted](ck::ai::Chunk) {
        emitted = true;
        return true;
    });
    EXPECT_FALSE(emitted);
}

TEST(LlmTests, EmbedReturnsModelSpecificHash)
{
    ScopedStubMode stub_mode(true);
    ck::ai::RuntimeConfig runtime;
    runtime.model_path = "model.gguf";
    auto llm = ck::ai::Llm::open(runtime.model_path, runtime);
    ASSERT_NE(llm, nullptr);

    auto a = llm->embed("foo");
    auto b = llm->embed("foo");
    auto c = llm->embed("bar");

    EXPECT_EQ(a, b);
    EXPECT_NE(a, c);
    EXPECT_NE(a.find(runtime.model_path), std::string::npos);
}

TEST(LlmTests, TokenCountSplitsOnWhitespace)
{
    ScopedStubMode stub_mode(true);
    ck::ai::RuntimeConfig runtime;
    auto llm = ck::ai::Llm::open("model", runtime);
    EXPECT_EQ(llm->token_count(""), 0u);
    EXPECT_EQ(llm->token_count("one"), 1u);
    EXPECT_EQ(llm->token_count("one two\tthree"), 3u);
}

TEST(LlmTests, CancellableGenerationStopsAfterTheCallbackDeclinesMoreOutput)
{
    ScopedStubMode stub_mode(true);
    ck::ai::RuntimeConfig runtime;
    auto llm = ck::ai::Llm::open("model", runtime);
    ASSERT_NE(llm, nullptr);

    std::size_t callbacks = 0;
    llm->generate_cancellable("hello", {}, [&](ck::ai::Chunk) {
        ++callbacks;
        return false;
    });

    EXPECT_EQ(callbacks, 1u);
}
