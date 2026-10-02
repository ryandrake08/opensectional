#pragma once

#include <filesystem>
#include <stdexcept>
#include <string>

namespace osect::test
{
    // A disposable directory holding a user.db and an ephemeral.db,
    // removed on destruction.
    class tmp_user_db
    {
    public:
        std::filesystem::path db_file;
        std::filesystem::path ephemeral_db_file;

        explicit tmp_user_db(const char* tag)
        {
            const auto base =
                std::filesystem::temp_directory_path() / ("osect_test_user_" + std::string(tag) + "_");
            for(int i = 0; i < 1000; ++i)
            {
                const auto candidate = base.string() + std::to_string(i);
                if(!std::filesystem::exists(candidate))
                {
                    dir = candidate;
                    std::filesystem::create_directories(dir);
                    db_file = dir / "user.db";
                    ephemeral_db_file = dir / "ephemeral.db";
                    return;
                }
            }
            throw std::runtime_error("could not pick a tmp dir");
        }

        ~tmp_user_db()
        {
            std::error_code ec;
            std::filesystem::remove_all(dir, ec);
        }

        tmp_user_db(const tmp_user_db&) = delete;
        tmp_user_db& operator=(const tmp_user_db&) = delete;

    private:
        std::filesystem::path dir;
    };
}
