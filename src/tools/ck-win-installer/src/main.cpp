#include <filesystem>
#include <cctype>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

namespace fs = std::filesystem;

namespace {
struct Options {
  fs::path payload;
  fs::path target;
  bool quiet = false;
  bool force = false;
  bool dry_run = false;
};

constexpr std::string_view kToolName{"cku-win-installer"};

std::string version_string() {
#ifdef CK_WIN_INSTALLER_VERSION
  return std::string{CK_WIN_INSTALLER_VERSION};
#else
  return "unknown";
#endif
}

void print_usage() {
  std::cout << "" << kToolName
            << " " << version_string() << "\n"
            << "Usage: " << kToolName
            << " [--payload <path>] [--target <path>] [--quiet] [--force]"
               " [--dry-run]\n"
            << "\n"
            << "Copies the packaged ck-utilities payload into a Windows\n"
               "installation directory. By default the installer looks for\n"
               "a 'payload' directory alongside the executable and installs\n"
               "to '%ProgramFiles%/ck-utilities'.\n";
}

fs::path exe_directory(const char* argv0) {
  std::error_code ec;
  fs::path exe_path = fs::absolute(fs::path(argv0), ec);
  if (ec) {
    exe_path = fs::path(argv0);
  }
  exe_path = fs::weakly_canonical(exe_path, ec);
  if (ec) {
    exe_path = fs::absolute(exe_path);
  }
  return exe_path.parent_path();
}

fs::path default_payload_path(const char* argv0) {
  fs::path base = exe_directory(argv0);
  if (base.empty()) {
    return fs::current_path() / "payload";
  }
  return base / "payload";
}

fs::path default_target_path() {
#ifdef _WIN32
  if (const char* program_files = std::getenv("ProgramFiles")) {
    std::error_code ec;
    fs::path path(program_files);
    path /= "ck-utilities";
    return fs::weakly_canonical(path, ec);
  }
#endif
  return fs::current_path() / "ck-utilities";
}

std::optional<Options> parse_arguments(int argc, char* argv[]) {
  Options opts;
  opts.payload = default_payload_path(argv[0]);
  opts.target = default_target_path();

  for (int i = 1; i < argc; ++i) {
    std::string_view arg(argv[i]);
    if (arg == "--help" || arg == "-h" || arg == "-?" ) {
      print_usage();
      return std::nullopt;
    }
    if (arg == "--version") {
      std::cout << version_string() << "\n";
      return std::nullopt;
    }
    if (arg == "--quiet" || arg == "-q") {
      opts.quiet = true;
      continue;
    }
    if (arg == "--force" || arg == "-f") {
      opts.force = true;
      continue;
    }
    if (arg == "--dry-run") {
      opts.dry_run = true;
      continue;
    }
    if (arg == "--payload") {
      if (i + 1 >= argc) {
        std::cerr << "Missing value for --payload" << std::endl;
        return std::nullopt;
      }
      opts.payload = fs::path(argv[++i]);
      continue;
    }
    if (arg == "--target" || arg == "-t") {
      if (i + 1 >= argc) {
        std::cerr << "Missing value for --target" << std::endl;
        return std::nullopt;
      }
      opts.target = fs::path(argv[++i]);
      continue;
    }
    std::cerr << "Unknown argument: " << arg << "\n";
    return std::nullopt;
  }

  std::error_code ec;
  opts.payload = fs::weakly_canonical(opts.payload, ec);
  if (ec) {
    opts.payload = fs::absolute(opts.payload);
  }
  opts.target = fs::weakly_canonical(opts.target, ec);
  if (ec) {
    opts.target = fs::absolute(opts.target);
  }

  return opts;
}

bool ensure_payload_exists(const Options& opts) {
  std::error_code ec;
  if (!fs::exists(opts.payload, ec) || !fs::is_directory(opts.payload, ec)) {
    std::cerr << "Payload directory not found: " << opts.payload << "\n";
    return false;
  }
  return true;
}

bool confirm_install(const Options& opts) {
  if (opts.quiet) {
    return true;
  }
  std::cout << "Install ck-utilities payload from\n  " << opts.payload
            << "\ninto\n  " << opts.target << "\n";
  if (!opts.force && fs::exists(opts.target)) {
    std::cout << "\nTarget exists and will be updated. Use --force to"
                 " remove it first.\n";
  }
  std::cout << "Proceed? [Y/n] " << std::flush;
  std::string response;
  if (!std::getline(std::cin, response)) {
    return false;
  }
  if (response.empty()) {
    return true;
  }
  unsigned char ch = static_cast<unsigned char>(response.front());
  char c = static_cast<char>(std::tolower(ch));
  return c == 'y';
}

bool clear_target(const Options& opts) {
  std::error_code ec;
  if (!fs::exists(opts.target, ec)) {
    return true;
  }
  if (!opts.force) {
    return true;
  }
  fs::remove_all(opts.target, ec);
  if (ec) {
    std::cerr << "Failed to remove existing target '" << opts.target
              << "': " << ec.message() << "\n";
    return false;
  }
  return true;
}

bool copy_payload(const Options& opts) {
  if (opts.dry_run) {
    std::cout << "[dry-run] Would copy payload from '" << opts.payload
              << "' to '" << opts.target << "'\n";
    return true;
  }

  std::error_code ec;
  fs::create_directories(opts.target, ec);
  if (ec) {
    std::cerr << "Failed to create target directory '" << opts.target
              << "': " << ec.message() << "\n";
    return false;
  }

  for (const auto& entry : fs::recursive_directory_iterator(opts.payload, fs::directory_options::follow_directory_symlink)) {
    const fs::path relative = fs::relative(entry.path(), opts.payload, ec);
    if (ec) {
      std::cerr << "Failed to compute relative path for '" << entry.path()
                << "': " << ec.message() << "\n";
      return false;
    }
    const fs::path destination = opts.target / relative;
    if (entry.is_directory()) {
      fs::create_directories(destination, ec);
      if (ec) {
        std::cerr << "Failed to create directory '" << destination
                  << "': " << ec.message() << "\n";
        return false;
      }
    } else if (entry.is_regular_file()) {
      fs::create_directories(destination.parent_path(), ec);
      ec.clear();
      fs::copy_file(entry.path(), destination,
                    opts.force ? fs::copy_options::overwrite_existing
                               : fs::copy_options::update_existing,
                    ec);
      if (ec) {
        std::cerr << "Failed to copy '" << entry.path() << "' to '"
                  << destination << "': " << ec.message() << "\n";
        return false;
      }
    } else if (entry.is_symlink()) {
      fs::create_directories(destination.parent_path(), ec);
      ec.clear();
      fs::copy(entry.path(), destination,
               fs::copy_options::overwrite_existing |
                   fs::copy_options::copy_symlinks,
               ec);
      if (ec) {
        std::cerr << "Failed to copy symlink '" << entry.path() << "' to '"
                  << destination << "': " << ec.message() << "\n";
        return false;
      }
    }
  }

  std::cout << "Installed payload into '" << opts.target << "'\n";
  return true;
}

}  // namespace

int main(int argc, char* argv[]) {
  auto parsed = parse_arguments(argc, argv);
  if (!parsed.has_value()) {
    // Either help/version displayed or error encountered.
    return parsed ? 0 : 1;
  }

  const Options& opts = *parsed;
  if (!ensure_payload_exists(opts)) {
    return 2;
  }

  if (!confirm_install(opts)) {
    std::cout << "Installation cancelled." << std::endl;
    return 0;
  }

  if (!clear_target(opts)) {
    return 3;
  }

  if (!copy_payload(opts)) {
    return 4;
  }

  if (!opts.quiet) {
    std::cout << "You can run ck-utilities from '" << opts.target
              << "\\bin'." << std::endl;
  }

  return 0;
}
