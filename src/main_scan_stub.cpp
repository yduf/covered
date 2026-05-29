// Backward-compatible entry point for the old covered_scan_size executable.
#include "commands.hpp"

int main(int argc, char* argv[]) {
    return cmd_scan(argc, argv);
}