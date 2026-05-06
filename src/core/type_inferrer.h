#pragma once

#include "table_data.h"

namespace dtv {
namespace core {
namespace TypeInferrer {

/**
 * Infers column types (Integer, Float, Boolean, String) by sampling rows.
 * Also populates TableData::numeric_cache for columns identified as numeric.
 */
void infer(TableData &data);

} // namespace TypeInferrer
} // namespace core
} // namespace dtv
