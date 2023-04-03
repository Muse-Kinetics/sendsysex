//
//  SendSysEx.c
//
//  Written by EricB on 2023/3/31
//  Copyright © 2023 Keith McMillen Instruments. All rights reserved.

#include <iostream>
#include <stdio.h>
#include <stdlib.h>
#include "inc/RtMidi.h"

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

void invalidArguments()
{
    fprintf(stderr,
            "KMI Send SysEx Utility \n"
            "Copyright © 2023 Keith McMillen Instruments. All rights reserved. \n"
            "Written by Eric Bateman. \n"
            "\n"
            "Standard usage: SendSysEx -p [Destination MIDI Port] -f [Source SysEx file]\n"
            "Example: SendSysEx BopPad BopPad.syx \n"
            "\n"
            "Bootloader update method: SendSysEx -p [Destination MIDI Port] -b [Bootloader Port Name] -bc [enter bootloader SysEx file] -f [Source SysEx file]\n"
            );
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
    char *syxFilePath;
    std::vector<unsigned char> sysexPayload;
    
    string thisPortName;
    string thisArg;
    
    bool enterBootloaderMode = false;
    unsigned char bootLoaderTimeout = 1;
    int appState = APP_INIT;
    bool appRunning = true;
    
    RtMidiOut *midiout = 0;

    int i, numOutPorts, thisOutPortNum;
    unsigned char syxByte;
    
    // Parse arguments
    if (argc > 1)
    {
        // load arguments
        for (int i = 1; i < argc; ++i)
        {
            cout << "argument " << i << " : " << (char *)argv[i];
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
            else
            {
                invalidArguments();
                return 0;
            }
            printf(" argument %d: %s\n", i, argv[i]);
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
    
    
    // RtMidiOut constructor
    try {
        midiout = new RtMidiOut();
    }
    catch ( RtMidiError &error ) {
        error.printMessage();
        appState = FINISH_ERROR_OTHER;
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
                
                thisOutPortNum = -1;
                for (i = 0; i < numOutPorts; i++)
                {
                    if (midiout->getPortName(i) == appPortName) // enter bootloader message goes to the app port
                    {
                        thisOutPortNum = i;
                    }
                }
                
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
                // get MIDI port number matching port name
                numOutPorts = midiout->getPortCount();
                
                thisOutPortNum = -1;
                for (i = 0; i < numOutPorts; i++)
                {
                    if (midiout->getPortName(i) == bootloaderPortName)
                    {
                        thisOutPortNum = i;
                    }
                }
                
                if (thisOutPortNum > -1)
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
                    thisPortName = bootloaderPortName;
                }
                else
                {
                    thisPortName = appPortName;
                }
                
                thisOutPortNum = -1;
                for (i = 0; i < numOutPorts; i++)
                {
                    if (midiout->getPortName(i) == thisPortName)
                    {
                        thisOutPortNum = i;
                    }
                }
                
                if (thisOutPortNum == -1)
                {
                    cout << "Error: MIDI output port \"" << thisPortName << "\" not found!\n";
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
                
                cout << "Successfully sent \"" << syxFilePath << "\" to MIDI OUT port: " << thisPortName << ", size: " << syxFileSize << " bytes.\n";
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
