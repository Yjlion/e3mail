// SPDX-License-Identifier: MPL-2.0
#include "Html.h"

#include <QHash>
#include <QList>
#include <QSet>
#include <QUrl>

namespace e3::mime {

namespace {

struct Token
{
    enum Kind { Text, Open, Close, SelfClose } kind;
    QString name;                       // lower-case tag name
    QList<QPair<QString, QString>> attrs; // lower-case names, decoded values
    QString text;                       // decoded text for Text tokens
};

QString decodeEntities(QStringView s)
{
    static const QHash<QString, char32_t> named = {
        {QStringLiteral("amp"), U'&'},     {QStringLiteral("lt"), U'<'},
        {QStringLiteral("gt"), U'>'},      {QStringLiteral("quot"), U'"'},
        {QStringLiteral("apos"), U'\''},   {QStringLiteral("nbsp"), U' '},
        {QStringLiteral("copy"), U'©'},    {QStringLiteral("reg"), U'®'},
        {QStringLiteral("trade"), U'™'},   {QStringLiteral("hellip"), U'…'},
        {QStringLiteral("mdash"), U'—'},   {QStringLiteral("ndash"), U'–'},
        {QStringLiteral("lsquo"), U'‘'},   {QStringLiteral("rsquo"), U'’'},
        {QStringLiteral("ldquo"), U'“'},   {QStringLiteral("rdquo"), U'”'},
        {QStringLiteral("laquo"), U'«'},   {QStringLiteral("raquo"), U'»'},
        {QStringLiteral("euro"), U'€'},    {QStringLiteral("pound"), U'£'},
        {QStringLiteral("yen"), U'¥'},     {QStringLiteral("cent"), U'¢'},
        {QStringLiteral("deg"), U'°'},     {QStringLiteral("middot"), U'·'},
        {QStringLiteral("bull"), U'•'},    {QStringLiteral("times"), U'×'},
        {QStringLiteral("zwnj"), U'‌'}, {QStringLiteral("zwj"), U'‍'},
        {QStringLiteral("shy"), U'­'},
    };
    QString out;
    out.reserve(s.size());
    for (qsizetype i = 0; i < s.size(); ++i) {
        if (s[i] != u'&') {
            out.append(s[i]);
            continue;
        }
        const qsizetype semi = s.indexOf(u';', i);
        if (semi < 0 || semi - i > 12) {
            out.append(s[i]);
            continue;
        }
        const QStringView ent = s.mid(i + 1, semi - i - 1);
        char32_t cp = 0;
        if (ent.startsWith(u'#')) {
            bool ok = false;
            cp = ent.size() > 1 && (ent[1] == u'x' || ent[1] == u'X') ? ent.mid(2).toUInt(&ok, 16)
                                                                      : ent.mid(1).toUInt(&ok, 10);
            if (!ok || cp == 0 || cp > 0x10FFFF)
                cp = 0xFFFD;
        } else if (auto it = named.constFind(ent.toString().toLower()); it != named.constEnd()) {
            cp = *it;
        }
        if (!cp) {
            out.append(s[i]);
            continue;
        }
        out.append(QString::fromUcs4(&cp, 1));
        i = semi;
    }
    return out;
}

// A forgiving tokenizer: it never fails, and anything it cannot read as markup
// becomes text, which the emitter escapes.
QList<Token> tokenize(const QString &html)
{
    QList<Token> tokens;
    qsizetype i = 0;
    const qsizetype n = html.size();
    QString text;
    auto flushText = [&] {
        if (!text.isEmpty()) {
            tokens.append({Token::Text, {}, {}, decodeEntities(text)});
            text.clear();
        }
    };
    while (i < n) {
        const QChar c = html[i];
        if (c != u'<') {
            text.append(c);
            ++i;
            continue;
        }
        if (html.mid(i, 4) == QLatin1String("<!--")) {
            flushText();
            const qsizetype end = html.indexOf(QLatin1String("-->"), i + 4);
            i = end < 0 ? n : end + 3;
            continue;
        }
        if (i + 1 < n && (html[i + 1] == u'!' || html[i + 1] == u'?')) {
            flushText();
            const qsizetype end = html.indexOf(u'>', i);
            i = end < 0 ? n : end + 1;
            continue;
        }
        const bool closing = i + 1 < n && html[i + 1] == u'/';
        qsizetype j = i + (closing ? 2 : 1);
        const qsizetype nameStart = j;
        while (j < n && (html[j].isLetterOrNumber() || html[j] == u'-' || html[j] == u':'))
            ++j;
        if (j == nameStart) {
            text.append(c);
            ++i;
            continue;
        }
        flushText();
        Token tok{closing ? Token::Close : Token::Open, html.mid(nameStart, j - nameStart).toLower(), {}, {}};
        // Attributes
        while (j < n && html[j] != u'>') {
            while (j < n && (html[j].isSpace() || html[j] == u'/')) {
                if (html[j] == u'/' && j + 1 < n && html[j + 1] == u'>')
                    tok.kind = Token::SelfClose;
                ++j;
            }
            if (j >= n || html[j] == u'>')
                break;
            const qsizetype an = j;
            while (j < n && !html[j].isSpace() && html[j] != u'=' && html[j] != u'>' && html[j] != u'/')
                ++j;
            const QString attrName = html.mid(an, j - an).toLower();
            QString value;
            while (j < n && html[j].isSpace())
                ++j;
            if (j < n && html[j] == u'=') {
                ++j;
                while (j < n && html[j].isSpace())
                    ++j;
                if (j < n && (html[j] == u'"' || html[j] == u'\'')) {
                    const QChar q = html[j++];
                    const qsizetype vs = j;
                    while (j < n && html[j] != q)
                        ++j;
                    value = html.mid(vs, j - vs);
                    if (j < n)
                        ++j;
                } else {
                    const qsizetype vs = j;
                    while (j < n && !html[j].isSpace() && html[j] != u'>')
                        ++j;
                    value = html.mid(vs, j - vs);
                }
            }
            if (!attrName.isEmpty())
                tok.attrs.append({attrName, decodeEntities(value)});
            if (j == an)
                ++j; // guarantee progress
        }
        i = j < n ? j + 1 : n;
        tokens.append(tok);

        // Raw-text elements: skip to the matching close tag without tokenizing.
        static const QSet<QString> rawText = {QStringLiteral("script"), QStringLiteral("style"),
                                              QStringLiteral("textarea"), QStringLiteral("title"),
                                              QStringLiteral("xmp")};
        if (tok.kind == Token::Open && rawText.contains(tok.name)) {
            const qsizetype end = html.indexOf(QLatin1String("</") + tok.name, i, Qt::CaseInsensitive);
            if (tok.name == QLatin1String("textarea") || tok.name == QLatin1String("title"))
                tokens.append({Token::Text, {}, {}, decodeEntities(html.mid(i, (end < 0 ? n : end) - i))});
            i = end < 0 ? n : end;
        }
    }
    flushText();
    return tokens;
}

// Elements whose content is dropped along with them.
const QSet<QString> &droppedWithContent()
{
    static const QSet<QString> s = {
        QStringLiteral("script"), QStringLiteral("style"),  QStringLiteral("head"),
        QStringLiteral("title"),  QStringLiteral("iframe"), QStringLiteral("object"),
        QStringLiteral("embed"),  QStringLiteral("applet"), QStringLiteral("noscript"),
        QStringLiteral("template"), QStringLiteral("svg"),  QStringLiteral("math"),
        QStringLiteral("audio"),  QStringLiteral("video"),  QStringLiteral("canvas"),
        QStringLiteral("select"), QStringLiteral("button"), QStringLiteral("frameset"),
    };
    return s;
}

const QSet<QString> &voidElements()
{
    static const QSet<QString> s = {QStringLiteral("br"),   QStringLiteral("hr"),   QStringLiteral("img"),
                                    QStringLiteral("meta"), QStringLiteral("link"), QStringLiteral("input"),
                                    QStringLiteral("base"), QStringLiteral("col"),  QStringLiteral("wbr"),
                                    QStringLiteral("area"), QStringLiteral("source")};
    return s;
}

const QSet<QString> &blockElements()
{
    static const QSet<QString> s = {
        QStringLiteral("p"),     QStringLiteral("div"),    QStringLiteral("h1"),  QStringLiteral("h2"),
        QStringLiteral("h3"),    QStringLiteral("h4"),     QStringLiteral("h5"),  QStringLiteral("h6"),
        QStringLiteral("ul"),    QStringLiteral("ol"),     QStringLiteral("li"),  QStringLiteral("blockquote"),
        QStringLiteral("pre"),   QStringLiteral("table"),  QStringLiteral("tr"),  QStringLiteral("hr"),
        QStringLiteral("center"), QStringLiteral("section"), QStringLiteral("article"),
        QStringLiteral("header"), QStringLiteral("footer"), QStringLiteral("address"),
        QStringLiteral("dl"),    QStringLiteral("dt"),     QStringLiteral("dd"),
    };
    return s;
}

// Tag renames: anything not here and not in the keep list is unwrapped.
QString mapTag(const QString &name)
{
    static const QHash<QString, QString> map = {
        {QStringLiteral("strong"), QStringLiteral("b")}, {QStringLiteral("em"), QStringLiteral("i")},
        {QStringLiteral("strike"), QStringLiteral("s")}, {QStringLiteral("del"), QStringLiteral("s")},
        {QStringLiteral("ins"), QStringLiteral("u")},    {QStringLiteral("center"), QStringLiteral("div")},
        {QStringLiteral("section"), QStringLiteral("div")}, {QStringLiteral("article"), QStringLiteral("div")},
        {QStringLiteral("header"), QStringLiteral("div")},  {QStringLiteral("footer"), QStringLiteral("div")},
        {QStringLiteral("address"), QStringLiteral("div")}, {QStringLiteral("main"), QStringLiteral("div")},
        {QStringLiteral("tt"), QStringLiteral("code")},     {QStringLiteral("kbd"), QStringLiteral("code")},
        {QStringLiteral("samp"), QStringLiteral("code")},
    };
    static const QSet<QString> keep = {
        QStringLiteral("p"),  QStringLiteral("br"),  QStringLiteral("div"), QStringLiteral("span"),
        QStringLiteral("b"),  QStringLiteral("i"),   QStringLiteral("u"),   QStringLiteral("s"),
        QStringLiteral("h1"), QStringLiteral("h2"),  QStringLiteral("h3"),  QStringLiteral("h4"),
        QStringLiteral("h5"), QStringLiteral("h6"),  QStringLiteral("ul"),  QStringLiteral("ol"),
        QStringLiteral("li"), QStringLiteral("blockquote"), QStringLiteral("pre"), QStringLiteral("code"),
        QStringLiteral("table"), QStringLiteral("thead"), QStringLiteral("tbody"), QStringLiteral("tfoot"),
        QStringLiteral("tr"), QStringLiteral("td"),  QStringLiteral("th"),  QStringLiteral("a"),
        QStringLiteral("hr"), QStringLiteral("sub"), QStringLiteral("sup"), QStringLiteral("small"),
        QStringLiteral("big"), QStringLiteral("dl"), QStringLiteral("dt"),  QStringLiteral("dd"),
    };
    const QString mapped = map.value(name, name);
    return keep.contains(mapped) ? mapped : QString();
}

bool looksRemote(const QString &value)
{
    const QString v = value.trimmed().toLower();
    return v.startsWith(QLatin1String("http:")) || v.startsWith(QLatin1String("https:"))
        || v.startsWith(QLatin1String("//")) || v.startsWith(QLatin1String("ftp:"))
        || v.contains(QLatin1String("url("));
}

} // namespace

QString escapeHtml(const QString &text)
{
    QString out;
    out.reserve(text.size() + 16);
    for (QChar c : text) {
        switch (c.unicode()) {
        case '&': out += QLatin1String("&amp;"); break;
        case '<': out += QLatin1String("&lt;"); break;
        case '>': out += QLatin1String("&gt;"); break;
        case '"': out += QLatin1String("&quot;"); break;
        default: out += c;
        }
    }
    return out;
}

QString safeHref(const QString &href)
{
    const QUrl url(href.trimmed(), QUrl::StrictMode);
    if (!url.isValid())
        return {};
    const QString scheme = url.scheme().toLower();
    if (scheme == QLatin1String("http") || scheme == QLatin1String("https") || scheme == QLatin1String("mailto"))
        return url.toString(QUrl::FullyEncoded);
    return {};
}

SanitizedHtml sanitizeHtml(const QString &html)
{
    SanitizedHtml result;
    const QList<Token> tokens = tokenize(html);
    QString &out = result.html;
    int dropDepth = 0;
    QString dropTag;
    QList<QString> open; // emitted open tags, to close what the sender left open
    QList<bool> anchorStack;

    for (const Token &t : tokens) {
        if (dropDepth > 0) {
            if (t.name == dropTag && t.kind == Token::Open)
                ++dropDepth;
            else if (t.name == dropTag && t.kind == Token::Close)
                --dropDepth;
            continue;
        }
        if (t.kind == Token::Text) {
            out += escapeHtml(t.text);
            continue;
        }
        // Every way a message can make the renderer fetch something counts as
        // blocked remote content, whatever element it is on.
        for (const auto &[name, value] : t.attrs) {
            if ((name == QLatin1String("src") || name == QLatin1String("background")
                 || name == QLatin1String("style") || name == QLatin1String("srcset")
                 || name == QLatin1String("poster") || name == QLatin1String("data"))
                && looksRemote(value))
                ++result.remoteBlocked;
        }
        if (t.name == QLatin1String("link") && t.kind != Token::Close)
            ++result.remoteBlocked;

        if (droppedWithContent().contains(t.name)) {
            if (t.kind == Token::Open && !voidElements().contains(t.name)) {
                dropDepth = 1;
                dropTag = t.name;
            }
            continue;
        }
        if (t.name == QLatin1String("img") && t.kind != Token::Close) {
            QString alt;
            for (const auto &[name, value] : t.attrs) {
                if (name == QLatin1String("alt"))
                    alt = value;
            }
            if (!alt.trimmed().isEmpty())
                out += QLatin1String("[") + escapeHtml(alt.trimmed()) + QLatin1String("]");
            continue;
        }
        const QString tag = mapTag(t.name);
        if (tag.isEmpty()) {
            // Unwrapped: keep a line break where a block would have been.
            if (blockElements().contains(t.name) && t.kind != Token::Open)
                out += QLatin1String("<br>");
            continue;
        }
        if (t.kind == Token::Close) {
            if (tag == QLatin1String("a")) {
                if (!anchorStack.isEmpty() && anchorStack.takeLast())
                    out += QLatin1String("</a>");
                continue;
            }
            const qsizetype idx = open.lastIndexOf(tag);
            if (idx < 0)
                continue; // stray close tag
            while (open.size() > idx)
                out += QLatin1String("</") + open.takeLast() + u'>';
            continue;
        }
        if (tag == QLatin1String("a")) {
            QString href;
            for (const auto &[name, value] : t.attrs) {
                if (name == QLatin1String("href"))
                    href = safeHref(value);
            }
            anchorStack.append(!href.isEmpty());
            if (!href.isEmpty()) {
                out += QLatin1String("<a href=\"") + escapeHtml(href) + QLatin1String("\">");
                ++result.linksKept;
            }
            continue;
        }
        QString attrs;
        if (tag == QLatin1String("td") || tag == QLatin1String("th")) {
            for (const auto &[name, value] : t.attrs) {
                if ((name == QLatin1String("colspan") || name == QLatin1String("rowspan")) && value.toInt() > 0)
                    attrs += u' ' + name + QLatin1String("=\"") + QString::number(qMin(value.toInt(), 100)) + u'"';
            }
        }
        const bool isVoid = voidElements().contains(tag);
        out += u'<' + tag + attrs + u'>';
        if (!isVoid && t.kind != Token::SelfClose)
            open.append(tag);
    }
    while (!anchorStack.isEmpty()) {
        if (anchorStack.takeLast())
            out += QLatin1String("</a>");
    }
    while (!open.isEmpty())
        out += QLatin1String("</") + open.takeLast() + u'>';
    return result;
}

QString htmlToText(const QString &html)
{
    const QList<Token> tokens = tokenize(html);
    QString out;
    int dropDepth = 0;
    QString dropTag;
    int preDepth = 0;
    QList<int> olCounters;
    QString pendingHref;
    QString anchorText;
    bool inAnchor = false;

    auto newline = [&](int count = 1) {
        int existing = 0;
        for (qsizetype k = out.size() - 1; k >= 0 && out[k] == u'\n'; --k)
            ++existing;
        if (out.isEmpty())
            return;
        for (int k = existing; k < count; ++k)
            out += u'\n';
    };

    for (const Token &t : tokens) {
        if (dropDepth > 0) {
            if (t.name == dropTag && t.kind == Token::Open)
                ++dropDepth;
            else if (t.name == dropTag && t.kind == Token::Close)
                --dropDepth;
            continue;
        }
        if (t.kind == Token::Text) {
            QString s = t.text;
            s.replace(QChar(0x00A0), u' ');
            if (!preDepth) {
                s = s.simplified().isEmpty() && !s.isEmpty() ? QStringLiteral(" ") : s;
                QString collapsed;
                bool space = false;
                for (QChar c : s) {
                    if (c.isSpace()) {
                        if (!space)
                            collapsed += u' ';
                        space = true;
                    } else {
                        collapsed += c;
                        space = false;
                    }
                }
                s = collapsed;
                if (out.isEmpty() || out.endsWith(u'\n') || out.endsWith(u' '))
                    while (s.startsWith(u' '))
                        s.remove(0, 1);
            }
            out += s;
            if (inAnchor)
                anchorText += s;
            continue;
        }
        if (droppedWithContent().contains(t.name)) {
            if (t.kind == Token::Open && !voidElements().contains(t.name)) {
                dropDepth = 1;
                dropTag = t.name;
            }
            continue;
        }
        const QString &tag = t.name;
        const bool open = t.kind != Token::Close;
        if (tag == QLatin1String("br")) {
            out += u'\n';
        } else if (tag == QLatin1String("pre")) {
            preDepth += open ? 1 : -1;
            preDepth = qMax(0, preDepth);
            newline();
        } else if (tag == QLatin1String("p") || (tag.startsWith(u'h') && tag.size() == 2 && tag[1].isDigit())) {
            newline(2);
        } else if (tag == QLatin1String("ol")) {
            if (open)
                olCounters.append(0);
            else if (!olCounters.isEmpty())
                olCounters.removeLast();
            newline();
        } else if (tag == QLatin1String("li") && open) {
            newline();
            if (!olCounters.isEmpty())
                out += QString::number(++olCounters.last()) + QLatin1String(". ");
            else
                out += QStringLiteral("• ");
        } else if (tag == QLatin1String("a")) {
            if (open) {
                pendingHref.clear();
                for (const auto &[name, value] : t.attrs) {
                    if (name == QLatin1String("href"))
                        pendingHref = safeHref(value);
                }
                inAnchor = true;
                anchorText.clear();
            } else if (inAnchor) {
                inAnchor = false;
                QString target = pendingHref;
                if (target.startsWith(QLatin1String("mailto:")))
                    target = target.mid(7);
                if (!target.isEmpty() && anchorText.trimmed() != target
                    && anchorText.trimmed() != pendingHref)
                    out += QLatin1String(" <") + pendingHref + u'>';
            }
        } else if (tag == QLatin1String("td") || tag == QLatin1String("th")) {
            if (!open)
                out += u'\t';
        } else if (blockElements().contains(tag)) {
            newline();
        }
    }
    // Trim trailing whitespace on each line and collapse runs of blank lines.
    QStringList lines = out.split(u'\n');
    QStringList cleaned;
    int blank = 0;
    for (QString l : lines) {
        while (l.endsWith(u' ') || l.endsWith(u'\t'))
            l.chop(1);
        if (l.isEmpty()) {
            if (++blank > 1)
                continue;
        } else {
            blank = 0;
        }
        cleaned.append(l);
    }
    return cleaned.join(u'\n').trimmed();
}

QString textToHtml(const QString &text)
{
    QString out;
    const QStringList lines = text.split(u'\n');
    int quoteDepth = 0;
    for (QString line : lines) {
        if (line.endsWith(u'\r'))
            line.chop(1);
        int depth = 0;
        while (line.startsWith(u'>')) {
            ++depth;
            line.remove(0, 1);
            if (line.startsWith(u' '))
                line.remove(0, 1);
        }
        while (quoteDepth < depth) {
            out += QLatin1String("<blockquote>");
            ++quoteDepth;
        }
        while (quoteDepth > depth) {
            out += QLatin1String("</blockquote>");
            --quoteDepth;
        }
        out += escapeHtml(line) + QLatin1String("<br>");
    }
    while (quoteDepth-- > 0)
        out += QLatin1String("</blockquote>");
    return out;
}

} // namespace e3::mime
