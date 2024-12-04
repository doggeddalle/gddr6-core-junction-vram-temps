FROM ubuntu:20.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y \
    gcc curl libpci-dev nvidia-cuda-toolkit \
    --no-install-recommends --no-install-suggests \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

COPY gputemps.c .

RUN gcc gputemps.c -o gputemps -O3 -lnvidia-ml -lpci -I/usr/local/cuda/targets/x86_64-linux/include
