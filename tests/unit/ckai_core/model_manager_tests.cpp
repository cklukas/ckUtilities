#include "ck/ai/model_manager.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string_view>

#include <gtest/gtest.h>

namespace {

class ModelManagerTest : public ::testing::Test {
protected:
  void SetUp() override {
    // Create temporary directory for test models
    test_dir_ =
        std::filesystem::temp_directory_path() /
        ("test_models_" +
         std::to_string(
             std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(test_dir_);

    // Initialize ModelManager with test directory
    modelManager_ = std::make_unique<ck::ai::ModelManager>();
    modelManager_->set_models_directory(test_dir_);

    // Clear any existing active model to ensure clean test state
    // This is important because ModelManager may have loaded config from the
    // global location
    auto activeModel = modelManager_->get_active_model();
    if (activeModel.has_value()) {
      modelManager_->deactivate_model(activeModel->id);
    }

    // Ensure downloaded models are also cleared
    auto downloadedModels = modelManager_->get_downloaded_models();
    for (const auto &model : downloadedModels) {
      if (model.is_active) {
        modelManager_->deactivate_model(model.id);
      }
    }
  }

  void TearDown() override {
    // Clean up test directory
    if (std::filesystem::exists(test_dir_)) {
      std::filesystem::remove_all(test_dir_);
    }
  }

  void createMockModelFile(const std::string &filename) {
    std::filesystem::path modelPath = test_dir_ / filename;
    std::ofstream file(modelPath);
    file << "Mock GGUF model content for testing";
    file.close();
  }

  std::filesystem::path test_dir_;
  std::unique_ptr<ck::ai::ModelManager> modelManager_;
};

// Basic functionality tests
TEST_F(ModelManagerTest, InitialState) {
  auto availableModels = modelManager_->get_available_models();
  auto downloadedModels = modelManager_->get_downloaded_models();
  auto activeModels = modelManager_->get_active_models();

  EXPECT_FALSE(availableModels.empty()); // Should have curated models
  EXPECT_TRUE(downloadedModels.empty()); // No models downloaded initially
  EXPECT_TRUE(activeModels.empty());     // No models active initially

  auto activeModel = modelManager_->get_active_model();
  EXPECT_FALSE(activeModel.has_value()); // No active model initially
}

TEST_F(ModelManagerTest, GetAvailableModels) {
  auto models = modelManager_->get_available_models();

  EXPECT_FALSE(models.empty());

  // Check that we have some expected models from the curated list
  bool foundTinyLlama = false;
  for (const auto &model : models) {
    if (model.name.find("TinyLlama") != std::string::npos) {
      foundTinyLlama = true;
      EXPECT_FALSE(model.id.empty());
      EXPECT_FALSE(model.filename.empty());
      EXPECT_FALSE(model.download_url.empty());
      EXPECT_GT(model.size_bytes, 0);
      EXPECT_FALSE(model.is_downloaded); // Initially not downloaded
      EXPECT_FALSE(model.is_active);     // Initially not active
    }
  }
  EXPECT_TRUE(foundTinyLlama);
}

TEST_F(ModelManagerTest, ModelNotDownloadedInitially) {
  auto models = modelManager_->get_available_models();
  ASSERT_FALSE(models.empty());

  const auto &model = models[0];
  EXPECT_FALSE(modelManager_->is_model_downloaded(model.id));
  EXPECT_FALSE(modelManager_->is_model_active(model.id));
}

// Activation/Deactivation tests (simulating downloaded models)
TEST_F(ModelManagerTest, ActivateNonExistentModel) {
  EXPECT_FALSE(modelManager_->activate_model("non-existent-model"));
}

TEST_F(ModelManagerTest, DeactivateNonExistentModel) {
  EXPECT_FALSE(modelManager_->deactivate_model("non-existent-model"));
}

TEST_F(ModelManagerTest, ActivateDeactivateSimulatedModel) {
  auto availableModels = modelManager_->get_available_models();
  ASSERT_FALSE(availableModels.empty());

  const auto &model = availableModels[0];

  // Simulate model being downloaded by creating the file
  createMockModelFile(model.filename);
  modelManager_->refresh_model_list(); // Refresh to detect the file

  auto downloadedModels = modelManager_->get_downloaded_models();
  ASSERT_FALSE(downloadedModels.empty());

  const auto &downloadedModel = downloadedModels[0];
  EXPECT_TRUE(modelManager_->is_model_downloaded(downloadedModel.id));
  EXPECT_FALSE(modelManager_->is_model_active(downloadedModel.id));

  // Test activation
  EXPECT_TRUE(modelManager_->activate_model(downloadedModel.id));
  EXPECT_TRUE(modelManager_->is_model_active(downloadedModel.id));

  auto activeModel = modelManager_->get_active_model();
  ASSERT_TRUE(activeModel.has_value());
  EXPECT_EQ(activeModel->id, downloadedModel.id);

  auto activeModels = modelManager_->get_active_models();
  EXPECT_EQ(activeModels.size(), 1);
  EXPECT_EQ(activeModels[0].id, downloadedModel.id);
  EXPECT_TRUE(activeModels[0].is_active);

  // Test deactivation
  EXPECT_TRUE(modelManager_->deactivate_model(downloadedModel.id));
  EXPECT_FALSE(modelManager_->is_model_active(downloadedModel.id));

  activeModel = modelManager_->get_active_model();
  EXPECT_FALSE(activeModel.has_value());

  activeModels = modelManager_->get_active_models();
  EXPECT_TRUE(activeModels.empty());
}

TEST_F(ModelManagerTest, ActivateSecondModelDeactivatesFirst) {
  auto availableModels = modelManager_->get_available_models();
  ASSERT_GE(availableModels.size(), 2);

  // Simulate two models being downloaded
  createMockModelFile(availableModels[0].filename);
  createMockModelFile(availableModels[1].filename);
  modelManager_->refresh_model_list();

  auto downloadedModels = modelManager_->get_downloaded_models();
  ASSERT_GE(downloadedModels.size(), 2);

  const auto &model1 = downloadedModels[0];
  const auto &model2 = downloadedModels[1];

  // Activate first model
  EXPECT_TRUE(modelManager_->activate_model(model1.id));
  EXPECT_TRUE(modelManager_->is_model_active(model1.id));
  EXPECT_FALSE(modelManager_->is_model_active(model2.id));

  // Activate second model should deactivate first
  EXPECT_TRUE(modelManager_->activate_model(model2.id));
  EXPECT_FALSE(modelManager_->is_model_active(model1.id));
  EXPECT_TRUE(modelManager_->is_model_active(model2.id));

  auto activeModel = modelManager_->get_active_model();
  ASSERT_TRUE(activeModel.has_value());
  EXPECT_EQ(activeModel->id, model2.id);
}

TEST_F(ModelManagerTest, DeleteModel) {
  auto availableModels = modelManager_->get_available_models();
  ASSERT_FALSE(availableModels.empty());

  const auto &model = availableModels[0];

  // Simulate model being downloaded
  createMockModelFile(model.filename);
  modelManager_->refresh_model_list();

  EXPECT_TRUE(modelManager_->is_model_downloaded(model.id));

  // Activate the model first
  EXPECT_TRUE(modelManager_->activate_model(model.id));
  EXPECT_TRUE(modelManager_->is_model_active(model.id));

  // Delete the model
  EXPECT_TRUE(modelManager_->delete_model(model.id));
  EXPECT_FALSE(modelManager_->is_model_downloaded(model.id));
  EXPECT_FALSE(modelManager_->is_model_active(model.id));

  // Check that file is deleted
  std::filesystem::path modelPath = test_dir_ / model.filename;
  EXPECT_FALSE(std::filesystem::exists(modelPath));

  // Check that no model is active
  auto activeModel = modelManager_->get_active_model();
  EXPECT_FALSE(activeModel.has_value());
}

TEST_F(ModelManagerTest, GetModelById) {
  auto availableModels = modelManager_->get_available_models();
  ASSERT_FALSE(availableModels.empty());

  const auto &expectedModel = availableModels[0];

  auto foundModel = modelManager_->get_model_by_id(expectedModel.id);
  ASSERT_TRUE(foundModel.has_value());
  EXPECT_EQ(foundModel->id, expectedModel.id);
  EXPECT_EQ(foundModel->name, expectedModel.name);
  EXPECT_EQ(foundModel->filename, expectedModel.filename);
}

TEST_F(ModelManagerTest, GetModelByIdNonExistent) {
  auto foundModel = modelManager_->get_model_by_id("non-existent-id");
  EXPECT_FALSE(foundModel.has_value());
}

TEST_F(ModelManagerTest, GetModelSize) {
  auto availableModels = modelManager_->get_available_models();
  ASSERT_FALSE(availableModels.empty());

  const auto &model = availableModels[0];
  std::size_t size = modelManager_->get_model_size(model.id);
  EXPECT_EQ(size, model.size_bytes);
  EXPECT_GT(size, 0);
}

TEST_F(ModelManagerTest, GetModelPath) {
  auto availableModels = modelManager_->get_available_models();
  ASSERT_FALSE(availableModels.empty());

  const auto &model = availableModels[0];
  auto path = modelManager_->get_model_path(model.id);

  EXPECT_EQ(path, test_dir_ / model.filename);
}

TEST_F(ModelManagerTest, RefreshModelList) {
  // Initially no downloaded models
  auto downloadedModels = modelManager_->get_downloaded_models();
  EXPECT_TRUE(downloadedModels.empty());

  // Create a model file
  auto availableModels = modelManager_->get_available_models();
  ASSERT_FALSE(availableModels.empty());
  createMockModelFile(availableModels[0].filename);

  // Refresh should detect the new file
  modelManager_->refresh_model_list();

  downloadedModels = modelManager_->get_downloaded_models();
  EXPECT_FALSE(downloadedModels.empty());
  EXPECT_EQ(downloadedModels[0].id, availableModels[0].id);
  EXPECT_TRUE(downloadedModels[0].is_downloaded);
}

TEST_F(ModelManagerTest, PersistentConfiguration) {
  auto availableModels = modelManager_->get_available_models();
  ASSERT_FALSE(availableModels.empty());

  const auto &model = availableModels[0];

  // Simulate model being downloaded and activated
  createMockModelFile(model.filename);
  modelManager_->refresh_model_list();
  EXPECT_TRUE(modelManager_->activate_model(model.id));

  // Create a new ModelManager instance to test persistence
  auto newModelManager = std::make_unique<ck::ai::ModelManager>();
  newModelManager->set_models_directory(test_dir_);

  // The active model should be restored
  auto activeModel = newModelManager->get_active_model();
  ASSERT_TRUE(activeModel.has_value());
  EXPECT_EQ(activeModel->id, model.id);
  EXPECT_TRUE(newModelManager->is_model_active(model.id));
}

// Edge cases and error handling
TEST_F(ModelManagerTest, ActivateAlreadyActiveModel) {
  auto availableModels = modelManager_->get_available_models();
  ASSERT_FALSE(availableModels.empty());

  const auto &model = availableModels[0];
  createMockModelFile(model.filename);
  modelManager_->refresh_model_list();

  // Activate model
  EXPECT_TRUE(modelManager_->activate_model(model.id));
  EXPECT_TRUE(modelManager_->is_model_active(model.id));

  // Activate again should still return true
  EXPECT_TRUE(modelManager_->activate_model(model.id));
  EXPECT_TRUE(modelManager_->is_model_active(model.id));
}

TEST_F(ModelManagerTest, DeactivateInactiveModel) {
  auto availableModels = modelManager_->get_available_models();
  ASSERT_FALSE(availableModels.empty());

  const auto &model = availableModels[0];
  createMockModelFile(model.filename);
  modelManager_->refresh_model_list();

  // Model is downloaded but not active
  EXPECT_TRUE(modelManager_->is_model_downloaded(model.id));
  EXPECT_FALSE(modelManager_->is_model_active(model.id));

  // Deactivate should still return true
  EXPECT_TRUE(modelManager_->deactivate_model(model.id));
  EXPECT_FALSE(modelManager_->is_model_active(model.id));
}

} // namespace
