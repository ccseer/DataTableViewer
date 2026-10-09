#pragma once

#include <QString>
#include <QStringView>
#include <string>

namespace dtv::ui {
// Cells may contain the line breaks of a multiline quoted CSV field; the table
// view renders one line per row, so those are folded to single spaces. The
// QStringView overload lets callers that already hold a QString skip the
// std::string round-trip (an allocation plus a UTF-8 conversion on both ends).
inline QString singleLineDisplayText(QStringView text)
{
    if(!text.contains(QChar('\r')) && !text.contains(QChar('\n')))
        return text.toString();

    QString out;
    out.reserve(text.size());
    bool previousWasNewline = false;

    for(const QChar ch : text) {
        if(ch == QLatin1Char('\r') || ch == QLatin1Char('\n')) {
            if(!previousWasNewline) {
                out += QLatin1Char(' ');
                previousWasNewline = true;
            }
            continue;
        }
        out += ch;
        previousWasNewline = false;
    }

    return out;
}

inline QString singleLineDisplayText(const QString &text)
{
    return singleLineDisplayText(QStringView(text));
}

inline QString singleLineDisplayText(const std::string &cell)
{
    return singleLineDisplayText(QString::fromStdString(cell));
}

} // namespace dtv::ui
