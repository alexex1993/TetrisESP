cmake_minimum_required(VERSION 3.16.0)

# Set before project(): otherwise ESP-IDF derives the version with `git
# describe`, which fails outright in a repository that has no commits yet.
set(PROJECT_VER "1.0.0")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(tetris-esp32c6-touch-lcd147)
