#include "program.hpp"
#include <csignal>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <sdl/event.hpp>
#include <string>
#include <vector>

namespace
{
    void signal_handler(int /*signum*/)
    {
        sdl::event_manager::push_quit_event();
    }
}

int main(int argc, char** argv)
{
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
#if defined(SIGPIPE)
    std::signal(SIGPIPE, SIG_IGN);
#endif

    try
    {
        std::vector<std::string> cmdline(argv, argv + argc);
        osect::program p(cmdline);
        p.run();
        return EXIT_SUCCESS;
    }
    catch(const osect::program::help_requested&)
    {
        return EXIT_SUCCESS;
    }
    catch(const std::exception& e)
    {
        std::cerr << "FATAL ERROR: " << e.what() << std::endl;
        return EXIT_FAILURE;
    }
}
