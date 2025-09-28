#pragma once

#include "../../../../include/ck/ai/model_manager.hpp"

#include "../tvision_include.hpp"

#include <memory>
#include <string>
#include <vector>

class SimpleModelManager {
public:
  SimpleModelManager(ck::ai::ModelManager &modelManager);

  void showModelInfo();
  void downloadFirstModel();
  void activateFirstModel();
  void deactivateFirstModel();
  void deleteFirstModel();
  void refreshModels();

private:
  ck::ai::ModelManager &modelManager_;
  std::vector<ck::ai::ModelInfo> availableModels_;
  std::vector<ck::ai::ModelInfo> downloadedModels_;

  void refreshModelList();
  void formatModelSize(std::size_t bytes, std::string &result) const;
};
