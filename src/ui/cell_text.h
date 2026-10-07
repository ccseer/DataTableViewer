#pragma once

#include <QString>
#include <string>

namespace dtv::ui {
inline QString singleLineDisplayText(const std::string &cell)
{
    bool hasLineBreak = false;
    for(char ch : cell) {
        if(ch == '\r' || ch == '\n') {
            hasLineBreak = true;
            break;
        }
    }

    if(!hasLineBreak) {
        return QString::fromStdString(cell);
    }

    const QString original = QString::fromStdString(cell);
    QString text;
    text.reserve(original.size());
    bool previousWasNewline = false;

    for(QChar ch : original) {
        if(ch == QLatin1Char('\r') || ch == QLatin1Char('\n')) {
            if(!previousWasNewline) {
                text += QLatin1Char(' ');
                previousWasNewline = true;
            }
            continue;
        }
        text += ch;
        previousWasNewline = false;
    }

    return text;
}

} // namespace dtv::ui
