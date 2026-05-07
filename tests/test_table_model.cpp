#include <QtTest>
#include "ui/table_model.h"

class TestTableModel : public QObject {
    Q_OBJECT
private slots:
    void displayRoleFlattensEmbeddedNewlinesButTooltipPreservesOriginal()
    {
        auto data = std::make_shared<dtv::core::TableData>();
        data->columns.push_back({"notes", dtv::core::ColumnMeta::Type::String});
        data->rows.push_back({"line one\nline two\r\nline three"});

        dtv::ui::TableModel model;
        model.setTableData(data);

        QModelIndex index = model.index(0, 0);
        QCOMPARE(index.data(Qt::DisplayRole).toString(), QString("line one line two line three"));
        QCOMPARE(index.data(Qt::ToolTipRole).toString(),
                 QString("line one\nline two\r\nline three"));
    }
};

QTEST_GUILESS_MAIN(TestTableModel)
#include "test_table_model.moc"
