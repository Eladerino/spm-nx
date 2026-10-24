// sys/un.h for Horizon (Switch build only). libnx's BSD sockets define AF_UNIX but ship no
// sockaddr_un, because Horizon's socket service has no local sockets. The one user is Discord Rich
// Presence (runtime/src/discord_presence.cpp), which looks for a Discord client's IPC socket; on the
// Switch its connect() fails like it does on a PC with Discord closed, and presence stays off.
#pragma once

#include <sys/socket.h>

struct sockaddr_un {
    sa_family_t sun_family;
    char sun_path[108];
};
