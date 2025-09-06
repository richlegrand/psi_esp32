#!/bin/bash

# Script to copy libdatachannel sources for ESP32 port
set -e

LIBDATACHANNEL_ROOT="../libdatachannel"
PROJECT_ROOT="."

echo "Copying libdatachannel sources for ESP32 port..."

# Create directory structure
mkdir -p components/libdatachannel/src/impl
mkdir -p components/libdatachannel/include/rtc
mkdir -p components/libjuice/src
mkdir -p components/libjuice/include
mkdir -p components/usrsctp/src/netinet
mkdir -p components/usrsctp/src/netinet6

# Copy libdatachannel headers
echo "Copying libdatachannel headers..."
cp -r $LIBDATACHANNEL_ROOT/include/rtc/* components/libdatachannel/include/rtc/

# Copy libdatachannel implementation files
echo "Copying libdatachannel implementation..."
# Core implementation
cp $LIBDATACHANNEL_ROOT/src/impl/*.cpp components/libdatachannel/src/impl/ 2>/dev/null || true
cp $LIBDATACHANNEL_ROOT/src/impl/*.hpp components/libdatachannel/src/impl/ 2>/dev/null || true

# Public API implementation
cp $LIBDATACHANNEL_ROOT/src/*.cpp components/libdatachannel/src/

# Copy only needed headers from src/
cp $LIBDATACHANNEL_ROOT/src/*.hpp components/libdatachannel/src/ 2>/dev/null || true

# Copy libjuice
echo "Copying libjuice..."
cp -r $LIBDATACHANNEL_ROOT/deps/libjuice/include/* components/libjuice/include/
cp $LIBDATACHANNEL_ROOT/deps/libjuice/src/*.c components/libjuice/src/
cp $LIBDATACHANNEL_ROOT/deps/libjuice/src/*.h components/libjuice/src/

# Copy usrsctp
echo "Copying usrsctp..."
cp $LIBDATACHANNEL_ROOT/deps/usrsctp/usrsctplib/*.c components/usrsctp/src/ 2>/dev/null || true
cp $LIBDATACHANNEL_ROOT/deps/usrsctp/usrsctplib/*.h components/usrsctp/src/ 2>/dev/null || true
cp $LIBDATACHANNEL_ROOT/deps/usrsctp/usrsctplib/netinet/*.c components/usrsctp/src/netinet/
cp $LIBDATACHANNEL_ROOT/deps/usrsctp/usrsctplib/netinet/*.h components/usrsctp/src/netinet/
cp $LIBDATACHANNEL_ROOT/deps/usrsctp/usrsctplib/netinet6/*.c components/usrsctp/src/netinet6/
cp $LIBDATACHANNEL_ROOT/deps/usrsctp/usrsctplib/netinet6/*.h components/usrsctp/src/netinet6/ 2>/dev/null || true

echo "Source files copied successfully!"

# Make script executable for next time
chmod +x copy_sources.sh