#pragma once

#include "../common.hpp"

// "Patch networks" screen: choose which network's patches are installed, download them from GitHub
// (checking for a newer tag first, reusing the cache otherwise) and update the Nimbus app itself.
namespace NetworkSwitch
{
    bool isActive();
    void open(MainStruct* mainStruct);
    // Draws both screens while active. Returns nothing: it never exits the app by itself.
    void update(MainStruct* mainStruct, C3D_RenderTarget* topScreen, C3D_RenderTarget* bottomScreen, u32 kDown, touchPosition touch);
    // Called once after startup: shows a hint when nothing is installed yet or an update exists.
    void onStartup(MainStruct* mainStruct);
}
