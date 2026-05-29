// Backward-compatible entry point for the old covered_match executable.
#include "commands.hpp"

int main(int argc, char* argv[]) {
    return cmd_match(argc, argv);
}