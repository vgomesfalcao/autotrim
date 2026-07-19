FROM debian:bookworm

# Toolchain + JUCE 8 Linux dependencies (VST3 build, no webkit/curl needed:
# the plugin sets JUCE_WEB_BROWSER=0 and JUCE_USE_CURL=0)
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    cmake \
    ninja-build \
    git \
    ca-certificates \
    pkg-config \
    libasound2-dev \
    libfreetype6-dev \
    libfontconfig1-dev \
    libx11-dev \
    libxcomposite-dev \
    libxcursor-dev \
    libxext-dev \
    libxinerama-dev \
    libxrandr-dev \
    libxrender-dev \
    libgl1-mesa-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /work
