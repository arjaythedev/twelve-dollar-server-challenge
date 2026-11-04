#include "inputs.hpp"
#include "runtime.hpp"

#include <chrono>
#include <csignal>
#include <filesystem>
#include <iostream>
#include <string_view>
#include <thread>

int main(int argc, char **argv)
{
    using namespace loadgen;
    try
    {
        if (argc == 2 && std::string_view(argv[1]) == "--version")
        {
            std::cout << "td-load " << version << '\n';
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--help")
        {
            std::cout << help();
            return 0;
        }
        const auto options = parse_options(argc, argv);
        std::signal(SIGINT, &stop_signal);
        std::signal(SIGTERM, &stop_signal);
        Load load(options.runtime, endpoint(options.base_url), read_tokens(options.tokens),
                  options.local_ips);
        if (!options.ready_file.empty())
        {
            write_file(options.ready_file, "ready\n");
        }
        if (!options.start_gate.empty())
        {
            std::cout << "ready, waiting for start gate\n" << std::flush;
            while (!std::filesystem::exists(options.start_gate) && !interrupted)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
        auto metrics = load.run();
        const auto summary = summarize(metrics, options.runtime);
        if (!options.summary.empty())
        {
            write_file(options.summary, summary.json);
        }
        std::cout << summary.json;
        return interrupted ? 130 : summary.passed ? 0 : 99;
    }
    catch (const std::exception &error)
    {
        std::cerr << "load_generator: " << error.what() << '\n';
        return 1;
    }
}
