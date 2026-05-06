#pragma once

#include <string>
#include <map>
#include <functional>
#include <memory>
#include "itable_parser.h"

namespace dtv {
namespace core {

class ParserRegistry {
public:
    using Factory = std::function<std::unique_ptr<ITableParser>()>;

    static ParserRegistry &instance();

    void registerParser(const std::string &extension, Factory factory);
    std::unique_ptr<ITableParser> createParser(const std::string &extension) const;

    // Called once during plugin init (idempotent)
    void registerBuiltinParsers();

private:
    ParserRegistry() = default;
    std::map<std::string, Factory> m_factories;
};

// Macro for static registration.
#define REGISTER_TABLE_PARSER(ext, factory_expr)                                                   \
    static inline bool registered_##ext##_flag = []() {                                            \
        dtv::core::ParserRegistry::instance().registerParser(#ext, factory_expr);                  \
        return true;                                                                               \
    }();

} // namespace core
} // namespace dtv
