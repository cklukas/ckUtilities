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

constexpr std::size_t align_tokens(std::size_t value, std::size_t step = 64)
{
  return step == 0 ? value : ((value + step - 1) / step) * step;
}

constexpr std::size_t recommended_response_tokens(std::size_t context)
{
  if (context == 0)
    return 0;
  std::size_t value = (context * 3) / 10; // 30% of context window
  if (value < 256)
    value = 256;
  if (value >= context)
  {
    value = context > 1024 ? context - 1024 : context / 2;
    if (value == 0)
      value = context;
  }
  value = align_tokens(value);
  if (value >= context)
    value = context > 64 ? context - 64 : context;
  return value;
}

constexpr std::size_t recommended_summary_trigger(std::size_t context)
{
  if (context == 0)
    return 0;
  std::size_t value = context / 2;
  if (value == 0)
    value = context;
  return align_tokens(value);
}

const std::vector<ModelInfo> CURATED_MODELS = {
    // CPU Models (Fast)
    {"tinyllama-1.1b", "TinyLlama 1.1B",
     "Fast, lightweight model for CPU inference",
     "tinyllama-1.1b-chat-v1.0.Q4_K_M.gguf",
     "https://huggingface.co/TheBloke/TinyLlama-1.1B-Chat-v1.0-GGUF/resolve/"
     "main/tinyllama-1.1b-chat-v1.0.Q4_K_M.gguf",
     "",
     636ull * MiB, // ~636 MB
     "CPU", false, false, "CPU Fast",
     2048, recommended_response_tokens(2048), recommended_summary_trigger(2048),
     {}},
    {"phi-3-mini", "Phi-3 Mini 3.8B", "Microsoft's efficient small model",
     "phi-3-mini-4k-instruct-q4.gguf",
     "https://huggingface.co/microsoft/Phi-3-mini-4k-instruct-gguf/resolve/"
     "main/Phi-3-mini-4k-instruct-q4.gguf",
     "",
     2ull * GiB, // ~2GB
     "CPU", false, false, "CPU Fast",
     4096, recommended_response_tokens(4096), recommended_summary_trigger(4096),
     {}},

    // GPU Models (Small - < 8GB)
    {"llama-3.2-3b", "Llama 3.2 3B", "Meta's latest 3B model",
     "llama-3.2-3b-instruct-q4_k_m.gguf",
     "https://huggingface.co/bartowski/Llama-3.2-3B-Instruct-GGUF/resolve/main/"
     "Llama-3.2-3B-Instruct-Q4_K_M.gguf",
     "",
     2ull * GiB, // ~2GB
     "GPU < 8GB", false, false, "GPU Small",
     8192, recommended_response_tokens(8192), recommended_summary_trigger(8192),
     {}},
    {"qwen-2.5-7b", "Qwen 2.5 7B", "Alibaba's efficient 7B model",
     "qwen2.5-7b-instruct-q4_k_m.gguf",
     "https://huggingface.co/bartowski/Qwen2.5-7B-Instruct-GGUF/resolve/main/"
     "qwen2.5-7b-instruct-q4_k_m.gguf",
     "",
     4ull * GiB, // ~4GB
     "GPU < 8GB", false, false, "GPU Small",
     131072, recommended_response_tokens(131072),
     recommended_summary_trigger(131072),
     {}},

    // GPU Models (Medium - < 16GB)
    {"llama-3.1-8b", "Llama 3.1 8B",
     "Meta's 8B model with excellent performance",
     "llama-3.1-8b-instruct-q4_k_m.gguf",
     "https://huggingface.co/bartowski/Llama-3.1-8B-Instruct-GGUF/resolve/main/"
     "Llama-3.1-8B-Instruct-Q4_K_M.gguf",
     "",
     5ull * GiB, // ~5GB
     "GPU < 16GB", false, false, "GPU Medium",
     131072, recommended_response_tokens(131072),
     recommended_summary_trigger(131072),
     {}},
    {"gemma-2-9b", "Gemma 2 9B", "Google's efficient 9B model",
     "gemma-2-9b-it-q4_k_m.gguf",
     "https://huggingface.co/bartowski/gemma-2-9b-it-GGUF/resolve/main/"
     "gemma-2-9b-it-Q4_K_M.gguf",
     "",
     5ull * GiB + 512ull * MiB, // ~5.5GB
     "GPU < 16GB", false, false, "GPU Medium",
     8192, recommended_response_tokens(8192), recommended_summary_trigger(8192),
     {}},

    // GPU Models (Large - < 32GB)
    {"llama-3.1-70b", "Llama 3.1 70B", "Meta's flagship 70B model",
     "llama-3.1-70b-instruct-q4_k_m.gguf",
     "https://huggingface.co/bartowski/Llama-3.1-70B-Instruct-GGUF/resolve/"
     "main/Llama-3.1-70B-Instruct-Q4_K_M.gguf",
     "",
     40ull * GiB, // ~40GB
     "GPU < 32GB", false, false, "GPU Large",
     131072, recommended_response_tokens(131072),
     recommended_summary_trigger(131072),
     {}},
    {"qwen-2.5-32b", "Qwen 2.5 32B", "Alibaba's powerful 32B model",
     "qwen2.5-32b-instruct-q4_k_m.gguf",
     "https://huggingface.co/bartowski/Qwen2.5-32B-Instruct-GGUF/resolve/main/"
     "qwen2.5-32b-instruct-q4_k_m.gguf",
     "",
     20ull * GiB, // ~20GB
     "GPU < 32GB", false, false, "GPU Large",
     131072, recommended_response_tokens(131072),
     recommended_summary_trigger(131072),
     {}},

    // OpenAI Open Source Models (GPT-OSS)
    {"gpt-oss-20b", "GPT-OSS 20B", "OpenAI's 20B parameter open-source model",
     "gpt-oss-20b-mxfp4.gguf",
     "https://huggingface.co/lmstudio-community/gpt-oss-20b-GGUF/resolve/main/"
     "gpt-oss-20b-MXFP4.gguf",
     "",
     12ull * GiB, // ~12GB
     "GPU < 24GB", false, false, "OpenAI Models",
     8192, recommended_response_tokens(8192), recommended_summary_trigger(8192),
     {"<|start|>user"}},
    {"gpt-oss-120b", "GPT-OSS 120B",
     "OpenAI's 120B parameter open-source model", "gpt-oss-120b-mxfp4.gguf",
     "https://huggingface.co/lmstudio-community/gpt-oss-120b-GGUF/resolve/main/"
     "gpt-oss-120b-MXFP4.gguf",
     "",
     60ull * GiB, // ~60GB
     "GPU < 80GB", false, false, "OpenAI Models",
     8192, recommended_response_tokens(8192), recommended_summary_trigger(8192),
     {"<|start|>user"}},

    // Additional CPU Models
    {"gemma-2-2b", "Gemma 2 2B", "Google's efficient 2B model",
     "gemma-2-2b-it-q4_k_m.gguf",
     "https://huggingface.co/bartowski/gemma-2-2b-it-GGUF/resolve/main/"
     "gemma-2-2b-it-Q4_K_M.gguf",
     "",
     1ull * GiB + 500ull * MiB, // ~1.5GB
     "CPU", false, false, "CPU Fast",
     8192, recommended_response_tokens(8192), recommended_summary_trigger(8192),
     {}},
    {"llama-3.2-1b", "Llama 3.2 1B", "Meta's ultra-lightweight 1B model",
     "llama-3.2-1b-instruct-q4_k_m.gguf",
     "https://huggingface.co/bartowski/Llama-3.2-1B-Instruct-GGUF/resolve/main/"
     "Llama-3.2-1B-Instruct-Q4_K_M.gguf",
     "",
     1ull * GiB, // ~1GB
     "CPU", false, false, "CPU Fast",
     8192, recommended_response_tokens(8192), recommended_summary_trigger(8192),
     {}}};

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
  auto *callback_data =
      static_cast<std::pair<std::function<void(const ModelDownloadProgress &)>,
                            std::string> *>(clientp);
  if (callback_data && callback_data->first) {
    ModelDownloadProgress progress;
    progress.model_id = callback_data->second;
    progress.bytes_downloaded = static_cast<std::size_t>(dlnow);
    progress.total_bytes = static_cast<std::size_t>(dltotal);
    progress.is_complete = (dltotal > 0 && dlnow >= dltotal);
    progress.progress_percentage =
        (dltotal > 0) ? (static_cast<double>(dlnow) / dltotal) * 100.0 : 0.0;
    progress.error_message.clear();
    callback_data->first(progress);
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
    std::function<void(const ModelDownloadProgress &)> progress_callback,
    std::string *error_message) {
  auto model_opt = get_model_by_id(model_id);
  if (!model_opt)
    return false;

  const auto &model = *model_opt;
  std::filesystem::path destination = models_directory_ / model.filename;
  std::filesystem::path temp_destination = destination;
  temp_destination += ".tmp";

  std::error_code ec;
  std::filesystem::remove(temp_destination, ec);

  bool success = download_file(model.download_url, temp_destination,
                               progress_callback, model_id, error_message);

  if (success) {
    std::filesystem::rename(temp_destination, destination, ec);
    if (ec) {
      if (error_message)
        *error_message = "Failed to move downloaded file: " + ec.message();
      std::filesystem::remove(temp_destination, ec);
      return false;
    }

    auto updateModel = [&](ModelInfo &downloaded_model) {
      downloaded_model.is_downloaded = true;
      downloaded_model.local_path = destination;
      if (downloaded_model.filename.empty())
        downloaded_model.filename = model.filename;
    };

    bool found = false;
    for (auto &downloaded_model : downloaded_models_) {
      if (downloaded_model.id == model_id) {
        updateModel(downloaded_model);
        found = true;
        break;
      }
    }

    if (!found) {
      ModelInfo downloaded_model = model;
      updateModel(downloaded_model);
      downloaded_model.is_active = false;
      downloaded_models_.push_back(downloaded_model);
    }

    save_configuration();
  } else {
    std::filesystem::remove(temp_destination, ec);
  }

  return success;
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
      for (auto &model : downloaded_models_)
        model.is_active = (model.id == active_model_id_);
    }
  } catch (const std::exception &) {
    // Ignore JSON parsing errors
  }
}

bool ModelManager::download_file(
    const std::string &url, const std::filesystem::path &destination,
    std::function<void(const ModelDownloadProgress &)> progress_callback,
    const std::string &model_id, std::string *error_message) {
  CURL *curl = curl_easy_init();
  if (!curl) {
    if (error_message)
      *error_message = "Failed to initialize CURL";
    return false;
  }

  std::ofstream file(destination, std::ios::binary);
  if (!file.is_open()) {
    curl_easy_cleanup(curl);
    if (error_message)
      *error_message =
          "Failed to open destination file: " + destination.string();
    return false;
  }

  auto callback_data = std::make_pair(std::move(progress_callback), model_id);

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &file);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
  curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, ProgressCallback);
  curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &callback_data);

  // Add timeout and connection settings
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 0L); // No timeout
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT,
                   30L); // 30 second connection timeout
  curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1000L); // 1KB/s minimum
  curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME,
                   60L); // Abort if below speed for 60s
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "ck-utilities/1.0");
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);

  CURLcode res = curl_easy_perform(curl);
  file.close();

  long response_code = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);

  bool success = (res == CURLE_OK && response_code == 200);

  if (!success && error_message) {
    if (res != CURLE_OK) {
      const char *curl_error = curl_easy_strerror(res);
      *error_message =
          curl_error ? std::string(curl_error) : "unknown curl error";
    } else if (response_code != 200) {
      *error_message = "HTTP error: " + std::to_string(response_code);
    }
  }

  curl_easy_cleanup(curl);

  if (!success) {
    std::error_code ec;
    std::filesystem::remove(destination, ec);
  }

  return success;
}

std::string ModelManager::generate_model_id(const std::string &name) const {
  std::string id = name;
  std::transform(id.begin(), id.end(), id.begin(), ::tolower);
  std::replace(id.begin(), id.end(), ' ', '-');
  return id;
}

} // namespace ck::ai
