// cover — top-hat command dispatcher
// Usage: cover <subcommand> [args...]
//
// Subcommands:
//   scan    Scan a filesystem directory to build the size database
//   match   Match a source DB against a backup DB
//   report  Compute and display coverage statistics
//   fuse    Mount the covered filesystem via FUSE

#include "commands.hpp"

#include <iostream>
#include <cstring>

static void print_usage(const char* prog) {
    std::cerr << "Usage: " << prog << " <subcommand> [args...]\n\n"
              << "Subcommands:\n"
              << "  scan    Scan a filesystem directory\n"
              << "  match   Match source against a backup\n"
              << "  report  Show coverage statistics\n"
              << "  fuse    Mount covered filesystem via FUSE\n"
              << "\n"
              << "Examples:\n"
              << "  " << prog << " scan /media/yves/Big\n"
              << "  " << prog << " scan --compute-hash /media/yves/Big\n"
              << "  " << prog << " match covered_media_yves_Big/ covered_nfs_tronaut_mnt_Backup/\n"
              << "  " << prog << " report covered_media_yves_Big/\n"
              << "  " << prog << " report -r covered_media_yves_Big/\n"
              << "  " << prog << " fuse covered_media_yves_Big/ /tmp/covered_mount\n"
              << "\n"
              << "Run '" << prog << " <subcommand> --help' for subcommand-specific options.\n";
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    std::string subcommand = argv[1];

    // Shift argc/argv by 1 so subcommand functions receive
    // argv[0] = <prog> <subcommand> style args.
    // We rebuild argv to pass the original argv[0] + subcommand as argv[0]
    // and the remaining args starting at index 1.

    int new_argc = argc - 1;
    char** new_argv = new char*[new_argc + 1];

    // Build a new argv[0] like "cover scan"
    std::string arg0 = std::string(argv[0]) + " " + subcommand;
    new_argv[0] = const_cast<char*>(arg0.c_str());

    for (int i = 2; i < argc; ++i) {
        new_argv[i - 1] = argv[i];
    }
    new_argv[new_argc] = nullptr;

    int ret = 1;

    if (subcommand == "scan") {
        ret = cmd_scan(new_argc, new_argv);
    } else if (subcommand == "match") {
        ret = cmd_match(new_argc, new_argv);
    } else if (subcommand == "report") {
        ret = cmd_report(new_argc, new_argv);
    } else if (subcommand == "fuse") {
        ret = cmd_fuse(new_argc, new_argv);
    } else if (subcommand == "--help" || subcommand == "-h") {
        print_usage(argv[0]);
        ret = 0;
    } else {
        std::cerr << "Unknown subcommand: " << subcommand << "\n\n";
        print_usage(argv[0]);
        ret = 1;
    }

    delete[] new_argv;
    return ret;
}