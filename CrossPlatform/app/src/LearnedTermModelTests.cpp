#include "LearnedTermModel.hpp"

#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {
using localflow::core::LearnedTerm;

int failures = 0;

void expect(const bool condition, const char* message) {
    if (condition) return;
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
}

LearnedTerm term(
    std::string id,
    std::string canonical,
    std::vector<std::string> aliases,
    const std::uint32_t uses,
    const std::int64_t lastUsed,
    std::string source) {
    LearnedTerm value;
    value.id = std::move(id);
    value.canonical = std::move(canonical);
    value.aliases = std::move(aliases);
    value.use_count = uses;
    value.created_at_ms = lastUsed - 1000;
    value.last_used_at_ms = lastUsed;
    value.source_app_id = std::move(source);
    return value;
}
}  // namespace

int main(int argc, char* argv[]) {
    QCoreApplication application(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("LocalFlowTests"));
    QCoreApplication::setApplicationName(QStringLiteral("LearnedTermModelTests"));

    QTemporaryDir directory;
    expect(directory.isValid(), "temporary directory is available");
    const QString path = directory.filePath(QStringLiteral("learned-terminology.json"));
    LearnedTermModel model(path);
    expect(model.count() == 0, "missing persistence file loads as an empty bank");
    expect(model.lastError().isEmpty(), "missing persistence file is not an error");

    const auto postgres = term(
        "postgresql", "PostgreSQL", {"post grass q l"}, 7, 1'725'000'000'000,
        "com.example.Terminal");
    const auto localFlow = term(
        "localflow", "LocalFlow", {"local flow"}, 3, 1'725'000'001'000,
        "com.example.Editor");
    expect(
        model.replaceTermsAndSave({postgres, localFlow}),
        "pipeline terms are persisted");
    expect(model.count() == 2, "persisted pipeline terms are exposed");
    const QModelIndex first = model.index(0, 0);
    expect(
        model.data(first, LearnedTermModel::CanonicalRole).toString()
            == QStringLiteral("PostgreSQL"),
        "canonical role is exposed");
    expect(
        model.data(first, LearnedTermModel::AliasesRole).toStringList()
            == QStringList{QStringLiteral("post grass q l")},
        "aliases role is exposed");
    expect(
        model.data(first, LearnedTermModel::UseCountRole).toULongLong() == 7,
        "use count role is exposed");
    expect(
        model.data(first, LearnedTermModel::LastUsedAtMsRole).toLongLong()
            == 1'725'000'000'000,
        "last-used role is exposed");
    expect(
        model.data(first, LearnedTermModel::SourceAppIdRole).toString()
            == QStringLiteral("com.example.Terminal"),
        "source application role is exposed");

    expect(model.removeTerm(0), "a term can be removed");
    expect(model.count() == 1 && model.canUndo(), "removal enables one-step undo");
    LearnedTermModel afterRemoval(path);
    expect(
        afterRemoval.count() == 1
            && afterRemoval.data(afterRemoval.index(0, 0), LearnedTermModel::CanonicalRole)
                .toString() == QStringLiteral("LocalFlow"),
        "removal uses the existing persistence schema");
    expect(model.undoLastRemoval(), "last removal can be undone");
    expect(model.count() == 2 && !model.canUndo(), "undo restores the removed row");

    expect(model.clearTerms(), "all learned terms can be cleared");
    expect(model.count() == 0 && model.canUndo(), "clear can also be undone");
    expect(model.undoLastRemoval(), "cleared terms can be restored");
    expect(model.count() == 2, "undo restores the cleared bank");

    QFile malformed(path);
    expect(
        malformed.open(QIODevice::WriteOnly | QIODevice::Truncate),
        "malformed fixture can be written");
    malformed.write("{not-json");
    malformed.close();
    expect(!model.reload(), "malformed persistence is reported");
    expect(model.count() == 2, "malformed persistence does not erase the live model");
    expect(!model.lastError().isEmpty(), "malformed persistence has an actionable error");

    if (failures == 0) {
        std::cout << "LearnedTermModel tests passed.\n";
        return EXIT_SUCCESS;
    }
    std::cerr << failures << " LearnedTermModel assertion(s) failed.\n";
    return EXIT_FAILURE;
}
