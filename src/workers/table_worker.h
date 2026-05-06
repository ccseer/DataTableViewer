#pragma once

#include <QObject>
#include <QString>
#include <memory>
#include "core/itable_parser.h"

namespace dtv {
namespace workers {

class TableWorker : public QObject {
    Q_OBJECT
public:
    TableWorker(std::unique_ptr<core::ITableParser> parser, const QString &path,
                const QString &tableName, int generation);

public slots:
    void doParse();

signals:
    void parseCompleted(std::shared_ptr<const core::TableParseResult> result,
                        const QString &tableName, int generation);

private:
    std::unique_ptr<core::ITableParser> m_parser;
    QString m_path;
    QString m_tableName;
    int m_generation;
};

} // namespace workers
} // namespace dtv
