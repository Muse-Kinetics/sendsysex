//
//  SendSysEx.c
//
//  Written by EricB on 2023/3/31
//  Copyright © 2023 Keith McMillen Instruments. All rights reserved.

#include <iostream>
#include <stdio.h>
#include <stdlib.h>
#include "RtMidi.h"

#include <chrono>
#include <thread>


using namespace std;

enum
{
    APP_INIT,
    ENTER_BOOTLOADER,
    WAIT_BOOTLOADER,
    SEND_FIRMWARE,
    FINISH_NO_ERR,
    FINISH_ERROR_OTHER
};

bool appRunning = true;
bool verboseMode = false;

RtMidiOut *midiout = 0;

void invalidArguments()
{
    fprintf(stderr,
            "\nKMI Send SysEx Utility \n"
            "Copyright (c) 2023 Keith McMillen Instruments. All rights reserved. \n"
            "Written by Eric Bateman. \n"
            "\n"
            "Standard usage: SendSysEx -p [Destination MIDI Port] -f [Source SysEx file]\n"
            "Example: SendSysEx BopPad BopPad.syx \n"
            "\n"
            "Bootloader update method: SendSysEx -p [Destination MIDI Port] -b [Bootloader Port Name] -t [time to wait for bootloader port] adob-bc [enter bootloader SysEx file] -f [Source SysEx file]\n"
            "\n"
            "Use flag -v for verbose mode.\n"
            );
}

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
    long syxFileSize = 0;
    FILE *syxFile;
    char *syxFilePath = NULL;
    std::vector<unsigned char> sysexPayload;
    
    string thisPortName, targetPortName;
    string thisArg;
    
    bool enterBootloaderMode = false;
    unsigned char bootLoaderTimeout = 1;
    int appState = APP_INIT;

    int i, numOutPorts, thisOutPortNum;
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
            if (thisArg == "-p" && argc > i)
            {
                appPortName = (char *)argv[i++ + 1];
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
            else if (thisArg == "-v")
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
            if (blCommandFilePath == NULL)
            {
                cout << "ERROR: No enter-bootloader command specified\n";
                return 0;
            }
            
            blCommand = fopen(blCommandFilePath,"r");
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
            appState = SEND_FIRMWARE;
        }
        
        // syx file
        {
            syxFile = fopen(syxFilePath,"r");
        
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
        switch (appState)
        {
            case ENTER_BOOTLOADER:
                
                // get MIDI port number matching port name
                numOutPorts = midiout->getPortCount();
                
                if (numOutPorts < 1)
                {
                    cout << "Error: no MIDI output ports available\n";
                    appState = FINISH_ERROR_OTHER;
                    break;
                }
                
                thisOutPortNum = getPortNumber(targetPortName);
                if (thisOutPortNum == -1)
                {
                    cout << "Error: MIDI output port \"" << appPortName << "\" not found!\n";
                    appState = FINISH_ERROR_OTHER;
                    break;
                }
                
                // Attempt to open midi output
                try {
                    midiout->openPort(thisOutPortNum);
                }
                catch ( RtMidiError &error ) {
                    error.printMessage();
                    appState = FINISH_ERROR_OTHER;
                    break;
                }
                
                try {
                    midiout->sendMessage( &blCommandPayload );
                }
                catch ( RtMidiError &error ) {
                    error.printMessage();
                    appState = FINISH_ERROR_OTHER;
                    break;
                }
                
                cout << "Successfully sent \"" << blCommandFilePath << "\" to MIDI OUT port: " << appPortName << ", size: " << blFileSize << " bytes.\n";
                
                try {
                    midiout->closePort();
                }
                catch ( RtMidiError &error ) {
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
                
                if (numOutPorts < 1)
                {
                    cout << "Error: no MIDI output ports available\n";
                    appState = FINISH_ERROR_OTHER;
                }
                
                // select name based on wether we are entering bootloader mode or not
                if (enterBootloaderMode)
                {
                    targetPortName = bootloaderPortName;
                }
                else
                {
                    targetPortName = appPortName;
                }
                
                thisOutPortNum = getPortNumber(targetPortName);
                if (thisOutPortNum == -1)
                {
                    cout << "Error: MIDI output port \"" << targetPortName << "\" not found!\n";
                    appState = FINISH_ERROR_OTHER;
                }
                
                // Attempt to open midi output
                try {
                    midiout->openPort(thisOutPortNum);
                }
                catch ( RtMidiError &error ) {
                    error.printMessage();
                    appState = FINISH_ERROR_OTHER;
                }
                
                try {
                    midiout->sendMessage( &sysexPayload );
                }
                catch ( RtMidiError &error ) {
                    error.printMessage();
                    appState = FINISH_ERROR_OTHER;
                }
                
                cout << "Successfully sent \"" << syxFilePath << "\" to MIDI OUT port: " << targetPortName << ", size: " << syxFileSize << " bytes.\n";
                appState = FINISH_NO_ERR;
                
                break;
                
            case FINISH_NO_ERR:
            case FINISH_ERROR_OTHER:

                delete midiout;
                return 0; // close app
                break;
            default:
                break;
        
        }
    }
}
