#ifndef MFG_LOOKUP_H
#define MFG_LOOKUP_H

#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>

// Look up a human-readable manufacturer name from the MIDI SysEx MFG IDs CSV.
// Accepts the three-byte manufacturer ID as used in Universal SysEx ID replies:
//   - 1-byte IDs:  byte0 != 0,  byte1 and byte2 are 0
//   - 3-byte IDs:  byte0 == 0,  byte1 and byte2 are the extended ID bytes
//
// The CSV format has columns:
//   SysEx ID Number (MMA format), Company Name, # of bytes, Dec 1b, Dec 2b, Dec 3b
//
// Returns an empty string if the CSV cannot be read or the ID is not found.
inline std::string lookupManufacturerName(const std::string &csvPath,
                                          uint8_t byte0,
                                          uint8_t byte1,
                                          uint8_t byte2)
{
    std::ifstream file(csvPath.c_str());
    if (!file.good())
        return std::string();

    const int target1b = static_cast<int>(byte0);
    const int target2b = static_cast<int>(byte1);
    const int target3b = static_cast<int>(byte2);
    const bool isExtended = (byte0 == 0);

    std::string line;
    bool firstLine = true;
    while (std::getline(file, line))
    {
        if (firstLine) { firstLine = false; continue; } // skip header

        // Split on commas into up to 6 fields.
        std::string fields[6];
        int fieldIdx = 0;
        std::string::size_type pos = 0;
        while (fieldIdx < 6)
        {
            const std::string::size_type comma = line.find(',', pos);
            if (comma == std::string::npos)
            {
                fields[fieldIdx++] = line.substr(pos);
                break;
            }
            fields[fieldIdx++] = line.substr(pos, comma - pos);
            pos = comma + 1;
        }
        if (fieldIdx < 4)
            continue;

        // Trim whitespace from company name (field 1).
        std::string name = fields[1];
        while (!name.empty() && (name[0] == ' ' || name[0] == '\t')) name.erase(name.begin());
        while (!name.empty() && (name.back() == ' ' || name.back() == '\t' || name.back() == '\r')) name.erase(name.end() - 1);

        if (name.empty())
            continue;

        // Parse decimal columns (fields 3, 4, 5).
        const int col1b = fields[3].empty() ? -1 : std::atoi(fields[3].c_str());
        const int col2b = fields[4].empty() ? -1 : std::atoi(fields[4].c_str());
        const int col3b = fields[5].empty() ? -1 : std::atoi(fields[5].c_str());

        if (!isExtended)
        {
            // 1-byte ID: Dec 1b matches byte0, Dec 2b and 3b empty.
            if (col2b == -1 && col3b == -1 && col1b == target1b)
                return name;
        }
        else
        {
            // 3-byte ID: Dec 1b == 0, Dec 2b matches byte1, Dec 3b matches byte2.
            if (col1b == 0 && col2b == target2b && col3b == target3b)
                return name;
        }
    }

    return std::string();
}

#endif // MFG_LOOKUP_H
