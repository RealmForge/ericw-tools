#include "test_main.hh"

#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include <common/log.hh>
#include <common/threads.hh>

bool tests_verbose = false;

int main(int argc, char **argv)
{
    logging::preinitialize();

    // writing console colors within test case output breaks doctest/CLion integration
    logging::enable_color_codes = false;

    for (int i = 1; i < argc; ++i) {
        // parse "-threads 1"
        if (!strcmp("-threads", argv[i]) || !strcmp("--threads", argv[i])) {
            if (!(i + 1 < argc)) {
                logging::print("--threads requires an argument\n");
                exit(1);
            }
            configureTBB(atoi(argv[i + 1]), false);
            continue;
        }
        // parse "-verbose"
        if (!strcmp("-verbose", argv[i]) || !strcmp("--verbose", argv[i])) {
            tests_verbose = true;
            continue;
        }
    }

    doctest::Context context;

    context.applyCommandLine(argc, argv);
    int res = context.run();

    if (context.shouldExit()) {
        return res;
    }

    return res;
}
