#include "parser_registry.h"
#include <algorithm>

namespace dtv {
namespace core {

ParserRegistry &ParserRegistry::instance()
{
    static ParserRegistry reg;
    return reg;
}

void ParserRegistry::registerParser(const std::string &extension, Factory factory)
{
    std::string ext = extension;
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    m_factories[ext] = std::move(factory);
}

std::unique_ptr<ITableParser> ParserRegistry::createParser(const std::string &extension) const
{
    std::string ext = extension;
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    auto it = m_factories.find(ext);
    if(it != m_factories.end()) {
        return it->second();
    }
    return nullptr;
}

void ParserRegistry::registerBuiltinParsers()
{
    // This exists to ensure the linker pulls in the parser translation units
    // if using static registration. If parsers are in the same library,
    // it's usually enough to reference them here or just let the macros work.
}

} // namespace core
} // namespace dtv
