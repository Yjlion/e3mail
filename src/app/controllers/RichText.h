// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <QString>

class QTextDocument;

// What the composer sends is re-emitted from the document model, never taken
// from what the editor would serialise: a fixed set of tags (b i u s h1-h3
// ul ol li blockquote pre code p br a) and nothing else, so neither the editor
// nor a paste decides what goes on the wire. Links survive only for http,
// https and mailto.
namespace RichText {

// Block roles the composer sets as a user property on QTextBlockFormat.
inline constexpr int RoleProperty = 0x100001; // QTextFormat::UserProperty + 1
enum BlockRole { Paragraph = 0, Quote = 1, Code = 2 };

struct Output
{
    QString html;   // empty when the document carries no formatting at all
    QString text;   // the plain-text alternative, always present
};

Output emit(const QTextDocument *doc);

} // namespace RichText
