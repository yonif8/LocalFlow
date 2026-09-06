#pragma once

#include <QString>
#include <QTextBoundaryFinder>
#include <string>
#include <utility>

namespace localflow::app {

// Unicode sentence boundaries, not a period/URL/filename regex. Preserve the
// last recognized sentence so it can be polished with the next audio segment.
inline std::pair<std::string, std::string> holdLastSentence(const std::string& input) {
    const auto text = QString::fromStdString(input);
    QTextBoundaryFinder finder(QTextBoundaryFinder::Sentence, text);
    qsizetype lastStart = 0;
    for (auto position = finder.toNextBoundary(); position >= 0; position = finder.toNextBoundary()) {
        if (position < text.size() && finder.boundaryReasons().testFlag(QTextBoundaryFinder::StartOfItem)) {
            lastStart = position;
        }
    }
    return {text.left(lastStart).trimmed().toStdString(), text.mid(lastStart).trimmed().toStdString()};
}

} // namespace localflow::app
