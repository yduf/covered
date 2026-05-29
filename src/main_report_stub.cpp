// Backward-compatible entry point for the old cover_report executable.
#include "commands.hpp"

int main(int argc, char* argv[]) {
    return cmd_report(argc, argv);
}