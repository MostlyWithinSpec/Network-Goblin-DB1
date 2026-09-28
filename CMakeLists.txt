cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# lwIP's default ARP table (10 entries) is too small for LAN sweeps
idf_build_set_property(COMPILE_DEFINITIONS "ARP_TABLE_SIZE=32" APPEND)
project(goblin_buddy)
