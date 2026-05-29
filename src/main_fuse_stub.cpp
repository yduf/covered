// Backward-compatible entry point for the old cover_fuse executable.
#include "commands.hpp"

int main(int argc, char* argv[]) {
    return cmd_fuse(argc, argv);
}