#include "ck/ai/model_manager.hpp"

#include <chrono>
#include <curl/curl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <thread>

namespace ck::ai {

namespace {
// Curated list of state-of-the-art models for different hardware configurations
constexpr std::size_t KiB = 1024ull;
constexpr std::size_t MiB = KiB * KiB;
constexpr std::size_t GiB = KiB * MiB;

const std::vector<ModelInfo> CURATED_MODELS = {
    // CPU Models (Fast)
    {"tinyllama-1.1b", "TinyLlama 1.1B",
     "Fast, lightweight model for CPU inference",
     "tinyllama-1.1b-chat-v1.0.Q4_K_M.gguf",
     "https://huggingface.co/TheBloke/TinyLlama-1.1B-Chat-v1.0-GGUF/resolve/"
     "main/tinyllama-1.1b-chat-v1.0.Q4_K_M.gguf",
     "",
     636ull * MiB, // ~636 MB
     "CPU", false, false, "CPU Fast"},
    {"phi-3-mini", "Phi-3 Mini 3.8B", "Microsoft's efficient small model",
     "phi-3-mini-4k-instruct-q4.gguf",
     "https://huggingface.co/microsoft/Phi-3-mini-4k-instruct-gguf/resolve/"
     "main/Phi-3-mini-4k-instruct-q4.gguf",
     "",
     2ull * GiB, // ~2GB
     "CPU", false, false, "CPU Fast"},

    // GPU Models (Small - < 8GB)
    {"llama-3.2-3b", "Llama 3.2 3B", "Meta's latest 3B model",
     "llama-3.2-3b-instruct-q4_k_m.gguf",
     "https://huggingface.co/bartowski/Llama-3.2-3B-Instruct-GGUF/resolve/main/"
     "Llama-3.2-3B-Instruct-Q4_K_M.gguf",
     "",
     2ull * GiB, // ~2GB
     "GPU < 8GB", false, false, "GPU Small"},
    {"qwen-2.5-7b", "Qwen 2.5 7B", "Alibaba's efficient 7B model",
     "qwen2.5-7b-instruct-q4_k_m.gguf",
     "https://huggingface.co/Qwen/Qwen2.5-7B-Instruct-GGUF/resolve/main/"
     "qwen2.5-7b-instruct-q4_k_m.gguf",
     "",
     4ull * GiB, // ~4GB
     "GPU < 8GB", false, false, "GPU Small"},

    // GPU Models (Medium - < 16GB)
    {"llama-3.1-8b", "Llama 3.1 8B",
     "Meta's 8B model with excellent performance",
     "llama-3.1-8b-instruct-q4_k_m.gguf",
     "https://huggingface.co/bartowski/Llama-3.1-8B-Instruct-GGUF/resolve/main/"
     "Llama-3.1-8B-Instruct-Q4_K_M.gguf",
     "",
     5ull * GiB, // ~5GB
     "GPU < 16GB", false, false, "GPU Medium"},
    {"gemma-2-9b", "Gemma 2 9B", "Google's efficient 9B model",
     "gemma-2-9b-it-q4_k_m.gguf",
     "https://huggingface.co/bartowski/gemma-2-9b-it-GGUF/resolve/main/"
     "gemma-2-9b-it-Q4_K_M.gguf",
     "",
     5ull * GiB + 512ull * MiB, // ~5.5GB
     "GPU < 16GB", false, false, "GPU Medium"},

    // GPU Models (Large - < 32GB)
    {"llama-3.1-70b", "Llama 3.1 70B", "Meta's flagship 70B model",
     "llama-3.1-70b-instruct-q4_k_m.gguf",
     "https://huggingface.co/bartowski/Llama-3.1-70B-Instruct-GGUF/resolve/"
     "main/Llama-3.1-70B-Instruct-Q4_K_M.gguf",
     "",
     40ull * GiB, // ~40GB
     "GPU < 32GB", false, false, "GPU Large"},
    {"qwen-2.5-32b", "Qwen 2.5 32B", "Alibaba's powerful 32B model",
     "qwen2.5-32b-instruct-q4_k_m.gguf",
     "https://huggingface.co/Qwen/Qwen2.5-32B-Instruct-GGUF/resolve/main/"
     "qwen2.5-32b-instruct-q4_k_m.gguf",
     "",
     20ull * GiB, // ~20GB
     "GPU < 32GB", false, false, "GPU Large"},

    // Recent OpenAI Models
    {"o1-preview", "o1-preview", "OpenAI's reasoning model (when available)",
     "o1-preview.gguf",
     "https://huggingface.co/example/o1-preview-gguf/resolve/main/"
     "o1-preview.gguf",
     "",
     10ull * GiB, // ~10GB
     "GPU < 16GB", false, false, "OpenAI Models"}};

// CURL write callback for downloads
size_t WriteCallback(void *contents, size_t size, size_t nmemb, void *userp) {
  size_t total_size = size * nmemb;
  std::ofstream *file = static_cast<std::ofstream *>(userp);
  file->write(static_cast<char *>(contents), total_size);
  return total_size;
}

// CURL progress callback
int ProgressCallback(void *clientp, curl_off_t dltotal, curl_off_t dlnow,
                     curl_off_t ultotal, curl_off_t ulnow) {
  auto *progress_callback =
      static_cast<std::function<void(const ModelDownloadProgress &)> *>(
          clientp);
  if (progress_callback && *progress_callback) {
    ModelDownloadProgress progress;
    progress.bytes_downloaded = static_cast<std::size_t>(dlnow);
    progress.total_bytes = static_cast<std::size_t>(dltotal);
    progress.is_complete = (dlnow >= dltotal && dltotal > 0);
    (*progress_callback)(progress);
  }
  return 0;
}
} // namespace

ModelManager::ModelManager() {
  // Initialize CURL
  curl_global_init(CURL_GLOBAL_DEFAULT);

  // Set default models directory
  std::filesystem::path home = std::getenv("HOME") ? std::getenv("HOME") : ".";
  models_directory_ = home / ".local" / "share" / "cktools" / "models" / "llm";

  // Create models directory if it doesn't exist
  std::filesystem::create_directories(models_directory_);

  load_curated_models();
  scan_downloaded_models();
  load_configuration();
}

ModelManager::~ModelManager() { curl_global_cleanup(); }

std::vector<ModelInfo> ModelManager::get_available_models() const {
  return available_models_;
}

std::vector<ModelInfo> ModelManager::get_downloaded_models() const {
  return downloaded_models_;
}

std::vector<ModelInfo> ModelManager::get_active_models() const {
  std::vector<ModelInfo> active_models;
  for (const auto &model : downloaded_models_) {
    if (model.is_active)
      active_models.push_back(model);
  }
  return active_models;
}

std::optional<ModelInfo>
ModelManager::get_model_by_id(const std::string &id) const {
  for (const auto &model : available_models_) {
    if (model.id == id)
      return model;
  }
  return std::nullopt;
}

std::optional<ModelInfo> ModelManager::get_active_model() const {
  if (active_model_id_.empty())
    return std::nullopt;

  return get_model_by_id(active_model_id_);
}

bool ModelManager::download_model(
    const std::string &model_id,
    std::function<void(const ModelDownloadProgress &)> progress_callback) {
  auto model_opt = get_model_by_id(model_id);
  if (!model_opt)
    return false;

  const auto &model = *model_opt;
  std::filesystem::path destination = models_directory_ / model.filename;

  // Create destination directory if it doesn't exist
  std::filesystem::create_directories(destination.parent_path());

  return download_file(model.download_url, destination, progress_callback);
}

bool ModelManager::activate_model(const std::string &model_id) {
  // Check if model is downloaded by looking in downloaded_models_ list
  bool found = false;
  for (const auto &model : downloaded_models_) {
    if (model.id == model_id) {
      found = true;
      break;
    }
  }

  if (!found)
    return false;

  // Deactivate current model
  if (!active_model_id_.empty()) {
    for (auto &model : downloaded_models_) {
      if (model.id == active_model_id_) {
        model.is_active = false;
        break;
      }
    }
  }

  // Activate new model
  active_model_id_ = model_id;
  for (auto &model : downloaded_models_) {
    if (model.id == model_id) {
      model.is_active = true;
      break;
    }
  }

  save_configuration();
  return true;
}

bool ModelManager::deactivate_model(const std::string &model_id) {
  // Find the model and deactivate it
  bool found = false;
  for (auto &model : downloaded_models_) {
    if (model.id == model_id) {
      model.is_active = false;
      found = true;
      break;
    }
  }

  if (!found)
    return false;

  // If this was the active model, clear active model
  if (active_model_id_ == model_id) {
    active_model_id_.clear();
  }

  save_configuration();
  return true;
}

bool ModelManager::delete_model(const std::string &model_id) {
  // Check if model is downloaded by looking in downloaded_models_ list
  bool found = false;
  std::string filename;
  for (const auto &model : downloaded_models_) {
    if (model.id == model_id) {
      found = true;
      filename = model.filename;
      break;
    }
  }

  if (!found)
    return false;

  std::filesystem::path model_path = models_directory_ / filename;
  if (std::filesystem::exists(model_path)) {
    std::filesystem::remove(model_path);
  }

  // Remove from downloaded models
  downloaded_models_.erase(std::remove_if(downloaded_models_.begin(),
                                          downloaded_models_.end(),
                                          [&model_id](const ModelInfo &model) {
                                            return model.id == model_id;
                                          }),
                           downloaded_models_.end());

  // If this was the active model, clear active model
  if (active_model_id_ == model_id) {
    active_model_id_.clear();
    save_configuration();
  }

  return true;
}

bool ModelManager::is_model_downloaded(const std::string &model_id) const {
  for (const auto &model : downloaded_models_) {
    if (model.id == model_id)
      return true;
  }
  return false;
}

bool ModelManager::is_model_active(const std::string &model_id) const {
  return active_model_id_ == model_id;
}

std::filesystem::path ModelManager::get_models_directory() const {
  return models_directory_;
}

std::filesystem::path
ModelManager::get_model_path(const std::string &model_id) const {
  auto model_opt = get_model_by_id(model_id);
  if (!model_opt)
    return {};

  return models_directory_ / model_opt->filename;
}

std::size_t ModelManager::get_model_size(const std::string &model_id) const {
  auto model_opt = get_model_by_id(model_id);
  if (!model_opt)
    return 0;

  return model_opt->size_bytes;
}

void ModelManager::set_models_directory(const std::filesystem::path &path) {
  models_directory_ = path;
  std::filesystem::create_directories(models_directory_);
  scan_downloaded_models();
}

void ModelManager::refresh_model_list() { scan_downloaded_models(); }

void ModelManager::load_curated_models() { available_models_ = CURATED_MODELS; }

void ModelManager::scan_downloaded_models() {
  downloaded_models_.clear();

  if (!std::filesystem::exists(models_directory_))
    return;

  for (const auto &entry :
       std::filesystem::directory_iterator(models_directory_)) {
    if (entry.is_regular_file() && entry.path().extension() == ".gguf") {
      // Find matching model in available models
      for (const auto &available_model : available_models_) {
        if (entry.path().filename() == available_model.filename) {
          ModelInfo downloaded_model = available_model;
          downloaded_model.local_path = entry.path();
          downloaded_model.is_downloaded = true;
          downloaded_model.is_active =
              (downloaded_model.id == active_model_id_);
          downloaded_models_.push_back(downloaded_model);
          break;
        }
      }
    }
  }
}

void ModelManager::save_configuration() {
  std::filesystem::path config_path = models_directory_ / "active_model.json";
  nlohmann::json config;
  config["active_model_id"] = active_model_id_;

  std::ofstream file(config_path);
  if (file.is_open()) {
    file << config.dump(2);
  }
}

void ModelManager::load_configuration() {
  std::filesystem::path config_path = models_directory_ / "active_model.json";
  if (!std::filesystem::exists(config_path))
    return;

  std::ifstream file(config_path);
  if (!file.is_open())
    return;

  try {
    nlohmann::json config;
    file >> config;
    if (config.contains("active_model_id")) {
      active_model_id_ = config["active_model_id"];
    }
  } catch (const std::exception &) {
    // Ignore JSON parsing errors
  }
}

bool ModelManager::download_file(
    const std::string &url, const std::filesystem::path &destination,
    std::function<void(const ModelDownloadProgress &)> progress_callback) {
  CURL *curl = curl_easy_init();
  if (!curl)
    return false;

  std::ofstream file(destination, std::ios::binary);
  if (!file.is_open()) {
    curl_easy_cleanup(curl);
    return false;
  }

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &file);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
  curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, ProgressCallback);
  curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &progress_callback);

  CURLcode res = curl_easy_perform(curl);
  file.close();

  bool success = (res == CURLE_OK);
  if (!success) {
    std::filesystem::remove(destination);
  }

  curl_easy_cleanup(curl);
  return success;
}

std::string ModelManager::generate_model_id(const std::string &name) const {
  std::string id = name;
  std::transform(id.begin(), id.end(), id.begin(), ::tolower);
  std::replace(id.begin(), id.end(), ' ', '-');
  return id;
}

} // namespace ck::ai
