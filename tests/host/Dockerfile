FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    afl++ \
    build-essential \
    ca-certificates \
    clang \
    clang-tidy \
    cmake \
    cppcheck \
    git \
    ninja-build \
    python3 \
    python3-pip \
    python3-venv \
    && rm -rf /var/lib/apt/lists/*

RUN apt-get update && apt-get install -y --no-install-recommends \
    libclang-rt-18-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /work

COPY . /work

RUN chmod +x tests/host/run.sh

CMD ["tests/host/run.sh", "all"]
