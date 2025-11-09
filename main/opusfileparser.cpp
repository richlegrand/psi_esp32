/**
 * ESP32 Opus File Parser (Dummy Implementation)
 * Returns empty audio samples for video-only streaming
 */

#include "opusfileparser.hpp"

using namespace std;

// For ESP32, we create a dummy Opus parser that doesn't actually read files
// It just returns empty samples with proper timing
OPUSFileParser::OPUSFileParser(string directory, bool loop, uint32_t samplesPerSecond):
    FileParser(directory, ".opus", samplesPerSecond, loop) {
    // Override the loadNextSample behavior to return empty samples
}