#include "separation/SeparateCommand.h"

#include <iostream>
#include <string>
#include <vector>

// overdub-separate: the Separation helper process (ADR 0007). See separation/SeparateCommand.h.
int main (int argc, char** argv)
{
    const std::vector<std::string> arguments (argv + 1, argv + argc);
    return overdub::separation::runSeparateCommand (arguments, std::cout, std::cerr);
}
