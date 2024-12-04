#!/bin/bash

set -e

cd "$(dirname "$0")"

docker build -t gputemps-builder .
docker create --name temp-container gputemps-builder
docker cp temp-container:/app/gputemps ./gputemps
docker rm temp-container

chmod +x ./gputemps
echo $'\nBuild done. Run with: sudo ./gputemps\n'
