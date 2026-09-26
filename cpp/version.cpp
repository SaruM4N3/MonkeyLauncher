#include "version.hpp"

#include "common.hpp"

namespace ml {

const std::string& current_version() {
    static const std::string v = [] {
        const fs::path here = exe_dir();
        std::error_code ec;
        for (const auto& candidate : {here / "VERSION", here.parent_path() / "VERSION"})
            if (fs::exists(candidate, ec)) return strip(read_text_file(candidate));
        return std::string("0.0.0");
    }();
    return v;
}

}  // namespace ml
