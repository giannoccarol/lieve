#pragma once

namespace Lieve
{
// Keeps the actions of ~/.local/share/applications/lieve.desktop in sync with
// the pinned and recent projects, so they show up when right-clicking Lieve
// in the Plasma task manager or in the application menu.
// Only touches the file installed by "make install" (marked X-Lieve-JumpList=true).
namespace JumpList
{
void update();
}
}
