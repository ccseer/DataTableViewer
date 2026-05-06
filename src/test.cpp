#include <QApplication>
#include <QFileDialog>
#include "ui/data_table_viewer.h"

// Mock internal classes from Seer for standalone test
struct MockViewOptionsPrivate {
    QString path;
    QString viewer_type;
    int theme = 1;
    qreal dpr = 1.0;
};

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    QString path = (argc > 1)
                       ? QString::fromLocal8Bit(argv[1])
                       : QFileDialog::getOpenFileName(
                             nullptr, "Open Table", {},
                             "Table Files (*.csv *.tsv *.sqlite *.db *.db3 *.sl3);;All Files (*)");
    if(path.isEmpty())
        return 0;

    DataTableViewer viewer;

    // Manual setup of ViewOptions mock
    MockViewOptionsPrivate d;
    d.dpr = 1.0;
    d.theme = 1;
    d.path = path;
    d.viewer_type = viewer.name();

    ViewOptions opts;
    opts.d_ptr = (ViewOptionsPrivate *)&d; // Hacky mock for standalone test

    viewer.setWindowTitle(path);
    viewer.load(nullptr, &opts);
    viewer.resize(viewer.getContentSize());
    viewer.show();

    return app.exec();
}
