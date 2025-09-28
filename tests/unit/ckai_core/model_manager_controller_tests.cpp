#include "ck/ai/model_manager_controller.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string_view>

#include <gtest/gtest.h>

namespace {

class ModelManagerControllerTest : public ::testing::Test {
protected:
  void SetUp() override {
    // Create temporary directory for test models
    test_dir_ =
        std::filesystem::temp_directory_path() /
        ("test_controller_" +
         std::to_string(
             std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(test_dir_);

    // Initialize ModelManager and Controller with test directory
    modelManager_ = std::make_unique<ck::ai::ModelManager>();
    modelManager_->set_models_directory(test_dir_);

    controller_ =
        std::make_unique<ck::ai::ModelManagerController>(*modelManager_);

    // Set up callbacks to capture status/error messages
    statusMessages_.clear();
    errorMessages_.clear();
    modelListUpdateCount_ = 0;

    controller_->setStatusCallback(
        [this](const std::string &msg) { statusMessages_.push_back(msg); });

    controller_->setErrorCallback(
        [this](const std::string &msg) { errorMessages_.push_back(msg); });

    controller_->setModelListUpdateCallback(
        [this]() { modelListUpdateCount_++; });
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
  std::unique_ptr<ck::ai::ModelManagerController> controller_;
  std::vector<std::string> statusMessages_;
  std::vector<std::string> errorMessages_;
  int modelListUpdateCount_;
};

// Basic functionality tests
TEST_F(ModelManagerControllerTest, InitialState) {
  auto availableModels = controller_->getAvailableModels();
  auto downloadedModels = controller_->getDownloadedModels();

  EXPECT_FALSE(availableModels.empty()); // Should have curated models
  EXPECT_TRUE(downloadedModels.empty()); // No models downloaded initially

  EXPECT_EQ(controller_->getSelectedAvailableIndex(), -1);
  EXPECT_EQ(controller_->getSelectedDownloadedIndex(), -1);

  EXPECT_FALSE(controller_->getSelectedAvailableModel().has_value());
  EXPECT_FALSE(controller_->getSelectedDownloadedModel().has_value());
}

TEST_F(ModelManagerControllerTest, SelectionManagement) {
  auto availableModels = controller_->getAvailableModels();
  ASSERT_FALSE(availableModels.empty());

  // Test selection of available model
  controller_->setSelectedAvailableModel(0);
  EXPECT_EQ(controller_->getSelectedAvailableIndex(), 0);
  EXPECT_EQ(controller_->getSelectedDownloadedIndex(),
            -1); // Should clear other selection

  auto selectedModel = controller_->getSelectedAvailableModel();
  ASSERT_TRUE(selectedModel.has_value());
  EXPECT_EQ(selectedModel->id, availableModels[0].id);

  // Create a downloaded model to test selection
  createMockModelFile(availableModels[0].filename);
  controller_->refreshModels();

  auto downloadedModels = controller_->getDownloadedModels();
  ASSERT_FALSE(downloadedModels.empty());

  // Test selection of downloaded model
  controller_->setSelectedDownloadedModel(0);
  EXPECT_EQ(controller_->getSelectedDownloadedIndex(), 0);
  EXPECT_EQ(controller_->getSelectedAvailableIndex(),
            -1); // Should clear other selection

  auto selectedDownloaded = controller_->getSelectedDownloadedModel();
  ASSERT_TRUE(selectedDownloaded.has_value());
  EXPECT_EQ(selectedDownloaded->id, downloadedModels[0].id);

  // Test clear selection
  controller_->clearSelection();
  EXPECT_EQ(controller_->getSelectedAvailableIndex(), -1);
  EXPECT_EQ(controller_->getSelectedDownloadedIndex(), -1);
}

TEST_F(ModelManagerControllerTest, ValidationMethods) {
  auto availableModels = controller_->getAvailableModels();
  ASSERT_FALSE(availableModels.empty());

  // Initially no selection, so all actions should be invalid
  EXPECT_FALSE(controller_->canActivateSelected());
  EXPECT_FALSE(controller_->canDeactivateSelected());
  EXPECT_FALSE(controller_->canDeleteSelected());
  EXPECT_FALSE(controller_->canDownloadSelected());

  // Select an available model
  controller_->setSelectedAvailableModel(0);
  EXPECT_FALSE(controller_->canActivateSelected());   // Not downloaded
  EXPECT_FALSE(controller_->canDeactivateSelected()); // Not downloaded
  EXPECT_FALSE(controller_->canDeleteSelected());     // Not downloaded
  EXPECT_TRUE(
      controller_->canDownloadSelected()); // Available and not downloaded

  // Create the model file and refresh
  createMockModelFile(availableModels[0].filename);
  controller_->refreshModels();

  auto downloadedModels = controller_->getDownloadedModels();
  ASSERT_FALSE(downloadedModels.empty());

  // Select downloaded model
  controller_->setSelectedDownloadedModel(0);
  EXPECT_TRUE(controller_->canActivateSelected()); // Downloaded and not active
  EXPECT_FALSE(controller_->canDeactivateSelected()); // Not active yet
  EXPECT_TRUE(controller_->canDeleteSelected());      // Downloaded
  EXPECT_FALSE(
      controller_->canDownloadSelected()); // No available model selected
}

TEST_F(ModelManagerControllerTest, ActivateSelectedModel) {
  auto availableModels = controller_->getAvailableModels();
  ASSERT_FALSE(availableModels.empty());

  // Try to activate without selection - should fail
  EXPECT_FALSE(controller_->activateSelectedModel());
  EXPECT_FALSE(errorMessages_.empty());
  EXPECT_EQ(errorMessages_.back(),
            "Please select a model from the downloaded list first");

  // Create and select a downloaded model
  createMockModelFile(availableModels[0].filename);
  controller_->refreshModels();
  controller_->setSelectedDownloadedModel(0);

  // Now activation should work
  statusMessages_.clear();
  modelListUpdateCount_ = 0;

  EXPECT_TRUE(controller_->activateSelectedModel());
  EXPECT_FALSE(statusMessages_.empty());
  EXPECT_TRUE(statusMessages_.back().find("activated") != std::string::npos);
  EXPECT_GT(modelListUpdateCount_, 0);

  // Validation should update
  EXPECT_FALSE(controller_->canActivateSelected());  // Already active
  EXPECT_TRUE(controller_->canDeactivateSelected()); // Now active
}

TEST_F(ModelManagerControllerTest, DeactivateSelectedModel) {
  auto availableModels = controller_->getAvailableModels();
  ASSERT_FALSE(availableModels.empty());

  // Create, select, and activate a model
  createMockModelFile(availableModels[0].filename);
  controller_->refreshModels();
  controller_->setSelectedDownloadedModel(0);
  ASSERT_TRUE(controller_->activateSelectedModel());

  // Now test deactivation
  statusMessages_.clear();
  modelListUpdateCount_ = 0;

  EXPECT_TRUE(controller_->deactivateSelectedModel());
  EXPECT_FALSE(statusMessages_.empty());
  EXPECT_TRUE(statusMessages_.back().find("deactivated") != std::string::npos);
  EXPECT_GT(modelListUpdateCount_, 0);

  // Validation should update
  EXPECT_TRUE(controller_->canActivateSelected());    // No longer active
  EXPECT_FALSE(controller_->canDeactivateSelected()); // Not active
}

TEST_F(ModelManagerControllerTest, DeleteSelectedModel) {
  auto availableModels = controller_->getAvailableModels();
  ASSERT_FALSE(availableModels.empty());

  // Create and select a model
  createMockModelFile(availableModels[0].filename);
  controller_->refreshModels();
  controller_->setSelectedDownloadedModel(0);

  auto downloadedModels = controller_->getDownloadedModels();
  ASSERT_FALSE(downloadedModels.empty());

  // Test deletion
  statusMessages_.clear();
  modelListUpdateCount_ = 0;

  EXPECT_TRUE(controller_->deleteSelectedModel());
  EXPECT_FALSE(statusMessages_.empty());
  EXPECT_TRUE(statusMessages_.back().find("deleted") != std::string::npos);
  EXPECT_GT(modelListUpdateCount_, 0);

  // Selection should be cleared after deletion
  EXPECT_EQ(controller_->getSelectedDownloadedIndex(), -1);

  // Model should no longer be downloaded
  EXPECT_TRUE(controller_->getDownloadedModels().empty());
}

TEST_F(ModelManagerControllerTest, ErrorHandling) {
  // Test error when trying to activate non-existent model
  errorMessages_.clear();
  EXPECT_FALSE(controller_->activateModel("non-existent"));
  EXPECT_FALSE(errorMessages_.empty());
  EXPECT_TRUE(errorMessages_.back().find("not downloaded") !=
              std::string::npos);

  // Test error when trying to deactivate non-existent model
  errorMessages_.clear();
  EXPECT_FALSE(controller_->deactivateModel("non-existent"));
  EXPECT_FALSE(errorMessages_.empty());
  EXPECT_TRUE(errorMessages_.back().find("not downloaded") !=
              std::string::npos);

  // Test error when trying to delete non-existent model
  errorMessages_.clear();
  EXPECT_FALSE(controller_->deleteModel("non-existent"));
  EXPECT_FALSE(errorMessages_.empty());
  EXPECT_TRUE(errorMessages_.back().find("not downloaded") !=
              std::string::npos);
}

TEST_F(ModelManagerControllerTest, ModelDisplayFormatting) {
  auto availableModels = controller_->getAvailableModels();
  ASSERT_FALSE(availableModels.empty());

  const auto &model = availableModels[0];

  // Test display name
  std::string displayName = controller_->getModelDisplayName(model);
  EXPECT_EQ(displayName, model.name);

  // Test size formatting
  std::string sizeStr = controller_->formatModelSize(1024);
  EXPECT_EQ(sizeStr, "1.0 KB");

  sizeStr = controller_->formatModelSize(1024 * 1024);
  EXPECT_EQ(sizeStr, "1.0 MB");

  sizeStr = controller_->formatModelSize(1024ULL * 1024 * 1024);
  EXPECT_EQ(sizeStr, "1.0 GB");

  sizeStr = controller_->formatModelSize(0);
  EXPECT_EQ(sizeStr, "Unknown");

  // Test status text for non-downloaded model
  std::string statusText = controller_->getModelStatusText(model);
  EXPECT_TRUE(statusText.find("GB") != std::string::npos ||
              statusText.find("MB") != std::string::npos ||
              statusText.find("Unknown") != std::string::npos);
  EXPECT_TRUE(statusText.find("[X]") == std::string::npos); // Not active
  EXPECT_TRUE(statusText.find("[ ]") == std::string::npos); // Not downloaded
}

TEST_F(ModelManagerControllerTest, ModelStatusText) {
  auto availableModels = controller_->getAvailableModels();
  ASSERT_FALSE(availableModels.empty());

  // Create and activate a model
  createMockModelFile(availableModels[0].filename);
  controller_->refreshModels();

  auto downloadedModels = controller_->getDownloadedModels();
  ASSERT_FALSE(downloadedModels.empty());

  // Test inactive downloaded model
  auto inactiveModel = downloadedModels[0];
  std::string statusText = controller_->getModelStatusText(inactiveModel);
  EXPECT_TRUE(statusText.find("[ ]") != std::string::npos); // Inactive checkbox

  // Activate the model
  controller_->activateModel(inactiveModel.id);

  // Get updated model info
  downloadedModels = controller_->getDownloadedModels();
  auto activeModel = downloadedModels[0];
  statusText = controller_->getModelStatusText(activeModel);
  EXPECT_TRUE(statusText.find("[X]") != std::string::npos); // Active checkbox
}

TEST_F(ModelManagerControllerTest, RefreshModels) {
  auto initialAvailable = controller_->getAvailableModels();
  auto initialDownloaded = controller_->getDownloadedModels();

  EXPECT_FALSE(initialAvailable.empty());
  EXPECT_TRUE(initialDownloaded.empty());

  // Create a model file
  ASSERT_FALSE(initialAvailable.empty());
  createMockModelFile(initialAvailable[0].filename);

  // Refresh should detect the new file
  statusMessages_.clear();
  modelListUpdateCount_ = 0;

  controller_->refreshModels();

  EXPECT_FALSE(statusMessages_.empty());
  EXPECT_TRUE(statusMessages_.back().find("refreshed") != std::string::npos);
  EXPECT_GT(modelListUpdateCount_, 0);

  auto newDownloaded = controller_->getDownloadedModels();
  EXPECT_FALSE(newDownloaded.empty());
  EXPECT_EQ(newDownloaded[0].id, initialAvailable[0].id);
}

TEST_F(ModelManagerControllerTest, CallbackIntegration) {
  // Test that all callbacks are properly triggered
  statusMessages_.clear();
  errorMessages_.clear();
  modelListUpdateCount_ = 0;

  // Trigger various operations that should call callbacks
  auto availableModels = controller_->getAvailableModels();
  ASSERT_FALSE(availableModels.empty());

  createMockModelFile(availableModels[0].filename);
  controller_->refreshModels(); // Should trigger status and modelListUpdate

  EXPECT_FALSE(statusMessages_.empty());
  EXPECT_GT(modelListUpdateCount_, 0);

  // Test error callback
  errorMessages_.clear();
  controller_->activateModel("non-existent");
  EXPECT_FALSE(errorMessages_.empty());
}

} // namespace
