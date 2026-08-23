#pragma once

#include <string>

namespace Romset {

/* Parses a --model name ("mk2", "st", "mk1", "cm300", "jv880", "scb55",
 * "rlp3237", "sc155", "sc155mk2").  Returns false if the name is unknown. */
bool Parse(const std::string &name, int *romset);

/* Upstream's autodetect: the first romset in enum order whose files are all
 * present in `dir`, or -1 when none is complete. */
int Autodetect(const std::string &dir);

const char *Name(int romset);

/* Applies the model flags for `romset` and reads its ROM files out of `dir`.
 * On failure returns false and prints what is missing or short. */
bool Load(const std::string &dir, int romset);

/* Prints the file names `romset` expects, for when nothing could be found. */
void PrintExpectedFiles(const std::string &dir, int romset);

} // namespace Romset
