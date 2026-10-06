// UNSW Battlecode 2026 bot: one process per dragon.
#include "brain.hpp"

#include <cstdio>

int main() {
    // The judge's stdout looks like a terminal; a full buffer makes each
    // turn's reply one write, which is what the CPU meter charges for.
    std::setvbuf(stdout, nullptr, _IOFBF, 1 << 16);

    bot::World world;
    if (!world.readInit()) return 0;
    bot::Brain brain(world);

    while (world.readTurn()) {
        std::string reply = brain.decide();
        reply += "PROTOCOL 3\nENDTURN\n";
        std::fwrite(reply.data(), 1, reply.size(), stdout);
        std::fflush(stdout);
    }
    return 0;
}
