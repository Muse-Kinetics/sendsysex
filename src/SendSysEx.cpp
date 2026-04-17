//
//  SendSysEx.cpp
//
//  Written by Eric Bateman on 2023/3/31
//  Copyright (c) 2023 Eric Bateman. All rights reserved.
//  eric@musekinetics.com
//
//  SPDX-License-Identifier: MIT

#include <iostream>
#include <stdio.h>
#include <stdlib.h>
#include "RtMidi.h"

#include <chrono>
#include <thread>



#define MAX_MIDI_SYSEX_SIZE 250000 // reject payloads larger than this, also prevents buffer overflow

#define MIDI_SX_START 0xF0
#define MIDI_SX_STOP 0xF7

using namespace std;

enum
{
    APP_INIT,
    ENTER_BOOTLOADER,
    WAIT_BOOTLOADER,
    SEND_FIRMWARE,
    FW_SENT_WAIT,
    FINISH_NO_ERR,
    FINISH_ERROR_OTHER
};

bool appRunning = true;
bool verboseMode = false;
int appState = APP_INIT;

RtMidiOut *midiout = 0;

std::vector<unsigned char> packet; // the midi TX buffer, put outgoing midi messages here to transmit them

unsigned int sysExTxChunkSize = 48; // this is equivalent to 16 USB MIDI messages, or a full USB MIDI packet 
unsigned int sysExTxChunkDelay = 2; // limit sysex to one packet per 1ms

auto syxExTxChunkTimer = std::chrono::high_resolution_clock::now();
auto timeNow = std::chrono::high_resolution_clock::now();

void slotEmptyMIDIBuffer();

// get MIDI port number matching port name
int getPortNumber(string findPortName)
{ 
    string thisPortName;
    int numOutPorts = midiout->getPortCount();

    int thisOutPortNum = -1;
    for (int i = 0; i < numOutPorts; i++)
    {
        thisPortName = midiout->getPortName(i);
        
        if (verboseMode)
        {
            cout << "MIDI Output Port detected: " << thisPortName << "\n";
        }
        
        if (thisPortName == findPortName)
        {
            thisOutPortNum = i;
        }
    }
    return thisOutPortNum;
}

void invalidArguments()
{
    fprintf(stderr,
            "\nSend SysEx Utility \n"
            "Copyright (c) 2023 Eric Bateman. All rights reserved. \n"
            "eric@musekinetics.com \n"
            "Written by Eric Bateman. \n"
            "\n"
            "Standard usage: SendSysEx -p [Destination MIDI Port Number] -f [Source SysEx file]\n"
            "Example: SendSysEx BopPad BopPad.syx \n"
            "\n"
            "Bootloader update method: SendSysEx -p [Destination MIDI Port Number] -b [Bootloader Port Name] -t [time to wait for bootloader port]  -bc [enter bootloader SysEx file] -f [Source SysEx file]\n"
            "\n"
            "Use flag -l to list ports.\n"
            );

    verboseMode = true;
    int listPorts = getPortNumber(""); // list ports
}




int main(int argc, const char * argv[])
{
    using namespace std::this_thread; // sleep_for, sleep_until
    using namespace std::chrono; // nanoseconds, system_clock, seconds
    
    char *bootloaderPortName;
    long blFileSize = 0;
    FILE *blCommand;
    char *blCommandFilePath = NULL;
    std::vector<unsigned char> blCommandPayload;
    
    char *appPortName;
    char *appPortNum;
    long syxFileSize = 0;
    FILE *syxFile;
    char *syxFilePath = NULL;
    std::vector<unsigned char> sysexPayload;
    
    string thisPortName, targetPortName;
    string thisArg;
    
    bool enterBootloaderMode = false;
    bool useOnePortNumber = false;
    unsigned char bootLoaderTimeout = 1;

    int i, numOutPorts;
    int thisOutPortNum = -1;
    unsigned char syxByte;
    
    // RtMidiOut constructor
    try {
        midiout = new RtMidiOut();
    }
    catch ( RtMidiError &error ) {
        error.printMessage();
        appState = FINISH_ERROR_OTHER;
    }

    // Parse arguments
    if (argc > 1)
    {
        // load arguments
        for (int i = 1; i < argc; i++)
        {
            //cout << "argument " << i << " : " << (char *)argv[i];
            thisArg = argv[i];
            if (thisArg == "-n" && i + 1 < argc)
            {
                appPortName = (char *)argv[++i];
                std::cout << "Port Name: " << appPortName << "\n";
            }
            else if (thisArg == "-p" && i + 1 < argc)
            {
                useOnePortNumber = true;
                thisOutPortNum = std::atoi(argv[++i]); // Increment i and convert to int
                std::cout << "Using port: " << thisOutPortNum << "\n";
            }
            else if (thisArg == "-f" && argc > i)
            {
                syxFilePath = (char *)argv[i++ + 1];
            }
            else if (thisArg == "-b" && argc > i)
            {
                bootloaderPortName = (char *)argv[i++ + 1];
                enterBootloaderMode = true;
            }
            else if (thisArg == "-bc" && argc > i)
            {
                blCommandFilePath = (char *)argv[i++ + 1];
            }
            else if (thisArg == "-t" && argc > i)
            {
                bootLoaderTimeout = strtol(argv[i++ + 1], nullptr, 0);
            }
            else if (thisArg == "-l")
            {
                verboseMode = true;
                thisOutPortNum = getPortNumber(""); // list ports
            }
            else
            {
                invalidArguments();
                return 0;
            }
            //printf(" argument %d: %s\n", i, argv[i]);
        }
        
        if (syxFilePath == NULL)
        {
            cout << "ERROR: No SysEx File Specified!";
            return 0;
        }
        
        // bootloader
        if (enterBootloaderMode)
        {
            cout << "Bootloader + firmware mode\n";
            if (blCommandFilePath == NULL)
            {
                cout << "ERROR: No enter-bootloader command specified\n";
                return 0;
            }
            
            blCommand = fopen(blCommandFilePath,"rb");
            if (blCommand == nullptr)
            {
                cout << "ERROR: file \"" << blCommandFilePath << "\" not found!\n";
                return 0;
            }
            fseek(blCommand, 0L, SEEK_END);   // goto end of the file
            blFileSize = ftell(blCommand);   // get size
            rewind(blCommand);                // back to beginning of file
            
            if (blFileSize < 1)
            {
                cout << "ERROR: file \"" << blCommandFilePath << "\" is zero bytes!\n";
                return 0;
            }
            
            // load file into vector
            cout << "Loading \"" << blCommandFilePath << "\"\n";
            for (i = 0; i < blFileSize; i++)
            {
                syxByte = (unsigned char)fgetc(blCommand);
                blCommandPayload.push_back(syxByte);
            }
            
            appState = ENTER_BOOTLOADER;
        }
        else
        {
             cout << "Firmware only mode\n";
            appState = SEND_FIRMWARE;
        }
        
        // syx file
        {
            syxFile = fopen(syxFilePath,"rb");
        
            if (syxFile == nullptr)
            {
                cout << "ERROR: file \"" << syxFilePath << "\" not found!\n";
                return 0;
            }
            
            fseek(syxFile, 0L, SEEK_END);   // goto end of the file
            syxFileSize = ftell(syxFile);   // get size
            rewind(syxFile);                // back to beginning of file
            
            if (syxFileSize < 1)
            {
                cout << "ERROR: file \"" << syxFilePath << "\" is zero bytes!\n";
                return 0;
            }
            
            // load file into vector
            cout << "Loading \"" << syxFilePath << "\"\n";
            for (i = 0; i < syxFileSize; i++)
            {
                syxByte = (unsigned char)fgetc(syxFile);
                sysexPayload.push_back(syxByte);
            }
        }
        
    }
    else
    {
        invalidArguments();
        return 0;
    }
    
    // MAIN LOOP
    while (appRunning)
    {
        timeNow = std::chrono::high_resolution_clock::now();
        unsigned int duration = std::chrono::duration_cast<std::chrono::milliseconds>(timeNow - syxExTxChunkTimer).count();

        if (duration > sysExTxChunkDelay)
        {
            syxExTxChunkTimer = std::chrono::high_resolution_clock::now();
            slotEmptyMIDIBuffer();
        }

        //cout << "Execute App State: " << appState << "\n";
        switch (appState)
        {
            case ENTER_BOOTLOADER:
                cout << "Enter bootloader\n";
                // get MIDI port number matching port name
                numOutPorts = midiout->getPortCount();
                
                if (numOutPorts < 1)
                {
                    cout << "Error: no MIDI output ports available\n";
                    appState = FINISH_ERROR_OTHER;
                    break;
                }
                
                thisOutPortNum = useOnePortNumber ? getPortNumber(targetPortName) : thisOutPortNum;
                if (thisOutPortNum == -1)
                {
                    cout << "Error: MIDI output port \"" << appPortName << "\" not found!\n";
                    appState = FINISH_ERROR_OTHER;
                    break;
                }
                
                // Attempt to open midi output
                cout << "Opening port: " << thisOutPortNum << "\n";
                try {
                    midiout->openPort(thisOutPortNum);
                }
                catch ( RtMidiError &error ) {
                    cout << "**ERROR**\n";
                    error.printMessage();
                    appState = FINISH_ERROR_OTHER;
                    break;
                }
                
                cout << "Sending Bootloader image/command to port: " << thisOutPortNum << "\n";
                try {
                    midiout->sendMessage( &blCommandPayload );
                }
                catch ( RtMidiError &error ) {
                    cout << "**ERROR**\n";
                    error.printMessage();
                    appState = FINISH_ERROR_OTHER;
                    break;
                }
                
                cout << "Successfully sent \"" << blCommandFilePath << "\" to MIDI OUT port: " << appPortName << ", size: " << blFileSize << " bytes.\n";
                
                try {
                    midiout->closePort();
                }
                catch ( RtMidiError &error ) {
                    cout << "**ERROR**\n";
                    error.printMessage();
                    appState = FINISH_ERROR_OTHER;
                    break;
                }
                
                appState = WAIT_BOOTLOADER;
                break;
                
            case WAIT_BOOTLOADER:
                cout << "Waiting " << to_string(bootLoaderTimeout) << " second(s) for Bootloader port...\n";
                sleep_for(seconds(bootLoaderTimeout)); // putting this here to give midi system time before running the first check
                
                if (getPortNumber(bootloaderPortName) > -1)
                {
                    cout << "Found bootloader port: " << bootloaderPortName << "\n";
                    appState = SEND_FIRMWARE;
                    break;
                }
                else
                {
                    // seep_for could alternately go here
                    break;
                }
                
                break; // redundant but safety
            case SEND_FIRMWARE:
                
                // get MIDI port number matching port name
                numOutPorts = midiout->getPortCount();
                
                cout << "Send Firmware - numOutPorts: " << numOutPorts << "\n";

                if (numOutPorts < 1)
                {
                    cout << "Error: no MIDI output ports available\n";
                    appState = FINISH_ERROR_OTHER;
                    break;
                }
                
                // cout << "enterBootloaderMode: " << enterBootloaderMode << "\n";

                // // select name based on wether we are entering bootloader mode or not
                // if (enterBootloaderMode)
                // {
                //     cout << "1\n";
                //     targetPortName = bootloaderPortName;
                // }
                // else
                // {
                //     cout << "2\n";
                //     targetPortName = appPortName;
                // }

                cout << "3\n";
                
                cout << "thisOutPortNum: " << thisOutPortNum << "\n";

                thisOutPortNum = useOnePortNumber ? thisOutPortNum : getPortNumber(targetPortName);
                if (thisOutPortNum == -1)
                {
                    cout << "Error: MIDI output port \"" << targetPortName << "\" not found!\n";
                    appState = FINISH_ERROR_OTHER;
                    break;
                }
                
                cout << "Opening port: " << thisOutPortNum << "\n";
                // Attempt to open midi output
                try {
                    midiout->openPort(thisOutPortNum);
                }
                catch ( RtMidiError &error ) {
                    cout << "**ERROR**\n";
                    error.printMessage();
                    appState = FINISH_ERROR_OTHER;
                    break;
                }

                // try {
                //     midiout->sendMessage( &sysexPayload );
                // }
                // catch ( RtMidiError &error ) {
                //     cout << "**ERROR**\n";
                //     error.printMessage();
                //     appState = FINISH_ERROR_OTHER;
                //     break;
                // }
                // appState = FINISH_ERROR_OTHER;
                // break;
                
                cout << "Appending Firmware packet to payload - size: " << sysexPayload.size() << "\n";
                packet.insert(packet.end(), sysexPayload.begin(), sysexPayload.end());
                
                appState = FW_SENT_WAIT;
                break;

            case FW_SENT_WAIT:
                break;

            case FINISH_ERROR_OTHER: 
                cout << "ERROR - OTHER\n";
            case FINISH_NO_ERR:
                cout << "Exiting app.\n";
                delete midiout;
                return 0; // close app
                break;
            default:
                break;
        
        }
    }
}

// this function is called every 1ms
void slotEmptyMIDIBuffer()
{
    std::vector<unsigned char> message;
    static bool sendLastChunk = false;

    if (packet.size() == 0)
    {
        return;
    }

    if (packet.size() > MAX_MIDI_SYSEX_SIZE)
    {
        cout << "ERROR: SYSEX TX BUFFER OVERFLOW, DISCARDING";
        packet.clear();
        return;
    }

    // send sysex in chunks
    if (packet.size() > sysExTxChunkSize || sendLastChunk == true)
    {
        unsigned int sizeToSend = sendLastChunk ? (unsigned int)packet.size() : sysExTxChunkSize;

        cout << "Sending SysEx: " << sizeToSend << "/" << packet.size() << " bytes, current\n";
        // Create a sub-vector for the chunk to send
        std::vector<uint8_t> chunkToSend(packet.begin(), packet.begin() + sizeToSend);

        // Send the chunk
        try
        {
            midiout->sendMessage(&chunkToSend);
        }
        catch (RtMidiError &error)
        {
            cout << "MIDI SEND LARGE SYSEX ERR";
            packet.clear();
            appState = FINISH_ERROR_OTHER;
            return;
        }

        if (sendLastChunk) // if we just sent the last chunk, clear flag/buffer and exit
        {
            sendLastChunk = false;
            packet.clear();

            appState = FINISH_NO_ERR;
            return;
        }

        // Remove the sent chunk from the packet
        packet.erase(packet.begin(), packet.begin() + sizeToSend);


        if (packet.size() < sysExTxChunkSize) // if we have one chunk left, loop around and send it
        {
            sendLastChunk = true;
        }
        return;
    }
    else
    {
        for (size_t i = 0; i < packet.size(); ++i)
        {
            
            if (packet[i] == MIDI_SX_START) // small sysex, less than chunk size
            {
                cout << "Sending Small SysEx: " << packet.size() << "\n";
                std::vector<unsigned char> smallSysExPacket;

                bool stopSearch = false;

                while (!stopSearch)
                {
                    smallSysExPacket.push_back(packet[i++]);

                    if (packet[i] == MIDI_SX_STOP || i >= packet.size())
                    {
                        stopSearch = true;
                    }
                    else if (packet[i] > 127)
                    {
                        i--; // decrement index so the next loop looks at this byte
                        stopSearch = true;
                    }
                }
                smallSysExPacket.push_back(MIDI_SX_STOP); // always end sysex properly even if packet is incomplete

                try
                {
                    midiout->sendMessage( &smallSysExPacket );
                }
                catch (RtMidiError &error)
                {
                    cout << "MIDI SEND SMALL SYSEX ERR: " << error.getMessage();
                    packet.clear();
                    appState = FINISH_ERROR_OTHER;
                    return;
                }
                cout << "Sent Small SysEx";
                appState = FINISH_NO_ERR;
                return;
            }
            else // channel messages
            {   
                cout << "Sending Channel Message: " << i << "/" << packet.size() << "\n";
                // Check if the current byte is a status byte
                if (packet[i] >= 0x80)
                {
                    
                    // If there's already a message being constructed, send it
                    if (!message.empty())
                    {
                        try
                        {
                            midiout->sendMessage( &message );
                        }
                        catch (RtMidiError &error)
                        {
                            cout << "MIDI SEND PACKET ERR: " << error.getMessage();
                        }
                        message.clear(); // Clear the message vector for the next message
                    }
                }
            }

            // Add the current byte to the message
            message.push_back(packet[i]);

            // If it's the last byte but not a status byte, ensure the message is sent
            if (i == packet.size() - 1 && !message.empty())
            {
                cout << "Last Byte: " << packet.size() << "\n";
                try
                {
                    midiout->sendMessage( &message );
                }
                catch (RtMidiError &error)
                {
                   cout << "MIDI SEND PACKET ERR: " << error.getMessage();
                }
                cout << "Sent Channel Msg";
                appState = FINISH_NO_ERR;
                return;
            }
        }
    }

    // Clear the packet after processing all messages
    packet.clear();
}