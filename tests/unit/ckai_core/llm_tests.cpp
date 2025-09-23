#include "ck/ai/llm.hpp"

#include <gtest/gtest.h>

#include <string>

TEST(LlmTests, GeneratesDeterministicStub)
{
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

TEST(LlmTests, EmbedReturnsModelSpecificHash)
{
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
    ck::ai::RuntimeConfig runtime;
    auto llm = ck::ai::Llm::open("model", runtime);
    EXPECT_EQ(llm->token_count(""), 0u);
    EXPECT_EQ(llm->token_count("one"), 1u);
    EXPECT_EQ(llm->token_count("one two\tthree"), 3u);
}
