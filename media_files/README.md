# Media Files for ESP32 Streamer

This directory contains H.264 video samples that will be packaged into the LittleFS partition during build.

## Structure:
- `h264/` - H.264 NAL units, one per file (0.h264, 1.h264, 2.h264, ...)

## Build Process:
These files are automatically packaged into the LittleFS partition (8MB) at build time.

## Usage:
Replace the dummy H.264 files with actual encoded video frames.
Each file should contain one H.264 NAL unit.
Files are played sequentially in numerical order.