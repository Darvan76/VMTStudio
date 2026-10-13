#pragma once

#include <QSyntaxHighlighter>
#include <QTextCharFormat>
#include <QRegularExpression>
#include <vector>

namespace ui {
namespace qt {

class VmtHighlighter : public QSyntaxHighlighter {
    Q_OBJECT

public:
    explicit VmtHighlighter(QTextDocument* parent = nullptr);

protected:
    void highlightBlock(const QString& text) override;

private:
    struct HighlightingRule {
        QRegularExpression pattern;
        QTextCharFormat format;
    };

    std::vector<HighlightingRule> m_rules;
    QRegularExpression m_commentPattern;
    QTextCharFormat m_commentFormat;
};

} // namespace qt
} // namespace ui
