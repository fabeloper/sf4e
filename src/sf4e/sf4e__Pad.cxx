#include <windows.h>
#include <detours/detours.h>

#include "../Dimps/Dimps__Pad.hxx"
#include "sf4e__Pad.hxx"

namespace rPad = Dimps::Pad;
using rSystem = rPad::System;

namespace fPad = sf4e::Pad;
using fSystem = fPad::System;

fSystem::Inputs fSystem::playbackData[PLAYBACK_MAX][2];
int fSystem::playbackFrame = -1;
bool fSystem::bSuppressGameInput = false;

void fPad::Install() {
	System::Install();
}

void fSystem::Install() {
    unsigned int (fSystem:: * _fGetButtons_MappedOn)(int) = &GetButtons_MappedOn;
    unsigned int (fSystem:: * _fGetButtons_RawOn)(int) = &GetButtons_RawOn;
    unsigned int (fSystem:: * _fGetButtons_RawRising)(int) = &GetButtons_RawRising;
    unsigned int (fSystem:: * _fGetButtons_RawFalling)(int) = &GetButtons_RawFalling;
    unsigned int (fSystem:: * _fGetButtons_RawRisingWithRepeat)(int) = &GetButtons_RawRisingWithRepeat;
    DetourAttach((PVOID*)&rSystem::publicMethods.GetButtons_MappedOn, *(PVOID*)&_fGetButtons_MappedOn);
    DetourAttach((PVOID*)&rSystem::publicMethods.GetButtons_RawOn, *(PVOID*)&_fGetButtons_RawOn);
    DetourAttach((PVOID*)&rSystem::publicMethods.GetButtons_RawRising, *(PVOID*)&_fGetButtons_RawRising);
    DetourAttach((PVOID*)&rSystem::publicMethods.GetButtons_RawFalling, *(PVOID*)&_fGetButtons_RawFalling);
    DetourAttach((PVOID*)&rSystem::publicMethods.GetButtons_RawRisingWithRepeat, *(PVOID*)&_fGetButtons_RawRisingWithRepeat);
}

unsigned int fSystem::GetButtons_RawRising(int pindex) {
    if (bSuppressGameInput) {
        return 0;
    }
    return (this->*rSystem::publicMethods.GetButtons_RawRising)(pindex);
}

unsigned int fSystem::GetButtons_RawFalling(int pindex) {
    if (bSuppressGameInput) {
        return 0;
    }
    return (this->*rSystem::publicMethods.GetButtons_RawFalling)(pindex);
}

unsigned int fSystem::GetButtons_RawRisingWithRepeat(int pindex) {
    if (bSuppressGameInput) {
        return 0;
    }
    return (this->*rSystem::publicMethods.GetButtons_RawRisingWithRepeat)(pindex);
}

unsigned int fSystem::GetButtons_MappedOn(int pindex) {
    if (playbackFrame > -1) {
        return playbackData[playbackFrame][pindex].mappedOn;
    }
    if (bSuppressGameInput) {
        return 0;
    }

    rSystem* _this = (rSystem*)this;
    return (this->*rSystem::publicMethods.GetButtons_MappedOn)(pindex);
}

unsigned int fSystem::GetButtons_RawOn(int pindex) {
    if (playbackFrame > -1) {
        return playbackData[playbackFrame][pindex].rawOn;
    }
    if (bSuppressGameInput) {
        return 0;
    }

    rSystem* _this = (rSystem*)this;
    return (this->*rSystem::publicMethods.GetButtons_RawOn)(pindex);
}
