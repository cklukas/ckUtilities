#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace ck::ai {

struct ModelInfo {
  std::string id;          // Unique identifier (e.g., "tinyllama-1.1b")
  std::string name;        // Display name (e.g., "TinyLlama 1.1B")
  std::string description; // Model description
  std::string
      filename; // Model filename (e.g., "tinyllama-1.1b-chat-v1.0.Q4_K_M.gguf")
  std::string download_url;          // URL to download the model
  std::filesystem::path local_path;  // Local file path
  std::size_t size_bytes;            // Model size in bytes
  std::string hardware_requirements; // e.g., "CPU", "GPU < 8GB", "GPU < 16GB"
  bool is_downloaded;                // Whether model is downloaded locally
  bool is_active;       // Whether model is currently active/selected
  std::string category; // e.g., "CPU Fast", "GPU Small", "GPU Large"
  std::size_t default_context_window_tokens = 0;
  std::size_t default_max_output_tokens = 0;
  std::size_t default_summary_trigger_tokens = 0;
  std::vector<std::string> default_stop_sequences;
};

struct ModelDownloadProgress {
  std::string model_id;
  std::size_t bytes_downloaded;
  std::size_t total_bytes;
  bool is_complete;
  std::string error_message;
  double progress_percentage;
};

class ModelManager {
public:
  ModelManager();
  ~ModelManager();

  // Model discovery and listing
  std::vector<ModelInfo> get_available_models() const;
  std::vector<ModelInfo> get_downloaded_models() const;
  std::vector<ModelInfo> get_active_models() const;
  std::optional<ModelInfo> get_model_by_id(const std::string &id) const;
  std::optional<ModelInfo> get_active_model() const;

  // Model management
  bool download_model(const std::string &model_id,
                      std::function<void(const ModelDownloadProgress &)>
                          progress_callback = nullptr,
                      std::string *error_message = nullptr);
  bool activate_model(const std::string &model_id);
  bool deactivate_model(const std::string &model_id);
  bool delete_model(const std::string &model_id);
  bool is_model_downloaded(const std::string &model_id) const;
  bool is_model_active(const std::string &model_id) const;

  // Model information
  std::filesystem::path get_models_directory() const;
  std::filesystem::path get_model_path(const std::string &model_id) const;
  std::size_t get_model_size(const std::string &model_id) const;

  // Configuration
  void set_models_directory(const std::filesystem::path &path);
  void refresh_model_list();

private:
  std::filesystem::path models_directory_;
  std::vector<ModelInfo> available_models_;
  std::vector<ModelInfo> downloaded_models_;
  std::string active_model_id_;

  void load_curated_models();
  void scan_downloaded_models();
  void save_configuration();
  void load_configuration();
  bool download_file(
      const std::string &url, const std::filesystem::path &destination,
      std::function<void(const ModelDownloadProgress &)> progress_callback,
      const std::string &model_id = "", std::string *error_message = nullptr);
  std::string generate_model_id(const std::string &name) const;
};

} // namespace ck::ai
