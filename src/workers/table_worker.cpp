#include "table_worker.h"
#include "core/type_inferrer.h"
#include <QFile>
#include <QElapsedTimer>
#include <QScopeGuard>
#include <QThread>
#include <QDebug>
#include <thread>

namespace dtv {
namespace workers {

namespace {
constexpr qint64 kMaxFileBytes = 64 * 1024 * 1024;
}

TableWorker::TableWorker(std::unique_ptr<core::ITableParser> parser, const QString &path,
                         const QString &tableName, int generation)
    : m_parser(std::move(parser)), m_path(path), m_tableName(tableName), m_generation(generation)
{}

void TableWorker::doParse()
{
    auto cleanup = qScopeGuard([this] {
        deleteLater();
    });

    if(thread()->isInterruptionRequested())
        return;

    auto result = std::make_shared<core::TableParseResult>();
    QByteArray fileData;
    core::ParseInput input;
    std::string pathStd = m_path.toStdString();
    std::string tableStd = m_tableName.toStdString();

    input.file_path = pathStd;
    input.table_name = tableStd;

    if(m_parser->input_kind() == core::ParseInputKind::Bytes) {
        QFile file(m_path);
        if(!file.open(QIODevice::ReadOnly)) {
            result->ok = false;
            result->error = "Could not open file: " + file.errorString().toStdString();
            emit parseCompleted(result, m_tableName, m_generation);
            return;
        }
        if(file.size() > kMaxFileBytes) {
            result->ok = false;
            result->error = "File too large (> 64 MB)";
            emit parseCompleted(result, m_tableName, m_generation);
            return;
        }
        fileData = file.readAll();

        // Strip UTF-8 BOM
        const char *dataPtr = fileData.constData();
        size_t dataSize = fileData.size();
        if(dataSize >= 3 && static_cast<unsigned char>(dataPtr[0]) == 0xEF &&
           static_cast<unsigned char>(dataPtr[1]) == 0xBB &&
           static_cast<unsigned char>(dataPtr[2]) == 0xBF) {
            dataPtr += 3;
            dataSize -= 3;
        }
        input.bytes = std::string_view(dataPtr, dataSize);
    }

    // Capture metadata before parse in case of failure
    std::string fmtName = m_parser->format_name();
    std::string libCredit = m_parser->library_credit();

    QElapsedTimer timer;
    timer.start();

    try {
        *result = m_parser->parse(input);
    } catch(const std::exception &e) {
        result->ok = false;
        result->error = std::string("Parser exception: ") + e.what();
    } catch(...) {
        result->ok = false;
        result->error = "Parser threw an unknown exception";
    }

    result->elapsed_ms = timer.elapsed();
    result->format_name = fmtName;
    result->library_credit = libCredit;
    result->file_bytes = fileData.size();

    if(result->ok && result->data && m_parser->input_kind() == core::ParseInputKind::Bytes) {
        core::TypeInferrer::infer(*result->data);
    }

    if(thread()->isInterruptionRequested())
        return;

    emit parseCompleted(result, m_tableName, m_generation);
}

} // namespace workers
} // namespace dtv
