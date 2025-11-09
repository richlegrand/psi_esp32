/**
 * ESP32 Opus File Parser (Dummy Implementation)
 * Returns empty audio samples for video-only streaming
 */

#ifndef opusfileparser_hpp
#define opusfileparser_hpp

#include "fileparser.hpp"

class OPUSFileParser: public FileParser {
    static const uint32_t defaultSamplesPerSecond = 50;

public:
    OPUSFileParser(std::string directory, bool loop, uint32_t samplesPerSecond = OPUSFileParser::defaultSamplesPerSecond);
};

#endif /* opusfileparser_hpp */