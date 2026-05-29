// Backward-compatible entry point for the old cover_update executable.
#include "commands.hpp"

int main(int argc, char* argv[]) {
    return cmd_update(argc, argv);
}