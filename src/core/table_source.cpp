#include "table_source.h"
#include <stdexcept>
namespace dtv::core {
MaterializedTableSource::MaterializedTableSource(std::shared_ptr<const TableData> data)
    : m_data(std::move(data))
{
    if(!m_data)
        throw std::invalid_argument("Table data is required");
    if(!m_data->truncated)
        m_total = static_cast<int64_t>(m_data->rows.size());
    else if(m_data->total_rows)
        m_total = static_cast<int64_t>(m_data->total_rows);
}
const std::vector<ColumnMeta> &MaterializedTableSource::columns() const
{
    return m_data->columns;
}
std::optional<int64_t> MaterializedTableSource::rowCount() const
{
    return m_total;
}
void MaterializedTableSource::setKnownTotal(int64_t total)
{
    if(total >= 0)
        m_total = total;
}
PageResult MaterializedTableSource::first(int)
{
    PageResult result;
    result.ok = true;
    result.data = m_data;
    return result;
}
PageResult MaterializedTableSource::next(const PageToken &, int)
{
    return first(0);
}
PageResult MaterializedTableSource::prev(const PageToken &, int)
{
    return first(0);
}
PageResult MaterializedTableSource::last(int, std::optional<int64_t>)
{
    return first(0);
}
RefetchResult MaterializedTableSource::refetch(const RefetchKey &)
{
    return {false, "Materialized cells do not need refetch", {}, {}};
}
} // namespace dtv::core
