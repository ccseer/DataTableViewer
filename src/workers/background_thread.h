#pragma once

#include <QThread>

class BackgroundThread : public QThread {
    Q_OBJECT
public:
    using QThread::QThread;

    ~BackgroundThread() override
    {
        quit();
        wait();
    }
};
