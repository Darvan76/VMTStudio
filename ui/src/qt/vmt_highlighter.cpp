#include "vmt_highlighter.h"

#include <QFont>
#include <QColor>

namespace ui {
namespace qt {

VmtHighlighter::VmtHighlighter(QTextDocument* parent)
    : QSyntaxHighlighter(parent) {
    // 1. Shaders
    QTextCharFormat shaderFormat;
    shaderFormat.setForeground(QColor("#4ec9b0"));
    shaderFormat.setFontWeight(QFont::Bold);
    const QStringList shaderPatterns = {
        QStringLiteral("\"?LightmappedGeneric\"?"),
        QStringLiteral("\"?VertexLitGeneric\"?"),
        QStringLiteral("\"?UnlitGeneric\"?"),
        QStringLiteral("\"?WorldVertexTransition\"?"),
        QStringLiteral("\"?Water\"?"),
        QStringLiteral("\"?Refract\"?"),
        QStringLiteral("\"?Decal\"?"),
        QStringLiteral("\"?Subrect\"?"),
        QStringLiteral("\"?Sprite\"?"),
        QStringLiteral("\"?Cable\"?"),
        QStringLiteral("\"?Patch\"?"),
        QStringLiteral("\"?ShatteredGlass\"?"),
        QStringLiteral("\"?Sky\"?")
    };
    for (const QString& pattern : shaderPatterns) {
        HighlightingRule rule;
        rule.pattern = QRegularExpression(QStringLiteral("\\b") + pattern + QStringLiteral("\\b"),
                                          QRegularExpression::CaseInsensitiveOption);
        rule.format = shaderFormat;
        m_rules.push_back(rule);
    }

    // 2. Parameters ($basetexture, "$basetexture", %keywords, etc.)
    QTextCharFormat paramFormat;
    paramFormat.setForeground(QColor("#9cdcfe"));
    HighlightingRule paramRule;
    paramRule.pattern = QRegularExpression(QStringLiteral("\"?[$%][a-zA-Z0-9_]+\"?"));
    paramRule.format = paramFormat;
    m_rules.push_back(paramRule);

    // 3. Numbers / Floats
    QTextCharFormat numberFormat;
    numberFormat.setForeground(QColor("#b5cea8"));
    HighlightingRule numRule;
    numRule.pattern = QRegularExpression(QStringLiteral("\\b[-+]?[0-9]*\\.?[0-9]+\\b"));
    numRule.format = numberFormat;
    m_rules.push_back(numRule);

    // 4. Quoted strings (texture paths, parameters values)
    QTextCharFormat stringFormat;
    stringFormat.setForeground(QColor("#ce9178"));
    HighlightingRule stringRule;
    stringRule.pattern = QRegularExpression(QStringLiteral("\"[^\"]*\""));
    stringRule.format = stringFormat;
    m_rules.push_back(stringRule);

    // 5. Braces
    QTextCharFormat braceFormat;
    braceFormat.setForeground(QColor("#ffd700"));
    braceFormat.setFontWeight(QFont::Bold);
    HighlightingRule braceRule;
    braceRule.pattern = QRegularExpression(QStringLiteral("[{}]"));
    braceRule.format = braceFormat;
    m_rules.push_back(braceRule);

    // 6. Comments (take priority, applied last)
    m_commentFormat.setForeground(QColor("#6a9955"));
    m_commentFormat.setFontItalic(true);
    m_commentPattern = QRegularExpression(QStringLiteral("//[^\r\n]*"));
}

void VmtHighlighter::highlightBlock(const QString& text) {
    for (const auto& rule : m_rules) {
        auto matchIterator = rule.pattern.globalMatch(text);
        while (matchIterator.hasNext()) {
            auto match = matchIterator.next();
            setFormat(match.capturedStart(), match.capturedLength(), rule.format);
        }
    }

    auto commentIterator = m_commentPattern.globalMatch(text);
    while (commentIterator.hasNext()) {
        auto match = commentIterator.next();
        setFormat(match.capturedStart(), match.capturedLength(), m_commentFormat);
    }
}

} // namespace qt
} // namespace ui
