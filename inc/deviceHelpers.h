#ifndef DEVICE_HELPERS_H
#define DEVICE_HELPERS_H

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "MIDI_device_metadata.hpp"

// mm:ss for progress-bar elapsed/eta display.
inline std::string formatDuration(double seconds)
{
    if (seconds < 0.0)
        seconds = 0.0;

    int totalSeconds = static_cast<int>(seconds + 0.5);
    int minutes = totalSeconds / 60;
    int secs = totalSeconds % 60;

    std::ostringstream buffer;
    buffer << std::setw(2) << std::setfill('0') << minutes
           << ":" << std::setw(2) << std::setfill('0') << secs;
    return buffer.str();
}

// \r-updating progress bar shared by every byte-streaming send path (raw -f,
// and --fw-update's chunked/sub-split sends via chunkedSysExTransfer.h) so
// they all look and behave the same. Prints a trailing newline once the
// transfer completes (bytesSent >= totalBytes) so subsequent output starts
// on a fresh line.
inline void printProgress(std::size_t bytesSent,
                          std::size_t totalBytes,
                          std::size_t currentPacketSize,
                          const std::chrono::steady_clock::time_point &startTime)
{
    const int barWidth = 30;
    double progress = totalBytes > 0 ? static_cast<double>(bytesSent) / static_cast<double>(totalBytes) : 1.0;
    if (progress > 1.0)
        progress = 1.0;

    int filled = static_cast<int>(progress * barWidth + 0.5);
    if (filled > barWidth)
        filled = barWidth;

    const std::size_t bytesRemaining = totalBytes > bytesSent ? totalBytes - bytesSent : 0;
    const double elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - startTime).count() / 1000.0;
    const double eta = (bytesSent > 0 && progress > 0.0) ? (elapsed / static_cast<double>(bytesSent)) * static_cast<double>(bytesRemaining) : 0.0;

    std::cout << '\r'
              << '[' << std::string(filled, '#') << std::string(barWidth - filled, '-') << "] "
              << "pkt " << std::setw(4) << currentPacketSize << " B  "
              << "sent " << std::setw(6) << bytesSent << '/' << std::setw(6) << totalBytes << " B  "
              << "left " << std::setw(6) << bytesRemaining << " B  "
              << "elapsed " << formatDuration(elapsed) << "  "
              << "eta " << formatDuration(eta)
              << std::flush;

    if (bytesRemaining == 0)
        std::cout << "\n";
}

inline std::string trimCopy(const std::string &text)
{
    std::string::size_type start = 0;
    while (start < text.size() && std::isspace(static_cast<unsigned char>(text[start])))
        ++start;

    std::string::size_type end = text.size();
    while (end > start && std::isspace(static_cast<unsigned char>(text[end - 1])))
        --end;

    return text.substr(start, end - start);
}

inline std::string collapseWhitespaceCopy(const std::string &text)
{
    std::ostringstream result;
    bool inWhitespace = false;

    for (std::string::const_iterator it = text.begin(); it != text.end(); ++it)
    {
        unsigned char c = static_cast<unsigned char>(*it);
        if (std::isspace(c))
        {
            if (!inWhitespace)
            {
                result << ' ';
                inWhitespace = true;
            }
            continue;
        }

        result << static_cast<char>(c);
        inWhitespace = false;
    }

    return trimCopy(result.str());
}

inline std::string toLowerCopy(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

inline bool parseUnsignedOption(const char *text, unsigned int &value)
{
    char *endPtr = 0;
    unsigned long parsed = std::strtoul(text, &endPtr, 0);

    if (text == endPtr || *endPtr != '\0')
        return false;

    value = static_cast<unsigned int>(parsed);
    return true;
}

inline std::string normalizeFamilyId(std::string value)
{
    std::string normalized;
    for (std::size_t i = 0; i < value.size(); ++i)
    {
        const unsigned char c = static_cast<unsigned char>(value[i]);
        if (std::isalnum(c))
            normalized.push_back(static_cast<char>(std::tolower(c)));
    }

    if (normalized == "kboardpro4")
        return "kbp4";

    if (normalized == "12step1" || normalized == "12step2" || normalized == "twelvestep")
        return "12step";

    return normalized;
}

inline bool parseVersionString(const std::string &text, version_t &version)
{
    version = {0, 0, 0, 0};

    std::stringstream ss(text);
    int parts[4] = {0, 0, 0, 0};
    int count = 0;

    while (count < 4)
    {
        if (!(ss >> parts[count]))
            break;

        if (parts[count] < 0 || parts[count] > 255)
            return false;

        ++count;
        if (ss.peek() == '.')
        {
            ss.get();
            continue;
        }
        break;
    }

    if (count < 3 || !ss.eof())
        return false;

    version.major = static_cast<uint8_t>(parts[0]);
    version.minor = static_cast<uint8_t>(parts[1]);
    version.patch = static_cast<uint8_t>(parts[2]);
    version.dev = static_cast<uint8_t>(count > 3 ? parts[3] : 0);
    return true;
}

inline std::string versionToString(const version_t &version)
{
    std::ostringstream buffer;
    buffer << static_cast<int>(version.major)
           << "." << static_cast<int>(version.minor)
           << "." << static_cast<int>(version.patch);

    if (version.dev != 0)
        buffer << "." << static_cast<int>(version.dev);

    return buffer.str();
}

inline bool readBinaryFile(const std::string &filePath, std::vector<unsigned char> &bytes, std::string *errorMessage = 0)
{
    bytes.clear();

    std::ifstream input(filePath.c_str(), std::ios::in | std::ios::binary);
    if (!input.good())
    {
        if (errorMessage != 0)
            *errorMessage = "Could not open file: " + filePath;
        return false;
    }

    input.seekg(0, std::ios::end);
    const std::streamoff size = input.tellg();
    input.seekg(0, std::ios::beg);

    if (size <= 0)
    {
        if (errorMessage != 0)
            *errorMessage = "File is empty: " + filePath;
        return false;
    }

    bytes.resize(static_cast<std::size_t>(size));
    input.read(reinterpret_cast<char *>(&bytes[0]), size);

    if (!input.good() && !input.eof())
    {
        if (errorMessage != 0)
            *errorMessage = "Failed while reading file: " + filePath;
        bytes.clear();
        return false;
    }

    return true;
}

inline std::string resolveDataPath(const std::string &relativePath)
{
    const std::string candidates[] = {
        relativePath,
        std::string("./") + relativePath,
        std::string("../") + relativePath,
        std::string("../../") + relativePath
    };
    const std::size_t count = sizeof(candidates) / sizeof(candidates[0]);
    for (std::size_t i = 0; i < count; ++i)
    {
        std::ifstream probe(candidates[i].c_str(), std::ios::in | std::ios::binary);
        if (probe.good())
            return candidates[i];
    }
    return std::string();
}

#endif
