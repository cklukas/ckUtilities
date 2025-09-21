# syntax=docker/dockerfile:1
FROM mcr.microsoft.com/devcontainers/cpp:1-ubuntu-22.04

ARG DEBIAN_FRONTEND=noninteractive

USER root

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
      ca-certificates \
      curl \
      gnupg \
      lsb-release \
      software-properties-common \
    && rm -rf /var/lib/apt/lists/*

RUN curl -fsSL https://apt.kitware.com/keys/kitware-archive-latest.asc \
      | gpg --dearmor -o /usr/share/keyrings/kitware-archive-keyring.gpg \
    && echo "deb [signed-by=/usr/share/keyrings/kitware-archive-keyring.gpg] https://apt.kitware.com/ubuntu/ $(lsb_release -cs) main" \
      > /etc/apt/sources.list.d/kitware.list

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
      cmake \
      ninja-build \
      clang-15 \
      clang-tidy-15 \
      lld-15 \
      llvm-15-dev \
      git \
      pandoc \
      texinfo \
      dpkg-dev \
      rpm \
      ccache \
      lcov \
    && rm -rf /var/lib/apt/lists/*

RUN update-alternatives --install /usr/bin/clang clang /usr/bin/clang-15 150 \
    && update-alternatives --install /usr/bin/clang++ clang++ /usr/bin/clang++-15 150 \
    && update-alternatives --install /usr/bin/clang-tidy clang-tidy /usr/bin/clang-tidy-15 150

RUN CMAKE_VERSION=$(cmake --version | head -n1 | awk '{print $3}') \
    && if dpkg --compare-versions "${CMAKE_VERSION}" lt 3.27; then \
         echo "CMake 3.27+ is required, but ${CMAKE_VERSION} is installed." >&2; \
         exit 1; \
       fi

USER vscode
