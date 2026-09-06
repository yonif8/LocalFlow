#include "../src/SentenceCarry.hpp"

#include <QCoreApplication>
#include <QDebug>

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    const auto check = [](const std::string& input, const std::string& head, const std::string& tail) {
        const auto parts = localflow::app::holdLastSentence(input);
        if (parts.first != head || parts.second != tail) {
            qCritical() << "Sentence carry mismatch";
            return false;
        }
        return true;
    };
    bool ok = true;
    ok &= check("", "", "");
    ok &= check("Keep these words.", "", "Keep these words.");
    ok &= check("First sentence. Second sentence.", "First sentence.", "Second sentence.");
    ok &= check("Open Example.swift. Then check PostgreSQL.", "Open Example.swift.", "Then check PostgreSQL.");
    ok &= check("Completed sentence. Unfinished words", "Completed sentence.", "Unfinished words");
    ok &= check("你好。再见。", "你好。", "再见。");
    return ok ? 0 : 1;
}
