#!/bin/bash

cd "$(dirname "$0")"
cp ../gputemps.c .

docker build -t gputemps-builder .
docker create --name temp-container gputemps-builder
docker cp temp-container:/app/gputemps ./gputemps
docker rm temp-container

chmod +x ./gputemps
echo $'\nBuild done. Run with: sudo ./gputemps\n'
