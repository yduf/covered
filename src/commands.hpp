#ifndef COVERED_COMMANDS_HPP
#define COVERED_COMMANDS_HPP

// Command-line entry points for each subcommand.
// Each function expects argc/argv exactly as if it were the only
// executable (i.e. argv[0] is the subcommand name).

int cmd_scan(int argc, char* argv[]);
int cmd_match(int argc, char* argv[]);
int cmd_report(int argc, char* argv[]);
int cmd_fuse(int argc, char* argv[]);
int cmd_update(int argc, char* argv[]);

#endif // COVERED_COMMANDS_HPP