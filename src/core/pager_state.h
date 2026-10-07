#pragma once
#include <algorithm>
#include <string>
#include <cstdint>
#include <optional>
#include <limits>
namespace dtv::core {
inline int64_t firstRowOnPage(int64_t page, int64_t pageSize) {
    return (page - 1) * pageSize + 1;
}
inline int normalizePageRows(std::optional<int64_t> value = std::nullopt) {
    return value ? static_cast<int>(std::clamp<int64_t>(*value, 100, 3000)) : 500;
}
inline int normalizePageRows(const std::string &value) {
    try {
        size_t end = 0;
        const auto parsed = std::stoll(value, &end);
        return end == value.size() ? normalizePageRows(parsed) : 500;
    } catch(...) {
        return 500;
    }
}
struct PagerState {
    int64_t page = 1;
    int pageSize = 500;
    std::optional<int64_t> total;
    bool hasMore = false;
    bool arrivedFromPrev = false;
    int64_t pages() const {
        return total && pageSize > 0
                   ? std::max<int64_t>(1, *total / pageSize + (*total % pageSize != 0))
                   : 0;
    }
    bool canFirst() const {
        return page > 1;
    }
    bool canPrev() const {
        return page > 1;
    }
    bool canNext() const {
        return total ? page < pages() : (hasMore || arrivedFromPrev);
    }
    bool canLast() const {
        return total && page < pages();
    }
};
} // namespace dtv::core
