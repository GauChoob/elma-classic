#ifndef EOL_API
#define EOL_API

#include <optional>
#include <string>

#define JSON_DIAGNOSTICS 1
#include <nlohmann/json.hpp>
using json = nlohmann::ordered_json;

namespace eol_api {

void init();
void cleanup();

// Download an lgr to your lgr/ folder
// Returns an error string if not successful
std::optional<std::string> lgr_get(const std::string& lgr_name);

// BLAH BLAH
// Returns an error string if not successful
std::pair<json, std::string> lgr_info();

// TEST
void update_lgrs();

} // namespace eol_api

#endif
