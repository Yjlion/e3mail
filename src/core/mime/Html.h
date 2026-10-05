// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <QString>

namespace e3::mime {

struct SanitizedHtml
{
    QString html;
    int remoteBlocked = 0;  // remote images, stylesheets, backgrounds removed
    int linksKept = 0;
};

// The first of two barriers between a message and the network (the second is
// that the app's QML engine has no network access at all). Re-emits a fixed
// set of tags and attributes and drops everything else, keeping text. Script,
// style and similar elements are dropped with their content. Links survive only
// for http, https and mailto, and the UI asks before following one.
SanitizedHtml sanitizeHtml(const QString &html);

// Readable plain text from HTML: blocks become lines, list items get bullets,
// links keep their target when it differs from the text.
QString htmlToText(const QString &html);

QString escapeHtml(const QString &text);
// Plain text as HTML paragraphs, for showing text/plain mail in the same view.
QString textToHtml(const QString &text);

// Returns the href unchanged if it is http(s) or mailto, otherwise empty.
QString safeHref(const QString &href);

} // namespace e3::mime
